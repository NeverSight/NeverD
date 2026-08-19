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

add_subdirectory(symbolic)
add_subdirectory(solver)
add_subdirectory(sigs)
add_subdirectory(debug)
add_subdirectory(libc)
add_subdirectory(lift)
add_subdirectory(translate)
add_subdirectory(semantic)
add_subdirectory(evm)
add_subdirectory(sbf)
add_subdirectory(plugin)

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
get_property(_neverd_sbf_test_targets GLOBAL PROPERTY NEVERD_SBF_TEST_TARGETS)
if(NOT _neverd_sbf_test_targets)
  message(FATAL_ERROR "No NeverDSBF test targets were registered")
endif()
add_custom_target(check-neverd-sbf
  COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR}
          -L ^NeverDSBF
          --output-on-failure --progress -j ${_neverd_ncpu} -C $<CONFIG>
  DEPENDS ${_neverd_sbf_test_targets} ${_neverd_run_deps}
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

if(TARGET NeverDWindowsEHCorpusTests)
  add_custom_target(check-neverd-windows-eh-corpus
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR}
            -L ^NeverDWindowsEHCorpusTests$
            --output-on-failure --progress -j ${_neverd_ncpu} -C $<CONFIG>
    DEPENDS NeverDWindowsEHCorpusTests
    USES_TERMINAL
    COMMENT "Running the pinned Windows multi-toolchain exception corpus")
endif()

if(TARGET NeverDRustEHCorpusTests)
  add_custom_target(check-neverd-rust-eh-corpus
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR}
            -L ^NeverDRustEHCorpusTests$
            --output-on-failure --progress -j ${_neverd_ncpu} -C $<CONFIG>
    DEPENDS NeverDRustEHCorpusTests
    USES_TERMINAL
    COMMENT "Running the pinned Rust unwinding and panic corpus")
endif()

if(TARGET NeverDGoEHCorpusTests)
  add_custom_target(check-neverd-go-eh-corpus
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR}
            -L ^NeverDGoEHCorpusTests$
            --output-on-failure --progress -j ${_neverd_ncpu} -C $<CONFIG>
    DEPENDS NeverDGoEHCorpusTests
    USES_TERMINAL
    COMMENT "Running the pinned Go runtime-metadata corpus")
endif()

if(TARGET NeverDCxxItaniumEHCorpusTests)
  add_custom_target(check-neverd-cxx-itanium-eh-corpus
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR}
            -L ^NeverDCxxItaniumEHCorpusTests$
            --output-on-failure --progress -j ${_neverd_ncpu} -C $<CONFIG>
    DEPENDS NeverDCxxItaniumEHCorpusTests
    USES_TERMINAL
    COMMENT "Running the pinned C++ Itanium and ARM EHABI corpus")
endif()

if(TARGET NeverDAdaDEHCorpusTests)
  add_custom_target(check-neverd-ada-d-eh-corpus
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR}
            -L ^NeverDAdaDEHCorpusTests$
            --output-on-failure --progress -j ${_neverd_ncpu} -C $<CONFIG>
    DEPENDS NeverDAdaDEHCorpusTests
    USES_TERMINAL
    COMMENT "Running the pinned Ada and D Itanium exception corpus")
endif()

if(TARGET NeverDObjCEHCorpusTests)
  add_custom_target(check-neverd-objc-eh-corpus
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR}
            -L ^NeverDObjCEHCorpusTests$
            --output-on-failure --progress -j ${_neverd_ncpu} -C $<CONFIG>
    DEPENDS NeverDObjCEHCorpusTests
    USES_TERMINAL
    COMMENT "Running the pinned Objective-C runtime exception corpus")
endif()

# One target for every line at once, because what the corpus is for is the
# claim that no line regressed while another was being worked on.
set(_neverd_corpus_targets)
foreach(_line windows-eh rust-eh go-eh cxx-itanium-eh objc-eh ada-d-eh)
  if(TARGET check-neverd-${_line}-corpus)
    list(APPEND _neverd_corpus_targets check-neverd-${_line}-corpus)
  endif()
endforeach()
if(_neverd_corpus_targets)
  add_custom_target(check-neverd-corpus DEPENDS ${_neverd_corpus_targets})
endif()

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
