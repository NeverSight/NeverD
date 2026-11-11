# Local generic void setter and reference updater slots

Independent Java declares a pure `assign(V)` property writer and the ordinary
`Map.Entry.setValue(V)` updater. A validated constant-pool name encoding changes
only `assign` to `setValue`, producing the original distinct JVM void/reference
slots without changing Code, descriptors, flags or Signatures. Original input
callers receive the same encoding before the baseline is frozen.

Java source names the void writer separately and reports that method-name/ABI
difference explicitly. Original method identities remain in the report and map
independently to compiled source identities. The reference-return interface API
keeps its original name. Frozen observers accept only the disclosed void name;
their binary code stays unchanged.

Direct/recovered-caller, frozen interface-caller, reflection and method-handle
routes check old-value snapshots, exact field commits before callbacks, callback
replacement, nulls, polluted values, class initialization and exact failures.
The SDK callback calls only the unchanged `restore` API; its original bytes are
preserved. Same-Code signature mutants and independent wrong programs protect
scoped variable identity, source naming and observable operation order.
