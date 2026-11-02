add_neverd_tool(neverd-bench
  neverd-bench.cpp

  LINK_LIBS
  neverd_shared
)

add_neverd_tool(neverd-signature-bench
  signature-bench.cpp
  LINK_COMPONENTS Sigs)

add_neverd_tool(neverd-signature-context-bench
  signature-context-bench.cpp
  LINK_COMPONENTS Sigs)

add_neverd_tool(neverd-med-bench
  med-bench.cpp
  LINK_COMPONENTS Pipeline)

if(NEVERD_ENABLE_CPU_EMULATION)
  add_neverd_tool(neverd-cpu-bench
    cpu-bench.cpp
    LINK_COMPONENTS EmulationCPU)
  neverd_sign_hypervisor(neverd-cpu-bench)
  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND
     CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$" AND
     NEVERD_EMULATION_BACKEND_HVF)
    # Opt-in diagnostic library; never linked into timing executables.
    add_library(neverd-hvf-entry-counter SHARED EXCLUDE_FROM_ALL
      hvf-entry-counter.cpp)
    target_compile_features(neverd-hvf-entry-counter PRIVATE cxx_std_20)
    target_link_libraries(neverd-hvf-entry-counter PRIVATE "-framework Hypervisor")
  endif()
endif()

option(NEVERD_BUILD_SOLVER_BENCH "Build the solver comparison and query export tool" OFF)
if(NEVERD_BUILD_SOLVER_BENCH)
  add_neverd_tool(neverd-solver-bench
    solver-bench.cpp
    LINK_COMPONENTS Solver Symbolic
  )
  if(NEVERD_ENABLE_Z3)
    add_custom_command(TARGET neverd-solver-bench POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:neverd-solver-bench>/licenses/z3"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/LICENSES/Z3.txt"
        "$<TARGET_FILE_DIR:neverd-solver-bench>/licenses/z3/LICENSE.txt"
      VERBATIM)
  endif()
endif()
