add_neverd_unittest(NeverDSafetyTests
  SinkCatalogTests.cpp
  SinkScannerTests.cpp
  ArgSlicerTests.cpp
  ObjectModelTests.cpp
  HuntEngineTests.cpp
  AllocLifetimeTests.cpp

  LINK_COMPONENTS Safety Lift
)

target_include_directories(NeverDSafetyTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)

# End-to-end coverage over a real lifted binary.  The fixture is compiled for
# the host-native format so each CI platform exercises its own object format
# (Mach-O on macOS, ELF on Linux, PE on Windows).  Extra architectures are
# compiled here when the host compiler can emit them; recipes in
# fixtures/Makefile reproduce other targets by hand.
if(TARGET neverd_shared)
  set(_safety_fixture_src ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/safety_cases.c)

  # Compile and link in one shot on Unix: a separate .o → link step on Darwin
  # leaves empty DWARF in the image, and the object model then underestimates
  # stack buffers.  MSVC needs an add_executable so /Od /GS- /Oi- apply.
  if(MSVC)
    add_executable(neverd_safety_cases_native EXCLUDE_FROM_ALL
      fixtures/safety_cases.c)
    set_target_properties(neverd_safety_cases_native PROPERTIES
      FOLDER "NeverD/Tests"
      C_STANDARD 11
      C_STANDARD_REQUIRED ON)
    target_compile_options(neverd_safety_cases_native PRIVATE
      /nologo /Od /Z7 /GS- /Oi- /wd4996)
    target_compile_definitions(neverd_safety_cases_native PRIVATE
      _CRT_SECURE_NO_WARNINGS)
    # Default CRT startup pulls a large runtime the hunt would scan and the
    # LLVM emitter often cannot finish.  The fixture only needs its own
    # functions plus the IAT for the libc/ucrt names it calls.
    target_link_options(neverd_safety_cases_native PRIVATE
      /DEBUG /NODEFAULTLIB /ENTRY:main /SUBSYSTEM:CONSOLE)
    target_link_libraries(neverd_safety_cases_native PRIVATE
      ucrt vcruntime kernel32)
    set_property(TARGET neverd_safety_cases_native PROPERTY
      MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
    add_custom_target(neverd_safety_fixture DEPENDS neverd_safety_cases_native)
    set(_safety_fixture_native "$<TARGET_FILE:neverd_safety_cases_native>")
  else()
    set(_safety_fixture_bin ${CMAKE_CURRENT_BINARY_DIR}/safety_cases_native)
    add_custom_command(
      OUTPUT ${_safety_fixture_bin}
      COMMAND ${CMAKE_C_COMPILER} -O0 -g -fno-stack-protector -fno-builtin
              ${_safety_fixture_src} -o ${_safety_fixture_bin}
      DEPENDS ${_safety_fixture_src}
      COMMENT "Building host-native memory-safety fixture")
    add_custom_target(neverd_safety_fixture DEPENDS ${_safety_fixture_bin})
    set(_safety_fixture_native "${_safety_fixture_bin}")
  endif()

  set(_safety_extra "")
  macro(neverd_try_safety_fixture stem)
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/${stem}")
    execute_process(
      COMMAND ${ARGN} -O0 -g -fno-stack-protector -fno-builtin
              "${_safety_fixture_src}" -o "${_out}"
      RESULT_VARIABLE _rc
      OUTPUT_QUIET ERROR_QUIET)
    if(_rc EQUAL 0 AND EXISTS "${_out}")
      list(APPEND _safety_extra "${_out}")
    endif()
  endmacro()

  if(APPLE)
    if(NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
      neverd_try_safety_fixture(safety_cases_macho_x64
        ${CMAKE_C_COMPILER} -arch x86_64)
    endif()
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
      neverd_try_safety_fixture(safety_cases_macho_arm64
        ${CMAKE_C_COMPILER} -arch arm64)
    endif()
  endif()

  if(_safety_extra)
    string(JOIN "," _safety_extra_joined ${_safety_extra})
  else()
    set(_safety_extra_joined "")
  endif()

  add_neverd_unittest(NeverDSafetyIntegrationTests
    SafetyIntegrationTests.cpp
    LINK_LIBS neverd_shared
    TIMEOUT 300
  )
  add_dependencies(NeverDSafetyIntegrationTests neverd_safety_fixture)
  target_include_directories(NeverDSafetyIntegrationTests PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
  target_compile_definitions(NeverDSafetyIntegrationTests PRIVATE
    SAFETY_FIXTURE_NATIVE="${_safety_fixture_native}"
    SAFETY_FIXTURE_EXTRA="${_safety_extra_joined}")
endif()
