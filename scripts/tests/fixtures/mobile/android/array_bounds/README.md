# Private array-intersection method fixture

The owned Java fixture is compiled independently, then one Signature UTF8
constant is replaced with the JVM-representable but non-Java constraint
`C: Object[] & R, R: Object`. All method bytecode and runtime descriptors remain
unchanged. D8 supplies the actual recovery input; SDK dexdump checks the full
method inventory. The JVM baseline also verifies both original bounds.

The recovered private static method uses its runtime descriptor and reports
the erased generic constraints. The oracle recompiles all recovered sources,
compares descriptors and private access, and checks both array/fallback branches,
reference identity, side effects, exception identity and null ordering. It
explicitly confirms the generic-reflection difference.
