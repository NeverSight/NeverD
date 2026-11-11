# Inherited wide primitive consumers

A closed generic interface inherits consumer and producer contracts and repeats
a fixed metadata getter. Its final implementation keeps an original Number to
long write bridge, a typed read method with its original covariant bridge, and
the metadata getter. A separate marker argument remains parameterized.

The raw consumer is independently authored and compiled. An owned class-file
encoding adds only its original bridge flags and the closed interface argument;
all Code attributes remain unchanged. Recovery discloses the raw source edge
and the consumer bridge flags while retaining the complete original inheritance
graph for producer and metadata checks.

The immutable JVM harness and caller cover 100 value checks, 138 identities,
115 states and 36 ordered failures: long boundaries, floating Number conversion,
custom conversion side effects, null receivers/contexts/values, target and
conversion exceptions, mutable getter aliases, reflection and method handles.
Source and dependency profiles compare all class, field, method and exception
metadata outside the disclosed source differences. Negative same-Code inputs
change the consumer or producer binding or remove a bridge flag; independent
javac also rejects the incompatible contracts. Native controls corrupt wide
argument words, incoming origins, siblings and interface ancestry.

The source profile retains six classes and twelve methods. The immutable
interface declarations leave two input classes and seven methods. Recovery
reports the explicit original consumer bridge and regenerated producer bridge;
no input runtime entry is added or removed.
