# Static final monitor stages

Independently authored Java 8 fixture with one synchronized region between
static final assignments. D8 debug mode retains the linear cleanup layout;
control-flow adapters outside this linear release profile stay unsupported.
The original lock identity and both release paths
must survive recovery. A frozen observer checks held-lock state inside and
outside the region, array identity and bits, partial preparation defaults,
exact checked/Error initialization failures and cached failures. A separate
daemon thread must acquire the lock after each successful or failed attempt.
Source and SDK-provider profiles compare every original definition and
supported declaration, compile every emitted Java unit and reject independent
wrong programs before recovery.
