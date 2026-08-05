cmake_minimum_required(VERSION 3.20)
project(NeverD VERSION 0.1.0 LANGUAGES C CXX)

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
        core support irreader bitwriter bitreader linker
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
        LLVMCore LLVMSupport LLVMIRReader LLVMBitWriter LLVMBitReader LLVMLinker
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

# Capstone's decode DFA is the single hottest dependency in the front end:
# function detection and CFG construction spend the bulk of their time inside
# it.  It is self-contained third-party code we never step through, so build it
# fully optimized irrespective of the application's own build type.  A Release
# build already compiles it at -O3 -DNDEBUG, but a Debug tree would otherwise
# get -O0 capstone (~3x slower decode) and a RelWithDebInfo tree only -O2 — both
# used for the day-to-day and profiling runs where decode throughput still
# matters.  Force -O3 + NDEBUG (the latter also strips capstone's internal
# asserts) on the capstone OBJECT target so the speed-up flows through both the
# static and shared capstone libraries.  The per-target options are appended
# after the global build-type flags, so they win on the optimization level.
if(TARGET capstone)
    if(MSVC)
        target_compile_options(capstone PRIVATE /O2)
    else()
        target_compile_options(capstone PRIVATE -O3 -fomit-frame-pointer)
    endif()
    target_compile_definitions(capstone PRIVATE NDEBUG)
endif()

# --- NeverD CMake helpers (mirrors LLVM's AddLLVM.cmake) ---
include(${CMAKE_SOURCE_DIR}/cmake/AddNeverD.cmake)

# --- NeverD component libraries ---
add_subdirectory(lib)

# --- libneverd shared library (core engine, C API) ---
# CLI, GUI, and plugins all link this shared library.
option(NEVERD_BUILD_SHARED "Build libneverd shared library" ON)
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
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(unittests)
endif()
