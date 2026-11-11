# Guarded constructor zero provenance

Owned Java and genuine Kotlin JVM 8 classes retain a conditional superclass
argument followed by a safe cast. D8 reuses the original null argument word
after the superclass call; a different nominal type on the other branch must
merge with null. No original classfile or DEX is patched.
Frozen direct, reflection and method-handle callers check seed effects before
receiver escape, parent fields, null/nonmatching inputs, selected child identity
and exact checked/Error failures. Source and SDK-provider profiles retain all
original definitions and explicitly compare metadata and compiler differences.
