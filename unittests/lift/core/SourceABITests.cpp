#include "../../../lib/pipeline/PipelineReturnModelingDetail.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedSourceParameterUses.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>

namespace {
using namespace neverd;

TEST(SourceABI, EmptyBoundCallDoesNotAcquireUnrelatedRegisterArguments) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    for (bool Malformed : {false, true}) {
      SCOPED_TRACE(static_cast<unsigned>(Architecture));
      SCOPED_TRACE(Malformed);
      MedFunc Function;
      Function.Name = "zero_argument_call";
      MedBlock Block;
      Block.Id = 0;
      MedOp Write;
      Write.Opcode = NdOp::COPY;
      Write.Output.Kind = MedVar::Reg;
      Write.Output.Id = 42;
      Write.Output.RegOff = TRI.IntParamRegs.front();
      Write.Output.Size = 8;
      Write.Output.TheArch = Architecture;
      Write.addInput(MedVar::makeConst(73, 8));
      auto Hint = std::make_shared<SourceCallTypeHint>();
      Hint->Signature.ReturnType = NdType::makeVoid();
      std::string Error;
      ASSERT_TRUE(
          assignDarwinScalarSourceABI(Hint->Signature, Architecture, Error))
          << Error;
      MedOp Call;
      Call.Opcode = NdOp::CALL;
      Call.addInput(MedVar::makeConst(0x2000, 8));
      if (Malformed)
        Call.addInput(Write.Output);
      Call.SourceCallHint = Hint;
      MedOp Return;
      Return.Opcode = NdOp::RETURN;
      Block.Ops = {Write, Call, Return};
      Function.Blocks = {Block};
      const auto High = MedToHighConverter().convert(Function, Architecture);
      unsigned Calls = 0;
      walkStmts(High.Body, [&](const HighStmt &Statement) {
        forEachExpr(Statement, [&](const ExprPtr &Expression) {
          if (Expression && Expression->Kind == ExprKind::Call &&
              Expression->CallAddr == 0x2000) {
            ++Calls;
            EXPECT_TRUE(Expression->Operands.empty());
          }
        });
      });
      EXPECT_EQ(Calls, 1U);
    }
  }
}

TEST(SourceABI, BoundRecordCallAbiRetainsEveryRenamedComponent) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Floating : {false, true}) {
      const auto Member = Floating ? NdType::makeFloat(8) : NdType::makeInt(8);
      auto Hint = std::make_shared<SourceCallTypeHint>();
      Hint->TargetAddress = 0x2000;
      Hint->TargetName = "bound_record";
      Hint->Signature.ReturnType = NdType::makeVoid();
      Hint->Signature.Parameters = {
          {"pair", NdType::makeStruct({Member, Member})}};
      std::string Error;
      if (Floating && Architecture == Arch::X64) {
        // Darwin x64 floating records are outside the supported fixed layout.
        EXPECT_FALSE(assignDarwinFixedSourceABI(Hint->Signature, Architecture, Error));
        continue;
      }
      ASSERT_TRUE(
          assignDarwinFixedSourceABI(Hint->Signature, Architecture, Error)) << Error;
      MedOp Call;
      Call.Opcode = NdOp::CALL;
      Call.SourceCallHint = Hint;
      Call.addInput(MedVar::makeConst(0x2000, 8));
      for (unsigned I = 0; I < 2; ++I) {
        MedVar V;
        V.Kind = MedVar::Temp;
        V.Id = 73 + I;
        V.SSAVer = 9;
        V.Size = 8;
        V.TheArch = Architecture;
        Call.addInput(V);
      }
      MedFunc Function;
      Function.Blocks.resize(1);
      Function.Blocks[0].Id = 0;
      Function.Blocks[0].Ops = {Call};
      recoverCallAbi(Function, Architecture, {});
      ASSERT_EQ(Function.CallInfos.size(), 1U);
      const auto &Info = Function.CallInfos[0];
      EXPECT_EQ(Info.SourceCallHint, Hint);
      ASSERT_EQ(Info.Args.size(), 2U);
      for (unsigned I = 0; I < 2; ++I) {
        EXPECT_EQ(Info.Args[I].Id, 73 + I);
        EXPECT_EQ(Info.Args[I].SSAVer, 9);
      }
    }
}

TEST(SourceABI, AddressUsesDoNotRewriteDeclaredIntegerParameters) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    SourceFunctionTypeHint Hint;
    Hint.ReturnType = NdType::makeInt(8);
    Hint.Parameters = {{"address_bits", NdType::makeInt(8)}};
    std::string Error;
    ASSERT_TRUE(assignDarwinScalarSourceABI(Hint, Architecture, Error));
    MedFunc Function;
    Function.Name = "read_address_bits";
    Function.SourceTypeHint = Hint;
    Function.ReturnType = Hint.ReturnType;
    MedVar Address;
    Address.Kind = MedVar::Param;
    Address.Id = 0;
    Address.Size = 8;
    Address.TheArch = Architecture;
    Address.RegOff = Hint.Parameters[0].Location.RegisterOffset;
    Function.Params = {Address};
    Function.TypedParams = {{"address_bits", Hint.Parameters[0].Type}};
    MedOp Load;
    Load.Opcode = NdOp::LOAD;
    Load.Output.Kind = MedVar::Temp;
    Load.Output.Id = 5;
    Load.Output.Size = 8;
    Load.addInput(Address);
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.addInput(Load.Output);
    MedBlock Block;
    Block.Id = 0;
    Block.Ops = {Load, Return};
    Function.Blocks = {Block};
    const auto High = MedToHighConverter().convert(Function, Architecture);
    ASSERT_EQ(High.Params.size(), 1U);
    EXPECT_TRUE(equalSourceTypes(High.Params[0].Type, Hint.Parameters[0].Type));
  }
}

TEST(SourceABI, TerminalIntrinsicsDoNotDefineSyntheticCallResults) {
  for (const auto [Architecture, IntrinsicId] :
       {std::pair{Arch::AArch64, Intrinsic::Brk},
        std::pair{Arch::X64, Intrinsic::Ud2}}) {
    MedFunc Function;
    Function.Name = "terminal_intrinsic";
    MedBlock Block;
    Block.Id = 0;
    MedOp Trap;
    Trap.Opcode = NdOp::INTRINSIC;
    Trap.Output.Kind = MedVar::Temp;
    Trap.Output.Id = 0;
    Trap.Output.Size = 8;
    Trap.addInput(MedVar::makeConst(static_cast<uint64_t>(IntrinsicId), 4));
    Block.Ops = {Trap};
    Function.Blocks = {Block};
    const auto High = MedToHighConverter().convert(Function, Architecture);
    unsigned Traps = 0;
    walkStmts(High.Body, [&](const HighStmt &Statement) {
      EXPECT_NE(Statement.Kind, StmtKind::Assign);
      if (Statement.Kind == StmtKind::Call && Statement.CallExpr &&
          Statement.CallExpr->IntrinsicId == IntrinsicId) {
        ++Traps;
        EXPECT_EQ(Statement.CallExpr->Type->Kind, NdTypeKind::Void);
      }
    });
    EXPECT_EQ(Traps, 1U);
  }
}

TEST(SourceABI, DarwinIntegerPairResultRequiresBothReturnRegisters) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    SourceFunctionTypeHint Hint;
    Hint.ReturnType = NdType::makeInt(16, false);
    std::string Diagnostic;
    ASSERT_TRUE(assignDarwinScalarSourceABI(Hint, Architecture, Diagnostic))
        << Diagnostic;
    EXPECT_TRUE(validateSourceABI(Hint, Diagnostic)) << Diagnostic;
    const auto &TRI = getTargetRegInfo(Architecture);
    ASSERT_EQ(Hint.ReturnComponents.size(), 2U);
    EXPECT_EQ(Hint.ReturnLocation.Kind, SourceABICarrierKind::None);
    for (unsigned I = 0; I != 2; ++I) {
      EXPECT_EQ(Hint.ReturnComponents[I].RegisterOffset, TRI.IntReturnRegs[I]);
      EXPECT_EQ(Hint.ReturnComponents[I].ValueBytes, 8U);
    }
    for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
      auto Bad = Hint;
      switch (Mutation) {
      case 0:
        Bad.ReturnComponents.pop_back();
        break;
      case 1:
        Bad.ReturnComponents[1] = Bad.ReturnComponents[0];
        break;
      case 2:
        std::swap(Bad.ReturnComponents[0], Bad.ReturnComponents[1]);
        break;
      case 3:
        Bad.ReturnComponents[1].ValueBytes = 4;
        break;
      case 4:
        Bad.ReturnComponents[1].ExtendTo32Bits = true;
        break;
      case 5:
        Bad.ReturnComponents[1].EntryStackOffset = 8;
        break;
      case 6:
        Bad.ReturnComponents[1].Kind = SourceABICarrierKind::FloatingRegister;
        break;
      case 7:
        Bad.ReturnLocation = Bad.ReturnComponents[0];
        break;
      case 8:
        Bad.ReturnType = NdType::makeVoid();
        break;
      case 9:
        Bad.ReturnType = NdType::makeInt(8);
        break;
      }
      EXPECT_FALSE(validateSourceABI(Bad, Diagnostic)) << Mutation;
    }
    Hint.ReturnType = NdType::makeInt(8);
    ASSERT_TRUE(assignDarwinScalarSourceABI(Hint, Architecture, Diagnostic));
    EXPECT_TRUE(Hint.ReturnComponents.empty());
    Hint.Parameters = {{"wide", NdType::makeInt(16)}};
    EXPECT_FALSE(assignDarwinScalarSourceABI(Hint, Architecture, Diagnostic));
  }
}

TEST(SourceABI, SwiftWordCallsKeepConventionAndRejectUnmodelledResults) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    for (const auto &Result :
         {Pointer, NdType::makeInt(16, false),
          NdType::makeStruct({Pointer, NdType::makeInt(8)})}) {
      for (size_t Count = 0; Count <= TRI.IntParamRegs.size() + 2; ++Count) {
        SourceFunctionTypeHint Hint;
        Hint.ReturnType = Result;
        Hint.Parameters.assign(Count, {"word", NdType::makeInt(8, false)});
        std::string Error;
        ASSERT_TRUE(assignDarwinSwiftSourceABI(Hint, Architecture, Error))
            << Error;
        EXPECT_EQ(Hint.Convention,
                  SourceFunctionTypeHint::ConventionKind::Swift);
        EXPECT_TRUE(validateSourceABI(Hint, Error));
        for (size_t I = 0; I < Count; ++I) {
          const auto &Location = Hint.Parameters[I].Location;
          EXPECT_EQ(Location.Kind, I < TRI.IntParamRegs.size()
                                       ? SourceABICarrierKind::IntegerRegister
                                       : SourceABICarrierKind::Stack);
          if (I < TRI.IntParamRegs.size())
            EXPECT_EQ(Location.RegisterOffset, TRI.IntParamRegs[I]);
        }
        if (Result->Size == 16) {
          ASSERT_EQ(Hint.ReturnComponents.size(), 2U);
          for (size_t I = 0; I < 2; ++I)
            EXPECT_EQ(Hint.ReturnComponents[I].RegisterOffset,
                      TRI.IntReturnRegs[I]);
        }
        auto Invalid = Hint;
        Invalid.Convention =
            static_cast<SourceFunctionTypeHint::ConventionKind>(255);
        EXPECT_FALSE(validateSourceABI(Invalid, Error));
        if (Count) {
          Invalid = Hint;
          Invalid.Parameters[0].Location = {SourceABICarrierKind::Stack, 0, 8,
                                            8};
          EXPECT_FALSE(validateSourceABI(Invalid, Error));
          Invalid = Hint;
          Invalid.Parameters[0].Location.RegisterOffset =
              TRI.IntParamRegs.back();
          EXPECT_FALSE(validateSourceABI(Invalid, Error));
        }
        ASSERT_TRUE(assignDarwinFixedSourceABI(Hint, Architecture, Error));
        EXPECT_EQ(Hint.Convention, SourceFunctionTypeHint::ConventionKind::C);
      }
    }
    for (const auto &Unsupported :
         {NdType::makeInt(2), NdType::makeFloat(8),
          NdType::makeStruct({Pointer, Pointer, Pointer})}) {
      SourceFunctionTypeHint Hint;
      Hint.ReturnType = Unsupported;
      std::string Error;
      EXPECT_FALSE(assignDarwinSwiftSourceABI(Hint, Architecture, Error));
    }
    for (const auto &Unsupported :
         {NdType::makeFloat(8),
          NdType::makeStruct({Pointer, Pointer, Pointer})}) {
      SourceFunctionTypeHint Hint;
      std::string Error;
      Hint.ReturnType = NdType::makeVoid();
      Hint.Parameters = {{"unsupported", Unsupported}};
      EXPECT_FALSE(assignDarwinSwiftSourceABI(Hint, Architecture, Error));
    }
  }
}

TEST(SourceABI, SwiftScalarArgumentsKeepNarrowAndStackCarriers) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    const auto Word = NdType::makeInt(8, false);
    const auto Byte = NdType::makeInt(1, false);
    SourceFunctionTypeHint Hint;
    Hint.ReturnType = NdType::makeVoid();
    Hint.Parameters = {{"a0", Word},
                       {"a1", Word},
                       {"a2", Byte},
                       {"a3", Word},
                       {"a4", NdType::makePtr(NdType::makeVoid())},
                       {"a5", Word},
                       {"a6", Word},
                       {"a7", Byte},
                       {"a8", Word},
                       {"a9", NdType::makeInt(4, false)}};
    std::string Error;
    ASSERT_TRUE(assignDarwinSwiftSourceABI(Hint, Architecture, Error)) << Error;
    ASSERT_TRUE(validateSourceABI(Hint, Error)) << Error;
    int64_t StackOffset = Architecture == Arch::X64 ? 8 : 0;
    for (size_t I = 0; I < Hint.Parameters.size(); ++I) {
      const auto &Parameter = Hint.Parameters[I];
      const auto &Location = Parameter.Location;
      EXPECT_EQ(Location.ValueBytes, Parameter.Type->Size) << I;
      if (I < TRI.IntParamRegs.size()) {
        EXPECT_EQ(Location.Kind, SourceABICarrierKind::IntegerRegister) << I;
        EXPECT_EQ(Location.RegisterOffset, TRI.IntParamRegs[I]) << I;
        EXPECT_EQ(Location.ExtendTo32Bits,
                  Parameter.Type->Kind == NdTypeKind::Int &&
                      Parameter.Type->Size < 4)
            << I;
      } else {
        const int64_t Alignment =
            Architecture == Arch::AArch64 ? Parameter.Type->Size : 8;
        StackOffset = (StackOffset + Alignment - 1) & -Alignment;
        EXPECT_EQ(Location.Kind, SourceABICarrierKind::Stack) << I;
        EXPECT_EQ(Location.EntryStackOffset, StackOffset) << I;
        EXPECT_FALSE(Location.ExtendTo32Bits) << I;
        StackOffset += Architecture == Arch::AArch64 ? Parameter.Type->Size : 8;
      }
    }
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      auto Invalid = Hint;
      if (Mutation == 0)
        ++Invalid.Parameters[2].Location.ValueBytes;
      else if (Mutation == 1)
        Invalid.Parameters[2].Location.ExtendTo32Bits = false;
      else if (Mutation == 2)
        ++Invalid.Parameters.back().Location.EntryStackOffset;
      else if (Mutation == 3)
        Invalid.Parameters.back().Location.Kind =
            SourceABICarrierKind::IntegerRegister;
      else
        Invalid.Parameters[0].Type = NdType::makeFloat(8);
      EXPECT_FALSE(validateSourceABI(Invalid, Error)) << Mutation;
    }
  }
}

TEST(SourceABI, SwiftSpecialParametersUseDedicatedRegistersWithoutBankSlots) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    SourceFunctionTypeHint Hint;
    Hint.ReturnType = NdType::makeVoid();
    Hint.Parameters = {
        {"result", Pointer}, {"object", Pointer}, {"self", Pointer}};
    Hint.Parameters[0].TheRole =
        SourceParameterTypeHint::Role::SwiftIndirectResult;
    Hint.Parameters[2].TheRole = SourceParameterTypeHint::Role::SwiftContext;
    std::string Error;
    ASSERT_TRUE(assignDarwinSwiftSourceABI(Hint, Architecture, Error)) << Error;
    ASSERT_TRUE(validateSourceABI(Hint, Error)) << Error;
    const auto &TRI = getTargetRegInfo(Architecture);
    EXPECT_EQ(Hint.Parameters[0].Location.RegisterOffset,
              Architecture == Arch::AArch64 ? TRI.indirectResultReg()
                                            : TRI.IntReturnReg);
    EXPECT_EQ(Hint.Parameters[1].Location.RegisterOffset,
              TRI.IntParamRegs.front());
    EXPECT_EQ(Hint.Parameters[2].Location.RegisterOffset,
              Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13);
    for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
      auto Bad = Hint;
      if (Mutation == 0)
        Bad.Convention = SourceFunctionTypeHint::ConventionKind::C;
      else if (Mutation == 1)
        Bad.Parameters[0].Type = NdType::makeInt(8, false);
      else if (Mutation == 2)
        std::swap(Bad.Parameters[0], Bad.Parameters[1]);
      else if (Mutation == 3)
        Bad.ReturnType = Pointer;
      else if (Mutation == 4)
        Bad.Parameters[1].TheRole =
            SourceParameterTypeHint::Role::SwiftIndirectResult;
      else if (Mutation == 5)
        Bad.Parameters[1].TheRole = SourceParameterTypeHint::Role::SwiftContext;
      else if (Mutation == 6)
        Bad.Parameters[0].Location.RegisterOffset = TRI.IntParamRegs.front();
      else if (Mutation == 7)
        Bad.Parameters[2].Location.RegisterOffset = TRI.IntParamRegs.front();
      else
        Bad.Parameters[2].TheRole =
            static_cast<SourceParameterTypeHint::Role>(255);
      EXPECT_FALSE(validateSourceABI(Bad, Error)) << Mutation;
    }
    EXPECT_FALSE(assignDarwinFixedSourceABI(Hint, Architecture, Error));
  }
}

TEST(SourceABI, SourceReturnComponentsNeverBecomeRewriteABIEvidence) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    BinaryImage Image;
    Image.Arch = Architecture;
    const auto &TRI = getTargetRegInfo(Architecture);
    PipelineResult Result;
    Result.MedFuncs.resize(2);
    auto &Caller = Result.MedFuncs[0];
    auto &Callee = Result.MedFuncs[1];
    Caller.Entry = 0x1000;
    Callee.Entry = 0x2000;
    Caller.Blocks.resize(1);
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Output.Kind = MedVar::Temp;
    Call.Output.Id = 10;
    Call.Output.Size = 16;
    Call.addInput(MedVar::makeConst(Callee.Entry, 8));
    auto Hint = std::make_shared<SourceCallTypeHint>();
    Hint->Signature.ReturnType = NdType::makeInt(16, false);
    std::string Diagnostic;
    ASSERT_TRUE(
        assignDarwinScalarSourceABI(Hint->Signature, Architecture, Diagnostic));
    Call.SourceCallHint = Hint;
    Caller.Blocks[0].Ops.push_back(Call);
    for (unsigned I = 0; I != 2; ++I) {
      MedOp Extract;
      Extract.Opcode = NdOp::SUBBYTES;
      Extract.Output.Kind = MedVar::Reg;
      Extract.Output.Id = 11 + I;
      Extract.Output.Size = 8;
      Extract.Output.RegOff = TRI.IntReturnRegs[I];
      Extract.addInput(Call.Output);
      Extract.addInput(MedVar::makeConst(I * 8, 4));
      Caller.Blocks[0].Ops.push_back(Extract);
    }
    recoverStructReturnFromCallers(Image, Result);
    EXPECT_TRUE(Callee.MultiReturn.empty());
    // Preserve the ordinary aggregate-remodeling path when no source hint
    // supplies the extracts; source annotation must be the deciding boundary.
    Caller.Blocks[0].Ops[0].SourceCallHint.reset();
    recoverStructReturnFromCallers(Image, Result);
    ASSERT_EQ(Callee.MultiReturn.size(), 2U);
    EXPECT_EQ(Callee.MultiReturn[1].RegOff, TRI.IntReturnRegs[1]);
  }
}

TEST(SourceABI, CallbackTypesKeepTheirSignaturesAndRejectMalformedGraphs) {
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  const auto Callback =
      NdType::makePtr(NdType::makeFunc(NdType::makeVoid(), {Pointer}));
  const auto Other = NdType::makePtr(NdType::makeFunc(
      NdType::makeVoid(), {NdType::makePtr(NdType::makeVoid())}));
  EXPECT_TRUE(equalSourceTypes(Callback, Other));
  EXPECT_FALSE(equalSourceTypes(Callback, Pointer));
  EXPECT_FALSE(equalSourceTypes(Callback, NdType::makePtr(NdType::makeFunc(
                                              NdType::makeInt(4), {Pointer}))));
  EXPECT_FALSE(equalSourceTypes(
      Callback, NdType::makePtr(NdType::makeFunc(NdType::makeVoid()))));
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    SourceFunctionTypeHint Hint;
    Hint.ReturnType = Callback;
    Hint.Parameters = {{"callback", Callback}, {"context", Pointer}};
    std::string Diagnostic;
    ASSERT_TRUE(assignDarwinScalarSourceABI(Hint, Architecture, Diagnostic))
        << Diagnostic;
    EXPECT_EQ(Hint.Parameters[0].Location.RegisterOffset,
              getTargetRegInfo(Architecture).IntParamRegs[0]);
    EXPECT_EQ(Hint.Parameters[0].Location.ValueBytes, 8U);
    for (const auto &Invalid :
         {NdType::makeFunc(nullptr),
          NdType::makeFunc(NdType::makeVoid(), {NdType::makeVoid()}),
          NdType::makeFunc(NdType::makeVoid(), {Callback->Pointee}),
          NdType::makeFunc(Callback->Pointee),
          NdType::makeFunc(NdType::makeVoid(),
                           std::vector<TypeRef>(65, Pointer))}) {
      Hint.Parameters[0].Type = NdType::makePtr(Invalid);
      EXPECT_FALSE(validateSourceABI(Hint, Diagnostic));
      EXPECT_FALSE(
          equalSourceTypes(Hint.Parameters[0].Type, Hint.Parameters[0].Type));
    }
    auto Cycle = NdType::makePtr();
    Cycle->Pointee = Cycle;
    Hint.Parameters[0].Type = Cycle;
    EXPECT_FALSE(validateSourceABI(Hint, Diagnostic));
    EXPECT_FALSE(equalSourceTypes(Cycle, Cycle));
    Cycle->Pointee.reset();
    auto Recursive = NdType::makeFunc(NdType::makeVoid());
    auto RecursivePointer = NdType::makePtr(Recursive);
    Recursive->ParamTypes.push_back(RecursivePointer);
    Hint.Parameters[0].Type = RecursivePointer;
    EXPECT_FALSE(validateSourceABI(Hint, Diagnostic));
    EXPECT_FALSE(equalSourceTypes(RecursivePointer, RecursivePointer));
    Recursive->ParamTypes.clear();
    auto TooDeep = Pointer;
    for (unsigned I = 0; I < 17; ++I)
      TooDeep = NdType::makePtr(TooDeep);
    EXPECT_FALSE(equalSourceTypes(TooDeep, TooDeep));
  }
}

SourceFunctionTypeHint declaration(TypeRef Result) {
  SourceFunctionTypeHint Hint;
  Hint.ReturnType = std::move(Result);
  Hint.Parameters = {{"objc_self", NdType::makePtr()},
                     {"objc_cmd", NdType::makePtr()}};
  return Hint;
}

TEST(SourceABI, DarwinMixedArgumentsUseIndependentBanks) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Hint = declaration(NdType::makeFloat(8));
    Hint.Parameters.insert(Hint.Parameters.end(),
                           {{"arg0", NdType::makeInt(4)},
                            {"arg1", NdType::makeFloat(8)},
                            {"arg2", NdType::makeInt(8)},
                            {"arg3", NdType::makeFloat(4)}});
    std::string Error;
    ASSERT_TRUE(assignDarwinObjCSourceABI(Hint, Architecture, Error)) << Error;
    const auto &TRI = getTargetRegInfo(Architecture);
    EXPECT_EQ(Hint.Parameters[2].Location.RegisterOffset, TRI.IntParamRegs[2]);
    EXPECT_EQ(Hint.Parameters[3].Location.RegisterOffset, TRI.FPParamRegs[0]);
    EXPECT_EQ(Hint.Parameters[4].Location.RegisterOffset, TRI.IntParamRegs[3]);
    EXPECT_EQ(Hint.Parameters[5].Location.RegisterOffset, TRI.FPParamRegs[1]);
    EXPECT_EQ(Hint.Parameters[5].Location.ValueBytes, 4);
    EXPECT_EQ(Hint.ReturnLocation.Kind, SourceABICarrierKind::FloatingRegister);
    EXPECT_EQ(Hint.ReturnLocation.RegisterOffset, TRI.FPReturnReg);
  }
}

TEST(SourceABI, DarwinStackLayoutPreservesNarrowArgumentOffsets) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Hint = declaration(NdType::makeInt(4));
    const auto Registers = getTargetRegInfo(Architecture).IntParamRegs.size();
    for (size_t I = 2; I < Registers; ++I)
      Hint.Parameters.push_back(
          {"arg" + std::to_string(I - 2), NdType::makeInt(4)});
    Hint.Parameters.insert(Hint.Parameters.end(),
                           {{"byte", NdType::makeInt(1)},
                            {"short_value", NdType::makeInt(2)},
                            {"integer", NdType::makeInt(4)}});
    std::string Error;
    ASSERT_TRUE(assignDarwinObjCSourceABI(Hint, Architecture, Error)) << Error;
    for (size_t I = 0; I != 3; ++I) {
      const auto &L = Hint.Parameters[Registers + I].Location;
      EXPECT_EQ(L.Kind, SourceABICarrierKind::Stack);
      EXPECT_EQ(L.EntryStackOffset, Architecture == Arch::AArch64
                                        ? int64_t(I * 2)
                                        : int64_t(8 + I * 8));
    }
  }
}

TEST(SourceABI, FloatingBankOverflowDoesNotConsumeIntegerRegisters) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Hint = declaration(NdType::makeFloat(8));
    for (unsigned I = 0; I != 9; ++I)
      Hint.Parameters.push_back(
          {"arg" + std::to_string(I), NdType::makeFloat(8)});
    Hint.Parameters.push_back({"integer", NdType::makeInt(8)});
    std::string Error;
    ASSERT_TRUE(assignDarwinObjCSourceABI(Hint, Architecture, Error)) << Error;
    EXPECT_EQ(Hint.Parameters[10].Location.Kind, SourceABICarrierKind::Stack);
    EXPECT_EQ(Hint.Parameters[10].Location.EntryStackOffset,
              Architecture == Arch::X64 ? 8 : 0);
    EXPECT_EQ(Hint.Parameters[11].Location.RegisterOffset,
              getTargetRegInfo(Architecture).IntParamRegs[2]);
  }
}

TEST(SourceABI, RejectsConflictingCarriersAndUnmodelledTypes) {
  auto Hint = declaration(NdType::makeInt(8));
  std::string Error;
  ASSERT_TRUE(assignDarwinObjCSourceABI(Hint, Arch::AArch64, Error));
  auto Bad = Hint;
  Bad.Parameters[1].Location = Bad.Parameters[0].Location;
  EXPECT_FALSE(validateSourceABI(Bad, Error));
  EXPECT_NE(Error.find("share"), std::string::npos);
  Bad = Hint;
  Bad.ReturnLocation.Kind = SourceABICarrierKind::FloatingRegister;
  EXPECT_FALSE(validateSourceABI(Bad, Error));
  Bad = Hint;
  Bad.Parameters[0].Location.ValueBytes = 4;
  EXPECT_FALSE(validateSourceABI(Bad, Error));
  Bad = Hint;
  Bad.Parameters[0].Type = NdType::makeFloat(16);
  EXPECT_FALSE(assignDarwinObjCSourceABI(Bad, Arch::AArch64, Error));
  Bad = Hint;
  Bad.Parameters[0].Location = {SourceABICarrierKind::Stack, 0, 0, 8};
  Bad.Parameters[1].Location = {SourceABICarrierKind::Stack, 0, 0, 8};
  EXPECT_FALSE(validateSourceABI(Bad, Error));
  EXPECT_NE(Error.find("Overlapping"), std::string::npos);
}

TEST(SourceABI, NarrowReturnExtensionRequiresAnExplicitDarwinArm64Carrier) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const uint16_t Bytes : {1, 2, 4, 8}) {
      auto Hint = declaration(NdType::makeInt(Bytes));
      std::string Error;
      ASSERT_TRUE(assignDarwinObjCSourceABI(Hint, Architecture, Error));
      EXPECT_EQ(Hint.ReturnLocation.ExtendTo32Bits,
                Architecture == Arch::AArch64 && Bytes < 4);
      Hint.ReturnLocation.ExtendTo32Bits = true;
      EXPECT_EQ(validateSourceABI(Hint, Error),
                Architecture == Arch::AArch64 && Bytes < 4);
    }
  }
  auto Hint = declaration(NdType::makeInt(1));
  std::string Error;
  ASSERT_TRUE(assignDarwinObjCSourceABI(Hint, Arch::AArch64, Error));
  Hint.Parameters[0].Location.ExtendTo32Bits = true;
  EXPECT_FALSE(validateSourceABI(Hint, Error));
  Hint.Parameters[0].Location.ExtendTo32Bits = false;
  Hint.ReturnType = NdType::makeFloat(4);
  Hint.ReturnLocation = {SourceABICarrierKind::FloatingRegister,
                         getTargetRegInfo(Arch::AArch64).FPReturnReg, 0, 4,
                         true};
  EXPECT_FALSE(validateSourceABI(Hint, Error));
  Hint.ReturnType = NdType::makeVoid();
  Hint.ReturnLocation = {};
  Hint.ReturnLocation.ExtendTo32Bits = true;
  EXPECT_FALSE(validateSourceABI(Hint, Error));
}

TEST(SourceABI, NarrowParameterExtensionRequiresAnIntegerRegisterCarrier) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (uint16_t Bytes : {1, 2, 4, 8}) {
      for (bool Signed : {false, true}) {
        SourceFunctionTypeHint Hint;
        Hint.ReturnType = NdType::makeVoid();
        Hint.Parameters.assign(10, {"value", NdType::makeInt(Bytes, Signed)});
        std::string Error;
        ASSERT_TRUE(assignDarwinScalarSourceABI(Hint, Architecture, Error));
        for (auto &Parameter : Hint.Parameters) {
          const bool Extended =
              Bytes < 4 &&
              Parameter.Location.Kind == SourceABICarrierKind::IntegerRegister;
          EXPECT_EQ(Parameter.Location.ExtendTo32Bits, Extended);
          auto Bad = Hint;
          const auto Index = &Parameter - Hint.Parameters.data();
          Bad.Parameters[Index].Location.ExtendTo32Bits = true;
          EXPECT_EQ(validateSourceABI(Bad, Error), Extended);
        }
      }
    }
  }
}

TEST(SourceABI, ExplicitSwiftReceiverCanUseDedicatedCalleeSavedRegister) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    SourceFunctionTypeHint Hint;
    Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
    Hint.Architecture = Architecture;
    Hint.HasExplicitABI = true;
    Hint.ReturnType = NdType::makeInt(8);
    const auto &TRI = getTargetRegInfo(Architecture);
    Hint.ReturnLocation = {SourceABICarrierKind::IntegerRegister,
                           TRI.IntReturnReg, 0, 8};
    // Swift class receivers use these dedicated registers; the validator must
    // not impose Objective-C's two hidden leading arguments.
    const auto Dedicated =
        Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13;
    Hint.Parameters = {
        {"self",
         NdType::makePtr(),
         {SourceABICarrierKind::IntegerRegister, Dedicated, 0, 8}}};
    std::string Error;
    EXPECT_TRUE(validateSourceABI(Hint, Error)) << Error;
  }
}

TEST(SourceABI, PackedGenericSlotReadsBindToDistinctSourceParameters) {
  auto Hint = declaration(NdType::makeInt(4));
  for (unsigned I = 0; I != 6; ++I)
    Hint.Parameters.push_back({"arg" + std::to_string(I), NdType::makeInt(4)});
  Hint.Parameters.insert(Hint.Parameters.end(), {{"arg6", NdType::makeInt(1)},
                                                 {"arg7", NdType::makeInt(2)},
                                                 {"arg8", NdType::makeInt(4)}});
  MedFunc Func;
  Func.Name = "packed";
  Func.SourceTypeHint = Hint;
  MedBlock Block;
  Block.Id = 0;
  for (unsigned I = 0; I != 3; ++I) {
    MedOp Read;
    Read.Opcode = I ? NdOp::SUBBYTES : NdOp::COPY;
    Read.Output.Kind = MedVar::Temp;
    Read.Output.Id = 20 + I;
    Read.Output.Size = uint16_t(1U << I);
    MedVar Slot;
    Slot.Kind = MedVar::Param;
    Slot.Id = 8;
    Slot.RegOff = kNoParamReg;
    Slot.Size = I ? 8 : 1;
    Read.addInput(Slot);
    if (I)
      Read.addInput(MedVar::makeConst(I * 2, 4));
    Block.Ops.push_back(Read);
  }
  Func.Blocks.push_back(Block);
  inferMedTypes(Func, Arch::AArch64);
  ASSERT_TRUE(Func.SourceTypeHint);
  ASSERT_TRUE(Func.SourceParametersBound);
  ASSERT_EQ(Func.Params.size(), 11U);
  for (unsigned I = 0; I != 3; ++I) {
    const auto &Read = Func.Blocks[0].Ops[I];
    EXPECT_EQ(Read.Inputs[0].Id, 8 + int(I));
    EXPECT_EQ(Read.Inputs[0].Size, 1U << I);
    if (I)
      EXPECT_EQ(Read.Inputs[1].ConstVal, 0U);
  }
  inferMedTypes(Func, Arch::AArch64);
  ASSERT_TRUE(Func.SourceTypeHint);
  EXPECT_EQ(Func.Blocks[0].Ops[2].Inputs[0].Id, 10);
  // An eight-byte read of this packed slot includes independent arguments and
  // padding; reject the projection instead of assigning it to the first one.
  Func.SourceParametersBound = false;
  Func.Blocks[0].Ops.resize(1);
  Func.Blocks[0].Ops[0].Inputs[0].Size = 8;
  Func.Blocks[0].Ops[0].Output.Size = 8;
  inferMedTypes(Func, Arch::AArch64);
  EXPECT_FALSE(Func.SourceTypeHint);
}

TEST(SourceABI, SourceStackExpressionsPreserveExactEntryOffsets) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    auto Hint = declaration(NdType::makeInt(4));
    for (size_t I = 2; I < TRI.IntParamRegs.size(); ++I)
      Hint.Parameters.push_back(
          {"unused" + std::to_string(I), NdType::makeInt(8)});
    Hint.Parameters.insert(Hint.Parameters.end(),
                           {{"byte", NdType::makeInt(1)},
                            {"short_value", NdType::makeInt(2)},
                            {"integer", NdType::makeInt(4)}});
    std::string Error;
    ASSERT_TRUE(assignDarwinObjCSourceABI(Hint, Architecture, Error)) << Error;
    for (size_t I = 0; I != 3; ++I) {
      const auto Index = TRI.IntParamRegs.size() + I;
      const auto &Location = Hint.Parameters[Index].Location;
      const int64_t BaseOffset = Architecture == Arch::X64 ? 8 : 0;
      const auto SlotOffset = Location.EntryStackOffset - BaseOffset;
      MedFunc Func;
      Func.Name = "stack_parameter";
      Func.SourceTypeHint = Hint;
      Func.SourceTypeHint->ReturnType = Hint.Parameters[Index].Type;
      Func.SourceTypeHint->ReturnLocation.ValueBytes = Location.ValueBytes;
      MedVar Slot;
      Slot.Kind = MedVar::Param;
      Slot.TheArch = Architecture;
      Slot.Id = static_cast<int>(TRI.IntParamRegs.size() + SlotOffset / 8);
      Slot.RegOff = kNoParamReg;
      Slot.Size = SlotOffset % 8 ? 8 : Location.ValueBytes;
      MedOp Read;
      Read.Opcode = SlotOffset % 8 ? NdOp::SUBBYTES : NdOp::COPY;
      Read.Output.Kind = MedVar::Reg;
      Read.Output.TheArch = Architecture;
      Read.Output.Id = 20;
      Read.Output.RegOff = TRI.IntReturnReg;
      Read.Output.Size = Location.ValueBytes;
      Read.addInput(Slot);
      if (SlotOffset % 8)
        Read.addInput(MedVar::makeConst(SlotOffset % 8, 4));
      MedOp Return;
      Return.Opcode = NdOp::RETURN;
      Return.addInput(Read.Output);
      MedBlock Block;
      Block.Id = 0;
      Block.Ops = {Read, Return};
      Func.Blocks.push_back(Block);
      inferMedTypes(Func, Architecture);
      ASSERT_TRUE(Func.SourceTypeHint);
      ASSERT_EQ(Func.Params[Index].RegOff, kNoParamReg);
      auto High = MedToHighConverter().convert(Func, Architecture);
      ASSERT_TRUE(High.SourceTypeHint);
      unsigned References = 0;
      auto Check = [&](auto &&Self, const ExprPtr &Expression) -> void {
        if (!Expression)
          return;
        if (Expression->Kind == ExprKind::Var &&
            Expression->Var.Kind == MedVar::Param) {
          ++References;
          EXPECT_EQ(Expression->Var.Id, int(Index));
          EXPECT_EQ(Expression->Var.StackOff, Location.EntryStackOffset);
          EXPECT_NE(Expression->Var.StackOff, -1);
          EXPECT_EQ(Expression->Var.Size, Location.ValueBytes);
        }
        for (const auto &Operand : Expression->Operands)
          Self(Self, Operand);
      };
      walkStmts(High.Body, [&](const HighStmt &Statement) {
        forEachRhsExpr(Statement, [&](const ExprPtr &Expression) {
          Check(Check, Expression);
        });
      });
      EXPECT_GT(References, 0U);
      EXPECT_EQ(Func.Params[Index].RegOff, kNoParamReg);
    }
  }
}

TEST(SourceABI, BitCastRejectsMismatchedWidthsAndDistinguishesTargetTypes) {
  auto Bits = HighExpr::makeConst(0x80000000, 4);
  auto Float = HighExpr::makeBitCast(Bits, NdType::makeFloat(4));
  auto Integer = HighExpr::makeBitCast(Bits, NdType::makeInt(4, true));
  EXPECT_EQ(Float->Kind, ExprKind::BitCast);
  EXPECT_FALSE(Float->structuralEq(*Integer));
  EXPECT_EQ(HighExpr::makeBitCast(Bits, NdType::makeFloat(8))->Kind,
            ExprKind::Undef);
  auto RoundTrip = HighExpr::makeBitCast(Float, Bits->Type);
  EXPECT_TRUE(RoundTrip->structuralEq(*Bits));
}

HighFunc scalarFloatFunction(Arch Architecture, uint16_t Width, bool Add,
                             bool Fused = false) {
  const unsigned InputCount = Fused ? 3 : Add ? 2 : 1;
  MedFunc Func;
  Func.Name = std::string(Architecture == Arch::X64 ? "x64_" : "a64_") +
              (Width == 4 ? "f32_" : "f64_") +
              (Fused ? "fma"
               : Add ? "add"
                     : "identity");
  auto Hint = declaration(NdType::makeFloat(Width));
  Hint.Parameters.push_back({"arg0", NdType::makeFloat(Width)});
  for (unsigned I = 1; I < InputCount; ++I)
    Hint.Parameters.push_back(
        {"arg" + std::to_string(I), NdType::makeFloat(Width)});
  Func.SourceTypeHint = Hint;
  const auto &TRI = getTargetRegInfo(Architecture);
  MedBlock Block;
  Block.Id = 0;
  std::vector<MedVar> Incoming;
  for (unsigned I = 0; I != InputCount; ++I) {
    MedVar Parameter;
    Parameter.Kind = MedVar::Reg;
    Parameter.TheArch = Architecture;
    Parameter.Id = 20 + I;
    Parameter.RegOff = TRI.FPParamRegs[I];
    Parameter.Size = Architecture == Arch::X64 ? 16 : Width;
    MedOp Marker;
    Marker.Opcode = NdOp::COPY;
    Marker.Output = Parameter;
    Marker.addInput(Parameter);
    Block.Ops.push_back(Marker);
    Incoming.push_back(Parameter);
  }
  if (Add || Fused) {
    for (unsigned I = 0; I != InputCount; ++I) {
      MedOp Slice;
      Slice.Opcode = NdOp::SUBBYTES;
      Slice.Output.Kind = MedVar::Temp;
      Slice.Output.Id = 30 + I;
      Slice.Output.Size = Width;
      Slice.addInput(Incoming[I]);
      Slice.addInput(MedVar::makeConst(0, 4));
      Block.Ops.push_back(Slice);
    }
    MedOp Sum;
    Sum.Opcode = Fused ? NdOp::FLOAT_FMA : NdOp::FLOAT_ADD;
    Sum.Output.Kind = MedVar::Temp;
    Sum.Output.Id = 36;
    Sum.Output.Size = Width;
    for (unsigned I = 0; I < InputCount; ++I)
      Sum.addInput(Block.Ops[InputCount + I].Output);
    Block.Ops.push_back(Sum);
    MedOp Widen;
    Widen.Opcode = NdOp::INT_ZEXT;
    Widen.Output.Kind = MedVar::Reg;
    Widen.Output.Id = 37;
    Widen.Output.Size = 16;
    Widen.Output.RegOff = TRI.FPReturnReg;
    Widen.addInput(Sum.Output);
    Block.Ops.push_back(Widen);
  }
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  // On x64 a generic return exposes RAX even when this declaration's result
  // is in XMM0. That unrelated integer carrier must not supply a float result.
  if (Architecture == Arch::X64) {
    MedVar RAX;
    RAX.Kind = MedVar::Reg;
    RAX.Id = 50;
    RAX.Size = 8;
    RAX.RegOff = TRI.IntReturnReg;
    Return.addInput(RAX);
  }
  Block.Ops.push_back(Return);
  Func.Blocks.push_back(Block);
  inferMedTypes(Func, Architecture);
  EXPECT_TRUE(Func.SourceTypeHint);
  auto High = MedToHighConverter().convert(Func, Architecture);
  EXPECT_TRUE(High.SourceTypeHint);
  EXPECT_EQ(High.ReturnType->Kind, NdTypeKind::Float);
  return High;
}

void executeC(const std::string &Source, bool Math = false) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  auto FoundCompiler = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(static_cast<bool>(FoundCompiler)) << "clang is required";
  const std::string Compiler = *FoundCompiler;
#endif
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-source-abi", "c", SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-source-abi", "exe",
                                                  BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-source-abi", "err",
                                                  ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream OS(SourcePath, EC);
    ASSERT_FALSE(EC) << EC.message();
    OS << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  for (llvm::StringRef Optimization : {"-O0", "-O2"}) {
    llvm::SmallVector<llvm::StringRef, 16> Arguments{
        Compiler,   "-std=c11", Optimization, "-Werror=return-type",
        SourcePath, "-o",       BinaryPath};
    (void)Math;
#ifndef _WIN32
    if (Math)
      Arguments.push_back("-lm");
#endif
    std::string Error;
    int Status = llvm::sys::ExecuteAndWait(Compiler, Arguments, std::nullopt,
                                           Redirects, 30, 0, &Error);
    auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Status, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << '\n'
                         << Source;
    llvm::SmallVector<llvm::StringRef, 1> RunArguments{BinaryPath};
    Status = llvm::sys::ExecuteAndWait(BinaryPath, RunArguments, std::nullopt,
                                       Redirects, 30, 0, &Error);
    Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    EXPECT_EQ(Status, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << '\n'
                         << Source;
  }
}

TEST(SourceABI, IndirectRecordCallInitializesAllFieldsAndKeepsReturnClobbers) {
  const auto Architecture = Arch::AArch64;
  const auto &TRI = getTargetRegInfo(Architecture);
  const auto Word = NdType::makeInt(8, false);
  const auto SignedWord = NdType::makeInt(8, true);
  const auto Record = NdType::makeStruct({SignedWord, SignedWord, SignedWord});
  SourceFunctionTypeHint Entry;
  Entry.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Entry.ReturnType = Word;
  Entry.Parameters = {{"value", Word}, {"output", NdType::makePtr(Word)}};
  SourceFunctionTypeHint Callee;
  Callee.ReturnType = Record;
  Callee.Parameters = {{"value", Word}};
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(Entry, Architecture, Error));
  ASSERT_TRUE(assignDarwinFixedSourceABI(Callee, Architecture, Error));
  for (bool ReadUnspecifiedResult : {false, true}) {
    LowFunc Low;
    Low.Entry = 0x1000;
    Low.Name = "indirect_record";
    LowBlock Block;
    Block.Id = 0;
    Block.StartAddr = Low.Entry;
    auto Add = [&](NdOp Opcode, NdVar Output,
                   std::initializer_list<NdVar> Inputs) {
      LowOp Op;
      Op.Opcode = Opcode;
      Op.Addr = Low.Entry + Block.Ops.size() * 4;
      Op.Output = Output;
      for (const auto &Input : Inputs)
        Op.addInput(Input);
      Block.Ops.push_back(Op);
    };
    const auto X0 = NdVar::reg(TRI.IntReturnReg, 8);
    const auto X8 = NdVar::reg(TRI.indirectResultReg(), 8);
    const auto Buffer = NdVar::reg(a64reg::X19, 8);
    Add(NdOp::COPY, Buffer, {NdVar::reg(TRI.IntParamRegs[1], 8)});
    Add(NdOp::COPY, X8, {Buffer});
    Add(NdOp::CALL, {}, {NdVar::cst(0x2000, 8)});
    // The hidden pointer is caller-saved. Post-call stores must retain the
    // pre-call address even after this register is overwritten.
    Add(NdOp::COPY, X8, {NdVar::cst(0, 8)});
    if (!ReadUnspecifiedResult) {
      Add(NdOp::LOAD, NdVar::tmp(0, 8), {Buffer});
      Add(NdOp::INT_ADD, NdVar::tmp(1, 8), {Buffer, NdVar::cst(8, 8)});
      Add(NdOp::LOAD, NdVar::tmp(2, 8), {NdVar::tmp(1, 8)});
      Add(NdOp::INT_ADD, NdVar::tmp(3, 8), {Buffer, NdVar::cst(16, 8)});
      Add(NdOp::LOAD, NdVar::tmp(4, 8), {NdVar::tmp(3, 8)});
      Add(NdOp::INT_ADD, NdVar::tmp(5, 8),
          {NdVar::tmp(0, 8), NdVar::tmp(2, 8)});
      Add(NdOp::INT_ADD, X0, {NdVar::tmp(5, 8), NdVar::tmp(4, 8)});
    }
    Add(NdOp::RETURN, {}, {X0});
    Block.EndAddr = Low.Entry + Block.Ops.size() * 4;
    Low.Blocks.push_back(Block);
    std::map<va_t, SourceFunctionTypeHint> Hints{{Low.Entry, Entry},
                                                 {0x2000, Callee}};
    LowToMedConverter Converter;
    Converter.setSourceCallHintsEnabled(true);
    Converter.setSourceCalleeTypeHints(&Hints);
    auto Med = Converter.convert(Low, Architecture, BinaryFormat::MachO);
    Med.SourceTypeHint = Entry;
    const std::map<va_t, std::string> Names{{0x2000, "make_three"}};
    recoverCallAbi(Med, Architecture, Names);
    inferMedTypes(Med, Architecture);
    MedToHighConverter HighConverter;
    HighConverter.setFuncNames(&Names);
    auto High = HighConverter.convert(Med, Architecture);
    unsigned Unknown = 0;
    walkStmts(High.Body, [&](const HighStmt &Statement) {
      forEachExpr(Statement, [&](const ExprPtr &Expression) {
        Unknown += Expression && Expression->Kind == ExprKind::Undef;
      });
    });
    if (ReadUnspecifiedResult) {
      EXPECT_GT(Unknown, 0U);
      continue;
    }
    EXPECT_EQ(Unknown, 0U);
    ASSERT_TRUE(High.SourceTypeHint);
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
    Source += "\nstatic unsigned calls;\n" + typeToC(Record) +
              " make_three(uint64_t value) { ++calls; " + typeToC(Record) +
              " result; uint64_t words[] = {value, value + 7, value * 3}; "
              "memcpy(&result, words, sizeof(result)); return result; }\n";
    executeC(Source + R"(
int main(void) {
    const uint64_t inputs[] = {0, 1, 17, UINT64_MAX, UINT64_C(0x8000000000000000)};
    for (unsigned i = 0; i != 5; ++i) {
        uint64_t output[] = {101, 102, 103, 104};
        uint64_t value = inputs[i];
        unsigned before = calls;
        if (indirect_record(value, output) != value + (value + 7) + value * 3) return 1;
        if (output[0] != value || output[1] != value + 7 || output[2] != value * 3) return 2;
        if (output[3] != 104 || calls != before + 1) return 3;
    }
    return 0;
}
)");
  }
}

TEST(SourceABI, PartialFPRegisterPreservationDoesNotInventArguments) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    for (bool ObserveUpper : {false, true}) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(ObserveUpper);
      SourceFunctionTypeHint Hint;
      Hint.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      Hint.ReturnType = NdType::makeFloat(8);
      Hint.Parameters = {{"integer", NdType::makeInt(8)},
                         {"floating", NdType::makeFloat(8)},
                         {"output", NdType::makePtr(NdType::makeInt(8))}};
      std::string Error;
      ASSERT_TRUE(assignDarwinScalarSourceABI(Hint, Architecture, Error));
      LowFunc Low;
      Low.Entry = 0x1200;
      Low.Name = "partial_fp";
      LowBlock Block;
      Block.Id = 0;
      Block.StartAddr = Low.Entry;
      Block.EndAddr = Low.Entry + 4;
      auto Add = [&](NdOp Opcode, NdVar Output,
                     std::initializer_list<NdVar> Inputs) {
        LowOp Op;
        Op.Opcode = Opcode;
        Op.Addr = Low.Entry;
        Op.Output = Output;
        for (const auto &Input : Inputs)
          Op.addInput(Input);
        Block.Ops.push_back(Op);
      };
      const auto Return = NdVar::reg(TRI.FPReturnReg, 16);
      const auto Scratch = NdVar::reg(TRI.FPParamRegs[2], 16);
      Add(NdOp::FLOAT_INT2FLOAT, NdVar::tmp(0, 8),
          {NdVar::reg(Hint.Parameters[0].Location.RegisterOffset, 8)});
      Add(NdOp::SUBBYTES, NdVar::tmp(1, 8), {Scratch, NdVar::cst(8, 4)});
      Add(NdOp::CONCAT, Scratch, {NdVar::tmp(1, 8), NdVar::tmp(0, 8)});
      Add(NdOp::SUBBYTES, NdVar::tmp(2, 8), {Scratch, NdVar::cst(0, 4)});
      Add(NdOp::SUBBYTES, NdVar::tmp(3, 8), {Return, NdVar::cst(0, 4)});
      Add(NdOp::FLOAT_ADD, NdVar::tmp(4, 8),
          {NdVar::tmp(3, 8), NdVar::tmp(2, 8)});
      Add(NdOp::SUBBYTES, NdVar::tmp(5, 8), {Return, NdVar::cst(8, 4)});
      Add(NdOp::CONCAT, Return, {NdVar::tmp(5, 8), NdVar::tmp(4, 8)});
      if (ObserveUpper)
        Add(NdOp::STORE, {},
            {NdVar::reg(Hint.Parameters[2].Location.RegisterOffset, 8),
             NdVar::tmp(1, 8)});
      Add(NdOp::RETURN, {}, {NdVar::reg(TRI.IntReturnReg, 8)});
      Low.Blocks.push_back(Block);
      std::map<va_t, SourceFunctionTypeHint> Hints{{Low.Entry, Hint}};
      LowToMedConverter Converter;
      Converter.setSourceCalleeTypeHints(&Hints);
      auto Med = Converter.convert(Low, Architecture, BinaryFormat::MachO);
      Med.SourceTypeHint = Hint;
      inferMedTypes(Med, Architecture);
      if (ObserveUpper) {
        EXPECT_FALSE(Med.SourceTypeHint);
        continue;
      }
      EXPECT_TRUE(Med.SourceTypeHint);
      if (!Med.SourceTypeHint)
        continue;
      auto High = MedToHighConverter().convert(Med, Architecture);
      ASSERT_TRUE(High.SourceTypeHint);
      EXPECT_EQ(High.Params.size(), Hint.Parameters.size());
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      CEmitterOptions Options;
      Options.TheArch = Architecture;
      ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
      executeC(Source + R"(
int main(void) {
    for (int64_t i = -512; i < 512; ++i) {
        int64_t output = 1234;
        double input = (double)i * 0.25;
        if (partial_fp(i, input, &output) != (double)i + input) return 1;
        if (output != 1234) return 2;
    }
    return 0;
}
)");
    }
  }
}

TEST(SourceABI, EntryByteDemandsKeepPhiEffectsAndRejectIncompleteGraphs) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    SourceFunctionTypeHint Hint;
    Hint.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Hint.ReturnType = NdType::makeInt(8);
    Hint.Parameters = {{"condition", NdType::makeInt(8)},
                       {"output", NdType::makePtr(NdType::makeInt(8))}};
    std::string Error;
    ASSERT_TRUE(assignDarwinScalarSourceABI(Hint, Architecture, Error));
    auto Variable = [&](int Id, unsigned Size,
                        uint64_t Register = kNoParamReg) {
      MedVar V;
      V.Kind = Register == kNoParamReg ? MedVar::Temp : MedVar::Reg;
      V.Id = Id;
      V.Size = Size;
      V.RegOff = Register;
      V.TheArch = Architecture;
      return V;
    };
    auto Operation = [](NdOp Opcode, MedVar Output,
                        std::initializer_list<MedVar> Inputs) {
      MedOp Op;
      Op.Opcode = Opcode;
      Op.Output = Output;
      for (const auto &Input : Inputs)
        Op.addInput(Input);
      return Op;
    };
    const auto Unknown = Variable(10, 16, TRI.FPParamRegs[2]);
    const auto Pointer =
        Variable(11, 8, Hint.Parameters[1].Location.RegisterOffset);
    const auto Condition =
        Variable(12, 8, Hint.Parameters[0].Location.RegisterOffset);
    auto Returned = Variable(30, 8, TRI.IntReturnReg);
    Returned.SSAVer = 1;
    for (unsigned Case = 0; Case < 5; ++Case) {
      MedFunc Function;
      Function.Entry = 0x1000;
      Function.Blocks.resize(4);
      for (unsigned I = 0; I < 4; ++I) {
        Function.Blocks[I].Id = I;
        Function.Blocks[I].StartAddr = 0x1000 + I * 16;
      }
      auto &Entry = Function.Blocks[0];
      Entry.Succs = {1, 2};
      Entry.Ops = {Operation(NdOp::COPY, Unknown, {Unknown}),
                   Operation(NdOp::COPY, Pointer, {Pointer}),
                   Operation(NdOp::COPY, Condition, {Condition}),
                   Operation(NdOp::COND_BR, {},
                             {MedVar::makeConst(0x1020, 8), Condition})};
      auto &Left = Function.Blocks[1];
      Left.Preds = {0};
      Left.Succs = {3};
      Left.Ops = {Operation(NdOp::SUBBYTES, Variable(20, 8),
                            {Unknown, MedVar::makeConst(8, 4)}),
                  Operation(NdOp::CONCAT, Variable(21, 16),
                            {Variable(20, 8), MedVar::makeConst(7, 8)})};
      auto &Right = Function.Blocks[2];
      Right.Preds = {0};
      Right.Succs = {3};
      Right.Ops = {
          Operation(NdOp::CONCAT, Variable(22, 16),
                    {MedVar::makeConst(0, 8), MedVar::makeConst(9, 8)})};
      auto &Join = Function.Blocks[3];
      Join.Preds = {1, 2};
      PhiNode Phi;
      Phi.Output = Variable(23, 16);
      Phi.Args = {{1, Variable(21, 16)}, {2, Variable(22, 16)}};
      Join.Phis.push_back(Phi);
      Join.Ops = {Operation(NdOp::SUBBYTES, Returned,
                            {Phi.Output, MedVar::makeConst(0, 4)}),
                  Operation(NdOp::RETURN, {}, {Returned})};
      if (Case == 1) {
        Join.Ops.insert(
            Join.Ops.begin(),
            {Operation(NdOp::SUBBYTES, Variable(24, 8),
                       {Phi.Output, MedVar::makeConst(8, 4)}),
             Operation(NdOp::STORE, {}, {Pointer, Variable(24, 8)})});
      } else if (Case == 2) {
        Join.Phis[0].Args[1].second = Variable(99, 16);
      } else if (Case == 3) {
        Left.Ops.push_back(Left.Ops.back());
      } else if (Case == 4) {
        Join.Ops.pop_back();
      }
      const auto Blocks = Function.Blocks;
      std::vector<unsigned> Order{0, 1, 2, 3};
      do {
        Function.Blocks.clear();
        for (auto Index : Order)
          Function.Blocks.push_back(Blocks[Index]);
        const auto Observed = observedMedSourceEntryRegisters(Function, Hint);
        EXPECT_EQ(bool(Observed), Case < 2);
        if (Observed)
          EXPECT_EQ(Observed->count(Unknown.RegOff), Case == 1 ? 1U : 0U);
      } while (std::next_permutation(Order.begin(), Order.end()));
    }
  }
}

TEST(SourceABI, EntryDemandsDistinguishImplicitCallDefinitionsFromInputs) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    SourceFunctionTypeHint Hint;
    Hint.ReturnType = NdType::makeInt(8);
    std::string Error;
    ASSERT_TRUE(assignDarwinScalarSourceABI(Hint, Architecture, Error));
    for (int Version : {0, 3})
      for (bool Used : {false, true}) {
        MedVar Clobbered;
        Clobbered.Kind = MedVar::Reg;
        Clobbered.Id = 10;
        Clobbered.RegOff =
            Architecture == Arch::AArch64 ? a64reg::X8 : x86reg::R10;
        Clobbered.Size = 8;
        Clobbered.SSAVer = Version;
        Clobbered.TheArch = Architecture;
        MedOp Copy;
        Copy.Opcode = NdOp::COPY;
        Copy.Output = Clobbered;
        Copy.Output.Id = 20;
        Copy.Output.RegOff = getTargetRegInfo(Architecture).IntReturnReg;
        Copy.addInput(Used ? Clobbered : MedVar::makeConst(7, 8));
        MedOp Return;
        Return.Opcode = NdOp::RETURN;
        MedFunc Function;
        Function.Blocks.resize(1);
        Function.Blocks[0].Ops = {Copy, Return};
        Function.CallClobbers.push_back({Clobbered, 1});
        const auto Observed = observedMedSourceEntryRegisters(Function, Hint);
        EXPECT_EQ(bool(Observed), !Used);
        if (Observed)
          EXPECT_TRUE(Observed->empty());
      }
  }
}

TEST(SourceABI, EntryDemandsTraceOnlyProvenCallPreservedPrefixes) {
  SourceFunctionTypeHint Hint;
  Hint.ReturnType = NdType::makeInt(8);
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(Hint, Arch::AArch64, Error));
  MedVar Before;
  Before.Kind = MedVar::Reg;
  Before.Id = 10;
  Before.RegOff = a64reg::V(8);
  Before.Size = 16;
  Before.TheArch = Arch::AArch64;
  auto After = Before;
  After.SSAVer = 1;
  for (unsigned Offset : {0, 8}) {
    MedOp Extract;
    Extract.Opcode = NdOp::SUBBYTES;
    Extract.Output = Before;
    Extract.Output.Id = 20;
    Extract.Output.RegOff = a64reg::X0;
    Extract.Output.Size = 8;
    Extract.addInput(After);
    Extract.addInput(MedVar::makeConst(Offset, 4));
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    MedFunc Function;
    Function.Blocks.resize(1);
    Function.Blocks[0].Ops = {Extract, Return};
    Function.CallClobbers.push_back({After, 1, Before, 8});
    const auto Observed = observedMedSourceEntryRegisters(Function, Hint);
    EXPECT_EQ(bool(Observed), Offset == 0);
    if (Observed)
      EXPECT_EQ(*Observed, std::set<uint64_t>{Before.RegOff});
    Function.CallClobbers[0].PreservedInput.Size = 4;
    EXPECT_FALSE(observedMedSourceEntryRegisters(Function, Hint));
    Function.CallClobbers[0].PreservedInput = After;
    EXPECT_FALSE(observedMedSourceEntryRegisters(Function, Hint));
  }
}

TEST(SourceABI, EffectEntryDemandsDoNotCertifyUnprovenReturnBytes) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
      SCOPED_TRACE(Mutation);
      const auto &TRI = getTargetRegInfo(Architecture);
      SourceFunctionTypeHint Hint;
      Hint.ReturnType = NdType::makeInt(8);
      std::string Error;
      ASSERT_TRUE(assignDarwinScalarSourceABI(Hint, Architecture, Error));
      MedVar Context;
      Context.Kind = MedVar::Reg;
      Context.Id = 10;
      Context.RegOff =
          Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13;
      Context.Size = 8;
      Context.TheArch = Architecture;
      auto Unknown = Context;
      Unknown.Id = 11;
      Unknown.RegOff = TRI.IntReturnReg;
      Unknown.SSAVer = 1;
      MedOp Load;
      Load.Opcode = NdOp::LOAD;
      Load.Output = Context;
      Load.Output.Kind = MedVar::Temp;
      Load.Output.Id = 12;
      Load.addInput(Mutation == 1 ? Unknown : Context);
      MedOp Return;
      Return.Opcode = NdOp::RETURN;
      MedFunc Function;
      Function.Blocks.resize(1);
      Function.Blocks[0].Ops = {Load, Return};
      Function.CallClobbers.push_back({Unknown, 1});
      if (Mutation == 2)
        Function.CallClobbers.push_back({Context, 1});
      if (Mutation == 3)
        Function.Blocks[0].Ops.pop_back();
      EXPECT_FALSE(observedMedSourceEntryBytes(Function, Hint));
      const auto Effects = observedMedSourceEntryBytes(
          Function, Hint, SourceEntryDemand::EffectsOnly);
      if (Mutation) {
        EXPECT_FALSE(Effects);
      } else {
        ASSERT_TRUE(Effects);
        EXPECT_EQ(*Effects,
                  (std::map<uint64_t, uint64_t>{{Context.RegOff, 0xff}}));
      }
    }
}

TEST(SourceABI, IntegerPairCallsPreserveBothWordsThroughSSAAndReturns) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    const auto &TRI = getTargetRegInfo(Architecture);
    SourceFunctionTypeHint Pair;
    Pair.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Pair.ReturnType = NdType::makeInt(16, false);
    std::string Diagnostic;
    ASSERT_TRUE(assignDarwinScalarSourceABI(Pair, Architecture, Diagnostic));
    std::map<va_t, SourceFunctionTypeHint> Hints{{0x1100, Pair}};
    auto Scalar = Pair;
    Scalar.ReturnType = NdType::makeInt(8, false);
    ASSERT_TRUE(assignDarwinScalarSourceABI(Scalar, Architecture, Diagnostic));
    Hints[0x1150] = Scalar;
    std::vector<HighFunc> Functions;
    auto Operation = [](NdOp Opcode, NdVar Output,
                        std::initializer_list<NdVar> Inputs) {
      LowOp Op;
      Op.Opcode = Opcode;
      Op.Addr = 0x1200;
      Op.Output = Output;
      for (const auto &Input : Inputs)
        Op.addInput(Input);
      return Op;
    };
    for (unsigned Mode = 0; Mode != 5; ++Mode) {
      LowFunc Low;
      Low.Entry = 0x1200;
      Low.Name = "pair_mode_" + std::to_string(Mode);
      LowBlock Block;
      Block.Id = 0;
      Block.StartAddr = Low.Entry;
      Block.EndAddr = 0x1220;
      Block.Ops.push_back(Operation(NdOp::CALL, NdVar::reg(TRI.IntReturnReg, 8),
                                    {NdVar::cst(0x1100, 8)}));
      auto Entry = Pair;
      if (Mode == 1) {
        Entry.ReturnType = NdType::makeInt(8, false);
        ASSERT_TRUE(
            assignDarwinScalarSourceABI(Entry, Architecture, Diagnostic));
        Block.Ops.push_back(Operation(NdOp::COPY,
                                      NdVar::reg(TRI.IntReturnReg, 8),
                                      {NdVar::reg(TRI.IntReturnRegs[1], 8)}));
      } else if (Mode == 2) {
        Block.Ops.push_back(Operation(NdOp::COPY,
                                      NdVar::reg(TRI.IntReturnRegs[1], 8),
                                      {NdVar::cst(0x123456789abcdef0ULL, 8)}));
      } else if (Mode == 3) {
        // A later scalar call still destroys the second word. The source
        // declaration of the earlier call cannot make that clobber disappear.
        Block.Ops.push_back(Operation(NdOp::CALL,
                                      NdVar::reg(TRI.IntReturnReg, 8),
                                      {NdVar::cst(0x1150, 8)}));
      }
      Block.Ops.push_back(Operation(NdOp::RETURN, {}, {}));
      Low.Blocks.push_back(Block);
      if (Mode == 4) {
        Low.Blocks.resize(4);
        Low.Blocks[0].Ops.back() =
            Operation(NdOp::COND_BR, {},
                      {NdVar::cst(0x1400, 8), NdVar::reg(TRI.IntReturnReg, 1)});
        Low.Blocks[0].Succs = {1, 2};
        for (unsigned I = 1; I != 4; ++I) {
          auto &B = Low.Blocks[I];
          B.Id = I;
          B.StartAddr = 0x1200 + I * 0x100;
          B.EndAddr = B.StartAddr + 0x10;
          if (I != 3) {
            B.Preds = {0};
            B.Succs = {3};
            if (I == 2)
              B.Ops.push_back(Operation(NdOp::COPY,
                                        NdVar::reg(TRI.IntReturnRegs[1], 8),
                                        {NdVar::cst(17, 8)}));
            B.Ops.push_back(
                Operation(NdOp::BRANCH, {}, {NdVar::cst(0x1500, 8)}));
          } else {
            B.Preds = {1, 2};
            B.Ops.push_back(Operation(NdOp::RETURN, {}, {}));
          }
        }
      }
      Hints[Low.Entry] = Entry;
      LowToMedConverter Converter;
      Converter.setSourceCallHintsEnabled(true);
      Converter.setSourceCalleeTypeHints(&Hints);
      auto Med = Converter.convert(Low, Architecture, BinaryFormat::MachO);
      recoverCallAbi(Med, Architecture, {});
      Med.SourceTypeHint = Entry;
      inferMedTypes(Med, Architecture);
      auto High = MedToHighConverter().convert(Med, Architecture);
      EXPECT_TRUE(Med.MultiReturn.empty());
      if (Mode == 3) {
        std::string Source;
        llvm::raw_string_ostream OS(Source);
        CEmitterOptions Options;
        Options.TheArch = Architecture;
        ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
        EXPECT_EQ(Source.find("caller-saved register clobbered"),
                  std::string::npos)
            << Source;
      } else {
        Functions.push_back(std::move(High));
      }
    }
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    ASSERT_TRUE(HighCEmitter().emit(Functions, OS, Options));
    EXPECT_EQ(Source.find("caller-saved register clobbered"), std::string::npos)
        << Source;
    EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;
    Source += R"(
static uint64_t low_word, high_word;
static unsigned calls;
unsigned __int128 sub_1100(void) {
  ++calls;
  return ((unsigned __int128)high_word << 64) | low_word;
}
int main(void) {
  for (unsigned i = 0; i != 4096; ++i) {
    low_word = UINT64_C(0x9e3779b97f4a7c15) * i;
    high_word = UINT64_C(0xfedcba9876543210) ^ ~low_word;
    unsigned __int128 expected = ((unsigned __int128)high_word << 64) | low_word;
    calls = 0;
    if (pair_mode_0() != expected || calls != 1) return 1;
    if (pair_mode_1() != high_word || calls != 2) return 2;
    expected = ((unsigned __int128)UINT64_C(0x123456789abcdef0) << 64) | low_word;
    if (pair_mode_2() != expected || calls != 3) return 3;
    expected = ((unsigned __int128)((uint8_t)low_word ? 17 : high_word) << 64) | low_word;
    if (pair_mode_4() != expected || calls != 4) return 4;
  }
  return 0;
}
)";
    ASSERT_NO_FATAL_FAILURE(executeC(Source));
  }
}

TEST(SourceABI, NarrowDarwinReturnsPreserveWordReadsWithoutInventingHighBits) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto ReturnReg = getTargetRegInfo(Architecture).IntReturnReg;
    const auto SavedReg =
        Architecture == Arch::AArch64 ? a64reg::X19 : x86reg::RBX;
    SCOPED_TRACE(static_cast<int>(Architecture));
    for (const uint16_t Bytes : {1, 2, 4}) {
      for (const bool Signed : {false, true}) {
        SCOPED_TRACE(Bytes);
        SCOPED_TRACE(Signed);
        const uint16_t KnownBytes = Architecture == Arch::AArch64 ? 4 : Bytes;
        SourceFunctionTypeHint Callee;
        Callee.ReturnType = NdType::makeInt(Bytes, Signed);
        std::string Error;
        ASSERT_TRUE(assignDarwinScalarSourceABI(Callee, Architecture, Error));
        std::map<va_t, SourceFunctionTypeHint> Hints{{0x1100, Callee}};
        std::string Source;
        llvm::raw_string_ostream OS(Source);
        std::vector<HighFunc> Functions;
        for (const uint16_t ReadBytes : {4, 8, 12, 16}) {
          LowFunc Low;
          Low.Entry = 0x1200;
          Low.Name = "narrow_read_" + std::to_string(ReadBytes);
          LowBlock Block;
          Block.Id = 0;
          Block.StartAddr = 0x1200;
          Block.EndAddr = 0x1208;
          LowOp Call;
          Call.Addr = 0x1200;
          Call.Opcode = NdOp::CALL;
          Call.Output = NdVar::reg(ReturnReg, 8);
          Call.addInput(NdVar::cst(0x1100, 8));
          LowOp Return;
          Return.Addr = 0x1204;
          Return.Opcode = NdOp::RETURN;
          Block.Ops = {Call};
          if (ReadBytes == 8) {
            // A full-register move preserves the known low word even though
            // the call's upper word is not part of the declared result.
            LowOp Copy;
            Copy.Addr = 0x1204;
            Copy.Opcode = NdOp::COPY;
            Copy.Output = NdVar::reg(SavedReg, 8);
            Copy.addInput(NdVar::reg(ReturnReg, 8));
            Block.Ops.push_back(Copy);
            Copy.Output = NdVar::reg(ReturnReg, 8);
            Copy.NumInputs = 0;
            Copy.addInput(NdVar::reg(SavedReg, 8));
            Block.Ops.push_back(Copy);
            Return.addInput(NdVar::reg(ReturnReg, KnownBytes));
          } else {
            Return.addInput(
                NdVar::reg(ReturnReg, ReadBytes == 16 ? 8 : KnownBytes));
          }
          Block.Ops.push_back(Return);
          Low.Blocks.push_back(Block);
          if (ReadBytes == 12) {
            // Merge a full saved register, then move it into the ABI result.
            // The return's low lane must reach back through that copy and
            // merge; its unused upper bits must not poison the source body.
            Low.Blocks.clear();
            Low.Blocks.resize(4);
            for (int I = 0; I < 4; ++I) {
              auto &B = Low.Blocks[I];
              B.Id = I;
              B.StartAddr = 0x1200 + I * 0x100;
              B.EndAddr = B.StartAddr + 0x20;
            }
            LowOp Branch;
            Branch.Opcode = NdOp::COND_BR;
            Branch.Addr = 0x1204;
            Branch.addInput(NdVar::cst(0x1400, 8));
            Branch.addInput(NdVar::reg(ReturnReg, 1));
            Low.Blocks[0].Ops = {Call, Branch};
            Low.Blocks[0].Succs = {1, 2};
            for (int I = 1; I <= 2; ++I) {
              auto &B = Low.Blocks[I];
              B.Preds = {0};
              B.Succs = {3};
              LowOp Copy;
              Copy.Opcode = NdOp::COPY;
              Copy.Addr = B.StartAddr;
              Copy.Output = NdVar::reg(SavedReg, 8);
              Copy.addInput(I == 1 ? NdVar::reg(ReturnReg, 8)
                                   : NdVar::cst(1, 8));
              LowOp Jump;
              Jump.Opcode = NdOp::BRANCH;
              Jump.Addr = B.StartAddr + 4;
              Jump.addInput(NdVar::cst(0x1500, 8));
              B.Ops = {Copy, Jump};
            }
            LowOp Copy;
            Copy.Opcode = NdOp::COPY;
            Copy.Addr = 0x1500;
            Copy.Output = NdVar::reg(ReturnReg, 8);
            Copy.addInput(NdVar::reg(SavedReg, 8));
            Return.Addr = 0x1504;
            Low.Blocks[3].Preds = {1, 2};
            Low.Blocks[3].Ops = {Copy, Return};
          }
          SourceFunctionTypeHint Entry;
          Entry.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
          Entry.ReturnType =
              NdType::makeInt(ReadBytes == 16 ? 8 : KnownBytes,
                              Architecture == Arch::X64 && Signed);
          ASSERT_TRUE(assignDarwinScalarSourceABI(Entry, Architecture, Error));
          Hints[Low.Entry] = Entry;
          LowToMedConverter Converter;
          Converter.setSourceCallHintsEnabled(true);
          Converter.setSourceCalleeTypeHints(&Hints);
          auto Med = Converter.convert(Low, Architecture, BinaryFormat::MachO);
          recoverCallAbi(Med, Architecture, {{0x1100, "narrow_result"}});
          Med.SourceTypeHint = Entry;
          inferMedTypes(Med, Architecture);
          auto High = MedToHighConverter().convert(Med, Architecture);
          if (ReadBytes == 16) {
            std::string Unproven;
            llvm::raw_string_ostream UnprovenOS(Unproven);
            CEmitterOptions Options;
            Options.TheArch = Architecture;
            ASSERT_TRUE(HighCEmitter().emit({High}, UnprovenOS, Options));
            EXPECT_EQ(Unproven.find("caller-saved register clobbered"),
                      std::string::npos)
                << Unproven;
          } else {
            Functions.push_back(std::move(High));
          }
        }
        CEmitterOptions Options;
        Options.TheArch = Architecture;
        ASSERT_TRUE(HighCEmitter().emit(Functions, OS, Options));
        OS.flush();
        EXPECT_EQ(Source.find("caller-saved register clobbered"),
                  std::string::npos)
            << Source;
        const std::string Type = std::string(Signed ? "int" : "uint") +
                                 std::to_string(Bytes * 8) + "_t";
        Source += "\nstatic uint32_t input;\n" + Type +
                  " sub_1100(void) { return (" + Type + ")input; }\n";
        Source += "int main(void) {\n"
                  "const uint32_t edges[] = {0x7fffffffU, 0x80000000U, "
                  "0xfffffffeU, 0xffffffffU};\n"
                  "for (unsigned round = 0; round != 65540; ++round) {\n"
                  "input = round < 65536 ? round : edges[round - 65536];\n"
                  "uint32_t expected = (uint32_t)(" +
                  Type +
                  ")input;\n"
                  "if ((uint32_t)narrow_read_4() != expected) return 1;\n"
                  "if ((uint32_t)narrow_read_8() != expected) return 2;\n"
                  "if ((uint32_t)narrow_read_12() != "
                  "((uint8_t)input ? 1U : expected)) return 3;\n"
                  "}\nreturn 0;\n}\n";
        ASSERT_NO_FATAL_FAILURE(executeC(Source));
      }
    }
  }
}

TEST(SourceABI, NarrowParameterCallArgumentsKeepDeclaredWidths) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (uint16_t Bytes : {1, 2}) {
      for (bool Signed : {false, true}) {
        SourceFunctionTypeHint Hint;
        Hint.ReturnType = NdType::makeInt(8, true);
        Hint.Parameters = {{"value", NdType::makeInt(Bytes, Signed)}};
        std::string Error;
        ASSERT_TRUE(assignDarwinScalarSourceABI(Hint, Architecture, Error));
        LowFunc Low;
        Low.Entry = 0x1200;
        Low.Name = "forward_narrow";
        LowBlock Block;
        Block.Id = 0;
        Block.StartAddr = Low.Entry;
        Block.EndAddr = Low.Entry + 8;
        LowOp Call;
        Call.Opcode = NdOp::CALL;
        Call.Addr = Low.Entry;
        Call.addInput(NdVar::cst(0x1100, 8));
        Block.Ops.push_back(Call);
        LowOp Return;
        Return.Opcode = NdOp::RETURN;
        Return.Addr = Low.Entry + 4;
        Return.addInput(
            NdVar::reg(getTargetRegInfo(Architecture).IntReturnReg, 8));
        Block.Ops.push_back(Return);
        Low.Blocks.push_back(Block);
        std::map<va_t, SourceFunctionTypeHint> Hints{{0x1100, Hint},
                                                     {Low.Entry, Hint}};
        LowToMedConverter Converter;
        Converter.setSourceCallHintsEnabled(true);
        Converter.setSourceCalleeTypeHints(&Hints);
        auto Med = Converter.convert(Low, Architecture, BinaryFormat::MachO);
        recoverCallAbi(Med, Architecture, {});
        Med.SourceTypeHint = Hint;
        inferMedTypes(Med, Architecture);
        const auto High = MedToHighConverter().convert(Med, Architecture);
        std::string Source;
        llvm::raw_string_ostream OS(Source);
        CEmitterOptions Options;
        Options.TheArch = Architecture;
        ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
        ASSERT_EQ(Source.find("bad source call"), std::string::npos) << Source;
        const auto Type = std::string(Signed ? "int" : "uint") +
                          std::to_string(Bytes * 8) + "_t";
        Source += "\nstatic unsigned calls;\nint64_t sub_1100(" + Type +
                  " value) { ++calls; return (int64_t)value - 17; }\n"
                  "int main(void) { for (uint32_t i=0;i!=65536;++i) {\n" +
                  Type + " value=(" + Type +
                  ")i;\n"
                  "if (forward_narrow(value) != (int64_t)value-17 || "
                  "calls != i+1) return 1; } return 0; }\n";
        ASSERT_NO_FATAL_FAILURE(executeC(Source));
      }
    }
  }
}

TEST(SourceABI, NarrowParametersPreserveKnownBytesThroughWideCopies) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    const auto Argument = NdVar::reg(TRI.IntParamRegs[0], 8);
    const auto Saved = NdVar::reg(TRI.CalleeSaveRegs.front(), 8);
    for (const uint16_t Bytes : {1, 2, 4}) {
      for (const bool Signed : {false, true}) {
        std::vector<HighFunc> Functions;
        for (unsigned Mode = 0; Mode != 5; ++Mode) {
          LowFunc Low;
          Low.Entry = 0x1200;
          Low.Name = "saved_parameter_" + std::to_string(Mode);
          LowBlock Block;
          Block.Id = 0;
          Block.StartAddr = 0x1200;
          Block.EndAddr = 0x1220;
          LowOp Save;
          Save.Opcode = NdOp::COPY;
          Save.Addr = 0x1200;
          Save.Output = Saved;
          Save.addInput(Argument);
          Block.Ops.push_back(Save);
          if (Mode != 0) {
            LowOp Call;
            Call.Opcode = NdOp::CALL;
            Call.Addr = 0x1204;
            Call.addInput(NdVar::cst(0x1100, 8));
            Block.Ops.push_back(Call);
          }
          LowOp Slice;
          Slice.Opcode = NdOp::SUBBYTES;
          Slice.Addr = 0x1208;
          Slice.Output = NdVar::reg(TRI.IntReturnReg, Mode == 4 ? 8 : 4);
          Slice.addInput(Mode == 2 ? Argument : Saved);
          Slice.addInput(NdVar::cst(Mode == 3 ? 4 : 0, 4));
          Block.Ops.push_back(Slice);
          LowOp Return;
          Return.Opcode = NdOp::RETURN;
          Return.Addr = 0x120c;
          Return.addInput(NdVar::reg(TRI.IntReturnReg, Mode == 4   ? 8
                                                       : Mode == 3 ? 1
                                                                   : 4));
          Block.Ops.push_back(Return);
          Low.Blocks.push_back(Block);
          SourceFunctionTypeHint Entry, Callee;
          Entry.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
          Entry.ReturnType = NdType::makeInt(Mode == 4   ? 8
                                             : Mode == 3 ? 1
                                                         : 4,
                                             Signed);
          Entry.Parameters = {{"value", NdType::makeInt(Bytes, Signed)}};
          Callee.ReturnType = NdType::makeVoid();
          std::string Error;
          ASSERT_TRUE(assignDarwinScalarSourceABI(Entry, Architecture, Error));
          ASSERT_TRUE(assignDarwinScalarSourceABI(Callee, Architecture, Error));
          std::map<va_t, SourceFunctionTypeHint> Hints{{0x1100, Callee},
                                                       {Low.Entry, Entry}};
          LowToMedConverter Converter;
          Converter.setSourceCallHintsEnabled(true);
          Converter.setSourceCalleeTypeHints(&Hints);
          auto Med = Converter.convert(Low, Architecture, BinaryFormat::MachO);
          recoverCallAbi(Med, Architecture, {});
          Med.SourceTypeHint = Entry;
          inferMedTypes(Med, Architecture);
          auto High = MedToHighConverter().convert(Med, Architecture);
          if (Mode < 2) {
            Functions.push_back(std::move(High));
          } else {
            std::string Source;
            llvm::raw_string_ostream OS(Source);
            CEmitterOptions Options;
            Options.TheArch = Architecture;
            ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
            EXPECT_NE(Source.find("unknown"), std::string::npos) << Source;
          }
        }
        std::string Source;
        llvm::raw_string_ostream OS(Source);
        CEmitterOptions Options;
        Options.TheArch = Architecture;
        ASSERT_TRUE(HighCEmitter().emit(Functions, OS, Options));
        EXPECT_EQ(Source.find("unknown"), std::string::npos) << Source;
        const auto Type = std::string(Signed ? "int" : "uint") +
                          std::to_string(Bytes * 8) + "_t";
        Source +=
            "\nstatic unsigned calls;\n"
            "void sub_1100(void) { ++calls; }\n"
            "int main(void) {\n"
            "for (uint32_t i = 0; i != 65536; ++i) {\n" +
            Type + " value = (" + Type +
            ")(i * UINT32_C(65537));\n"
            "if (saved_parameter_0(value) != value || calls != i) return 1;\n"
            "if (saved_parameter_1(value) != value || calls != i + 1) return "
            "2;\n"
            "}\nreturn 0;\n}\n";
        ASSERT_NO_FATAL_FAILURE(executeC(Source));
      }
    }
  }
}

TEST(SourceABI, UnsupportedArithmeticNeverManufacturesAZeroResult) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(Mutation);
      MedFunc M;
      M.Name = "unknown_arithmetic";
      M.ReturnType = NdType::makeInt(8, false);
      MedOp Arithmetic;
      Arithmetic.Opcode = Mutation == 0 ? NdOp::FLOAT_MIN : NdOp::FLOAT_FMA;
      Arithmetic.Output.Kind = MedVar::Reg;
      Arithmetic.Output.RegOff = getTargetRegInfo(Architecture).IntReturnReg;
      Arithmetic.Output.Id = 30;
      Arithmetic.Output.Size = Mutation == 2 ? 2 : 8;
      Arithmetic.Output.SSAVer = 1;
      Arithmetic.addInput(MedVar::makeConst(1, 8));
      Arithmetic.addInput(MedVar::makeConst(2, Mutation == 3 ? 4 : 8));
      if (Mutation >= 2)
        Arithmetic.addInput(MedVar::makeConst(3, 8));
      MedOp Return;
      Return.Opcode = NdOp::RETURN;
      Return.addInput(Arithmetic.Output);
      MedBlock Block;
      Block.Id = 0;
      Block.Ops = {Arithmetic, Return};
      M.Blocks.push_back(std::move(Block));
      const auto High = MedToHighConverter().convert(M, Architecture);
      const auto HasUnknown = [](const auto &Self,
                                 const ExprPtr &Expression) -> bool {
        if (!Expression)
          return false;
        if (Expression->Kind == ExprKind::Undef)
          return true;
        for (const auto &Operand : Expression->Operands)
          if (Self(Self, Operand))
            return true;
        return false;
      };
      bool Unknown = false;
      walkStmts(High.Body, [&](const HighStmt &Statement) {
        Unknown |= HasUnknown(HasUnknown, Statement.RetVal);
      });
      EXPECT_TRUE(Unknown);
    }
}

TEST(SourceABI, FusedMultiplyAddPreservesSingleRoundingAndAllOperands) {
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  std::string Checks;
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    std::vector<HighFunc> Functions;
    for (uint16_t Width : {4, 8})
      Functions.push_back(
          scalarFloatFunction(Architecture, Width, false, true));
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    ASSERT_TRUE(HighCEmitter().emit(Functions, OS, Options));
    const std::string Prefix = Architecture == Arch::X64 ? "x64_" : "a64_";
    Checks += "if (" + Prefix +
              "f32_fma(0, 0, 0x1.000002p0f, 0x1.fffffcp-1f, -1.0f) != "
              "-0x1p-46f) return 1;\n";
    Checks += "if (" + Prefix +
              "f64_fma(0, 0, 0x1.0000000000001p0, 0x1.ffffffffffffep-1, -1.0) "
              "!= -0x1p-104) return 2;\n";
    Checks += "if (" + Prefix +
              "f32_fma(0, 0, -7.5f, 2.0f, 4.25f) != -10.75f) return 3;\n";
    Checks += "if (" + Prefix +
              "f64_fma(0, 0, -7.5, 2.0, 4.25) != -10.75) return 4;\n";
  }
  OS.flush();
  executeC(Source + "\nint main(void) {\n" + Checks + "return 0;\n}\n", true);
}

TEST(SourceABI,
     FloatProjectionExecutesNumericOperationsAndPreservesIdentityBits) {
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  std::string Checks;
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    std::vector<HighFunc> Functions;
    for (uint16_t Width : {4, 8})
      for (bool Add : {false, true})
        Functions.push_back(scalarFloatFunction(Architecture, Width, Add));
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    ASSERT_TRUE(HighCEmitter().emit(Functions, OS, Options));
    const std::string Prefix = Architecture == Arch::X64 ? "x64_" : "a64_";
    Checks +=
        "if (" + Prefix + "f32_add(0, 0, -7.5f, 2.25f) != -5.25f) return 1;\n";
    Checks += "if (" + Prefix +
              "f64_add(0, 0, -1024.5, 3.125) != -1021.375) return 2;\n";
    Checks +=
        "for (unsigned i = 0; i < sizeof(fbits)/sizeof(fbits[0]); ++i) {\n"
        "float v; memcpy(&v, &fbits[i], 4); float r = " +
        Prefix +
        "f32_identity(0, 0, v); uint32_t bits; memcpy(&bits, &r, 4);"
        "if (bits != fbits[i]) return 3; }\n";
    Checks +=
        "for (unsigned i = 0; i < sizeof(dbits)/sizeof(dbits[0]); ++i) {\n"
        "double v; memcpy(&v, &dbits[i], 8); double r = " +
        Prefix +
        "f64_identity(0, 0, v); uint64_t bits; memcpy(&bits, &r, 8);"
        "if (bits != dbits[i]) return 4; }\n";
  }
  OS.flush();
  Source += R"(
#include <string.h>
int main(void) {
  const uint32_t fbits[] = {0, 0x80000000U, 0x7f800000U, 0xff800000U,
                           0x7fc12345U, 1, 0xc0f00000U};
  const uint64_t dbits[] = {0, UINT64_C(0x8000000000000000),
                           UINT64_C(0x7ff0000000000000),
                           UINT64_C(0xfff0000000000000),
                           UINT64_C(0x7ff8000000001234), 1,
                           UINT64_C(0xc020800000000000)};
)" + Checks +
            "return 0;\n}\n";
  executeC(Source);
}

} // namespace

TEST(SourceABI, DarwinVariadicArgumentsKeepPromotionsAndStackBoundary) {
  using namespace neverd;
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    SourceFunctionTypeHint Hint;
    Hint.ReturnType = NdType::makeVoid();
    Hint.Parameters = {{"format", NdType::makePtr(NdType::makeVoid())},
                       {"integer", NdType::makeInt(4)},
                       {"floating", NdType::makeFloat(8)},
                       {"pointer", NdType::makePtr(NdType::makeVoid())}};
    std::string Error;
    ASSERT_TRUE(assignDarwinVariadicSourceABI(Hint, 1, Architecture, Error));
    const auto &TRI = getTargetRegInfo(Architecture);
    EXPECT_EQ(Hint.Parameters[0].Location.RegisterOffset, TRI.IntParamRegs[0]);
    if (Architecture == Arch::AArch64) {
      for (unsigned I = 1; I < 4; ++I) {
        EXPECT_EQ(Hint.Parameters[I].Location.Kind,
                  SourceABICarrierKind::Stack);
        EXPECT_EQ(Hint.Parameters[I].Location.EntryStackOffset, (I - 1) * 8);
      }
    } else {
      EXPECT_EQ(Hint.Parameters[1].Location.RegisterOffset,
                TRI.IntParamRegs[1]);
      EXPECT_EQ(Hint.Parameters[2].Location.RegisterOffset, TRI.FPParamRegs[0]);
      EXPECT_EQ(Hint.Parameters[3].Location.RegisterOffset,
                TRI.IntParamRegs[2]);
    }
    for (const auto &Bad : {NdType::makeInt(1), NdType::makeInt(2),
                            NdType::makeFloat(4), NdType::makeInt(16)}) {
      Hint.Parameters.back().Type = Bad;
      EXPECT_FALSE(assignDarwinVariadicSourceABI(Hint, 1, Architecture, Error));
    }
  }
  SourceFunctionTypeHint Packed;
  Packed.ReturnType = NdType::makeVoid();
  Packed.Parameters.resize(11, {"value", NdType::makeInt(1, false)});
  Packed.Parameters.back().Type = NdType::makeFloat(8);
  std::string Error;
  ASSERT_TRUE(assignDarwinVariadicSourceABI(Packed, 10, Arch::AArch64, Error));
  EXPECT_EQ(Packed.Parameters[8].Location.EntryStackOffset, 0);
  EXPECT_EQ(Packed.Parameters[9].Location.EntryStackOffset, 1);
  EXPECT_EQ(Packed.Parameters[10].Location.EntryStackOffset, 8);
  EXPECT_FALSE(assignDarwinVariadicSourceABI(Packed, 0, Arch::AArch64, Error));
  EXPECT_FALSE(assignDarwinVariadicSourceABI(Packed, 12, Arch::AArch64, Error));
}
