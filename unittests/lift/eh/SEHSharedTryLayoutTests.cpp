//===- SEHSharedTryLayoutTests.cpp - Blocks that share a __try ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// LLVM C prints a function whose SEH ranges are fragmented block by block,
/// each block inside the `__try` of every scope it runs in.  Blocks that
/// follow one another under the same scopes share those tries as far as C
/// lets them: a jump may not enter a `__try`, so a block that code outside
/// the try jumps to opens a try of its own.
///
/// A `__finally` printed that way runs its termination code unless a flag
/// says the try was left normally.  A jump sets the flag where it leaves the
/// try of a `__finally`, and a block under one clears it where a jump may
/// reach the block with the flag set.
///
//===----------------------------------------------------------------------===//

#include "PrintedTries.h"
#include "gtest/gtest.h"

#include "neverd/Common.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace neverd;
using printed_tries::jumpIntoATry;

constexpr const char *Prelude = R"ir(
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"
declare void @work(i32)
declare void @ended(i8, ptr)
declare void @llvm.seh.try.begin()
declare void @llvm.seh.try.end()
declare i32 @__C_specific_handler(...)
)ir";

/// The definition of \p Name in the LLVM C of \p Body, from its name to its
/// closing brace.
std::string printedFunction(const char *Body, llvm::StringRef Name) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(std::string(Prelude) + Body,
                                          Diagnostic, Context);
  if (!Module) {
    ADD_FAILURE() << Diagnostic.getMessage().str() << " at line "
                  << Diagnostic.getLineNo();
    return {};
  }
  std::string Verification;
  llvm::raw_string_ostream VerificationOS(Verification);
  if (llvm::verifyModule(*Module, &VerificationOS)) {
    ADD_FAILURE() << Verification;
    return {};
  }
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  Options.TheArch = Arch::X64;
  Options.PreserveLLVMFunctionTypes = true;
  EXPECT_TRUE(LLVMCEmitter().emit(*Module, OS, Options));
  OS.flush();
  // For the check that runs both: the IR and the C it prints, side by side.
  if (const char *Directory = std::getenv("NEVERD_SEH_LAYOUT_OUTPUT")) {
    const auto Write = [&](const char *Extension, llvm::StringRef Text) {
      std::error_code Error;
      llvm::raw_fd_ostream File(
          (llvm::Twine(Directory) + "/" + Name + Extension).str(), Error);
      EXPECT_FALSE(Error) << Error.message();
      File << Text;
    };
    Write(".ll", std::string(Prelude) + Body);
    Write(".c", Source);
  }
  const size_t Start = Source.find((Name + "(").str());
  if (Start == std::string::npos) {
    ADD_FAILURE() << Source;
    return {};
  }
  const size_t End = Source.find("\n}\n", Start);
  return Source.substr(Start, End == std::string::npos ? End : End + 3 - Start);
}

size_t countOf(llvm::StringRef Text, llvm::StringRef Needle) {
  size_t Count = 0;
  for (size_t At = Text.find(Needle); At != llvm::StringRef::npos;
       At = Text.find(Needle, At + 1))
    ++Count;
  return Count;
}

/// The text from the label \p Label to the next label.
llvm::StringRef blockAt(llvm::StringRef Function, llvm::StringRef Label) {
  const size_t At = Function.find(("\n" + Label + ":\n").str());
  if (At == llvm::StringRef::npos)
    return {};
  size_t End = At + 1;
  for (End = Function.find('\n', End); End != llvm::StringRef::npos;
       End = Function.find('\n', End + 1))
    if (End + 1 < Function.size() && Function[End + 1] != ' ' &&
        Function[End + 1] != '}')
      break;
  return Function.slice(At + 1, End);
}

// Two blocks in an outer scope with two blocks of an inner scope between
// them: each scope prints one try, around all of its blocks.
TEST(SEHSharedTryLayout, BlocksUnderTheSameScopesShareTheirTries) {
  const std::string Function = printedFunction(R"ir(
define i32 @nested() personality ptr @__C_specific_handler {
entry:
  invoke void @llvm.seh.try.begin() to label %outer.a unwind label %outer.pad
outer.a:
  invoke void @work(i32 1) to label %inner.begin unwind label %outer.pad
inner.begin:
  invoke void @llvm.seh.try.begin() to label %inner.a unwind label %inner.pad
inner.a:
  invoke void @work(i32 2) to label %inner.b unwind label %inner.pad
inner.b:
  invoke void @work(i32 3) to label %inner.end unwind label %inner.pad
inner.end:
  invoke void @llvm.seh.try.end() to label %outer.b unwind label %inner.pad
outer.b:
  invoke void @work(i32 4) to label %outer.end unwind label %outer.pad
outer.end:
  invoke void @llvm.seh.try.end() to label %done unwind label %outer.pad
done:
  ret i32 0
inner.pad:
  %inner.switch = catchswitch within none [label %inner.catch] unwind label %outer.pad
inner.catch:
  %inner.caught = catchpad within %inner.switch [ptr null]
  catchret from %inner.caught to label %inner.handler
inner.handler:
  br label %done
outer.pad:
  %outer.switch = catchswitch within none [label %outer.catch] unwind to caller
outer.catch:
  %outer.caught = catchpad within %outer.switch [ptr null]
  catchret from %outer.caught to label %outer.handler
outer.handler:
  br label %done
}
)ir",
                                               "nested");
  ASSERT_FALSE(Function.empty());
  EXPECT_EQ(jumpIntoATry(Function), "") << Function;
  EXPECT_EQ(countOf(Function, "__try {"), 2u) << Function;
  EXPECT_EQ(countOf(Function, "} __except ("), 2u) << Function;
  for (const char *Call : {"work(1)", "work(2)", "work(3)", "work(4)"})
    EXPECT_EQ(countOf(Function, Call), 1u) << Call << '\n' << Function;
  // The inner scope's try opens after the first call and closes before the
  // last, inside the outer one.
  const size_t Outer = Function.find("__try {");
  const size_t Inner = Function.find("__try {", Outer + 1);
  const size_t InnerEnd = Function.find("} __except (");
  const size_t OuterEnd = Function.find("} __except (", InnerEnd + 1);
  EXPECT_LT(Outer, Function.find("work(1)")) << Function;
  EXPECT_LT(Function.find("work(1)"), Inner) << Function;
  EXPECT_LT(Inner, Function.find("work(2)")) << Function;
  EXPECT_LT(Function.find("work(3)"), InnerEnd) << Function;
  EXPECT_LT(InnerEnd, Function.find("work(4)")) << Function;
  EXPECT_LT(Function.find("work(4)"), OuterEnd) << Function;
}

// Two scopes one after the other: each block stands in the try of its own
// scope, though both are one try deep.
TEST(SEHSharedTryLayout, BlocksUnderDifferentScopesKeepTheirOwnTries) {
  const std::string Function = printedFunction(R"ir(
define i32 @siblings(i1 %early) personality ptr @__C_specific_handler {
entry:
  invoke void @llvm.seh.try.begin() to label %a unwind label %first.pad
a:
  invoke void @work(i32 1) to label %first.end unwind label %first.pad
first.end:
  invoke void @llvm.seh.try.end() to label %second.begin unwind label %first.pad
second.begin:
  invoke void @llvm.seh.try.begin() to label %b unwind label %second.pad
b:
  invoke void @work(i32 2) to label %b.next unwind label %second.pad
b.next:
  br i1 %early, label %leave, label %second.end
leave:
  ret i32 1
second.end:
  invoke void @llvm.seh.try.end() to label %done unwind label %second.pad
done:
  ret i32 0
first.pad:
  %switch = catchswitch within none [label %catch] unwind to caller
catch:
  %caught = catchpad within %switch [ptr null]
  catchret from %caught to label %handler
handler:
  br label %done
second.pad:
  %cleanup = cleanuppad within none []
  call void @ended(i8 1, ptr null) [ "funclet"(token %cleanup) ]
  cleanupret from %cleanup unwind to caller
}
)ir",
                                               "siblings");
  ASSERT_FALSE(Function.empty());
  EXPECT_EQ(jumpIntoATry(Function), "") << Function;
  EXPECT_EQ(countOf(Function, "__try {"), 2u) << Function;
  EXPECT_EQ(countOf(Function, "} __except ("), 1u) << Function;
  EXPECT_EQ(countOf(Function, "} __finally {"), 1u) << Function;
  const std::vector<unsigned> First =
      printed_tries::at(Function, Function.find("work(1)"));
  const std::vector<unsigned> Second =
      printed_tries::at(Function, Function.find("work(2)"));
  ASSERT_EQ(First.size(), 1u) << Function;
  ASSERT_EQ(Second.size(), 1u) << Function;
  EXPECT_NE(First.front(), Second.front()) << Function;
  // The first try ends in the `__except`, the second in the `__finally`.
  EXPECT_LT(Function.find("} __except ("), Function.find("work(2)"))
      << Function;
  EXPECT_LT(Function.find("work(2)"), Function.find("} __finally {"))
      << Function;
}

// A scope that two paths enter at different blocks: the second block is
// jumped to from outside the scope, so it cannot stand inside the try the
// first block opened.  Each of the two opens its own.
TEST(SEHSharedTryLayout, ABlockJumpedToFromOutsideOpensItsOwnTry) {
  const std::string Function = printedFunction(R"ir(
define i32 @entered_twice(i1 %late) personality ptr @__C_specific_handler {
entry:
  br i1 %late, label %begin.b, label %begin.a
begin.a:
  invoke void @llvm.seh.try.begin() to label %a unwind label %pad
begin.b:
  invoke void @llvm.seh.try.begin() to label %b unwind label %pad
a:
  invoke void @work(i32 1) to label %b unwind label %pad
b:
  invoke void @work(i32 2) to label %end unwind label %pad
end:
  invoke void @llvm.seh.try.end() to label %done unwind label %pad
done:
  ret i32 0
pad:
  %switch = catchswitch within none [label %catch] unwind to caller
catch:
  %caught = catchpad within %switch [ptr null]
  catchret from %caught to label %handler
handler:
  br label %done
}
)ir",
                                               "entered_twice");
  ASSERT_FALSE(Function.empty());
  EXPECT_EQ(jumpIntoATry(Function), "") << Function;
  EXPECT_EQ(countOf(Function, "__try {"), 2u) << Function;
  EXPECT_EQ(countOf(Function, "} __except ("), 2u) << Function;
  EXPECT_EQ(countOf(Function, "work(1)"), 1u) << Function;
  EXPECT_EQ(countOf(Function, "work(2)"), 1u) << Function;
}

// Taking a block out of a shared try can leave another jump outside the try
// of its label: here the jump from the first block to the third, once the
// second, which code printed after all three jumps to, opens a try of its
// own.  The third then opens one too.
TEST(SEHSharedTryLayout, ASplitTryIsSplitAgainWhereAJumpNowEntersIt) {
  const std::string Function = printedFunction(R"ir(
define i32 @split_twice(i1 %late, i1 %skip) personality ptr @__C_specific_handler {
entry:
  br i1 %late, label %later, label %begin.a
begin.a:
  invoke void @llvm.seh.try.begin() to label %a unwind label %pad
a:
  invoke void @work(i32 1) to label %a.next unwind label %pad
a.next:
  br i1 %skip, label %c, label %b
b:
  invoke void @work(i32 2) to label %c unwind label %pad
c:
  invoke void @work(i32 3) to label %end unwind label %pad
end:
  invoke void @llvm.seh.try.end() to label %done unwind label %pad
done:
  ret i32 0
later:
  call void @work(i32 9)
  br label %begin.b
begin.b:
  invoke void @llvm.seh.try.begin() to label %b unwind label %pad
pad:
  %switch = catchswitch within none [label %catch] unwind to caller
catch:
  %caught = catchpad within %switch [ptr null]
  catchret from %caught to label %handler
handler:
  br label %done
}
)ir",
                                               "split_twice");
  ASSERT_FALSE(Function.empty());
  EXPECT_EQ(jumpIntoATry(Function), "") << Function;
  for (const char *Call : {"work(1)", "work(2)", "work(3)", "work(9)"})
    EXPECT_EQ(countOf(Function, Call), 1u) << Call << '\n' << Function;
  // The third block is reached from the first block's try and from the
  // second's: it stands in neither.
  EXPECT_EQ(countOf(Function, "__try {"), 3u) << Function;
  EXPECT_EQ(printed_tries::depthOf(Function, "work(9)"), 0) << Function;
}

// An `__except` whose handler resumes inside its own scope, at the scope's
// second block: the clause stands outside its try, so that block cannot stay
// inside the try the first block opens.
TEST(SEHSharedTryLayout,
     AHandlerThatResumesInsideItsOwnScopeEntersFromOutside) {
  const std::string Function = printedFunction(R"ir(
define i32 @resumed() personality ptr @__C_specific_handler {
entry:
  invoke void @llvm.seh.try.begin() to label %a unwind label %pad
a:
  invoke void @work(i32 1) to label %b unwind label %pad
b:
  invoke void @work(i32 2) to label %end unwind label %pad
end:
  invoke void @llvm.seh.try.end() to label %done unwind label %pad
done:
  ret i32 0
pad:
  %switch = catchswitch within none [label %catch] unwind to caller
catch:
  %caught = catchpad within %switch [ptr null]
  catchret from %caught to label %handler
handler:
  invoke void @llvm.seh.try.begin() to label %b unwind label %pad
}
)ir",
                                               "resumed");
  ASSERT_FALSE(Function.empty());
  EXPECT_EQ(jumpIntoATry(Function), "") << Function;
  EXPECT_EQ(countOf(Function, "__try {"), 2u) << Function;
  EXPECT_EQ(countOf(Function, "} __except ("), 2u) << Function;
  EXPECT_EQ(countOf(Function, "goto L_b;"), 3u) << Function;
}

// A `__finally` scope with a loop and a return inside it.  The jump back to
// the scope's first block leaves the try, whose label stands before it, and
// so do the return and the jump past the scope's end: each says first that
// it is no exception.  A jump on to the next block of the try says nothing,
// and that block clears nothing.
TEST(SEHSharedTryLayout, TheNormalExitFlagIsSetAndClearedWhereATryIsLeft) {
  const std::string Function = printedFunction(R"ir(
define i32 @looped(i1 %again, i1 %early) personality ptr @__C_specific_handler {
entry:
  invoke void @llvm.seh.try.begin() to label %head unwind label %pad
head:
  invoke void @work(i32 1) to label %tail unwind label %pad
tail:
  invoke void @work(i32 2) to label %tail.next unwind label %pad
tail.next:
  br i1 %again, label %head, label %check
check:
  br i1 %early, label %leave, label %end
leave:
  ret i32 1
end:
  invoke void @llvm.seh.try.end() to label %done unwind label %pad
done:
  ret i32 0
pad:
  %cleanup = cleanuppad within none []
  call void @ended(i8 1, ptr null) [ "funclet"(token %cleanup) ]
  cleanupret from %cleanup unwind to caller
}
)ir",
                                               "looped");
  ASSERT_FALSE(Function.empty());
  EXPECT_EQ(jumpIntoATry(Function), "") << Function;
  EXPECT_EQ(countOf(Function, "__try {"), 1u) << Function;
  EXPECT_EQ(countOf(Function, "} __finally {"), 1u) << Function;
  EXPECT_EQ(countOf(Function, "if (!neverd_seh_normal) {"), 1u) << Function;

  const llvm::StringRef Head = blockAt(Function, "L_head");
  ASSERT_FALSE(Head.empty()) << Function;
  // The first block clears the flag before its try, and jumps on inside it.
  EXPECT_TRUE(
      Head.starts_with("L_head:\n    neverd_seh_normal = 0;\n    __try {\n"))
      << Head.str();
  EXPECT_EQ(countOf(Head, "neverd_seh_normal = 1;"), 0u) << Head.str();
  const llvm::StringRef Tail = blockAt(Function, "L_tail");
  ASSERT_FALSE(Tail.empty()) << Function;
  EXPECT_EQ(countOf(Tail, "neverd_seh_normal"), 0u) << Tail.str();
  // Back to the first block: out of the try.
  EXPECT_NE(Function.find("neverd_seh_normal = 1;\n            goto L_head;"),
            std::string::npos)
      << Function;
  // On to the block that returns: still inside it.
  EXPECT_NE(Function.find("{\n            goto L_leave;"), std::string::npos)
      << Function;
  EXPECT_NE(Function.find("neverd_seh_normal = 1;\n        return 1;"),
            std::string::npos)
      << Function;
  EXPECT_NE(Function.find("neverd_seh_normal = 1;\n            goto L_done;"),
            std::string::npos)
      << Function;
  EXPECT_EQ(countOf(Function, "neverd_seh_normal = 0;"), 1u) << Function;
  EXPECT_EQ(countOf(Function, "neverd_seh_normal = 1;"), 3u) << Function;
}

// An `__except` inside a `__finally` whose handler goes on under the
// `__finally`: the clause's jump stays inside the outer try, so it sets no
// flag, and the handler's block, reached with the flag clear, clears nothing.
// A block reached from outside the `__finally` does clear it.
TEST(SEHSharedTryLayout, AHandlerUnderTheScopeAroundItsClauseStaysInThatTry) {
  const std::string Function = printedFunction(R"ir(
define i32 @handled_inside(i1 %early) personality ptr @__C_specific_handler {
entry:
  invoke void @llvm.seh.try.begin() to label %outer.a unwind label %outer.pad
outer.a:
  invoke void @work(i32 1) to label %inner.begin unwind label %outer.pad
inner.begin:
  invoke void @llvm.seh.try.begin() to label %inner.a unwind label %inner.pad
inner.a:
  invoke void @work(i32 2) to label %inner.end unwind label %inner.pad
inner.end:
  invoke void @llvm.seh.try.end() to label %outer.b unwind label %inner.pad
handler:
  invoke void @llvm.seh.try.begin() to label %outer.b unwind label %outer.pad
outer.b:
  invoke void @work(i32 3) to label %outer.next unwind label %outer.pad
outer.next:
  br i1 %early, label %leave, label %outer.end
leave:
  ret i32 1
outer.end:
  invoke void @llvm.seh.try.end() to label %done unwind label %outer.pad
done:
  ret i32 0
inner.pad:
  %inner.switch = catchswitch within none [label %inner.catch] unwind label %outer.pad
inner.catch:
  %inner.caught = catchpad within %inner.switch [ptr null]
  catchret from %inner.caught to label %handler
outer.pad:
  %cleanup = cleanuppad within none []
  call void @ended(i8 1, ptr null) [ "funclet"(token %cleanup) ]
  cleanupret from %cleanup unwind to caller
}
)ir",
                                               "handled_inside");
  ASSERT_FALSE(Function.empty());
  EXPECT_EQ(jumpIntoATry(Function), "") << Function;
  EXPECT_EQ(countOf(Function, "__try {"), 2u) << Function;
  EXPECT_EQ(countOf(Function, "} __finally {"), 1u) << Function;
  EXPECT_EQ(countOf(Function, "} __except ("), 1u) << Function;
  // The clause's jump to the handler's block, and nothing before it.
  EXPECT_NE(Function.find(") {\n            goto L_outer_b;\n        }"),
            std::string::npos)
      << Function;
  // Only the scope's first block clears the flag.
  EXPECT_EQ(countOf(Function, "neverd_seh_normal = 0;"), 1u) << Function;
  const llvm::StringRef First = blockAt(Function, "L_outer_a");
  ASSERT_FALSE(First.empty()) << Function;
  EXPECT_TRUE(First.starts_with(
      "L_outer_a:\n    neverd_seh_normal = 0;\n    __try {\n"))
      << First.str();
  // The return inside the scope and the jump out of it at its end set it.
  EXPECT_EQ(countOf(Function, "neverd_seh_normal = 1;"), 2u) << Function;
}

} // namespace
