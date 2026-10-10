add_neverd_unittest(NeverDWebAvailabilityTests
  AvailabilityTests.cpp
  LINK_LIBS neverd_shared LLVMSupport
)
target_include_directories(NeverDWebAvailabilityTests PRIVATE
  "${CMAKE_SOURCE_DIR}/include" ${LLVM_INCLUDE_DIRS})
target_compile_definitions(NeverDWebAvailabilityTests PRIVATE
  NEVERD_TEST_WEB=$<BOOL:${NEVERD_ENABLE_WEB_ANALYSIS}>
  NEVERD_TEST_WEB_JS=$<AND:$<BOOL:${NEVERD_ENABLE_WEB_ANALYSIS}>,$<BOOL:${NEVERD_ENABLE_WEB_JAVASCRIPT}>>)
