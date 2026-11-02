# One source-built static provider for LLVM, the core and standalone frontends.
include_guard(GLOBAL)
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/zlib")

if(TARGET ZLIB::ZLIB)
  message(FATAL_ERROR
    "Initialize NeverDZlib before LLVM, Qt or any system ZLIB discovery")
endif()

include(FetchContent)
function(neverd_fetch_zlib)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()
  set(ZLIB_BUILD_TESTING OFF)
  set(ZLIB_BUILD_SHARED OFF)
  set(ZLIB_BUILD_STATIC ON)
  set(ZLIB_INSTALL OFF)
  FetchContent_Declare(neverd_mobile_zlib
    URL
      https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz
      https://zlib.net/fossils/zlib-1.3.2.tar.gz
    URL_HASH SHA256=bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16
    TIMEOUT 180)
  FetchContent_MakeAvailable(neverd_mobile_zlib)
  set(NEVERD_ZLIB_SOURCE_DIR "${neverd_mobile_zlib_SOURCE_DIR}" PARENT_SCOPE)
  set(NEVERD_ZLIB_BINARY_DIR "${neverd_mobile_zlib_BINARY_DIR}" PARENT_SCOPE)
endfunction()
neverd_fetch_zlib()
set_target_properties(zlibstatic PROPERTIES
  OUTPUT_NAME neverd_zlib DEBUG_POSTFIX ""
  POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden
  ARCHIVE_OUTPUT_DIRECTORY "${NEVERD_ZLIB_BINARY_DIR}/lib")

# LLVM reads an imported library's LOCATION when producing llvm-config. An
# imported facade preserves that contract while the real target still builds
# with this project's compiler, architecture, CRT and configuration.
add_library(ZLIB::ZLIB STATIC IMPORTED GLOBAL)
set(_archive "${CMAKE_STATIC_LIBRARY_PREFIX}neverd_zlib${CMAKE_STATIC_LIBRARY_SUFFIX}")
set_target_properties(ZLIB::ZLIB PROPERTIES
  IMPORTED_LOCATION "${NEVERD_ZLIB_BINARY_DIR}/lib/${_archive}"
  INTERFACE_INCLUDE_DIRECTORIES "${NEVERD_ZLIB_BINARY_DIR};${NEVERD_ZLIB_SOURCE_DIR}"
  NEVERD_ZLIB_VERSION "1.3.2")
target_compile_definitions(ZLIB::ZLIB INTERFACE
  "$<TARGET_PROPERTY:zlibstatic,INTERFACE_COMPILE_DEFINITIONS>")
set(_configs Debug Release RelWithDebInfo MinSizeRel ${CMAKE_BUILD_TYPE} ${CMAKE_CONFIGURATION_TYPES})
list(REMOVE_DUPLICATES _configs)
foreach(_config IN LISTS _configs)
  string(TOUPPER "${_config}" _upper)
  set(_directory "${NEVERD_ZLIB_BINARY_DIR}/lib/${_config}")
  set_property(TARGET zlibstatic PROPERTY "ARCHIVE_OUTPUT_DIRECTORY_${_upper}" "${_directory}")
  set_property(TARGET ZLIB::ZLIB APPEND PROPERTY IMPORTED_CONFIGURATIONS "${_upper}")
  set_property(TARGET ZLIB::ZLIB PROPERTY "IMPORTED_LOCATION_${_upper}" "${_directory}/${_archive}")
endforeach()
add_dependencies(ZLIB::ZLIB zlibstatic)

install(FILES "${NEVERD_ZLIB_SOURCE_DIR}/LICENSE"
  DESTINATION share/neverd/licenses/zlib)
message(STATUS "NeverD zlib: bundled static 1.3.2")
