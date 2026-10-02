find_program(NEVERD_PROCESS_LLD_LINK NAMES lld-link HINTS "${LLVM_TOOLS_BINARY_DIR}")
if(NOT NEVERD_TEST_CLANG_EXECUTABLE OR NOT NEVERD_PROCESS_LLD_LINK)
  message(STATUS "PE process fixtures unavailable: Clang and lld-link are required")
  return()
endif()
set(_windows_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/windows-process-fixtures")
file(MAKE_DIRECTORY "${_windows_fixture_dir}")
# Generate the import library from the exact same named model inventory, while
# fixture prototypes and native Windows execution independently check the ABI.
foreach(_provider Kernel Native)
  if(_provider STREQUAL "Kernel")
    set(_module kernel32)
  else()
    set(_module ntdll)
  endif()
  file(STRINGS "${CMAKE_SOURCE_DIR}/lib/emulation/os/windows/process/WindowsProcessServices.def"
    _windows_api_lines REGEX "^NEVERD_WINDOWS_PROCESS_API\\([A-Za-z0-9_]+, ${_provider},")
  set(_windows_exports "LIBRARY ${_module}.dll\nEXPORTS\n")
  foreach(_line IN LISTS _windows_api_lines)
    string(REGEX REPLACE "^NEVERD_WINDOWS_PROCESS_API\\(([A-Za-z0-9_]+),.*" "\\1" _name "${_line}")
    string(APPEND _windows_exports "  ${_name}\n")
  endforeach()
  file(CONFIGURE OUTPUT "${_windows_fixture_dir}/${_module}.def" CONTENT "${_windows_exports}" @ONLY)
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/lib/emulation/os/windows/process/WindowsProcessServices.def")
set(_windows_outputs)
foreach(_arch X64 AArch64)
  if(_arch STREQUAL "X64")
    set(_target x86_64-pc-windows-msvc)
    set(_machine x64)
  else()
    set(_target aarch64-pc-windows-msvc)
    set(_machine arm64)
  endif()
  set(_base "${_windows_fixture_dir}/${_arch}")
  add_custom_command(OUTPUT "${_base}.exe" "${_base}.obj" "${_base}-kernel32.lib" "${_base}-ntdll.lib"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /lib "/machine:${_machine}"
      "/def:${_windows_fixture_dir}/kernel32.def" "/out:${_base}-kernel32.lib"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /lib "/machine:${_machine}"
      "/def:${_windows_fixture_dir}/ntdll.def" "/out:${_base}-ntdll.lib"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_target}"
      -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
      -fno-vectorize -fno-slp-vectorize -O1 -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/windows_process.c" -o "${_base}.obj"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /nodefaultlib /entry:entry /subsystem:console
      "/machine:${_machine}" /base:0x140000000 /include:_tls_used /timestamp:0
      "${_base}.obj" "${_base}-kernel32.lib" "${_base}-ntdll.lib" "/out:${_base}.exe"
    DEPENDS fixtures/windows_process.c fixtures/WindowsProcessCases.def
      fixtures/WindowsMemoryCases.def fixtures/WindowsMemoryFixture.inc
      "${_windows_fixture_dir}/kernel32.def" "${_windows_fixture_dir}/ntdll.def"
    VERBATIM)
  list(APPEND _windows_outputs "${_base}.exe")
endforeach()
add_custom_target(NeverDWindowsProcessFixtures DEPENDS ${_windows_outputs})
foreach(_owner NeverDWindowsProcessTests NeverDProcessPublicTests)
  if(TARGET ${_owner})
    add_dependencies(${_owner} NeverDWindowsProcessFixtures)
    target_compile_definitions(${_owner} PRIVATE
      NEVERD_WINDOWS_PROCESS_FIXTURE_DIR="${_windows_fixture_dir}")
  endif()
endforeach()
