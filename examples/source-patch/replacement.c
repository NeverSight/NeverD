//===- replacement.c - Saved original and new imports example -------------===//
//
// NeverD Decompiler
//
/// \file
/// Replace target(X) with its saved body plus a new library's function/data.
//
//===----------------------------------------------------------------------===//

#ifdef _WIN32
#define IMPORT __declspec(dllimport)
#else
#define IMPORT
#endif

extern int saved_target(int);
IMPORT int extra_function(int);
IMPORT extern int extra_state;

int replacement(int X) {
  return saved_target(X) + extra_function(X) + extra_state;
}
