# Independent generic constructor choice with a collection input

A Java reference and independent ASM Core expose the same generic API. The
bytecode allocates one Widget before a null branch, then selects its no-argument
or Collection constructor. Input and allocated types remain distinct. The class
and collection constructor retain complete original generic metadata. Frozen
JVMs compare both originals and all recovered units across class initialization,
sticky failures, partial final stores, input/result identity and handler failures.
The collection observer rejects invented traversal. Both branch polarities and
source/SDK declarations are available; no compiler original is rewritten.
