# Nested completion callback binding

An independently compiled policy interface and implementation share a callback
consuming Completion<? super R>, plus the same independently passed completion.
The callback returns arbitrary Object values. Frozen callers check exact
receiver/completion identity, deferred captures, sentinels, nulls, repeated calls
and ordered failures. Every original generic contract and compiler entry is
audited in owned and SDK declaration configurations; no metadata edits.
