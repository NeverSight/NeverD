# Empty package metadata retains its compiler identity

Two named packages have canonical empty package-info classes: synthetic abstract
interfaces extending Object in classfile metadata, with no members, annotations
or type parameters. Their Java files contain package declarations. The recovery
report requires `java_compiler_options: ["-Xpkginfo:always"]` to retain these
otherwise absent classfiles; see the official javac option documentation.

Frozen original classes and an independent observer check package identity,
compiler flags, reflection, empty members/annotations and ordinary ordered calls
in source and SDK modes. Wrong compiler flags, omitted/renamed metadata,
invented annotations, changed arithmetic and lost/repeated calls must fail.
