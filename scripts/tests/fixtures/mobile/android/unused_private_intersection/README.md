# Unused private intersection formal

An owned private instance method retains one unused formal with two ordered
interface bounds. All runtime arguments and results remain closed, so ordinary
objects that implement neither bound are valid inputs. An original compiler
accessor reaches the private method, whose virtual provider calls retain their
receiver, opaque result, callback order and exact failures.

The runner compiles every emitted unit and accounts for every original class,
method and Code/static/abstract role. It requires complete generic metadata and
explicitly checks the existing ordinary-accessor projection: only that accessor's
synthetic bit may be lost, with its original identity, visibility and body kept.
This is disclosed separately from runtime equivalence and never permits a
bridge-role change. Independent wrong programs and same-Code bound mutations
qualify frozen callers before NeverD is run.
