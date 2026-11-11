# Scoped raw interface parameters

Core<T extends Bound> implements a generic interface diamond while its original
matches(Bound) method has no method Signature. Javac generates the original
matches(Object) checked forwarding bridge. These are ordinary compiled Java
inputs; the positive fixture needs no class-file patching.

Source and immutable interface-JAR profiles retain eight classes/13 methods and
four classes/11 methods. The separately compiled harness and frozen caller use
seven routes, a recovered source caller, reflected metadata, and an external
subclass overriding the nominal target. They compare 73 values, 156 identities,
161 states and 56 failures. Wrong types fail before target effects; virtual
bound reads, target failures, null handling and copied object identities remain
observable. Full original inventories, metadata and Exceptions are checked,
with independent expectations for the four throw helpers.

Negative class-file encodings keep Code unchanged while widening the class
bound, changing one diamond binding or removing a synthetic flag. Independent
javac rejects the widened bound against the original nominal method. Original
input, harness, dependency and platform bytes stay frozen across both runs.
