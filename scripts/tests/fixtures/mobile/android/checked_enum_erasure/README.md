# Checked enum parameter metadata fixture

The independent javac baseline implements `Transform<Enum, String>`, reads its
own primitive field, then checks the input against an enum's interface. Only
generic Signature attributes are narrowed to the owned `Choice` enum; every
runtime descriptor and executable byte stays intact. D8 supplies the input.

The recovered declaration retains Enum and the original interface cast. The
JVM oracle includes a different enum implementing that interface, an enum that
does not, and non-enum bridge inputs. It checks both cast stages, reference and
exception identity, primitive capture values, null arguments and receivers,
runtime descriptors, generic reflection and regenerated bridge identity.
