**Languages**: [English](ios.md) | [简体中文](zh-CN/ios.md) | [繁體中文](zh-TW/ios.md) | [日本語](ja/ios.md) | [한국어](ko/ios.md) | [Français](fr/ios.md) | [Deutsch](de/ios.md) | [Español](es/ios.md) | [Italiano](it/ios.md) | [Русский](ru/ios.md) | [العربية](ar/ios.md)

# iOS native and source recovery

[← Documentation index](README.md) · [Mobile overview](mobile.md)

`neverd mobile` accepts IPA, `.app`, and Mach-O inputs. It exports native C, runtime metadata, and experimental Objective-C `.m` and Swift `.swift` source for supported native bodies. A published result can contain unrecovered methods; inspect the coverage report before using the source. Mobile containers are a CLI workflow; the native C SDK accepts the selected Mach-O separately.

Compilation removes comments, formatting, identifiers, and source constructs. This workflow reconstructs source representations; it cannot restore the original text or certify equivalent behavior for an arbitrary application. It does not launch the analyzed application.

## Start and dependencies

```sh
cmake --build build --target neverd
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-arm64 --arch=arm64
neverd mobile executable -o metadata --metadata-only
neverd mobile App.app -o recovered-framework --artifact Frameworks/Example.framework/Example
```

Build the `neverd` target with C++20 support. The mobile workflow runs in the native CLI without a Python interpreter. Swift signature demangling comes from `LLVMSwiftDemangle` in the NeverD LLVM fork; both source builds and matching published LLVM packages include this component. NeverD does not fetch a separate Swift source dependency. Building and running NeverD require no installed Swift compiler or toolchain. LLVM, Capstone, and other native library dependencies still apply; distribute the libraries and license notices required by your build. Independently compiling generated Apple-language source and running the Swift behavior tests on macOS require Apple Clang, the SDK, and `swiftc` as appropriate.

Swift signature recovery consumes structured nodes from `LLVMSwiftDemangle` directly inside the C++ process. It does not locate or start an external demangling executable or toolchain discovery command. The former executable-path option is removed, and the former demangler environment variable is not read. `--metadata-only` invokes neither the native source exporter nor signature demangling.

The signature inventory in `metadata/swift-signatures.json` records the built-in component as:

```json
{
  "demangler": {
    "name": "llvm-swift-demangle",
    "execution": "builtin",
    "version": "6.3.3"
  }
}
```

```sh
neverd mobile App.ipa -o recovered-swift --timeout=600 --json
```

## Inputs and selection

IPA must contain exactly one top-level `Payload/*.app`. `.app` and IPA main executables are selected through `Info.plist`'s `CFBundleExecutable`. `--artifact` is relative to that application bundle in both formats; it selects one embedded executable, without recursively analyzing every framework or extension. Raw Mach-O input does not accept `--artifact`.

For fat binaries, `--arch=auto` prefers arm64, arm, x86_64, then i386. Missing or unsupported slices fail explicitly. Source-language projection currently targets arm64 and x86_64; selecting another family does not imply Objective-C/Swift source support. A selected slice with `cryptid != 0` is rejected. Provide an already decrypted, readable analysis input. Archives and directory inputs reject unsafe paths, symbolic links, special files, and conflicting entries.

## Options and resource limits

| Option | Default | Meaning |
|--------|---------|---------|
| `-o DIRECTORY` | Required | New output directory outside a directory input; existing output is preserved |
| `--platform=auto\|ios` | `auto` | Infer the input platform or select iOS |
| `--arch=auto\|arm64\|arm\|x86_64\|i386` | `auto` | Select one Mach-O slice |
| `--artifact PATH` | Main executable | Application-relative executable path |
| `--metadata-only` | Off | Read metadata without source recovery or tool invocation |
| `--max-func N` | `0` | Native function limit; zero means all discovered functions; ignored in metadata-only mode |
| `--timeout N` | `300` | Positive total analysis budget in seconds; child processes use the remaining budget |
| `--max-files N` | `20000` | Positive entry budget; Swift symbol inventory is also bounded |
| `--max-bytes N` | `2147483648` | Positive input, extracted-data, and final-output byte budget |
| `--json` | Off | Print the versioned report as JSON |

The work area is monitored with up to three times the configured entry/byte budgets for staged input and intermediate output. Logs are limited to 16 MiB per process; Swift signature JSON is additionally limited to 32 MiB. Limits are resource controls, not process isolation. Increasing a timeout does not disable byte or entry limits. Methods excluded by `--max-func` remain unrecovered when present in the metadata inventory.

## Objective-C source and runtime structure

Both source-report modes include `source_projection_graph`. Its nodes report
the final typed native bodies, local diagnostics, merged native/Block
`dependencies`, and the production `closure_closed` result. Local checks and
propagated dependency failures have separate reasons. Missing typed bodies have
incomplete diagnostics. These edges describe the current projection; fixing an
unbound call may introduce further dependencies. A closed node has passed the
dependency stage only: each method must still pass emission and source-text
checks before its status becomes `recovered`. Use this graph for helper work;
`native_dependency_graph` remains a separate LowIR call inventory.

For repeated coverage analysis, the following command runs the same analysis
and publication checks as
`--format=objc-methods`, but omits `native_source` and each method's `source`.
The JSON adds `sources_omitted=true`; method identities, statuses, diagnostics,
signatures, shared helper references and dependency evidence retain their full
export meaning. Rendering checks still run, so this saves source retention,
encoding and output rather than bypassing validation. Use the full mode to
obtain compilable source artifacts. The corresponding C entry point is
`neverd_objc_methods_summary_json(session, max_functions)`; free its result with
`neverd_free_string`.

```sh
neverd export WMF --format=objc-methods-summary -o summary.json
```

The native loader binds a runtime method record, executable IMP address, and supported type encoding to explicit source ABI locations. Fixed scalar/pointer bindings include hidden `self`/`_cmd`, unused arguments, separate integer/floating register banks, and supported stack positions. Float/double bit reinterpretation is distinct from numeric conversion. Type hints are source-projection inputs, not authenticated ABI evidence or permission to patch executable code.

`sources/objc.m` places actual recovered statements in `@implementation` bodies and preserves required C helpers and typed calls. Eligible call targets must have a supported source binding; unknown targets and incomplete dependency groups remain unrecovered. Missing definitions, invalid executable addresses, conflicting encodings, unsupported ABI mappings, incomplete decoding, and rejected IR cannot become recovered methods merely because a declaration is available.

Native helper parameters may acquire pointer types for source projection when complete incoming values reach already bound pointer arguments through COPY/PHI operations without conflicting scalar uses. This inference preserves physical ABI locations and generic IR types; source bodies and their entire native dependency groups still require validation.

A native scalar helper may return an observed incoming parameter when its exact ABI register covers the complete result width. A bounded proof combines every return path with the initial entry and loop backedges; calls and partial writes invalidate the value. Undeclared live-ins, unused parameter placeholders, SSA seeds and PHIs do not establish inputs. This source-only inference preserves physical locations and still requires complete body and dependency validation.

Local native helpers can expose full-width integer inputs in auxiliary registers, including result-buffer pointers, when observable-entry analysis and complete LowIR reads agree. Caller-saved inputs may later be overwritten as scratch; a written preserved context input requires an independent byte proof that every exit restores the incoming preserved, stack and link state. If that context register is itself overwritten, its complete entry identity must first reach a non-preservation use such as a bound call or address calculation; copies, returns, and private-frame save/restore traffic alone cannot create a source parameter. Implicit call definitions, including SSA version zero, cannot become entry inputs; only explicitly preserved bytes may pass through a call. Regenerated callers and definitions share the same physical input locations. This does not infer an external C or Swift prototype, and complete bodies and dependency closures remain mandatory.

Fixed C callback types preserve their parameter and return signatures in source declarations and casts. The exact `swift_once` import binds a predicate, a `void (*)(void *)` callback, and a context, with no result. This retains the runtime call; it does not prove callback-body recovery or ownership of shared initialization storage. Incomplete dependencies remain unrecovered.

Known imported Objective-C runtime calls retain explicit argument and return bindings: retain/release, autorelease, strong/weak storage, object allocation, and fixed-signature property setters. ARM64 register-specific retain/release entry points read the named register and emit the corresponding ordinary runtime call. Import identity and ABI must agree; an arbitrary similarly named function does not qualify. Recompiled ARC fixtures check strong-reference lifetimes, weak zeroing, property copying, and destruction against the original methods on macOS.

The exact libobjc optimized class and selector queries retain their runtime calls, including nil handling and custom overrides. Their byte results preserve the caller’s native conversions; they are not replaced with a guessed class hierarchy test.

Source binding builds the verified direct class-object identity map only when a pointer argument has a constant address. The map is local to one binding operation; each later operation validates the current image again, including class names, metaclass flags and conflicting identities.

Associated-object get/set/remove calls preserve the object, key, value, and pointer-sized policy arguments. Generated C uses the public Objective-C runtime header, and executable regression tests compare retention, copying, and removal with the original methods.

Objective-C exports also bind a fixed set of ordinary C-ABI Swift runtime imports for retain/release, native and unknown-object weak references, object metadata, and begin/end access. Generated C preserves these calls and requires the Swift runtime when linked. Register-specialized entry points, unrecognized Swift calling conventions and arbitrary Swift symbols remain unsupported; import identities and exact scalar carriers must agree.

Separate bindings support the exact Darwin String → NSString import `_$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF` and optional NSString → String import `_$sSS10FoundationE36_unconditionallyBridgeFromObjectiveCySSSo8NSStringCSgFZ` on arm64 and x86_64. Both preserve ownership and Clang `swiftcall`; linking requires Swift Foundation and Swift Core. Their exact static call identities and source ABIs may participate in native state-restoration proofs. The reverse bridge carries both returned String words as an unsigned 128-bit integer, split into explicit return registers before SSA. This is a bit carrier, not a recovered String layout or general aggregate ABI. Unknown signatures and incomplete initialization dependencies remain unsupported.

Static keys with exact addresses in read-only Mach-O C-string storage are rebuilt as shared opaque identities only at authenticated associated-object key arguments. Equal original addresses share a key; different interior addresses remain distinct. Mobile export merges these helpers automatically. The C API lists their names in `shared_identity_functions`; link one definition of each helper across the participating method units. These identities belong to rebuilt code, not an already loaded original image. Other uses of unbound image addresses remain recovery limitations.

Verified imports for `os_unfair_lock_lock`, `os_unfair_lock_unlock`, `os_unfair_lock_trylock`, `os_unfair_lock_assert_owner`, `os_unfair_lock_assert_not_owner` call the real Darwin implementation through `<os/lock.h>`, preserving the lock address, ownership checks, and Boolean result. Unknown variants remain unsupported. Integer call results preserve only their declared ABI bits: Darwin arm64 extends 8/16-bit results to 32 bits according to signedness; the remaining register bits stay unknown. Declared method return widths enter SSA before branch merging so unused high bits do not obscure valid low-byte results.

Verified Darwin `__cfstring` records can be rebuilt as constant objects when their class import, layout, character storage and fixups are complete. ASCII bytes and UTF-16 code units, including embedded NULs, are retained. Equal original object addresses share one rebuilt identity; distinct records stay distinct. These helpers join `shared_identity_functions` and require Foundation when linking. This does not authorize raw memory accesses to constant-object records.

Numeric profiling counters in a bounded, pointer-free `__DATA,__llvm_prf_cnts` section can use shared rebuilt storage. NeverD preserves captured initial bytes, overlapping 1–16-byte loads/stores and updates across method source files. An exact native source dependency may receive an interior counter pointer when its complete typed body proves that the parameter has only bounded, unordered numeric accesses at that exact address. Mobile export merges the storage helpers; C API consumers must link one definition of each `shared_storage_functions` helper. This storage is independent of the original image and its profiling runtime. Escaping, forwarding or offsetting addresses, pointer-typed or ordered accesses, incomplete mappings and pointer relocations remain unsupported. Instrumented block callbacks may update this image storage while their private addresses remain confined; ordinary counter writes do not count as private frame spills.

Class metadata retains superclass identity, instance start/size, and scalar/pointer ivars with checked offsets, widths, and alignments. Declarations use padding where necessary. A method requiring unavailable instance layout remains unrecovered. Categories retain separate class/category/address identities and separate implementations; identical entries repeated in class/category inventories are counted once. External categories use an existing Foundation class declaration where supported. Unknown external class headers are reported as missing dependencies; no substitute class layout is invented.

Recovery also requires complete class declarations, definitions for local ancestor classes, and emitted source in which values are defined on every path before use and reachable exits have the required return; an empty local superclass receives an empty `@implementation` only when verified layout and a complete method inventory prove that it has no ordinary methods of its own. If this evidence is missing, or a name conflicts with a known Foundation import, affected methods remain in the coverage denominator with reasons while independent recoverable classes continue to be emitted; these name checks do not cover every SDK name, iOS SDK, or version.

Supported Objective-C Block calls use a complete fixed scalar invocation ABI, including the hidden Block object and every argument and return carrier. The runtime encoding `@?` is widened to `id` only in declarations; it does not supply an invocation prototype. Global Block references preserve shared object identity. Supported synchronous scalar captures require proven native capture storage and invocation flow. Strong object captures copied with a verified `objc_retainBlock` / `_Block_copy` call may escape when construction initializes their storage on every reaching path and the invocation, copy and dispose bodies are fully recovered. The generated descriptor keeps the original helper ABIs and ownership layout. Weak/byref ownership, unknown layouts and unproven consumers remain unrecovered. Compiler-declared nonescaping C block parameters are also supported when the exact import, parameter position and complete callback ABI agree. This lifetime contract does not imply read-only memory. When linking C API units together, provide one definition of each `shared_block_functions` entry. Mobile export merges matching definitions and rejects conflicts, including differences in private callees.

Protocol method declarations are read from resolved local runtime records, including inherited protocols and required/optional instance and class methods. Ordinary and relative method lists share the same decoder. All matching class and protocol declarations must agree before a selector receives a fixed call signature; malformed records and inheritance cycles cannot supply hints. Well-formed pointers to structures, unions and arrays use opaque pointer carriers without inferring their layouts. `objc_metadata.protocols` exposes declarations separately: they do not count as recovered implementations or establish class conformance. For otherwise identical signatures, signed and unsigned 64-bit integer results share an unsigned bit carrier; narrower integer, floating-point, pointer, or argument-type conflicts still reject the call.

Compiler-declared NSString format calls can recover promoted scalar arguments from a verified constant format object. Sequential and positional formats must identify every argument without gaps or conflicting types. An exact selector stub can preserve a dynamic format only with an empty tail or a tail made entirely of source pointers, including scalar-spelled SSA values whose complete definition chains end in authenticated pointer-returning Objective-C runtime calls. Integer, floating, mixed-definition and otherwise untyped tails still require parsed immutable format text. Darwin arm64 reads unnamed arguments from eight-byte stack slots; x86_64 uses its integer and floating register banks before overflowing to the stack. Generated calls keep an ellipsis and dynamic dispatch. Unknown conversions, count writes, long double and unsupported extensions remain unrecovered. The same format analysis supports declared C imports such as `NSLog`, with exact library export checks and a shared variadic prototype across calls with different argument counts.

Private frame stores retain every byte read anywhere in the function. With proven frame bounds, immutable entry aliases and no address escape, NeverD can remove unread stores or shorten an integer store’s unread tail. This allows promoted scalar arguments to remain recoverable when only their unused stack padding is unknown. Unknown bits are never filled in. Ordered accesses, unbound calls, ambiguous addresses, effectful values and exhausted analysis budgets retain the original stores.

Colliding category methods keep validated ABI declarations even when their runtime override order is unknown. A dynamic call may use that ABI only when every matching declaration agrees; ambiguous implementations remain ineligible for source body selection. Incompatible or malformed declarations still veto the call.

Known `objc_enumerationMutation` calls preserve the object argument and the continuation because an installed mutation handler may return. Exact Darwin `__stack_chk_guard` imports bind the runtime object identity; guard loads, comparisons, and `__stack_chk_fail` calls remain observable in recovered source.

Linked 64-bit Darwin images importing system Foundation also consult built-in compiler-derived framework declarations. Every runtime and framework declaration for a selector must agree; variadic, unsupported aggregate, or platform-inconsistent signatures stay unbound. The catalog supplies call types, not receiver classes or function bodies. Using NeverD does not require a local Apple SDK. CoreData has a separate catalog activated by its exact system framework dependency. Declarations belong to the framework owning their public header; included dependency headers do not activate another framework.

ARM64 UIKit Objective-C declarations are admitted only when compiler-produced iPhoneOS and arm64 iPhoneSimulator artifacts agree on the owner and desugared signature, and the image imports the exact system UIKit provider. The catalog covers scalar, pointer and supported record carriers such as `CGFloat`, `NSInteger`, `CGSize` and Core Graphics object pointers, including agreed view hierarchy, geometry, text, control-event, navigation, responder, image, nib, progress-style, accessibility and color-initializer signatures. It preserves owner-qualified object results such as UIImage's right-to-left layout-direction flip. A selector shared by incompatible owners remains unbound unless receiver, exact result-carrier evidence, or an unchanged pointer-to-pointer parameter from the enclosing Objective-C method selects one compatible declaration. The parameter proof requires every method record sharing the caller entry to agree, and publication rechecks the entry carrier, message argument position and current selector declarations. A full-width receiver or declared pointer-to-pointer value may cross an exact private-frame spill when its slot is wholly below every escaped frame address. Reloads at or above an escaped base, overlapping writes, partial or transformed values and conflicting control-flow facts do not qualify. For example, UIKit's declared `+[UIScreen mainScreen]` result remains a `UIScreen` receiver across an exact ARC retain-return identity, allowing `-[UIScreen scale]` to select the owner-qualified `double` contract. A standalone floating-point use of `scale` can also select that compatible result ABI. Architectures without matching compiler evidence remain unsupported. ARM64 CoreImage selectors use the same dual-SDK evidence and exact-provider gate; supported record arguments such as `CGRect` retain their floating-point carriers.

An exact private-frame storage address passed directly as a message argument can also select the unique pointer-to-pointer declaration, including CoreData's `save:` contract. This is structural evidence for the pointer-to-pointer shape only; it does not infer the pointee type or stored value. Publication checks the HighIR argument, exact negative entry-SP-relative offset, private frame bounds and current declarations. Incoming or positive offsets, reloaded or transformed values and multiple matching declarations remain unbound.

An authenticated Objective-C message target preserves receiver identities held in complete Darwin call-preserved registers even when that message lacks a source declaration. All caller-saved and non-receiver facts are discarded, and private frame storage is treated as escaped. A later message gains a binding only from its own exact receiver declaration and ABI.

The C declaration catalog also covers CoreGraphics, CoreServices and ImageIO exports. Opaque image, color, string and `iovec` pointers, integer counts and floating results retain their declared ABI. Public system framework aliases are generated alongside export facts; private paths, different framework versions and undeclared symbols gain no binding. Mobile declarations now use the loader’s type grammar too: well-formed aggregate pointees become opaque pointers without assuming their layout. Fixed `notify.h`, vector I/O and `UTTypeConformsTo` APIs come from the same four-target compiler intersection and require their exact system exports.

UIKit fixed C functions absent from the command-line-tools catalog require an exact symbol and dyld provider match. On ARM64, `NSStringFromCGSize` and `UIGraphicsBeginImageContext` use the shared fixed-record ABI: the two-double `CGSize` value is read from its two floating-point carriers. `UIGraphicsGetCurrentContext` and `UIGraphicsGetImageFromCurrentImageContext` return opaque pointers, while `UIGraphicsEndImageContext` returns void. Generated source retains every real UIKit call. Weak imports, addends, conflicting storage, other providers and unsupported architectures stay unbound.

Large immortal Swift string literals can bind their UTF-8 bytes to shared static storage at an established Foundation bridge. The count, flags, terminator, valid UTF-8, immutable storage and exact import must agree. The original tagged representation and bridge remain intact; embedded-zero literals and other storage forms remain unbound.

Validated constant NSString objects also retain shared identity when an integer carrier with complete data-address provenance is assigned or stored. Scalar immediates, incomplete addresses, numeric operations and accesses to private object bytes do not gain that binding.

Ordinary scalar loads from proven immutable, nonrelocated image bytes can become bit-preserving constants. Integer widths of 1, 2, 4 and 8 bytes and 4/8-byte floating values are supported. Writable or ambiguous storage, ordered loads and address consumers remain unbound; a numeric occurrence does not authorize pointer uses of the same expression.

Indexed scalar loads can use an immutable byte table when the shared source-flow analysis proves an unsigned upper bound on every reaching path. Guards, masks, native integer widths and wraparound retain their semantics; writes and escaped locals invalidate earlier facts. Each table is limited to 4,096 entries and 65,536 bytes. Publication rechecks current bounds, storage, fixups and every helper use. Loaded full-width bits naming a mapped image section remain pointer-ambiguous; narrower scalar pieces and segment padding alone do not establish pointer identity. Only scalar reads receive these copied bytes; table addresses cannot escape. Executable regressions compare integer bits, floating bits including negative zero, and out-of-range behavior with the original methods.

Fixed C calls additionally use compiler-derived declarations and SDK export/reexport facts. Binding requires the exact dyld library, symbol and scalar ABI; weak imports, unknown providers and unsupported prototypes remain unbound. The public `dispatch_once_f` and `dispatch_queue_set_specific` callback prototypes replace only their catalog's opaque `^?` callback encodings, at the exact libdispatch export boundary. An exact `dispatch_once_f` call may rebuild a complete writable predicate and a unique local callback with the public callback ABI; the callback still needs a complete source dependency closure. Generated C uses separate identifiers linked to the original symbols and fresh shared predicate storage. Ordinary synchronization calls without language dispatch tables preserve their real calls and memory effects.

An exact integer zero passed to a declared pointer parameter remains a null pointer even when ARM64 materializes it through a narrower `W` register. Source generation emits an explicit null pointer constant for that case. Nonzero narrow integers and undeclared pointer uses remain rejected as carrier mismatches.

The C catalog includes `sys/mount.h` declarations with architecture-specific linker identities: ARM64 uses `getmntinfo`, while x86-64 uses `getmntinfo$INODE64`. The output remains an opaque pointer-to-pointer, the mode and result remain signed 32-bit integers, and exact system export checks apply. No filesystem-record layout or returned buffer contents are invented.

External data bindings use common non-TLS SDK declarations and exact library export evidence. Generated C refers to the real symbol storage and preserves subsequent memory accesses, including the distinction between a global pointer and its pointee. Weak imports, conflicting identities and unsupported storage remain unbound. Data declarations do not establish block construction or ownership. The catalog includes CoreData, CoreImage, CoreGraphics, ImageIO and CoreSpotlight data. Built-in literal storage is derived by compiling empty collections and Boolean objects on every target; only direct addresses of external non-TLS data qualify, with the same export checks. Catalog generation requires the Clang compiler through `--clang` in addition to libclang.

On ARM64, the external `kCIContextPriorityRequestLow` and `kCIContextUseSoftwareRenderer` storage identities use matching declarations from complete iPhoneOS and arm64 iPhoneSimulator SDKs, with an exact CoreImage import. Context options retain their real pointer loads and runtime values; the binding does not substitute string keys or rendering behavior.

ARM64 `UIApplicationDidEnterBackgroundNotification` also binds to its exact UIKit external storage. Both SDK declarations agree; the original pointer load and runtime notification identity remain intact.

This is a limited reconstruction of runtime information. Complete properties, protocols, original ownership annotations, arbitrary aggregates, variadic tails, exception-dependent bodies, and unmodelled Block/capture layouts are not promised. Runtime encoding describes fixed arguments and cannot prove that the original declaration had no variadic tail. Chained pointers are used only where the native loader resolved the relevant slots; unresolved formats retain diagnostics.

## Swift source and storage

Structured demangler output classifies callable signatures separately from non-callable metadata. Supported signatures are bound to the selected binary's symbols, entries, and explicit machine ABI before native body projection. Swift receivers use Swift ABI rules; Objective-C hidden arguments are not substituted. A user-supplied signature file is a hint that still requires validation.

The experimental emitter can construct supported free functions, class methods, designated initializers, and fixed-layout struct methods, including supported mutating receiver forms. Class/struct declarations and stored fields require recovered layout metadata. Native calls are emitted only when the required source declarations and bodies form a complete supported dependency group. Recovered source units contain those declarations and methods together, rather than a bridge that calls the original binary.

Supported Swift getter and setter bodies come from the native implementation and are assembled into properties. Private backing storage preserves the established field layout; initializers and other methods use the same storage names. A property declaration or field record alone cannot establish a recovered accessor body.

Supported allocating constructors, trivial destructors/deallocators, type metadata accessors, and `_modify`/resume entries can project into an emitted type unit. Each requires a bounded proof of the complete native flow and effects, actual recovered context/initializer/property dependencies, and exception-handling and IR audits of the related bodies. Allocator writes must match the actual initializing constructor; `_modify` must bind the exact mutable field and continuation. Runtime metadata calls retain their modeled semantics within the recovered type. These entries are explicitly reported as compiler source projections; they do not constitute separately recovered ordinary method bodies or original source text.

Generic or resilient layouts, async/throwing functions, unknown calling conventions, unsupported accessors/allocators/thunks, incomplete initialization, and unbound native or runtime dependencies remain individually `unrecovered`. A mangled symbol or nominal type name alone is not a recovered method. Stripped symbols and unclassified demangler nodes make coverage incomplete or unknown.

## Output and coverage

```text
recovered-ios/
  artifacts/selected.macho
  sources/native.c
  sources/objc.m
  sources/swift.swift
  metadata/objc.h
  metadata/objc.json
  metadata/objc-methods.json
  metadata/swift.json
  metadata/swift-signatures.json
  metadata/swift-methods.json
  logs/
  report.json
```

Source-language files exist only when source can be emitted. `objc.json` contains classes, categories, ivars, and raw method encodings; `objc.h` contains supported declarations. `swift.json` contains nominal-type metadata and mangled symbols. Signature and method JSON files preserve classification, omissions, reasons, and counts. Logs contain native diagnostics and native Swift export diagnostics when that export runs. There are no external Swift toolchain-discovery or demangling logs. Output paths in `report.json` are relative to its directory. The selected binary is an analysis artifact; generated source does not link it as a recovery bridge.

Temporary package copies and intermediate backend JSON are removed. A normal run with no native function bodies fails even when metadata exists. Metadata-only output contains the selected artifact, `objc.h`, `objc.json`, `swift.json`, and `report.json`; source directories and method-coverage/signature files are absent, and `native_function_count`, `objc_method_recovery`, and `swift_method_recovery` are `null`. All modes use the native loader’s resolved Objective-C metadata. Swift metadata uses bounded native-image reads; unsupported fixups, relocatable layouts, or references retain partial diagnostics.

A shortened illustrative report deliberately shows partial recovery:

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "ios",
  "architecture": "arm64",
  "objc_method_recovery": {
    "status": "partial",
    "method_count": 3,
    "recovered_method_count": 2,
    "unrecovered_method_count": 1
  },
  "swift_method_recovery": {
    "status": "partial",
    "coverage_status": "partial",
    "method_count": 4,
    "recovered_method_count": 2,
    "source_body_method_count": 1,
    "compiler_projection_method_count": 1,
    "unrecovered_method_count": 2,
    "metadata_symbol_count": 5,
    "unclassified_symbol_count": 1
  }
}
```

Outer `status: "success"` means validated output was published. Method coverage `recovered`, `partial`, `unrecovered`, or `no-methods` describes the discovered inventory, not semantic equivalence or original-program completeness. Every unrecovered method has a reason. Objective-C `recovered` additionally requires complete runtime metadata. An empty inventory cannot prove there were no methods.

An unrecovered Objective-C row may include `native_backend: {status, reason, diagnostics}` when a unique backend result exactly matches its runtime identity. This bounded optional summary preserves the backend's intermediate result even when a declaration or layout check fails. The row's primary status, reason and recovery counts remain authoritative; an absent summary means the evidence was unavailable.

Swift `coverage_status` counts classified callables only. Overall Swift `status` also accounts for unknown symbols and can be `unclassified`, `unsupported-architecture`, or `no-symbols`. Non-callable metadata is listed under `non_method_symbols` with `not-callable`; unknown symbols use `unclassified`. `types`, `type_metadata_count`, and `source_type_count` count type metadata/emitted type units independently and must not inflate method counts.

Each recovered Swift row reports `source_representation` as `native-method-body` or `compiler-generated-from-type`. Compiler projections also retain `compiler_projection_kind` and `compiler_projection_evidence`. `source_body_method_count` counts recovered native method bodies; `compiler_projection_method_count` counts proven compiler projections. Their sum equals `recovered_method_count`. Compiler entries remain in the `method_count` denominator and retain their exact identity in one corresponding `type` source unit. Neither type metadata nor a dependency name alone increases recovered coverage. Native batch JSON includes `source` on compiler rows and type units; mobile `source_units` retain descriptions without `source`, and the complete source is in `sources/swift.swift`.

Native Swift batch `source_units` records `{kind, module, name, source, method_entries, method_identities}`; kind is `function` or `type`. Each identity is `{entry, mangled_symbol}`. Different symbols may share one entry and retain distinct ABI projections; every recovered identity must appear exactly once and no unrecovered identity may appear. `method_entries` must equal the ordered entry projection of `method_identities`, including repeated addresses. Repeated identical identities cannot be silently merged. Batch `source` equals the ordered concatenation of each unit’s source plus a newline. Mobile retains aggregate source in `sources/swift.swift` and unit descriptions in coverage JSON. Standalone method `source` is for inspection; concatenating those rows does not reconstruct class declarations correctly.

## Direct native exports and SDK

```sh
neverd export recovered-ios/artifacts/selected.macho \
  --format=objc-methods --max-func=20 -o objc-batch.json
neverd export recovered-ios/artifacts/selected.macho \
  --format=swift-methods \
  --source-signatures=recovered-ios/metadata/swift-signatures.json -o swift-batch.json
```

Swift export consumes the structured signature inventory produced by the built-in signature parser during a normal mobile run. Objective-C batch JSON includes `native_source`, `native_function_count`, `objc_metadata`, and per-method C source, function name, return type, and parameters. Mobile performs further declaration/body/layout checks before generating `.m`, so its final method coverage can be narrower than batch C coverage. A successful native export can contain no recovered methods.

When a method fails call binding, its batch row includes `unbound_call` with the first rejected target name/address, indirect-call flag, and recovered argument count. If a source signature exists, the record also includes its binding name and expected argument count. This identifies the first blocking call; it is not an inventory of every remaining problem in that method.

Each method also includes `projection_diagnostics`: `items` records independently checkable blockers with a `code`, `reason`, and available statement, call, value, or dependency evidence. `checks_complete: false` means a missing prerequisite or resource limit prevented further checks; an empty partial report does not establish recovery. This uses the same checks as the admission gate and leaves the existing `status`, `reason`, and `unbound_call` contract intact.

The batch `native_dependency_graph` inventories calls in the final LowIR, starting from supported Objective-C method signatures and following direct calls into image code. It preserves caller, block, and instruction addresses, including shared callees and cycles. Indirect targets remain null. `inventory_complete` is false when a required LowIR function is missing or the call record limit is reached; `targets_complete` additionally requires all inventoried calls to have direct targets. This scope excludes unsupported method roots and unresolved indirect destinations; it does not prove complete native execution coverage or dependency source recovery.

Validated local protocol-reference slots use `objc_getProtocol` to preserve registered protocol identity. The batch `runtime_protocols` dependency inventory includes native callees. Conflicting names, incomplete declarations, imported slots and unresolved fixups remain unbound. Standalone export reports missing protocol registration instead of emitting a body that could receive a null protocol.

The Swift runtime ABI catalog is generated from pinned upstream declarations and preserves the C or Swift calling convention. Known pointer and pointer-sized unsigned types form fixed scalar signatures. Swift calls require an exact strong libswiftCore import; the two supported versioned metadata availability classes do not authorize weak imports. Custom parameter registers, unknown representations, unsupported availability classes and conflicting declarations remain excluded. In particular, swift_willThrow needs compiler-added swiftself/swifterror attributes absent from the declaration DSL. Calls retain their effects and specialized existing callback/byte contracts. Regenerate with `scripts/generate_swift_runtime_declarations.py --input RuntimeFunctions.def --source-sha256 <pinned-hash>`; `--check` verifies the catalog. Revision, content hash and third-party notices accompany the facts.

Required Swift value-witness calls preserve their runtime table lookup on arm64 and x86_64. A bounded machine-dataflow proof pairs the metadata argument with the table at metadata minus one pointer and one of the eight required function slots. The source ABI covers destruction, buffer/value copy, assignment, take, and single-payload enum tag access. Enum tags and counts remain unsigned 32-bit values, including the result of `getEnumTagSinglePayload`. Source calls retain `swiftcall`, memory effects, and the exact metadata argument. Unknown slots, conflicting paths, partial writes, and optional enum witness entries remain unsupported. The canonical signatures and slot order follow Swift's [ValueWitness.def](https://github.com/swiftlang/swift/blob/main/include/swift/ABI/ValueWitness.def); this binds a source projection, not permission to fold a dynamic target into a local implementation.

Fixed Swift runtime declarations also retain two-word results: two pointers, or the declared metadata pointer and state word. The shared ABI layer assigns both result registers, and source validation rechecks their order, types and exact import. Box allocation and metadata queries still execute the real runtime calls; aggregate parameters, unknown layouts and hidden contexts remain unsupported.

Separately, exact compiler-observed Foundation value bridges for URLRequest, Notification, URL, Data, Date and IndexPath retain `swiftcall` and provider identity. Indirect results and context/self use `swift_indirect_result` and `swift_context` in their dedicated registers (x8/x20 on arm64 and RAX/R13 on x86_64) without consuming the ordinary integer-argument bank. Data keeps its explicit two-word transport. These are call-carrier declarations, not recovered value layouts or a general Swift ABI.

Native scalar-return inference recognizes a complete, immediate extraction of both words from a call with a validated result ABI. Temporary identity, member offsets, widths and physical registers must all match. This lets a helper return the first member directly while retaining ordinary register-view rules, call clobbers and the requirement for a defined result on every return path.

The fixed C runtime catalog also preserves 32-bit integer carriers and explicitly zero-extended Boolean results. Boolean results use a complete unsigned byte; this does not establish Boolean parameter extension rules. Narrower unknown representations and 32-bit Swift-convention declarations remain excluded. Runtime tests cover successful and failed casts, exact counted retains, and object destruction; calls and their ownership effects remain observable.

Stored-field metadata separates known byte layout from unknown language types. Stable Swift classes use pointer-sized field-offset globals even on arm64; unexposed fields may have an empty Objective-C type encoding. NeverD retains that absence while validating offsets, sizes, alignment, and overlap. Recovered C bodies use runtime ivar lookup for these offsets. An unavailable field type still prevents emitting a class declaration that would require an invented type.

Stable Swift ivar identities may remain valid when their offset slots are runtime-initialized zero-fill storage. Such classes use `ivar_status: "runtime"`; unknown offsets are JSON `null`, and a zero field size denotes a runtime-sized field. Method bodies can query the existing runtime class for these offsets, and declared object types can follow exact offset slots. Literal offsets still require known layout. This does not reconstruct a Swift class layout: standalone class export continues to reject unavailable layouts and field types.

For a directly bound native call, an ivar-offset address can be replaced with the address of a local scalar only when the callee reads that pointer exactly once at entry, before observable effects, and never writes, retains, compares or otherwise uses it. The local is populated by the existing runtime ivar lookup; the native ABI and callee body remain unchanged. This bounded proof accepts flat control flow and rejects uncertain uses or exhausted analysis limits. C API recovery includes `.cxx_destruct`; standalone Objective-C method syntax still cannot emit that selector.

Immutable byte buffers are rebuilt only for imported calls whose validated contract bounds each nonnegative read and excludes writes, retained pointers, and address-identity comparisons. The original bytes and length are preserved; unrelated pointer uses remain unresolved. The exact standard-library assertion failure preserves its compiler-derived Swift scalar and stack carriers, `swiftcall`, `noreturn` effect, and linker spelling; the C reporting shims keep their declared C ABI and a separate following trap. Static strings and immortal String literal storage are copied only for this authenticated, nonretaining terminal consumer. Dynamic or owned String values are not treated as literals.

For an already loaded Mach-O session, `neverd_objc_methods_json(session, max_functions)` and `neverd_swift_methods_json(session, signatures_json, max_functions)` return the corresponding reports. Zero selects all discovered functions. Free successful strings with `neverd_free_string`; `NULL` indicates failure and the session error explains it. These APIs do not load IPA or `.app` containers.

Objective-C native dependency inference may further narrow a `NativeAnalysis` 64-bit integer register parameter to 32 bits after source calls are bound. An exhaustive HighIR use proof must show that every occurrence observes exactly the low four bytes, either through a zero-offset byte extraction or an exact bound integer call argument. The proof is independent for each parameter; full-width, nonzero-offset, malformed, or budget-exhausted uses keep the original width. The refined helper is re-lifted and must pass the ordinary body and dependency-closure checks; the rewrite ABI is unchanged.

Swift lazy static object getters exposed through Objective-C entry thunks can be projected only when the `vgZTo` symbol, runtime method signature, predicate test, `swift_once` call, storage load, and `objc_retainAutoreleaseReturnValue` result form one exact compiler pattern. The `_Wz` predicate, `_WZ` initializer, and `vpZ` storage symbols must agree; the raw third register may occur only as the once context, and the initializer must ignore that context and have no ordinary direct caller. The projection rebuilds predicate and object storage, passes null as the irrelevant context, and keeps the initializer as a checked dependency. Any shape, symbol, parameter-use, or callback drift remains unrecovered.

Swift Objective-C constructor thunks with an authenticated `cfcTo` symbol and the runtime `init` signature can also discard an undeclared third argument register when its sole occurrence is the context of one exact `swift_once` call. The predicate `_Wz` and initializer `_WZ` must match; the callback must ignore the context and have no ordinary direct caller. The projection passes null, infers only the predicate’s eight-byte storage extent, and preserves every other control-flow, memory and call effect; normal source-body and dependency-closure checks still apply.

ARM64 once callbacks can forward an otherwise unused x2 context to one nested `swift_once` when the outer `_WZ` symbol and inner `_Wz`/`_WZ` pair are exact, neither callback has an ordinary direct caller, and an independently typed leaf ignores its context. The same contract controls discovery and projection. Projection passes null and keeps every statement; the void callback ABI permits discarding only pure register/temporary or constant return carriers. Stack reads, loads or calls in return expressions, and unresolved dependencies remain rejected.

## Verification and troubleshooting

On macOS, builds with `BUILD_TESTING` enabled provide `check-neverd-mobile-ios`, which runs all three native recovery suites through CTest.

Python is used only by the development test harnesses below; built-in mobile recovery runs in the native C++20 CLI.

```sh
cmake --build build --target check-neverd-mobile-ios
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_ios_backend.py --neverd build/bin/neverd
python3 scripts/test_mobile_ios_calls_backend.py --neverd build/bin/neverd
python3 scripts/test_mobile_swift_backend.py --neverd build/bin/neverd
```

On macOS, the Objective-C runners compile originals, recover `.m`, and link only generated source with an independent calling harness. The scalar runner covers integer boundaries, branches, loops, pointer reads/writes, hidden parameters, float/double identity bits, mixed parameters, and stack arguments. The calls runner adds message dispatch, inheritance, categories, ivar storage, native helpers, and Block invocation/captures/shared identity. The calls corpus requires 21/21 recovered methods and 134/134 independent expected results for each arm64/x86_64 × classic/default variant. Run these checks against the current native CLI build.

The strict Swift runner checks 22 user declarations, three getter/setter entries, and nine compiler-generated callable entries; none may disappear from the inventory. Each variant has 858 original-program oracle checks. It independently compiles generated `.swift` and its harness, without the original dylib, module, bridge, or handcrafted replacement declarations. Cases include scalar/native calls, class initialization/storage, struct value/mutating methods, floating and stack parameters, pointers, and loops. The native C++20 CLI must pass all four arm64/x86_64 × classic/default variants without skips: each must recover 25 native method bodies plus nine compiler projections, preserve all 34 callable identities, and match 858/858 oracle checks for both originals and independently compiled generated Swift. These results are limited to this corpus and do not guarantee recovery of arbitrary applications or original source text. The runner rejects missing coverage, source compilation failures, and behavior mismatches.

These four variants target macOS. The two additional compiler entries are the empty value initializer and its metadata accessor; native proofs validate them separately, and one `struct Empty {}` source unit retains both identities. Passing this corpus does not qualify a real iOS application.

All three scripts support `--arch all|arm64|x86_64`, `--fixups both|classic|default`, `--timeout N`, and `--work-dir NEW_DIRECTORY`. `--setup-only` validates originals and does not test recovery. Acceptance requires every requested architecture and fixup variant to complete, including the scalar runner; a missing variant or an architecture the host cannot execute fails the run. Skips are not permitted. Use retained failure artifacts to distinguish missing source coverage, compilation errors, and behavior mismatches. Check current test output before claiming verified support.

The [Mobile Real Applications workflow](../.github/workflows/mobile-real-apps.yml) uses public applications pinned in the [corpus manifest](../scripts/mobile_real_apps.json). Its gate requires independent inventories of complete iOS bundles and APK DEX sets, independent rebuilding of originals and generated source, and behavior comparisons. Missing stages, unknown inventory coverage, or missing required cases fail the gate. Real-app recompile and behavior stages remain incomplete, so support retains the Experimental label; passing harness guard tests does not establish real-app success.

Publication is transactional: choose a new output directory, inspect process exit status first, and keep redirected JSON outside that directory. Failures remove staged output and preserve existing output. Nonzero backend exits include a bounded log tail. Backend timeouts preserve the timeout message and append a bounded tail when captured log text is available; budget failures retain their own diagnostics. Successful native CLI runs return zero. Recovery failures return nonzero; `--json` reports handled failures with `schema_version`, `status: "error"`, and `error`. Argument parsing, native executable or library startup failures, and interruptions can instead report on stderr. Consumers must inspect the exit status first.

For an encrypted slice, supply readable input; for an absent architecture, inspect available slices; for omitted methods, read their exact reasons and metadata diagnostics. Increasing `--max-func` helps only functions excluded by the limit. Missing layouts, signatures, external headers, exception support, or unsupported ABI behavior require implementation or additional valid metadata, not a claim of complete recovery. Preserve applicable dependency license notices when distributing tools or generated packages.

Native scalar helper inference also supports float/double register parameters and results. The shared MedIR entry-byte analysis must prove that a wide vector input observes only one low scalar lane. Source-local CONCAT values may be narrowed only when every definition has the same low width, discarded upper expressions have no effects, and every use explicitly reads that prefix. Calls, stores, branch positions and exact non-NaN floating bits remain preserved; unknown upper bits are never supplied as values.

A leaf native helper forwarding to validated external void tail calls may use an internal void source signature when it has no complete scalar result. LowIR must match every bound call and its synthetic return. The leaf proof forbids writes to preserved, frame, stack and link registers; a bounded byte-taint fixed point also rejects storing a stack-derived value or passing one to a call. CFG, body and dependency checks still apply. The signature supplies no result bits: callers that read an unknown result remain unrecovered. Runtime regressions check conditional object destruction, independent caller results and rejection of a result-reading caller.

Void native summaries also support saved-register frames and ordinary calls when a bounded LowIR analysis proves that every exit restores the original preserved bytes, stack pointer and link register. Partial writes, implicit zero extensions, overlapping stores and call clobbers invalidate affected facts. Stores through addresses proven to contain no frame-derived bytes are disjoint from the invocation's private frame and preserve its spill facts; partial or exact frame-derived aliases, frame-address spills and escapes reject the proof. The sole borrowed-frame exception is the exact first argument of a source-bound `objc_msgSendSuper2`: it may name a complete 16-byte `objc_super` object wholly inside the allocated frame because that runtime contract reads it synchronously. Ordinary messages, incomplete pointers, out-of-frame objects and every other frame argument remain rejected. HighIR byte liveness treats a stack-slot value as a bounded read of that slot, while taking or passing its address remains an escape; malformed or overlapping accesses remain conservative. This lets disjoint private stores discard only unread suffix bytes. After re-lifting, auxiliary register parameters with no HighIR occurrence may be removed and the pipeline rerun. Existing HighIR cleanup owns private-store elimination; canonical parameters and actual input uses remain intact.

The arm64 UIKit catalog also binds `UIProgressView`’s `observedProgress` object result and `setProgress:animated:`. The latter preserves the separate `float` and Boolean argument registers. Both declarations require agreement between compiler-produced iPhoneOS and arm64 iPhoneSimulator evidence and the exact system UIKit provider; other architectures remain unsupported.

Tagged literal words may retain a full-width image address from an immutable C-string pool under the exact high-bit tag. Source binding relocates only that proven address into the existing permanent shared pool and preserves the integer OR, tag and interior offset. Scalar or partial address lookalikes, conflicting provenance, mutable or relocated pools and direct memory-address uses remain rejected; this does not infer a Swift String object layout.

`UIProgressView`’s `setProgress:` requires a proven receiver, including a typed property result preserved across an exact ARC identity call. Its verified superclass and protocol closure selects the `float` argument. Unqualified calls remain ambiguous because UIKit and local classes also declare object-valued setters with the same selector.

The arm64 UIKit catalog also records `UIButton`’s `setTitleColor:forState:` with an object argument and an unsigned 64-bit `UIControlState`, plus `UIView`’s void `invalidateIntrinsicContentSize`. Both complete device and simulator ASTs agree. The observed `UIImageView → UIView` inheritance edge lets existing receiver proofs distinguish local object setters from unrelated floating-point setters with the same selector. Unknown receivers, conflicting subclass declarations, missing parent providers and x86_64 remain unsupported.

A dictionary lookup returning unqualified `id` does not gain a receiver class by joining a path that returns a known class from `new`. Every incoming path must retain receiver evidence; even an explicitly typed pointer argument cannot select an object-valued setter over a conflicting floating-point declaration.

Arm64 `+[NSSet setWithObjects:]` retains the SDK’s nil-terminated variadic declaration. The first bounded form requires an exact platform class import and selector stub, plus immutable Objective-C string arguments whose complete eight-byte machine values agree on every incoming path. Recovery stops at the first definite nil; a nil first object needs no stack-tail proof, and later slots are not read. The shared Darwin variadic ABI keeps three fixed parameters and places the remaining objects and nil on the stack. Publication revalidates the declaration, provider, receiver, selector and every argument against the current image. Dynamic objects, missing or partial tail stores, escaped frames, local overrides and other architectures remain unsupported. A stack argument additionally requires a linear private-frame proof at the exact load: later overwrites cannot change the value already read, and overlapping writes, unknown calls, escapes or control flow prevent certification.

Source export can project a compiler-generated BOOL super getter per Objective-C root only when complete current ARM64 instruction, CFG and save/restore proofs authenticate its entry, shared body and metadata accessor (`CMa`). Discovery, binding and rendering use one contract. The helper preserves the actual CMa call and its closed dependencies, then reads the runtime-registered SEL cell and calls typed `objc_msgSendSuper2`; distinct selectors keep distinct cells. The loader rechecks local linkage through LLVM Mach-O symbols/export trie and exact current segment/section mappings. Projection leaves unrelated callers and the shared function’s global native ABI unchanged; missing or stale evidence is rejected.

An arm64 Swift once initializer may copy an object between separate static slots through an already typed native helper. One use contract accounts for every current parameter and every path, preserving predicate read, optional `swift_once`, source load, destination store, `objc_retain` and return in order. It accepts four role parameters or one additional unused input; native inference alone owns parameter removal. Discovery and binding revalidate the same contract, the exact callback and storage symbols, disjoint eight-byte extents and the callback’s unused context against the current image. ABI, control flow, memory effects and dependency checks remain intact; aliases, partial or ordered accesses, extra uses and stale evidence are rejected.

The complete iOS device and simulator SDK declarations also establish `UIGraphicsBeginImageContextWithOptions(CGSize, BOOL, CGFloat)` as a fixed void C call. On ARM64, the two size fields occupy `d0` and `d1`, the Boolean uses the byte carried in `w0`, and scale occupies `d2`. The exact UIKit linker export authenticates the provider. The source keeps the real call and any wrapper counter updates; wrong providers, unsupported architectures, and altered declarations remain rejected. Native wrappers still require independent state and return-value proofs.

The same verified UIKit SDK evidence binds `CGSizeFromString(NSString *)` with its two-double `CGSize` result. The shared ABI keeps both `d0` and `d1`, including separate results that remain live across another call. Current import and signature revalidation rejects foreign providers, scalar substitutes, changed pointer parameters and missing or exchanged result carriers.

ARM64 native frame preservation can read an incoming stack argument when the current inferred entry signature explicitly describes its aligned eight-byte scalar slot. The LowIR load must match that complete slot exactly. These bytes remain unknown input values and cannot establish a saved-register identity or an address inside the callee’s private frame. Stores, partial reads, gaps and undeclared slots do not gain this permission. The source body, every caller’s argument values and the dependency closure still require validation.

The complete device and simulator UIKit SDK declarations also bind `UIAccessibilityPostNotification` as `void(uint32_t, id nullable)`: ARM64 passes the unsigned notification word in `w0` and the object pointer in `x1`. `UIAccessibilityAnnouncementNotification` is external `const uint32_t` storage. Its exact UIKit import identifies the storage address, while the source keeps the original four-byte read and the real notification call, including a nil argument. No notification number is substituted. Wrong providers, weak imports, altered widths or signedness, and changed return contracts remain rejected; unrelated lazy-storage dependencies still require their own proof.

Mach-O generic64 chained rebases preserve the encoded high eight bits in both `DYLD_CHAINED_PTR_64` and `DYLD_CHAINED_PTR_64_OFFSET`. The offset form first reconstructs the complete word, then adds the preferred image base with overflow checking, matching [dyld’s execution](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/common/MachOLoaded.cpp#L771-L792). Loaded bytes retain that complete runtime value. Ordinary code and data pointer ownership checks use the full mapped address; tagged Swift strings require their separate string representation proof. Legitimate mapped high addresses remain supported.

A separate immutable chained-value reader exposes the complete resolved runtime word only for an exact Mach-O rebase slot in uniquely owned, file-backed immutable storage. It rejects unresolved, ambiguous, imported and overlapping fixups. This value has no ordinary pointer identity or permission to copy relocated bytes: a source consumer must independently prove its representation and relocation, including any Swift string tag.

An immutable Swift string once callback can be projected when its complete six-instruction caller, twelve-instruction shared body and three-instruction addressor prove one exact path. The projection preserves both eight-byte stores in order and the real `swift_bridgeObjectRetain` call. It validates the tagged string and rebuilds its address using shared whole-section literal storage, while preserving the destination symbol’s shared storage. Current image bytes, complete pipeline audits, frame preservation, strong runtime imports and the final source body are revalidated; no general ABI is assigned to the shared helper or addressor.

ARM64 Objective-C source recovery supports up to eight caller-proven Swift string comparisons with a raw one-bit result. It verifies the current entry ABI, strong import, exact call occurrence and every consumer before zero-extending the normalized value. Generated C declares the runtime as `_Bool` with `swiftcall` and explicitly converts its result to `uint8_t`; the original masks and surrounding calls remain. Publication repeats the LowIR and pipeline-audit checks and rejects duplicate evaluations or foreign-function evidence. Ordinary byte returns, native-call guesses, patch mode and lift mode do not gain this normalization. Each occurrence must pass its own proof; other raw Boolean calls establish input locations only, without defining any result bytes. Publication matches each surviving evaluation to a distinct current machine occurrence.

The manual Swift String ABI evidence workflow also accepts the fixed `prefix` probe for `String.hasPrefix`. It retains independent Swift and C compiler outputs for both ARM64 iOS SDKs and requires the exact `swiftcc i1(i64, ptr, i64, ptr)` call. The manifest records the probe, symbol, compiler identities and source hashes; collecting evidence does not install a source binding.

The same per-occurrence Boolean proof now supports the exact `String.hasPrefix` import. Xcode 26.5 device and simulator probes establish a four-input `swiftcc i1` call, with the prefix String pair before the receiver String pair. Its `_Bool swiftcall` declaration remains distinct from the five-input comparison helper, and publication revalidates the current import kind as well as each call occurrence. Two prefix evaluations retain their original branch order and effects.

Strongly imported Swift generic single-payload enum tag helpers now have their declared `swiftcall` ABI: 32-bit case and tag values, metadata and callback pointers, a 32-bit getter result, and a void setter result. Weak or foreign imports do not qualify. This binding does not infer undefined upper return bits or the callback body.

The pinned Swift declarations also bind `swift_initClassMetadata2` and `swift_updateClassMetadata2` with five pointer-sized arguments and a two-word `swiftcall` metadata dependency result (metadata pointer, then state word). Only exact strong libswiftCore imports qualify. The generator now reproduces these entries and the generic enum tag entries from the pinned source.

Swift lazy Objective-C class getters that bridge a cached Dictionary or Array to NSDictionary or NSArray have an effect-preserving once contract. Projection requires an exact `vgZTo` class method and Foundation bridge binding, a single `swift_once` whose only x2 use is its context, matching `_Wz`/`_WZ` symbols, and an initializer that ignores context and has no ordinary direct caller. Only that context becomes null; bridge, autorelease, storage reads, control flow, and dependency checks remain intact.

HighC renders signed add/subtract overflow flags with Clang-compatible result-pointer builtins and a disposable result at the original operand width. It reinterprets each input bit pattern as signed even when an intermediate local is unsigned. The generated C for recovered Swift once initializers therefore keeps boundary behavior and compiles under Apple Clang.
