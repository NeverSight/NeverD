A genuine Kotlin parent compiled with JVM defaults and a generic compatibility
child compiled separately produce an inherited boolean default accessor. The
child keeps its unused own formal; the parent keeps its fully closed
Function1<? super Parent, Boolean> contract and has no compatibility accessor.

Frozen external callers select the static accessor, DefaultImpls entry and a
qualified default control through reflection and MethodHandles. They distinguish
lexical default calls from receiver overrides and check true/false values, null
boxed results, null callbacks/receivers, aliases, exception identity, partial
trace effects and repeat calls. Source and SDK-trace profiles compile every
emitted Java unit and compare all definitions and supported metadata/Exceptions.
Only the exact recorded source filenames, D8 CLASS annotations and reported
synthetic-accessor/helper differences are permitted.
The reported interface private helper requires Java 9: the runner checks and
records the exact class major-version change from 52 to 53 separately. The
original Kotlin program, observers and DEX bytes remain frozen.
