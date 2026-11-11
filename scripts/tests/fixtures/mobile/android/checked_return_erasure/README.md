# Checked reference return metadata fixture

An independently compiled final adapter implements `Transform<Owner<?>, Object>`.
Only its class and method Signature strings are narrowed to `Result`; the
classfile declaration body, including every Code attribute, is compared byte
for byte. D8 and the Android SDK inventory independently identify all methods.

The recovered method and interface keep Object as their runtime return type.
Typed and bridge calls check fresh result identity, mutable input values, wide
stamps, read and constructor failures, exception identity and effect order,
null owners and receivers, and the original bridge's failed cast. Reflection
separately confirms the reported loss of the narrowed generic return metadata.

A fresh JVM also proves that the allocated class initializes before an instance
field constructor argument is read: initialization changes that field after
the earlier callback. A separate factory checks two ordered field reads and a
null field receiver failure before the constructor's effects.
