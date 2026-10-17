find_program(NEVERD_PROCESS_LD64 NAMES ld64.lld HINTS "${LLVM_TOOLS_BINARY_DIR}")
if(NOT NEVERD_TEST_CLANG_EXECUTABLE OR NOT NEVERD_PROCESS_LD64)
  message(STATUS "Darwin process fixtures unavailable: Clang and ld64.lld are required")
  return()
endif()
set(_darwin_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/darwin-fixtures")
file(MAKE_DIRECTORY "${_darwin_fixture_dir}")
# LLD can retain an unused dyld_stub_binder import with chained fixups. Only
# the classic lazy fixture exports it; each image must have a binding closure.
foreach(_darwin_link_kind classic chained)
  set(_darwin_files_binder "")
  if(_darwin_link_kind STREQUAL "classic")
    set(_darwin_files_binder ", dyld_stub_binder")
  endif()
  configure_file(fixtures/darwin_libsystem_files.tbd.in
    "${_darwin_fixture_dir}/libsystem-files-${_darwin_link_kind}.tbd" @ONLY)
  configure_file(fixtures/darwin_libsystem_memory.tbd.in
    "${_darwin_fixture_dir}/libsystem-memory-${_darwin_link_kind}.tbd" @ONLY)
endforeach()
set(_darwin_outputs)
foreach(_darwin_profile macos ios ios-simulator)
  foreach(_darwin_arch x86_64 arm64)
    if(_darwin_profile STREQUAL "ios" AND _darwin_arch STREQUAL "x86_64")
      continue()
    endif()
    if(_darwin_profile STREQUAL "macos")
      set(_darwin_target "${_darwin_arch}-apple-macos11")
    elseif(_darwin_profile STREQUAL "ios")
      set(_darwin_target "${_darwin_arch}-apple-ios14")
    else()
      set(_darwin_target "${_darwin_arch}-apple-ios14-simulator")
    endif()
    set(_darwin_output "${_darwin_fixture_dir}/${_darwin_profile}-${_darwin_arch}")
    add_custom_command(OUTPUT "${_darwin_output}" "${_darwin_output}.o"
      COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_darwin_target}"
        -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
        -nostdinc -isysroot "${_darwin_fixture_dir}"
        -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
        -fno-asynchronous-unwind-tables -O1 -c
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/darwin_process.c" -o "${_darwin_output}.o"
      COMMAND "${NEVERD_PROCESS_LD64}" -arch "${_darwin_arch}"
        -platform_version "${_darwin_profile}" 14.0 14.0 -e _main
        -no_fixup_chains "${_darwin_output}.o" -o "${_darwin_output}"
      DEPENDS fixtures/darwin_process.c
      VERBATIM)
    list(APPEND _darwin_outputs "${_darwin_output}")
    set(_darwin_libsystem_object "${_darwin_output}-libsystem.o")
    add_custom_command(OUTPUT "${_darwin_libsystem_object}"
      COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_darwin_target}"
        -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
        -nostdinc -isysroot "${_darwin_fixture_dir}"
        -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
        -fno-asynchronous-unwind-tables -O1 -c
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/darwin_libsystem.c"
        -o "${_darwin_libsystem_object}"
      DEPENDS fixtures/darwin_libsystem.c VERBATIM)
    foreach(_darwin_link_kind classic chained)
      if(_darwin_link_kind STREQUAL "classic")
        set(_darwin_fixup_flag -no_fixup_chains)
      else()
        set(_darwin_fixup_flag -fixup_chains)
      endif()
      set(_darwin_libsystem_output "${_darwin_output}-libsystem-${_darwin_link_kind}")
      add_custom_command(OUTPUT "${_darwin_libsystem_output}"
        COMMAND "${NEVERD_PROCESS_LD64}" -arch "${_darwin_arch}"
          -platform_version "${_darwin_profile}" 14.0 14.0 -e _main
          "${_darwin_fixup_flag}" "${_darwin_libsystem_object}"
          "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/darwin_libsystem.tbd"
          -o "${_darwin_libsystem_output}"
        DEPENDS "${_darwin_libsystem_object}" fixtures/darwin_libsystem.tbd
        VERBATIM)
      list(APPEND _darwin_outputs "${_darwin_libsystem_output}")
    endforeach()
    set(_darwin_lazy_output "${_darwin_output}-libsystem-lazy")
    add_custom_command(OUTPUT "${_darwin_lazy_output}" "${_darwin_lazy_output}.o"
      COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_darwin_target}"
        -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
        -nostdinc -isysroot "${_darwin_fixture_dir}"
        -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
        -fno-asynchronous-unwind-tables -O1 -c
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/darwin_libsystem_lazy.c"
        -o "${_darwin_lazy_output}.o"
      COMMAND "${NEVERD_PROCESS_LD64}" -arch "${_darwin_arch}"
        -platform_version "${_darwin_profile}" 14.0 14.0 -e _main
        -no_fixup_chains "${_darwin_lazy_output}.o"
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/darwin_libsystem_lazy.tbd"
        -o "${_darwin_lazy_output}"
      DEPENDS fixtures/darwin_libsystem_lazy.c fixtures/darwin_libsystem_lazy.tbd
      VERBATIM)
    list(APPEND _darwin_outputs "${_darwin_lazy_output}")
    set(_darwin_files_object "${_darwin_output}-libsystem-files.o")
    add_custom_command(OUTPUT "${_darwin_files_object}"
      COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_darwin_target}"
        -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
        -nostdinc -isysroot "${_darwin_fixture_dir}"
        -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
        -fno-asynchronous-unwind-tables -O1 -c
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/darwin_libsystem_files.c"
        -o "${_darwin_files_object}"
      DEPENDS fixtures/darwin_libsystem_files.c VERBATIM)
    foreach(_darwin_link_kind classic chained)
      if(_darwin_link_kind STREQUAL "classic")
        set(_darwin_fixup_flag -no_fixup_chains)
      else()
        set(_darwin_fixup_flag -fixup_chains)
      endif()
      set(_darwin_files_output "${_darwin_output}-libsystem-files-${_darwin_link_kind}")
      add_custom_command(OUTPUT "${_darwin_files_output}"
        COMMAND "${NEVERD_PROCESS_LD64}" -arch "${_darwin_arch}"
          -platform_version "${_darwin_profile}" 14.0 14.0 -e _main
          "${_darwin_fixup_flag}" "${_darwin_files_object}"
          "${_darwin_fixture_dir}/libsystem-files-${_darwin_link_kind}.tbd"
          -o "${_darwin_files_output}"
        DEPENDS "${_darwin_files_object}"
          "${_darwin_fixture_dir}/libsystem-files-${_darwin_link_kind}.tbd"
        VERBATIM)
      list(APPEND _darwin_outputs "${_darwin_files_output}")
    endforeach()
    set(_darwin_memory_object "${_darwin_output}-libsystem-memory.o")
    add_custom_command(OUTPUT "${_darwin_memory_object}"
      COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_darwin_target}"
        -std=c11 -ffreestanding -fno-builtin -fno-stack-protector
        -nostdinc -isysroot "${_darwin_fixture_dir}"
        -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
        -fno-asynchronous-unwind-tables -O1 -c
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/darwin_libsystem_memory.c"
        -o "${_darwin_memory_object}"
      DEPENDS fixtures/darwin_libsystem_memory.c VERBATIM)
    foreach(_darwin_link_kind classic chained)
      if(_darwin_link_kind STREQUAL "classic")
        set(_darwin_fixup_flag -no_fixup_chains)
      else()
        set(_darwin_fixup_flag -fixup_chains)
      endif()
      set(_darwin_memory_output "${_darwin_output}-libsystem-memory-${_darwin_link_kind}")
      add_custom_command(OUTPUT "${_darwin_memory_output}"
        COMMAND "${NEVERD_PROCESS_LD64}" -arch "${_darwin_arch}"
          -platform_version "${_darwin_profile}" 14.0 14.0 -e _main
          "${_darwin_fixup_flag}" "${_darwin_memory_object}"
          "${_darwin_fixture_dir}/libsystem-memory-${_darwin_link_kind}.tbd"
          -o "${_darwin_memory_output}"
        DEPENDS "${_darwin_memory_object}"
          "${_darwin_fixture_dir}/libsystem-memory-${_darwin_link_kind}.tbd"
        VERBATIM)
      list(APPEND _darwin_outputs "${_darwin_memory_output}")
    endforeach()
  endforeach()
endforeach()
add_custom_target(NeverDDarwinFixtures DEPENDS ${_darwin_outputs})
add_dependencies(NeverDDarwinProcessTests NeverDDarwinFixtures)
target_compile_definitions(NeverDDarwinProcessTests PRIVATE
  NEVERD_DARWIN_FIXTURE_DIR="${_darwin_fixture_dir}")
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_CROSSCOMPILING)
  if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(_darwin_native_arch arm64)
  elseif(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(_darwin_native_arch x86_64)
  else()
    message(FATAL_ERROR "Unsupported native Darwin oracle architecture")
  endif()
  set(_darwin_native "${_darwin_fixture_dir}/macos-native")
  set(_darwin_native_object "${_darwin_fixture_dir}/macos-${_darwin_native_arch}.o")
  # Reuse the exact authored object. Only the real host executable links
  # libSystem for dyld's main handoff; guest images remain SDK-free.
  add_custom_command(OUTPUT "${_darwin_native}"
    COMMAND xcrun clang -arch "${_darwin_native_arch}"
      "${_darwin_native_object}" -o "${_darwin_native}"
    DEPENDS "${_darwin_native_object}"
    VERBATIM)
  add_custom_target(NeverDDarwinNativeOracle DEPENDS "${_darwin_native}")
  add_dependencies(NeverDDarwinProcessTests NeverDDarwinNativeOracle)
  target_compile_definitions(NeverDDarwinProcessTests PRIVATE
    NEVERD_DARWIN_NATIVE_ORACLE="${_darwin_native}")
  set(_darwin_libsystem_native "${_darwin_fixture_dir}/macos-libsystem-native")
  add_custom_command(OUTPUT "${_darwin_libsystem_native}"
    COMMAND xcrun clang -arch "${_darwin_native_arch}"
      "${_darwin_fixture_dir}/macos-${_darwin_native_arch}-libsystem.o"
      -o "${_darwin_libsystem_native}"
    DEPENDS "${_darwin_fixture_dir}/macos-${_darwin_native_arch}-libsystem.o"
    VERBATIM)
  add_custom_target(NeverDDarwinLibSystemNativeOracle DEPENDS "${_darwin_libsystem_native}")
  add_dependencies(NeverDDarwinProcessTests NeverDDarwinLibSystemNativeOracle)
  target_compile_definitions(NeverDDarwinProcessTests PRIVATE
    NEVERD_DARWIN_LIBSYSTEM_NATIVE_ORACLE="${_darwin_libsystem_native}")
  set(_darwin_lazy_native "${_darwin_fixture_dir}/macos-libsystem-lazy-native")
  add_custom_command(OUTPUT "${_darwin_lazy_native}"
    COMMAND xcrun clang -arch "${_darwin_native_arch}" -Wl,-no_fixup_chains
      "${_darwin_fixture_dir}/macos-${_darwin_native_arch}-libsystem-lazy.o"
      -o "${_darwin_lazy_native}"
    DEPENDS "${_darwin_fixture_dir}/macos-${_darwin_native_arch}-libsystem-lazy.o"
    VERBATIM)
  add_custom_target(NeverDDarwinLazyNativeOracle DEPENDS "${_darwin_lazy_native}")
  add_dependencies(NeverDDarwinProcessTests NeverDDarwinLazyNativeOracle)
  target_compile_definitions(NeverDDarwinProcessTests PRIVATE
    NEVERD_DARWIN_LAZY_NATIVE_ORACLE="${_darwin_lazy_native}")
  set(_darwin_files_native "${_darwin_fixture_dir}/macos-libsystem-files-native")
  add_custom_command(OUTPUT "${_darwin_files_native}"
    COMMAND xcrun clang -arch "${_darwin_native_arch}" -Wl,-no_fixup_chains
      "${_darwin_fixture_dir}/macos-${_darwin_native_arch}-libsystem-files.o"
      -o "${_darwin_files_native}"
    DEPENDS "${_darwin_fixture_dir}/macos-${_darwin_native_arch}-libsystem-files.o"
    VERBATIM)
  add_custom_target(NeverDDarwinFilesNativeOracle DEPENDS "${_darwin_files_native}")
  add_dependencies(NeverDDarwinProcessTests NeverDDarwinFilesNativeOracle)
  target_compile_definitions(NeverDDarwinProcessTests PRIVATE
    NEVERD_DARWIN_FILES_NATIVE_ORACLE="${_darwin_files_native}")
  set(_darwin_memory_native "${_darwin_fixture_dir}/macos-libsystem-memory-native")
  add_custom_command(OUTPUT "${_darwin_memory_native}"
    COMMAND xcrun clang -arch "${_darwin_native_arch}" -Wl,-no_fixup_chains
      "${_darwin_fixture_dir}/macos-${_darwin_native_arch}-libsystem-memory.o"
      -o "${_darwin_memory_native}"
    DEPENDS "${_darwin_fixture_dir}/macos-${_darwin_native_arch}-libsystem-memory.o"
    VERBATIM)
  add_custom_target(NeverDDarwinMemoryNativeOracle DEPENDS "${_darwin_memory_native}")
  add_dependencies(NeverDDarwinProcessTests NeverDDarwinMemoryNativeOracle)
  target_compile_definitions(NeverDDarwinProcessTests PRIVATE
    NEVERD_DARWIN_MEMORY_NATIVE_ORACLE="${_darwin_memory_native}")
endif()
if(TARGET NeverDProcessPublicTests)
  add_dependencies(NeverDProcessPublicTests NeverDDarwinFixtures)
  target_compile_definitions(NeverDProcessPublicTests PRIVATE
    NEVERD_DARWIN_FIXTURE_DIR="${_darwin_fixture_dir}")
endif()
include(DarwinDylibFixtures.cmake)
include(DarwinAddendFixtures.cmake)

include(DarwinCallbackFixtures.cmake)

include(DarwinGuardFixtures.cmake)
