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
  "${CMAKE_CURRENT_BINARY_DIR}/overlay"
  "${CMAKE_SOURCE_DIR}/lib/web/hermes-overlay"
  "${CMAKE_SOURCE_DIR}/include")
add_compile_definitions(HERMES_LIMIT_STACK_DEPTH HERMES_PARSE_FLOW=0
                        HERMES_PARSE_TS=0 HERMES_PARSE_JSX=0)

# The pinned parser converts async-arrow SpreadElement parameters to
# RestElement without their source locations. Preserve the parser's own
# locations at that conversion, before validation or any NeverD consumer.
# Generate a private copy with the upstream notices intact; never modify a
# shared FetchContent checkout or invoke a patching script/tool.
set(_private "${CMAKE_CURRENT_BINARY_DIR}/private")
set(_overlay "${CMAKE_CURRENT_BINARY_DIR}/overlay")
file(READ "${_hermes}/lib/Parser/JSParserImpl.cpp" _parser_patched)
file(READ "${_hermes}/lib/Parser/JSParserImpl.h" _parser_header)
file(READ "${_hermes}/include/hermes/Parser/JSLexer.h" _lexer_header)
file(READ "${_hermes}/lib/AST/SemanticValidator.cpp" _validator_patched)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${_hermes}/lib/Parser/JSParserImpl.cpp"
  "${_hermes}/lib/Parser/JSParserImpl.h"
  "${_hermes}/include/hermes/Parser/JSLexer.h"
  "${_hermes}/lib/AST/SemanticValidator.cpp")
include(Patches.cmake)
function(neverd_hermes_write path content)
  if(EXISTS "${path}")
    file(READ "${path}" _previous)
  endif()
  if(NOT _previous STREQUAL content)
    file(WRITE "${path}" "${content}")
  endif()
endfunction()
neverd_hermes_write("${_private}/JSParserImpl.cpp" "${_parser_patched}")
neverd_hermes_write("${_private}/JSParserImpl.h" "${_parser_header}")
neverd_hermes_write("${_overlay}/hermes/Parser/JSLexer.h" "${_lexer_header}")
configure_file("${_hermes}/include/hermes/Parser/TokenKinds.def"
  "${_overlay}/hermes/Parser/TokenKinds.def" COPYONLY)
neverd_hermes_write("${_private}/SemanticValidator.cpp" "${_validator_patched}")
configure_file("${CMAKE_SOURCE_DIR}/lib/web/HermesResourceDeclarations.inc"
  "${_private}/HermesResourceDeclarations.inc" COPYONLY)

function(add_hermes_library name)
  cmake_parse_arguments(ARG "" "" "LINK_LIBS" ${ARGN})
  if(name STREQUAL "hermesParser")
    # Every parser translation unit sees the same private JSParserImpl header.
    set(_sources)
    foreach(_source IN LISTS ARG_UNPARSED_ARGUMENTS)
      if(NOT _source STREQUAL "JSParserImpl.cpp" AND
         NOT _source STREQUAL "JSParserImpl.h")
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/${_source}"
          "${_private}/${_source}" COPYONLY)
      endif()
      list(APPEND _sources "${_private}/${_source}")
    endforeach()
    set(ARG_UNPARSED_ARGUMENTS ${_sources})
  elseif(name STREQUAL "hermesAST")
    list(TRANSFORM ARG_UNPARSED_ARGUMENTS REPLACE "^SemanticValidator\\.cpp$"
      "${_private}/SemanticValidator.cpp")
  endif()
  add_library(${name} STATIC ${ARG_UNPARSED_ARGUMENTS})
  if(name STREQUAL "hermesParser")
    target_include_directories(${name} PRIVATE "${_hermes}/lib/Parser")
  elseif(name STREQUAL "hermesAST")
    target_include_directories(${name} PRIVATE "${_hermes}/lib/AST")
  endif()
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
  "${_overlay}"
  "${CMAKE_SOURCE_DIR}/lib/web/hermes-overlay")
target_compile_definitions(neverd_js_parser INTERFACE
  HERMES_LIMIT_STACK_DEPTH HERMES_PARSE_FLOW=0 HERMES_PARSE_TS=0 HERMES_PARSE_JSX=0)
