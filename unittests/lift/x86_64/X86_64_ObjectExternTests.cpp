//===- X86_64_ObjectExternTests.cpp - Data an object leaves undefined -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A relocatable object reads data another module defines.  The C of both
// routes declares that data and reads the program's, as directly addressed
// code and code reaching it through a GOT entry do.  Each kernel is
// decompiled and run on the host against its own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

class X86_64_ObjectExtern : public HighCHostExecutionTest {
protected:
  /// Expect both routes' C of \p Kernel, built with and without PIC when
  /// \p BothModels and position-independent otherwise, to run like it.
  void
  expectBothRoutesRunLikeSource(const char *Kernel, bool BothModels,
                                std::initializer_list<const char *> Names) {
    for (const bool Direct : {false, true}) {
      if (Direct && !BothModels)
        continue;
      SCOPED_TRACE(Direct ? "-fno-pic" : "GOT");
      for (const bool Route : {false, true}) {
        SCOPED_TRACE(Route ? "LLVM C" : "HighC");
        LLVMC = Route;
        std::vector<std::string> Flags = {"-target", "x86_64-linux-gnu"};
        if (Direct)
          Flags.push_back("-fno-pic");
        ASSERT_NO_FATAL_FAILURE(expectHighCRunsLikeSource(
            Flags, Kernel, "entry", Names, {-3, 0, 1, 77}));
      }
    }
  }
};

// The kernel, built at its optimization level, leaves Ext undefined, and the
// reference copy, built without optimization, defines it: both C read that
// one definition, which neither writes.
TEST_F(X86_64_ObjectExtern, DataAnotherModuleDefinesIsDeclared) {
  expectBothRoutesRunLikeSource(R"C(
#ifdef __OPTIMIZE__
extern int Ext;
#else
int Ext = 41;
#endif
int gx(int a) __attribute__((noinline));
int gx(int a) { return Ext * 3 + a; }
int entry(int a) { return gx(a) + gx(1); }
)C",
                                true, {"entry", "gx"});
}

// The C library's stdout, read directly or through its GOT entry, is the
// program's.  Without PIC the format string's absolute address in a 32-bit
// register is its whole address once zero-extended.
TEST_F(X86_64_ObjectExtern, TheLibrarysStreamIsTheProgramsOwn) {
  expectBothRoutesRunLikeSource(R"C(
#include <stdio.h>
int ff(int a) __attribute__((noinline));
int ff(int a) { return fprintf(stdout, "%d\n", a); }
int entry(int a) { return ff(a) * 3 + ff(-a); }
)C",
                                true, {"entry", "ff"});
}
