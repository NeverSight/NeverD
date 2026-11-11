# Final assignments between ordinary control-flow regions

An owned constructor writes one final field, selects another final value in two
exclusive arms, then executes an ordinary loop and switch before writing its
last final field. The switch has an original throwing default. Every callback
observes the same partially constructed object, including the fields assigned
before an exact checked or unchecked failure.

The middle assignments need structured source while the surrounding operations
retain their original control flow. Source and SDK-interface profiles compare
all original identities, complete metadata and Exceptions, compile every Java
unit and run a frozen independent observer. Wrong programs must fail before
NeverD runs; final modifiers and all original effects must remain intact.
