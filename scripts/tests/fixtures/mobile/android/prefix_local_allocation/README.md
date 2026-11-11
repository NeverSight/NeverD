# Anonymous constructor argument before this delegation

A genuine Kotlin secondary constructor allocates an anonymous implementation
before delegating to another constructor of the same class. The anonymous body
captures only original input parameters and has no enclosing-instance field.
Independent frozen observers must compare original and recovered behavior,
constructor/effect order, wide input bits, captured identity and partial final
state, with every original method and metadata difference accounted for.
