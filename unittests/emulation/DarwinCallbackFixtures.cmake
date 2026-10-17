# Original, SDK-free startup dependency graph for all five Darwin profiles.
set(_darwin_callback_outputs)
foreach(_darwin_kind classic chained)
  set(_darwin_files_binder "")
  if(_darwin_kind STREQUAL "classic")
    set(_darwin_files_binder ", dyld_stub_binder")
  endif()
  configure_file(fixtures/darwin_callback_libsystem.tbd.in
    "${_darwin_fixture_dir}/callback-libsystem-${_darwin_kind}.tbd" @ONLY)
endforeach()
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
    set(_darwin_prefix "${_darwin_fixture_dir}/${_darwin_profile}-${_darwin_arch}-callbacks")
    foreach(_darwin_unit leaf middle main)
      if(_darwin_unit STREQUAL "main")
        set(_darwin_extension cpp)
        set(_darwin_language -std=c++17 -fno-exceptions -fno-rtti)
      else()
        set(_darwin_extension c)
        set(_darwin_language -std=c11)
      endif()
      add_custom_command(OUTPUT "${_darwin_prefix}-${_darwin_unit}.o"
        COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_darwin_target}"
          ${_darwin_language} -ffreestanding -fno-builtin -fno-stack-protector -nostdinc
          -fno-vectorize -fno-slp-vectorize -fno-unwind-tables
          -fno-asynchronous-unwind-tables -O1 -c
          "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/darwin_callback_${_darwin_unit}.${_darwin_extension}"
          -o "${_darwin_prefix}-${_darwin_unit}.o"
        DEPENDS "fixtures/darwin_callback_${_darwin_unit}.${_darwin_extension}"
          fixtures/darwin_callback_startup.h VERBATIM)
    endforeach()
    foreach(_darwin_kind classic chained)
      if(_darwin_kind STREQUAL "classic")
        set(_darwin_flag -no_fixup_chains)
      else()
        set(_darwin_flag -fixup_chains)
      endif()
      set(_darwin_dir "${_darwin_prefix}-${_darwin_kind}")
      file(MAKE_DIRECTORY "${_darwin_dir}")
      set(_darwin_base "${_darwin_dir}/libleaf.dylib")
      set(_darwin_leaf "${_darwin_dir}/libmiddle.dylib")
      set(_darwin_system "${_darwin_fixture_dir}/callback-libsystem-${_darwin_kind}.tbd")
      add_custom_command(OUTPUT "${_darwin_base}"
        COMMAND "${NEVERD_PROCESS_LD64}" -arch "${_darwin_arch}"
          -platform_version "${_darwin_profile}" 14.0 14.0 ${_darwin_flag}
          -dylib -install_name "@loader_path/libleaf.dylib"
          -current_version 1.2 -compatibility_version 1.0
          "${_darwin_prefix}-leaf.o" "${_darwin_system}"
          -o "${_darwin_base}"
        DEPENDS "${_darwin_prefix}-leaf.o"
          "${_darwin_system}" VERBATIM)
      add_custom_command(OUTPUT "${_darwin_leaf}"
        COMMAND "${NEVERD_PROCESS_LD64}" -arch "${_darwin_arch}"
          -platform_version "${_darwin_profile}" 14.0 14.0 ${_darwin_flag}
          -dylib -install_name "@loader_path/libmiddle.dylib"
          -current_version 1.2 -compatibility_version 1.0
          "${_darwin_prefix}-middle.o" "${_darwin_base}" "${_darwin_system}"
          -o "${_darwin_leaf}"
        DEPENDS "${_darwin_prefix}-middle.o" "${_darwin_base}" "${_darwin_system}"
        VERBATIM)
      add_custom_command(OUTPUT "${_darwin_dir}/main"
        COMMAND "${NEVERD_PROCESS_LD64}" -arch "${_darwin_arch}"
          -platform_version "${_darwin_profile}" 14.0 14.0 ${_darwin_flag} -e _main
          "${_darwin_prefix}-main.o" "${_darwin_leaf}" "${_darwin_base}"
          "${_darwin_system}" -o "${_darwin_dir}/main"
        DEPENDS "${_darwin_prefix}-main.o" "${_darwin_base}"
          "${_darwin_leaf}" "${_darwin_system}" VERBATIM)
      list(APPEND _darwin_callback_outputs "${_darwin_dir}/main")
    endforeach()
  endforeach()
endforeach()
add_custom_target(NeverDDarwinCallbackFixtures DEPENDS ${_darwin_callback_outputs})
add_dependencies(NeverDDarwinProcessTests NeverDDarwinCallbackFixtures)
if(TARGET NeverDProcessPublicTests)
  add_dependencies(NeverDProcessPublicTests NeverDDarwinCallbackFixtures)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_CROSSCOMPILING)
  set(_darwin_callback_native_outputs)
  set(_darwin_native_prefix "${_darwin_fixture_dir}/macos-${_darwin_native_arch}-callbacks")
  foreach(_darwin_kind classic chained)
    if(_darwin_kind STREQUAL "classic")
      set(_darwin_flag -Wl,-no_fixup_chains)
    else()
      set(_darwin_flag -Wl,-fixup_chains)
    endif()
    set(_darwin_dir "${_darwin_fixture_dir}/macos-callbacks-native-${_darwin_kind}")
    file(MAKE_DIRECTORY "${_darwin_dir}")
    set(_darwin_base "${_darwin_dir}/libleaf.dylib")
    set(_darwin_leaf "${_darwin_dir}/libmiddle.dylib")
    add_custom_command(OUTPUT "${_darwin_base}"
      COMMAND xcrun clang -arch "${_darwin_native_arch}" ${_darwin_flag}
        -dynamiclib -Wl,-install_name,@loader_path/libleaf.dylib
        "${_darwin_native_prefix}-leaf.o" -o "${_darwin_base}"
      DEPENDS "${_darwin_native_prefix}-leaf.o" VERBATIM)
    add_custom_command(OUTPUT "${_darwin_leaf}"
      COMMAND xcrun clang -arch "${_darwin_native_arch}" ${_darwin_flag}
        -dynamiclib -Wl,-install_name,@loader_path/libmiddle.dylib
        "${_darwin_native_prefix}-middle.o" "${_darwin_base}" -o "${_darwin_leaf}"
      DEPENDS "${_darwin_native_prefix}-middle.o" "${_darwin_base}" VERBATIM)
    add_custom_command(OUTPUT "${_darwin_dir}/main"
      COMMAND xcrun clang++ -arch "${_darwin_native_arch}" ${_darwin_flag} -nostdlib++
        "${_darwin_native_prefix}-main.o" "${_darwin_leaf}" "${_darwin_base}"
        -o "${_darwin_dir}/main"
      DEPENDS "${_darwin_native_prefix}-main.o" "${_darwin_leaf}" "${_darwin_base}"
      VERBATIM)
    list(APPEND _darwin_callback_native_outputs "${_darwin_dir}/main")
    string(TOUPPER "${_darwin_kind}" _darwin_callback_macro)
    target_compile_definitions(NeverDDarwinProcessTests PRIVATE
      "NEVERD_DARWIN_CALLBACK_NATIVE_${_darwin_callback_macro}=\"${_darwin_dir}/main\"")
  endforeach()
  add_custom_target(NeverDDarwinCallbackNativeOracle DEPENDS ${_darwin_callback_native_outputs})
  add_dependencies(NeverDDarwinProcessTests NeverDDarwinCallbackNativeOracle)
endif()
include(DarwinExitFixtures.cmake)
