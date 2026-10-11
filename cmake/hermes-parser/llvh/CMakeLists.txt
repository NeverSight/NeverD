# Parser-only LLVH configuration. Based on Hermes external/llvh/CMakeLists.txt
# at 602befee340da188ea1560cde7a9cc33e6b180dc (Meta Platforms, MIT license).
# The LLVM-derived source retains its own LICENSE.TXT. Skip upstream tools
# and tests; NeverD already owns its test framework and LLVM build.
set(_llvh "${NEVERD_HERMES_SOURCE_DIR}/external/llvh")
set(HERMES_LLVM True)
add_compile_definitions(HERMES_LLVM)
set(LLVM_MAIN_SRC_DIR "${_llvh}")
set(LLVM_MAIN_BIN_DIR "${CMAKE_CURRENT_BINARY_DIR}")
set(LLVM_SOURCE_DIR "${LLVM_MAIN_SRC_DIR}")
set(LLVM_BINARY_DIR "${LLVM_MAIN_BIN_DIR}")
set(PACKAGE_NAME LLVH)
set(PACKAGE_STRING "LLVH 8.0.0svn")
set(PACKAGE_VERSION "8.0.0svn")
set(LLVM_VERSION_MAJOR 8)
set(LLVM_VERSION_MINOR 0)
set(LLVM_VERSION_PATCH 0)
set(LLVM_ON_UNIX ${UNIX})
set(LLVM_ENABLE_THREADS 1)
set(LLVM_ENABLE_ZLIB OFF)
set(LLVM_ENABLE_TERMINFO OFF)
list(PREPEND CMAKE_MODULE_PATH "${_llvh}/cmake" "${_llvh}/cmake/modules")
include("${_llvh}/cmake/config-ix.cmake")
configure_file("${_llvh}/include/llvh/Config/config.h.cmake"
               include/llvh/Config/config.h)
configure_file("${_llvh}/include/llvh/Config/llvm-config.h.cmake"
               include/llvh/Config/llvm-config.h)
add_subdirectory("${_llvh}/lib" lib)

# LLVM and LLVH both expose this Windows signal callback with C linkage. Keep
# the parser's definition and signal registration private to its support copy.
if(WIN32)
  set_property(SOURCE "${_llvh}/lib/Support/Signals.cpp"
    TARGET_DIRECTORY LLVHSupport APPEND PROPERTY COMPILE_DEFINITIONS
    HandleAbort=NeverDLLVHHandleAbort)
endif()
