The Java program is an independent executable oracle for a synchronized update
with two nested finally actions on exceptional paths. The smali input is authored
separately before recovery; no javac or D8 instructions are rewritten.

The frozen harness checks every cleanup failure mask, RuntimeException and Error
identity, callback/store ordering, partial allocation, lock ownership during each
cleanup action, release on exit, and nested reentrant failures. Existing concurrent
and split-cleanup observers also run unchanged. Source and SDK-provider layouts
must retain every original definition and metadata item and compile all Java.
