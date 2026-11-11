# Static field reads before superclass construction

This extends the repository's owned getter/inner-member fixture with integer,
wide and reference static reads. Independent Java 25 and smali bodies preserve
the outer capture before class initialization and superclass construction.

The superclass changes every static field after the reads. The oracle checks
that the constructor retains the original snapshots afterward, plus reference
identity, wide values and the exact class initialization trace. A fresh JVM
also tests initializer failure identity and failure on a subsequent access;
the superclass must have no effects after either failure. The existing member
modifier projection remains explicit. Null outer construction is not claimed.
