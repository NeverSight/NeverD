# Long arithmetic allocation

Owned Java 8 nested allocations compute long addition, subtraction and multiplication as constructor arguments. The last three arguments sign-extend dynamic int inputs. Separately compiled frozen callers observe all six full-width results, extrema and wrapping overflow, provider order, partial final fields, repeated allocation identity and failures at every effect boundary. Source and SDK-provider profiles compile all emitted Java and compare original definitions, metadata and Exceptions. Eight independent wrong implementations must be detected.

The admission covers total long operations only. Integer division/remainder, malformed wide words and unsupported literal long forms retain their existing rejection rules.
