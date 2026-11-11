# Private interface result

Owned Java 8 corpus: a private static helper binds an Object element through a covariant List and contravariant callback, alongside an explicitly supplied interface-bounded result formal. The actual return comes from opaque state, not from the input target. Source and SDK-input variants preserve original declarations, metadata, casts and effects.

The separately compiled observer covers direct, separate caller, reflection and method-handle routes; repeated calls, class initialization failure, null and empty inputs, callback and append failure order, partial snapshots, wrong callback/result casts, and arbitrary/null/input result identity. Signature-only counterexamples and independent wrong programs test the same observer without changing its bytes.
