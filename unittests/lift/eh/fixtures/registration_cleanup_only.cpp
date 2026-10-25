// A PE32 parent whose two objects unwind into the caller's catch. Build with
// i686-pc-windows-msvc, exceptions and frame pointers at both O0 and O1.
extern "C" __declspec(dllimport) __declspec(noreturn) void __stdcall
ExitProcess(unsigned);

extern "C" {
__declspec(dllexport) volatile unsigned cleanup_only_trace = 0;
__declspec(dllexport) volatile unsigned cleanup_only_enabled = 1;
#ifdef NEVERD_NORETURN_CXX_PARENT
__declspec(noreturn)
#endif
__declspec(dllexport) __declspec(noinline) void cleanup_only_throw() {
  throw 7;
}
}

struct CleanupOnlyGuard {
  unsigned Tag;
  __declspec(noinline) ~CleanupOnlyGuard();
};

CleanupOnlyGuard::~CleanupOnlyGuard() {
  cleanup_only_trace = cleanup_only_trace * 10 + Tag;
}

#ifdef NEVERD_NESTED_CXX_PARENT
extern "C" __declspec(noinline) int cleanup_only_inner();

extern "C" __declspec(dllexport) __declspec(noinline) int
cleanup_only_parent() {
  if (!cleanup_only_enabled)
    return cleanup_only_inner();
  CleanupOnlyGuard Outer{3};
  CleanupOnlyGuard Inner{4};
  return cleanup_only_inner();
}
#else
extern "C" __declspec(dllexport) __declspec(noinline) int
#ifdef NEVERD_SHARED_PROLOGUE
cleanup_only_parent();
#else
cleanup_only_parent() {
#ifndef NEVERD_NORETURN_CXX_PARENT
  if (!cleanup_only_enabled)
    return 0;
#endif
  CleanupOnlyGuard Outer{1};
  CleanupOnlyGuard Inner{2};
#ifdef NEVERD_NORETURN_CXX_PARENT
  if (!cleanup_only_enabled)
    cleanup_only_trace = 3;
#endif
  cleanup_only_throw();
#ifndef NEVERD_NORETURN_CXX_PARENT
  return 9;
#endif
}
#endif
#endif

extern "C" __declspec(noreturn) void mainCRTStartup() {
#ifdef NEVERD_SHARED_PROLOGUE
  const unsigned Before = cleanup_only_trace;
  cleanup_only_enabled = 0;
  if (cleanup_only_parent() != 0 || cleanup_only_trace != Before)
    ExitProcess(3);
  cleanup_only_enabled = 1;
#endif
  try {
    cleanup_only_parent();
  } catch (int Value) {
#ifdef NEVERD_NESTED_CXX_PARENT
    const unsigned Expected = 2143;
#else
    const unsigned Expected = 21;
#endif
    ExitProcess(Value == 7 && cleanup_only_trace == Expected ? 0 : 1);
  }
  ExitProcess(2);
}
