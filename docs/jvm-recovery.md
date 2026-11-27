# Java class and JAR recovery

`neverd java` reads class files, JARs and attached UTF-8 Java source. The engine uses the class-file version in the input. Analyzing a file requires no JVM installation. The CLI, C SDK, worker and workbench share the `language-recovery-v1` report and the same per-method diagnostics.

```sh
neverd java library.jar --inventory --release=17
neverd java library.jar --release=17 --class=example/Calculator --method='sum(II)I'
neverd java library.jar --release=17 --class=example/Calculator --method='sum(II)I' --view=ir
neverd java library.jar --component=4 --view=bytecode
neverd java Example.class --dependency=interfaces.jar --dependency=base.jar --json -o report.json
```

## Version coverage

| Class profile | Recognition, inventory and disassembly | Typed IR and Java |
| --- | --- | --- |
| 45.0–45.2 | Early short `Code` lengths, stack and local counts, qualified by owned wire samples | The sample methods recover to Java |
| 45.3 (Java 1.0 / 1.1) | Real compiler output, including pre-stack-map bytecode | The sample arithmetic, control flow and typed catch recover |
| 46–51 (Java 1.2–7) | One compiler class for every major | The same method corpus is rebuilt at each target level |
| 52–65 (Java 8–21) | One class for every major, including native 8, 11, 17 and 21 compiler outputs | Per-method lifting and the same behavior qualification |
| 66–71 (Java 22–27) | One compiler class for every major | The same subset and behavior qualification |
| Preview | For major 56 onward, minor 65535 requires the matching explicit runtime release and `--enable-preview`. Minor 0 selects ordinary bytecode. | Each method and feature still goes through validation |
| Kotlin metadata 1.9.0 and 2.4.0 | Two independently recorded metadata versions in major 61 classes | The method view uses JVM semantics and Java rendering |

A class-file version identifies a format. A 45.3 header can represent either Java 1.0 or 1.1. Newer compilers can also target older formats. Recognition, member inventory, instruction decoding, typed lifting and Java recovery have separate outcomes. A future major produces a version diagnostic. The formal class and attribute rules are in the [JVM specification](https://docs.oracle.com/javase/specs/jvms/se27/html/jvms-4.html).

## Member selection and evidence

The default JAR view selects base entries. With an explicit runtime and an enabled main-manifest `Multi-Release` attribute, each logical path selects the highest eligible variant. A Java 17 request can therefore select an entry from `META-INF/versions/11/`. All physical variants remain visible with their member paths, stored extents and content hashes. A component selector can inspect an unselected physical entry under a compatible runtime. Archive handling follows the [JAR specification](https://docs.oracle.com/en/java/javase/26/docs/specs/jar/jar.html).

A class has an immutable identity derived from its input, enclosing artifact, physical member, variant, requested runtime and internal name. Methods use the raw name and descriptor, including constructors and overloads. Instruction rows carry method-relative PCs, physical class offsets, successors and any mapped Java lines. The reverse source map lists every mapped PC for a generated line. Unmapped instructions have an empty span list. The original compiler line-number table, local variable table and generic signatures are available separately. Rendered parameter and local identifiers are deterministic placeholders.

Use repeated `--dependency` options to supply ordered local class files or JARs. The component graph records missing definitions, ambiguous candidates and their input order. Classpath definitions must also match their JAR paths and the selected runtime and preview policy. The typed frontend uses locally established parent relationships. It leaves an assignability question unresolved when a needed definition is missing or ambiguous. JNI edges carry short and long symbol candidates. Reflection, library loading and dynamic linkage remain explicit unresolved edges. Bootstrap methods, static initializers and class loaders are never executed during analysis.

## Method views

The bytecode view exposes decoded instructions, constant-pool references, handlers in dispatch order and original locations. The typed IR tracks locals, category-2 slots, allocation and constructor identities, merges, and normal and exceptional successors. Versioned frames constrain each incoming state. A malformed or unsupported method retains its own diagnostic and the last available view. Other methods in a valid class or JAR stay accessible.

The Java renderer handles qualified expressions, field effects, constructors, structured branches and loops, switch regions and single typed catches. It can render an integer SAM lambda or a string concatenation when the bootstrap handle, descriptor, arguments and target match a supported pattern. Method-level synchronization preserves the class-file flag. General monitor regions, legacy subroutines, complex joins, multi-handler cleanup, try-with-resources, record-specific bootstrap bodies and unsupported dynamic calls have bytecode or IR coverage. Generic signatures, record components and nesting metadata remain available even when source-level syntax cannot be recovered.

A `reconstructed_source` row contains generated Java for one method. An `extracted_source` row contains the original attached text. Abstract and native methods have declaration metadata. A `disassembly_only` row can also have typed IR. A component or job can report `partial_reconstruction`, `malformed`, `budget_exceeded`, `unsupported` or `cancelled`, with a reason and input offset where available. Kotlin annotations retain their own metadata version, kind and available values. Their interpretation follows the [Kotlin metadata documentation](https://kotlinlang.org/docs/metadata-jvm.html).

In the workbench, open **File → Inspect JVM classes and JARs**, select a release and inspect the inventory. Choose a physical member and a raw method key to analyze. The tabs show Java, bytecode, typed IR and artifact evidence. Selecting an instruction locates its source line. Selecting a source line highlights its mapped instructions. Copy operates on the active tab. View and report exports create a new file.

## SDK, limits and qualification

The C entry point is `neverd_jvm_recover_json_v1(path, options_json, stop, context)`. Release its string with `neverd_free_string`. The worker operation is `jvm_recover` with a path and the same options object. Supported selectors are `release`, `preview`, `inventory`, `class`, `method`, `component` and ordered `dependencies`. One bounded process-local index supports successive method requests. A hit requires the same content digests, input names, input order, runtime view and index admission limits. Each request analyzes only the selected bodies.

Default admission limits include 32 MiB of input across dependencies, 64 MiB of expanded archives, 4 MiB per class/source member, 4,096 components, 32 dependencies and a 4 MiB serialized report. Smaller `max_member_bytes`, `max_expanded_bytes`, `max_method_work` and `max_job_items` values can be supplied in SDK options. Cancellation is checked at file and parse boundaries and during bytecode analysis. Nested archives stay opaque. Global container errors reject the container. Per-member errors retain independent results.

The checked-in fixtures in `unittests/jvm/fixtures` include owned sources, compiler flags, class versions, content hashes, bytecode bytes and independent `javap -v -p -c -s` observations. `qualify.py` regenerates them from local toolchains. The historical formats use cross-compilation where appropriate. The JDK Class-File API can additionally parse the old targets and verify compatible profiles with `VerifyOwned.java`. `qualify_jars.py` creates the archive corpus. `synthetic.py` builds valid wire edge cases and malformed samples.

`qualify_behavior.py` recompiles selected emitted methods in owned class shells and runs the same harness on original and recovered builds. Across 27 release targets, each ordinary class has seven reconstructed methods and one native declaration. The harness compares 42 observations per target, plus the stripped and second-compiler cases. Separate inputs compare ordered field effects and exceptions, and qualified lambda and concatenation behavior. Production analysis only reads artifact bytes.

`qualify_performance.py --library /path/to/libneverd` measures the public SDK on a fixed ten-class-member JAR. It reports the input digest, per-query output digest, method count, cold inventory, cold and post-inventory selection, repeated selection, whole-JAR throughput and process peak RSS. Each cold measurement starts a new SDK process and excludes process startup.
