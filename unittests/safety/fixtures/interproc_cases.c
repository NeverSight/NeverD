// Interprocedural hunt fixtures are compiled separately from safety_cases.c:
// light optimization avoids artificial argument spill/reload findings, while
// sibling-call inhibition preserves the intended direct internal call edges.

#if defined(_MSC_VER)
#define FIXTURE_EXPORT __declspec(dllexport)
// External COFF linkage keeps private function identities in the PDB, while
// the lack of dllexport keeps them out of the PE export directory.
#define FIXTURE_HIDDEN
#define FIXTURE_NOINLINE __declspec(noinline)
#elif defined(__GNUC__)
#define FIXTURE_EXPORT __attribute__((visibility("default")))
#define FIXTURE_HIDDEN __attribute__((visibility("hidden")))
#define FIXTURE_NOINLINE __attribute__((noinline))
#else
#define FIXTURE_EXPORT
#define FIXTURE_HIDDEN
#define FIXTURE_NOINLINE
#endif

#if defined(__clang__) || defined(__GNUC__)
#define FIXTURE_USED __attribute__((used))
#else
#define FIXTURE_USED
#endif

extern char *getenv(const char *);
FIXTURE_HIDDEN void interproc_forward_inner(const char *);
FIXTURE_HIDDEN void interproc_isolation_leaf(const char *);

// Give both optimized objects one stable logical source identity in reports.
#line 30 "interproc_cases.c"
FIXTURE_HIDDEN FIXTURE_NOINLINE void interproc_forward_outer(const char *s) {
  interproc_forward_inner(s);
}

FIXTURE_EXPORT FIXTURE_NOINLINE void interproc_budget_entry(void) {
  char *s = getenv("PAYLOAD");
  interproc_forward_outer(s);
}

#line 50 "interproc_cases.c"
// Hunt negative control: this leaf is structurally reachable only from a clean
// exported root.  The dead tainted helper below must not pollute that context.
FIXTURE_EXPORT FIXTURE_NOINLINE void interproc_isolation_entry(void) {
  interproc_isolation_leaf("CLEAN");
}

// Keep the helper discoverable in companions without making it an export or a
// control-flow root.  Its getenv flow is intentionally unreachable.
FIXTURE_HIDDEN FIXTURE_NOINLINE FIXTURE_USED void
interproc_dead_tainted_helper(void) {
  char *s = getenv("PAYLOAD");
  interproc_isolation_leaf(s);
}
