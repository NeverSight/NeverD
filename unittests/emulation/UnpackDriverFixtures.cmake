set(_unpack_driver_image "${_driver_fixture_dir}/unpack-driver.sys")
set(_unpack_driver_object "${_driver_fixture_dir}/unpack-driver.obj")
add_custom_command(OUTPUT "${_unpack_driver_image}" "${_unpack_driver_object}"
  COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}"
    --target=x86_64-pc-windows-msvc -std=c11 -ffreestanding -fno-builtin
    -fno-stack-protector -O1 -c
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/driver_unpack.c"
    -o "${_unpack_driver_object}"
  COMMAND "${NEVERD_DRIVER_LLD_LINK}" /machine:x64 /entry:DriverEntry
    /subsystem:native /driver /nodefaultlib /fixed:no /dynamicbase
    /base:0x180000000
    /noimplib /timestamp:0 /release /section:.prog,ERW /merge:.data=.drvstat
    "/out:${_unpack_driver_image}" "${_unpack_driver_object}"
    "${_driver_import_lib}"
  DEPENDS fixtures/driver_unpack.c fixtures/driver_io.c "${_driver_import_lib}"
  VERBATIM)
# Use the full eight-byte state-section name: zero-padded ".data" followed by
# a 0x70-byte BSS extent spells an unaligned modeled object address in the PE
# header. The positive fixture must not trigger that conservative state guard.
add_custom_target(NeverDUnpackDriverFixtures DEPENDS "${_unpack_driver_image}")
add_neverd_unittest(NeverDUnpackDriverTests
  UnpackDriverTests.cpp
  TIMEOUT 120
  LINK_COMPONENTS Unpack Emulation)
target_include_directories(NeverDUnpackDriverTests PRIVATE
  "${CMAKE_SOURCE_DIR}/unittests/unpack")
target_compile_definitions(NeverDUnpackDriverTests PRIVATE
  NEVERD_UNPACK_DRIVER_FIXTURE="${_unpack_driver_image}"
  NEVERD_UNPACK_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/unittests/unpack/fixtures/binaries")
add_dependencies(NeverDUnpackDriverTests NeverDUnpackDriverFixtures)
if(TARGET neverd AND TARGET neverd_shared)
  target_link_libraries(NeverDUnpackDriverTests PRIVATE neverd_shared)
  target_compile_definitions(NeverDUnpackDriverTests PRIVATE
    NEVERD_UNPACK_DRIVER_CLI="$<TARGET_FILE:neverd>")
  add_dependencies(NeverDUnpackDriverTests neverd)
endif()
if(WIN32)
  target_compile_definitions(NeverDUnpackDriverTests PRIVATE NOMINMAX)
endif()
