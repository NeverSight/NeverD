add_neverd_unittest(NeverDConcolicTests
  LowIRConcolicTests.cpp

  LINK_COMPONENTS Concolic
)

target_include_directories(NeverDConcolicTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/lib/concolic
)

set(_concolic_fixture_root
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/binaries)
set(_concolic_fixture_files
    lowir_concolic_elf_x64
    lowir_concolic_elf_arm64
    lowir_concolic_macho_x64
    lowir_concolic_macho_arm64
    lowir_concolic_pe_x64
    lowir_concolic_pe_arm64)
foreach(_fixture IN LISTS _concolic_fixture_files)
  if(NOT EXISTS "${_concolic_fixture_root}/${_fixture}")
    message(FATAL_ERROR "Required concolic fixture is missing: ${_fixture}")
  endif()
endforeach()

add_neverd_unittest(NeverDConcolicIntegrationTests
  LowIRConcolicIntegrationTests.cpp

  LINK_COMPONENTS
  Concolic
  Support
  IRLow
  Decode
)

target_include_directories(NeverDConcolicIntegrationTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)
target_compile_definitions(NeverDConcolicIntegrationTests PRIVATE
  CONCOLIC_FIXTURE_ROOT="${_concolic_fixture_root}"
)

if(TARGET neverd_shared)
  add_neverd_unittest(NeverDConcolicCAPITests
    ConcolicCAPIHeaderC.c
    ConcolicCAPITests.cpp

    LINK_LIBS
    neverd_shared
    LLVMSupport
  )

  target_include_directories(NeverDConcolicCAPITests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${LLVM_INCLUDE_DIRS}
  )
  target_compile_definitions(NeverDConcolicCAPITests PRIVATE
    NEVERD_CONCOLIC_FIXTURE_DIR="${_concolic_fixture_root}"
  )

  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(ConcolicCAPIHeaderC.c PROPERTIES
      COMPILE_OPTIONS "-fshort-enums"
    )
  endif()
  if(MSVC)
    set_source_files_properties(ConcolicCAPIHeaderC.c PROPERTIES
      COMPILE_OPTIONS /std:c11)
  endif()

  add_dependencies(NeverDConcolicCAPITests neverd_shared)

  add_test(
    NAME NeverDConcolicPythonIntegration
    COMMAND "${CMAKE_COMMAND}" -E env
            "PYTHONPATH=${CMAKE_SOURCE_DIR}/pluginsdk/python"
            "NEVERD_TEST_LIBNEVERD=$<TARGET_FILE:neverd_shared>"
            "${Python3_EXECUTABLE}" -m unittest discover
            -s "${CMAKE_SOURCE_DIR}/pluginsdk/python/tests"
            -p "test_concolic_integration.py" -v
  )
  set_tests_properties(NeverDConcolicPythonIntegration PROPERTIES
    LABELS "NeverDConcolicPythonIntegration"
    TIMEOUT 60
  )

  if(TARGET neverd)
    add_neverd_unittest(NeverDConcolicCLITests
      ConcolicCLIContractTests.cpp

      LINK_LIBS LLVMSupport
      TIMEOUT 300
    )

    target_include_directories(NeverDConcolicCLITests PRIVATE
      ${CMAKE_SOURCE_DIR}/include
      ${LLVM_INCLUDE_DIRS}
    )
    target_compile_definitions(NeverDConcolicCLITests PRIVATE
      CONCOLIC_FIXTURE_ROOT="${_concolic_fixture_root}"
      NEVERD_BINARY="$<TARGET_FILE:neverd>"
    )

    add_dependencies(NeverDConcolicCLITests neverd)
  endif()
endif()
