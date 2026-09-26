//===- driver_loader.c - Original PE cookie and relocation fixture --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A real compiler-inserted /GS check executes through an ordinary PE entry
/// wrapper; the loader never bypasses the wrapper.
///
//===----------------------------------------------------------------------===//

typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef int NTSTATUS;

volatile U64 __security_cookie = 0x2b992ddfa232ULL;
volatile U64 __security_cookie_complement = ~0x2b992ddfa232ULL;
static volatile U64 Payload = 0x76543210abcdef98ULL;
static volatile U64 *volatile PayloadAddress = &Payload;
static volatile U64 ObservedCookie;
static volatile U64 ObservedComplement;
static volatile U64 ObservedPayload;

// The public PE32+ layout through SecurityCookie, emitted under the symbol
// name recognized by the Microsoft-compatible linker.
struct LOAD_CONFIG_COOKIE {
  U32 Size;
  U32 TimeDateStamp;
  U16 MajorVersion;
  U16 MinorVersion;
  U32 GlobalFlagsClear;
  U32 GlobalFlagsSet;
  U32 CriticalSectionDefaultTimeout;
  U64 DeCommitFreeBlockThreshold;
  U64 DeCommitTotalFreeThreshold;
  U64 LockPrefixTable;
  U64 MaximumAllocationSize;
  U64 VirtualMemoryThreshold;
  U64 ProcessAffinityMask;
  U32 ProcessHeapFlags;
  U16 CSDVersion;
  U16 DependentLoadFlags;
  U64 EditList;
  const volatile U64 *SecurityCookie;
};
_Static_assert(sizeof(struct LOAD_CONFIG_COOKIE) == 96, "PE32+ cookie layout");
_Static_assert(__builtin_offsetof(struct LOAD_CONFIG_COOKIE, SecurityCookie) ==
                   88,
               "PE32+ cookie field");
const struct LOAD_CONFIG_COOKIE _load_config_used = {
    sizeof(struct LOAD_CONFIG_COOKIE),
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    &__security_cookie};

__declspec(noinline) __attribute__((no_stack_protector)) void
__security_check_cookie(U64 Value) {
  if (Value != __security_cookie)
    __builtin_trap();
}

__declspec(noinline) static NTSTATUS ProtectedEntry(void *Driver,
                                                    void *Registry) {
  volatile U8 Buffer[48];
  for (unsigned I = 0; I != sizeof(Buffer); ++I)
    Buffer[I] = (U8)I;
  ObservedCookie = __security_cookie;
  ObservedComplement = __security_cookie_complement;
  ObservedPayload = *PayloadAddress;
  if (!Driver || !Registry || !ObservedCookie ||
      ObservedCookie == 0x2b992ddfa232ULL || ObservedCookie >> 48 ||
      ObservedComplement != ~ObservedCookie ||
      ObservedPayload != 0x76543210abcdef98ULL || Buffer[47] != 47)
    return (NTSTATUS)0xc000000dU;
  return 0;
}

__attribute__((no_stack_protector)) NTSTATUS GsDriverEntry(void *Driver,
                                                           void *Registry) {
  // This models the entry wrapper's obligation without copying CRT code.
  // The loader must already have supplied a non-default global cookie.
  if (!__security_cookie || __security_cookie == 0x2b992ddfa232ULL)
    return (NTSTATUS)0xc000000dU;
  __security_cookie_complement = ~__security_cookie;
  return ProtectedEntry(Driver, Registry);
}
