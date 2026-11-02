# Keep compiler compatibility workarounds private to the fetched test framework.
include_guard(GLOBAL)

function(neverd_configure_googletest)
  if(NOT MSVC OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    return()
  endif()

  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-Wno-error=character-conversion"
    NEVERD_GTEST_HAS_CHARACTER_CONVERSION_DIAGNOSTIC)
  if(NOT NEVERD_GTEST_HAS_CHARACTER_CONVERSION_DIAGNOSTIC)
    return()
  endif()

  # GoogleTest 1.17's char8_t printer implicitly widens to char32_t. New Clang
  # diagnoses that conversion, and GoogleTest enables /WX on MSVC-compatible
  # drivers. Keep the diagnostic visible without weakening other warnings or
  # exporting this exception to NeverD's tests and production targets.
  foreach(_target gtest gtest_main gmock gmock_main)
    if(TARGET ${_target})
      get_target_property(_imported ${_target} IMPORTED)
      if(NOT _imported)
        target_compile_options(${_target} PRIVATE
          "$<$<COMPILE_LANGUAGE:CXX>:-Wno-error=character-conversion>")
      endif()
    endif()
  endforeach()
endfunction()
