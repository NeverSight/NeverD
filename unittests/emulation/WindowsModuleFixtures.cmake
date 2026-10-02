if(NOT TARGET NeverDWindowsProcessFixtures)
  return()
endif()
set(_windows_module_dir "${CMAKE_CURRENT_BINARY_DIR}/windows-module-fixtures")
file(MAKE_DIRECTORY "${_windows_module_dir}")
foreach(_module leaf middle)
  string(TOUPPER "${_module}" _kind)
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsModuleCases.def"
    _exports REGEX "^NEVERD_MODULE_${_kind}_EXPORT")
  set(_definition "LIBRARY ${_module}.dll\nEXPORTS\n")
  foreach(_export IN LISTS _exports)
    string(REGEX REPLACE "^NEVERD_MODULE_${_kind}_EXPORT\\(([A-Za-z0-9_]+), \"([^\"]*)\"\\)" "  \\1 \\2\n" _line "${_export}")
    string(APPEND _definition "${_line}")
  endforeach()
  file(CONFIGURE OUTPUT "${_windows_module_dir}/${_module}.def" CONTENT "${_definition}" @ONLY)
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsModuleCases.def")
set(_module_outputs)
foreach(_arch X64 AArch64)
  if(_arch STREQUAL "X64")
    set(_target x86_64-pc-windows-msvc)
    set(_machine x64)
  else()
    set(_target aarch64-pc-windows-msvc)
    set(_machine arm64)
  endif()
  set(_dir "${_windows_module_dir}/${_arch}")
  file(MAKE_DIRECTORY "${_dir}")
  set(_compile "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_target}"
    -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
    -fno-vectorize -fno-slp-vectorize -O1 -c)
  set(_kernel "${_windows_fixture_dir}/${_arch}-kernel32.lib")
  add_custom_command(OUTPUT "${_dir}/leaf.dll" "${_dir}/leaf.lib" "${_dir}/leaf.obj"
    COMMAND ${_compile} "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/windows_module_leaf.c"
      -o "${_dir}/leaf.obj"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /dll /noentry /nodefaultlib /subsystem:console
      "/machine:${_machine}" /base:0x180000000 /timestamp:0
      "/def:${_windows_module_dir}/leaf.def"
      "${_dir}/leaf.obj" "${_kernel}" "/out:${_dir}/leaf.dll" "/implib:${_dir}/leaf.lib"
    DEPENDS fixtures/windows_module_leaf.c fixtures/WindowsModuleFixture.h
      fixtures/WindowsModuleCases.def "${_windows_module_dir}/leaf.def" "${_kernel}"
    VERBATIM)
  add_custom_command(OUTPUT "${_dir}/middle.dll" "${_dir}/middle.lib" "${_dir}/middle.obj"
    COMMAND ${_compile} "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/windows_module_middle.c"
      -o "${_dir}/middle.obj"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /dll /noentry /nodefaultlib /subsystem:console
      "/machine:${_machine}" /base:0x180000000 /timestamp:0
      "/def:${_windows_module_dir}/middle.def"
      "${_dir}/middle.obj" "${_dir}/leaf.lib" "${_kernel}"
      "/out:${_dir}/middle.dll" "/implib:${_dir}/middle.lib"
    DEPENDS fixtures/windows_module_middle.c fixtures/WindowsModuleFixture.h
      fixtures/WindowsModuleCases.def "${_windows_module_dir}/middle.def"
      "${_dir}/leaf.lib" "${_kernel}"
    VERBATIM)
  add_custom_command(OUTPUT "${_dir}/module process.exe" "${_dir}/process.obj"
    COMMAND ${_compile} "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/windows_module_process.c"
      -o "${_dir}/process.obj"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /nodefaultlib /entry:entry /subsystem:console
      "/machine:${_machine}" /base:0x140000000 /include:_tls_used /timestamp:0
      "${_dir}/process.obj" "${_dir}/middle.lib" "${_dir}/leaf.lib" "${_kernel}"
      "/out:${_dir}/module process.exe"
    DEPENDS fixtures/windows_module_process.c fixtures/WindowsModuleFixture.h
      fixtures/WindowsModuleCases.def "${_dir}/middle.lib" "${_dir}/leaf.lib" "${_kernel}"
    VERBATIM)
  list(APPEND _module_outputs "${_dir}/module process.exe")
endforeach()
add_custom_target(NeverDWindowsModuleFixtures DEPENDS ${_module_outputs})
foreach(_owner NeverDWindowsProcessTests NeverDProcessPublicTests)
  if(TARGET ${_owner})
    add_dependencies(${_owner} NeverDWindowsModuleFixtures)
    target_compile_definitions(${_owner} PRIVATE NEVERD_WINDOWS_MODULE_FIXTURE_DIR="${_windows_module_dir}")
  endif()
endforeach()
