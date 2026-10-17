//===- original.c - Original source patch example -------------------------===//
//
// NeverD Decompiler
//
/// \file
/// A small native program whose exported target will be replaced.
//
//===----------------------------------------------------------------------===//

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

EXPORT __attribute__((noinline)) int target(int X) {
  volatile int Value = X;
  return Value + 1;
}

int main(int Argc, char **Argv) {
  if (Argc != 2)
    return 2;
  printf("%d\n", target(atoi(Argv[1])));
  return 0;
}
