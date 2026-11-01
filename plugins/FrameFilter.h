#pragma once

extern "C" {
    struct AVFrame;
}

class FrameFilter {
public:
  virtual ~FrameFilter() = default;

  virtual AVFrame* Process(AVFrame* input) = 0;
  virtual const char *GetName() const = 0;

  // virtual void Configure() = 0;
};