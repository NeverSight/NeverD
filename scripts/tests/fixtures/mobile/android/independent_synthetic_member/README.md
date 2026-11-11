# Independent synthetic member

Authored metadata witness for a final synthetic named member with no outer
parameter or capture field. Its own generic class formal remains independent of enclosing
scopes; a primary and delegated constructor retain their actual callbacks and
stores. Encoding changes only class/InnerClasses flags and preserves every Code
attribute and descriptor. Both compiler-original and encoded inputs execute
against the frozen harness; generated Java discloses its static/synthetic class
metadata changes and preserves all original definitions, effects and failures.
SDK mode supplies only the public carrier interface.
