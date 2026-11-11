# Independent paired instance result fixture

This authored fixture binds two Object-bounded method formals through independent invariant type tokens. A complete binary callback consumes both formals and returns Boolean; the instance methods return an unrelated Object, return a long, or consume inputs without a result.

Source and SDK profiles compare all original definitions, metadata and roles, compile every recovered unit, and run frozen observers against original and recovered programs. Cases cover virtual overrides, checked callback bridges, token and argument identity, nulls, ignored results, live field reads, result snapshots, partial state, wide result snapshots and exact checked/Error identity. Independent wrong programs must fail the observer. Signature-only controls retain Code and descriptors while rejecting missing scopes, bounds and incomplete declarations. No external application is executed.
