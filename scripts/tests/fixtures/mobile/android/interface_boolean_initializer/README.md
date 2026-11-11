# Interface boolean initialization

An owned interface compares two effectful integer producers, selects a zero/one value, and assigns its runtime boolean field once. The second producer changes the first producer's live field, so the first result must remain its original snapshot. The first producer reads the still-initializing interface field and observes its default value.

The immutable JVM observer checks operand order, one evaluation per producer, signed comparisons, runtime initialization, original failure identity and erroneous-class behavior over repeated attempts. Source and dependency profiles compare all original definitions and compiler metadata, including the absence of a boolean ConstantValue. Independently compiled wrong programs must fail the observer.
