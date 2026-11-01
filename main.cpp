#include <condition_variable>
#include <cstddef>
#include <deque>
#include <expected>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <thread>
#include <unordered_map>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
}

#include "plugins/FrameFilter.h"

struct Resolution {
  size_t width;
  size_t height;
};

#define CONCAT_IMPL(x, y) x##y
#define CONCAT(x, y) CONCAT_IMPL(x, y)

template <typename F> class DeferAction {
  F func;

public:
  DeferAction(F &&f) : func(std::forward<F>(f)) {}
  ~DeferAction() { func(); }
};

#define defer DeferAction CONCAT(defer_obj_, __LINE__) = [&]()

template <typename T> class BoundedChannel {
public:
  explicit BoundedChannel(size_t capacity) : capacity_(capacity) {}

  bool Send(T item) {
    std::unique_lock lock{channel_guard_};
    not_full_.wait(lock, [&] { return queue_.size() < capacity_ || closed_; });

    if (closed_) {
      return false;
    }

    queue_.push_back(std::move(item));
    lock.unlock();

    not_empty_.notify_one();
    return true;
  }

  std::optional<T> Receive() {
    std::unique_lock lock{channel_guard_};
    not_empty_.wait(lock, [&] { return !queue_.empty() || closed_; });

    if (!queue_.empty()) {
      auto item = std::move(queue_.front());
      queue_.pop_front();
      lock.unlock();

      not_full_.notify_one();
      return item;
    }

    return std::nullopt;
  }

  void Close() {
    {
      std::unique_lock lock{channel_guard_};
      closed_ = true;
    }

    not_empty_.notify_all();
    not_full_.notify_all();
  }

private:
  std::mutex channel_guard_;
  size_t capacity_;
  bool closed_{false};
  std::deque<T> queue_;

  std::condition_variable not_full_;
  std::condition_variable not_empty_;
};

class FrameExtractor {
public:
  explicit FrameExtractor(BoundedChannel<AVPacket *> &decoder_input)
      : decoder_input_{decoder_input} {}

  std::function<bool()>
  Start(std::function<void(AVFormatContext *)> notify_ready_to_decode) {
    AVFormatContext *fmt_ctx = nullptr;
    const AVInputFormat *input_fmt = av_find_input_format("avfoundation");

    if (!input_fmt) {
      std::cerr << "AVFoundation input format not found.\n";
      return nullptr;
    }

    // "0" = first video device (e.g., FaceTime HD Camera), use "" for
    // audio-only or "1" for screen
    const char *device_name = "0";
    AVDictionary *options = nullptr;
    av_dict_set(&options, "framerate", "30", 0);
    av_dict_set(&options, "video_size", "1280x720", 0);
    av_dict_set(&options, "pixel_format", "nv12", 0);

    if (avformat_open_input(&fmt_ctx, device_name, input_fmt, &options) < 0) {
      std::cerr << "Could not open input device.\n";
      av_dict_free(&options);
      notify_ready_to_decode(nullptr);
      return nullptr;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
      std::cerr << "Could not find stream information.\n";
      avformat_close_input(&fmt_ctx);
      av_dict_free(&options);
      notify_ready_to_decode(nullptr);
      return nullptr;
    }

    notify_ready_to_decode(fmt_ctx);
    std::cout << "Successfully opened AVFoundation stream! Reading frames...\n";
    av_dict_free(&options);

    AVPacket *read_pkt = av_packet_alloc();
    return [this, fmt_ctx, read_pkt]() mutable {
      auto ret = av_read_frame(fmt_ctx, read_pkt);
      if (ret >= 0) {
        AVPacket *queue_pkt = av_packet_alloc();
        av_packet_move_ref(queue_pkt, read_pkt);

        if (!decoder_input_.Send(queue_pkt)) {
          std::cout << "decoder_input_ is closed" << std::endl;

          av_packet_free(&queue_pkt);
          return false;
        }

        // std::cout << "Send frame to decoder" << std::endl;
        return true;
      }

      if (ret == AVERROR(EAGAIN)) {
        // No frame ready yet — not an error, just retry shortly.
        return true;
      }

      av_packet_free(&read_pkt);
      std::cout << "av_read_frame() is failed" << std::endl;
      return false;
    };
  }

  void Stop() { decoder_input_.Close(); }

private:
  BoundedChannel<AVPacket *> &decoder_input_;
};

class FrameDecoder {
public:
  explicit FrameDecoder(BoundedChannel<AVPacket *> &decoder_input,
                        BoundedChannel<AVFrame *> &display_frames)
      : decoder_input_{decoder_input}, display_frames_{display_frames} {}

  std::function<bool()> Start(AVFormatContext *fmt_ctx) {
    int video_stream_idx = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
      if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        video_stream_idx = static_cast<int>(i);
        break;
      }
    }

    if (video_stream_idx == -1) {
      std::cerr << "No video stream.\n";
      Stop();
      return nullptr;
    }

    AVCodecParameters *codec_params =
        fmt_ctx->streams[video_stream_idx]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codec_params->codec_id);
    if (!codec) {
      std::cerr << "No codec is found.\n";
      Stop();
      return nullptr;
    }

    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, codec_params);

    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
      std::cerr << "Can't open codec.\n";
      Stop();
      avcodec_free_context(&codec_ctx);
      return nullptr;
    }

    AVFrame *decoded_frame = av_frame_alloc();

    return [this, codec_ctx, video_stream_idx, decoded_frame]() mutable {
      std::optional<AVPacket *> pkt = decoder_input_.Receive();
      if (!pkt.has_value()) {
        av_frame_free(&decoded_frame);
        avcodec_free_context(&codec_ctx);
        return false;
      }

      defer { av_packet_free(&pkt.value()); };

      if (video_stream_idx != pkt.value()->stream_index) {
        // Not a video packet. Skip it.
        return true;
      }

      int response = avcodec_send_packet(codec_ctx, pkt.value());
      if (response < 0) {
        std::cerr << "Packet sending error.\n";
        av_frame_free(&decoded_frame);
        avcodec_free_context(&codec_ctx);
        Stop();
        return false;
      }

      while (response >= 0) {
        response = avcodec_receive_frame(codec_ctx, decoded_frame);
        if (response == AVERROR(EAGAIN)) {
          // std::cout << "AVERROR(EAGAIN) " << std::endl;
          break;
        } else if (response == AVERROR_EOF) {
          av_frame_free(&decoded_frame);
          avcodec_free_context(&codec_ctx);
          // std::cout << "AVERROR(AVERROR_EOF) " << std::endl;
          Stop();
          return false;
        } else if (response < 0) {
          av_frame_free(&decoded_frame);
          avcodec_free_context(&codec_ctx);
          Stop();
          return false;
        }

        // std::cout << "Decoded frame "
        //           << " Resolution: " << decoded_frame->width << "x"
        //           << decoded_frame->height
        //           << " Pxl format: " << decoded_frame->format << "\n";
        AVFrame *display_frame = av_frame_alloc();
        av_frame_move_ref(display_frame, decoded_frame);
        if (!display_frames_.Send(display_frame)) {
          av_frame_free(&decoded_frame);
          av_frame_free(&display_frame);
          avcodec_free_context(&codec_ctx);
          return false;
        }
      }

      av_frame_unref(decoded_frame);
      return true;
    };
  }

  void Stop() { decoder_input_.Close(); }

private:
  BoundedChannel<AVPacket *> &decoder_input_;
  BoundedChannel<AVFrame *> &display_frames_;
};

class FilterRegistry {
public:
  void RegiserStatic(const std::string &name,
                     std::function<std::unique_ptr<FrameFilter>()> factory) {
    static_factories_[name] = std::move(factory);
  }

  // void RegisterDynamic. Loads from .so

  std::unique_ptr<FrameFilter> Create(const std::string &name) {
    if (static_factories_.contains(name)) {
      return static_factories_[name]();
    }

    // todo: fallback - find the dynamic handle for name.

    return nullptr;
  }

private:
  std::unordered_map<std::string, std::function<std::unique_ptr<FrameFilter>()>>
      static_factories_;
  // todo: dynamic_handles
};

class FilterChain {
public:
  explicit FilterChain(BoundedChannel<AVFrame *> &decoded_frames,
                       BoundedChannel<AVFrame *> &display_frames)
      : decoded_frames_{decoded_frames}, display_frames_{display_frames} {}

  void AddFilter(std::unique_ptr<FrameFilter> filter) {
    filters_.push_back(std::move(filter));
  };

  bool Process() {
    auto decoded_frame = decoded_frames_.Receive();
    if (!decoded_frame) {
      return false;
    }

    AVFrame *current = decoded_frame.value();
    for (auto &filter : filters_) {
      AVFrame *next = filter->Process(current);
      av_frame_free(&current);
      current = next;
    }

    display_frames_.Send(current);

    return true;
  }

private:
  BoundedChannel<AVFrame *> &decoded_frames_;
  BoundedChannel<AVFrame *> &display_frames_;
  std::vector<std::unique_ptr<FrameFilter>> filters_;
};

class GrayScaleCpuFilter : public FrameFilter {
public:
  AVFrame *Process(AVFrame *input) override {
    AVFrame *result = av_frame_clone(input);
    std::memset(result->data[1], 128,
                static_cast<size_t>(result->linesize[1] * result->height / 2));
    return result;
  }

  const char *GetName() const override { return "GrayScaleCpu"; }
};

class BlurCpuFilter : public FrameFilter {
public:
  explicit BlurCpuFilter(std::vector<float> blur_kernel)
      : blur_kernel_{std::move(blur_kernel)} {}

  AVFrame *Process(AVFrame *input) override {
    const int kernel_radius = static_cast<int>(blur_kernel_.size() / 2);

    AVFrame *temp = av_frame_alloc();
    temp->format = input->format;
    temp->width = input->width;
    temp->height = input->height;
    av_frame_get_buffer(temp, 0);

    AVFrame *result = av_frame_alloc();
    result->format = input->format;
    result->width = input->width;
    result->height = input->height;
    av_frame_get_buffer(result, 0);

    const uint8_t *src = input->data[0];
    uint8_t *tmp = temp->data[0];
    const int src_stride = input->linesize[0];
    const int tmp_stride = temp->linesize[0];

    // Horizontal pass: input -> temp
    for (int y = 0; y < input->height; ++y) {
      const uint8_t *src_row = src + y * src_stride;
      uint8_t *tmp_row = tmp + y * tmp_stride;
      for (int x = 0; x < input->width; ++x) {
        float sum = 0.0f;
        for (int k = -kernel_radius; k <= kernel_radius; ++k) {
          int sx = std::clamp(x + k, 0, input->width - 1);
          sum += src_row[sx] * blur_kernel_.data()[k + kernel_radius];
        }
        tmp_row[x] = static_cast<uint8_t>(sum);
      }
    }

    // Vertical pass: temp -> result
    uint8_t *dst = result->data[0];
    const int dst_stride = result->linesize[0];
    for (int y = 0; y < input->height; ++y) {
      uint8_t *dst_row = dst + y * dst_stride;
      for (int x = 0; x < input->width; ++x) {
        float sum = 0.0f;
        for (int k = -kernel_radius; k <= kernel_radius; ++k) {
          int sy = std::clamp(y + k, 0, input->height - 1);
          sum +=
              tmp[sy * tmp_stride + x] * blur_kernel_.data()[k + kernel_radius];
        }
        dst_row[x] = static_cast<uint8_t>(sum);
      }
    }

    av_frame_free(&temp);

    int uv_height = input->height / 2;
    for (int y = 0; y < uv_height; ++y) {
      memcpy(result->data[1] + y * result->linesize[1],
             input->data[1] + y * input->linesize[1],
             static_cast<size_t>(input->width)); // NV12: width bytes = uv_width*2 (U+V interleaved)
    }

    return result;
  }

  const char *GetName() const override { return "BlurCpuFilter"; }

private:
  std::vector<float> blur_kernel_;
};

int main(int, char **) {
  avdevice_register_all();

  BoundedChannel<AVPacket *> decoder_input{6};
  BoundedChannel<AVFrame *> decoded_frames{2};
  BoundedChannel<AVFrame *> display_frames{2};
  FrameExtractor frame_extractor{decoder_input};
  FrameDecoder frame_decoder{decoder_input, decoded_frames};

  FilterRegistry filter_registry{};
  filter_registry.RegiserStatic(
      "GrayScale", []() { return std::make_unique<GrayScaleCpuFilter>(); });
  filter_registry.RegiserStatic("Blur", []() {
    return std::make_unique<BlurCpuFilter>(
        std::vector<float>{0.054f, 0.244f, 0.403f, 0.244f, 0.054f});
  });

  FilterChain filter_chain{decoded_frames, display_frames};
  // filter_chain.AddFilter(filter_registry.Create("GrayScale"));
  filter_chain.AddFilter(filter_registry.Create("Blur"));

  static AVFormatContext *format_context = nullptr;
  std::unique_ptr<std::jthread> decoder_thread{};
  std::unique_ptr<std::jthread> processing_thread{};

  std::jthread frame_extractor_thread{
      [&frame_extractor, &frame_decoder, &filter_chain, &decoder_thread,
       &processing_thread](std::stop_token stoken) {
        auto push_frame_to_decoder = frame_extractor.Start(
            [&frame_decoder, &filter_chain, &decoder_thread,
             &processing_thread](AVFormatContext *fmt_ctx) {
              format_context = fmt_ctx;
              if (!format_context) {
                return;
              }

              auto decode_frame = frame_decoder.Start(format_context);
              if (!decode_frame) {
                return;
              }

              decoder_thread = std::make_unique<std::jthread>(
                  [decode_frame](std::stop_token stoken) {
                    while (!stoken.stop_requested()) {
                      if (!decode_frame()) {
                        break;
                      }
                    }

                    std::cout << "Frame Decoder is done" << std::endl;
                  });

              processing_thread = std::make_unique<std::jthread>(
                  [&filter_chain](std::stop_token stoken) {
                    while (!stoken.stop_requested()) {
                      if (!filter_chain.Process()) {
                        break;
                      }
                    }

                    std::cout << "Frame Processing is done" << std::endl;
                  });
            });

        if (!format_context) {
          return;
        }

        while (!stoken.stop_requested()) {
          if (!push_frame_to_decoder()) {
            break;
          }
        }

        std::cout << "Frame Extractor is done" << std::endl;
      }};

  // const Resolution resolution{.width = 1280, .height = 720};
  const Resolution display_resolution = {.width = 1280, .height = 720};

  SDL_Init(SDL_INIT_VIDEO);

  const float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
  SDL_Window *window =
      SDL_CreateWindow("SDL3 Metal Backend Example", display_resolution.width,
                       display_resolution.height,
                       SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE);
  if (!window) {
    std::cerr << "Window Creation Error: " << SDL_GetError() << std::endl;
    SDL_Quit();
    return -1;
  }

  SDL_Renderer *renderer = SDL_CreateRenderer(window, "metal");
  SDL_SetRenderVSync(renderer, 1);
  if (!renderer) {
    std::cerr << "Failed to create Metal Renderer: " << SDL_GetError()
              << std::endl;
    SDL_DestroyWindow(window);
    SDL_Quit();
    return -1;
  }

  SDL_Texture *texture = nullptr;

  // Setup Dear ImGui context
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  (void)io;
  io.ConfigFlags |=
      ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
  io.ConfigFlags |=
      ImGuiConfigFlags_NavEnableGamepad; // Enable Gamepad Controls

  // Setup Dear ImGui style
  ImGui::StyleColorsDark();
  // ImGui::StyleColorsLight();

  // Setup scaling
  ImGuiStyle &style = ImGui::GetStyle();
  style.ScaleAllSizes(
      main_scale); // Bake a fixed style scale. (until we have a solution for
                   // dynamic style scaling, changing this requires resetting
                   // Style + calling this again)
  style.FontScaleDpi =
      main_scale; // Set initial font scale. (in docking branch: using
                  // io.ConfigDpiScaleFonts=true automatically overrides this
                  // for every window depending on the current monitor)

  ImFontConfig config;
  config.SizePixels = 32.0f;
  ImFont *my_large_font = io.Fonts->AddFontDefault(&config);

  // Setup Platform/Renderer backends
  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  // Shutdown
  defer {
    display_frames.Close();
    decoded_frames.Close();
    decoder_input.Close();

    if (processing_thread) {
      processing_thread->request_stop();
    }

    if (decoder_thread) {
      decoder_thread->request_stop();
    }
    frame_extractor_thread.request_stop();

    if (processing_thread) {
      processing_thread->join();
    }

    if (decoder_thread) {
      decoder_thread->join();
    }
    frame_extractor_thread.join();

    if (format_context) {
      avformat_close_input(&format_context);
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
  };

  std::chrono::milliseconds last_frame_time_processing{0};

  bool running = true;
  while (running) {
    auto now = std::chrono::steady_clock::now();

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);

      if (event.type == SDL_EVENT_QUIT) {
        running = 0;
      }

      if (event.type == SDL_EVENT_KEY_DOWN) {
        switch (event.key.key) {
        case SDLK_UP:
          break;
        case SDLK_DOWN:
          break;
        case SDLK_LEFT:
          break;
        case SDLK_RIGHT:
          decoder_input.Close();
          break;
        }
      }
    }

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x,
                       io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    std::optional<AVFrame *> display_frame = display_frames.Receive();
    if (display_frame.has_value()) {
      if (!texture) {
        texture = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_NV12, SDL_TEXTUREACCESS_STREAMING,
            display_frame.value()->width, display_frame.value()->height);
      }

      SDL_UpdateNVTexture(texture, nullptr, display_frame.value()->data[0],
                          display_frame.value()->linesize[0],
                          display_frame.value()->data[1],
                          display_frame.value()->linesize[1]);
      av_frame_free(&display_frame.value());
    }

    SDL_RenderTexture(renderer, texture, nullptr, nullptr);

    // ImGui::Begin("My Texture");
    // ImGui::Image(reinterpret_cast<ImTextureID>(texture), ImVec2(400, 300));
    // ImGui::End();

    ImGui::PushFont(my_large_font);

    ImDrawList *draw_list = ImGui::GetForegroundDrawList();
    ImVec2 text_position = ImVec2(1000.0f, 50.f); // Screen coordinates
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
    ImU32 text_color = IM_COL32(255, 255, 0, 255);
#pragma GCC diagnostic pop

    const std::string fps =
        last_frame_time_processing == std::chrono::milliseconds{0}
            ? std::string{"unknown"}
            : std::to_string(
                  1 / std::chrono::duration<float>(last_frame_time_processing)
                          .count());
    draw_list->AddText(text_position, text_color, ("FPS: " + fps).c_str());

    ImGui::PopFont();

    ImGui::Render();
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);

    SDL_RenderPresent(renderer);

    last_frame_time_processing =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - now);
  }

  return 0;
}