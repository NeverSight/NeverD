//===- LLVMCNonHeaderRegionTests.cpp - Internal scalar loop exits ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"

namespace {
void replaceAll(std::string &Text, llvm::StringRef From, llvm::StringRef To) {
  for (size_t At = 0; (At = Text.find(From.str(), At)) != std::string::npos;) {
    Text.replace(At, From.size(), To.str());
    At += To.size();
  }
}

/// Where \p Source defines the function whose header starts with \p Header:
/// the header's line that opens its body.  A whole module declares each
/// definition before the bodies (DefinitionDeclarations), which a unit of
/// one function leaves out.
size_t definitionAt(llvm::StringRef Source, llvm::StringRef Header) {
  for (size_t At = Source.find(Header); At != llvm::StringRef::npos;
       At = Source.find(Header, At + 1)) {
    const size_t End = Source.find('\n', At);
    if (Source.slice(At, End).rtrim().ends_with("{"))
      return At;
  }
  return llvm::StringRef::npos;
}

std::string emit(const std::string &IR) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Error;
  auto M = llvm::parseAssemblyString(IR, Error, Context);
  if (!M) {
    ADD_FAILURE() << Error.getMessage().str();
    return {};
  }
  EXPECT_FALSE(llvm::verifyModule(*M, &llvm::errs()));
  auto Text = [&] {
    std::string S;
    llvm::raw_string_ostream OS(S);
    M->print(OS, nullptr);
    return S;
  };
  const auto Before = Text();
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  std::string Whole, Selected;
  llvm::raw_string_ostream Out(Whole), SelectedOut(Selected);
  EXPECT_TRUE(neverd::LLVMCEmitter().emit(*M, Out, Options));
  EXPECT_TRUE(neverd::LLVMCEmitter().emit(*M, SelectedOut, Options, nullptr,
                                          nullptr, M->getFunction("exercise")));
  auto At = definitionAt(Whole, " exercise("),
       SelectedAt = definitionAt(Selected, " exercise(");
  EXPECT_NE(At, std::string::npos);
  EXPECT_NE(SelectedAt, std::string::npos);
  if (At != std::string::npos && SelectedAt != std::string::npos)
    EXPECT_EQ(Whole.substr(At), Selected.substr(SelectedAt));
  EXPECT_EQ(Before, Text());
  return Whole;
}

void runOracle(const std::string &IR, const std::string &C,
               const std::string &Harness) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  auto Found = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Found));
  const std::string Compiler = *Found;
#endif
  llvm::SmallString<128> Paths[5];
  std::vector<std::unique_ptr<llvm::FileRemover>> Cleanup;
  const char *Suffixes[] = {"ll", "c", "c", "exe", "err"};
  for (unsigned I = 0; I < 5; ++I) {
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-internal-loop-exit",
                                                    Suffixes[I], Paths[I]));
    Cleanup.push_back(std::make_unique<llvm::FileRemover>(Paths[I]));
  }
  const std::string *Contents[] = {&IR, &C, &Harness};
  for (unsigned I = 0; I < 3; ++I) {
    std::error_code Error;
    llvm::raw_fd_ostream Out(Paths[I], Error);
    ASSERT_FALSE(Error);
    Out << *Contents[I];
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, Paths[4].str()};
  for (unsigned Input = 0; Input < 2; ++Input) {
    for (const char *Level : {"-O0", "-O2"}) {
      SCOPED_TRACE(Input);
      SCOPED_TRACE(Level);
      llvm::SmallVector<llvm::StringRef, 16> Args{Compiler,
                                                  "-std=c11",
                                                  Level,
                                                  "-Werror=uninitialized",
                                                  "-Werror=return-type",
                                                  Paths[Input],
                                                  Paths[2],
                                                  "-o",
                                                  Paths[3]};
      if (Input) {
        Args.push_back("-fsanitize=undefined");
        Args.push_back("-fsanitize-trap=all");
      }
      std::string Error;
      auto Built = llvm::sys::ExecuteAndWait(Compiler, Args, std::nullopt,
                                             Redirects, 30, 0, &Error);
      auto Errors = llvm::MemoryBuffer::getFile(Paths[4]);
      ASSERT_EQ(Built, 0) << Error
                          << (Errors ? (*Errors)->getBuffer().str() : "")
                          << '\n'
                          << C;
      auto Ran = llvm::sys::ExecuteAndWait(Paths[3], {Paths[3]}, std::nullopt,
                                           Redirects, 30, 0, &Error);
      ASSERT_EQ(Ran, 0) << Error << '\n' << C;
    }
  }
}

// The independent oracle records every observer call, including its value.
// Native LLVM and emitted C must both match the high-level loop and trace.
std::string harness(llvm::StringRef Body, bool Wide = false) {
  return std::string(R"(
#include <stdint.h>
static uint64_t trace, expected_trace;
static uint32_t calls, expected_calls;
static void record(uint64_t *t, uint32_t tag, uint32_t value) {
  *t = (*t ^ ((uint64_t)tag << 32) ^ value) * UINT64_C(1099511628211);
}
void observe(uint32_t tag, uint32_t value) { record(&trace,tag,value); ++calls; }
static void expect(uint32_t tag, uint32_t value) {
  record(&expected_trace,tag,value); ++expected_calls;
}
extern )") +
         (Wide ? "uint64_t" : "uint32_t") + R"( exercise(uint32_t,uint32_t);
int main(void) {
  uint32_t random=0x1379bdfu;
  for (unsigned n=0; n<256; ++n) {
    for (unsigned sample=0; sample<32; ++sample) {
      random=random*1664525u+1013904223u;
      uint32_t seed=sample==0?0:sample==1?UINT32_MAX:random;
      trace=expected_trace=UINT64_C(1469598103934665603);
      calls=expected_calls=0;
      uint64_t expected;
)" + Body.str() +
         R"(
      uint64_t actual=exercise(seed,n);
      if (actual!=expected || trace!=expected_trace || calls!=expected_calls)
        return 1;
    }
  }
  return 0;
})";
}

constexpr char TailCopies[] = R"(
declare void @observe(i32, i32)
define i64 @exercise(i32 %seed, i32 %n) {
entry:
 %bound = and i32 %n, 7
 %initial.b = xor i32 %seed, 165
 %empty = icmp eq i32 %bound, 0
 br i1 %empty, label %exit, label %head
head:
 %i = phi i32 [0, %entry], [%next, %latch]
 %a = phi i32 [%seed, %entry], [%b, %latch]
 %b = phi i32 [%initial.b, %entry], [%a, %latch]
 call void @observe(i32 1, i32 %a)
 br label %body
body:
 call void @observe(i32 2, i32 %b)
 %next = add i32 %i, 1
 %done = icmp eq i32 %next, %bound
 br i1 %done, label %exit, label %latch
latch:
 call void @observe(i32 3, i32 %next)
 br label %head
exit:
 %out.a = phi i32 [%seed, %entry], [%b, %body]
 %out.b = phi i32 [%initial.b, %entry], [%a, %body]
 call void @observe(i32 4, i32 %out.a)
 call void @observe(i32 5, i32 %out.b)
 %lo = zext i32 %out.a to i64
 %hi = zext i32 %out.b to i64
 %shift = shl i64 %hi, 32
 %result = or i64 %lo, %shift
 ret i64 %result
})";

TEST(LLVMCInternalExitRegions, ExitAndBackedgeCopiesRetainParallelValues) {
  for (bool Invert : {false, true}) {
    std::string IR = TailCopies;
    if (Invert) {
      replaceAll(IR, "icmp eq i32 %next, %bound", "icmp ne i32 %next, %bound");
      replaceAll(IR, "br i1 %done, label %exit, label %latch",
                 "br i1 %done, label %latch, label %exit");
    }
    const auto C = emit(IR);
    EXPECT_EQ(C.find("goto "), std::string::npos) << C;
    EXPECT_NE(C.find("break;"), std::string::npos) << C;
    runOracle(IR, C,
              harness(R"(
      uint32_t a=seed,b=seed^165u,bound=n&7u;
      for (unsigned i=0; i<bound; ++i) {
        expect(1,a); expect(2,b);
        uint32_t old=a; a=b; b=old;
        if (i+1<bound) expect(3,i+1);
      }
      expect(4,a); expect(5,b);
      expected=(uint64_t)a|((uint64_t)b<<32);
)",
                      true));
  }
}

std::string nested(bool OuterTail, bool InnerTail) {
  std::string IR = R"(
declare void @observe(i32, i32)
define i32 @exercise(i32 %seed, i32 %n) {
entry:
 %rows = and i32 %n, 3
 %shift = lshr i32 %n, 2
 %cols = and i32 %shift, 3
 %empty = icmp eq i32 %rows, 0
 br i1 %empty, label %exit, label %outer
outer:
 %row = phi i32 [0, %entry], [%next.row, %outer.latch]
 %state = phi i32 [%seed, %entry], [%updated, %outer.latch]
 call void @observe(i32 1, i32 %state)
 $OUTER_HEAD
setup:
 %start = xor i32 %state, %row
 $INNER_ENTRY
inner:
 %col = phi i32 [0, %setup], [%next.col, %inner.latch]
 %value = phi i32 [%start, %setup], [%sum, %inner.latch]
 call void @observe(i32 2, i32 %value)
 $INNER_HEAD
inner.body:
 %offset = add i32 %row, 13
 %sum = add i32 %value, %offset
 %next.col = add i32 %col, 1
 call void @observe(i32 3, i32 %sum)
 $INNER_TEST
inner.latch:
 call void @observe(i32 4, i32 %next.col)
 br label %inner
after.inner:
 $INNER_RESULT
 %updated = xor i32 %inner.result, %state
 call void @observe(i32 5, i32 %updated)
 %next.row = add i32 %row, 1
 $OUTER_TEST
outer.latch:
 call void @observe(i32 6, i32 %next.row)
 br label %outer
exit:
 $RESULT
 call void @observe(i32 7, i32 %out)
 ret i32 %out
})";
  replaceAll(IR, "$OUTER_HEAD",
             OuterTail ? "br label %setup"
                       : "%outer.more = icmp ult i32 %row, %rows\n"
                         " br i1 %outer.more, label %setup, label %exit");
  replaceAll(IR, "$INNER_ENTRY",
             InnerTail ? "%inner.empty = icmp eq i32 %cols, 0\n"
                         " br i1 %inner.empty, label %after.inner, label %inner"
                       : "br label %inner");
  replaceAll(IR, "$INNER_HEAD",
             InnerTail
                 ? "br label %inner.body"
                 : "%inner.more = icmp ult i32 %col, %cols\n"
                   " br i1 %inner.more, label %inner.body, label %after.inner");
  replaceAll(IR, "$INNER_TEST",
             InnerTail
                 ? "%inner.done = icmp eq i32 %next.col, %cols\n"
                   " br i1 %inner.done, label %after.inner, label %inner.latch"
                 : "br label %inner.latch");
  replaceAll(
      IR, "$INNER_RESULT",
      InnerTail
          ? "%inner.result = phi i32 [%start, %setup], [%sum, %inner.body]"
          : "%inner.result = phi i32 [%value, %inner]");
  replaceAll(IR, "$OUTER_TEST",
             OuterTail ? "%outer.done = icmp eq i32 %next.row, %rows\n"
                         " br i1 %outer.done, label %exit, label %outer.latch"
                       : "br label %outer.latch");
  replaceAll(IR, "$RESULT",
             OuterTail
                 ? "%out = phi i32 [%seed, %entry], [%updated, %after.inner]"
                 : "%out = phi i32 [%seed, %entry], [%state, %outer]");
  return IR;
}

TEST(LLVMCInternalExitRegions,
     NestedHeaderAndInternalExitsKeepEveryObservation) {
  for (bool OuterTail : {false, true}) {
    for (bool InnerTail : {false, true}) {
      SCOPED_TRACE(OuterTail);
      SCOPED_TRACE(InnerTail);
      auto IR = nested(OuterTail, InnerTail);
      const auto C = emit(IR);
      EXPECT_EQ(C.find("goto "), std::string::npos) << C;
      auto Body = std::string("      const int outer_tail=") +
                  (OuterTail ? "1" : "0") +
                  ",inner_tail=" + (InnerTail ? "1" : "0") + ";\n" + R"(
      uint32_t state=seed, rows=n&3u, cols=(n>>2)&3u;
      if (rows) for (unsigned row=0;;++row) {
        expect(1,state);
        if (!outer_tail && row==rows) break;
        uint32_t value=state^row;
        if (!inner_tail || cols) for (unsigned col=0;;++col) {
          expect(2,value);
          if (!inner_tail && col==cols) break;
          value+=row+13u; expect(3,value);
          if (inner_tail && col+1==cols) break;
          expect(4,col+1);
        }
        state=value^state; expect(5,state);
        if (outer_tail && row+1==rows) break;
        expect(6,row+1);
      }
      expect(7,state); expected=state;
)";
      runOracle(IR, C, harness(Body));
    }
  }
}

constexpr char MultipleExits[] = R"(
declare void @observe(i32, i32)
define i32 @exercise(i32 %seed, i32 %n) {
entry:
 %bound = and i32 %n, 7
 %bit = and i32 %seed, 1
 %odd = icmp ne i32 %bit, 0
 br label %head
head:
 %i = phi i32 [0, %entry], [%next, %latch]
 %v = phi i32 [%seed, %entry], [%sum, %latch]
 call void @observe(i32 1, i32 %v)
 %more = icmp ult i32 %i, %bound
 br i1 %more, label %body, label %exit
body:
 %sum = add i32 %v, 5
 call void @observe(i32 2, i32 %sum)
 %one = icmp eq i32 %i, 1
 %early = and i1 %odd, %one
 br i1 %early, label %exit, label %latch
latch:
 %next = add i32 %i, 1
 br label %head
exit:
 %out = phi i32 [%v, %head], [%sum, %body]
 call void @observe(i32 3, i32 %out)
 ret i32 %out
})";

TEST(LLVMCInternalExitRegions, SharedDestinationRetainsDistinctExitValues) {
  const auto C = emit(MultipleExits);
  EXPECT_EQ(C.find("goto "), std::string::npos) << C;
  runOracle(MultipleExits, C, harness(R"(
      uint32_t v=seed;
      for (unsigned i=0;;++i) {
        expect(1,v);
        if (i==(n&7u)) break;
        v+=5u; expect(2,v);
        if ((seed&1u) && i==1) break;
      }
      expect(3,v); expected=v;
)"));
}

TEST(LLVMCInternalExitRegions,
     HeaderAndBodyExitsKeepDistinctUncoalescedValues) {
  for (bool HeaderWork : {false, true}) {
    for (bool Invert : {false, true}) {
      SCOPED_TRACE(HeaderWork);
      SCOPED_TRACE(Invert);
      std::string IR = MultipleExits;
      // Keep the header's exit value distinct from the carried value. PHI
      // coalescing must not hide an incorrectly repeated header-edge copy.
      replaceAll(IR, "[%v, %head], [%sum, %body]",
                 "[%seed, %head], [%sum, %body]");
      if (!HeaderWork)
        replaceAll(IR, " call void @observe(i32 1, i32 %v)\n", "");
      if (Invert) {
        replaceAll(IR, "icmp ult i32 %i, %bound", "icmp uge i32 %i, %bound");
        replaceAll(IR, "br i1 %more, label %body, label %exit",
                   "br i1 %more, label %exit, label %body");
      }
      const auto C = emit(IR);
      EXPECT_EQ(C.find("goto "), std::string::npos) << C;
      std::string Body = R"(
      uint32_t v=seed;
      for (unsigned i=0;;++i) {
        HEADER_OBSERVER
        if (i==(n&7u)) { v=seed; break; }
        v+=5u; expect(2,v);
        if ((seed&1u) && i==1) break;
      }
      expect(3,v); expected=v;
)";
      replaceAll(Body, "HEADER_OBSERVER", HeaderWork ? "expect(1,v);" : "");
      runOracle(IR, C, harness(Body));
    }
  }
}

TEST(LLVMCInternalExitRegions, FinalIterationReloadRetainsValueAndPointer) {
  const std::string IR = R"(
declare void @advance(ptr)
define i32 @exercise(ptr %words, i32 %limit) {
entry:
 %other = getelementptr [4 x i32], ptr %words, i64 0, i64 1
 %saved = getelementptr [4 x i32], ptr %words, i64 0, i64 2
 br label %head
head:
 %i = phi i32 [0, %entry], [%next, %body]
 %before = load i32, ptr %words
 %ready = icmp ne i32 %before, 0
 br i1 %ready, label %exit, label %body
body:
 call void @advance(ptr %words)
 %next = add i32 %i, 1
 %done = icmp uge i32 %next, %limit
 br i1 %done, label %reload, label %head
reload:
 %after = load i32, ptr %words
 br label %exit
exit:
 %value = phi i32 [%before, %head], [%after, %reload]
 %destination = phi ptr [%words, %head], [%other, %reload]
 store i32 %value, ptr %saved
 %marked = xor i32 %value, 90
 store i32 %marked, ptr %destination
 ret i32 %value
})";
  const auto C = emit(IR);
  EXPECT_EQ(C.find("goto "), std::string::npos) << C;
  runOracle(IR, C, R"(
#include <stdint.h>
#include <string.h>
uint32_t words[4];
static uint32_t calls, arrival, payload;
void advance(void *storage) {
  uint32_t *p=storage;
  ++calls;
  if (calls==arrival) p[0]=payload;
}
extern uint32_t exercise(void *,uint32_t);
int main(void) {
  const uint32_t values[]={1,0x80000000u,UINT32_MAX};
  for (unsigned limit=1;limit<=8;++limit)
    for (arrival=0;arrival<=10;++arrival)
      for (unsigned v=0;v<3;++v) {
        payload=values[v]; calls=0;
        words[0]=arrival==0?payload:0;
        words[1]=0x12345678u; words[2]=0x87654321u; words[3]=0xabcdef01u;
        uint32_t expected[4]; memcpy(expected,words,sizeof words);
        unsigned count=0, destination=0;
        while (!expected[0]) {
          ++count;
          if (count==arrival) expected[0]=payload;
          if (count>=limit) { destination=1; break; }
        }
        uint32_t result=expected[0];
        expected[2]=result; expected[destination]=result^90u;
        if (exercise(words,limit)!=result || calls!=count ||
            memcmp(words,expected,sizeof words)) return 1;
      }
  return 0;
})");
}

TEST(LLVMCInternalExitRegions, EarlyContinuationRetainsCompleteFallback) {
  const std::string IR = R"(
declare void @observe(i32, i32)
define i32 @exercise(i32 %seed, i32 %n) {
entry:
 %bits = and i32 %n, 3
 %bound = add i32 %bits, 1
 br label %head
head:
 %i = phi i32 [0, %entry], [%next, %latch]
 call void @observe(i32 1, i32 %i)
 %bit = and i32 %i, 1
 %skip = icmp ne i32 %bit, 0
 br i1 %skip, label %latch, label %test
test:
 %done = icmp uge i32 %i, %bound
 br i1 %done, label %exit, label %latch
latch:
 call void @observe(i32 2, i32 %i)
 %next = add i32 %i, 1
 br label %head
exit:
 %result = add i32 %i, %seed
 ret i32 %result
})";
  const auto C = emit(IR);
  EXPECT_NE(C.find("goto "), std::string::npos) << C;
  runOracle(IR, C, harness(R"(
      unsigned i=0;
      for (;;++i) {
        expect(1,i);
        if (!(i&1u) && i>=(n&3u)+1u) break;
        expect(2,i);
      }
      expected=(uint32_t)(seed+i);
)"));
}

TEST(LLVMCInternalExitRegions, FailedLaterRegionPublishesNoPartialStructure) {
  std::string IR = MultipleExits;
  // Two effectful exit destinations still require the complete fallback.
  replaceAll(IR, "br i1 %early, label %exit, label %latch",
             "br i1 %early, label %early.exit, label %latch");
  replaceAll(IR, "exit:\n %out", R"(early.exit:
 call void @observe(i32 3, i32 %sum)
 ret i32 %sum
exit:
 %out)");
  replaceAll(IR, "[%v, %head], [%sum, %body]", "[%v, %head]");
  replaceAll(IR, "%entry", "%prefix.end");
  replaceAll(IR, "entry:\n", R"(entry:
 br label %prefix
prefix:
 %j = phi i32 [0, %entry], [%jn, %prefix.body]
 %again = icmp ult i32 %j, 2
 br i1 %again, label %prefix.body, label %prefix.end
prefix.body:
 call void @observe(i32 11, i32 %j)
 %jn = add i32 %j, 1
 br label %prefix
prefix.end:
)");
  const auto C = emit(IR);
  EXPECT_NE(C.find("goto "), std::string::npos) << C;
  EXPECT_EQ(C.find("while ("), std::string::npos) << C;
  EXPECT_EQ(C.find("for ("), std::string::npos) << C;
  runOracle(IR, C, harness(R"(
      expect(11,0); expect(11,1);
      uint32_t v=seed;
      for (unsigned i=0;;++i) {
        expect(1,v);
        if (i==(n&7u)) break;
        v+=5u; expect(2,v);
        if ((seed&1u) && i==1) break;
      }
      expect(3,v); expected=v;
)"));
}
} // namespace
