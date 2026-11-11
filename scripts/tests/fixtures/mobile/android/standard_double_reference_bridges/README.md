Independently authored double-to-scoped-reference carrier adapter. The original
Number.doubleValue conversion retains its two-word result and both input
registers in the original instance delegation. Before baseline, bounded
Signature and bridge-flag edits attach the full Double/scoped-result contract;
Code and descriptors remain unchanged. The immutable JVM oracle checks signed
zero, infinities, distinct NaN payloads, custom Number conversion, raw result
identity and ordered conversion/target failures. Full metadata is checked except
for the disclosed raw source interface and bridge reflection flags.
