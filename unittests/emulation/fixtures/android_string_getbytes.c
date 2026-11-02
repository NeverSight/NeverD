//===- android_string_getbytes.c - JNI named UTF-8 arrays -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent UTF-16 inputs, byte-array guards, call forms and retained refs.
///
//===----------------------------------------------------------------------===//

typedef unsigned long long U64;
typedef const U64 *Table;
typedef Table *Handle;
typedef U64 Ref;
extern int JNI_GetCreatedJavaVMs(Handle *, int, int *);
#define C(E, R, S, ...) (((R (*)(Handle, ...))(*E)[S])(E, ##__VA_ARGS__))
#define CHECK(X)                                                               \
  do {                                                                         \
    if (!(X))                                                                  \
      return __LINE__;                                                         \
  } while (0)
static Handle vm;
static Handle attach(void) {
  int count = 0;
  Handle env = 0;
  if (JNI_GetCreatedJavaVMs(&vm, 1, &count) || count != 1 ||
      C(vm, int, 4, &env, (void *)0))
    return 0;
  return env;
}
int string_getbytes(U64 *out, int mode) {
  Handle e = attach();
  CHECK(e);
  Ref type = C(e, Ref, 6, "java/lang/String");
  Ref method = C(e, Ref, 33, type, "getBytes", "(Ljava/lang/String;)[B");
  CHECK(method);
  const unsigned short units[] = {'A', 0, 0x80, 0x800, 0xd800, 0xdc00, 0xdfff};
  const unsigned char expected[] = {'A',  0,    0xc2, 0x80, 0xe0, 0xa0,
                                    0x80, 0xf0, 0x90, 0x80, 0x80, '?'};
  Ref value = C(e, Ref, 163, units, 7);
  Ref charset = mode == 1 ? 0 : C(e, Ref, 167, mode == 2 ? "UTF8" : "uTf-8");
  Ref a = C(e, Ref, 34, value, method, charset);
  if (mode == 1) {
    CHECK(!a && C(e, int, 228));
    Ref exception = C(e, Ref, 15);
    C(e, void, 17);
    Ref npe = C(e, Ref, 6, "java/lang/NullPointerException");
    CHECK(C(e, int, 32, exception, npe));
    out[0] = 17;
    return C(vm, int, 5);
  }
  CHECK(a && !C(e, int, 228));
  U64 args[] = {charset};
  Ref b = C(e, Ref, 36, value, method, args);
  CHECK(b && !C(e, int, 24, a, b));
  CHECK(C(e, int, 171, a) == sizeof(expected));
  unsigned char guarded[16];
  for (unsigned i = 0; i != sizeof(guarded); ++i)
    guarded[i] = 0xa5;
  C(e, void, 200, a, 0, (int)sizeof(expected), guarded + 2);
  CHECK(guarded[0] == 0xa5 && guarded[1] == 0xa5 && guarded[14] == 0xa5 &&
        guarded[15] == 0xa5);
  for (unsigned i = 0; i != sizeof(expected); ++i)
    CHECK(guarded[i + 2] == expected[i]);
  Ref held = C(e, Ref, 21, b);
  unsigned char changed = 9;
  C(e, void, 208, a, 0, 1, &changed);
  Ref empty = C(e, Ref, 163, units, 0);
  Ref x = C(e, Ref, 34, empty, method, charset);
  Ref y = C(e, Ref, 36, empty, method, args);
  CHECK(x && y && !C(e, int, 24, x, y) && C(e, int, 171, x) == 0);
  CHECK(C(vm, int, 5) == 0);
  e = attach();
  CHECK(e);
  CHECK(C(e, int, 171, held) == sizeof(expected));
  C(e, void, 200, held, 0, (int)sizeof(expected), guarded + 2);
  for (unsigned i = 0; i != sizeof(expected); ++i)
    CHECK(guarded[i + 2] == expected[i]);
  for (unsigned i = 0; i != 6; ++i)
    out[i] = 1;
  C(e, void, 22, held);
  CHECK(!C(e, int, 228));
  return C(vm, int, 5);
}
