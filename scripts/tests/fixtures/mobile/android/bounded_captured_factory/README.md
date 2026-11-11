# Bounded captured factory

Independent Java sources bind one Object formal through an outer carrier and two
more through a converter, including an exact non-generic Vector class bound. The
factory returns a real instance member using those two inner arguments. NeverD's
existing constructor capture proof retains the explicit outer parameter/field and
discloses its static source form and removed generic owner arguments.

The frozen observer compares direct wrappers, reflection and method handles,
callback results, vector mutation, the actual selected outer (which may differ
from the input outer), partial constructor writes and exact exception identity.
All original runtime entries and metadata are reconciled after the documented
member flag, capture-field synthetic flag and factory/constructor Signature
changes. No input bytecode edits or private sample content is used. The dependency
variant freezes the original factory bytes. Three compiled wrong programs must
fail; native cases reject bounds, variance, incomplete formal uses and capture drift.
