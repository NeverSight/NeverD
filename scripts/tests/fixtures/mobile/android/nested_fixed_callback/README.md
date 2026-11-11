# Nested callback with a fixed result

Original Java metadata declares a private static callback extractor and its raw
public accessor. The input producer contains a consumer callback of the same
formal with an independently declared fixed result. Frozen interface
implementations expose getter count, dispatch, returned callback identity, nulls,
failed casts and exact exceptions. No application data or metadata edits are used.
