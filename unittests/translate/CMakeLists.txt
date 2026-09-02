add_neverd_unittest(NeverDTranslationContractTests
  ${CMAKE_SOURCE_DIR}/tools/neverd/NeverDTranslateOutput.cpp
  GuestMemoryRuntimeTests.cpp
  GuestStateTests.cpp
  NativeTranslationSessionTests.cpp
  ResolvedHostTargetTests.cpp
  RuntimeCodeMemoryTests.cpp
  RuntimeGuestStateTests.cpp
  RuntimeHelpersTests.cpp
  RuntimeSymbolRegistryTests.cpp
  TranslationIRVerifierTests.cpp
  TranslationArtifactVerifierTests.cpp
  TranslationJITLinkerTests.cpp
  TranslationCLIContractTests.cpp
  TranslationLinkGraphVerifierTests.cpp
  TranslationObjectCompilerTests.cpp
  TranslationObjectRequestTests.cpp
  X86TranslationBlockBuilderTests.cpp
  X86TranslationBlockLowererTests.cpp

  LINK_COMPONENTS
  Support
  Translate

  LINK_LIBS
  LLVMObjectYAML
)

target_include_directories(NeverDTranslationContractTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/lib/translate
  ${CMAKE_SOURCE_DIR}/tools/neverd
  ${LLVM_INCLUDE_DIRS}
)

target_compile_definitions(NeverDTranslationContractTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
)

if(TARGET neverd)
  add_dependencies(NeverDTranslationContractTests neverd)
endif()

if(TARGET neverd_shared)
  # Pins the standalone C layout and the owned libneverd translation boundary
  # without folding the shared-library ABI into the internal C++ contract
  # suite.
  add_neverd_unittest(NeverDTranslationCAPITests
    TranslationCAPIHeaderC.c
    TranslationCAPITests.cpp

    LINK_LIBS
    neverd_shared
    LLVMObject
    LLVMTargetParser
  )

  target_include_directories(NeverDTranslationCAPITests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${LLVM_INCLUDE_DIRS}
  )

  # The public translation ABI must retain its frozen layout even when a
  # consumer compiles C enum tags with the smallest possible representation.
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(TranslationCAPIHeaderC.c PROPERTIES
      COMPILE_OPTIONS "-fshort-enums"
    )
  endif()
  if(MSVC)
    # File-scope _Static_assert is C11.  MSVC compiles .c as C89 unless told.
    set_source_files_properties(TranslationCAPIHeaderC.c PROPERTIES
      COMPILE_OPTIONS /std:c11)
  endif()

  # Compile the emitted SDK exactly as a native C consumer sees it.
  # Deliberately do not link neverd_shared: its PUBLIC source-tree include
  # directory must not mask an incomplete staged include hierarchy.  The
  # target-level dependency guarantees that neverd_shared's POST_BUILD SDK
  # copy has finished before Ninja or a multi-configuration generator starts
  # either C compilation.
  add_executable(NeverDStagedCSDKHeaderTests
    StagedCSDKCanonicalHeader.c
    StagedCSDKFlatHeader.c
  )

  set_target_properties(NeverDStagedCSDKHeaderTests PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
    FOLDER "NeverD/Tests"
  )

  target_include_directories(NeverDStagedCSDKHeaderTests PRIVATE
    "$<TARGET_FILE_DIR:neverd_shared>/sdk"
  )

  add_dependencies(NeverDStagedCSDKHeaderTests neverd_shared)
  add_dependencies(NeverDUnitTests NeverDStagedCSDKHeaderTests)

  add_test(
    NAME NeverDStagedCSDKHeaderTests
    COMMAND NeverDStagedCSDKHeaderTests
  )
  set_tests_properties(NeverDStagedCSDKHeaderTests PROPERTIES
    LABELS NeverDStagedCSDKHeaderTests
    TIMEOUT 30
  )

  add_test(
    NAME NeverDTranslationPythonIntegration
    COMMAND "${CMAKE_COMMAND}" -E env
            "PYTHONPATH=${CMAKE_SOURCE_DIR}/pluginsdk/python"
            "NEVERD_TEST_LIBNEVERD=$<TARGET_FILE:neverd_shared>"
            "${Python3_EXECUTABLE}" -m unittest discover
            -s "${CMAKE_SOURCE_DIR}/pluginsdk/python/tests"
            -p "test_translation_integration.py" -v
  )
  set_tests_properties(NeverDTranslationPythonIntegration PROPERTIES
    LABELS "NeverDTranslationPythonIntegration"
  )
endif()
