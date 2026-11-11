# Protected consumers with mixed array parameters

Eight final classes specialize Base<T> with one primitive array kind each. Their
protected write(Token, T, int) callback records the original inputs, checks an
original failure, updates the token and stores a typed value into the array. The
input encoding changes only each original compiler bridge's protected access to
public; Code attributes remain unchanged.

The immutable caller and harness compare inherited, typed, reflected and
method-handle routes. They check aliases, changed and unchanged array contents,
null receivers/arguments, out-of-bounds stores, original target exceptions and
cast ordering. Floating stores include negative zero. Wrong primitive array
kinds, reference arrays and rank changes must fail before any target side effect.
Native cases additionally check copied casts, reference/multidimensional arrays,
wide argument words and malformed incoming registers.

The source profile has ten classes and twenty-eight original methods; Base and
Token as immutable dependencies leave eight classes and twenty-four methods.
Every original runtime entry and all class/method/Exceptions metadata are checked
outside the reported typed-access changes. Same-Code counterexamples alter the
array rank or element, or remove a bridge synthetic flag; independent javac also
rejects incompatible specialization.

The JVM matrix checks 10,384 values, 3,656 identities, 4,704 states and 4,608
ordered failures across all eight primitive element types.
