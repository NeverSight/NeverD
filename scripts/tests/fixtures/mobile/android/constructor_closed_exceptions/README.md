# Closed constructor exception contracts

This authored fixture covers a captured anonymous class whose original
constructor has no Exceptions attribute while its exact superclass constructor
declares IOException. The runner removes only that anonymous constructor's
Exceptions attribute after compilation, verifies unchanged Code and all other
bytes, and retains the original artifact separately.

Ordinary derived constructors retain ordered exact and wider closed exception
declarations. A separate overload declares an unchecked exception while its
derived constructor declares none. Unrelated method generic metadata ensures
the exception proof is independent of an input-wide metadata switch.

The fresh JVM observer checks the capture during superclass initialization,
argument and result identity, stores on partial objects, ordered callbacks,
exact checked and unchecked throwable identity, and later capture updates.
Source and declaration-provider runs compile every emitted Java unit. The
anonymous class rename, captured field flags and newly explicit checked
exception declaration are audited against the recovery report.
