# Signature quality and performance

Signature growth is useful only when it adds correct identifications without
unacceptable false names, latency or memory cost. Record these separately;
pattern count and raw match count are not recognition rates. Keep stronger
existing packs when supplementing a library, and measure the supplement's
net contribution after selection, reference checks and ambiguity resolution.

## Strict identity and retained candidates

The default policy separates byte candidates from names permitted to drive
analysis. `matches()` and `buildCandidateNameMap()` retain byte, CRC, reference
and ambiguity results. `buildNameMap()` admits only exact spellings independently
stated by the image's function symbols/exports, plus the existing dedicated
exception-personality recognizer's metadata and whole-function checks. A symbol
for one alias does not certify a different alias. Analysis-generated names,
user labels, data/section symbols and a matching name at another address cannot
provide this evidence. Successful byte-pattern reloads/reapplication clear prior
decisions; failed loads leave the previous transaction intact.

This deliberately reduces automatic naming of stripped programs. An isolated
legacy byte pattern cannot recover erased C++ types or prove that the database
contains every function with those bytes. Even a completely confirmed callee
graph is not independent proof of the caller's original source identity.
Correct suggestions remain available for inspection; deferring a false name
is an abstention, not an increase in recognition coverage. The separately gated
library-feature recognizer retains its explicit identity verifier.

COFF source aliases are retained only when function symbols share one section
and offset. Identical bytes at different locations remain separate claims.
Canonical x86/x64 indirect import references can distinguish same-byte wrappers
using the exact export name at a mapped IAT slot. This is positive evidence:
unresolved aliases, ordinal imports, duplicate slots and different spellings
remain unknown, and unsupported prefixed instructions are not guessed. The
slot is data, so reference checking never scans it as a function. Confirmation
still does not establish the caller's independent source identity.
The x64 form also accepts one canonical REX.W prefix, including the indirect
tail jumps emitted for Windows; LLVM describes this encoding in its
[X86 control-flow definitions](https://github.com/llvm/llvm-project/blob/main/llvm/lib/Target/X86/X86InstrControl.td).
Additional address, segment, mandatory or REX prefixes remain unsupported.

For x64 COFF, a canonical RIP-relative address of a primary MSVC vftable
can also supply its exact mangled type. The loader checks its complete-object
locator, self RVA, hierarchy, self base descriptor and bounded type name.
Unreadable or unsupported RTTI remains unknown; a valid different type
contradicts the candidate. These reads are lazy and cached within one match
application. The data addresses never enter the function-reference graph.

The separate `!vtable` record describes a complete primary x64 COFF table,
with its exact linkage name and every source-declared pointer slot. Its bytes
are all wildcards and it never enters the function byte index. The image must
have matching strict primary RTTI, a relocation-backed locator and every code
slot, and one read-only, nonexecuting owner for the complete table. A known
contradicting sibling or conflicting source layout rejects the table. Only
requested executable function entries without settled names are queried, using
complete `!caller` bodies with the existing fixed-byte floor. Multiple matching
nominal alternatives at one slot remain ambiguous. Weak symbols retain their
explicit fallback alternatives; their search rules do not establish aliases.
A matching fallback body is a candidate, not proof that the linker selected
that definition instead of an unknown override. This path never grants source
identity or automatic naming.

Table candidates receive one outgoing-reference check against the frozen prior
matches, even when that prior set is empty. They cannot confirm each other,
restart incoming inference, or establish data anchors. Thus table metadata
cannot bootstrap an unrooted call cycle. The pass reads indexed relocation
slots only, and is skipped when no table records are loaded.
`neverd-sigmaker --vtable-evidence` emits JSONL declarations from x64 COFF
objects, including incomplete counterclaims. Source tables need dedicated
read-only COMDAT storage, zero-addend ADDR64 relocations and exact locator/type
agreement. Unsupported weak chains or ownership remain incomplete. The offline
selector requires all declarations for a selected type to agree before it
publishes a new native record; it never repackages existing function bodies.

The optional NeverD `!refs` token immediately after a pattern's total length
requires whole-function coverage and at least one reference. Every reference
must be confirmed before the hit becomes a candidate or supports another
candidate. Required hits cannot bootstrap each other from raw bytes; a cycle
without independent evidence stays unresolved. The existing bounded withdrawal
algorithm removes dependent claims when their evidence changes. Older parsers
reject this extension, and cache format 5 preserves context requirements, typed data references and
explicit table metadata. Published
constraint packs use `.npat`, which older directory readers ignore while
retaining the ordinary `.pat` packs. New readers apply the same profile and
toolset selection to both extensions.
`neverd-sigmaker --contextual` emits this form for referenced functions,
including those below the ordinary fixed-byte floor, and covers their entire
body. Unreferenced functions retain the ordinary floor. This changes candidate
eligibility, not the independent identity gate.

The separate `!caller` form describes a complete body whose public names all
label the start. These records are indexed by
linkage name and excluded from the global byte index. After ordinary reference
settlement, a caller can request a matching body at its actual direct branch
or supported function-address target. The caller must have a settled candidate,
whole-function coverage and the ordinary fixed-byte floor. Every surviving
variant of that name must state the same complete set of alternatives at the
same site. A site with several alternatives needs complete native bodies for
every alternative and exactly one name surviving body and frozen reference
checks. Missing alternatives or multiple surviving names leave it unresolved.
Ambiguous or partial callers cannot anchor a request. Context-dependent
callers cannot start a chain without a complete ordinary root.
Only requested executable entries without a settled candidate are eligible.
An incoming candidate with complete coverage and the ordinary fixed-byte floor
can extend a rooted chain. Each round visits only its new frontier, up to
`MaxIncomingRounds` in `lib/sigs/ReferenceCheckLimits.def`. A short body needs
the independent callee proof described below before it can extend the chain. A previously named target cannot form a new edge back into the
chain, so unrooted cycles cannot bootstrap it. Every generation checks its own
outgoing references with the same resolver against the frozen prior
result from before incoming inference: contradictions reject it, and missing
evidence remains unknown. Descendants never confirm an ancestor's references
or change its supporting proof. Reaching the depth budget leaves the supported
prefix intact and its unexplored descendants unknown. Multiple
validated incoming names at one target are retained as candidate aliases. Only
names actually requested by those callers participate; matching bytes do not
import additional nominal names from the source pattern. This adds suggestions, never
independent identity evidence or automatic names.
A short incoming body can also extend its rooted chain when every source
variant agrees on a direct call or branch to a separately established ordinary
callee. A short `!refs` root must already be confirmed and additionally have a
complete `!caller` source refinement; independently pruned clauses cannot stand
in for that source. The independent callee must have a settled exact linkage
name, complete coverage, the ordinary fixed-byte floor and no overlapping body
bytes with the parent. Ambiguous, partial, contextual and newly inferred callees
cannot supply this proof. In particular, old nominal rows at an address resolved
by an incoming round do not become independent evidence. Missing source fields
and alternatives at the supporting site refuse the extension. The existing
frozen outgoing validation and round budget still apply. Independent-body
checks reuse the frozen settlement address index. Body strength is evaluated
only at requested targets, and the separate data frontier settles names lazily.
No descendant can repair its parent's evidence or start an unrooted cycle.

A separate finite code-address fingerprint can identify two short functions
together in linked x64 PE. Its parent needs an existing complete `!refs` hit
and matching unpruned `!caller` source records that unanimously state one
canonical RIP-relative LEA to the leaf. Apart from that exact four-byte field,
both complete bodies must be literal bytes, with no CRC span or additional
wildcards; the leaf has no outgoing references. Each body is below the
ordinary fixed-byte floor, but their disjoint combined bytes must meet it.
Both addresses must already be requested executable function entries. All
matching source/leaf variants must agree on their complete extents and edge.
Competing joint identities, overlapping bodies, cycles and contradictions with
frozen names refuse the pair. This is a bounded two-body fingerprint, not
recursive confirmation through guessed names. The ordinary floor, rooted
caller-chain rules and candidate-only identity policy remain in force.
Source templates stay in the name index; no extra global byte scan is needed.

`neverd-sigmaker --caller-context` emits these records, including short bodies.
Generation and matching share the canonical x64 LEA field check. When the
preceding byte could be an instruction prefix, a bounded Capstone walk from
the declared function entry must reach that complete LEA without overlapping
instruction paths. Genuine prefixes, unsupported control, unreachable targets
and exhausted budgets stay unknown. The prefix budget is owned by
`MaxReferenceBoundaryBytes`; unrelated fields keep their existing fast path.
Only the most recent decoded prefix is cached per thread, with exact byte
comparison under that same budget. Reused image buffers cannot retain stale
positive or negative results.
The supplement builder requires exact raw source ownership and keeps distinct
same-byte names separate; they are queries, not claims of byte uniqueness.
When incoming context is loaded, the name index also reuses complete existing
function-reference `!refs` records. Data-dependent records wait for the separate
frozen data pass. Ordinary nominal byte claims are not opted into incoming
inference merely by being loaded. An incoming request supplies
the context for that occurrence; its own reference contradictions still reject
it. No duplicate body is needed just to support both directions of evidence.

Complete ordinary anchors can use supplemental source references missing from
an older pack. All matching source variants must agree on each new field, and
existing alternatives cannot be narrowed. Only refinements used by a matched
child enter the frozen validation pass. Contradicting a refinement removes its
dependent names, including individual aliases; temporary anchor rows never
appear in the results. Source references are intersected across raw versions,
and supplement deduplication respects independently selected library packs.
An ambiguous ordinary body does not cover an audited incoming-context record:
the latter supplies the source ownership needed for a name-directed query.

A long required-reference parent may also start an incoming chain when an
independently settled ordinary callee supplies a complete, separate executable
body. Like a short required root, it needs an unpruned source declaration;
its selected required clause alone cannot establish omitted edges. All source
variants must agree on the relevant sites. The ordinary callee is checked
against the original frozen candidates, so a descendant cannot manufacture
the evidence needed to support its ancestor. Missing or conflicting source
records, weak or partial callees, overlapping bodies and changed bytes leave
the child unresolved. This reuses the short-parent proof and name index. The
initial settlement and frozen body checks share one sorted address index;
missing names from that complete initial settlement do not trigger another
settlement query.

After the single frozen data pass, a newly confirmed data consumer may start
a bounded incoming chain only with complete coverage and the ordinary
fixed-byte floor. Its code references must still agree across every matching
source variant. A complete `!caller` source refinement remains distinct from
pruned `!refs` clauses, even when their union happens to list the same fields.
The continuation reuses the existing name index and round limit; an early
query check avoids settlement work for data consumers without relevant
callees. Every round reads the same frozen data bindings and post-data
candidates. Neither the parent nor any descendant can create another data
binding or trigger another data pass. Short data getters without an independent ordinary callee cannot extend a chain,
and all new names remain candidates rather than automatic identities.

An ordinary caller's ambiguous request can also wait for these frozen data
bindings. Only sites with multiple viable named alternatives and an explicit
data operand are deferred. The existing caller must still meet the complete-body
and fixed-byte rules, and all alternative bodies must be available. After the
single data pass, the matcher revisits only those ordinary parents through the
frozen address index, and only if an actual data operand acquired a binding.
A missing binding or surviving ambiguity leaves the request
unresolved. Neither a deferred child nor a subsequent descendant can establish
data bindings, change its parent or initiate another retry. This also works when
the loaded pack has data-bearing `!caller` records without global `!refs` hits.

The `&offset name` extension explicitly describes an external-data operand in
a complete `!refs` or `!caller` record. `neverd-sigmaker --data-references`
opts into its generation alongside `--contextual`, `--caller-context` or
`--collision-evidence`. Generation requires canonical x64 COFF RIP-relative
MOV/LEA or immediate CMP/MOV operands, zero addends, the exact REL32_N bias,
and external non-function symbols. Bare object-local/static spellings cannot
bind across functions. Code, import and primary-vftable references keep their
existing kinds. A reference site cannot mix data and function kinds.

For source-local data, `%offset anchor_offset:anchor_name` states an exact
same-object relation. The anchor offset is eight uppercase hexadecimal digits;
for example, `%0003 00000013:anchor` refers to the data operand at offset `0x13`
in `anchor`. `neverd-sigmaker --data-references --local-data-anchor NAME`
emits these relations only when the object uniquely defines that typed
external or static function and both canonical, zero-addend operands refer to
the same defined static data address. Source bounds, opcode overlaps, symbol
types and relocation biases must agree. Unsupported or duplicate anchor
definitions leave their raw bodies without that relation, preserving offline
counterclaims. Static variable
spellings never become cross-object identities. A static function's name is
only a query key: its independently matched complete body and exact operand
must still establish the binding. Multiple locations for one key remain
ambiguous.

Only the named independent ordinary anchor can establish this binding, at its
stated operand. Every matching source variant must agree, including variants
missing the relation. Other functions using the same storage cannot create the
binding or feed it back into the anchor. External and local data kinds remain
distinct in parsing, caches, raw collision audits and runtime settlement.
`refine_pattern_metadata.py --local-data` can add source-owned local fields to
existing complete contextual bodies without duplicating those bodies. Every
raw variant must state the field, and all alternative anchor operands are
retained. Alternatives can constrain a consumer but cannot establish an anchor;
ordinary byte records retain their existing admission rule.

`--local-data-layout` additionally admits a checked signed address delta within
one non-COMDAT source data section. For example,
`%000A 00000013-00000070:anchor` describes an address `0x70` bytes before the
anchor's exact operand. The delta is eight uppercase hexadecimal digits with
an explicit sign; zero uses the original syntax. Both source addresses and
complete operands must fit that section. Defined static or external consumer
symbols and signed relocation addends are supported; undefined, weak,
auxiliary, executable, split-section and out-of-bounds targets are refused.
The anchor still requires its canonical zero-addend static-data operand.
Runtime inverts the delta against frozen exact bindings with checked arithmetic
and corresponding mapped-region bounds. Relative consumers never become
bindings. Raw counterclaims with missing or different reference kinds remain
unresolved. The cache wire version is 9;
older cache entries are rebuilt from source patterns.

Only complete ordinary candidates meeting the existing fixed-byte floor can
anchor data bindings. Every matching complete source refinement must agree;
missing fields, alternatives, contradicted refinements and conflicting
names/locations leave the binding unknown. The full memory operand must fit
one readable nonexecuting mapping and, when present, one corresponding section;
zero-filled storage is valid. A candidate cannot supply its own only anchor.
After incoming inference, pending data candidates receive one validation pass
against frozen bindings and candidates. They cannot confirm each other; only
the independently anchored caller rules above can extend their code references. This remains candidate evidence, never automatic naming or
source-type identity. The data pass is skipped without pending hits and uses
the indexed source-anchor names rather than another image scan. Only requested
addresses and source anchors are grouped; frozen reference validation indexes
actually referenced targets and compacts only its new suffix. Previously
validated candidates are neither copied nor reconsidered.

The separate `!layout` record describes one complete non-COMDAT x64 COFF
code contribution. Public offsets retain distinct source functions, including
short getters; aliases belong only to the same source offset. Every byte is
literal except explicitly declared canonical import and data operands, and all
occurrences of a source data key must reach the same mapped storage. The data
keys are local to this observation: they establish no cross-function binding.
A complete independently settled ordinary member with the existing fixed-byte
floor and an exact image import supplies the query. Trailing alignment does
not strengthen that member. The entire contribution needs one immutable code
owner, every named member must already be a requested executable entry, and
existing names must not contradict the source. Missing members, changed bytes,
conflicting layouts, duplicate import slots and unsupported operands remain
unresolved. Multiple matching layouts retain their separate nominal claims.

These records are indexed by strong member names and excluded from the global
byte scan. A name-length filter avoids hashing unrelated long spellings. Their
single final pass reads frozen candidates only; new rows cannot create roots,
confirm callers, establish data bindings or trigger another layout query.
Individual candidate extents stop at the next source member. The independent
identity gate is unchanged. `neverd-sigmaker --code-layout-evidence` records
complete declarations and explicit refusals from source objects; COMDAT and
unsupported contributions never become layouts.
`supplement_code_layouts.py` preserves distinct claims, removes exact existing
duplicates and requires native verification before replacing an output file.
It accepts no evaluation image, map or desired-name list. Cache wire version 9
preserves the layout context and reruns the same native structural validation.

`scripts/signatures/refine_pattern_metadata.py` adds missing import or primary
vftable references to existing complete bodies. Every existing alias needs raw
source ownership, and all matching raw versions participate: an absent field
prevents its addition, while alternative names remain alternatives. The tool
preserves bytes, names and existing fields, writes changed files separately,
and records input/output hashes. Repeat `--name` to restrict a review to named
records; evaluate the changed packs through the normal CLI before publication.
Canonical x64 class/struct ThrowInfo references are also supported. Generation
requires a zero-addend LEA relocation to an external data symbol; a defined
source record must occupy read-only, non-executable storage. Matching validates
the complete bounded image-relative graph and compares its exact type spelling,
qualifiers and catchable count. Unknown flags, scalar/virtual-base layouts,
overlapping or truncated records, malformed descriptors and invalid code
pointers remain unknown. Ordinary descriptor cache storage may be writable;
its spare word must be empty and every type descriptor must share a checked
type-info vtable. No exception object size, function ABI or identity is inferred.
The structural reference is the compiler's
[MSVC exception metadata emission](https://github.com/NeverSight/llvm-project/blob/06788fb19109851e6a6b9f109a2b8027a606f0c6/clang/lib/CodeGen/MicrosoftCXXABI.cpp)
and [linker name encoding](https://github.com/NeverSight/llvm-project/blob/06788fb19109851e6a6b9f109a2b8027a606f0c6/clang/lib/AST/MicrosoftMangle.cpp).
`--caller-code` also permits missing code references in complete `!caller`
records. The same raw-version intersection and kind checks apply; ordinary
byte records and required-reference clauses do not gain function fields from
this option. This can update an existing caller body without another record
or byte-index entry.
`--external-data` similarly adds missing external-data operands to complete
contextual records. Every raw version must own the field with its external-data
kind; absent, function-kind and source-local fields prevent the refinement.
Alternative names and existing fields remain unchanged.

Required context can distinguish claims from independently selected packs,
but the supplement builder still checks every overlapping raw source claim,
including short, ambiguous and previously discarded functions. No missing
rival pack is treated as proof of uniqueness. A mandatory-reference claim can
survive even when two other claims with the same bytes remain ambiguous, but
only if its own references distinguish it from every raw counterclaim.
Missing common fields, overlapping alias uncertainty and ordinary optional
references do not acquire this exception. Pack publication also requires
held-out image checks and index/reference cost measurements.
`neverd-sigmaker --collision-evidence` generates that offline evidence with
complete bodies and without size/fixed-byte floors. Its `!evidence` records
are accepted only as supplement `--evidence` inputs: the matching parser
rejects them, so an unfiltered audit cannot accidentally become a pack.
Architecture filtering and unsupported-relocation diagnostics still apply.
Raw COFF, ELF and Mach-O evidence retains references to the function itself
and its aliases. A self-address constraint must not disappear from a rival
body during collision auditing. Runtime patterns still omit those references;
self references cannot bootstrap a match or certify an identity.
Typed reference kinds survive folding, aliases, complete single-site clauses
and raw collision checks. An incoming-only record does not cover a new audited
required clause, while an existing ordinary claim still does. The offline
selector also treats a shorter complete RET body as a potential counterclaim
to the same body followed only by INT3/NOP alignment: padding is not nominal
identity evidence. It preserves all known counterclaims across independently
compiled source drivers, including wrappers that compile identically.
Byte coverage is compared at absolute function offsets, even when two records
use different leading/tail splits. Wildcards can cover an opaque CRC region;
fixed bytes cannot be proved by that region, and a required CRC still needs
the same span and checksum. The offline collision refiner additionally handles
explicit Thumb BX LR/POP.W-to-PC returns followed by halfword zero/NOP padding
when its machine is ARM. Other architectures do not inherit that rule. Equal CRC spans
and checksums can precede these visible returns. The relation still rejects
different CRC spans or values, opaque returns/padding and any nonpadding
suffix; it never treats a checksum as the literal return bytes.

`scripts/signatures/refine_colliding_patterns.py` applies newly collected raw
counterclaims to existing ordinary records. A complete conflicting record gains
mandatory references only when they distinguish every new rival; otherwise it
becomes an incoming-only query. Incomplete conflicting records are omitted.
Each proposed single-site clause is checked against every raw rival and the
existing byte cohort. The refiner retains one sufficient image-owned
import/RTTI clause, preferring fewer alternatives and source field order;
alternatives at one site stay together. Call-only conditions retain every
original site: a callee name can itself be an ambiguous byte claim, so keeping
only one call can create an unsupported survivor. This replaces one global byte
record with at most one, avoiding a copy
of the whole body for every sufficient site. If several sites are jointly
necessary, the full clause stays.
Pruned clauses retain a separate complete incoming declaration, intersecting
outgoing fields across raw same-name versions. Unrelated unresolved calls need
not block a distinguishing clause, and omitted fields cannot be used as if the
pruned clause declared them.
Existing same-library byte cohorts remain intact across numbered parts only
when their typed references also agree with the raw claim. A known name with
different references or a claim in an independently selected library cannot
hide a new counterexample. Existing required-reference records undergo the
same audit; newly indistinguishable complete ones retain incoming queries. An existing
mandatory clause can be reduced only when its exact names, bytes and typed
references have a complete unpruned source witness, and the selected clause
is an image-owned import/RTTI anchor that still distinguishes every covered
counterclaim, including already loaded required claims. Existing call-only
constraints remain unchanged. The full source declaration
remains available for incoming queries; jointly necessary fields stay together.
The refiner adds no names
or aliases and preserves incoming queries and metadata constraints.
When one member needs refinement, the whole loaded byte cohort is rechecked
together across numbered parts. Removing an alternative cannot leave an
unqualified survivor that newly appears unique.
Its report records actions and input/output hashes. A withheld candidate is an
abstention, not a recovered function. Publish coverage losses and gains alongside
error changes, and keep the independent identity gate unchanged.

`neverd_sig_matches_json` retains `name`, `aliases` and `confirmed` as candidate
fields. Its `caller_confirmed` field identifies incoming reference support;
those names take precedence over unresolved byte-only alternatives. Its additive `auto_name` field reports the address's admitted spelling,
or an empty string. `name_evidence` is `image_symbol`, `exception_personality`,
or `none`; a candidate without an admitted name reports
`candidate_reason=independent_identity_missing`. Every row at one address
reports the same decision, even if its own candidate disagrees. `confirmed`
alone never authorizes a name. The CLI marks candidates explicitly and the
worker and call graph no longer classify them as library functions or stop
traversal at them. C API match counts
continue to count candidates, not automatically named functions.

The probe evaluator reports automatic and preferred-candidate accuracy
separately. It reads `auto_name` directly for current engines, labels historical
responses as `legacy`, and rejects mixed or inconsistent admission responses.
Keep both correct and wrong candidate suggestions visible when reporting a
policy change. Map/truth files are evaluation inputs only, never supplied to
the tested `--no-debug` matching process.

For comparison, the [FLIRT design](https://docs.hex-rays.com/user-guide/signatures/flirt/ida-f.l.i.r.t.-technology-in-depth)
uses byte/CRC/reference discrimination and explicit collision handling, while
acknowledging future false positives. [Binary Ninja WARP](https://docs.binary.ninja/guide/warp.html)
separates automatic matches from possible functions for user selection; its
[matcher](https://github.com/Vector35/binaryninja-api/blob/dev/plugins/warp/src/matcher.rs)
accepts a unique nontrivial candidate and otherwise requires sufficient unique
constraint support. [Ghidra Function ID](https://github.com/NationalSecurityAgency/ghidra/blob/master/Ghidra/Features/FunctionID/src/main/doc/fid.xml)
uses scores, parent/child evidence and per-record mitigation, and documents the
false-positive/false-negative tradeoff. These are design references, not
comparative benchmark results. NeverD chooses the stronger identity boundary
because names also affect type reconstruction and semantic analysis.

## Acceptance evidence

| Dimension | Evidence required |
| --- | --- |
| Coverage | Correctly named library addresses divided by independently known library addresses; report results by platform, architecture, toolchain and optimization mode. Keep unknown and disputed addresses separate. |
| Accuracy | Compare final settled names and aliases with linker maps, retained symbols or independent compiler artifacts. Include unrelated programs, same-byte wrappers, changed constants, relocation targets and missing dependencies. Do not train exclusions on the evaluation set. |
| Time | Separate source reading, text parsing or validated cache mapping, index construction, matching, reference resolution and total analysis. Compare cold-process and repeated-use runs with the same worker count and input addresses. |
| Memory | Record process peak RSS and selected source bytes/modules. Include a large negative workload as well as positive matches. |
| Provenance | Retain engine/build identity, signature revision, input/truth hashes, selected pack hashes, options and every timing sample. State which stages and platforms were not measured. |

An added pack must not introduce a known false name. A matcher optimization
must retain its exact ordered matches unless changing recognition behavior is
the explicit, independently tested purpose. Existing false names must remain
visible as baseline defects; unchanged output is not proof of correctness.
Do not compensate for speed regressions by silently dropping uncertain target
profiles, weakening byte verification or choosing an arbitrary ambiguous name.

Use held-out programs and toolchain builds to evaluate new coverage. A probe
built from the same library used to generate patterns is a compatibility test,
not evidence of generalization to other builds. A fair comparison with another
tool needs the same stripped inputs and truth, documented function discovery,
enabled databases, options and analysis stages. An unconfigured database is
not a meaningful recognition-rate baseline.

## Matching work

The existing pipeline selects compatible packs before pattern I/O, builds a
sparse leading-byte index, verifies masks/CRC/tails, then checks references and
settles aliases. Parsed-file caching and one index per loaded database already
avoid repeated text parsing and per-segment index construction.

Index construction preserves every decision and candidate order while sharing
one immutable candidate permutation across its leaves. Index copies retain that
storage; rebuilding one cannot invalidate a retained copy. Shared prologues are
examined together during construction instead of recounting every module at
every uniform byte. The resulting nodes still check each stated byte, including
the common prefix. Wildcards, short input bounds and final verification retain
their meaning.

Packed batch matching now reuses a CRC for candidates at the same input address,
leading width and CRC length. Changing the input address or leading width
invalidates that reuse. Bounds and leading-byte checks run before a checksum
request; tail and reference checks retain their previous meaning. The cache
uses 544 bytes for values and validity bits plus bookkeeping per active entry,
independent of library size, with no heap allocation or cross-image state.
The value-form scanning APIs retain direct checksums because their external
callbacks can modify the input between candidates.

The optional `SignatureMatcher::ScanStatistics` overload reports entries
inside the supplied segment, candidate checks, CRC requests, actual CRC
computations and bytes checksummed. It resets on each call and sums worker
blocks deterministically. Ordinary callers retain the existing overload and
do not record counters. These counters measure work, not naming confidence.

`SignatureMatcherCRCReuse.*` checks every nonzero byte-sized CRC length,
wrong CRCs and tails, short inputs, nonstandard leading widths, modified
input buffers, mutating callbacks, value/packed forms and sequential/parallel
agreement. Its
growth case requires 1,024 candidates sharing a 63-byte CRC window to perform
one checksum over 63 bytes, while still rejecting all wrong CRCs.

## Reproduce measurements

Build Release targets `NeverDSignatureTests`, `neverd-signature-bench`,
`neverd-signature-context-bench` and `neverd` using the repository build workflow. Never use Debug timings.

```sh
build-release/bin/NeverDSignatureTests --gtest_filter='SignatureMatcher*'
build-release/bin/neverd-signature-bench program.exe --sig-base signatures \
  --entries function-vas.txt --threads 1 --index-repetitions 7 \
  --repetitions 21 > matching.json
build-release/bin/neverd-signature-bench program.elf --sig-base signatures \
  --stride 16 --threads 1 --repetitions 21 > dense-scan.json
python signatures/scripts/evaluate_probes.py \
  --neverd build-release/bin/neverd --signatures signatures --auto \
  --report naming.json program-x64-probe.exe
```

`function-vas.txt` contains one hexadecimal VA per line. Entries are sorted
and deduplicated; a nonempty file is required. The stride alternative measures
dense candidate scanning, **not** discovered function coverage. The naming
evaluator requires the corresponding `.map` or `.truth.json`; use the normal
`neverd sigs --auto --json --no-debug` path for final naming accuracy.

The benchmark emits one JSON object with selected pack hashes, the loader's
input hash when available, a hash of the sorted entry VAs, raw ordered-hit
SHA-256, stage timings and work counters. `scan_cpu_ms` records process CPU
time, including worker threads; it is zero where the platform cannot report
CPU time. Address/module-index pairs and entry
VAs are hashed as little-endian 64-bit values. It discards one warmup scan and
records counters in a separate untimed scan whose results must agree. Peak
RSS is recorded on Unix/macOS and is null elsewhere. The benchmark parses text
without the persistent cache; it does not measure cache mapping, function
discovery, reference resolution or final names. Measure those separately with
the normal CLI/SDK. Peak RSS covers the whole benchmark process, including
source buffers and hashing, not just the matcher.

`--index-repetitions` constructs a fresh index each time, discarding its previous
storage outside the timed region. It records every build, including the first,
in `index_samples_ms` and `index_samples_cpu_ms`; the existing `index_ms` field
remains the first wall-time sample. `index_sha256` fingerprints decisions and
candidate order independently of their storage layout, and must agree across
builds. Hashing is outside the timed region. These are fresh index allocations
in one process, not cold-filesystem or cold-process measurements.

`neverd-signature-context-bench` measures complete application and candidate-name
settlement with both selected databases resident. It alternates before/after
order, discards each database's first application, and checks name digests on
every repetition. Loading/cache work is reported separately; timed application
includes reference resolution and context passes. For example:

```sh
build-release/bin/neverd-signature-context-bench program.exe \
  --before baseline-signatures --after selected-signatures \
  --entries function-vas.txt --threads 4 --repetitions 51 > context-pairs.json
```

Use the same tree for both roots as a control. Peak RSS includes both resident
databases and cannot attribute memory to either variant. Pair fresh CLI
processes to assess startup and per-variant memory, and state whether page and
signature caches were flushed. This benchmark does not measure function discovery
or establish candidate correctness; retain the independent CLI truth check.

Retain each build's executable and JSON. Alternate old/new runs, repeat with
one worker and production worker counts, report medians and spread, and avoid
interpreting scheduler noise as a speedup. CPU affinity helps on machines
with mixed performance/efficiency cores. Compare hashes, module counts and
entry sets before comparing timing; when packs change, separately attribute
the change in work to added coverage. Use the deterministic unit-test work
bound in ordinary CI instead of a fragile universal millisecond threshold.

The [2026-10-24 measurement record](evidence/signature-matching-2026-10-24.json)
retains paired samples, corpus/truth hashes, build limitations and naming
baselines. Four MSVC probes reduced checksum computations by 58–84% and median
matching CPU time by about 8–26% with one/four workers in the final sweep.
Host load caused substantial timing variation during development, so these
local measurements do not guarantee a latency improvement for every input.
Text loading and reference resolution are not covered by those speedup numbers.

## Remaining recognition gaps

The current matcher does not provide a general instruction-normalized
function fingerprint database. Ghidra's pinned [function hasher](https://github.com/NationalSecurityAgency/ghidra/blob/164003036607e669226647bd0f7bedaf281e56a6/Ghidra/Features/FunctionID/src/main/java/ghidra/feature/fid/hash/MessageDigestFidHasher.java)
maintains differently normalized hashes, and its [program seeker](https://github.com/NationalSecurityAgency/ghidra/blob/164003036607e669226647bd0f7bedaf281e56a6/Ghidra/Features/FunctionID/src/main/java/ghidra/feature/fid/service/FidProgramSeeker.java)
reuses function hashes and considers caller/callee relationships. These are
research references, not copied code or evidence of comparative accuracy.
Any additional fingerprint stage in NeverD needs architecture-owned decoding,
explicit unsupported-input handling and independent collision validation;
a similarity hash alone must not establish a concrete function identity.
Prefer a bounded fallback for unresolved candidates that reuses existing
decoded instructions, rather than decoding every function again for each pack.

Exact bytes also do not prove a C++ template type or owning class when code
is shared or relocated data references have been masked. A local four-probe
MSVC MAP audit found 24 existing final-name discrepancies and library-address
coverage of about 62–71%. These are limited compatibility measurements, not
a broad product score. The CRC optimization preserves those results; it does
not repair identity ambiguity. Data-reference/type witnesses and held-out
negative examples are required before admitting more specific names.

Some source identities are erased completely. The UCRT `_time64` and its
`common_time` template have distinct source declarations with identical complete
bytes and import references. Two independently linked stripped PE fixtures can
retain exactly the same complete image bytes while their source linker maps
name different declarations. More bytes, normalized hashes or a different
preferred spelling cannot reconstruct that erased name. A source-backed caller
or independent symbols/exports/debug evidence can add information; without it,
keep nominal alternatives unresolved. Nonsplittable contribution layouts do not
supply fixed offsets between separate COMDAT sections.

Other unqualified areas include a common cross-tool held-out corpus, broad
kernel/mobile/legacy-library truth coverage, and persistent index startup
cost. The current file cache stores parsed data, not the decision index.
Investigate those costs and coverage gaps with the measurements above before
adding another database format or claiming a general recognition/performance
lead.

## Reference evidence withdrawal

Reference checking must withdraw evidence as well as discover it. A candidate
whose own calls contradict its identity cannot keep confirming callers through
its original byte hit. That invalidation is specific to the module and input
address: another viable version of the same named routine can still confirm a
reference. An ambiguous target's raw-pattern availability can change even when
its settled name stays absent.

Callers rejected using provisional evidence are reconsidered when that evidence
changes. Otherwise a short-lived name at one level can permanently discard a
correct alternative higher in the call graph. Checking still uses a worklist
of affected callers, rather than rescanning the database after every round.
Small proposal groups use inline storage. Changed addresses are sorted and
deduplicated in contiguous buffers, and a reusable queue with one byte per
match admits each dependent candidate at most once per round. Its length is
bounded by the match count, independent of how many references reach it.
An unchanged settled name does not requeue callers merely because an unused
raw pattern disappeared. Single-public-name hits also bypass temporary name
grouping allocations; multiple names retain the existing alias ordering and
intersection rules.
The bounds remain in `lib/sigs/ReferenceCheckLimits.def`. If propagation reaches
the round limit, all proposals at the unresolved addresses and their dependent
callers are suppressed; an unchecked alternative cannot win by elimination.
This conservative fallback runs only when work remains at the limit.

`SignatureDBReferences.*` covers withdrawn raw hits, recovery through multiple
call levels, a surviving same-name version, unresolved cycles with unrelated
functions, and 4,096 callers checked with one/four workers in different entry
orders. The synthetic regressions establish the evidence rules independently
of any particular library name or evaluation address.

The [reference-check measurement record](evidence/signature-references-2026-10-24.json)
compares eight MSVC compatibility probes. The VS2019 x86 debug probe gains
26 correctly named library functions (996 to 1,022 of 1,800); none of the eight
loses a correct name or acquires a new wrong name. Existing naming errors remain
visible. In the paired one/four-worker measurements, seven probes reduce median
CPU time for matching, reference checking and name settlement by about 0.8–4.2%.
The debug probe that gains names needs about 1.2–1.3 ms more CPU time in that
stage (about 8%). End-to-end wall timings varied substantially with host load;
the record retains that uncertainty and does not claim a universal speedup.
These results do not establish generalization to held-out programs or
comparative accuracy against another tool.

## Identity-admission measurements

The [2026-10-25 identity record](evidence/signature-identity-2026-10-25.json)
compares the strict policy with that earlier automatic-name policy on the same
eight stripped MSVC probes. All ordered byte/reference candidates remain equal.
The earlier 13,670 correct names and 66 wrong names remain preferred candidates;
none becomes an automatic name because these inputs provide no independent
identity evidence. Automatic coverage is therefore zero on this corpus, and
automatic precision is undefined. The change prevents those false names from
driving analysis; it does not recover more identities or repair the candidates.
Separate SDK cases retain exact export identity and the dedicated personality
recognizer, and reject data, analysis-generated and user labels as evidence for
unrelated signature names. A call-tree regression also verifies that candidates
cannot suppress traversal as library code.

Paired warm matching, reference checking and candidate settlement used the same
database, addresses and executable, with one/four workers. Median CPU changes
ranged from about -1.0% to +3.3%; both raw-match and candidate-name digests agreed
in every pair. Full-command measurements were less stable: a four-worker x86
CPU increase repeated, whereas the initial ARM64 increase did not. A separate
same-binary diagnostic localized the larger x86 increase to the unchanged
signature-load/discovery phase; name refresh became cheaper and matching did
not reproduce that increase. The record retains every trial, the extra JSON
cost and the phase controls. Concurrent builds prevent an end-to-end latency
guarantee; an idle-host qualification remains necessary. No pattern was removed
or verification weakened to obtain these timings.

Source runtime pointer metadata (`!loadcfg`) is a separate candidate context.
Loader-owned configuration roles establish pointer storage; source declarations
establish the proposed default and an explicitly typed function body. A short
body is eligible only with complete bytes and every wildcard covered by a
canonical operand whose storage is independently bound. No ordinary strength
threshold is reduced. Source conflicts, mutable or unrelocated slots, unresolved
operands and contradictory known defaults remain unknown. Source metadata and
matching bytes do not by themselves grant automatic naming authority.
