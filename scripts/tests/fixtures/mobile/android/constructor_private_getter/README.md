# Private getter before constructor delegation

Independently authored Java 25 constructors select either an explicit value or
a private same-owner getter on a preexisting static receiver, then delegate.
The getter snapshots its value, changes the static receiver and may fail before
or after that mutation. Frozen external observations retain receiver identity,
partial field stores and exact default-class/getter/target failures.
Boolean/int result profiles each run with source/SDK Defaults. Both original
and version-only D8 inputs must execute with unchanged Code and attributes.
All Java, original entries, metadata and Exceptions are checked. No private
APK code is part of this fixture.
