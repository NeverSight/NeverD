//===- driver_invocation.c - Driver-stage finite call fixture -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
#define API __declspec(dllimport)
typedef struct {
  U16 Length, MaximumLength;
  U64 Buffer;
} US;
typedef struct {
  U32 Length;
  U64 Root, Name;
  U32 Flags;
  U64 Security, QoS;
} OA;
API U64 PsGetCurrentThreadWin32Thread(void);
API U32 ExCreateCallback(U64 *, OA *, U8, U8);
API U64 ExRegisterCallback(U64, void (*)(U64, U64, U64), U64);
API void ExNotifyCallback(U64, U64, U64);
API void ExUnregisterCallback(U64);
API void ObfDereferenceObject(U64);
API U8 KeGetCurrentIrql(void);
API U8 KfRaiseIrql(U8);
API void RtlCaptureContext(void *);
static volatile U64 Counter;
static volatile U64 *volatile CounterPointer = &Counter;
static U64 CallbackValue;
static void notified(U64 Context, U64 A, U64 B) {
  CallbackValue = KeGetCurrentIrql() ? 0xffff : Context + A + B;
}
U64 ProbeSum(U64 A, U64 B, U64 C, U64 D, U64 E, U64 F, U64 G) {
  return A + B + C + D + E + F + G + *CounterPointer;
}
U64 ProbeBuffers(U8 *Bytes, U64 Size) {
  for (U64 I = 0; I < Size; ++I)
    Bytes[I] ^= (U8)*CounterPointer;
  return ++*CounterPointer;
}
U64 ProbeModelTail(void) { return PsGetCurrentThreadWin32Thread(); }
U64 ProbeNotify(U64 Value) {
  static const U16 NameText[] = L"\\Callback\\NeverDInvocation";
  US Name = {sizeof(NameText) - 2, sizeof(NameText), (U64)NameText};
  OA Attr = {sizeof(OA), 0, (U64)&Name, 0x40, 0, 0};
  U64 Object = 0;
  if (ExCreateCallback(&Object, &Attr, 1, 1))
    return 0xff01;
  U64 Token = ExRegisterCallback(Object, notified, 7);
  if (!Token)
    return 0xff02;
  ExNotifyCallback(Object, Value, Value + 1);
  ExUnregisterCallback(Token);
  ObfDereferenceObject(Object);
  return CallbackValue;
}
U64 ProbeCapture(void *Context) {
  RtlCaptureContext(Context);
  return 17;
}
U64 ProbeIRQLLeak(void) {
  KfRaiseIrql(2);
  return 7;
}
U64 ProbeFault(void) { return *(volatile U64 *)0x12345000ULL; }
__attribute__((naked)) U64 ProbeBadStack(void) { __asm__ volatile("retq $8"); }
U64 ProbeLoop(void) {
  for (;;)
    __asm__ volatile("pause");
}
static void *volatile Functions[] = {
    (void *)&ProbeSum,    (void *)&ProbeBuffers,  (void *)&ProbeModelTail,
    (void *)&ProbeNotify, (void *)&ProbeCapture,  (void *)&ProbeIRQLLeak,
    (void *)&ProbeFault,  (void *)&ProbeBadStack, (void *)&ProbeLoop};
U32 DriverEntry(void *Driver, void *Registry) {
  if (!Driver || !Registry || !Functions[0])
    return 0xc000000d;
  *CounterPointer = 41;
  return 0;
}
