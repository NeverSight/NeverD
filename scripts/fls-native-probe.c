typedef unsigned int U32;
typedef unsigned long long U64;
typedef void (*Callback)(void *);
__declspec(dllimport) U32 FlsAlloc(Callback);
__declspec(dllimport) int FlsFree(U32);
__declspec(dllimport) void *FlsGetValue(U32);
__declspec(dllimport) int FlsSetValue(U32, void *);
__declspec(dllimport) U32 GetLastError(void);
__declspec(dllimport) void SetLastError(U32);
__declspec(dllimport) void *GetStdHandle(U32);
__declspec(dllimport) int WriteFile(void *, const void *, U32, U32 *, void *);
__declspec(dllimport) const char *GetCommandLineA(void);
__declspec(dllimport) void ExitProcess(U32);
static U32 Mode, Primary, Peer, Added = 0xffffffffU;
static void record(U32 Tag, U64 A, U64 B, U64 C, U64 D, U64 E, U64 F) {
  U64 Words[] = {Mode, Tag, A, B, C, D, E, F};
  U32 Written;
  WriteFile(GetStdHandle((U32)-11), Words, sizeof(Words), &Written, 0);
}
static void sample(U32 Tag, U32 Index) {
  SetLastError(1234);
  U64 Value = (U64)FlsGetValue(Index);
  U32 Error = GetLastError();
  record(Tag, Index, Value, Error, 0, 0, 0);
}
static void child(void *Value) {
  record('c', (U64)Value, Primary, Peer, Added, 0, 0);
  sample('p', Primary);
  sample('q', Peer);
  if (Added != 0xffffffffU)
    sample('a', Added);
}
static void parent(void *Value) {
  record('C', Primary, (U64)Value, 0, 0, 0, 0);
  sample('B', Primary);
  SetLastError(1234);
  int Set = FlsSetValue(Primary, (void *)99);
  U32 Error = GetLastError();
  record('W', Primary, Set, Error, 0, 0, 0);
  sample('V', Primary);
  if (Mode == 'F')
    return;
  if (Mode == 'G') {
    int Free = FlsFree(Peer);
    record('g', Peer, Free, 0, 0, 0, 0);
  }
  if (Mode == 'S') {
    int SetPeer = FlsSetValue(Peer, (void *)9);
    record('s', Peer, SetPeer, 0, 0, 0, 0);
  }
  Added = FlsAlloc(child);
  int SetAdded = FlsSetValue(Added, (void *)8);
  record('A', Added, SetAdded, Added == Primary, Added == Peer, 0, 0);
  if (Mode == 'N') {
    int Free = FlsFree(Added);
    record('n', Added, Free, 0, 0, 0, 0);
  }
}
static void tls(void *Image, U32 Reason, void *Reserved) {
  (void)Image;
  (void)Reserved;
  if (Reason == 0) {
    sample('T', Primary);
    sample('U', Peer);
    if (Added != 0xffffffffU)
      sample('Z', Added);
  }
}
__declspec(allocate(".tls")) char TLSStart;
__declspec(allocate(".tls$ZZZ")) char TLSEnd;
U32 _tls_index;
static void (*Callbacks[])(void *, U32, void *) = {tls, 0};
__declspec(allocate(".rdata")) const struct {
  const void *Start, *End;
  U32 *Index;
  const void *Callbacks;
  U32 ZeroFill, Characteristics;
} _tls_used = {&TLSStart, &TLSEnd, &_tls_index, Callbacks, 0, 0};
int entry(void) {
  const char *P = GetCommandLineA();
  for (; *P; ++P)
    if (*P == '!')
      Mode = (U32)P[1];
  Primary = FlsAlloc(parent);
  Peer = FlsAlloc(child);
  int SetPrimary = FlsSetValue(Primary, (void *)7);
  int SetPeer = FlsSetValue(Peer, Mode == 'E' || Mode == 'R' ? (void *)9 : 0);
  record('I', Primary, Peer, SetPrimary, SetPeer, 0, 0);
  if (Mode == 'F' || Mode == 'N') {
    int Free = FlsFree(Primary);
    record('D', Primary, Free, 0, 0, 0, 0);
    sample('P', Primary);
    U32 Reused = FlsAlloc(0);
    record('J', Reused, Reused == Primary, 0, 0, 0, 0);
    FlsFree(Reused);
    FlsFree(Peer);
  }
  if (Mode == 'R')
    return 43;
  ExitProcess(43);
  return 43;
}
