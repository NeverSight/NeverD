# Independent private seeded-result fixture

Java 8 declares two independent Object-bounded method formals on one private
member: a direct seed and consumer share S, while a producer and result share R.
The callback has a complete fixed Token tail. The provider body calls both
opaque harness implementations and returns their actual runtime result.

The immutable observer checks private/final flags, the full generic relation,
receiver/seed/result identities, repeated snapshots, nulls, callback and provider
failures, and the caller's checked, raw and discarded result paths. Legal wrong
programs change seeds, results, callback counts and cast timing. Both instance
and static private members are tested with source and SDK API declarations.
The input and frozen callers are never rewritten from recovered output.

The `--producer-only` profile independently authors one result formal, a closed
binary callback, an Integer argument and a nested covariant list argument before
compilation. The unchanged public callers still cover 80 fresh JVM cases with
two attempts. Reflection checks the complete producer/result and closed input
trees. A callback mutates the global auxiliary inputs after the original call
captures them; the private method must retain their earlier identities. Eight
compiled wrong programs expose late argument reads as well as callback, result
and cast changes. Source and frozen API profiles cover both private instance
and static dispatch; the instance's six invocation words require the original
D8 range form.

The `--producer-only --public-helper` profiles author the same producer and
complete closed auxiliary contracts on a public final method before javac/D8.
Reflection checks its original public flag; the DEX witness requires virtual
range dispatch for the instance and static dispatch for the static profile.
The frozen observers retain all 80 cases, exact failures, captured references,
later checked/raw/discarded results and eight independently wrong programs.
Source and dependency API profiles compile every recovered Java file and
compare all original definitions, generic metadata and Exceptions.

Adding `--scoped-consumer` authors a callback consuming that same result formal
contravariantly. The original Java body explicitly casts its Object seed to R;
R's Object erasure introduces no checked conversion. The independent observer
checks that lower bound, original seed/result identities and all eight wrong
programs before and after recovery. This also covers closed nested arguments
beside a consumer bound by a distinct covariant producer.

# Reifiable auxiliary profile

`--producer-only --public-helper --reifiable-auxiliary` authors a closed
`List<?>` auxiliary before compiling the input and frozen observer. It retains
the producer's formal result, exact reflection contracts, opaque auxiliary
snapshots, callback effects, later checked/raw/discarded results and original
failures. The ordinary and SDK profiles compare every definition, metadata and
Exceptions; eight wrong programs must fail the same observer.
