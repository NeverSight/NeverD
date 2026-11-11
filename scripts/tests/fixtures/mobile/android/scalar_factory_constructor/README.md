# Scalar-configured pair factories before constructor delegation

Original constructors pass incoming boolean, mixed integer/boolean, or a
canonical false default through a same-name independently scoped static pair
factory before selecting `this(Box<K,V>, Function<V,K>)`. A mutable field owns
the opaque factory result. The complete factory contract and selected
constructor contract retain separate scopes and predict no stored values.

Source and SDK profiles preserve every original compiler entry and metadata
contract, compile every recovered Java unit at the reported release, and run
frozen direct, compiled caller, reflection and method-handle observers. They
check configuration snapshots, null/invalid/replaced factory results, partial
final fields, exact callback/failure identities, effects and late casts.
Wrong programs and Code-preserving Signature controls protect the contract.
Recovered prologues require Java 25; originals compile at release 8.
