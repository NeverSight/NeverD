include("${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/NeverDJson.cmake")

add_library(neverd_tool_json STATIC Json.cpp)
include("${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/NeverDPerformance.cmake")
neverd_add_simdjson()
if(NEVERD_ENABLE_SIMDJSON)
  target_link_libraries(neverd_tool_json PRIVATE simdjson)
  target_compile_definitions(neverd_tool_json PRIVATE NEVERD_USE_SIMDJSON=1)
endif()
target_compile_features(neverd_tool_json PUBLIC cxx_std_20)
target_include_directories(neverd_tool_json PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_link_libraries(neverd_tool_json PUBLIC nlohmann_json::nlohmann_json)
# The GUI's standalone mock SDK embeds this client in a shared fixture.
set_target_properties(neverd_tool_json PROPERTIES POSITION_INDEPENDENT_CODE ON)

function(neverd_stage_json_notice Target)
  set(_licenses "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../../LICENSES")
  neverd_stage_notices(${Target} nlohmann-json
    "${_licenses}/nlohmann-json/LICENSE.MIT")
  if(NEVERD_ENABLE_SIMDJSON)
    # The notices the performance libraries stage in the same directory: the
    # fork build's, which name the exact source the build compiled.
    neverd_add_simdjson()
    get_property(_simdjson GLOBAL PROPERTY NEVERD_FORK_NOTICES_neverd_simdjson)
    if(NOT _simdjson)
      set(_simdjson "${_licenses}/simdjson")
    endif()
    neverd_stage_notices(${Target} simdjson "${_simdjson}")
  endif()
endfunction()

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../../LICENSES/nlohmann-json/LICENSE.MIT"
  DESTINATION share/neverd/licenses/nlohmann-json)
