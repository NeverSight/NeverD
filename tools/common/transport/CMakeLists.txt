include(FetchContent)
if(NOT TARGET nlohmann_json::nlohmann_json)
  set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(neverd_worker_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(neverd_worker_json)
endif()

add_library(neverd_tool_json STATIC Json.cpp)
target_compile_features(neverd_tool_json PUBLIC cxx_std_20)
target_include_directories(neverd_tool_json PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_link_libraries(neverd_tool_json PUBLIC nlohmann_json::nlohmann_json)
# The GUI's standalone mock SDK embeds this client in a shared fixture.
set_target_properties(neverd_tool_json PROPERTIES POSITION_INDEPENDENT_CODE ON)

function(neverd_stage_json_notice Target)
  add_custom_command(TARGET ${Target} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${Target}>/licenses/nlohmann-json"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../../LICENSES/nlohmann-json/LICENSE.MIT"
      "$<TARGET_FILE_DIR:${Target}>/licenses/nlohmann-json/LICENSE.MIT")
endfunction()

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../../LICENSES/nlohmann-json/LICENSE.MIT"
  DESTINATION share/neverd/licenses/nlohmann-json)
