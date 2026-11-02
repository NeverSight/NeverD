//===- X86_64_NoReturnCallTests.cpp - Calls of functions that never return
//-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A caller whose compiler knew a function never returns lays out nothing
/// after a call of it but the code of another path.  A PLT entry of exit
/// jumps through the slot the loader binds to exit, so it never returns, and
/// neither does a function whose every path ends in a call of it.  Where no
/// proof reaches the callee, the call that ends its function's code still
/// ends its path.
///
/// The fixture, test_noreturn_calls_x64.s, is GCC 15.2's output for such
/// functions and callers, linked and stripped: the proof needs no symbol of
/// the image, only the import the loader binds.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using namespace neverd;

namespace {

/// The fixture \p File, linked by ld.lld.
std::optional<BinaryImage> loadFixture(llvm::StringRef File) {
  const std::filesystem::path Path =
      std::filesystem::path(TEST_OBJ_DIR) / File.str();
  if (!std::filesystem::exists(Path))
    return std::nullopt;
  auto ImageOrErr = loadBinary(Path);
  if (!ImageOrErr) {
    ADD_FAILURE() << llvm::toString(ImageOrErr.takeError());
    return std::nullopt;
  }
  return std::move(*ImageOrErr);
}

const HighFunc *functionAt(const PipelineResult &Result, va_t Entry) {
  for (const HighFunc &Func : Result.HighFuncs)
    if (Func.Entry == Entry)
      return &Func;
  return nullptr;
}

const HighFunc *functionNamed(const PipelineResult &Result,
                              llvm::StringRef Name) {
  for (const HighFunc &Func : Result.HighFuncs)
    if (Func.Name == Name)
      return &Func;
  return nullptr;
}

/// The direct calls \p Body makes, in order: as statements, or as the values
/// statements assign or return.
std::vector<const HighExpr *> callsOf(const std::vector<HighStmt> &Body) {
  std::vector<const HighExpr *> Calls;
  std::function<void(const ExprPtr &)> Find = [&](const ExprPtr &E) {
    if (!E)
      return;
    if (E->Kind == ExprKind::Call && !E->IsIndirectCall && E->CallAddr)
      Calls.push_back(E.get());
    for (const ExprPtr &Operand : E->Operands)
      Find(Operand);
  };
  std::function<void(const std::vector<HighStmt> &)> Walk =
      [&](const std::vector<HighStmt> &Stmts) {
        for (const HighStmt &S : Stmts) {
          forEachExpr(S, Find);
          Walk(S.Body);
          Walk(S.ElseBody);
          for (const SwitchCase &Case : S.Cases)
            Walk(Case.Body);
          Walk(S.DefaultBody);
          for (const std::vector<HighStmt> &Clause : S.EHClauseBodies)
            Walk(Clause);
        }
      };
  Walk(Body);
  return Calls;
}

struct Decompiled {
  PipelineResult Result;
  std::string Source;
};

Decompiled decompileAll(const BinaryImage &Img, llvm::LLVMContext &Ctx) {
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Decompiled Out{Pipeline().run(Img, Ctx, Opts), {}};
  if (!Out.Result.Success) {
    ADD_FAILURE() << Out.Result.Error;
    return Out;
  }
  llvm::raw_string_ostream OS(Out.Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  if (!HighCEmitter().emit(Out.Result.HighFuncs, OS, Options))
    ADD_FAILURE() << "emission failed";
  return Out;
}

// plt_die's one call is of the PLT entry of exit, which jumps through exit's
// slot: both never return.
TEST(X86_64NoReturnCall, AJumpThroughTheSlotOfExitNeverReturns) {
  const std::optional<BinaryImage> Img =
      loadFixture("test_noreturn_calls_x64_stripped.so");
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  llvm::LLVMContext Ctx;
  const Decompiled Out = decompileAll(*Img, Ctx);
  const HighFunc *Pick = functionNamed(Out.Result, "plt_pick");
  ASSERT_NE(Pick, nullptr);
  const std::vector<const HighExpr *> PickCalls = callsOf(Pick->Body);
  ASSERT_FALSE(PickCalls.empty());
  const HighFunc *Die = functionAt(Out.Result, PickCalls.front()->CallAddr);
  ASSERT_NE(Die, nullptr);
  EXPECT_TRUE(Die->DoesNotReturn) << Out.Source;
  const std::vector<const HighExpr *> DieCalls = callsOf(Die->Body);
  ASSERT_EQ(DieCalls.size(), 1u) << Out.Source;
  EXPECT_TRUE(DieCalls[0]->DoesNotReturn) << Out.Source;
  const HighFunc *Stub = functionAt(Out.Result, DieCalls[0]->CallAddr);
  ASSERT_NE(Stub, nullptr) << Out.Source;
  EXPECT_TRUE(Stub->DoesNotReturn) << Out.Source;
}

// Each of plt_pick's calls of plt_die ends its path: the code after the first
// is the second's, which only `x == 2` reaches.
TEST(X86_64NoReturnCall, ACallOfAFunctionEndingInExitEndsItsPath) {
  const std::optional<BinaryImage> Img =
      loadFixture("test_noreturn_calls_x64_stripped.so");
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  llvm::LLVMContext Ctx;
  const Decompiled Out = decompileAll(*Img, Ctx);
  const HighFunc *Pick = functionNamed(Out.Result, "plt_pick");
  ASSERT_NE(Pick, nullptr);
  const std::vector<const HighExpr *> Calls = callsOf(Pick->Body);
  ASSERT_EQ(Calls.size(), 2u) << Out.Source;
  for (const HighExpr *Call : Calls)
    EXPECT_TRUE(Call->DoesNotReturn) << Out.Source;
  // The C says so, where plt_die is declared.
  const std::string Name = Calls[0]->CallTarget;
  const size_t Declaration = Out.Source.find(" " + Name + "(");
  ASSERT_NE(Declaration, std::string::npos) << Out.Source;
  const size_t Line = Out.Source.rfind('\n', Declaration) + 1;
  const size_t End = Out.Source.find(";\n", Declaration);
  ASSERT_NE(End, std::string::npos) << Out.Source;
  const std::string Text = Out.Source.substr(Line, End - Line);
  EXPECT_TRUE(Text.find("_Noreturn") != std::string::npos ||
              Text.find("__attribute__((noreturn))") != std::string::npos)
      << Out.Source;
}

// spin_forever loops forever with no call or trap, but its loop lifts
// completely, which alone proves it never returns.  spin_pick's second call
// of it is also the last instruction of spin_pick's code: the path ends
// there, and the C returns nothing after it.
TEST(X86_64NoReturnCall, ACallThatEndsItsFunctionsCodeEndsItsPath) {
  const std::optional<BinaryImage> Img =
      loadFixture("test_noreturn_calls_x64_stripped.so");
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  llvm::LLVMContext Ctx;
  const Decompiled Out = decompileAll(*Img, Ctx);
  const HighFunc *Pick = functionNamed(Out.Result, "spin_pick");
  ASSERT_NE(Pick, nullptr);
  const std::vector<const HighExpr *> Calls = callsOf(Pick->Body);
  ASSERT_FALSE(Calls.empty()) << Out.Source;
  const HighExpr *Last = Calls.front();
  for (const HighExpr *Call : Calls)
    if (Call->CallSiteAddr > Last->CallSiteAddr)
      Last = Call;
  EXPECT_TRUE(Last->DoesNotReturn) << Out.Source;
  const HighFunc *Spin = functionAt(Out.Result, Last->CallAddr);
  ASSERT_NE(Spin, nullptr);
  EXPECT_TRUE(Spin->DoesNotReturn) << Out.Source;
  // No path of spin_pick returns after a call of spin_forever.
  const size_t Begin = Out.Source.find(" spin_pick(int");
  ASSERT_NE(Begin, std::string::npos) << Out.Source;
  const std::string Definition =
      Out.Source.substr(Begin, Out.Source.find("\n}\n", Begin) - Begin);
  const std::string Callee = Last->CallTarget + "(";
  for (size_t At = Definition.find(Callee); At != std::string::npos;
       At = Definition.find(Callee, At + 1)) {
    const size_t Next = Definition.find('\n', At);
    if (Next == std::string::npos)
      break;
    const size_t After = Definition.find_first_not_of(" ", Next + 1);
    if (After != std::string::npos)
      EXPECT_NE(Definition.compare(After, 6, "return"), 0) << Definition;
  }
}

} // namespace
