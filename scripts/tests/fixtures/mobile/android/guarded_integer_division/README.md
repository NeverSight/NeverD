# Constructor prefix integer division and remainder

Independently authored Java8 constructors retain guarded and linear integer
division/remainder before a superclass call. The recovered source uses Java25
constructor prologues. Owned observers check both original list calls, partial
fields, opaque parent callbacks, overflow, zero divisors, exact exception order
and prefix words still consumed after initialization. No application executes.
