find_program(NEVERD_DRIVER_LLD_LINK NAMES lld-link
  HINTS "${LLVM_TOOLS_BINARY_DIR}")
set(NEVERD_KMDF_FIXTURE "" CACHE FILEPATH
  "Optional original KMDF lifecycle fixture linked with genuine WDK libraries")
set(NEVERD_KMDF_CFG_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK KMDF lifecycle fixture linked with active CFG")
set(NEVERD_KMDF_CONTROL_FIXTURE "" CACHE FILEPATH
  "Optional original KMDF control-device fixture linked with genuine WDK libraries")
set(NEVERD_KMDF_CONTROL_CFG_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK KMDF control-device fixture linked with active CFG")
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
foreach(_driver_name loader driver_io driver_direct driver_runtime driver_mdl driver_registry driver_async driver_dispatcher driver_context_limits)
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
  set(_driver_additional_dependencies)
  if(_driver_name STREQUAL "driver_async")
    list(APPEND _driver_additional_dependencies
      fixtures/driver_io.c fixtures/DriverAsyncCases.def
      "${CMAKE_SOURCE_DIR}/lib/emulation/windows/KernelValues.def"
      "${CMAKE_SOURCE_DIR}/lib/emulation/windows/WindowsKernelLayout.def")
  endif()
  if(_driver_name STREQUAL "driver_dispatcher")
    list(APPEND _driver_additional_dependencies
      fixtures/driver_io.c fixtures/DriverDispatcherCases.def
      "${CMAKE_SOURCE_DIR}/lib/emulation/windows/KernelDispatcherValues.def"
      "${CMAKE_SOURCE_DIR}/lib/emulation/windows/KernelValues.def")
  endif()
  if(_driver_name STREQUAL "driver_context_limits")
    list(APPEND _driver_additional_dependencies
      fixtures/driver_io.c fixtures/DriverContextLimitCases.def
      "${CMAKE_SOURCE_DIR}/lib/emulation/windows/KernelValues.def"
      "${CMAKE_SOURCE_DIR}/lib/emulation/windows/KernelPoolFlags.def"
      "${CMAKE_SOURCE_DIR}/lib/emulation/windows/WindowsKernelLayout.def"
      "${CMAKE_SOURCE_DIR}/lib/emulation/windows/KernelDispatcherValues.def")
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
      ${_driver_additional_dependencies}
    VERBATIM)
  list(APPEND _driver_outputs "${_driver_image}")
endforeach()

# These original, self-contained fixtures exercise compiler-generated guard
# calls. Keep the inactive link variant so instrumentation alone does not imply
# that CFG enforcement is enabled.
foreach(_driver_name driver_guard driver_guard_inactive
    driver_guard_invalid_check driver_guard_invalid_dispatch)
  set(_driver_object "${_driver_fixture_dir}/${_driver_name}.obj")
  set(_driver_image "${_driver_fixture_dir}/${_driver_name}.sys")
  set(_driver_guard_compile_flags)
  if(_driver_name STREQUAL "driver_guard_invalid_check")
    list(APPEND _driver_guard_compile_flags -DNEVERD_GUARD_INVALID_TARGET)
  elseif(_driver_name STREQUAL "driver_guard_invalid_dispatch")
    list(APPEND _driver_guard_compile_flags -DNEVERD_GUARD_INVALID_DISPATCH)
  endif()
  set(_driver_guard_link_flags /guard:cf)
  if(_driver_name STREQUAL "driver_guard_inactive")
    set(_driver_guard_link_flags)
  endif()
  add_custom_command(OUTPUT "${_driver_image}" "${_driver_object}"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}"
      --target=x86_64-pc-windows-msvc -std=c11 -ffreestanding
      -fno-stack-protector -O1 -Xclang -cfguard
      ${_driver_guard_compile_flags} -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/driver_guard.c"
      -o "${_driver_object}"
    COMMAND "${NEVERD_DRIVER_LLD_LINK}" /machine:x64 /entry:DriverEntry
      /subsystem:native /driver /nodefaultlib /dynamicbase /base:0x180000000
      ${_driver_guard_link_flags} "/out:${_driver_image}" "${_driver_object}"
    DEPENDS fixtures/driver_guard.c
    VERBATIM)
  list(APPEND _driver_outputs "${_driver_image}")
endforeach()
add_custom_target(NeverDDriverFixtures DEPENDS ${_driver_outputs})

# Microsoft kit files are optional external validation inputs. Never assume a
# local ignored SDK directory exists, and keep missing coverage visible.
set(_driver_kmdf_definitions)
set(_driver_has_kmdf_fixture FALSE)
set(_driver_has_kmdf_control_fixture FALSE)
foreach(_driver_kmdf_variable NEVERD_KMDF_FIXTURE NEVERD_KMDF_CFG_FIXTURE
    NEVERD_KMDF_CONTROL_FIXTURE NEVERD_KMDF_CONTROL_CFG_FIXTURE)
  if(_driver_kmdf_variable STREQUAL "NEVERD_KMDF_CFG_FIXTURE"
      AND NOT _driver_has_kmdf_fixture)
    message(STATUS "Driver KMDF CFG tests skipped: NEVERD_KMDF_FIXTURE is unavailable")
    continue()
  endif()
  if(_driver_kmdf_variable STREQUAL "NEVERD_KMDF_CONTROL_CFG_FIXTURE"
      AND NOT _driver_has_kmdf_control_fixture)
    message(STATUS "Driver KMDF control CFG tests skipped: NEVERD_KMDF_CONTROL_FIXTURE is unavailable")
    continue()
  endif()
  if(NOT "${${_driver_kmdf_variable}}" STREQUAL ""
      AND EXISTS "${${_driver_kmdf_variable}}"
      AND NOT IS_DIRECTORY "${${_driver_kmdf_variable}}")
    get_filename_component(_driver_kmdf_path
      "${${_driver_kmdf_variable}}" ABSOLUTE)
    list(APPEND _driver_kmdf_definitions
      "${_driver_kmdf_variable}=\"${_driver_kmdf_path}\"")
    if(_driver_kmdf_variable STREQUAL "NEVERD_KMDF_FIXTURE")
      set(_driver_has_kmdf_fixture TRUE)
    elseif(_driver_kmdf_variable STREQUAL "NEVERD_KMDF_CONTROL_FIXTURE")
      set(_driver_has_kmdf_control_fixture TRUE)
    endif()
    message(STATUS "Driver KMDF fixture ${_driver_kmdf_variable}: ${_driver_kmdf_path}")
  else()
    message(STATUS "Driver KMDF tests requiring ${_driver_kmdf_variable} skipped: optional fixture is missing")
  endif()
endforeach()

add_neverd_unittest(NeverDDriverEmulationTests
  DriverSessionTests.cpp
  DriverLoaderAdvancedTests.cpp
  DriverGuardTests.cpp
  DriverKMDFLifecycleTests.cpp
  DriverKMDFControlTests.cpp
  DriverIOTests.cpp
  ExecutionPolicyTests.cpp
  DriverScenarioTests.cpp
  KernelModelTests.cpp
  KernelRequestOwnershipTests.cpp
  KernelCancellationTests.cpp
  KernelFrameworkTests.cpp
  KernelFrameworkControlTests.cpp
  KernelFrameworkQueueTests.cpp
  KernelFrameworkRequestTests.cpp
  KernelFrameworkRequestAccessorTests.cpp
  KernelRequestMDLTests.cpp
  BackendFaultTests.cpp
  BackendContextTests.cpp
  KernelSchedulerTests.cpp
  DeviceLifecycleTests.cpp
  DriverAsyncTests.cpp
  DriverDispatcherTests.cpp
  KernelDispatcherTests.cpp
  KernelAPIIRQLTests.cpp
  DriverContextLimitTests.cpp
  KernelRuntimeTests.cpp
  DriverDirectIOTests.cpp
  DriverMDLTests.cpp
  DriverRegistryTests.cpp
  KernelRegistryTests.cpp
  KernelExportTests.cpp
  LINK_COMPONENTS Emulation
)
target_include_directories(NeverDDriverEmulationTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/lib/emulation
  ${LLVM_INCLUDE_DIRS})
target_compile_definitions(NeverDDriverEmulationTests PRIVATE
  NEVERD_DRIVER_FIXTURES="${_driver_fixture_dir}"
  NEVERD_DRIVER_IO_FIXTURE="${_driver_fixture_dir}/driver_io.sys"
  ${_driver_kmdf_definitions})
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
    NEVERD_DRIVER_CLI="$<TARGET_FILE:neverd>"
    ${_driver_kmdf_definitions})
  if(WIN32)
    target_link_libraries(NeverDDriverEmulationPublicTests PRIVATE LLVMSupport)
  endif()
  add_dependencies(NeverDDriverEmulationPublicTests NeverDDriverFixtures neverd)
endif()
