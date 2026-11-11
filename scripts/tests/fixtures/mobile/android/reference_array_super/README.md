# Reference arrays before generic superclass construction

Owned Java 25 inputs build a raw runtime reference array, construct each distinct
generic subclass in an integer loop, and pass the unchanged array and receiver
to a fully parameterized superclass constructor. All provider classes are also
compiled with Java 8 before freezing so the SDK route reads their exact original
declarations. The source constructor stays Java 25 in both routes.

Frozen direct, reflection and method-handle observers compare zero/one/multiple
iterations, negative lengths, null receivers, element initialization, partial
constructor failures, original identities and array mutation. Signature-only
mutants and independently compiled wrong programs challenge the proof and oracle.
