// Primary Native driver imports private code/data and, optionally, an ordinal.
typedef unsigned int U32;
typedef unsigned long long U64;
#define API __declspec(dllimport)
API U64 SupportProbe(U64);
API volatile U64 SupportValue;
#ifdef PRIMARY_ORDINAL_IMPORT
API U64 PrivateOrdinal(U64);
#endif
volatile U64 PrimaryEntryCount;
volatile U64 PrimaryObserved;
static volatile U64 *volatile Count = &PrimaryEntryCount;
U32 DriverEntry(void *Driver, void *Registry) {
  ++*Count;
#ifdef PRIMARY_ORDINAL_IMPORT
  PrivateOrdinal(99);
  const U64 Expected = 199;
#else
  const U64 Expected = 85;
#endif
  PrimaryObserved = SupportProbe(1) + SupportValue;
  return Driver && Registry && PrimaryObserved == Expected ? 0 : 0xc0000001;
}
