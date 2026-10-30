# x64 explicit continuation source, public export and independent CPU fixtures.
add_neverd_unittest(NeverDX64ContinuationSourceTests
  X64ContinuationSourceTests.cpp
  X64ContinuationIntegerTests.cpp
  LINK_COMPONENTS BackendC BackendLLVM Pipeline BinaryInterpreterSpecialization
)
target_compile_definitions(NeverDX64ContinuationSourceTests PRIVATE
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# The public continuation route owns source/manifest export and driver stops.
add_neverd_unittest(NeverDX64ContinuationExportTests
  X64ContinuationExportTests.cpp
  LINK_LIBS neverd_shared)
target_compile_definitions(NeverDX64ContinuationExportTests PRIVATE
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(NeverDX64ContinuationExportTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include ${LLVM_INCLUDE_DIRS})
if(NOT APPLE)
  target_link_libraries(NeverDX64ContinuationExportTests PRIVATE LLVMSupport)
endif()
add_dependencies(NeverDX64ContinuationExportTests neverd)
