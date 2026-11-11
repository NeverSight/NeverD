# Independent seeded callback and completion formals

Two method formals remain distinct: a direct input seed binds the callback's
first slot; its nested completion and final completion share the other formal.
One closed tag and an arbitrary Object callback result retain their original
slots. Frozen callers audit seed/tag/receiver identities, nulls, deferred
captures, sentinels and ordered failures. Complete original compiler metadata
is compared in owned and declaration configurations. Four independent wrong
programs must fail across 56 cases with two attempts before and after recovery.
