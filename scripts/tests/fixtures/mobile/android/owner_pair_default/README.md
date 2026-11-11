# Owner-formal pair default accessor

A genuine Kotlin all-compatibility interface keeps one Object-bound owner
formal and a default Boolean method accepting that same formal twice. Frozen
virtual and static compatibility calls, reflection and method handles compare
identity, nulls, short-circuit state reads, overrides and exception order. The
compiler's accessor must bypass a subclass override while the default body's
state getters still dispatch virtually. Four wrong programs must fail the
observer. All original entries, signatures and runtime metadata are checked;
D8's dropped CLASS annotations are separately recorded.
