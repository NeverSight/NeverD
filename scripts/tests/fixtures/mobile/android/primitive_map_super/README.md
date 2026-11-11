# Primitive allocation before a generic superclass constructor

An owned generic child allocates a generic map with exact int/float inputs,
then passes the initialized object to the original covariant Map constructor
beside an int overload. Two child input orders and a competing allocation
overload check argument slots. Four frozen constructor routes compare 96 cases
twice with source and SDK parents, including raw float bits, invalid map
configuration, allocation/parent/child order, partial writes, callback effects,
exact failures and later key casts. Every recovered unit and original metadata
entry, including a methodless marker, is compiled and audited. Wrong programs
and Code-preserving Signature controls test the observer and source scope.
Only owned maps execute.
