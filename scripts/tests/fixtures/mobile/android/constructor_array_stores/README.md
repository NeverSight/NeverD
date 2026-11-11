# Reference array stores before constructor delegation

Owned Java sources overwrite an incoming array parameter with a fresh String array, store an original argument at a variable index and pass both the array and assignment result to a factory before delegating. The variable length and index keep the original new-array and aput-object instructions in release D8 output. No classfile or DEX patching is used.

The frozen observer checks fresh arrays, original input preservation, bounds and covariance failures before null factory invocation, checked and Error identity, opaque or null factory results, and final-field timing and partial receiver escape. Reflection, method handles and a compiled caller each run twice per case. Wrong programs must fail before recovery; source and dependency-provider profiles audit every original definition and compiler metadata record.
