if(NOT TARGET NeverDWindowsProcessFixtures)
  return()
endif()
set(_generated_cases
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/UnpackGeneratedCases.def")
file(STRINGS "${_generated_cases}" _settings
  REGEX "^NEVERD_GENERATED_(BUILD|TEXT)\\(")
foreach(_setting IN LISTS _settings)
  if(_setting MATCHES "^NEVERD_GENERATED_(BUILD|TEXT)\\(([A-Za-z0-9_]+), \"([^\"]*)\"\\)$")
    set("_generated_${CMAKE_MATCH_2}" "${CMAKE_MATCH_3}")
  endif()
endforeach()
set(_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/${_generated_OutputDirectory}")
file(MAKE_DIRECTORY "${_generated_dir}")
# The import library comes from the same inventory the fixture declares.
file(STRINGS "${_generated_cases}" _apis
  REGEX "^NEVERD_GENERATED_KERNEL_API\\(")
set(_definition "LIBRARY ${_generated_KernelModule}\nEXPORTS\n")
foreach(_api IN LISTS _apis)
  string(REGEX REPLACE "^NEVERD_GENERATED_KERNEL_API\\(([A-Za-z0-9_]+)\\)"
    "  \\1\n" _line "${_api}")
  string(APPEND _definition "${_line}")
endforeach()
file(CONFIGURE OUTPUT "${_generated_dir}/KERNEL.def"
  CONTENT "${_definition}" @ONLY)
file(STRINGS "${_generated_cases}" _user_apis
  REGEX "^NEVERD_GENERATED_USER_API\\(")
set(_user_definition "LIBRARY ${_generated_OpaqueModule}\nEXPORTS\n")
foreach(_api IN LISTS _user_apis)
  string(REGEX REPLACE "^NEVERD_GENERATED_USER_API\\(([A-Za-z0-9_]+)\\)"
    "  \\1\n" _line "${_api}")
  string(APPEND _user_definition "${_line}")
endforeach()
file(CONFIGURE OUTPUT "${_generated_dir}/USER.def"
  CONTENT "${_user_definition}" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${_generated_cases}")
set(_generated_outputs)
foreach(_arch X64 AArch64)
  set(_dir "${_generated_dir}/${_arch}")
  file(MAKE_DIRECTORY "${_dir}")
  add_custom_command(OUTPUT "${_dir}/${_generated_ProgramFile}"
      "${_dir}/program.obj" "${_dir}/kernel.lib" "${_dir}/user.lib"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /lib
      "/machine:${_generated_${_arch}Machine}"
      "/def:${_generated_dir}/KERNEL.def" "/out:${_dir}/kernel.lib"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /lib
      "/machine:${_generated_${_arch}Machine}"
      "/def:${_generated_dir}/USER.def" "/out:${_dir}/user.lib"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}"
      "--target=${_generated_${_arch}Target}"
      -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
      -fno-vectorize -fno-slp-vectorize -O1 -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/unpack_generated.c"
      -o "${_dir}/program.obj"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /nodefaultlib /noimplib
      "/entry:${_generated_ProgramEntry}" /subsystem:console
      "/machine:${_generated_${_arch}Machine}"
      "/base:${_generated_ProgramBase}" /timestamp:0
      "/section:${_generated_ProgramAccess}"
      "/section:${_generated_RelayAccess}"
      "/section:${_generated_CacheAccess}"
      "/merge:${_generated_CacheMerge}"
      "${_dir}/program.obj" "${_dir}/kernel.lib" "${_dir}/user.lib"
      "/out:${_dir}/${_generated_ProgramFile}"
    DEPENDS fixtures/unpack_generated.c fixtures/unpack_pointer_state.h
      fixtures/unpack_dynamic_tls_state.h fixtures/unpack_runtime_state.h
      "${_generated_cases}"
      "${_generated_dir}/KERNEL.def" "${_generated_dir}/USER.def"
    VERBATIM)
  list(APPEND _generated_outputs "${_dir}/${_generated_ProgramFile}")
  add_custom_command(OUTPUT "${_dir}/${_generated_TLSHeapProgramFile}"
      "${_dir}/tls-heap.obj"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}"
      "--target=${_generated_${_arch}Target}"
      -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
      -fno-vectorize -fno-slp-vectorize -O1 -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/unpack_tls_heap.c"
      -o "${_dir}/tls-heap.obj"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /nodefaultlib /noimplib
      "/entry:${_generated_ProgramEntry}" /subsystem:console /include:_tls_used
      "/machine:${_generated_${_arch}Machine}"
      "/base:${_generated_ProgramBase}" /timestamp:0
      "/section:${_generated_ProgramAccess}"
      "${_dir}/tls-heap.obj" "${_dir}/kernel.lib"
      "/out:${_dir}/${_generated_TLSHeapProgramFile}"
    DEPENDS fixtures/unpack_tls_heap.c fixtures/unpack_pointer_state.h
      "${_generated_cases}"
      "${_dir}/${_generated_ProgramFile}"
    VERBATIM)
  list(APPEND _generated_outputs "${_dir}/${_generated_TLSHeapProgramFile}")

  add_custom_command(OUTPUT "${_dir}/delay.exe" "${_dir}/unpack_delay.dll"
      "${_dir}/delay-provider.obj" "${_dir}/delay-program.obj"
      "${_dir}/delay.lib"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}"
      "--target=${_generated_${_arch}Target}" -ffreestanding
      -fno-builtin -fno-stack-protector -O1 -DDELAY_PROVIDER -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/unpack_delay.c"
      -o "${_dir}/delay-provider.obj"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /nodefaultlib /dll
      /entry:dll_entry /base:0x190000000 /timestamp:0
      "/machine:${_generated_${_arch}Machine}"
      "${_dir}/delay-provider.obj" "/out:${_dir}/unpack_delay.dll"
      "/implib:${_dir}/delay.lib"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}"
      "--target=${_generated_${_arch}Target}" -ffreestanding
      -fno-builtin -fno-stack-protector -fno-vectorize -fno-slp-vectorize
      -O1 -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/unpack_delay.c"
      -o "${_dir}/delay-program.obj"
    COMMAND "${NEVERD_PROCESS_LLD_LINK}" /nodefaultlib /noimplib
      /entry:program /subsystem:console /base:0x180000000 /timestamp:0
      /section:.prog,rwe /delayload:unpack_delay.dll
      "/machine:${_generated_${_arch}Machine}"
      "${_dir}/delay-program.obj" "${_dir}/delay.lib" "${_dir}/kernel.lib"
      "/out:${_dir}/delay.exe"
    DEPENDS fixtures/unpack_delay.c "${_dir}/${_generated_ProgramFile}"
    VERBATIM)
  list(APPEND _generated_outputs "${_dir}/delay.exe" "${_dir}/unpack_delay.dll")
endforeach()
add_custom_target(NeverDUnpackGeneratedFixtures DEPENDS ${_generated_outputs})
foreach(_test NeverDUnpackExecutionTests NeverDUnpackPublicTests)
  if(TARGET ${_test})
    add_dependencies(${_test} NeverDUnpackGeneratedFixtures)
    target_compile_definitions(${_test} PRIVATE
      NEVERD_UNPACK_GENERATED_FIXTURE_DIR="${_generated_dir}")
  endif()
endforeach()
