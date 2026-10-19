typedef unsigned long long U64;
static volatile U64 Value = 9;
static volatile U64 *volatile Anchor = &Value;
__declspec(dllexport) U64 RelayOwn(void) { return *Anchor; }
