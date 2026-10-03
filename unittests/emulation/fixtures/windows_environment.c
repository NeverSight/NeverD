//===- windows_environment.c - Independent Win32 environment fixture ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned int U32;
typedef unsigned long long U64;
typedef unsigned short U16;
#define NEVERD_ENV_VALUE(Name, Value) enum { Name = Value };
#define NEVERD_ENV_MODE(Name, Value) enum { Name = Value };
#define NEVERD_ENV_WIDE(Name, Text) static const U16 Name[] = Text;
#include "WindowsEnvironmentCases.def"
#undef NEVERD_ENV_MODE
#undef NEVERD_ENV_WIDE
#undef NEVERD_ENV_VALUE
__declspec(dllimport) void ExitProcess(U32);
__declspec(dllimport) U16 *GetCommandLineW(void);
__declspec(dllimport) void *GetStdHandle(U32);
__declspec(dllimport) int WriteFile(void *, const void *, U32, U32 *, void *);
__declspec(dllimport) U32 GetLastError(void);
__declspec(dllimport) void SetLastError(U32);
__declspec(dllimport) U32 GetEnvironmentVariableW(const U16 *, U16 *, U32);
__declspec(dllimport) int SetEnvironmentVariableW(const U16 *, const U16 *);
__declspec(dllimport) U16 *GetEnvironmentStringsW(void);
__declspec(dllimport) int FreeEnvironmentStringsW(U16 *);
__declspec(dllimport) U32 ExpandEnvironmentStringsW(const U16 *, U16 *, U32);
__declspec(dllimport) void *VirtualAlloc(void *, U64, U32, U32);
__declspec(dllimport) int VirtualProtect(void *, U64, U32, U32 *);
static U16 Buffer[BufferUnits];
static U32 length(const U16 *S) {
  U32 N = 0;
  while (S[N])
    ++N;
  return N;
}
static void emit(const void *Bytes, U32 Size) {
  U32 Written;
  WriteFile(GetStdHandle(StdoutSelector), Bytes, Size, &Written, 0);
}
static void record(U32 Result) {
  U32 Values[] = {Result, GetLastError()};
  emit(Values, sizeof(Values));
}
static void wide(const U16 *S) { emit(S, (length(S) + 1) * sizeof(U16)); }
static void require(int Valid, U32 Site) {
  if (Valid)
    return;
  U32 Written;
  WriteFile(GetStdHandle(StderrSelector), &Site, sizeof(Site), &Written, 0);
  ExitProcess(FailureStatus);
}
static U16 **environment(void) {
  unsigned char *TEB;
#define NEVERD_ENV_ASM(Name, Text) __asm__(Text : "=r"(TEB));
#include "WindowsEnvironmentCases.def"
#undef NEVERD_ENV_ASM
  unsigned char *PEB = *(unsigned char **)(TEB + TebPEB);
  unsigned char *Params = *(unsigned char **)(PEB + PebParameters);
  return (U16 **)(Params + ParamsEnvironment);
}
static U16 *find(U16 *Block, const U16 *Name) {
  for (U16 *P = Block; *P; P += length(P) + 1) {
    U32 I = 0;
    while (Name[I] && P[I] == Name[I])
      ++I;
    if (!Name[I] && P[I] == '=')
      return P + I + 1;
  }
  return 0;
}
static void query(void) {
  SetLastError(LastErrorSeed);
  record(SetEnvironmentVariableW(Primary, First));
  SetLastError(LastErrorSeed);
  record(GetEnvironmentVariableW(Lower, 0, 0));
  SetLastError(LastErrorSeed);
  record(GetEnvironmentVariableW(Lower, Buffer, length(First)));
  SetLastError(LastErrorSeed);
  record(GetEnvironmentVariableW(Lower, Buffer, length(First) + 1));
  wide(Buffer);
  SetLastError(LastErrorSeed);
  record(SetEnvironmentVariableW(Primary, Empty));
  SetLastError(LastErrorSeed);
  record(GetEnvironmentVariableW(Primary, 0, 0));
  SetLastError(LastErrorSeed);
  record(GetEnvironmentVariableW(Primary, Buffer, 1));
  wide(Buffer);
  SetLastError(LastErrorSeed);
  record(SetEnvironmentVariableW(Primary, 0));
  SetLastError(LastErrorSeed);
  record(GetEnvironmentVariableW(Primary, Buffer, BufferUnits));
  SetLastError(LastErrorSeed);
  record(SetEnvironmentVariableW(Primary, 0));
  SetLastError(LastErrorSeed);
  record(SetEnvironmentVariableW(Empty, First));
  SetLastError(LastErrorSeed);
  record(SetEnvironmentVariableW(Invalid, First));
  SetLastError(LastErrorSeed);
  record(GetEnvironmentVariableW(Empty, Buffer, BufferUnits));
  SetLastError(LastErrorSeed);
  record(GetEnvironmentVariableW(0, Buffer, BufferUnits));
  SetLastError(LastErrorSeed);
  record(GetEnvironmentVariableW(Invalid, Buffer, BufferUnits));
}
static void mutation(void) {
  require(SetEnvironmentVariableW(Primary, First), 1);
  U16 *Old = GetEnvironmentStringsW();
  require(Old != 0, 2);
  require(SetEnvironmentVariableW(Lower, Second), 3);
  U16 *New = GetEnvironmentStringsW();
  require(New && New != Old, 4);
  U16 *Before = find(Old, Primary);
  require(Before != 0, 5);
  wide(Before);
  U16 *After = find(New, Primary);
  U16 *Renamed = find(New, Lower);
  U32 Mask = (After ? 1 : 0) | (Renamed ? 2 : 0);
  emit(&Mask, sizeof(Mask));
  require(After || Renamed, 6);
  wide(After ? After : Renamed);
  U16 *Live = find(*environment(), After ? Primary : Lower);
  require(Live != 0, 7);
  require(SetEnvironmentVariableW(Other, Live), 8);
  require(SetEnvironmentVariableW(Primary, 0), 9);
  require(GetEnvironmentVariableW(Other, Buffer, BufferUnits) == length(Second),
          10);
  wide(Buffer);
  // Input and output overlap after the API has consumed the name.
  for (U32 I = 0; I <= length(Other); ++I)
    Buffer[I] = Other[I];
  require(GetEnvironmentVariableW(Buffer, Buffer, BufferUnits) ==
              length(Second),
          11);
  wide(Buffer);
  SetLastError(LastErrorSeed);
  record(FreeEnvironmentStringsW(Old));
  SetLastError(LastErrorSeed);
  record(FreeEnvironmentStringsW(New));
}
static void expand(void) {
  require(SetEnvironmentVariableW(Primary, First), 1);
  require(SetEnvironmentVariableW(Other, Indirect), 2);
  SetLastError(LastErrorSeed);
  record(ExpandEnvironmentStringsW(Expansion, 0, 0));
  SetLastError(LastErrorSeed);
  record(ExpandEnvironmentStringsW(Expansion, Buffer, 1));
  SetLastError(LastErrorSeed);
  record(ExpandEnvironmentStringsW(Expansion, Buffer, BufferUnits));
  wide(Buffer);
  SetLastError(LastErrorSeed);
  record(ExpandEnvironmentStringsW(Empty, Buffer, BufferUnits));
  wide(Buffer);
  SetLastError(LastErrorSeed);
  record(ExpandEnvironmentStringsW(Second, Buffer, BufferUnits));
  wide(Buffer);
}
U32 entry(void) {
  U16 *Command = GetCommandLineW();
  U16 Mode = 0;
  for (U32 I = 0; Command[I]; ++I)
    if (Command[I] == ModePrefix)
      Mode = Command[I + 1];
  switch (Mode) {
  case QueryMode:
    query();
    break;
  case MutationMode:
    mutation();
    break;
  case ExpansionMode:
    expand();
    break;
  case ReclaimMode:
    for (U32 I = 0; I < RepeatCount; ++I) {
      U16 *Snapshot = GetEnvironmentStringsW();
      require(Snapshot != 0, 1);
      require(FreeEnvironmentStringsW(Snapshot), 2);
    }
    emit(&Mode, sizeof(Mode));
    break;
  case InitialMode:
    SetLastError(LastErrorSeed);
    record(GetEnvironmentVariableW(Initial, Buffer, BufferUnits));
    wide(Buffer);
    break;
  case NullSetMode:
    SetEnvironmentVariableW(0, First);
    break;
  case BadPointerMode:
    *environment() = Buffer;
    GetEnvironmentStringsW();
    break;
  case BadBlockMode:
    (*environment())[0] = '=';
    GetEnvironmentStringsW();
    break;
  case BadOutputMode: {
    U32 Old;
    U16 *ReadOnlyBuffer = VirtualAlloc(0, PageSize, ReserveCommit, ReadWrite);
    require(ReadOnlyBuffer != 0, 1);
    require(VirtualProtect(ReadOnlyBuffer, PageSize, ReadOnly, &Old), 2);
    require(SetEnvironmentVariableW(Primary, First), 3);
    GetEnvironmentVariableW(Primary, ReadOnlyBuffer, BufferUnits);
    break;
  }
  case DoubleFreeMode: {
    U16 *Snapshot = GetEnvironmentStringsW();
    require(Snapshot != 0, 1);
    require(FreeEnvironmentStringsW(Snapshot), 2);
    FreeEnvironmentStringsW(Snapshot);
    break;
  }
  case NonASCIIMode:
    SetEnvironmentVariableW(UnicodeName, First);
    break;
  case OverlapMode:
    Buffer[0] = 'x';
    Buffer[1] = 0;
    ExpandEnvironmentStringsW(Buffer, Buffer, BufferUnits);
    break;
  default:
    require(0, 1);
  }
  ExitProcess(ExitStatus);
  return FailureStatus;
}
