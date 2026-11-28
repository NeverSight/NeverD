foreach(_neverd_linux_test IN LISTS NeverDLinuxProcessTests_TESTS)
  if(_neverd_linux_test MATCHES
      "^Backends/LinuxProcess\\.SustainedGPUCommandsPreserveTimeAndContextOwnership/")
    # Both O0 and O2 run with their own 30-second guest deadline.
    set_tests_properties("${_neverd_linux_test}" PROPERTIES TIMEOUT 75)
  endif()
endforeach()
unset(_neverd_linux_test)
