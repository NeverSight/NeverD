# Scalar interface super forwarding

Independently authored Java 8 methods forward one float unchanged to an
inherited interface default and return its actual integer result. Api has a
diamond with one most-specific default; both an abstract owner and a final owner
also implement a second direct interface. A child override tests that lexical
interface super dispatch does not become virtual dispatch or recursion.

A witnessed class-file edit changes only the method name and bridge flag,
retaining every original Code attribute, descriptor and other attribute. Frozen
ordinary callers enter through an unchanged facade; reflection and method handles
use the explicitly disclosed source name. They check raw float bits including
NaN and signed zero, receiver identity, effects and exact failures. Four authored
wrong programs must fail those observers. Owned and dependency Trace variants
compile all recovered Java and account for every original entry, metadata field,
name projection, bridge flag difference and generated helper.
