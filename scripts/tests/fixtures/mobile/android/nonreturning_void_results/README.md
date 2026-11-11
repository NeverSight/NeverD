Independently authored anonymous iterator-style implementation capturing the
exact enclosing class formal. Before the JVM baseline, bounded class-file edits
attach its complete generic interface, rename three owned Void methods to their
interface slot names, mark the class final and its original void forwarders as bridges. Code,
descriptors, fields and runtime calls are unchanged by these edits.

Recovery keeps the original static prefix, exception allocation and throw in
distinct Void source aliases. Both wide and narrow callers retain their original
dispatch and result carriers. The void forwarders retain the call-and-discard
body and acquire the fully checked target parameter Signature. Alias names,
bridge flags and anonymous-class source binding differences are reported.
Signature-only parameter drift and an independently authored normal-return
body are counterexamples; neither may use this proof.
