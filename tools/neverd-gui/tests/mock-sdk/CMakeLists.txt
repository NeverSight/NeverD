cmake_minimum_required(VERSION 3.24)
project(NeverDGuiFixtureSDK LANGUAGES CXX)
# Test-only shared C ABI fixture. Never install or publish this library.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
include(FetchContent)
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(json)
find_package(Threads REQUIRED)
set(worker "${CMAKE_CURRENT_SOURCE_DIR}/../../../neverd-worker")
add_library(neverd_fixture SHARED "${worker}/tests/mock_engine.cpp" "${worker}/Protocol.cpp")
target_include_directories(neverd_fixture PRIVATE "${worker}" "${CMAKE_CURRENT_SOURCE_DIR}/../../../../include")
target_link_libraries(neverd_fixture PRIVATE nlohmann_json::nlohmann_json Threads::Threads)
