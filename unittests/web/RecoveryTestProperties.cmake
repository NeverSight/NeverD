# One optional whole-product qualification parses and reparses 2,345 modules.
# Keep ordinary unit-test deadlines and per-parser budgets unchanged.
list(FIND NeverDWebSourceTests_TESTS
  WebSourceRecovery.ClaudeCode21296AllJavaScriptWhenSupplied
  _neverd_recovery_qualification_index)
if(NOT _neverd_recovery_qualification_index EQUAL -1)
  set_tests_properties(WebSourceRecovery.ClaudeCode21296AllJavaScriptWhenSupplied
    PROPERTIES TIMEOUT 600)
endif()
unset(_neverd_recovery_qualification_index)
