#include "../../../lib/sdk/capi/ObjCSourceBindings.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/loader/ObjC/ObjCConstantStrings.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"
#include "neverd/loader/ObjC/ObjCSentinelCalls.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

using namespace neverd;
namespace {
constexpr auto Foundation =
    "/System/Library/Frameworks/Foundation.framework/Foundation";
constexpr auto CoreFoundation =
    "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation";

struct Fixture {
  BinaryImage Image;
  ObjCReceiverTypeHint Receiver;
  Fixture() {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.DynInfo.NeededLibs = {Foundation, CoreFoundation,
                                "/usr/lib/libobjc.A.dylib"};
    for (unsigned I = 0; I < 4; ++I) {
      Segment S;
      S.VA = 0x1000 + I * 0x1000;
      S.Size = S.FileSz = 0x1000;
      S.Data.resize(0x1000);
      S.Flags = SegmentFlags::Readable;
      if (!I)
        S.Flags = S.Flags | SegmentFlags::Executable;
      Image.Segments.push_back(S);
      Section Sec;
      Sec.VA = S.VA;
      Sec.Size = Sec.FileSz = S.Size;
      Sec.Flags = S.Flags;
      Sec.SegmentName = I == 1 || I == 3 ? "__DATA_CONST" : "__TEXT";
      Sec.Name = I == 1   ? "__cfstring"
                 : I == 2 ? "__cstring"
                 : I == 3 ? "__objc_classrefs"
                          : "__text";
      Sec.Type = I == 2   ? llvm::MachO::S_CSTRING_LITERALS
                 : I == 0 ? llvm::MachO::S_ATTR_PURE_INSTRUCTIONS
                          : 0;
      Image.Sections.push_back(Sec);
    }
    for (unsigned I = 0; I < 24; ++I) {
      const va_t Address = 0x2000 + I * 32;
      Image.recordDyldBindSlot(Address, "___CFConstantStringClassReference", 0,
                               CoreFoundation, false);
      auto *Record = Image.Segments[1].Data.data() + I * 32;
      llvm::support::endian::write64le(Record + 8, 0x7c8);
      llvm::support::endian::write64le(Record + 16, 0x3000 + I * 2);
      llvm::support::endian::write64le(Record + 24, 1);
      Image.Segments[2].Data[I * 2] = 'a' + I;
    }
    Image.recordDyldBindSlot(0x4800, "_OBJC_CLASS_$_NSSet", 0, CoreFoundation,
                             false);
    Image.ObjCSourceReferences[0x4800] = {ObjCSourceReference::Kind::Class,
                                          0x4800, 8, "NSSet"};
    Image.ObjCSourceReferences[0x4900] = {ObjCSourceReference::Kind::Selector,
                                          0x4900, 8, "setWithObjects:"};
    Image.ImportPtrSlots[0x4980] = "_objc_msgSend";
    Image.recordDyldBindSlot(0x4980, "_objc_msgSend", 0,
                             "/usr/lib/libobjc.A.dylib", false);
    // ADRP x1,0x4000; LDR x1,[x1,#0x900]; ADRP x16,0x4000;
    // LDR x16,[x16,#0x980]; BR x16.
    const uint32_t Stub[] = {0xf0000001, 0xf9448021, 0xf0000010, 0xf944c210,
                             0xd61f0200};
    for (unsigned I = 0; I < 5; ++I)
      llvm::support::endian::write32le(
          Image.Segments[0].Data.data() + 0x100 + I * 4, Stub[I]);
    Receiver.Origin = ObjCReceiverTypeHint::OriginKind::ClassReference;
    Receiver.Address = 0x4800;
    Receiver.ClassName = "NSSet";
    Receiver.IsClassMethod = true;
  }
};

LowOp op(NdOp Code, NdVar Output, std::initializer_list<NdVar> Inputs) {
  LowOp O;
  O.Opcode = Code;
  O.Output = Output;
  for (const auto &Input : Inputs)
    O.addInput(Input);
  return O;
}

LowFunc caller(unsigned Count) {
  LowFunc F;
  F.Entry = 0x1200;
  LowBlock B;
  B.Id = 0;
  B.StartAddr = 0x1200;
  const auto SP = NdVar::reg(a64reg::SP, 8);
  B.Ops = {op(NdOp::INT_SUB, SP, {SP, NdVar::cst(0x200, 8)}),
           op(NdOp::LOAD, NdVar::reg(a64reg::X0, 8), {NdVar::cst(0x4800, 8)}),
           op(NdOp::COPY, NdVar::reg(a64reg::X2, 8),
              {NdVar::cst(Count ? 0x2000 : 0, 8)})};
  for (unsigned I = 0; I < Count; ++I) {
    B.Ops.push_back(op(NdOp::INT_ADD, NdVar::reg(a64reg::X8, 8),
                       {SP, NdVar::cst(I * 8, 8)}));
    B.Ops.push_back(op(NdOp::STORE, {},
                       {NdVar::reg(a64reg::X8, 8),
                        NdVar::cst(I + 1 < Count ? 0x2020 + I * 32 : 0, 8)}));
  }
  B.Ops.push_back(op(NdOp::CALL, {}, {NdVar::cst(0x1100, 8)}));
  B.Ops.push_back(op(NdOp::RETURN, {}, {NdVar::reg(a64reg::X0, 8)}));
  for (size_t I = 0; I < B.Ops.size(); ++I)
    B.Ops[I].Addr = I + 2 == B.Ops.size()   ? 0x1500
                    : I + 1 == B.Ops.size() ? 0x1504
                                            : 0x1200 + I * 4;
  B.EndAddr = 0x1600;
  F.Blocks.push_back(B);
  return F;
}

ExprPtr reference(SourceCallTypeHint::Kind Kind, va_t Address,
                  llvm::StringRef Name) {
  auto E = HighExpr::makeCall("", 0, {});
  E->Type = NdType::makePtr(NdType::makeVoid());
  SourceCallTypeHint H;
  H.CallKind = Kind;
  H.TargetAddress = Address;
  H.TargetName = Name.str();
  H.Signature.ReturnType = E->Type;
  std::string Error;
  EXPECT_TRUE(assignDarwinScalarSourceABI(H.Signature, Arch::AArch64, Error));
  E->SourceCallHint = std::make_shared<SourceCallTypeHint>(H);
  return E;
}

ExprPtr expression(const SourceCallTypeHint &Hint) {
  std::vector<ExprPtr> Args{
      reference(SourceCallTypeHint::Kind::RuntimeClass, 0x4800, "NSSet"),
      reference(SourceCallTypeHint::Kind::RuntimeSelector, 0x4900,
                "setWithObjects:")};
  for (auto Address : Hint.NilTerminated->Objects)
    Args.push_back(HighExpr::makeConst(Address, 8,
                                       ConstantAddressProvenance::DataAddress));
  Args.push_back(HighExpr::makeConst(0, 8));
  auto E = HighExpr::makeCall("", 0x1100, std::move(Args));
  E->Type = Hint.Signature.ReturnType;
  E->SourceCallHint = std::make_shared<SourceCallTypeHint>(Hint);
  return E;
}

TEST(ObjCSentinelCalls, UsesTheRealVariadicABIThroughTheFirstNil) {
  Fixture F;
  for (unsigned Count : {0U, 1U, 10U, 24U}) {
    SCOPED_TRACE(Count);
    const auto Hints = buildObjCSourceCallHints(F.Image, caller(Count));
    ASSERT_TRUE(Hints.count(0x1500));
    const auto &Hint = Hints.at(0x1500);
    ASSERT_TRUE(Hint.NilTerminated);
    EXPECT_FALSE(Hint.Format);
    EXPECT_EQ(Hint.NilTerminated->Objects.size(), Count);
    ASSERT_EQ(Hint.Signature.Parameters.size(), Count + 3);
    for (unsigned I = 3; I < Count + 3; ++I) {
      EXPECT_EQ(Hint.Signature.Parameters[I].Location.Kind,
                SourceABICarrierKind::Stack);
      EXPECT_EQ(Hint.Signature.Parameters[I].Location.EntryStackOffset,
                (I - 3) * 8);
    }
    auto E = expression(Hint);
    E->CallTarget = "_objc_msgSend$setWithObjects:";
    EXPECT_TRUE(sdk::objcSourceCallBound(*E, F.Image, {}));
    HighFunc Function;
    Function.Name = "create_set";
    Function.ReturnType = E->Type;
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = E;
    Function.Body = {Return};
    std::string C;
    llvm::raw_string_ostream OS(C);
    HighCEmitter Emitter;
    CEmitterOptions Options;
    Options.TheArch = Arch::AArch64;
    EXPECT_TRUE(Emitter.emit({Function}, OS, Options));
    EXPECT_NE(C.find("id, SEL, void*, ..."), std::string::npos) << C;
  }
}

TEST(ObjCSentinelCalls,
     FirstNilNeedsNoStackAndStopsBeforeUnknownTrailingSlots) {
  Fixture F;
  auto FirstNil = caller(0);
  FirstNil.Blocks[0].Ops.erase(FirstNil.Blocks[0].Ops.begin());
  EXPECT_TRUE(buildObjCSourceCallHints(F.Image, FirstNil).count(0x1500));
  auto Early = caller(3);
  // The first variadic slot terminates the list. Later full-width stores do
  // not become arguments, even if their values have no object declaration.
  Early.Blocks[0].Ops[4].Inputs[1] = NdVar::cst(0, 8);
  Early.Blocks[0].Ops[6].Inputs[1] = NdVar::cst(0xdeadbeef, 8);
  const auto Hints = buildObjCSourceCallHints(F.Image, Early);
  ASSERT_TRUE(Hints.count(0x1500));
  EXPECT_EQ(Hints.at(0x1500).Signature.Parameters.size(), 4U);
  LowToMedConverter Converter;
  Converter.setBinaryImage(&F.Image);
  Converter.setSourceCallHintsEnabled(true);
  auto Med = Converter.convert(Early, Arch::AArch64, BinaryFormat::MachO);
  recoverCallAbi(Med, Arch::AArch64, {}, &F.Image);
  ASSERT_EQ(Med.CallInfos.size(), 1U);
  EXPECT_EQ(Med.CallInfos.front().Args.size(), 4U);
}

TEST(ObjCSentinelCalls, RejectsMissingPartialOrUnprovedArguments) {
  Fixture F;
  for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Low = caller(2);
    auto &Ops = Low.Blocks[0].Ops;
    if (Mutation == 0)
      Ops[2].Inputs[0] = NdVar::cst(0xdeadbeef, 8);
    if (Mutation == 1)
      Ops.erase(Ops.begin() + 4);
    if (Mutation == 2)
      Ops[4].Inputs[1].Size = 4;
    if (Mutation == 3)
      Ops[6].Inputs[1] = NdVar::cst(0x2020, 8);
    if (Mutation == 4)
      Ops[3].Inputs[1] = NdVar::cst(1, 8);
    if (Mutation == 5)
      Ops.erase(Ops.begin());
    if (Mutation == 6)
      Ops[6].Inputs[1] = NdVar::reg(a64reg::X9, 8);
    EXPECT_FALSE(buildObjCSourceCallHints(F.Image, Low).count(0x1500));
  }
}

TEST(ObjCSentinelCalls, RequiresAnExactPlatformClassAndUnmodifiedDeclaration) {
  Fixture F;
  for (const auto *Module : {Foundation, CoreFoundation}) {
    auto Image = F.Image;
    Image.DyldBindSlots.at(0x4800).Module = Module;
    EXPECT_TRUE(
        objcSentinelSourceCallHint(Image, "setWithObjects:", F.Receiver, {}));
  }
  for (unsigned Mutation = 0; Mutation < 15; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = F.Image;
    auto Receiver = F.Receiver;
    if (Mutation == 0)
      Image.Arch = Arch::X64;
    if (Mutation == 1)
      Image.IsRelocatable = true;
    if (Mutation == 2)
      Image.DyldBindSlots.at(0x4800).Module = "/tmp/CoreFoundation";
    if (Mutation == 3)
      Image.DyldBindSlots.at(0x4800).WeakImport = true;
    if (Mutation == 4)
      Image.DyldBindSlots.at(0x4800).Addend = 8;
    if (Mutation == 5)
      Image.DyldBindSlots.at(0x4800).Name = "_OBJC_CLASS_$_NSMutableSet";
    if (Mutation == 6)
      Image.DyldBindSlots.erase(0x4800);
    if (Mutation == 7)
      Image.ConflictingImportStorageSlots.insert(0x4800);
    if (Mutation == 8)
      Image.DynInfo.NeededLibs.erase(Image.DynInfo.NeededLibs.begin());
    if (Mutation == 9)
      Receiver.IsClassMethod = false;
    if (Mutation == 10)
      Receiver.Origin = ObjCReceiverTypeHint::OriginKind::MethodEntry;
    if (Mutation == 11)
      Receiver.Steps.emplace_back();
    if (Mutation == 12)
      Image.ObjCSourceReferences.at(0x4800).Size = 4;
    if (Mutation == 13) {
      ObjCClass Shadow;
      Shadow.Name = "NSSet";
      Image.ObjCClasses.push_back(Shadow);
    }
    if (Mutation == 14) {
      ObjCMethod Override;
      Override.ClassName = "NSSet";
      Override.Selector = "setWithObjects:";
      Override.IsClassMethod = true;
      Override.TypeEncoding = "@24@0:8@16";
      Override.TypeHint =
          parseObjCMethodEncoding(Override.Selector, Override.TypeEncoding);
      Image.ObjCMethods.push_back(Override);
    }
    EXPECT_FALSE(
        objcSentinelSourceCallHint(Image, "setWithObjects:", Receiver, {}));
  }
  EXPECT_FALSE(
      objcSentinelSourceCallHint(F.Image, "arrayWithObjects:", F.Receiver, {}));
}

TEST(ObjCSentinelCalls, RejectsASelectorStubBoundToAnotherRuntimeProvider) {
  for (bool Missing : {false, true}) {
    Fixture F;
    if (Missing)
      std::erase(F.Image.DynInfo.NeededLibs, "/usr/lib/libobjc.A.dylib");
    else
      F.Image.DyldBindSlots.at(0x4980).Module = "/tmp/libobjc.A.dylib";
    EXPECT_FALSE(objcSelectorStubSentinelSourceCallHint(F.Image, 0x1100,
                                                        F.Receiver, {}));
    EXPECT_FALSE(buildObjCSourceCallHints(F.Image, caller(0)).count(0x1500));
  }
}

TEST(ObjCSentinelCalls, RechecksActualArgumentsAndRejectsForeignPayloads) {
  Fixture F;
  const auto Hint = objcSelectorStubSentinelSourceCallHint(
      F.Image, 0x1100, F.Receiver, {0x2000, 0x2020});
  ASSERT_TRUE(Hint);
  for (unsigned Mutation = 0; Mutation < 18; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto E = expression(*Hint);
    auto Changed = std::make_shared<SourceCallTypeHint>(*Hint);
    E->SourceCallHint = Changed;
    if (Mutation == 0)
      E->Operands[0] = HighExpr::makeConst(0x4800, 8);
    if (Mutation == 1)
      E->Operands[1] = HighExpr::makeConst(0x4900, 8);
    if (Mutation == 2)
      E->Operands[2] = HighExpr::makeConst(0x2020, 8);
    if (Mutation == 3)
      E->Operands.back() = HighExpr::makeConst(0x2000, 8);
    if (Mutation == 4)
      E->Operands[2] = HighExpr::makeConst(0x2000, 4);
    if (Mutation == 5)
      Changed->NilTerminated->Objects.pop_back();
    if (Mutation == 6)
      Changed->CallKind = SourceCallTypeHint::Kind::Native;
    if (Mutation == 7)
      Changed->Format.emplace();
    if (Mutation == 8)
      Changed->DoesNotReturn = true;
    if (Mutation == 9)
      Changed->ReturnedArgument = 0;
    if (Mutation == 10)
      Changed->BorrowedByteInputs.emplace_back(2, 3);
    if (Mutation == 11)
      Changed->TargetAddress += 4;
    if (Mutation == 12)
      Changed->SelectorReferenceAddress += 8;
    if (Mutation == 13)
      Changed->Receiver->Address += 8;
    if (Mutation == 14)
      E->CallAddr += 4;
    if (Mutation >= 15) {
      auto Cast = std::make_shared<HighExpr>();
      Cast->Kind = Mutation == 15 ? ExprKind::Cast : ExprKind::BitCast;
      Cast->Type = NdType::makeInt(8, false);
      Cast->CastTo = NdType::makeInt(4, false);
      Cast->Operands = {E->Operands[2]};
      if (Mutation == 17) {
        Cast->CastTo.reset();
        Cast->Operands = {HighExpr::makeConst(0x2000, 4)};
      }
      E->Operands[2] = Cast;
    }
    EXPECT_FALSE(sdk::objcSourceCallBound(*E, F.Image, {}));
  }
  auto Foreign =
      reference(SourceCallTypeHint::Kind::RuntimeClass, 0x4800, "NSSet");
  auto Payload = std::make_shared<SourceCallTypeHint>(*Foreign->SourceCallHint);
  Payload->NilTerminated.emplace();
  Foreign->SourceCallHint = Payload;
  EXPECT_FALSE(sdk::objcSourceCallBound(*Foreign, F.Image, {}));
}

TEST(ObjCSentinelCalls, EveryIncomingPathMustDefineTheCompleteTerminator) {
  Fixture F;
  for (bool Missing : {false, true}) {
    auto Low = caller(2);
    auto Ops = Low.Blocks[0].Ops;
    Low.Blocks[0].Ops.assign(Ops.begin(), Ops.begin() + 5);
    Low.Blocks[0].Succs = {1, 2};
    LowBlock Left;
    Left.Id = 1;
    Left.StartAddr = 0x1300;
    Left.Preds = {0};
    Left.Succs = {3};
    Left.Ops = {Ops[5], Ops[6]};
    LowBlock Right = Left;
    Right.Id = 2;
    Right.StartAddr = 0x1400;
    if (Missing)
      Right.Ops.pop_back();
    LowBlock Join;
    Join.Id = 3;
    Join.StartAddr = 0x1500;
    Join.Preds = {1, 2};
    Join.Ops = {Ops[7], Ops[8]};
    const std::vector<LowBlock> Blocks{Low.Blocks[0], Left, Right, Join};
    std::array<unsigned, 4> Order{0, 1, 2, 3};
    do {
      Low.Blocks.clear();
      for (auto Index : Order)
        Low.Blocks.push_back(Blocks[Index]);
      EXPECT_EQ(buildObjCSourceCallHints(F.Image, Low).count(0x1500), !Missing);
    } while (std::next_permutation(Order.begin(), Order.end()));
  }
}

TEST(ObjCSentinelCalls, FinalAliasesRequireEveryDefinitionAndRejectStackReads) {
  Fixture F;
  const auto Hint = objcSelectorStubSentinelSourceCallHint(
      F.Image, 0x1100, F.Receiver, {0x2000});
  ASSERT_TRUE(Hint);
  for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Call = expression(*Hint);
    MedVar Variable;
    Variable.Kind = Mutation == 3 ? MedVar::Stack : MedVar::Temp;
    Variable.Id = 17;
    Variable.Size = 8;
    auto Alias = HighExpr::makeVar(Variable, NdType::makeInt(8, false));
    Call->Operands[2] = Alias;
    HighStmt Left;
    Left.Kind = StmtKind::Assign;
    Left.Dst = Alias;
    Left.Val = HighExpr::makeConst(0x2000, 8);
    HighStmt Right = Left;
    if (Mutation == 1)
      Right.Val = HighExpr::makeConst(0x2020, 8);
    if (Mutation == 2)
      Right.Val = Alias;
    HighFunc Function;
    Function.Body = {Left, Right};
    EXPECT_EQ(sdk::objcSourceCallBound(*Call, F.Image, {}, nullptr, nullptr,
                                       &Function),
              Mutation == 0);
  }
}

TEST(ObjCSentinelCalls, StackLoadsKeepTheValueAtTheirOwnReadTime) {
  Fixture F;
  const auto Hint = objcSelectorStubSentinelSourceCallHint(
      F.Image, 0x1100, F.Receiver, {0x2000, 0x2020});
  ASSERT_TRUE(Hint);
  for (unsigned Mutation = 0; Mutation < 11; ++Mutation) {
    SCOPED_TRACE(Mutation);
    HighFunc Function;
    Function.FrameSize = 32;
    MedVar Stack;
    Stack.Kind = MedVar::Reg;
    Stack.RegOff = a64reg::SP;
    Stack.Id = 100;
    Stack.Size = 8;
    const auto Word = NdType::makeInt(8, false);
    auto Frame = HighExpr::makeVar(Stack, Word);
    const auto Address = [&](unsigned Offset) {
      return HighExpr::makeBinop(NdOp::INT_SUB, Frame,
                                 HighExpr::makeConst(Offset, 8));
    };
    const auto Store = [&](unsigned Offset, uint64_t Value, unsigned Bytes) {
      HighStmt Statement;
      Statement.Kind = StmtKind::Store;
      Statement.StoreAddr = Address(Offset);
      Statement.StoreVal = HighExpr::makeConst(Value, Bytes);
      return Statement;
    };
    const auto Load = [&](unsigned Offset, unsigned Id) {
      HighStmt Statement;
      Statement.Kind = StmtKind::Assign;
      MedVar Value;
      Value.Kind = MedVar::Temp;
      Value.Id = Id;
      Value.Size = 8;
      Statement.Dst = HighExpr::makeVar(Value, Word);
      Statement.Val = HighExpr::makeLoad(Address(Offset), Word);
      return Statement;
    };
    auto Tail = Load(16, 80);
    auto Nil = Load(8, 81);
    auto Call = expression(*Hint);
    Call->Operands[3] = Tail.Dst;
    Call->Operands[4] = Nil.Dst;
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = Call;
    Function.Body = {Store(16, 0x2020, 8), Store(8, 0, 8), Tail, Nil,
                     Store(16, 0x2040, 8), Return};
    if (Mutation == 1)
      std::swap(Function.Body[2], Function.Body[4]);
    if (Mutation == 2)
      Function.Body.insert(Function.Body.begin() + 2, Store(12, 0, 4));
    if (Mutation == 3) {
      auto Unknown = Load(0, 82);
      Unknown.Val = HighExpr::makeCall("unknown", 0x1550, {});
      Unknown.Val->Type = Word;
      Function.Body.insert(Function.Body.begin() + 2, Unknown);
    }
    if (Mutation == 4) {
      auto Escape = Store(16, 0, 8);
      Escape.StoreAddr = HighExpr::makeConst(0x5000, 8);
      Escape.StoreVal = Address(16);
      Function.Body.insert(Function.Body.begin() + 2, Escape);
    }
    if (Mutation == 5)
      std::swap(Function.Body[0], Function.Body[2]);
    if (Mutation == 6) {
      HighStmt Branch;
      Branch.Kind = StmtKind::If;
      Branch.Cond = HighExpr::makeConst(1, 1);
      Function.Body.insert(Function.Body.begin() + 2, Branch);
    }
    if (Mutation == 7) {
      auto Reused = Load(16, 82);
      Reused.Val = Tail.Val;
      Function.Body.insert(Function.Body.end() - 1, Reused);
    }
    if (Mutation == 8) {
      auto Changed = std::make_shared<SourceCallTypeHint>(*Hint);
      Changed->NilTerminated->Objects[1] = 0x2040;
      Call->SourceCallHint = Changed;
    }
    if (Mutation == 9) {
      HighStmt Redefine;
      Redefine.Kind = StmtKind::Assign;
      Redefine.Dst = Frame;
      Redefine.Val = HighExpr::makeConst(0x5000, 8);
      Function.Body.insert(Function.Body.begin(), Redefine);
    }
    if (Mutation == 10) {
      auto Cast = std::make_shared<HighExpr>();
      Cast->Kind = ExprKind::Cast;
      Cast->Type = Word;
      Cast->CastTo = NdType::makeInt(4, false);
      Cast->Operands = {Function.Body[0].StoreVal};
      Function.Body[0].StoreVal = Cast;
    }
    EXPECT_EQ(sdk::objcSourceCallBound(*Call, F.Image, {}, nullptr, nullptr,
                                       &Function),
              Mutation == 0);
  }
}

TEST(ObjCSentinelCalls, EmittedVariadicCallsExecuteAgainstFoundation) {
#if defined(__APPLE__) && defined(__aarch64__) && defined(NEVERD_TEST_CLANG)
  Fixture F;
  std::vector<HighFunc> Functions;
  std::set<va_t> Strings;
  for (unsigned Count : {0U, 2U, 24U}) {
    std::vector<va_t> Objects;
    for (unsigned I = 0; I < Count; ++I)
      Objects.push_back(0x2000 + I * 32);
    const auto Hint = objcSelectorStubSentinelSourceCallHint(
        F.Image, 0x1100, F.Receiver, Objects);
    ASSERT_TRUE(Hint);
    HighFunc Function;
    Function.Name = "create_set_" + std::to_string(Count);
    Function.ReturnType = Hint->Signature.ReturnType;
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = expression(*Hint);
    Function.Body = {Return};
    auto Bound = sdk::bindObjCSourceReferences(Function, F.Image);
    EXPECT_EQ(Bound.ConstantStrings.size(), Count);
    Strings.insert(Bound.ConstantStrings.begin(), Bound.ConstantStrings.end());
    ASSERT_TRUE(
        sdk::objcSourceCallBound(*Bound.Function.Body[0].RetVal, F.Image, {}));
    Functions.push_back(std::move(Bound.Function));
  }
  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  ASSERT_TRUE(HighCEmitter().emit(Functions, OS, Options));
  std::set<std::string> Shared;
  C += sdk::renderObjCConstantStringHelpers(F.Image, Strings, Shared);
  C += R"(
int main(void) {
  SEL count = sel_registerName("count");
  return ((uintptr_t (*)(id, SEL))objc_msgSend)((id)create_set_0(), count) != 0 ||
         ((uintptr_t (*)(id, SEL))objc_msgSend)((id)create_set_2(), count) != 2 ||
         ((uintptr_t (*)(id, SEL))objc_msgSend)((id)create_set_24(), count) != 24;
}
)";
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-sentinel", "c", SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-sentinel", "exe", BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-sentinel", "err", ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code Error;
  {
    llvm::raw_fd_ostream Out(SourcePath, Error);
    ASSERT_FALSE(Error);
    Out << C;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  const std::string Compiler = NEVERD_TEST_CLANG;
  for (const auto *Optimization : {"-O0", "-O2"}) {
    llvm::SmallVector<llvm::StringRef> Args{
        Compiler,     "-x",         "c",          "-std=gnu11",
        "-arch",      "arm64",      Optimization, "-Werror",
        "-framework", "Foundation", "-framework", "CoreFoundation",
        SourcePath,   "-o",         BinaryPath};
    std::string Message;
    const auto Compiled = llvm::sys::ExecuteAndWait(
        Compiler, Args, std::nullopt, Redirects, 30, 0, &Message);
    const auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Compiled, 0) << Message
                           << (Errors ? (*Errors)->getBuffer().str() : "") << C;
    EXPECT_EQ(llvm::sys::ExecuteAndWait(BinaryPath, {BinaryPath}, std::nullopt,
                                        Redirects, 30, 0, &Message),
              0)
        << Message << C;
  }
#else
  GTEST_SKIP()
      << "Foundation runtime verification requires macOS arm64 and clang";
#endif
}

TEST(ObjCSentinelCalls, BoundsTheObjectListAndKeepsMarkerExclusiveToMessages) {
  Fixture F;
  std::vector<va_t> Objects(61, 0x2000);
  const auto Maximum = objcSentinelSourceCallHint(
      F.Image, "setWithObjects:", F.Receiver, Objects);
  ASSERT_TRUE(Maximum);
  EXPECT_EQ(Maximum->Signature.Parameters.size(), 64U);
  Objects.push_back(0x2000);
  EXPECT_FALSE(objcSentinelSourceCallHint(
      F.Image, "setWithObjects:", F.Receiver, Objects));
  auto Foreign =
      reference(SourceCallTypeHint::Kind::RuntimeClass, 0x4800, "NSSet");
  auto Hint = std::make_shared<SourceCallTypeHint>(*Foreign->SourceCallHint);
  Hint->NilTerminated.emplace();
  Foreign->SourceCallHint = Hint;
  HighFunc Function;
  Function.Name = "invalid_marker";
  Function.ReturnType = Foreign->Type;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = Foreign;
  Function.Body = {Return};
  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  ASSERT_TRUE(HighCEmitter().emit({Function}, OS, Options));
  EXPECT_NE(
      C.find("bad source call: invalid nil-terminated source declaration"),
      std::string::npos)
      << C;
}

TEST(ObjCSentinelCalls, EscapesAndOverlappingWritesRevokeTailEvidence) {
  Fixture F;
  for (bool Escape : {false, true}) {
    auto Low = caller(2);
    auto &Ops = Low.Blocks[0].Ops;
    std::vector<LowOp> Change;
    if (Escape) {
      Change.push_back(op(NdOp::STORE, {},
                          {NdVar::cst(0x5000, 8), NdVar::reg(a64reg::SP, 8)}));
    } else {
      Change.push_back(op(NdOp::INT_ADD, NdVar::reg(a64reg::X8, 8),
                          {NdVar::reg(a64reg::SP, 8), NdVar::cst(4, 8)}));
      Change.push_back(
          op(NdOp::STORE, {}, {NdVar::reg(a64reg::X8, 8), NdVar::cst(0, 4)}));
    }
    for (size_t I = 0; I < Change.size(); ++I)
      Change[I].Addr = 0x1400 + 4 * I;
    Ops.insert(Ops.end() - 2, Change.begin(), Change.end());
    EXPECT_FALSE(buildObjCSourceCallHints(F.Image, Low).count(0x1500));
  }
}
} // namespace
