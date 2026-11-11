# Anonymous superclass token result

This authored fixture calls a protected `Class<?>` method on a nongeneric
superclass from an anonymous class that captures its host. The override keeps
its prefix, null-input branch, exact superclass dispatch and suffix. The parent
declares `ClassNotFoundException, IOException`; the override declares
`IOException, ClassNotFoundException`. Both original orders must survive.

The observer checks receiver, argument and captured-host identity, call count,
the token read before and after mutable callbacks, branch-result snapshots,
and exact checked or runtime failure identity. Each fresh JVM executes two
attempts. Deliberately incorrect source programs prove these observations can
detect changed effects, dispatch, result timing and exception wrapping.

Source and SDK declaration-provider profiles compile every recovered Java
unit. Class and method accounting uses the reported anonymous-class name and
enclosing method, explicitly checks the additional implicit host constructor
parameter, and retains all metadata except the documented capture-field
synthetic flag and local class name. Invalid Signature/Throws controls change
only constant-pool metadata text and retain every Code byte.
