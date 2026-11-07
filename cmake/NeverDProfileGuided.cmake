# Profile-guided optimization for Clang builds. Neither option changes what
# the build computes, only how the compiler lays out and inlines it.
#
# NEVERD_PROFILE_GENERATE instruments every target; running the instrumented
# binaries writes raw profiles where LLVM_PROFILE_FILE names them, which
# `llvm-profdata merge -output=<file>.profdata` combines. NEVERD_PROFILE_USE
# then optimizes a build of the same sources with that merged profile.
include_guard(GLOBAL)

option(NEVERD_PROFILE_GENERATE
  "Instrument the build to record an execution profile (Clang)" OFF)
set(NEVERD_PROFILE_USE "" CACHE FILEPATH
  "Optimize the build with this merged llvm-profdata profile (Clang)")

if(NOT NEVERD_PROFILE_GENERATE AND NOT NEVERD_PROFILE_USE)
  return()
endif()
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "NEVERD_PROFILE_GENERATE and NEVERD_PROFILE_USE need Clang")
endif()
if(NEVERD_PROFILE_GENERATE AND NEVERD_PROFILE_USE)
  message(FATAL_ERROR "NEVERD_PROFILE_GENERATE and NEVERD_PROFILE_USE exclude each other")
endif()
if(NEVERD_PROFILE_GENERATE)
  add_compile_options(-fprofile-generate)
  add_link_options(-fprofile-generate)
  return()
endif()
if(NOT EXISTS "${NEVERD_PROFILE_USE}")
  message(FATAL_ERROR "NEVERD_PROFILE_USE names no file: ${NEVERD_PROFILE_USE}")
endif()
# A profile recorded before a source change still optimizes the functions it
# saw; the others build as they would without one.
add_compile_options("-fprofile-use=${NEVERD_PROFILE_USE}"
  -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date
  -Wno-backend-plugin)
