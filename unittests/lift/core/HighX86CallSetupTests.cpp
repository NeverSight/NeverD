//===- HighX86CallSetupTests.cpp - Live outgoing stack addresses ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute recovered x86 stack arithmetic across calls and nested uses.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

using namespace neverd;

namespace {
ExprPtr scalar(int Id, bool Parameter = false) {
  MedVar V;
  V.Kind = Parameter ? MedVar::Param : MedVar::Temp;
  V.Id = Id;
  V.Size = 4;
  V.TheArch = Arch::X86;
  return HighExpr::makeVar(V, NdType::makeInt(4, false));
}

ExprPtr offset(ExprPtr Base, NdOp Op, unsigned Bytes) {
  return HighExpr::makeBinop(Op, std::move(Base),
                             HighExpr::makeConst(Bytes, 4));
}

HighStmt result(ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Return;
  S.RetVal = std::move(Value);
  return S;
}

std::string source(bool Live, bool Nested) {
  HighFunc Callee;
  Callee.Entry = 0x402000;
  Callee.Name = "consume_word";
  Callee.ReturnType = NdType::makeVoid();
  Callee.Params = {{"word", NdType::makeInt(4, false)}};
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Callee.Body.push_back(Return);

  HighFunc Func;
  Func.Entry = 0x401000;
  Func.Name = "stack_after_call";
  Func.ReturnType = NdType::makeInt(4, false);
  Func.Params = {{"stack_base", NdType::makeInt(4, false)},
                 {"word", NdType::makeInt(4, false)},
                 {"choose", NdType::makeInt(4, false)}};
  for (unsigned I = 1; I <= 3; ++I) {
    auto Base = I == 1 ? scalar(0, true) : scalar(I - 1);
    auto Address = offset(Base, NdOp::INT_SUB, 4);
    HighStmt Assign;
    Assign.Kind = StmtKind::Assign;
    Assign.Dst = scalar(I);
    Assign.Val = Address;
    Func.Body.push_back(Assign);
    HighStmt Store;
    Store.Kind = StmtKind::Store;
    Store.StoreAddr = Address;
    Store.StoreVal = scalar(1, true);
    Func.Body.push_back(Store);
  }
  HighStmt Call;
  Call.Kind = StmtKind::Call;
  Call.CallExpr =
      HighExpr::makeCall(Callee.Name, Callee.Entry, {scalar(1, true)});
  Call.CallExpr->Type = Callee.ReturnType;
  Func.Body.push_back(Call);
  auto Restored = Live ? offset(scalar(3), NdOp::INT_ADD, 12) : scalar(0, true);
  if (Nested) {
    HighStmt Branch;
    Branch.Kind = StmtKind::IfElse;
    Branch.Cond = scalar(2, true);
    Branch.Body.push_back(result(Restored));
    Branch.ElseBody.push_back(
        result(Live ? offset(scalar(2), NdOp::INT_ADD, 8) : scalar(0, true)));
    Func.Body.push_back(Branch);
  } else {
    Func.Body.push_back(result(Restored));
  }
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X86;
  Options.EmitComments = false;
  EXPECT_TRUE(HighCEmitter().emit({Callee, Func}, OS, Options));
  return Source;
}

void compileAndRun(const std::string &Source,
                   llvm::StringRef Optimization = "-O1") {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program)) << "clang is required for recovered C execution";
  const std::string Compiler = *Program;
#endif
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-x86-call", "c", SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-x86-call", "exe", BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-x86-call", "err", ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream OS(SourcePath, EC);
    ASSERT_FALSE(EC);
    OS << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  llvm::SmallVector<llvm::StringRef, 12> Arguments{Compiler,
                                                   "-std=c11",
                                                   Optimization,
                                                   "-Werror=uninitialized",
                                                   "-Werror=return-type",
                                                   SourcePath,
                                                   "-o",
                                                   BinaryPath};
  std::string Error;
  const int Compiled = llvm::sys::ExecuteAndWait(
      Compiler, Arguments, std::nullopt, Redirects, 30, 0, &Error);
  auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
  ASSERT_EQ(Compiled, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << "\n"
                         << Source;
  const int Ran = llvm::sys::ExecuteAndWait(
      BinaryPath, {BinaryPath}, std::nullopt, Redirects, 30, 0, &Error);
  ASSERT_EQ(Ran, 0) << Error << "\n" << Source;
}

TEST(HighX86CallSetup, LiveAddressDependenciesExecuteAfterCalleePop) {
  for (bool Nested : {false, true}) {
    SCOPED_TRACE(Nested);
    const std::string Source = source(true, Nested);
    // Only the scalar address chain is live. The outgoing memory writes
    // belong to the recovered call and must still disappear.
    EXPECT_EQ(Source.find("*("), std::string::npos) << Source;
    compileAndRun(Source + R"(
int main(void) {
    const uint32_t bases[] = {0, 1, 12, 4096, 0x80000000u, 0xffffffffu};
    for (unsigned i = 0; i < sizeof(bases) / sizeof(bases[0]); ++i)
        for (unsigned choose = 0; choose < 2; ++choose)
            if (stack_after_call(bases[i], 17, choose) != bases[i]) return 1;
    return 0;
}
)");
  }
}

TEST(HighX86CallSetup, UnusedOutgoingSetupStillDisappears) {
  const std::string Source = source(false, false);
  EXPECT_EQ(Source.find("*("), std::string::npos) << Source;
  EXPECT_EQ(Source.find(" - 4"), std::string::npos) << Source;
}

TEST(HighX86CallSetup, AnImageLoadPassedToTheCallKeepsItsDefinition) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
    for (bool Nested : {false, true}) {
      SCOPED_TRACE(static_cast<int>(Format));
      SCOPED_TRACE(Nested);
      constexpr va_t Word = 0x404000;
      BinaryImage Image;
      Image.Arch = Arch::X86;
      Image.Bits = Bitness::Bits32;
      Image.Format = Format;
      Segment Data;
      Data.VA = Word;
      Data.Size = Data.FileSz = 4;
      Data.Data.resize(4);
      Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Image.Segments.push_back(std::move(Data));

      HighFunc Setter;
      Setter.Name = "set_image_word";
      Setter.Entry = 0x401000;
      Setter.ReturnType = NdType::makeVoid();
      Setter.Params = {{"word", NdType::makeInt(4, false)}};
      HighStmt Set;
      Set.Kind = StmtKind::Store;
      Set.StoreAddr = HighExpr::makeConst(Word, 4);
      Set.StoreVal = scalar(0, true);
      Setter.Body = {Set, result(nullptr)};

      HighFunc Callee;
      Callee.Name = "consume_loaded";
      Callee.Entry = 0x402000;
      Callee.ReturnType = NdType::makeInt(4, false);
      Callee.Params = {{"loaded", Callee.ReturnType},
                       {"word", Callee.ReturnType}};
      Callee.Body = {result(HighExpr::makeBinop(
          NdOp::INT_ADD,
          HighExpr::makeBinop(NdOp::INT_MULT, scalar(0, true),
                              HighExpr::makeConst(31, 4)),
          scalar(1, true)))};

      HighFunc Func;
      Func.Name = "image_call";
      Func.Entry = 0x403000;
      Func.ReturnType = Callee.ReturnType;
      Func.Params = {{"stack_base", Callee.ReturnType},
                     {"choose", Callee.ReturnType}};
      HighStmt Load;
      Load.Kind = StmtKind::Assign;
      Load.Dst = scalar(10);
      Load.Val =
          HighExpr::makeLoad(HighExpr::makeConst(Word, 4), Callee.ReturnType);
      HighStmt Adjust;
      Adjust.Kind = StmtKind::Assign;
      Adjust.Dst = scalar(11);
      Adjust.Val = offset(scalar(0, true), NdOp::INT_SUB, 4);
      HighStmt Push;
      Push.Kind = StmtKind::Store;
      Push.StoreAddr = Adjust.Val;
      Push.StoreVal = HighExpr::makeConst(7, 4);
      HighStmt Call;
      Call.Kind = StmtKind::Assign;
      Call.Dst = scalar(12);
      Call.Val = HighExpr::makeCall(Callee.Name, Callee.Entry,
                                    {scalar(10), HighExpr::makeConst(7, 4)});
      Call.Val->Type = Callee.ReturnType;
      Func.Body = {Load, Adjust, Push, Call, result(scalar(12))};
      if (Nested) {
        HighStmt Branch;
        Branch.Kind = StmtKind::IfElse;
        Branch.Cond = scalar(1, true);
        Branch.Body = std::move(Func.Body);
        Branch.ElseBody = {result(HighExpr::makeConst(0, 4))};
        Func.Body = {Branch};
      }

      std::string Source;
      llvm::raw_string_ostream OS(Source);
      CEmitterOptions Options;
      Options.TheArch = Arch::X86;
      Options.Format = Format;
      Options.Image = &Image;
      Options.EmitComments = false;
      ASSERT_TRUE(HighCEmitter().emit({Setter, Callee, Func}, OS, Options));
      // Changing the backing word between calls distinguishes a live load
      // from an invented zero or a stale cached value. A zero stack base is
      // safe only when the physical outgoing PUSH is correctly projected.
      for (llvm::StringRef Optimization : {"-O0", "-O2"})
        compileAndRun(Source + R"(
int main(void) {
    const uint32_t words[] = {0, 17, 0x80000001u, 0xffffffffu};
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
        set_image_word(words[i]);
        if (image_call(0, 1) != words[i] * 31u + 7u) return 1;
    }
    return 0;
}
)",
                      Optimization);
    }
}
} // namespace
