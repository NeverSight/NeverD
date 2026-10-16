# Shared CPU dependency for bounded driver execution and semantic tests.
include_guard(GLOBAL)

function(neverd_require_unicorn)
  if(TARGET unicorn)
    return()
  endif()

  # The pinned upstream MSVC build selects an x86 JIT from pointer width.
  # Refuse that incorrect host configuration rather than emit x86 code on ARM64.
  if(WIN32 AND MSVC AND
     (CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$" OR
      CMAKE_GENERATOR_PLATFORM MATCHES "^[Aa][Rr][Mm]64$" OR
      CMAKE_C_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64"))
    message(FATAL_ERROR
      "The pinned Unicorn MSVC build does not support an ARM64 host. "
      "Use an ARM64 LLVM-MinGW toolchain for Unicorn, or configure "
      "NEVERD_EMULATION_BACKEND_UNICORN=OFF and "
      "NEVERD_ENABLE_SEMANTIC_TESTS=OFF for native WHP tests.")
  endif()

  # Keep dependency policy local: libneverd still builds as a shared library.
  set(BUILD_SHARED_LIBS OFF)
  set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  set(UNICORN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(UNICORN_INSTALL OFF CACHE BOOL "" FORCE)
  set(UNICORN_ARCH "x86;arm;aarch64" CACHE STRING "" FORCE)
  add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/unicorn"
                   "${CMAKE_BINARY_DIR}/third_party/unicorn"
                   EXCLUDE_FROM_ALL)
  # Keep Unicorn's private CRC helper separate from static zlib on COFF.
  # Match function spellings without changing QEMU's generated helper tokens.
  set(_neverd_crc_header "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/UnicornCRCNamespace.h")
  foreach(_neverd_crc_target unicorn-common arm-softmmu aarch64-softmmu)
    if(MSVC)
      target_compile_options(${_neverd_crc_target} PRIVATE
        "$<$<COMPILE_LANGUAGE:C>:/FI${_neverd_crc_header}>")
    else()
      target_compile_options(${_neverd_crc_target} PRIVATE
        "$<$<COMPILE_LANGUAGE:C>:-include>"
        "$<$<COMPILE_LANGUAGE:C>:${_neverd_crc_header}>")
    endif()
  endforeach()
  if(WIN32)
    # Unicorn's public platform header includes windows.h.
    target_compile_definitions(unicorn INTERFACE NOMINMAX)
  endif()
endfunction()
