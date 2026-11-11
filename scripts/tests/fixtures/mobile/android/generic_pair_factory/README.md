# Independent pair factory formals

Owned Java 8 fixture with a generic factory owner and two independent static
method formals. Complete invariant unary inputs bind the two result slots;
a separate closed context supplies no formal. Class and method formal names
have separate scopes. No original classfile or DEX is patched.
Frozen raw, typed and method-handle callers compare input callback order,
null receiver failures, arbitrary carrier contents, result identity, partial
effects and exact checked/Error failures in source and SDK-provider profiles.
