# Plain class covariance

The abstract dependency-parent profile adds an unchanged SDK value declaration
and public/protected superclass methods returning that value. The input
forwarder returns an independently declared subclass and keeps both original
erased forwarding bridges. Neither input nor superclass borrows class formals.

Run `test_mobile_android_original_raw_parameters.py --dependency-interface
--abstract-dependency-parent --plain-class-covariance`. The immutable wrapper
checks full method metadata, exact public/protected bridge flags, nominal
result ancestry, mutable return aliases, typed and erased dispatch, independent
subclass overrides and effects before the original failure object. Original
runtime entries and metadata recompile unchanged; bridge source projections
remain disclosed as `partial` coverage.

Eight input classes and twenty-two original methods recompile with complete
metadata preserved. The wrapper adds 48 values, 112 identities, 64 state
observations and 24 ordered failures to the raw-parameter oracle.
