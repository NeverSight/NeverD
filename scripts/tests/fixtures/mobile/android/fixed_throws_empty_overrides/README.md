# Empty throws overrides of fixed interface exceptions

An owned plain `Handler` declares `invoke(Object, Method, Object[]) throws
Throwable`. Its generic abstract static parent implements that method without
declaring exceptions. A static concrete child supplies a typed predicate and
the original javac generic superclass bridge.

The independent JVM harness checks direct, inherited, interface and reflective
dispatch, argument and result identity, ordered parent and child effects,
runtime failures, the original bridge cast, raw heap pollution, arrays and null
receivers. It verifies the interface's fixed Exceptions attribute and every
implementation's empty exception list separately. Complete metadata and method
inventory must remain unchanged apart from reported source helpers.
