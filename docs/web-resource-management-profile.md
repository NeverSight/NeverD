# JavaScript explicit resource management

The C++ parser profiles `hermes-602befee-js-v3` and
`hermes-602befee-recovery-js-v2` retain `using` and `await using` as ESTree
`VariableDeclaration` nodes with those exact `kind` values. Initializers,
bindings, comments and original UTF-8 locations remain intact. There is no
lowering to `const`, target execution or synthesized disposal implementation.

The grammar follows the [ECMA-262 lexical declaration rules](https://tc39.es/ecma262/multipage/ecmascript-language-statements-and-declarations.html#sec-let-and-const-declarations)
and [loop rules](https://tc39.es/ecma262/multipage/ecmascript-language-statements-and-declarations.html#sec-for-in-and-for-of-statements),
checked on 2026-10-11. C++ tests distinguish ordinary `using` identifiers,
property access and labels from declarations. They cover keyword escapes,
comment/newline restrictions, multiple initialized bindings, forbidden binding
patterns, missing initializers, duplicates within a declaration, unbraced
statements, exports, script/module/CommonJS contexts, async functions, static
blocks, ordinary loops, for-of and for-await-of. A module selects the top-level
await grammar parameter. A plain script cannot own a top-level resource
declaration; the CommonJS profile models a synchronous function wrapper.

Private build copies of the pinned Hermes parser and validator own admission.
`cmake/hermes-parser/Patches.cmake` requires each patch anchor to match exactly
once. Every parser translation unit sees the same private header. Lexer
backtracking restores newline state together with tokens, comments and source
locations so lookahead cannot change automatic semicolon insertion. Original
dependency files and notices remain unchanged.

`SourceBindings` assigns immutable lexical bindings and temporal-dead-zone
metadata. For-of resource bindings have per-iteration scope; a C-style loop's
resource is in its enclosing loop scope. As with the existing lexical profile,
cross-declaration conflicts remain explicit `binding_status: partial` findings;
successful parsing is not a complete ECMAScript early-error validation claim.

`SourceEffects` conservatively includes method lookup, calls, exceptions,
unknown effects and the scope-exit obligation in the declaration's immediate
effects. Async disposal also includes suspension. Effects inside a function
remain deferred until that function is invoked. The analysis does not assert
the exact disposal target, order, reachability, suppression behavior or runtime
completion; a resource declaration cannot become a purity or rewrite proof.

Readable recovery only inserts whitespace, then reparses with the same profile
and compares the full retained tree, including declaration kinds. Failed
admission or tree comparison still produces no readable file. The original
bytes and diagnostic offsets remain available. Real qualification against the
official Claude Code artifact is described in
[web-claude-code-qualification.md](web-claude-code-qualification.md).
