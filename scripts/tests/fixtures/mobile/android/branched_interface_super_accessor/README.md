# Branched interface superclass accessors

Original Kotlin compiler output supplies a synthetic static accessor for a
default inherited through one of two direct interfaces. The other interface has
an unrelated abstract method and appears first in the original interface list.
The parent compiler emits no static compatibility accessor for this method.

Frozen callers, reflection, method handles and a rebuilt caller observe explicit
default dispatch, virtual calls inside that default, opaque receiver identity,
wide argument words, result values, reentry and exact checked/runtime/Error
identity. Every generated Java unit must compile. Original entries, metadata and
Code roles remain present, with only the established D8 CLASS annotation loss,
ordinary synthetic accessor flag and explicit private helper disclosures.
