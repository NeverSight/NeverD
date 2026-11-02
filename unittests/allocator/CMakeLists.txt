cmake_minimum_required(VERSION 3.20)

# Also runnable without building LLVM: cmake -S unittests/allocator -B build-allocator.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(NeverDAllocatorTests LANGUAGES C CXX)
  include(CTest)
  include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/NeverDMimalloc.cmake")
endif()

# CI supplies an expectation, leaving the production option at its default.
# A disabled allocator must not silently turn the default-on job into a test
# of the system allocator instead.
if(DEFINED NEVERD_TEST_EXPECT_MIMALLOC)
  if((NEVERD_TEST_EXPECT_MIMALLOC AND NOT NEVERD_MIMALLOC_ENABLED) OR
     (NOT NEVERD_TEST_EXPECT_MIMALLOC AND NEVERD_MIMALLOC_ENABLED))
    message(FATAL_ERROR
      "Expected mimalloc=${NEVERD_TEST_EXPECT_MIMALLOC}, got ${NEVERD_MIMALLOC_ENABLED}")
  endif()
endif()

# Deliberately has no dependency on mimalloc. Verify allocations in a shared
# library use the executable's allocator, including initialization before main.
add_library(neverd-allocator-probe SHARED AllocatorProbe.cpp)
set_target_properties(neverd-allocator-probe PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
add_executable(NeverDAllocatorTests AllocatorTests.cpp)
target_compile_features(NeverDAllocatorTests PRIVATE cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(NeverDAllocatorTests PRIVATE neverd-allocator-probe Threads::Threads)
neverd_link_mimalloc(NeverDAllocatorTests)
if(NEVERD_MIMALLOC_ENABLED)
  target_compile_definitions(NeverDAllocatorTests PRIVATE NEVERD_TEST_MIMALLOC=1)
  if(WIN32)
    target_compile_definitions(NeverDAllocatorTests PRIVATE
      "NEVERD_TEST_MIMALLOC_DLL=\"$<TARGET_FILE_NAME:mimalloc>\"")
  endif()
endif()
if(TARGET neverd_shared)
  target_link_libraries(NeverDAllocatorTests PRIVATE neverd_shared)
  target_compile_definitions(NeverDAllocatorTests PRIVATE NEVERD_TEST_SDK=1)
endif()
if(TARGET NeverDUnitTests)
  add_dependencies(NeverDUnitTests NeverDAllocatorTests)
endif()
set(_allocator_arguments "")
if(WIN32)
  # A second copy loaded only at runtime, with no mimalloc dependency.
  add_library(neverd-allocator-plugin MODULE AllocatorProbe.cpp)
  set_target_properties(neverd-allocator-plugin PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
  add_dependencies(NeverDAllocatorTests neverd-allocator-plugin)
  list(APPEND _allocator_arguments "$<TARGET_FILE:neverd-allocator-plugin>")
endif()
add_test(NAME NeverDAllocatorTests COMMAND NeverDAllocatorTests ${_allocator_arguments})
set_tests_properties(NeverDAllocatorTests PROPERTIES LABELS NeverDAllocatorTests TIMEOUT 60)
if(WIN32 AND NEVERD_MIMALLOC_ENABLED)
  add_test(NAME NeverDAllocatorRedirectDisabled
    COMMAND NeverDAllocatorTests ${_allocator_arguments})
  set_tests_properties(NeverDAllocatorRedirectDisabled PROPERTIES
    LABELS NeverDAllocatorTests TIMEOUT 60
    ENVIRONMENT "MIMALLOC_DISABLE_REDIRECT=1;NEVERD_TEST_EXPECT_SYSTEM_ALLOCATOR=1")
endif()
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
add_test(NAME NeverDMimallocConfigure
  COMMAND "${Python3_EXECUTABLE}" -u
    "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/tests/test_mimalloc_configure.py" -v)
# Keep nested policy fixtures on the selected generator. Otherwise Windows
# repeats platform-default generator discovery for every configure-only case.
set_tests_properties(NeverDMimallocConfigure PROPERTIES
  LABELS NeverDAllocatorTests TIMEOUT 120
  ENVIRONMENT "CMAKE_GENERATOR=${CMAKE_GENERATOR}")

if(WIN32 AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  # The standalone fixture also checks relocation of the installed runtime.
  install(TARGETS NeverDAllocatorTests neverd-allocator-probe neverd-allocator-plugin
    RUNTIME DESTINATION bin LIBRARY DESTINATION bin)
  add_test(NAME NeverDAllocatorInstalled
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/tests/test_mimalloc_install.py"
      --build-dir "${CMAKE_BINARY_DIR}" --config "$<CONFIG>"
      --executable "$<TARGET_FILE_NAME:NeverDAllocatorTests>"
      --plugin "$<TARGET_FILE_NAME:neverd-allocator-plugin>"
      --enabled "${NEVERD_MIMALLOC_ENABLED}")
  set_tests_properties(NeverDAllocatorInstalled PROPERTIES LABELS NeverDAllocatorTests TIMEOUT 120)
endif()
