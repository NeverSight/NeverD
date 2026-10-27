//===- registration_observer.c - Independent native SEH observations ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Executes the same source entry repeatedly and observes its actual caller PC.
__declspec(dllimport) void __stdcall RaiseException(unsigned long,
                                                    unsigned long,
                                                    unsigned long,
                                                    const unsigned long *);
__declspec(dllimport) void __stdcall ExitProcess(unsigned);
__declspec(dllimport) int __cdecl printf(const char *, ...);
void *_ReturnAddress(void);
unsigned long __readfsdword(unsigned long);
#pragma intrinsic(_ReturnAddress)
#pragma intrinsic(__readfsdword)
#if REGISTRATION_PARENT_POP == 4
extern int __stdcall registration_entry(int);
#elif REGISTRATION_PARENT_POP == 16
extern int __stdcall registration_entry(int, int, int, int);
#elif REGISTRATION_PARENT_POP == 20
extern int __stdcall registration_entry(int, int, int, int, int);
#elif REGISTRATION_CASE == 5 || REGISTRATION_CASE == 6 || REGISTRATION_CASE == 12
extern int registration_entry(int);
#else
extern int registration_entry(void);
#endif
#if REGISTRATION_CASE == 6
extern int registration_call_entry(void);
unsigned registration_parameter_after;
#endif
static unsigned observed_caller;
unsigned registration_trace;
#if REGISTRATION_SYNCHRONIZATION
unsigned registration_atomic_word;
unsigned registration_atomic_exchange;
unsigned registration_atomic_results[2];
#endif
#if REGISTRATION_THREAD_STACK_BASE
unsigned registration_thread_stack_base;
static unsigned expected_thread_stack_base;
#endif
#if REGISTRATION_STARTUP_INFO
__declspec(dllimport) void __stdcall GetStartupInfoW(void *);
unsigned registration_startup_snapshot[17];
static unsigned expected_startup[17];

static void prepare_startup_observation(void) {
  for (unsigned i = 0; i != 17; ++i) {
    // Standard handles may remain untouched without STARTF_USESTDHANDLES.
    expected_startup[i] = 0xa5a5a5a5;
    registration_startup_snapshot[i] = 0;
  }
  GetStartupInfoW(expected_startup);
}

static int startup_observation_matches(void) {
  int matches = 1;
  for (unsigned i = 0; i != 17; ++i)
    if (registration_startup_snapshot[i] != expected_startup[i]) {
      printf("startup word %u: expected=%08x observed=%08x\n", i,
             expected_startup[i], registration_startup_snapshot[i]);
      matches = 0;
    }
  return matches;
}
#endif
#if (REGISTRATION_CASE >= 36 && REGISTRATION_CASE <= 49) ||                    \
    (REGISTRATION_CASE == 52 || REGISTRATION_CASE == 57 ||                     \
     REGISTRATION_CASE == 64 || REGISTRATION_CASE == 65 ||                     \
     (REGISTRATION_CASE >= 72 && REGISTRATION_CASE <= 81) ||                   \
     REGISTRATION_CASE == 84 || REGISTRATION_CASE == 85 ||                     \
     REGISTRATION_CASE == 88)
unsigned registration_iteration;
#endif
#if REGISTRATION_CASE >= 48 && REGISTRATION_CASE <= 51
unsigned registration_helper_visits;
#endif
#if REGISTRATION_CASE == 1 || REGISTRATION_CASE == 2
#define EXPECTED_TRACE 123
#elif REGISTRATION_CASE == 3
#define EXPECTED_TRACE 1
#elif REGISTRATION_CASE == 4
#define EXPECTED_TRACE 2
#elif REGISTRATION_CASE == 5
#define EXPECTED_TRACE 7
#elif REGISTRATION_CASE == 6
#define EXPECTED_TRACE 177
#elif REGISTRATION_CASE == 7 || REGISTRATION_CASE == 8
#define EXPECTED_TRACE 7u
#elif REGISTRATION_CASE == 28 || REGISTRATION_CASE == 29 ||                    \
    REGISTRATION_CASE == 30 || REGISTRATION_CASE == 31 ||                      \
    REGISTRATION_CASE == 34 || REGISTRATION_CASE == 35
#define EXPECTED_TRACE 243
#elif REGISTRATION_CASE >= 43 && REGISTRATION_CASE <= 49
#define EXPECTED_TRACE ((iterations & 1) + 1)
#elif REGISTRATION_CASE >= 76 && REGISTRATION_CASE <= 78
#define EXPECTED_TRACE 6662345u
#elif REGISTRATION_CASE >= 79 && REGISTRATION_CASE <= 81
#define EXPECTED_TRACE 2666345u
#elif REGISTRATION_CASE == 84
#define EXPECTED_TRACE ((iterations & 1) ? 24u : 23u)
#elif REGISTRATION_CASE == 86 || REGISTRATION_CASE == 87
#define EXPECTED_TRACE 234u
#elif REGISTRATION_CASE >= 36 && REGISTRATION_CASE <= 42
#define EXPECTED_TRACE 2345
#elif REGISTRATION_CASE == 68 || REGISTRATION_CASE == 70 ||                    \
    REGISTRATION_CASE == 74
#define EXPECTED_TRACE 23u
#elif REGISTRATION_CASE == 69 || REGISTRATION_CASE == 71 ||                    \
    REGISTRATION_CASE == 75
#define EXPECTED_TRACE 263u
#elif REGISTRATION_CASE == 72
#define EXPECTED_TRACE ((iterations & 1) ? 23u : 235u)
#elif REGISTRATION_CASE == 73
#define EXPECTED_TRACE ((iterations & 1) ? 263u : 26375u)
#elif REGISTRATION_CASE == 57
#define EXPECTED_TRACE ((iterations & 1) ? 23u : 235u)
#elif REGISTRATION_CASE == 54 || REGISTRATION_CASE == 58 ||                    \
    REGISTRATION_CASE == 60 || REGISTRATION_CASE == 62 ||                      \
    REGISTRATION_CASE == 64 || REGISTRATION_CASE == 66
#define EXPECTED_TRACE 236u
#elif REGISTRATION_CASE == 59 || REGISTRATION_CASE == 61 ||                    \
    REGISTRATION_CASE == 63 || REGISTRATION_CASE == 65 ||                      \
    REGISTRATION_CASE == 67
#define EXPECTED_TRACE 2367u
#elif REGISTRATION_CASE == 55
#define EXPECTED_TRACE 234u
#elif REGISTRATION_CASE == 9 || REGISTRATION_CASE == 10 ||                     \
    REGISTRATION_CASE == 12 || REGISTRATION_CASE >= 13
#define EXPECTED_TRACE 23u
#elif REGISTRATION_CASE == 11
#define EXPECTED_TRACE 3u
#else
#define EXPECTED_TRACE 0
#endif

void registration_raise(void) {
  observed_caller = (unsigned)_ReturnAddress();
#if REGISTRATION_CASE == 8
  RaiseException(0xe0420042, 1, 0, 0);
#else
  RaiseException(0xe0420042, 0, 0, 0);
#endif
}

void registration_no_raise(void) {
  observed_caller = (unsigned)_ReturnAddress();
#if REGISTRATION_CASE == 48 || REGISTRATION_CASE == 50
  ++registration_helper_visits;
#endif
}

void mainCRTStartup(void) {
  unsigned chain = __readfsdword(0);
  unsigned iterations = 0;
  int value = 0, chain_ok = 1;
  for (; iterations != 4; ++iterations) {
    registration_trace = 0;
#if REGISTRATION_SYNCHRONIZATION
    registration_atomic_word = 17;
    registration_atomic_exchange = 23 + iterations;
    registration_atomic_results[0] = registration_atomic_results[1] = 0;
#endif
#if REGISTRATION_THREAD_STACK_BASE
    registration_thread_stack_base = 0;
    // Observe the field directly, independently of the source's Self load.
    expected_thread_stack_base = __readfsdword(4);
#if REGISTRATION_SYNCHRONIZATION
    registration_atomic_exchange = expected_thread_stack_base;
#endif
#endif
#if REGISTRATION_PARENT_POP
    unsigned stack_before, stack_after;
    volatile unsigned stack_guard[2] = {0x12345678, 0xa5c39e71};
    __asm mov stack_before, esp
#endif
#if REGISTRATION_STARTUP_INFO
    prepare_startup_observation();
#endif
#if (REGISTRATION_CASE >= 36 && REGISTRATION_CASE <= 49) ||                    \
    (REGISTRATION_CASE == 52 || REGISTRATION_CASE == 57 ||                     \
     REGISTRATION_CASE == 64 || REGISTRATION_CASE == 65 ||                     \
     (REGISTRATION_CASE >= 72 && REGISTRATION_CASE <= 81) ||                   \
     REGISTRATION_CASE == 84 || REGISTRATION_CASE == 85 ||                     \
     REGISTRATION_CASE == 88)
    registration_iteration = iterations;
#endif
#if REGISTRATION_CASE >= 48 && REGISTRATION_CASE <= 51
    registration_helper_visits = 0;
#endif
#if REGISTRATION_PARENT_POP == 4
    value = registration_entry(7);
#elif REGISTRATION_PARENT_POP == 16
    value = registration_entry(7, 31, 47, 63);
#elif REGISTRATION_PARENT_POP == 20
    value = registration_entry(7, 31, 47, 63, 79);
#elif REGISTRATION_CASE == 6
    registration_parameter_after = 0;
    value = registration_call_entry();
    registration_trace += registration_parameter_after * 10;
#elif REGISTRATION_CASE == 5 || REGISTRATION_CASE == 12
    value = registration_entry(7);
#else
    value = registration_entry();
#endif
    chain_ok &= __readfsdword(0) == chain;
#if REGISTRATION_PARENT_POP
    __asm mov stack_after, esp
    chain_ok &= stack_before == stack_after && stack_guard[0] == 0x12345678 &&
                stack_guard[1] == 0xa5c39e71;
#endif
#if REGISTRATION_STARTUP_INFO
    if (!startup_observation_matches())
      break;
#endif
#if REGISTRATION_SYNCHRONIZATION
    if (registration_atomic_results[0] != 17 ||
        registration_atomic_results[1] != registration_atomic_exchange ||
        registration_atomic_word != registration_atomic_exchange) {
      printf("atomic observation: first=%u second=%u word=%u expected=%u\n",
             registration_atomic_results[0], registration_atomic_results[1],
             registration_atomic_word, registration_atomic_exchange);
      break;
    }
#endif
#if REGISTRATION_THREAD_STACK_BASE
    if (registration_thread_stack_base != expected_thread_stack_base) {
      printf("thread StackBase observation: got=%08x expected=%08x\n",
             registration_thread_stack_base, expected_thread_stack_base);
      break;
    }
#endif
    if (value != 7 || !chain_ok || registration_trace != EXPECTED_TRACE)
      break;
  }
  printf("neverd-registration-rewrite: value=%d caller=%08x entry=%08x "
         "chain=%d iterations=%u trace=%u\n",
         value, observed_caller, (unsigned)&registration_entry, chain_ok,
         iterations, registration_trace);
  ExitProcess(value == 7 && chain_ok && iterations == 4 ? 0 : 1);
}
