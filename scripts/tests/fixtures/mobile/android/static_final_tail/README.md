# Static final cleanup tail

Independently authored Java 8 fixture for static final assignments after a
try/finally cleanup. D8 places an empty alignment instruction after the cleanup
rethrow and before an array payload. The original throw, field initialization
order, array identity and contents, and class initialization failures must survive
recovery. The frozen observer uses an isolated class loader for each attempt and
compares reflection, method handles and direct access, including repeated active
use after success and after failure.
