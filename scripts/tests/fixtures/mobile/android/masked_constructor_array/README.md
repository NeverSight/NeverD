# Conditional array defaults before constructor delegation

Owned Java sources select an original argument or call a factory with a new array inside a constructor's default branch. The resulting object becomes a final field through a delegated constructor. Normal compilation and release D8 produce the original conditional prefix without classfile or DEX patching.

The immutable observer uses reflection, method handles and a compiled constructor call. It checks unused default branches, array allocation before null factory invocation, negative and empty lengths, fresh arrays and mutations, opaque or null results, checked and Error identity, and callbacks before and after the final store. Source and dependency-provider profiles compare every original definition, metadata record and Exceptions attribute. Wrong programs must fail the observer before recovery.
