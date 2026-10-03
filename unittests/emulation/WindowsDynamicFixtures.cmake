if(NOT TARGET NeverDWindowsProcessFixtures)
  return()
endif()
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsDynamicCases.def"
  _settings REGEX "^NEVERD_DYNAMIC_(BUILD|TEXT)\\(")
foreach(_setting IN LISTS _settings)
  if(_setting MATCHES "^NEVERD_DYNAMIC_(BUILD|TEXT)\\(([A-Za-z0-9_]+), \"([^\"]*)\"\\)$")
    set("_dynamic_${CMAKE_MATCH_2}" "${CMAKE_MATCH_3}")
  endif()
endforeach()
set(_dynamic_dir "${CMAKE_CURRENT_BINARY_DIR}/${_dynamic_OutputDirectory}")
file(MAKE_DIRECTORY "${_dynamic_dir}")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsDynamicCases.def"
  _exports REGEX "^NEVERD_DYNAMIC_TOP_EXPORT")
set(_definition "LIBRARY ${_dynamic_TopFile}\nEXPORTS\n")
foreach(_export IN LISTS _exports)
  string(REGEX REPLACE "^NEVERD_DYNAMIC_TOP_EXPORT\\(([A-Za-z0-9_]+), \"([^\"]*)\"\\)" "  \\1 \\2\n" _line "${_export}")
  string(APPEND _definition "${_line}")
endforeach()
file(CONFIGURE OUTPUT "${_dynamic_dir}/top.def" CONTENT "${_definition}" @ONLY)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsDynamicCases.def"
  _exports REGEX "^NEVERD_DYNAMIC_MIDDLE_EXPORT")
set(_definition "LIBRARY ${_dynamic_MiddleFile}\nEXPORTS\n")
foreach(_export IN LISTS _exports)
  string(REGEX REPLACE "^NEVERD_DYNAMIC_MIDDLE_EXPORT\\(([A-Za-z0-9_]+), \"([^\"]*)\"\\)" "  \\1 \\2\n" _line "${_export}")
  string(APPEND _definition "${_line}")
endforeach()
file(CONFIGURE OUTPUT "${_dynamic_dir}/middle.def" CONTENT "${_definition}" @ONLY)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsDynamicCases.def"
  _apis REGEX "^NEVERD_DYNAMIC_API")
set(_definition "LIBRARY ${_dynamic_ProviderFile}\nEXPORTS\n")
foreach(_api IN LISTS _apis)
  string(REGEX REPLACE "^NEVERD_DYNAMIC_API\\(([A-Za-z0-9_]+)\\)" "  \\1\n" _line "${_api}")
  string(APPEND _definition "${_line}")
endforeach()
file(CONFIGURE OUTPUT "${_dynamic_dir}/kernel32.def" CONTENT "${_definition}" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsDynamicCases.def")
set(_dynamic_outputs)
foreach(_arch X64 AArch64)
  set(_target "${_dynamic_${_arch}Target}")
  set(_machine "${_dynamic_${_arch}Machine}")
  set(_dir "${_dynamic_dir}/${_arch}")
  file(MAKE_DIRECTORY "${_dir}/${_dynamic_NoEntryDirectory}")
  set(_compile "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_target}"
    -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
    -fno-vectorize -fno-slp-vectorize -O1 -c)
  get_filename_component(_provider "${_dynamic_ProviderFile}" NAME_WE)
  set(_kernel "${_windows_fixture_dir}/${_arch}-${_provider}.lib")
  get_filename_component(_dynamic_LeafImport "${_dynamic_LeafFile}" NAME_WE)
  string(APPEND _dynamic_LeafImport ".lib")
  set(_dynamic_kernel "${_dir}/${_provider}.lib")
  add_custom_command(OUTPUT "${_dynamic_kernel}"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /lib "/machine:${_machine}"
      "/def:${_dynamic_dir}/kernel32.def" "/out:${_dynamic_kernel}"
    DEPENDS "${_dynamic_dir}/kernel32.def" VERBATIM)
  foreach(_module leaf middle top)
    string(SUBSTRING "${_module}" 0 1 _initial)
    string(TOUPPER "${_initial}" _initial)
    string(SUBSTRING "${_module}" 1 -1 _rest)
    set(_file "${_dynamic_${_initial}${_rest}File}")
    get_filename_component(_stem "${_file}" NAME_WE)
    set(_imports "${_kernel}" "${_dynamic_kernel}")
    set(_link /include:_tls_used)
    if(_module STREQUAL "middle")
      list(APPEND _link "/def:${_dynamic_dir}/middle.def")
      list(APPEND _imports "${_dir}/${_dynamic_LeafImport}")
    elseif(_module STREQUAL "top")
      set(_link "/def:${_dynamic_dir}/top.def")
    endif()
    add_custom_command(OUTPUT "${_dir}/${_file}" "${_dir}/${_stem}.lib" "${_dir}/${_module}.obj"
      COMMAND ${_compile} "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/windows_dynamic_${_module}.c"
        -o "${_dir}/${_module}.obj"
      COMMAND "${NEVERD_PROCESS_LLD_LINK}" /dll "/entry:${_dynamic_DLLEntry}" /nodefaultlib /subsystem:console
        "/machine:${_machine}" "/base:${_dynamic_DLLBase}" /timestamp:0 ${_link}
        "${_dir}/${_module}.obj" ${_imports}
        "/out:${_dir}/${_file}" "/implib:${_dir}/${_stem}.lib"
      DEPENDS "fixtures/windows_dynamic_${_module}.c" fixtures/WindowsDynamicFixture.h
        fixtures/WindowsDynamicCases.def "${_dynamic_dir}/top.def" "${_dynamic_dir}/middle.def" ${_imports}
      VERBATIM)
    if(NOT _module STREQUAL "top")
      add_custom_command(OUTPUT "${_dir}/${_dynamic_NoEntryDirectory}/${_file}"
        COMMAND "${NEVERD_PROCESS_LLD_LINK}" /dll /noentry /nodefaultlib /subsystem:console
          "/machine:${_machine}" "/base:${_dynamic_DLLBase}" /timestamp:0 ${_link}
          "${_dir}/${_module}.obj" ${_imports}
          "/out:${_dir}/${_dynamic_NoEntryDirectory}/${_file}"
        DEPENDS "${_dir}/${_module}.obj" ${_imports} VERBATIM)
      list(APPEND _dynamic_outputs "${_dir}/${_dynamic_NoEntryDirectory}/${_file}")
    endif()
    list(APPEND _dynamic_outputs "${_dir}/${_file}")
  endforeach()
  foreach(_kind ProgramFile StaticProgramFile)
    set(_file "${_dynamic_${_kind}}")
    get_filename_component(_program "${_file}" NAME_WE)
    set(_flags)
    set(_imports "${_kernel}" "${_dynamic_kernel}")
    if(_kind STREQUAL "StaticProgramFile")
      list(APPEND _flags -DNEVERD_DYNAMIC_STATIC)
      get_filename_component(_middle "${_dynamic_MiddleFile}" NAME_WE)
      list(APPEND _imports "${_dir}/${_middle}.lib")
    endif()
    add_custom_command(OUTPUT "${_dir}/${_file}" "${_dir}/${_program}.obj"
      COMMAND ${_compile} ${_flags} "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/windows_dynamic_process.c"
        -o "${_dir}/${_program}.obj"
      COMMAND "${NEVERD_PROCESS_LLD_LINK}" /nodefaultlib "/entry:${_dynamic_ProgramEntry}" /subsystem:console
        "/machine:${_machine}" "/base:${_dynamic_ProgramBase}" /timestamp:0
        "${_dir}/${_program}.obj" ${_imports} "/out:${_dir}/${_file}"
      DEPENDS fixtures/windows_dynamic_process.c fixtures/WindowsDynamicFixture.h
        fixtures/WindowsDynamicCases.def ${_imports} VERBATIM)
    list(APPEND _dynamic_outputs "${_dir}/${_file}")
  endforeach()
endforeach()
add_custom_target(NeverDWindowsDynamicFixtures DEPENDS ${_dynamic_outputs})
add_dependencies(NeverDWindowsProcessTests NeverDWindowsDynamicFixtures)
target_compile_definitions(NeverDWindowsProcessTests PRIVATE
  NEVERD_WINDOWS_DYNAMIC_FIXTURE_DIR="${_dynamic_dir}")
