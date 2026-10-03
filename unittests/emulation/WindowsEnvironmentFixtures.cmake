if(NOT TARGET NeverDWindowsProcessFixtures)
  return()
endif()
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsEnvironmentCases.def"
  _settings REGEX "^NEVERD_ENV_(BUILD|TEXT)\\(")
foreach(_setting IN LISTS _settings)
  if(_setting MATCHES "^NEVERD_ENV_(BUILD|TEXT)\\(([A-Za-z0-9_]+), \"([^\"]*)\"\\)$")
    set("_env_${CMAKE_MATCH_2}" "${CMAKE_MATCH_3}")
  endif()
endforeach()
set(_env_dir "${CMAKE_CURRENT_BINARY_DIR}/${_env_OutputDirectory}")
file(MAKE_DIRECTORY "${_env_dir}")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsEnvironmentCases.def"
  _apis REGEX "^NEVERD_ENV_API")
set(_definition "LIBRARY ${_env_ProviderFile}\nEXPORTS\n")
foreach(_api IN LISTS _apis)
  string(REGEX REPLACE "^NEVERD_ENV_API\\(([A-Za-z0-9_]+)\\)" "  \\1\n" _line "${_api}")
  string(APPEND _definition "${_line}")
endforeach()
file(CONFIGURE OUTPUT "${_env_dir}/provider.def" CONTENT "${_definition}" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/WindowsEnvironmentCases.def")
set(_env_outputs)
foreach(_arch X64 AArch64)
  set(_dir "${_env_dir}/${_arch}")
  file(MAKE_DIRECTORY "${_dir}")
  add_custom_command(OUTPUT "${_dir}/${_env_ProgramFile}" "${_dir}/program.obj"
      "${_dir}/provider.lib"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /lib "/machine:${_env_${_arch}Machine}"
      "/def:${_env_dir}/provider.def" "/out:${_dir}/provider.lib"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_env_${_arch}Target}"
      -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
      -fno-vectorize -fno-slp-vectorize -O1 -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/windows_environment.c" -o "${_dir}/program.obj"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /nodefaultlib "/entry:${_env_ProgramEntry}"
      /subsystem:console "/machine:${_env_${_arch}Machine}"
      "/base:${_env_ProgramBase}" /timestamp:0
      "${_dir}/program.obj" "${_dir}/provider.lib" "/out:${_dir}/${_env_ProgramFile}"
    DEPENDS fixtures/windows_environment.c fixtures/WindowsEnvironmentCases.def
      "${_env_dir}/provider.def" VERBATIM)
  list(APPEND _env_outputs "${_dir}/${_env_ProgramFile}")
endforeach()
add_custom_target(NeverDWindowsEnvironmentFixtures DEPENDS ${_env_outputs})
add_dependencies(NeverDWindowsProcessTests NeverDWindowsEnvironmentFixtures)
target_compile_definitions(NeverDWindowsProcessTests PRIVATE
  NEVERD_WINDOWS_ENVIRONMENT_FIXTURE_DIR="${_env_dir}")
