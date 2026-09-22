find_program(NEVERD_DRIVER_LLD_LINK NAMES lld-link
  HINTS "${LLVM_TOOLS_BINARY_DIR}")
if(NOT NEVERD_TEST_CLANG_EXECUTABLE OR NOT NEVERD_DRIVER_LLD_LINK)
  message(STATUS "Driver emulation fixtures unavailable: Clang and lld-link are required")
  return()
endif()

set(_driver_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
file(MAKE_DIRECTORY "${_driver_fixture_dir}")
set(_driver_import_lib "${_driver_fixture_dir}/ntoskrnl.lib")
add_custom_command(OUTPUT "${_driver_import_lib}"
  COMMAND "${NEVERD_DRIVER_LLD_LINK}" /lib /machine:x64
    "/def:${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ntoskrnl.def"
    "/out:${_driver_import_lib}"
  DEPENDS fixtures/ntoskrnl.def
  VERBATIM)

set(_driver_outputs)
set(_driver_case 0)
foreach(_driver_name success failure unknown fault loop devices readonly privileged opaque gs apifault pendingentry opaqueargument)
  set(_driver_object "${_driver_fixture_dir}/${_driver_name}.obj")
  set(_driver_image "${_driver_fixture_dir}/${_driver_name}.sys")
  add_custom_command(OUTPUT "${_driver_image}" "${_driver_object}"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}"
      --target=x86_64-pc-windows-msvc -std=c11 -ffreestanding
      -fno-stack-protector -O1 "-DDRIVER_CASE=${_driver_case}" -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/driver.c" -o "${_driver_object}"
    COMMAND "${NEVERD_DRIVER_LLD_LINK}" /machine:x64 /entry:DriverEntry
      /subsystem:native /driver /nodefaultlib /fixed /base:0x180000000
      "/out:${_driver_image}" "${_driver_object}" "${_driver_import_lib}"
    DEPENDS fixtures/driver.c "${_driver_import_lib}"
    VERBATIM)
  list(APPEND _driver_outputs "${_driver_image}")
  math(EXPR _driver_case "${_driver_case} + 1")
endforeach()
# The loader fixture deliberately retains relocations and a compiler-inserted
# stack protector. The WDM fixture covers the complete synchronous lifecycle.
foreach(_driver_name loader driver_io driver_direct driver_runtime)
  set(_driver_object "${_driver_fixture_dir}/${_driver_name}.obj")
  set(_driver_image "${_driver_fixture_dir}/${_driver_name}.sys")
  if(_driver_name STREQUAL "loader")
    set(_driver_source fixtures/driver_loader.c)
    set(_driver_stack_flag -fstack-protector-all)
    set(_driver_entry GsDriverEntry)
  else()
    set(_driver_source "fixtures/${_driver_name}.c")
    set(_driver_stack_flag -fno-stack-protector)
    set(_driver_entry DriverEntry)
  endif()
  add_custom_command(OUTPUT "${_driver_image}" "${_driver_object}"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}"
      --target=x86_64-pc-windows-msvc -std=c11 -ffreestanding
      ${_driver_stack_flag} -O1 -c
      "${CMAKE_CURRENT_SOURCE_DIR}/${_driver_source}" -o "${_driver_object}"
    COMMAND "${NEVERD_DRIVER_LLD_LINK}" /machine:x64 "/entry:${_driver_entry}"
      /subsystem:native /driver /nodefaultlib /dynamicbase /base:0x180000000
      "/out:${_driver_image}" "${_driver_object}" "${_driver_import_lib}"
    DEPENDS "${_driver_source}" "${_driver_import_lib}"
    VERBATIM)
  list(APPEND _driver_outputs "${_driver_image}")
endforeach()
add_custom_target(NeverDDriverFixtures DEPENDS ${_driver_outputs})

add_neverd_unittest(NeverDDriverEmulationTests
  DriverSessionTests.cpp
  DriverLoaderAdvancedTests.cpp
  DriverIOTests.cpp
  ExecutionPolicyTests.cpp
  DriverScenarioTests.cpp
  KernelModelTests.cpp
  BackendFaultTests.cpp
  KernelRuntimeTests.cpp
  DriverDirectIOTests.cpp
  KernelExportTests.cpp
  LINK_COMPONENTS Emulation
)
target_include_directories(NeverDDriverEmulationTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/lib/emulation
  ${LLVM_INCLUDE_DIRS})
target_compile_definitions(NeverDDriverEmulationTests PRIVATE
  NEVERD_DRIVER_FIXTURES="${_driver_fixture_dir}"
    NEVERD_DRIVER_IO_FIXTURE="${_driver_fixture_dir}/driver_io.sys")
add_dependencies(NeverDDriverEmulationTests NeverDDriverFixtures)

if(TARGET neverd_shared AND TARGET neverd)
  # Keep public tests on the shared engine, without a second static copy of
  # the emulation component or LLVM's process-global registries.
  add_neverd_unittest(NeverDDriverEmulationPublicTests
    DriverEmulationPublicTests.cpp
    DriverScenarioPublicTests.cpp
    LINK_LIBS neverd_shared)
  target_include_directories(NeverDDriverEmulationPublicTests PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${LLVM_INCLUDE_DIRS})
  target_compile_definitions(NeverDDriverEmulationPublicTests PRIVATE
    NEVERD_DRIVER_FIXTURES="${_driver_fixture_dir}"
    NEVERD_DRIVER_IO_FIXTURE="${_driver_fixture_dir}/driver_io.sys"
    NEVERD_DRIVER_CLI="$<TARGET_FILE:neverd>")
  if(WIN32)
    target_link_libraries(NeverDDriverEmulationPublicTests PRIVATE LLVMSupport)
  endif()
  add_dependencies(NeverDDriverEmulationPublicTests NeverDDriverFixtures neverd)
endif()
