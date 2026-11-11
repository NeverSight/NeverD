Owned regression for constructor forwarding between captured sibling members.
The original constructors omit the enclosing argument from their Signature and
share the exact outer formal inside a contravariant callback. The frozen caller
checks enclosing identities, partial final fields, argument identity, ordered
effects and exact exceptions; its callback implementation is outside the DEX.
Metadata mutations retain all original Code while changing formal identity,
variance or the sibling owner edge. All names and source are authored fixtures.
