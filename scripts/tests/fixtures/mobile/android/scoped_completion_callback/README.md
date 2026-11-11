# Closed scope and completion callback

A public static generic entry receives a three-slot callback consuming an
independently declared fixed Scope and a nested completion of the same method
formal as the final completion. Frozen callers check original scope/completion
identity, saved references, nulls, sentinels, wide scope state and failure order.
Every original compiler entry and metadata is compared before and after source
recovery. Owned and SDK variants run 56 cases twice per stage; four independently
compiled wrong programs must fail.

The `--instance-provider` variant copies these authored sources into a fresh
directory, moves the implementation to Engine, and keeps an unchanged generic
entry on a public Provider interface. Frozen callers use the original interface
receiver through ordinary dispatch, reflection and method handles; the engine
records that receiver before any completion callback. A separate frozen observer
checks that a null receiver fails before entering the provider body. All original
descriptors, Signatures, access flags, exceptions and other metadata remain exact.

The separate `--leading-word` variant keeps the static entry and adds one long
before the same callback/completion pair. Frozen callers supply both long extrema
and nonzero high words through ordinary calls, reflection and method handles.
The provider records that exact value before completion/context effects, even
when they fail. A fifth independent wrong program truncates the word to int.
Original metadata and all generated Java are checked in both declaration modes.

The `--leading-context` variant adds a separate Scope reference before the
callback pair. Frozen callers check its exact identity before context/completion
effects, including null, deferred and failing paths. The callback's own Scope is
independent. Both declaration modes run 56 cases twice and compare all original
metadata; a fifth wrong program loses the leading context reference.
