typedef unsigned long long U64;
#define EXPORT __declspec(dllexport)
#define API __declspec(dllimport)
API U64 SupportProbe(U64);
API volatile U64 SupportValue;
API U64 PrivateOrdinal(U64);
static volatile U64 Salt = 5;
static volatile U64 *volatile Anchor = &Salt;
EXPORT U64 DependentProbe(U64 Value) {
  return SupportProbe(Value) + SupportValue + *Anchor;
}
EXPORT U64 DependentSet(U64 Value) { return PrivateOrdinal(Value); }
