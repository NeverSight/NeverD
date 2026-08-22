add_subdirectory(ir)
add_subdirectory(evm)
add_subdirectory(loader)
add_subdirectory(sbf)
add_subdirectory(decode)
add_subdirectory(lift)
add_subdirectory(translate)
add_subdirectory(backend)
add_subdirectory(pipeline)
add_subdirectory(pass)
add_subdirectory(debug)
add_subdirectory(libc)

# Target register metadata delegates architecture-specific names to the
# lifters, while the MedIR ABI pass queries the libc catalog.  Keep those
# archive dependencies explicit so one-pass static linkers rescan them after
# their consumers.
target_link_libraries(NeverDIR PUBLIC NeverDLift)
target_link_libraries(NeverDIRMed PUBLIC NeverDLibC)

add_subdirectory(support)
add_subdirectory(symbolic)
add_subdirectory(solver)
add_subdirectory(safety)
add_subdirectory(sigs)
