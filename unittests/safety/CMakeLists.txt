add_neverd_unittest(NeverDSafetyTests
  CallEffectsTests.cpp
  SinkCatalogTests.cpp
  SinkScannerTests.cpp
  ArgSlicerTests.cpp
  ObjectModelTests.cpp
  HuntEngineTests.cpp
  AllocLifetimeTests.cpp
  CallAbiSafetyTests.cpp
  InterprocReachabilityTests.cpp
  SafetyReportTests.cpp
  ProcessReplayTests.cpp
  ProcessReplayExecutorTests.cpp
  NativeProcessReplayAdapterTests.cpp
  SanitizerPublicationMetadataTests.cpp
  SanitizerPublicationExecutorTests.cpp
  RuntimeSanitizerTests.cpp
  RuntimeBoundsSanitizerTests.cpp

  LINK_COMPONENTS Safety PassIR Lift LibC
)

target_include_directories(NeverDSafetyTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/lib/safety
)

# Keeps the independent process-replay-v1 plan-candidate validator runnable
# without implying that a process runner or execution attestation exists.
add_neverd_unittest(NeverDProcessReplayTests
  ProcessReplayTests.cpp

  LINK_COMPONENTS Safety
)

target_include_directories(NeverDProcessReplayTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)

# Keeps the platform-neutral exact replay coordinator independently runnable
# from native process adapters, which must prove confinement and attestation.
add_neverd_unittest(NeverDProcessReplayExecutorTests
  ProcessReplayExecutorTests.cpp

  LINK_COMPONENTS Safety
)

target_include_directories(NeverDProcessReplayExecutorTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)

# Publishes only a fail-closed native availability/factory boundary until one
# host can prove every process-replay-exec-v1 capability atomically.
add_neverd_unittest(NeverDNativeProcessReplayAdapterTests
  NativeProcessReplayAdapterTests.cpp

  LINK_COMPONENTS Safety
)

target_include_directories(NeverDNativeProcessReplayAdapterTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)

# Keeps the pure security-metadata-plan-v1 policy independently runnable from
# filesystem observation and publication executors.
add_neverd_unittest(NeverDSanitizerPublicationMetadataTests
  SanitizerPublicationMetadataTests.cpp

  LINK_COMPONENTS Safety
)

target_include_directories(NeverDSanitizerPublicationMetadataTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)

# Keeps the platform-neutral metadata transaction coordinator independently
# runnable from native filesystem adapters.
add_neverd_unittest(NeverDSanitizerPublicationExecutorTests
  SanitizerPublicationExecutorTests.cpp

  LINK_COMPONENTS Safety
)

target_include_directories(NeverDSanitizerPublicationExecutorTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)

# Exercises the portable fault seam on every host and the held-fd/openat/
# renameatx_np implementation against a real temporary directory on Darwin.
add_neverd_unittest(NeverDDarwinSanitizerPublicationAdapterTests
  DarwinSanitizerPublicationAdapterTests.cpp

  LINK_COMPONENTS Safety
)

target_include_directories(NeverDDarwinSanitizerPublicationAdapterTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)

# Keeps the strict finding-to-emitted-callsite sanitizer planner independently
# runnable while the end-to-end runtime hardening pipeline evolves.
add_neverd_unittest(NeverDRuntimeSanitizerTests
  RuntimeSanitizerTests.cpp

  LINK_COMPONENTS Safety Lift LibC
)

target_include_directories(NeverDRuntimeSanitizerTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/lib/safety
)

# Keeps transactional LLVM guard insertion independently runnable from the
# aggregate safety suite during RED/GREEN iterations.
add_neverd_unittest(NeverDRuntimeBoundsSanitizerTests
  RuntimeBoundsSanitizerTests.cpp

  LINK_COMPONENTS Safety PassIR Lift LibC
)

target_include_directories(NeverDRuntimeBoundsSanitizerTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/lib/safety
  ${LLVM_INCLUDE_DIRS}
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
    SafetyCLIContractTests.cpp
    RuntimeSanitizerCLIContractTests.cpp
    SafetyIntegrationTests.cpp
    LINK_LIBS neverd_shared
    TIMEOUT 300
  )
  target_include_directories(NeverDSafetyIntegrationTests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tools/neverd/cmd)
  target_link_libraries(NeverDSafetyIntegrationTests PRIVATE LLVMSupport)
  target_compile_definitions(NeverDSafetyIntegrationTests PRIVATE
    SAFETY_FIXTURE_ROOT="${_safety_fixture_root}"
    NEVERD_BINARY="$<TARGET_FILE:neverd>"
    NEVERD_RUNTIME_FIXTURE_COMPILER="${NEVERD_HOST_RUNTIME_FIXTURE_COMPILER_EXECUTABLE}")
  if(TARGET neverd)
    add_dependencies(NeverDSafetyIntegrationTests neverd)
  endif()
endif()
