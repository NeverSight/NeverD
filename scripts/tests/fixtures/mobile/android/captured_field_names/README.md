This fixture uses genuine Kotlin class-based SAM captures with an original wide
parameter and independent object/callback captures. A single field-name constant
pool edit gives the wide capture a legal JVM name containing Java-illegal hyphens.
Every Code attribute and descriptor stays byte-identical. Frozen callers exercise
the public factory and LongSupplier dispatch before and after source rebuilding.
