// Native guest module fixture; entry initialization is explicitly separate.
typedef unsigned char U8;
typedef unsigned int U32;
typedef unsigned long long U64;
#define EXPORT __declspec(dllexport)
#define API __declspec(dllimport)
API U8 KeGetCurrentIrql(void);
API U32 ZwQuerySystemInformation(U32, void *, U32, U32 *);
EXPORT volatile U64 SupportValue = 42;
static volatile U64 *volatile Anchor = &SupportValue;
EXPORT U64 SupportProbe(U64 Value) {
  return *Anchor + Value + KeGetCurrentIrql();
}
EXPORT U64 SupportSet(U64 Value) { return *Anchor = Value; }
EXPORT U64 SupportFlags(U64 Record, U64 Value) {
  return *(volatile U32 *)(Record + 0x68) = (U32)Value;
}
EXPORT U64 SupportRead(U64 Address) {
  *Anchor += KeGetCurrentIrql() + 1;
  U64 Value = *(volatile U64 *)Address;
  return Value + *Anchor;
}
EXPORT U64 SupportIndirect(U64 Address, U64 Value) {
  ++*Anchor;
  return ((U64 (*)(U64))Address)(Value) + *Anchor;
}
EXPORT U64 SupportModules(void *Bytes, U64 Size) {
  U32 Required = 0;
  U32 Status = ZwQuerySystemInformation(11, Bytes, (U32)Size, &Required);
  return ((U64)Status << 32) | Required;
}
EXPORT U64 SupportRing(U64 First) {
  U64 Current = First, Count = 0;
  if (!First)
    return 0;
  do {
    Current = *(volatile U64 *)Current;
    if (++Count > 256)
      return ~0ULL;
  } while (Current != First);
  return Count;
}
EXPORT U64 SupportRecord(U64 First, U64 Base) {
  U64 Current = First;
  for (U32 Count = 0; Current && Count < 256; ++Count) {
    if (*(volatile U64 *)(Current + 0x30) == Base)
      return Current;
    Current = *(volatile U64 *)Current;
    if (Current == First)
      break;
  }
  return 0;
}
U32 DriverEntry(void *Driver, void *Registry) {
  *Anchor = 1000;
  return Driver && Registry ? 0 : 0xc000000d;
}
