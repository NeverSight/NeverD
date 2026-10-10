# Embedded C++ URL parser only. Do not enter upstream's generators, tools,
# bindings, benchmarks, package installation or URLPattern dependency graph.
include(FetchContent)
function(neverd_add_web_url_parser)
  if(TARGET neverd_web_url)
    return()
  endif()
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()
  FetchContent_Declare(neverd_ada
    URL https://codeload.github.com/ada-url/ada/tar.gz/b12a893a45809da8103bb4f1e2f6f5ee13f9100b
    URL_HASH SHA256=1eb38d21c35e162ccc6c71e8fdd9a5b418eb359f2b39f1c50bf3836a5949b623
    SOURCE_SUBDIR neverd-no-automatic-subdirectory
    TIMEOUT 120)
  FetchContent_MakeAvailable(neverd_ada)
  add_library(neverd_web_url STATIC "${neverd_ada_SOURCE_DIR}/src/ada.cpp")
  set_target_properties(neverd_web_url PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
  target_compile_features(neverd_web_url PUBLIC cxx_std_20)
  target_include_directories(neverd_web_url SYSTEM PUBLIC
    "${neverd_ada_SOURCE_DIR}/include")
  target_include_directories(neverd_web_url PRIVATE "${neverd_ada_SOURCE_DIR}/src")
  target_compile_definitions(neverd_web_url PUBLIC ADA_INCLUDE_URL_PATTERN=0)
endfunction()
