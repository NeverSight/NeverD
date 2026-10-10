# Parser-only integration of Meta Hermes, revision 602befee340da188ea1560cde7a9cc33e6b180dc.
# Upstream CMake source manifests and configuration templates retain their
# original notices in the fetched source and in the installed license set.
set(_hermes "${NEVERD_HERMES_SOURCE_DIR}")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(HERMES_ENABLE_EH_RTTI ON)
set(HERMES_ENABLE_BITCODE OFF)
set(HERMES_ENABLE_UNICODE_REGEXP_PROPERTY_ESCAPES ON)
set(HERMES_CHECK_NATIVE_STACK OFF)

include_directories(SYSTEM
  "${_hermes}/include"
  "${_hermes}/external"
  "${_hermes}/external/llvh/include"
  "${CMAKE_CURRENT_BINARY_DIR}/llvh/include")
include_directories(BEFORE
  "${CMAKE_SOURCE_DIR}/lib/web/hermes-overlay"
  "${CMAKE_SOURCE_DIR}/include")
add_compile_definitions(HERMES_LIMIT_STACK_DEPTH HERMES_PARSE_FLOW=0
                        HERMES_PARSE_TS=0 HERMES_PARSE_JSX=0)

function(add_hermes_library name)
  cmake_parse_arguments(ARG "" "" "LINK_LIBS" ${ARGN})
  add_library(${name} STATIC ${ARG_UNPARSED_ARGUMENTS})
  target_link_libraries(${name} ${ARG_LINK_LIBS} ${HERMES_LINK_COMPONENTS})
  set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()

add_subdirectory(llvh)
set(HERMES_LINK_COMPONENTS LLVHSupport)
add_subdirectory("${_hermes}/external/dtoa" dtoa)

# CharacterProperties supplies the parser's Unicode identifier and regexp
# tables. No locale/date/case-conversion runtime or no-op Unicode fallback.
add_hermes_library(hermesPlatformUnicode
  "${_hermes}/lib/Platform/Unicode/CharacterProperties.cpp")
add_subdirectory("${_hermes}/lib/Support" support)
add_subdirectory("${_hermes}/lib/Regex" regex)
add_subdirectory("${_hermes}/lib/AST" ast)
add_subdirectory("${_hermes}/lib/Parser" parser)

add_library(neverd_js_parser INTERFACE)
target_link_libraries(neverd_js_parser INTERFACE hermesParser hermesAST)
target_include_directories(neverd_js_parser SYSTEM INTERFACE
  "${_hermes}/include" "${_hermes}/external/llvh/include"
  "${CMAKE_CURRENT_BINARY_DIR}/llvh/include")
target_include_directories(neverd_js_parser BEFORE INTERFACE
  "${CMAKE_SOURCE_DIR}/lib/web/hermes-overlay")
target_compile_definitions(neverd_js_parser INTERFACE
  HERMES_LIMIT_STACK_DEPTH HERMES_PARSE_FLOW=0 HERMES_PARSE_TS=0 HERMES_PARSE_JSX=0)
