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
  x86/X86TranslationBlockBuilderTests.cpp
  x86/X86TranslationBlockLowererTests.cpp
  x86/X87PortableRuntimeTests.cpp

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
  # utility dependency waits for neverd_shared's linked output and POST_BUILD
  # SDK copy.  A direct library dependency allows Ninja to start either C
  # compilation before linking, while the staged headers are still absent.
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

  add_custom_target(NeverDStagedCSDKHeadersReady
    DEPENDS "$<TARGET_FILE:neverd_shared>"
  )
  add_dependencies(NeverDStagedCSDKHeaderTests NeverDStagedCSDKHeadersReady)
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

add_neverd_unittest(NeverDX87PortableExecutionTests
  x86/X87PortableExecutionTests.cpp
  x86/X87PortableRuntimeCHeader.c

  LINK_COMPONENTS
  Translate

  LINK_LIBS
  neverd_x87_runtime
)
set_target_properties(NeverDX87PortableExecutionTests PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED YES
  C_EXTENSIONS NO)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(x86/X87PortableRuntimeCHeader.c PROPERTIES
    COMPILE_OPTIONS "-fshort-enums")
endif()

add_neverd_unittest(NeverDX87PortablePreparationTests
  x86/X87PortablePreparationTests.cpp

  LINK_COMPONENTS
  Pipeline
  BackendLLVM
  BackendC
  IRMed
)

add_neverd_unittest(NeverDX87PortableEmissionTests
  x86/X87PortableEmissionTests.cpp

  LINK_COMPONENTS
  Pipeline
  BackendLLVM
  BackendC
  IRMed
)

add_neverd_unittest(NeverDX87PortableGeneratedTests
  x86/X87PortableGeneratedTests.cpp

  LINK_COMPONENTS
  Pipeline
  BackendLLVM
  BackendC
  IRMed
  Translate

  LINK_LIBS
  neverd_x87_runtime
)
target_compile_definitions(NeverDX87PortableGeneratedTests PRIVATE
  NEVERD_X87_RUNTIME_PATH="$<TARGET_FILE:neverd_x87_runtime>")
if(NEVERD_TEST_CLANG_EXECUTABLE)
  target_compile_definitions(NeverDX87PortableGeneratedTests PRIVATE
    NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}")
endif()

add_neverd_unittest(NeverDX87TranslationRuntimeTests
  x86/RuntimeX87GuestStateTests.cpp
  x86/RuntimeX87HelpersTests.cpp

  LINK_COMPONENTS
  Translate
)

add_neverd_unittest(NeverDX87TranslationTests
  x86/X87TranslationTests.cpp
  x86/X87TranslationFrozenTests.cpp
  x86/X87TranslationResetTests.cpp

  LINK_COMPONENTS
  Translate
)

add_executable(NeverDX87PortableBenchmark x86/X87PortableBenchmark.cpp)
target_link_libraries(NeverDX87PortableBenchmark PRIVATE NeverDTranslate neverd_x87_runtime)
set_target_properties(NeverDX87PortableBenchmark PROPERTIES FOLDER "NeverD/Tests")
