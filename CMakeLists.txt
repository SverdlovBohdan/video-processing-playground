cmake_minimum_required(VERSION 3.28)

project(
    YUV-Converter
    VERSION 0.1.0
    DESCRIPTION "video processing playground"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(project_warnings INTERFACE)

target_compile_options(project_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
)

option(ENABLE_SANITIZERS "Enable sanitizers" ON)

add_library(project_options INTERFACE)
target_compile_options(project_options INTERFACE -fexperimental-library)

find_package(Boost REQUIRED)
find_package(SDL3 REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(AVDEVICE REQUIRED IMPORTED_TARGET libavdevice)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)

include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking # Or use a specific version like 'v1.90'
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)

target_link_libraries(imgui PRIVATE
    SDL3::SDL3
    "-framework Metal"
    "-framework MetalKit"
    "-framework Foundation"
    "-framework QuartzCore"
)

target_include_directories(imgui PUBLIC SYSTEM
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

add_executable(${PROJECT_NAME}
    main.cpp
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        project_warnings
        project_options
        Boost::headers
        SDL3::SDL3
        PkgConfig::AVDEVICE
        PkgConfig::AVFORMAT
        PkgConfig::AVCODEC
        PkgConfig::AVUTIL
        imgui
)

message(STATUS "Path avutil: ${AVUTIL_ACTUAL_PATH}")

target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE
    ${AVCODEC_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${AVDEVICE_INCLUDE_DIRS}
)

target_compile_features(${PROJECT_NAME}
    PRIVATE
        cxx_std_23
)

include(CheckIPOSupported)

check_ipo_supported(RESULT ipo_supported)

if(ipo_supported AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(
        TARGET ${PROJECT_NAME}
        PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE
    )
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# include(CTest)

# if(BUILD_TESTING)
#     enable_testing()
# endif()