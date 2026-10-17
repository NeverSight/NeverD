if(NOT NEVERD_TEST_CLANG_EXECUTABLE OR NOT NEVERD_PROCESS_LLD)
  message(STATUS "Android native fixtures unavailable: Clang and ld.lld required")
  return()
endif()
set(_android_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/android-fixtures")
file(MAKE_DIRECTORY "${_android_fixture_dir}")
set(_android_object "${_android_fixture_dir}/native.o")
add_custom_command(OUTPUT "${_android_object}"
  COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" --target=aarch64-linux-android28
    -std=c11 -ffreestanding -fno-builtin -fPIC -fstack-protector-all
    -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
    -fno-asynchronous-unwind-tables -O1 -c
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/android_native.c" -o "${_android_object}"
  DEPENDS fixtures/android_native.c VERBATIM)
set(_android_output_object "${_android_fixture_dir}/output.o")
add_custom_command(OUTPUT "${_android_output_object}"
  COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" --target=aarch64-linux-android28
    -std=c11 -ffreestanding -fno-builtin -fPIC -fstack-protector-all
    -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
    -fno-asynchronous-unwind-tables -O1 -c
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/android_output.c" -o "${_android_output_object}"
  DEPENDS fixtures/android_output.c fixtures/LinuxOutputCases.def VERBATIM)
set(_android_outputs)
foreach(_packing none android relr)
  set(_android_file "${_android_fixture_dir}/${_packing}.so")
  add_custom_command(OUTPUT "${_android_file}"
    COMMAND "${NEVERD_PROCESS_LLD}" -shared -z max-page-size=4096
      --build-id=none --hash-style=gnu "--pack-dyn-relocs=${_packing}"
      "${_android_object}" "${_android_output_object}" -o "${_android_file}"
    DEPENDS "${_android_object}" "${_android_output_object}" VERBATIM)
  list(APPEND _android_outputs "${_android_file}")
endforeach()
foreach(_optimization O0 O2)
  set(_once_object "${_android_fixture_dir}/once-${_optimization}.o")
  add_custom_command(OUTPUT "${_once_object}"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" --target=aarch64-linux-android28
      -std=c11 -ffreestanding -fno-builtin -fPIC -fstack-protector-all
      -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
      -fno-asynchronous-unwind-tables "-${_optimization}" -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/android_once.c" -o "${_once_object}"
    DEPENDS fixtures/android_once.c VERBATIM)
  foreach(_packing none android relr)
    set(_once_file "${_android_fixture_dir}/once-${_optimization}-${_packing}.so")
    add_custom_command(OUTPUT "${_once_file}"
      COMMAND "${NEVERD_PROCESS_LLD}" -shared -z max-page-size=4096
        --build-id=none --hash-style=gnu "--pack-dyn-relocs=${_packing}"
        "${_once_object}" -o "${_once_file}"
      DEPENDS "${_once_object}" VERBATIM)
    list(APPEND _android_outputs "${_once_file}")
  endforeach()
endforeach()
foreach(_optimization O0 O2)
  set(_token_object "${_android_fixture_dir}/token-${_optimization}.o")
  add_custom_command(OUTPUT "${_token_object}"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" --target=aarch64-linux-android28
      -std=c11 -ffreestanding -fno-builtin -fPIC -fstack-protector-all
      -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
      -fno-asynchronous-unwind-tables "-${_optimization}" -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/android_token.c" -o "${_token_object}"
    DEPENDS fixtures/android_token.c VERBATIM)
  foreach(_packing none android relr)
    set(_token_file "${_android_fixture_dir}/token-${_optimization}-${_packing}.so")
    add_custom_command(OUTPUT "${_token_file}"
      COMMAND "${NEVERD_PROCESS_LLD}" -shared -z max-page-size=4096
        --build-id=none --hash-style=gnu "--pack-dyn-relocs=${_packing}"
        "${_token_object}" -o "${_token_file}"
      DEPENDS "${_token_object}" VERBATIM)
    list(APPEND _android_outputs "${_token_file}")
  endforeach()
endforeach()
foreach(_optimization O0 O2)
  set(_android_mutex_object "${_android_fixture_dir}/mutex-${_optimization}.o")
  add_custom_command(OUTPUT "${_android_mutex_object}"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" --target=aarch64-linux-android28
      -std=c11 -ffreestanding -fno-builtin -fPIC -fstack-protector-all
      -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
      -fno-asynchronous-unwind-tables "-${_optimization}" -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/android_mutex.c"
      -o "${_android_mutex_object}"
    DEPENDS fixtures/android_mutex.c VERBATIM)
  foreach(_packing none android relr)
    set(_android_mutex_file "${_android_fixture_dir}/mutex-${_optimization}-${_packing}.so")
    add_custom_command(OUTPUT "${_android_mutex_file}"
      COMMAND "${NEVERD_PROCESS_LLD}" -shared -z max-page-size=4096
        --build-id=none --hash-style=gnu "--pack-dyn-relocs=${_packing}"
        "${_android_mutex_object}" -o "${_android_mutex_file}"
      DEPENDS "${_android_mutex_object}" VERBATIM)
    list(APPEND _android_outputs "${_android_mutex_file}")
  endforeach()
endforeach()
foreach(_optimization O0 O2)
  set(_android_syscall_object "${_android_fixture_dir}/syscall-${_optimization}.o")
  add_custom_command(OUTPUT "${_android_syscall_object}"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" --target=aarch64-linux-android28
      -std=c11 -ffreestanding -fno-builtin -fPIC -fstack-protector-all
      -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
      -fno-asynchronous-unwind-tables "-${_optimization}" -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/android_syscall.c"
      -o "${_android_syscall_object}"
    DEPENDS fixtures/android_syscall.c VERBATIM)
  foreach(_packing none android relr)
    set(_android_syscall_file "${_android_fixture_dir}/syscall-${_optimization}-${_packing}.so")
    add_custom_command(OUTPUT "${_android_syscall_file}"
      COMMAND "${NEVERD_PROCESS_LLD}" -shared -z max-page-size=4096
        --build-id=none --hash-style=gnu "--pack-dyn-relocs=${_packing}"
        "${_android_syscall_object}" -o "${_android_syscall_file}"
      DEPENDS "${_android_syscall_object}" VERBATIM)
    list(APPEND _android_outputs "${_android_syscall_file}")
  endforeach()
endforeach()
foreach(_optimization O0 O2)
  foreach(_file_source android_time.c linux_files.c android_search.c
      android_signals.c android_snapshots.c android_allocation.c android_log.c linux_residency.c
      android_sleep.c linux_devices.c android_proc.c android_jni.c android_binder.c
      android_semaphore.c android_random.c android_network.c android_auxv.c
      android_calendar.c)
    get_filename_component(_file_kind "${_file_source}" NAME_WE)
    string(REGEX REPLACE "^(android|linux)_" "" _file_kind "${_file_kind}")
    set(_service_object "${_android_fixture_dir}/${_file_kind}-${_optimization}.o")
    add_custom_command(OUTPUT "${_service_object}"
      COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" --target=aarch64-linux-android28
        -std=c11 -ffreestanding -fno-builtin -fPIC -fstack-protector-all
        -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
        -fno-asynchronous-unwind-tables "-${_optimization}" -c
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/${_file_source}" -o "${_service_object}"
      DEPENDS "fixtures/${_file_source}" VERBATIM)
    foreach(_packing none android relr)
      set(_service_file "${_android_fixture_dir}/${_file_kind}-${_optimization}-${_packing}.so")
      add_custom_command(OUTPUT "${_service_file}"
        COMMAND "${NEVERD_PROCESS_LLD}" -shared -z max-page-size=4096
          --build-id=none --hash-style=gnu "--pack-dyn-relocs=${_packing}"
          "${_service_object}" -o "${_service_file}"
        DEPENDS "${_service_object}" VERBATIM)
      list(APPEND _android_outputs "${_service_file}")
    endforeach()
  endforeach()
endforeach()
foreach(_optimization O0 O2)
  foreach(_string_kind format scan)
    set(_string_object "${_android_fixture_dir}/${_string_kind}-${_optimization}.o")
    add_custom_command(OUTPUT "${_string_object}"
      COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" --target=aarch64-linux-android28
        -std=c11 -ffreestanding -fno-builtin -fPIC -fstack-protector-all
        -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
        -fno-asynchronous-unwind-tables "-${_optimization}" -c
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/android_${_string_kind}.c" -o "${_string_object}"
      DEPENDS fixtures/android_${_string_kind}.c VERBATIM)
    foreach(_packing none android relr)
      set(_string_file "${_android_fixture_dir}/${_string_kind}-${_optimization}-${_packing}.so")
      add_custom_command(OUTPUT "${_string_file}"
        COMMAND "${NEVERD_PROCESS_LLD}" -shared -z max-page-size=4096
          --build-id=none --hash-style=gnu "--pack-dyn-relocs=${_packing}"
          "${_string_object}" -o "${_string_file}"
        DEPENDS "${_string_object}" VERBATIM)
      list(APPEND _android_outputs "${_string_file}")
    endforeach()
  endforeach()
endforeach()
foreach(_optimization O0 O2)
  set(_attribute_object "${_android_fixture_dir}/thread-attributes-${_optimization}.o")
  add_custom_command(OUTPUT "${_attribute_object}"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" --target=aarch64-linux-android28
      -std=c11 -ffreestanding -fno-builtin -fPIC -fstack-protector-all
      -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
      -fno-asynchronous-unwind-tables "-${_optimization}" -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/android_thread_attributes.c" -o "${_attribute_object}"
    DEPENDS fixtures/android_thread_attributes.c VERBATIM)
  foreach(_packing none android relr)
    set(_attribute_file "${_android_fixture_dir}/thread-attributes-${_optimization}-${_packing}.so")
    add_custom_command(OUTPUT "${_attribute_file}"
      COMMAND "${NEVERD_PROCESS_LLD}" -shared -z max-page-size=4096
        --build-id=none --hash-style=gnu "--pack-dyn-relocs=${_packing}"
        "${_attribute_object}" -o "${_attribute_file}"
      DEPENDS "${_attribute_object}" VERBATIM)
    list(APPEND _android_outputs "${_attribute_file}")
  endforeach()
endforeach()
foreach(_optimization O0 O2)
  set(_threads_object "${_android_fixture_dir}/threads-${_optimization}.o")
  add_custom_command(OUTPUT "${_threads_object}"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" --target=aarch64-linux-android28
      -std=c11 -ffreestanding -fno-builtin -fPIC -fstack-protector-all
      -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
      -fno-asynchronous-unwind-tables "-${_optimization}" -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/android_threads.c" -o "${_threads_object}"
    DEPENDS fixtures/android_threads.c VERBATIM)
  foreach(_packing none android relr)
    set(_threads_file "${_android_fixture_dir}/threads-${_optimization}-${_packing}.so")
    add_custom_command(OUTPUT "${_threads_file}"
      COMMAND "${NEVERD_PROCESS_LLD}" -shared -z max-page-size=4096
        --build-id=none --hash-style=gnu "--pack-dyn-relocs=${_packing}"
        "${_threads_object}" -o "${_threads_file}"
      DEPENDS "${_threads_object}" VERBATIM)
    list(APPEND _android_outputs "${_threads_file}")
  endforeach()
endforeach()
foreach(_optimization O0 O2)
  set(_finalizers_object "${_android_fixture_dir}/finalizers-${_optimization}.o")
  add_custom_command(OUTPUT "${_finalizers_object}"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" --target=aarch64-linux-android28
      -std=c11 -ffreestanding -fno-builtin -fPIC -fstack-protector-all
      -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
      -fno-asynchronous-unwind-tables "-${_optimization}" -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/android_finalizers.c" -o "${_finalizers_object}"
    DEPENDS fixtures/android_finalizers.c VERBATIM)
  foreach(_packing none android relr)
    set(_finalizers_file "${_android_fixture_dir}/finalizers-${_optimization}-${_packing}.so")
    add_custom_command(OUTPUT "${_finalizers_file}"
      COMMAND "${NEVERD_PROCESS_LLD}" -shared -z max-page-size=4096
        --build-id=none --hash-style=gnu "--pack-dyn-relocs=${_packing}"
        "${_finalizers_object}" -o "${_finalizers_file}"
      DEPENDS "${_finalizers_object}" VERBATIM)
    list(APPEND _android_outputs "${_finalizers_file}")
  endforeach()
endforeach()
foreach(_optimization O0 O2)
  set(_properties_object "${_android_fixture_dir}/properties-${_optimization}.o")
  add_custom_command(OUTPUT "${_properties_object}"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" --target=aarch64-linux-android28
      -std=c11 -ffreestanding -fno-builtin -fPIC -fstack-protector-all
      -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
      -fno-asynchronous-unwind-tables "-${_optimization}" -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/android_properties.c"
      -o "${_properties_object}"
    DEPENDS fixtures/android_properties.c VERBATIM)
  foreach(_packing none android relr)
    set(_properties_file "${_android_fixture_dir}/properties-${_optimization}-${_packing}.so")
    add_custom_command(OUTPUT "${_properties_file}"
      COMMAND "${NEVERD_PROCESS_LLD}" -shared -z max-page-size=4096
        --build-id=none --hash-style=gnu "--pack-dyn-relocs=${_packing}"
        "${_properties_object}" -o "${_properties_file}"
      DEPENDS "${_properties_object}" VERBATIM)
    list(APPEND _android_outputs "${_properties_file}")
  endforeach()
endforeach()
foreach(_optimization O0 O2)
  set(_stdio_object "${_android_fixture_dir}/stdio-${_optimization}.o")
  add_custom_command(OUTPUT "${_stdio_object}"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" --target=aarch64-linux-android28
      -std=c11 -ffreestanding -fno-builtin -fPIC -fstack-protector-all
      -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
      -fno-asynchronous-unwind-tables "-${_optimization}" -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/android_stdio.c"
      -o "${_stdio_object}"
    DEPENDS fixtures/android_stdio.c VERBATIM)
  foreach(_packing none android relr)
    set(_stdio_file "${_android_fixture_dir}/stdio-${_optimization}-${_packing}.so")
    add_custom_command(OUTPUT "${_stdio_file}"
      COMMAND "${NEVERD_PROCESS_LLD}" -shared -z max-page-size=4096
        --build-id=none --hash-style=gnu "--pack-dyn-relocs=${_packing}"
        "${_stdio_object}" -o "${_stdio_file}"
      DEPENDS "${_stdio_object}" VERBATIM)
    list(APPEND _android_outputs "${_stdio_file}")
  endforeach()
endforeach()
add_custom_target(NeverDAndroidFixtures DEPENDS ${_android_outputs})
add_dependencies(NeverDAndroidNativeTests NeverDAndroidFixtures)
target_compile_definitions(NeverDAndroidNativeTests PRIVATE
  NEVERD_ANDROID_FIXTURE_DIR="${_android_fixture_dir}")
if(TARGET NeverDProcessPublicTests)
  add_dependencies(NeverDProcessPublicTests NeverDAndroidFixtures)
  target_compile_definitions(NeverDProcessPublicTests PRIVATE
    NEVERD_ANDROID_FIXTURE_DIR="${_android_fixture_dir}")
endif()
