#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCMethods.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <functional>
#include <initializer_list>
#include <set>

namespace {
using namespace neverd;

TEST(ObjCSourceTypeHints, PreservesConstantReturnAndUnusedHiddenParameters) {
  MedFunc Func;
  Func.Name = "answer";
  Func.Entry = 0x1000;
  SourceFunctionTypeHint Hint;
  Hint.ReturnType = NdType::makeInt(4);
  Hint.Parameters = {{"objc_self", NdType::makePtr(NdType::makeVoid())},
                     {"objc_cmd", NdType::makePtr(NdType::makeVoid())}};
  Func.SourceTypeHint = Hint;
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Func.Entry;
  MedOp Set;
  Set.Opcode = NdOp::COPY;
  Set.Output.Kind = MedVar::Reg;
  Set.Output.TheArch = Arch::AArch64;
  Set.Output.Id = 1;
  Set.Output.SSAVer = 1;
  Set.Output.Size = 4;
  Set.Output.RegOff = getTargetRegInfo(Arch::AArch64).IntReturnReg;
  Set.addInput(MedVar::makeConst(42, 4));
  Block.Ops.push_back(Set);
  MedOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Block.Ops.push_back(Ret);
  Func.Blocks.push_back(Block);

  inferMedTypes(Func, Arch::AArch64);
  EXPECT_EQ(Func.ReturnValueEvidence, MedReturnValueEvidence::Unknown);
  ASSERT_EQ(Func.TypedParams.size(), 2U);
  auto High = MedToHighConverter().convert(Func, Arch::AArch64);
  ASSERT_EQ(High.Params.size(), 2U);
  EXPECT_EQ(High.Params[0].Name, "objc_self");
  EXPECT_EQ(High.Params[1].Name, "objc_cmd");
  ASSERT_TRUE(High.SourceTypeHint);
  EXPECT_EQ(High.ReturnType->Size, 4);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
  OS.flush();
  EXPECT_NE(Source.find("return 42;"), std::string::npos) << Source;
  EXPECT_NE(Source.find("objc_self"), std::string::npos) << Source;
  EXPECT_NE(Source.find("objc_cmd"), std::string::npos) << Source;
}

BinaryImage runtimeImage() {
  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Format = BinaryFormat::MachO;
  Img.Bits = Bitness::Bits64;
  Segment Seg;
  Seg.VA = 0x1000;
  Seg.Size = Seg.FileSz = 0x1000;
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data.resize(Seg.Size);
  Img.Segments.push_back(Seg);
  auto AddSection = [&](const char *Name, uint64_t Off, uint64_t Size,
                        bool Code = false) {
    Section Sec;
    Sec.Name = Name;
    Sec.VA = 0x1000 + Off;
    Sec.FileOff = Off;
    Sec.Size = Sec.FileSz = Size;
    Sec.Flags = SegmentFlags::Readable;
    if (Code) {
      Sec.Flags = Sec.Flags | SegmentFlags::Executable;
      Sec.Type = static_cast<uint32_t>(llvm::MachO::S_REGULAR) |
                 static_cast<uint32_t>(llvm::MachO::S_ATTR_PURE_INSTRUCTIONS);
    }
    Img.Sections.push_back(Sec);
  };
  AddSection("__text", 0x100, 0x100, true);
  AddSection("__objc_classlist", 0x200, 8);
  AddSection("__objc_const", 0x300, 0x700);
  auto W64 = [&](size_t O, uint64_t V) {
    llvm::support::endian::write64le(Img.Segments[0].Data.data() + O, V);
  };
  auto W32 = [&](size_t O, uint32_t V) {
    llvm::support::endian::write32le(Img.Segments[0].Data.data() + O, V);
  };
  W32(0x100, 0xd65f03c0); // executable AArch64 RET
  W64(0x200, 0x1300);     // class
  W64(0x320, 0x1400);     // class_ro_t
  W64(0x418, 0x1600);     // class name
  W64(0x420, 0x1500);     // methods
  W32(0x500, 24);
  W32(0x504, 1);
  W64(0x508, 0x1620);
  W64(0x510, 0x1640);
  W64(0x518, 0x1100);
  const char Name[] = "Calculator";
  const char Selector[] = "answer";
  const char Encoding[] = "i16@0:8";
  std::memcpy(Img.Segments[0].Data.data() + 0x600, Name, sizeof(Name));
  std::memcpy(Img.Segments[0].Data.data() + 0x620, Selector, sizeof(Selector));
  std::memcpy(Img.Segments[0].Data.data() + 0x640, Encoding, sizeof(Encoding));
  return Img;
}

std::optional<HighFunc> recoverMethodCode(std::initializer_list<uint32_t> Code,
                                          const char *Selector,
                                          const char *Encoding) {
  auto Img = runtimeImage();
  Img.Base = 0x1000;
  Img.Entry = 0x1100;
  size_t Offset = 0x100;
  for (uint32_t Instruction : Code) {
    llvm::support::endian::write32le(Img.Segments[0].Data.data() + Offset,
                                     Instruction);
    Offset += 4;
  }
  Img.Sections[0].Size = Img.Sections[0].FileSz = Code.size() * 4;
  auto *Data = Img.Segments[0].Data.data();
  std::memcpy(Data + 0x620, Selector, std::strlen(Selector) + 1);
  std::memcpy(Data + 0x640, Encoding, std::strlen(Encoding) + 1);
  parseObjCMethods(Img);
  llvm::LLVMContext Context;
  Pipeline ThePipeline;
  PipelineOptions Options;
  Options.MaxFunctions = 1;
  Options.EmitDumpOutput = false;
  auto Result = ThePipeline.run(Img, Context, Options);
  EXPECT_TRUE(Result.Success) << Result.Error;
  for (const auto &Func : Result.HighFuncs)
    if (Func.Entry == Img.Entry)
      return Func;
  return std::nullopt;
}

TEST(ObjCSourceTypeHints, BareReturnPreservesDeclaredReceiver) {
  // AArch64 returning self is a single RET: the native instruction has no
  // explicit X0 operand for ordinary live-in parameter discovery to observe.
  auto Func = recoverMethodCode({0xd65f03c0}, "selfValue", "@16@0:8");
  ASSERT_TRUE(Func);
  ASSERT_TRUE(Func->SourceTypeHint);
  bool ReturnsSelf = false;
  walkStmts(Func->Body, [&](const HighStmt &Stmt) {
    if (Stmt.Kind == StmtKind::Return && Stmt.RetVal &&
        Stmt.RetVal->Kind == ExprKind::Var &&
        Stmt.RetVal->Var.Kind == MedVar::Param && Stmt.RetVal->Var.Id == 0)
      ReturnsSelf = true;
  });
  EXPECT_TRUE(ReturnsSelf);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  ASSERT_TRUE(HighCEmitter().emit({*Func}, OS, Options));
  OS.flush();
  EXPECT_EQ(Source.find("v_1_0"), std::string::npos) << Source;
}

TEST(ObjCSourceTypeHints, BareReturnWithoutDeclarationDoesNotInventReceiver) {
  auto Func = recoverMethodCode({0xd65f03c0}, "selfValue", "?16@0:8");
  ASSERT_TRUE(Func);
  EXPECT_FALSE(Func->SourceTypeHint);
  EXPECT_TRUE(Func->Params.empty());
}

TEST(ObjCSourceTypeHints, UnknownCallPreventsIncomingReceiverReturnFallback) {
  MedFunc Func;
  Func.Entry = 0x1000;
  Func.Name = "unknown_call";
  SourceFunctionTypeHint Hint;
  Hint.ReturnType = NdType::makePtr(NdType::makeVoid());
  Hint.Parameters = {{"objc_self", Hint.ReturnType},
                     {"objc_cmd", Hint.ReturnType}};
  Func.SourceTypeHint = Hint;
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Func.Entry;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.addInput(MedVar::makeConst(0x5000, 8));
  Block.Ops.push_back(Call);
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Block.Ops.push_back(Return);
  Func.Blocks.push_back(Block);
  inferMedTypes(Func, Arch::AArch64);
  auto High = MedToHighConverter().convert(Func, Arch::AArch64);
  walkStmts(High.Body, [&](const HighStmt &Stmt) {
    if (Stmt.Kind != StmtKind::Return || !Stmt.RetVal)
      return;
    EXPECT_FALSE(Stmt.RetVal->Kind == ExprKind::Var &&
                 Stmt.RetVal->Var.Kind == MedVar::Param &&
                 Stmt.RetVal->Var.Id == 0);
  });
  EXPECT_EQ(Func.ReturnValueEvidence, MedReturnValueEvidence::Unknown);
}

TEST(ObjCSourceTypeHints,
     LoopLoadUsesAdvancingPointerInsteadOfInitialArgument) {
  // int sum(const int *values, int count): the post-incrementing X2 pointer is
  // a loop PHI. Its first incoming value cannot replace subsequent loads.
  auto Func = recoverMethodCode({0x7100047f, 0x5400010b, 0x52800000, 0x2a0303e8,
                                 0xb8404449, 0x0b000120, 0xf1000508, 0x54ffffa1,
                                 0xd65f03c0, 0x52800000, 0xd65f03c0},
                                "sum:count:", "i28@0:8r^i16i24");
  ASSERT_TRUE(Func);
  unsigned Loads = 0;
  std::set<const HighExpr *> Seen;
  std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &Expr) {
    if (!Expr || !Seen.insert(Expr.get()).second)
      return;
    if (Expr->Kind == ExprKind::Load) {
      ++Loads;
      ASSERT_FALSE(Expr->Operands.empty());
      const auto &Address = Expr->Operands[0];
      ASSERT_TRUE(Address);
      EXPECT_FALSE(Address->Kind == ExprKind::Var &&
                   Address->Var.Kind == MedVar::Param)
          << "loop load was frozen at the original argument";
    }
    for (const auto &Operand : Expr->Operands)
      Visit(Operand);
  };
  walkStmts(Func->Body,
            [&](const HighStmt &Stmt) { forEachExpr(Stmt, Visit); });
  EXPECT_GT(Loads, 0U);
}

TEST(ObjCSourceTypeHints, LoadBeforePotentiallyAliasingStoreKeepsItsOldValue) {
  for (const char *Encoding : {"i36@0:8r^i16^i24i32", "?36@0:8r^i16^i24i32"}) {
    for (bool UseTwice : {false, true}) {
      SCOPED_TRACE(Encoding);
      SCOPED_TRACE(UseTwice);
      // ldr w0,[x2]; str w4,[x3]; ret. The two pointer arguments may alias.
      // Run with and without usable metadata so ordinary HighIR is covered too.
      // The second sequence uses the loaded W5 twice after the write.
      auto Func = UseTwice
                      ? recoverMethodCode(
                            {0xb9400045, 0xb9000064, 0x0b0500a0, 0xd65f03c0},
                            "read:write:value:", Encoding)
                      : recoverMethodCode({0xb9400040, 0xb9000064, 0xd65f03c0},
                                          "read:write:value:", Encoding);
      ASSERT_TRUE(Func);
      bool SawLoad = false;
      bool SawStore = false;
      unsigned LoadCount = 0;
      auto HasLoad = [&](const ExprPtr &Root) {
        bool Found = false;
        std::set<const HighExpr *> Seen;
        std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &Expr) {
          if (!Expr || !Seen.insert(Expr.get()).second)
            return;
          Found |= Expr->Kind == ExprKind::Load;
          for (const auto &Operand : Expr->Operands)
            Visit(Operand);
        };
        Visit(Root);
        return Found;
      };
      for (const auto &Stmt : Func->Body) {
        if (Stmt.Kind == StmtKind::Assign && HasLoad(Stmt.Val)) {
          EXPECT_FALSE(SawStore)
              << "read moved after a potentially aliasing write";
          SawLoad = true;
          ++LoadCount;
        }
        if (Stmt.Kind == StmtKind::Store) {
          EXPECT_TRUE(SawLoad)
              << "the old value was not materialized before write";
          SawStore = true;
        }
        if (Stmt.Kind == StmtKind::Return)
          EXPECT_FALSE(HasLoad(Stmt.RetVal))
              << "return re-reads modified memory";
      }
      EXPECT_TRUE(SawStore);
      EXPECT_EQ(LoadCount, 1U);
    }
  }
}

TEST(ObjCSourceTypeHints,
     RuntimeMetadataSeedsExecutableIMPAndTypedDeclaration) {
  auto Img = runtimeImage();
  parseObjCMethods(Img);
  ASSERT_EQ(Img.ObjCMethods.size(), 1U);
  const auto &Method = Img.ObjCMethods[0];
  EXPECT_EQ(Method.ClassName, "Calculator");
  EXPECT_EQ(Method.Selector, "answer");
  EXPECT_EQ(Method.Status, "supported");
  ASSERT_TRUE(Method.TypeHint);
  EXPECT_EQ(Method.TypeHint->ReturnType->Size, 4);
  EXPECT_TRUE(Img.hasFunctionSymbolAt(0x1100));
}

TEST(ObjCSourceTypeHints, RejectsNonExecutableIMPAndUnresolvedChainedPointers) {
  auto Img = runtimeImage();
  llvm::support::endian::write64le(Img.Segments[0].Data.data() + 0x518, 0x1600);
  parseObjCMethods(Img);
  ASSERT_EQ(Img.ObjCMethods.size(), 1U);
  EXPECT_EQ(Img.ObjCMethods[0].Status, "invalid_implementation");
  EXPECT_FALSE(Img.ObjCMethods[0].TypeHint);
  Img = runtimeImage();
  Img.MachOHasChainedFixups = true;
  parseObjCMethods(Img);
  EXPECT_TRUE(Img.ObjCMethods.empty());
  EXPECT_FALSE(Img.ObjCMetadataDiagnostics.empty());
}

TEST(ObjCSourceTypeHints, RejectsSectionMappingMismatchAndInvalidHiddenTypes) {
  auto Img = runtimeImage();
  ++Img.Sections[1].FileOff;
  parseObjCMethods(Img);
  EXPECT_TRUE(Img.ObjCMethods.empty());
  EXPECT_FALSE(Img.ObjCMetadataDiagnostics.empty());
  Img = runtimeImage();
  const char Encoding[] = "i16i0i8";
  std::memcpy(Img.Segments[0].Data.data() + 0x640, Encoding, sizeof(Encoding));
  parseObjCMethods(Img);
  ASSERT_EQ(Img.ObjCMethods.size(), 1U);
  EXPECT_EQ(Img.ObjCMethods[0].Status, "unsupported_encoding");
  EXPECT_FALSE(Img.ObjCMethods[0].TypeHint);
}

TEST(ObjCSourceTypeHints, OverlappingSectionCannotValidateClassListMetadata) {
  for (bool ZeroFill : {false, true}) {
    SCOPED_TRACE(ZeroFill);
    auto Img = runtimeImage();
    auto Shadow = Img.Sections[1];
    Shadow.Name = "__data";
    Img.Sections.insert(Img.Sections.begin() + 1, Shadow);
    auto &ClassList = Img.Sections[2];
    if (ZeroFill)
      ClassList.Type = llvm::MachO::S_ZEROFILL;
    else
      ++ClassList.FileOff;
    // Generic VA lookup finds the valid earlier section. It must not lend
    // that section's mapping/attributes to the named Objective-C class list.
    ASSERT_NE(Img.getSectionFor(ClassList.VA), &ClassList);
    parseObjCMethods(Img);
    EXPECT_TRUE(Img.ObjCClasses.empty());
    EXPECT_TRUE(Img.ObjCMethods.empty());
    EXPECT_FALSE(Img.ObjCMetadataDiagnostics.empty());
  }
}

TEST(ObjCSourceTypeHints, RejectsZeroFillAndExcessiveMethodCounts) {
  auto Img = runtimeImage();
  Img.Sections[1].Type = llvm::MachO::S_ZEROFILL;
  parseObjCMethods(Img);
  EXPECT_TRUE(Img.ObjCMethods.empty());
  Img = runtimeImage();
  llvm::support::endian::write32le(Img.Segments[0].Data.data() + 0x504,
                                   UINT32_MAX);
  parseObjCMethods(Img);
  EXPECT_TRUE(Img.ObjCMethods.empty());
  EXPECT_FALSE(Img.ObjCMetadataDiagnostics.empty());
}

TEST(ObjCSourceTypeHints, AcceptsOnlyIndividuallyResolvedChainedSlots) {
  auto Img = runtimeImage();
  Img.MachOHasChainedFixups = true;
  Img.MachOResolvedChainedPointerSlots = {0x1200, 0x1320, 0x1418, 0x1420,
                                          0x1508, 0x1510, 0x1518};
  parseObjCMethods(Img);
  ASSERT_EQ(Img.ObjCMethods.size(), 1U);
  EXPECT_EQ(Img.ObjCMethods[0].Status, "supported");
  Img.MachOResolvedChainedPointerSlots.erase(0x1518);
  parseObjCMethods(Img);
  ASSERT_EQ(Img.ObjCMethods.size(), 1U);
  EXPECT_EQ(Img.ObjCMethods[0].Status, "invalid_implementation");
  EXPECT_FALSE(Img.ObjCMethods[0].TypeHint);
}

TEST(ObjCSourceTypeHints, ConflictingDeclarationsAtOneIMPHaveNoTypeHint) {
  auto Img = runtimeImage();
  auto *Data = Img.Segments[0].Data.data();
  llvm::support::endian::write32le(Data + 0x504, 2);
  llvm::support::endian::write64le(Data + 0x520, 0x1660);
  llvm::support::endian::write64le(Data + 0x528, 0x1680);
  llvm::support::endian::write64le(Data + 0x530, 0x1100);
  const char Selector[] = "nothing";
  const char Encoding[] = "v16@0:8";
  std::memcpy(Data + 0x660, Selector, sizeof(Selector));
  std::memcpy(Data + 0x680, Encoding, sizeof(Encoding));
  parseObjCMethods(Img);
  ASSERT_EQ(Img.ObjCMethods.size(), 2U);
  for (const auto &Method : Img.ObjCMethods) {
    EXPECT_EQ(Method.Status, "conflicting_encoding");
    EXPECT_FALSE(Method.TypeHint);
  }
}

TEST(ObjCSourceTypeHints, DecodesRelativeMethodsAndLegacyIntegerWidth) {
  auto Img = runtimeImage();
  auto *Data = Img.Segments[0].Data.data();
  llvm::support::endian::write32le(Data + 0x500, 0xc000000cU);
  llvm::support::endian::write32le(Data + 0x508, 0x1620 - 0x1508);
  llvm::support::endian::write32le(Data + 0x50c, 0x1640 - 0x150c);
  llvm::support::endian::write32le(Data + 0x510,
                                   static_cast<uint32_t>(0x1100 - 0x1510));
  const char Encoding[] = "L16@0:8";
  std::memcpy(Data + 0x640, Encoding, sizeof(Encoding));
  parseObjCMethods(Img);
  ASSERT_EQ(Img.ObjCMethods.size(), 1U);
  const auto &Method = Img.ObjCMethods[0];
  EXPECT_EQ(Method.Implementation, 0x1100U);
  ASSERT_TRUE(Method.TypeHint);
  EXPECT_EQ(Method.TypeHint->ReturnType->Size, 4);
  EXPECT_FALSE(Method.TypeHint->ReturnType->IsSigned);
}

TEST(ObjCSourceTypeHints, RegisterBoundaryIncludesHiddenParameters) {
  for (Arch TheArch : {Arch::AArch64, Arch::X64}) {
    const size_t Registers = TheArch == Arch::AArch64 ? 8 : 6;
    for (size_t Count : {Registers, Registers + 1}) {
      auto Img = runtimeImage();
      Img.Arch = TheArch;
      std::string Selector;
      std::string Encoding = "q" + std::to_string(Count * 8) + "@0:8";
      for (size_t I = 2; I < Count; ++I) {
        Selector += "a:";
        Encoding += "q" + std::to_string(I * 8);
      }
      auto *Data = Img.Segments[0].Data.data();
      std::memcpy(Data + 0x620, Selector.c_str(), Selector.size() + 1);
      std::memcpy(Data + 0x640, Encoding.c_str(), Encoding.size() + 1);
      parseObjCMethods(Img);
      ASSERT_EQ(Img.ObjCMethods.size(), 1U);
      const auto &Method = Img.ObjCMethods[0];
      EXPECT_EQ(Method.Status,
                Count == Registers ? "supported" : "unsupported_abi");
      EXPECT_EQ(Method.TypeHint.has_value(), Count == Registers);
      if (Method.TypeHint) {
        EXPECT_EQ(Method.TypeHint->Parameters.size(), Registers);
        EXPECT_TRUE(Method.TypeHint->ReturnType->IsSigned);
      }
    }
  }
}

TEST(ObjCSourceTypeHints, PreservesDeclaredParameterOrderAndNamesInBody) {
  auto Img = runtimeImage();
  const char Selector[] = "plusOne:";
  const char Encoding[] = "q24@0:8q16";
  std::memcpy(Img.Segments[0].Data.data() + 0x620, Selector, sizeof(Selector));
  std::memcpy(Img.Segments[0].Data.data() + 0x640, Encoding, sizeof(Encoding));
  parseObjCMethods(Img);
  ASSERT_EQ(Img.ObjCMethods.size(), 1U);
  ASSERT_TRUE(Img.ObjCMethods[0].TypeHint);

  MedFunc Func;
  Func.Entry = 0x1100;
  Func.Name = "plus_one";
  Func.SourceTypeHint = Img.ObjCMethods[0].TypeHint;
  const auto &TRI = getTargetRegInfo(Arch::X64);
  MedVar Arg;
  Arg.Kind = MedVar::Reg;
  Arg.TheArch = Arch::X64;
  Arg.Id = 20;
  Arg.Size = 8;
  Arg.RegOff = TRI.IntParamRegs[2];
  Func.Params.push_back(Arg);
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Func.Entry;
  MedOp Marker;
  Marker.Opcode = NdOp::COPY;
  Marker.Output = Arg;
  Marker.addInput(Arg);
  Block.Ops.push_back(Marker);
  MedOp Add;
  Add.Opcode = NdOp::INT_ADD;
  Add.Output = Arg;
  Add.Output.Id = 21;
  Add.Output.SSAVer = 1;
  Add.Output.RegOff = TRI.IntReturnReg;
  Add.addInput(Arg);
  Add.addInput(MedVar::makeConst(1, 8));
  Block.Ops.push_back(Add);
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Block.Ops.push_back(Return);
  Func.Blocks.push_back(Block);
  inferMedTypes(Func, Arch::X64);
  // A second type pass must not erase a declaration or reorder carriers.
  inferMedTypes(Func, Arch::X64);
  ASSERT_EQ(Func.Params.size(), 3U);
  EXPECT_EQ(Func.Params[2].RegOff, TRI.IntParamRegs[2]);
  EXPECT_EQ(Func.ReturnValueEvidence, MedReturnValueEvidence::Unknown);
  const auto High = MedToHighConverter().convert(Func, Arch::X64);
  ASSERT_EQ(High.Params.size(), 3U);
  EXPECT_EQ(High.Params[2].Name, "arg0");
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
  OS.flush();
  EXPECT_NE(Source.find("arg0 + 1"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("arg2"), std::string::npos) << Source;
}

TEST(ObjCSourceTypeHints,
     ExplicitVoidRemainsVoidAndUnsupportedCarriersStayInferred) {
  HighFunc High;
  High.Name = "nothing";
  High.ReturnType = NdType::makeInt(8);
  SourceFunctionTypeHint Hint;
  Hint.ReturnType = NdType::makeVoid();
  High.SourceTypeHint = Hint;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeConst(42, 8);
  High.Body.push_back(Return);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit({High}, OS, CEmitterOptions{}));
  OS.flush();
  EXPECT_NE(Source.find("void nothing(void)"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("return 42"), std::string::npos) << Source;

  MedFunc Func;
  Hint.ReturnType = NdType::makeInt(8);
  Hint.Parameters = {{"objc_self", NdType::makePtr()},
                     {"objc_cmd", NdType::makePtr()}};
  Func.SourceTypeHint = Hint;
  MedVar Extra;
  Extra.Kind = MedVar::Param;
  Extra.Id = 5;
  Extra.Size = 8;
  Extra.RegOff = getTargetRegInfo(Arch::AArch64).FPParamRegs[0];
  Func.Params.push_back(Extra);
  inferMedTypes(Func, Arch::AArch64);
  EXPECT_FALSE(Func.SourceTypeHint);
  EXPECT_EQ(Func.Params.size(), 1U);
  EXPECT_EQ(Func.ReturnValueEvidence, MedReturnValueEvidence::Unknown);
}
} // namespace
