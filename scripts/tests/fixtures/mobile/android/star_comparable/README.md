Two arguments share one method formal with a single Comparable<?> interface
bound. The comparison body preserves identity shortcuts, null ordering and an
original raw compareTo call with its original bridge/type failures. A public
facade inherits the package-private declaring class's static method; calling it
must initialize the declaring class and leave the facade uninitialized.
