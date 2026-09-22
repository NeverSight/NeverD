# Shared CPU dependency for bounded driver execution and semantic tests.
include_guard(GLOBAL)

function(neverd_require_unicorn)
  if(TARGET unicorn)
    return()
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
  if(WIN32)
    # Unicorn's public platform header includes windows.h.
    target_compile_definitions(unicorn INTERFACE NOMINMAX)
  endif()
endfunction()
