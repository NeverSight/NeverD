//===- X86_32_DebugHighCTests.cpp - VC6 debug facts in HighC -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/NdTypes.h"
#include "neverd/ir/high/HighIR.h"

#include "llvm/Support/raw_ostream.h"

#include <map>
#include <string>
#include <vector>

using namespace neverd;

namespace {

class Vc6DebugContext : public DebugContext {
public:
  FunctionSym Function;
  std::map<int64_t, VariableSym> Locals;

  std::optional<FunctionSym> resolveFunction(va_t Addr) const override {
    if (Addr == Function.Addr)
      return Function;
    return std::nullopt;
  }
  std::optional<VariableSym> resolveVariable(va_t FuncAddr,
                                             int64_t Offset) const override {
    if (FuncAddr != Function.Addr)
      return std::nullopt;
    const auto It = Locals.find(Offset);
    if (It == Locals.end())
      return std::nullopt;
    return It->second;
  }
  std::optional<TypeSym> resolveType(uint64_t) const override {
    return std::nullopt;
  }
  std::optional<SourceLoc> sourceLocation(va_t) const override {
    return std::nullopt;
  }
  std::vector<FunctionSym> allFunctions() const override { return {Function}; }
  bool hasInfo() const override { return true; }
};

} // namespace

TEST(X86_32_DebugHighC, StdcallParamsAndLocalsAppearInC) {
  HighFunc Func;
  Func.Name = "legacy_target";
  Func.Entry = 0x401100;
  Func.ReturnType = NdType::makeInt(4, true);

  HighParam Left;
  Left.Name = "arg0";
  Left.Type = NdType::makeInt(4, true);
  HighParam Right;
  Right.Name = "arg1";
  Right.Type = NdType::makeInt(4, true);
  Func.Params = {Left, Right};

  HighLocal Scratch;
  Scratch.Name = "var_4";
  Scratch.Type = NdType::makeInt(4, true);
  Scratch.StackOff = -4;
  Func.Locals.push_back(Scratch);

  MedVar Param0;
  Param0.Kind = MedVar::Param;
  Param0.Id = 0;
  Param0.Size = 4;
  MedVar Stack;
  Stack.Kind = MedVar::Stack;
  Stack.StackOff = -4;
  Stack.Size = 4;

  HighStmt Assign;
  Assign.Kind = StmtKind::Assign;
  Assign.Dst = HighExpr::makeVar(Stack, Scratch.Type);
  Assign.Val = HighExpr::makeVar(Param0, Left.Type);
  Func.Body.push_back(Assign);
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = HighExpr::makeVar(Stack, Scratch.Type);
  Func.Body.push_back(Ret);

  Vc6DebugContext Dbg;
  Dbg.Function.Name = "legacy_target";
  Dbg.Function.Addr = 0x401100;
  Dbg.Function.Size = 0x20;
  Dbg.Function.CallConv = DebugCallConv::Stdcall;
  Dbg.Function.ReturnType = NdType::makeInt(4, true);
  Dbg.Function.Params = {{"left", NdType::makeInt(4, true)},
                         {"right", NdType::makeInt(4, true)}};
  VariableSym ScratchSym;
  ScratchSym.Name = "scratch";
  ScratchSym.Type = NdType::makeInt(4, true);
  ScratchSym.StackOffset = -4;
  Dbg.Locals[-4] = ScratchSym;

  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Opts;
  Opts.TheArch = Arch::X86;
  Opts.Format = BinaryFormat::COFF;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Opts, &Dbg));
  OS.flush();

  EXPECT_NE(C.find("legacy_target"), std::string::npos) << C;
  EXPECT_NE(C.find("__attribute__((stdcall))"), std::string::npos) << C;
  EXPECT_NE(C.find("left"), std::string::npos) << C;
  EXPECT_NE(C.find("scratch"), std::string::npos) << C;
}

TEST(X86_32_DebugHighC, EmptyBodyEmitsTrapNotSilentBraces) {
  HighFunc Func;
  Func.Name = "empty_target";
  Func.Entry = 0x401200;
  Func.ReturnType = NdType::makeInt(4, false);

  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Opts;
  Opts.TheArch = Arch::X86;
  Opts.Format = BinaryFormat::COFF;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Opts, nullptr));
  OS.flush();

  EXPECT_NE(C.find("empty_target"), std::string::npos) << C;
  EXPECT_NE(C.find("neverd.entry: 0x401200"), std::string::npos) << C;
  EXPECT_NE(C.find("__builtin_trap"), std::string::npos) << C;
  EXPECT_NE(C.find("no structured body"), std::string::npos) << C;
  // A silent `name() {}` is not a decompiled body.
  EXPECT_EQ(C.find("empty_target(void) {\n}"), std::string::npos) << C;
  EXPECT_EQ(C.find("empty_target(void) {}"), std::string::npos) << C;
}

TEST(X86_32_DebugHighC, EmptyKeepIdentityCannotCountAsDecompileSuccess) {
  HighFunc Func;
  Func.Name = "keep_identity";
  Func.Entry = 0x401300;
  Func.ReturnType = NdType::makeInt(4, false);

  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Opts;
  Opts.TheArch = Arch::X86;
  Opts.Format = BinaryFormat::COFF;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Opts, nullptr));
  OS.flush();

  const size_t Brace = C.find("keep_identity(void) {");
  ASSERT_NE(Brace, std::string::npos) << C;
  const size_t Close = C.find('}', Brace);
  ASSERT_NE(Close, std::string::npos) << C;
  const std::string Body = C.substr(Brace, Close - Brace);
  std::string Stripped = Body;
  for (const char *Tok :
       {"/* neverd: no structured body */", "__builtin_trap();",
        "keep_identity(void) {", " ", "\n", "\t"}) {
    for (size_t P = Stripped.find(Tok); P != std::string::npos;
         P = Stripped.find(Tok))
      Stripped.erase(P, std::char_traits<char>::length(Tok));
  }
  EXPECT_TRUE(Stripped.empty()) << "trap-only keepIdentity leftover:\n" << C;
  EXPECT_NE(C.find("__builtin_trap"), std::string::npos) << C;
}

TEST(X86_32_DebugHighC, ThiscallQualifiedNameAndBodyAppearInC) {
  HighFunc Func;
  Func.Name = "CBase::GetId";
  Func.Entry = 0x401400;
  Func.ReturnType = NdType::makeInt(4, true);

  HighParam This;
  This.Name = "this";
  This.Type = NdType::makePtr();
  Func.Params = {This};

  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = HighExpr::makeConst(7, 4);
  Func.Body.push_back(Ret);

  Vc6DebugContext Dbg;
  Dbg.Function.Name = "CBase::GetId";
  Dbg.Function.Addr = 0x401400;
  Dbg.Function.Size = 0x10;
  Dbg.Function.CallConv = DebugCallConv::Thiscall;
  Dbg.Function.ReturnType = NdType::makeInt(4, true);
  Dbg.Function.Params = {{"this", NdType::makePtr()}};

  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Opts;
  Opts.TheArch = Arch::X86;
  Opts.Format = BinaryFormat::COFF;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Opts, &Dbg));
  OS.flush();

  EXPECT_NE(C.find("CBase_GetId"), std::string::npos) << C;
  EXPECT_NE(C.find("__attribute__((thiscall))"), std::string::npos) << C;
  EXPECT_NE(C.find("return"), std::string::npos) << C;
  EXPECT_EQ(C.find("no structured body"), std::string::npos) << C;
  EXPECT_EQ(C.find("CBase_GetId(void) {\n}"), std::string::npos) << C;
}

TEST(X86_32_DebugHighC, GotoSkeletonEmitsLabelsNotTrap) {
  HighFunc Func;
  Func.Name = "wide_cfg";
  Func.Entry = 0x401000;
  Func.ReturnType = NdType::makeInt(4, false);

  HighStmt Mark;
  Mark.Kind = StmtKind::Nop;
  Mark.Addr = 0x401000;
  Func.Body.push_back(Mark);
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = 0x401010;
  Func.Body.push_back(Jump);
  HighStmt Lab;
  Lab.Kind = StmtKind::Nop;
  Lab.Addr = 0x401010;
  Func.Body.push_back(Lab);
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Func.Body.push_back(Ret);

  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Opts;
  Opts.TheArch = Arch::X86;
  Opts.Format = BinaryFormat::COFF;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Opts, nullptr));
  OS.flush();

  EXPECT_NE(C.find("goto L_"), std::string::npos) << C;
  EXPECT_EQ(C.find("no structured body"), std::string::npos) << C;
  EXPECT_EQ(C.find("__builtin_trap"), std::string::npos) << C;
}

TEST(X86_32_DebugHighC, SynthesizedNameTakesDebugFunctionName) {
  HighFunc Func;
  Func.Name = "sub_401100";
  Func.Entry = 0x401100;
  Func.ReturnType = NdType::makeInt(4, true);
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = HighExpr::makeConst(1, 4);
  Func.Body.push_back(Ret);

  Vc6DebugContext Dbg;
  Dbg.Function.Name = "legacy_target";
  Dbg.Function.Addr = 0x401100;
  Dbg.Function.Size = 0x20;

  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Opts;
  Opts.TheArch = Arch::X86;
  Opts.Format = BinaryFormat::COFF;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Opts, &Dbg));
  OS.flush();

  EXPECT_NE(C.find("legacy_target"), std::string::npos) << C;
  EXPECT_EQ(C.find("sub_401100"), std::string::npos) << C;
}
