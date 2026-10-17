# Default compiler static guards in two dependency dylibs and the main image.
set(_darwin_guard_outputs)
configure_file(fixtures/darwin_guard_libcpp.tbd.in
  "${_darwin_fixture_dir}/guard-libcpp.tbd" @ONLY)
set(_darwin_guard_cpp "${_darwin_fixture_dir}/guard-libcpp.tbd")
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
    set(_darwin_guard_isa)
    if(_darwin_arch STREQUAL "arm64")
      set(_darwin_guard_isa -march=armv8-a -mcpu=generic)
    endif()
    set(_darwin_guard_prefix "${_darwin_fixture_dir}/${_darwin_profile}-${_darwin_arch}-guard")
    foreach(_darwin_unit leaf parent main)
      add_custom_command(OUTPUT "${_darwin_guard_prefix}-${_darwin_unit}.o"
        COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_darwin_target}"
          -std=c++17 -fno-exceptions -fno-rtti ${_darwin_guard_isa}
          -ffreestanding -fno-builtin -fno-stack-protector -nostdinc
          -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
          -fno-asynchronous-unwind-tables -O1 -c
          "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/darwin_guard_${_darwin_unit}.cpp"
          -o "${_darwin_guard_prefix}-${_darwin_unit}.o"
        DEPENDS "fixtures/darwin_guard_${_darwin_unit}.cpp"
          fixtures/darwin_guard_startup.h VERBATIM)
    endforeach()
    foreach(_darwin_kind classic chained)
      if(_darwin_kind STREQUAL "classic")
        set(_darwin_flag -no_fixup_chains)
      else()
        set(_darwin_flag -fixup_chains)
      endif()
      set(_darwin_dir "${_darwin_guard_prefix}-${_darwin_kind}")
      file(MAKE_DIRECTORY "${_darwin_dir}")
      set(_darwin_base "${_darwin_dir}/libleaf.dylib")
      set(_darwin_parent "${_darwin_dir}/libparent.dylib")
      set(_darwin_system "${_darwin_fixture_dir}/callback-libsystem-${_darwin_kind}.tbd")
      add_custom_command(OUTPUT "${_darwin_base}"
        COMMAND "${NEVERD_PROCESS_LD64}" -arch "${_darwin_arch}"
          -platform_version "${_darwin_profile}" 14.0 14.0 ${_darwin_flag}
          -dylib -install_name "@loader_path/libleaf.dylib"
          -current_version 1.2 -compatibility_version 1.0
          "${_darwin_guard_prefix}-leaf.o" "${_darwin_system}" "${_darwin_guard_cpp}" -o "${_darwin_base}"
        DEPENDS "${_darwin_guard_prefix}-leaf.o" "${_darwin_system}" "${_darwin_guard_cpp}" VERBATIM)
      add_custom_command(OUTPUT "${_darwin_parent}"
        COMMAND "${NEVERD_PROCESS_LD64}" -arch "${_darwin_arch}"
          -platform_version "${_darwin_profile}" 14.0 14.0 ${_darwin_flag}
          -dylib -install_name "@loader_path/libparent.dylib"
          -current_version 1.2 -compatibility_version 1.0
          "${_darwin_guard_prefix}-parent.o" "${_darwin_base}" "${_darwin_system}" "${_darwin_guard_cpp}"
          -o "${_darwin_parent}"
        DEPENDS "${_darwin_guard_prefix}-parent.o" "${_darwin_base}"
          "${_darwin_system}" "${_darwin_guard_cpp}" VERBATIM)
      add_custom_command(OUTPUT "${_darwin_dir}/main"
        COMMAND "${NEVERD_PROCESS_LD64}" -arch "${_darwin_arch}"
          -platform_version "${_darwin_profile}" 14.0 14.0 ${_darwin_flag} -e _main
          "${_darwin_guard_prefix}-main.o" "${_darwin_parent}" "${_darwin_base}"
          "${_darwin_system}" "${_darwin_guard_cpp}" -o "${_darwin_dir}/main"
        DEPENDS "${_darwin_guard_prefix}-main.o" "${_darwin_base}"
          "${_darwin_parent}" "${_darwin_system}" "${_darwin_guard_cpp}" VERBATIM)
      list(APPEND _darwin_guard_outputs "${_darwin_dir}/main")
    endforeach()
  endforeach()
endforeach()
add_custom_target(NeverDDarwinGuardFixtures DEPENDS ${_darwin_guard_outputs})
add_dependencies(NeverDDarwinProcessTests NeverDDarwinGuardFixtures)
if(TARGET NeverDProcessPublicTests)
  add_dependencies(NeverDProcessPublicTests NeverDDarwinGuardFixtures)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_CROSSCOMPILING)
  set(_darwin_guard_native_outputs)
  set(_darwin_native_prefix "${_darwin_fixture_dir}/macos-${_darwin_native_arch}-guard")
  foreach(_darwin_kind classic chained)
    if(_darwin_kind STREQUAL "classic")
      set(_darwin_flag -Wl,-no_fixup_chains)
    else()
      set(_darwin_flag -Wl,-fixup_chains)
    endif()
    set(_darwin_dir "${_darwin_fixture_dir}/macos-guard-native-${_darwin_kind}")
    file(MAKE_DIRECTORY "${_darwin_dir}")
    set(_darwin_base "${_darwin_dir}/libleaf.dylib")
    set(_darwin_parent "${_darwin_dir}/libparent.dylib")
    add_custom_command(OUTPUT "${_darwin_base}"
      COMMAND xcrun clang++ -arch "${_darwin_native_arch}" ${_darwin_flag}
        -dynamiclib -Wl,-install_name,@loader_path/libleaf.dylib
        "${_darwin_native_prefix}-leaf.o" -o "${_darwin_base}"
      DEPENDS "${_darwin_native_prefix}-leaf.o" VERBATIM)
    add_custom_command(OUTPUT "${_darwin_parent}"
      COMMAND xcrun clang++ -arch "${_darwin_native_arch}" ${_darwin_flag}
        -dynamiclib -Wl,-install_name,@loader_path/libparent.dylib
        "${_darwin_native_prefix}-parent.o" "${_darwin_base}" -o "${_darwin_parent}"
      DEPENDS "${_darwin_native_prefix}-parent.o" "${_darwin_base}" VERBATIM)
    add_custom_command(OUTPUT "${_darwin_dir}/main"
      COMMAND xcrun clang++ -arch "${_darwin_native_arch}" ${_darwin_flag}
        "${_darwin_native_prefix}-main.o" "${_darwin_parent}" "${_darwin_base}"
        -o "${_darwin_dir}/main"
      DEPENDS "${_darwin_native_prefix}-main.o" "${_darwin_parent}"
        "${_darwin_base}" VERBATIM)
    list(APPEND _darwin_guard_native_outputs "${_darwin_dir}/main")
    string(TOUPPER "${_darwin_kind}" _darwin_guard_macro)
    target_compile_definitions(NeverDDarwinProcessTests PRIVATE
      "NEVERD_DARWIN_GUARD_NATIVE_${_darwin_guard_macro}=\"${_darwin_dir}/main\"")
  endforeach()
  add_custom_target(NeverDDarwinGuardNativeOracle DEPENDS ${_darwin_guard_native_outputs})
  add_dependencies(NeverDDarwinProcessTests NeverDDarwinGuardNativeOracle)
endif()
