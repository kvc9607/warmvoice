cmake_minimum_required(VERSION 3.16)
project(warmvoice CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(MSVC)
  add_compile_options(/W4 /fp:fast /utf-8)
  add_compile_definitions(_USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS NOMINMAX)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
else()
  # x86-64 baseline (SSE2) so the same binary runs on practically every PC.
  add_compile_options(-Wall -Wextra -ffast-math -fno-finite-math-only)
endif()

add_library(wvcore STATIC
  core/src/bands.cpp core/src/spectral.cpp core/src/dsp.cpp
  core/src/resolve.cpp core/src/pipeline.cpp core/src/ai_denoiser.cpp)
target_include_directories(wvcore PUBLIC core/include)
if(NOT WIN32)
  target_link_libraries(wvcore PUBLIC ${CMAKE_DL_LIBS})
endif()

add_executable(wvproc tools/wvproc.cpp)
target_link_libraries(wvproc wvcore)

# Test stand-in for the AI library (same interface and delay as DeepFilterNet + our patch).
add_library(warmvoice_ai_mock SHARED tests/mock/mock_ai.cpp tests/mock/stft.cpp tests/mock/fft.cpp)
target_include_directories(warmvoice_ai_mock PRIVATE core/include tests/mock)
set_target_properties(warmvoice_ai_mock PROPERTIES CXX_VISIBILITY_PRESET hidden)

add_executable(wvtests tests/test_main.cpp tests/test_pipeline.cpp tests/mock/stft.cpp tests/mock/fft.cpp)
target_include_directories(wvtests PRIVATE tests/mock)
target_link_libraries(wvtests wvcore)
target_compile_definitions(wvtests PRIVATE WV_MOCK_AI="$<TARGET_FILE:warmvoice_ai_mock>")
add_dependencies(wvtests warmvoice_ai_mock)

if(WIN32)
  add_subdirectory(apo)
endif()
