add_neverd_unittest(NeverDSafetyTests
  SinkCatalogTests.cpp
  SinkScannerTests.cpp
  ArgSlicerTests.cpp
  ObjectModelTests.cpp
  HuntEngineTests.cpp
  AllocLifetimeTests.cpp
  CallAbiSafetyTests.cpp
  SafetyReportTests.cpp

  LINK_COMPONENTS Safety Lift LibC
)

target_include_directories(NeverDSafetyTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)

# End-to-end coverage over checked-in PE, ELF, and Mach-O binaries for both
# supported native architectures.  These inputs are mandatory on every host;
# the fixture Makefile contains the rebuild recipes.
if(TARGET neverd_shared)
  set(_safety_fixture_root
      ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/binaries)
  set(_safety_fixture_files
      safety_cases_elf_x64
      safety_cases_elf_x64.map
      safety_cases_elf_arm64
      safety_cases_elf_arm64.map
      safety_cases_macho_x64
      safety_cases_macho_x64.map
      safety_cases_macho_x64.dSYM/Contents/Resources/DWARF/safety_cases_macho_x64
      safety_cases_macho_arm64
      safety_cases_macho_arm64.map
      safety_cases_macho_arm64.dSYM/Contents/Resources/DWARF/safety_cases_macho_arm64
      safety_cases_pe_x64.exe
      safety_cases_pe_x64.pdb
      safety_cases_pe_x64.exe.map
      safety_cases_pe_arm64.exe
      safety_cases_pe_arm64.pdb
      safety_cases_pe_arm64.exe.map)
  foreach(_fixture IN LISTS _safety_fixture_files)
    if(NOT EXISTS "${_safety_fixture_root}/${_fixture}")
      message(FATAL_ERROR "Required safety fixture is missing: ${_fixture}")
    endif()
  endforeach()

  add_neverd_unittest(NeverDSafetyIntegrationTests
    SafetyIntegrationTests.cpp
    LINK_LIBS neverd_shared
    TIMEOUT 300
  )
  target_include_directories(NeverDSafetyIntegrationTests PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(NeverDSafetyIntegrationTests PRIVATE LLVMSupport)
  target_compile_definitions(NeverDSafetyIntegrationTests PRIVATE
    SAFETY_FIXTURE_ROOT="${_safety_fixture_root}")
endif()
