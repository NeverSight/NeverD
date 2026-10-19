typedef unsigned char U8;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef struct {
  U8 Opaque[96];
  void *Context;
} TABLE;
typedef struct {
  U32 Key, Value;
} ITEM;
typedef struct {
  U32 Allocations, Frees, Comparisons;
  U8 Fail;
} CONTEXT;
__declspec(dllimport) void RtlInitializeGenericTableAvl(TABLE *, void *, void *,
                                                        void *, void *);
__declspec(dllimport) void *RtlInsertElementGenericTableAvl(TABLE *, void *,
                                                            int, U8 *);
__declspec(dllimport) void *
RtlInsertElementGenericTableFullAvl(TABLE *, void *, int, U8 *, void *, U32);
__declspec(dllimport) void *RtlLookupElementGenericTableAvl(TABLE *, void *);
__declspec(dllimport) void *RtlLookupElementGenericTableFullAvl(TABLE *, void *,
                                                                void **, U32 *);
__declspec(dllimport) U8 RtlDeleteElementGenericTableAvl(TABLE *, void *);
__declspec(dllimport) U32 RtlNumberGenericTableElementsAvl(TABLE *);
__declspec(dllimport) U8 RtlIsGenericTableEmptyAvl(TABLE *);
__declspec(dllimport) void *ExAllocatePoolWithTag(U32, U64, U32);
__declspec(dllimport) void ExFreePoolWithTag(void *, U32);
__declspec(dllimport) int DbgPrint(const char *, ...);
static U32 Compare(TABLE *T, ITEM *A, ITEM *B) {
  CONTEXT *C = (CONTEXT *)T->Context;
  ++C->Comparisons;
  return A->Key < B->Key ? 0 : A->Key > B->Key ? 1 : 2;
}
static void *Allocate(TABLE *T, int Bytes) {
  CONTEXT *C = (CONTEXT *)T->Context;
  if (C->Fail)
    return (void *)0;
  ++C->Allocations;
  return ExAllocatePoolWithTag(0x200, (U32)Bytes, 0x414c564e);
}
static void Free(TABLE *T, void *P) {
  CONTEXT *C = (CONTEXT *)T->Context;
  ++C->Frees;
  ExFreePoolWithTag(P, 0x414c564e);
}
static void Unload(void *D) {
  (void)D;
  DbgPrint("avl unload passed");
}
int DriverEntry(void *Driver, void *Registry) {
  (void)Registry;
  TABLE T;
  CONTEXT C = {0};
  RtlInitializeGenericTableAvl(&T, (void *)Compare, (void *)Allocate,
                               (void *)Free, &C);
  const U32 Keys[] = {4, 2, 6, 1, 3, 5, 7};
  if (!RtlIsGenericTableEmptyAvl(&T))
    return (int)0xc000000d;
  for (U32 I = 0; I < 7; ++I) {
    ITEM V = {Keys[I], Keys[I] * 10};
    U8 New = 0;
    void *Parent = (void *)0x1234;
    U32 Search = 0xa5;
    if (RtlLookupElementGenericTableFullAvl(&T, &V, &Parent, &Search))
      return (int)0xc000000d;
    if (!I && (Search || Parent != (void *)0x1234))
      return (int)0xc000000d;
    ITEM *P = RtlInsertElementGenericTableFullAvl(&T, &V, sizeof(V), &New,
                                                  Parent, Search);
    if (!P || !New || P == &V || P->Key != V.Key || P->Value != V.Value ||
        RtlNumberGenericTableElementsAvl(&T) != I + 1)
      return (int)0xc000000d;
  }
  ITEM Duplicate = {4, 999};
  U8 New = 1;
  ITEM *Old =
      RtlInsertElementGenericTableAvl(&T, &Duplicate, sizeof(Duplicate), &New);
  if (!Old || New || Old->Value != 40 || C.Allocations != 7)
    return (int)0xc000000d;
  ITEM Missing = {100, 1000};
  C.Fail = 1;
  New = 0xa5;
  if (RtlInsertElementGenericTableAvl(&T, &Missing, sizeof(Missing), &New) ||
      New || RtlNumberGenericTableElementsAvl(&T) != 7)
    return (int)0xc000000d;
  C.Fail = 0;
  for (U32 I = 0; I < 7; ++I) {
    ITEM V = {Keys[I], 0};
    ITEM *P = RtlLookupElementGenericTableAvl(&T, &V);
    if (!P || P->Value != Keys[I] * 10 ||
        !RtlDeleteElementGenericTableAvl(&T, &V))
      return (int)0xc000000d;
  }
  if (!RtlIsGenericTableEmptyAvl(&T) ||
      RtlDeleteElementGenericTableAvl(&T, &Missing) || C.Frees != 7 ||
      !C.Comparisons)
    return (int)0xc000000d;
  *(void (**)(void *))((U8 *)Driver + 0x68) = Unload;
  DbgPrint("avl guest callbacks passed");
  return 0;
}
