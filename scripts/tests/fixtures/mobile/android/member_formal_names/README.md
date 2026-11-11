# Member and class-formal name collisions

Compile an independently authored Owner<Value extends Owner.Item>, then encode
only class/field/method Signature tokens Value -> Item. Code and all other
attributes remain unchanged. The JVM can distinguish the two declarations;
Java source resolves the member ahead of the class formal inside its body.
Frozen observers retain member binary identity, exact formal declaration and
bound, method-formal shadowing, effects, stores, calls and exception precedence.
A source-only class-formal name is accepted only through explicit scope receipts.
