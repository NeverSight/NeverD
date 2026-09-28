add_neverd_unittest(NeverDInterpreterSpecializationTests
  InterpreterSpecializationTests.cpp
  NativeStackSpecializationTests.cpp
  ControlStateRecoveryTests.cpp
  MaskedControlRelationTests.cpp
  CanonicalControlFieldsTests.cpp
  ControlDiscoveryTests.cpp
  FiniteValuesTests.cpp
  FiniteQueryCacheTests.cpp
  LINK_COMPONENTS InterpreterSpecialization
)

add_neverd_unittest(NeverDDevirtualizationSourceTests
  DevirtualizationSourceTests.cpp
  RecoverySourcePublicationTests.cpp
  PrivateSourceFrameTests.cpp
  NativeRegisterRecoveryTests.cpp
  LINK_COMPONENTS Pipeline BackendC Support
)
target_compile_definitions(NeverDDevirtualizationSourceTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
add_dependencies(NeverDDevirtualizationSourceTests neverd)

add_neverd_unittest(NeverDInterpreterMachineStateTests
  InterpreterMachineStateTests.cpp
  LINK_COMPONENTS Pipeline BackendC Support
)
target_compile_definitions(NeverDInterpreterMachineStateTests PRIVATE
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
)
add_dependencies(NeverDInterpreterMachineStateTests neverd)
