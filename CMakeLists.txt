cmake_minimum_required(VERSION 3.20)
project(NeverD VERSION 3389.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Unified output directory — CLI, GUI, libneverd all go to build/bin/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- LLVM ---
# Backends: X86 (i386+x86_64), ARM (arm32), AArch64 (arm64). See docs/targets.md.
# Two ways to get LLVM (both expose the same LLVM_LIBS to the rest of the build):
#   default                : build our LLVM fork locally from the
#                            third_party/llvm-project submodule (integrated build;
#                            first compile is slow, later builds are incremental).
#   NEVERD_LLVM_PREBUILT=ON : download the CI/CD-built NeverD LLVM release package
#                            and use it via find_package (no local LLVM compile).
#                            Switch to this once the fork is stable.
option(NEVERD_LLVM_PREBUILT "Use the CI/CD prebuilt NeverD LLVM release instead of building the fork locally" OFF)

if(NEVERD_LLVM_PREBUILT)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/NeverDLLVMPrebuilt.cmake)
    neverd_fetch_prebuilt_llvm()
    find_package(LLVM REQUIRED CONFIG)
    message(STATUS "Using prebuilt NeverD LLVM ${LLVM_PACKAGE_VERSION} (${LLVM_DIR})")
    llvm_map_components_to_libnames(LLVM_LIBS
        core support demangle irreader bitwriter bitreader linker
        analysis passes target
        x86codegen x86asmparser x86desc x86info
        armcodegen armasmparser armdesc arminfo
        aarch64codegen aarch64asmparser aarch64desc aarch64info
        debuginfodwarf debuginfopdb debuginfocodeview object
    )
    separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
    add_definitions(${LLVM_DEFINITIONS_LIST})
else()
    message(STATUS "Building LLVM fork locally from third_party/llvm-project (integrated build)")
    # X86 = i386 + x86_64; ARM = 32-bit ARM (separate from AArch64 arm64)
    set(LLVM_TARGETS_TO_BUILD "X86;ARM;AArch64" CACHE STRING "" FORCE)
    set(LLVM_ENABLE_PROJECTS  ""            CACHE STRING "" FORCE)
    set(LLVM_BUILD_TOOLS       OFF CACHE BOOL "" FORCE)
    set(LLVM_BUILD_UTILS       OFF CACHE BOOL "" FORCE)
    set(LLVM_BUILD_TESTS       OFF CACHE BOOL "" FORCE)
    set(LLVM_BUILD_EXAMPLES    OFF CACHE BOOL "" FORCE)
    set(LLVM_BUILD_DOCS        OFF CACHE BOOL "" FORCE)
    set(LLVM_BUILD_BENCHMARKS  OFF CACHE BOOL "" FORCE)
    set(LLVM_ENABLE_BINDINGS   OFF CACHE BOOL "" FORCE)
    set(LLVM_INCLUDE_TESTS     OFF CACHE BOOL "" FORCE)
    set(LLVM_INCLUDE_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(LLVM_INCLUDE_EXAMPLES  OFF CACHE BOOL "" FORCE)
    set(LLVM_INCLUDE_DOCS      OFF CACHE BOOL "" FORCE)
    set(LLVM_INCLUDE_TOOLS     OFF CACHE BOOL "" FORCE)
    set(LLVM_INCLUDE_UTILS     OFF CACHE BOOL "" FORCE)
    set(LLVM_ENABLE_RTTI        ON  CACHE BOOL "" FORCE)
    set(LLVM_BUILD_LLVM_DYLIB  OFF CACHE BOOL "" FORCE)
    set(LLVM_LINK_LLVM_DYLIB   OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS       OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/llvm-project/llvm EXCLUDE_FROM_ALL)

    set(LLVM_LIBS
        LLVMCore LLVMSupport LLVMDemangle LLVMIRReader LLVMBitWriter
        LLVMBitReader LLVMLinker
        LLVMAnalysis LLVMPasses LLVMTarget
        LLVMX86CodeGen LLVMX86AsmParser LLVMX86Desc LLVMX86Info
        LLVMARMCodeGen LLVMARMAsmParser LLVMARMDesc LLVMARMInfo
        LLVMAArch64CodeGen LLVMAArch64AsmParser LLVMAArch64Desc LLVMAArch64Info
        LLVMDebugInfoDWARF LLVMDebugInfoPDB LLVMDebugInfoCodeView LLVMObject
    )
    set(LLVM_INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/llvm-project/llvm/include
        ${CMAKE_CURRENT_BINARY_DIR}/third_party/llvm-project/llvm/include
    )
endif()

# ORC is deliberately opt-in for the differential JIT tests. Keep it out of
# NEVERD_COMMON_LIBS so production components do not inherit a JIT dependency.
if(NEVERD_LLVM_PREBUILT)
    llvm_map_components_to_libnames(NEVERD_LLVM_JIT_LIBS orcjit)
else()
    set(NEVERD_LLVM_JIT_LIBS LLVMOrcJIT)
endif()

# --- Capstone (our fork, built from submodule) ---
set(CAPSTONE_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_STATIC_LIBS ON  CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_CSTOOL      OFF CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_LEGACY_TESTS OFF CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_CSTEST      OFF CACHE BOOL "" FORCE)
set(CAPSTONE_ARCHITECTURE_DEFAULT OFF CACHE BOOL "" FORCE)
set(CAPSTONE_X86_SUPPORT       ON  CACHE BOOL "" FORCE)
set(CAPSTONE_ARM_SUPPORT        ON  CACHE BOOL "" FORCE)
set(CAPSTONE_AARCH64_SUPPORT    ON  CACHE BOOL "" FORCE)
add_subdirectory(third_party/capstone)

# Capstone's decode DFA is the single hottest dependency in the front end, so
# keep it explicitly optimized in non-Debug configurations.  This preserves
# representative decode throughput in Release and RelWithDebInfo builds while
# allowing Debug to honor its source-level debugging contract: no forced
# optimization and Capstone's internal assertions enabled.  In particular,
# MSVC's Debug runtime checks are incompatible with /O2.  The options apply to
# the capstone OBJECT target so they flow through its static and shared
# libraries.  Use RelWithDebInfo when both symbols and decode performance
# matter.
if(TARGET capstone)
    if(MSVC)
        target_compile_options(capstone PRIVATE
            "$<$<NOT:$<CONFIG:Debug>>:/O2>")
    else()
        target_compile_options(capstone PRIVATE
            "$<$<NOT:$<CONFIG:Debug>>:-O3;-fomit-frame-pointer>")
    endif()
    target_compile_definitions(capstone PRIVATE
        "$<$<NOT:$<CONFIG:Debug>>:NDEBUG>")
endif()

# --- NeverD CMake helpers (mirrors LLVM's AddLLVM.cmake) ---
include(${CMAKE_SOURCE_DIR}/cmake/AddNeverD.cmake)

# --- NeverD component libraries ---
add_subdirectory(lib)

# --- libneverd shared library (core engine, C API) ---
# CLI, GUI, and plugins all link this shared library.
option(NEVERD_BUILD_SHARED "Build libneverd shared library" ON)
option(NEVERD_ENABLE_PYTHON_PLUGINS
       "Enable embedded CPython support for NeverD plugins" ON)

set(NEVERD_PYTHON_PLUGIN_HOST_ENABLED OFF)
if(NEVERD_ENABLE_PYTHON_PLUGINS AND NEVERD_BUILD_SHARED)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter Development.Embed)
    set(NEVERD_PYTHON_PLUGIN_HOST_ENABLED ON)
elseif(NEVERD_ENABLE_PYTHON_PLUGINS)
    message(STATUS
        "Python plugin host omitted because NEVERD_BUILD_SHARED is OFF")
endif()

if(NEVERD_BUILD_SHARED)
    add_subdirectory(lib/sdk)
endif()

# --- Tools ---
add_subdirectory(tools)


# --- Example plugins ---
option(NEVERD_BUILD_PLUGINS "Build NeverD example plugins" OFF)
if(NEVERD_BUILD_PLUGINS)
  add_subdirectory(plugins/example)
endif()

# --- Copy signature files to build/bin/sig/ ---
# Auto-detection expects sig/ alongside the executable.
add_custom_target(copy_sigs ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/bin/signatures
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/signatures
        ${CMAKE_BINARY_DIR}/bin/signatures
    COMMENT "Copying signature files to build/bin/signatures/"
)

# --- Unit tests ---
option(NEVERD_ENABLE_BINARY_CORPUS_TESTS
       "Enable integration tests backed by the pinned testbins corpus" OFF)
set(NEVERD_BINARY_CORPUS_ROOT
    "${CMAKE_SOURCE_DIR}/unittests/corpus"
    CACHE PATH "Root of the NeverD binary test corpus")
# Each corpus line is produced by its own CI workflow and lands in the
# submodule independently, so a checkout that predates one line's first
# publication is a normal state rather than a broken one.  Probing the lines
# separately keeps the ones that have shipped under test instead of failing the
# configure for the ones that have not; only a checkout carrying no manifest at
# all is an error, because that is an uninitialized submodule.
set(NEVERD_HAVE_WINDOWS_EH_CORPUS OFF)
set(NEVERD_HAVE_RUST_EH_CORPUS OFF)
set(NEVERD_HAVE_GO_EH_CORPUS OFF)
set(NEVERD_HAVE_CXX_ITANIUM_EH_CORPUS OFF)
if(NEVERD_ENABLE_BINARY_CORPUS_TESTS)
    get_filename_component(NEVERD_BINARY_CORPUS_ROOT
        "${NEVERD_BINARY_CORPUS_ROOT}" ABSOLUTE)
    foreach(_corpus_line windows-eh rust-eh go-eh cxx-itanium-eh)
        string(REPLACE "-" "_" _corpus_slug "${_corpus_line}")
        string(TOUPPER "${_corpus_slug}" _corpus_slug)
        set(_corpus_manifest
            "${NEVERD_BINARY_CORPUS_ROOT}/manifests/${_corpus_line}.json")
        if(EXISTS "${_corpus_manifest}")
            set(NEVERD_HAVE_${_corpus_slug}_CORPUS ON)
        else()
            message(STATUS
                "Omitting the ${_corpus_line} corpus tests: this checkout of "
                "${NEVERD_BINARY_CORPUS_ROOT} carries no "
                "manifests/${_corpus_line}.json")
        endif()
    endforeach()
    if(NOT NEVERD_HAVE_WINDOWS_EH_CORPUS AND
       NOT NEVERD_HAVE_RUST_EH_CORPUS AND
       NOT NEVERD_HAVE_GO_EH_CORPUS AND
       NOT NEVERD_HAVE_CXX_ITANIUM_EH_CORPUS)
        message(FATAL_ERROR
            "NEVERD_ENABLE_BINARY_CORPUS_TESTS requires at least one manifest "
            "under ${NEVERD_BINARY_CORPUS_ROOT}/manifests; "
            "initialize the unittests/corpus submodule at a generated-corpus revision")
    endif()
endif()

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(unittests)
endif()
