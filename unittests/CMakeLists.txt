cmake_minimum_required(VERSION 3.20)

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.17.0
  FIND_PACKAGE_ARGS NAMES GTest
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
include(GoogleTest)

add_neverd_unittest(NeverDTestProcessTests
  TestProcessTests.cpp
)

add_subdirectory(libc)
add_subdirectory(lift)
add_subdirectory(semantic)
add_subdirectory(evm)
add_subdirectory(sbf)

# One-shot test runners (like NeverC's check-neverc): build deps, then invoke
# CTest.  Pure CMake/CTest — no shell scripts, so it works the same on
# Windows / Linux / macOS.  CTest prints native "N/total" progress and Ctrl+C
# stops the run cleanly.
include(ProcessorCount)
ProcessorCount(_neverd_ncpu)
if(_neverd_ncpu LESS 1)
  set(_neverd_ncpu 1)
endif()

set(_neverd_run_deps "")
if(TARGET neverd)
  list(APPEND _neverd_run_deps neverd)
endif()
if(TARGET neverd_shared)
  list(APPEND _neverd_run_deps neverd_shared)
endif()

# All tests (lift / libc / semantic).
add_custom_target(check-neverd
  COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR}
          --output-on-failure --progress -j ${_neverd_ncpu} -C $<CONFIG>
  DEPENDS NeverDUnitTests ${_neverd_run_deps}
  USES_TERMINAL
  COMMENT "Running all NeverD tests via CTest")

# Semantic roundtrip + rewrite only (ctest -L selects this binary's cases).
add_custom_target(check-neverd-semantic
  COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR}
          -L NeverDSemanticTests
          --output-on-failure --progress -j ${_neverd_ncpu} -C $<CONFIG>
  DEPENDS NeverDSemanticTests ${_neverd_run_deps}
  USES_TERMINAL
  COMMENT "Running NeverDSemanticTests via CTest")

# Solana SBF loader, ISA, analysis, LLVM, C/Rust, and public API coverage.
add_custom_target(check-neverd-sbf
  COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR}
          -L ^NeverDSBF
          --output-on-failure --progress -j ${_neverd_ncpu} -C $<CONFIG>
  DEPENDS NeverDSBFMetadataTests NeverDSBFLoaderTests NeverDSBFAnalyzerTests
          NeverDSBFSemanticTests NeverDSBFLLVMEmitterTests
          NeverDSBFEmitterTests NeverDSBFIntegrationTests ${_neverd_run_deps}
  USES_TERMINAL
  COMMENT "Running Solana SBF tests via CTest")

# Instruction-substitution (patch) equivalence only: verifies the --subst /
# neverd_patch_full transform preserves semantics across 4 ISAs x 3 formats.
add_custom_target(check-neverd-patch-full
  COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR}
          -L NeverDPatchFullTests
          --output-on-failure --progress -j ${_neverd_ncpu} -C $<CONFIG>
  DEPENDS NeverDPatchFullTests ${_neverd_run_deps}
  USES_TERMINAL
  COMMENT "Running NeverDPatchFullTests via CTest")

# Switch / jump-table transformation probing only: a small standalone binary so
# transformation + backend bugs can be hunted incrementally (ctest -L selects it).
add_custom_target(check-neverd-switch-xform
  COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR}
          -L NeverDSwitchXformTests
          --output-on-failure --progress -j ${_neverd_ncpu} -C $<CONFIG>
  DEPENDS NeverDSwitchXformTests ${_neverd_run_deps}
  USES_TERMINAL
  COMMENT "Running NeverDSwitchXformTests via CTest")

# CFG / loop structuring transformation probing only: standalone binary hunting
# lift->Med->High control-flow reconstruction + backend re-lowering bugs
# incrementally (ctest -L selects this binary's cases).
add_custom_target(check-neverd-cfgloop-xform
  COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR}
          -L NeverDCFGLoopXformTests
          --output-on-failure --progress -j ${_neverd_ncpu} -C $<CONFIG>
  DEPENDS NeverDCFGLoopXformTests ${_neverd_run_deps}
  USES_TERMINAL
  COMMENT "Running NeverDCFGLoopXformTests via CTest")

# Adjacent PIC jump-table probing only: standalone binary hunting the
# resolver's adjacent-unguarded-table run over-read incrementally.
add_custom_target(check-neverd-twotable-xform
  COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR}
          -L NeverDTwoTableXformTests
          --output-on-failure --progress -j ${_neverd_ncpu} -C $<CONFIG>
  DEPENDS NeverDTwoTableXformTests ${_neverd_run_deps}
  USES_TERMINAL
  COMMENT "Running NeverDTwoTableXformTests via CTest")
