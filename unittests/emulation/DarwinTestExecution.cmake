# Apply after PRE_TEST discovery. These semantic cases repeat whole guest
# workloads; their aggregate host guard must cover every run in the batch.
foreach(_neverd_darwin_test IN LISTS NeverDDarwinProcessTests_TESTS)
  if(_neverd_darwin_test MATCHES
      "^Transports/DarwinProcess\\.ImmediateSelectPreservesBitmapsCopiesAndProviderPremises/")
    set_tests_properties("${_neverd_darwin_test}" PROPERTIES TIMEOUT 120)
  elseif(_neverd_darwin_test MATCHES
      "^Transports/DarwinProcess\\.SymbolicFileSystemsKeepOwnSnapshotAndFollowedTargetSeparate/")
    set_tests_properties("${_neverd_darwin_test}" PROPERTIES TIMEOUT 240)
  elseif(_neverd_darwin_test MATCHES
      "^Transports/DarwinProcess\\.CreatedDirectoryFileSystemsKeepActualParentAndHeldLifetime/")
    # Eight separately bounded twenty-second guests, plus host setup/teardown.
    set_tests_properties("${_neverd_darwin_test}" PROPERTIES TIMEOUT 180)
  elseif(_neverd_darwin_test MATCHES
      "^Transports/DarwinProcess\\.CreatedRegularAttributesKeepIndependentProvidersAndObjectState/")
    # Twenty separately bounded twenty-second guests, plus host setup/teardown.
    set_tests_properties("${_neverd_darwin_test}" PROPERTIES TIMEOUT 420)
  endif()
endforeach()
unset(_neverd_darwin_test)
