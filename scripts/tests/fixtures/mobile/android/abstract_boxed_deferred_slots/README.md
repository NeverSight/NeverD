# Abstract boxed results with a deferred plain slot

This authored sibling of `abstract_boxed_generic_results` keeps `hasNext()` only
on `Child`. `Core` inherits the abstract requirement without declaring a method
or changing the original class/interface ownership. All scalar, typed/erased
boxing entries, factories, method-owned scopes and exception paths stay intact.

Run the shared verifier with `--abstract-owner --deferred-plain-slot`, with or
without `--dependency-root`. The immutable harness checks the original abstract
owner has no `hasNext` declaration and the concrete child owns that method.
It compares the unchanged 246 values, 804 identities, 636 states and 42 failures,
including inherited default callbacks, effects, aliases, nulls and exceptions.
Complete metadata and inventory checks allow only the disclosed boxed source
argument, final typed source name, bridge flags and private helpers.

A same-Code private child provider fails recovery, and an independently authored
private-provider source fails javac. Native controls also reject missing,
static, abstract, method-generic and incompatible nearest declarations.
