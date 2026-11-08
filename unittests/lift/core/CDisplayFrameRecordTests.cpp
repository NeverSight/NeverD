//===- CDisplayFrameRecordTests.cpp - Source pointer types ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Source pointer and C record display regression cases.
///
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/HighC/HighCWriter.h"
#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

#include <limits>

namespace {
using namespace neverd;

class RecordContext : public NullDebugContext {
public:
  TypeRef Record;
  explicit RecordContext(bool MissingFieldTypes = false,
                         std::string Name = "DISPLAY_RECORD",
                         uint16_t Size = 16, uint16_t Alignment = 8)
      : Record(NdType::makeNamedRecord(std::move(Name), Size)) {
    Record->FieldDisplayNames = {"first", "second"};
    Record->FieldDisplayOffsets = {0, 8};
    Record->FieldDisplayTypes = {NdType::makeInt(8, false),
                                 NdType::makeInt(8, false)};
    Record->FieldDisplayAlignment = Alignment;
    if (MissingFieldTypes)
      Record->FieldDisplayTypes = {nullptr, nullptr};
  }
  std::optional<FunctionSym>
  resolvePrototype(llvm::StringRef Name) const override {
    if (Name != "borrow_record" && Name != "opaque_frame")
      return std::nullopt;
    FunctionSym F;
    F.Name = Name.str();
    F.ReturnType = NdType::makeVoid();
    F.Params = {{"value", Name == "borrow_record" ? NdType::makePtr(Record)
                                                  : NdType::makeInt(8, false)}};
    F.PrototypeFromHeader = true;
    F.PrototypeLibrary = "frame-record-fixture";
    F.PlatformConvention = true;
    return F;
  }
  std::optional<FunctionSym> resolveFunction(va_t Address) const override {
    const char *Name = Address == 0x2000   ? "opaque_frame"
                       : Address == 0x3000 ? "borrow_record"
                                           : nullptr;
    if (!Name)
      return std::nullopt;
    auto F = resolvePrototype(Name);
    F->Addr = Address;
    return F;
  }
  bool hasInfo() const override { return true; }
};

void executeView(llvm::StringRef Text) {
  const auto Compiler = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(Compiler) << "clang is required for C layout execution";
  llvm::SmallString<128> Source, Program, Errors;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-frame-record", "c", Source));
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-frame-record", "exe",
                                                  Program));
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-frame-record", "err", Errors));
  llvm::FileRemover RemoveSource(Source), RemoveProgram(Program),
      RemoveErrors(Errors);
  std::error_code EC;
  llvm::raw_fd_ostream Out(Source, EC);
  ASSERT_FALSE(EC);
  Out << "#include <stdint.h>\n"
         "typedef struct DISPLAY_RECORD { uint64_t first,second; } "
         "DISPLAY_RECORD;\n"
         "void opaque_frame(uint64_t); void borrow_record(DISPLAY_RECORD *);\n"
      << Text << R"(
static uintptr_t base_seen;
static int layout_ok;
void opaque_frame(uint64_t base) { base_seen=(uintptr_t)base; }
void borrow_record(DISPLAY_RECORD *p) {
  layout_ok=((uintptr_t)p+16==base_seen && (uintptr_t)p%8==0);
  p->first=7; p->second=11;
}
int main(void) { record_view(); return !layout_ok; }
)";
  Out.close();
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, Errors.str()};
  for (const char *Optimization : {"-O0", "-O2"}) {
    llvm::SmallVector<llvm::StringRef, 12> Args{*Compiler,
                                                "-std=c11",
                                                Optimization,
                                                "-fno-strict-aliasing",
                                                "-fsanitize=undefined",
                                                "-fsanitize-trap=undefined",
                                                Source,
                                                "-o",
                                                Program};
    auto Result =
        llvm::sys::ExecuteAndWait(*Compiler, Args, std::nullopt, Redirects, 30);
    auto Diagnostic = llvm::MemoryBuffer::getFile(Errors);
    ASSERT_EQ(Result, 0) << (Diagnostic ? (*Diagnostic)->getBuffer().str() : "")
                         << Text.str();
    EXPECT_EQ(llvm::sys::ExecuteAndWait(Program, {Program}, std::nullopt,
                                        Redirects, 10),
              0)
        << Text.str();
  }
}

/// The C either writer gives a function that passes the frame address
/// \p Distance bytes below its entry stack pointer to borrow_record. A
/// nonzero \p FrameAlignment aligns the LLVM frame as MedLLVMFuncBody does.
std::string renderView(bool LLVM, RecordContext &Debug, uint64_t Distance,
                       uint64_t FrameAlignment = 0) {
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::COFF;
  Options.EmitIncludes = false;
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  if (LLVM) {
    llvm::LLVMContext C;
    llvm::Module M("record-view", C);
    M.setDataLayout("e-p:64:64");
    llvm::IRBuilder<> B(C);
    auto *F = llvm::Function::Create(
        llvm::FunctionType::get(B.getInt64Ty(), false),
        llvm::GlobalValue::ExternalLinkage, "record_view", M);
    rewrite_source::setOriginalVA(*F, 0x1000);
    B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", F));
    auto *Frame = B.CreateAlloca(llvm::ArrayType::get(B.getInt8Ty(), 152),
                                 nullptr, "frame");
    if (FrameAlignment)
      Frame->setAlignment(llvm::Align(FrameAlignment));
    auto *Base = B.CreatePtrToInt(
        B.CreateGEP(B.getInt8Ty(), Frame, B.getInt64(72), "frame_end"),
        B.getInt64Ty(), "rsp_init");
    auto Opaque =
        M.getOrInsertFunction("opaque_frame", B.getVoidTy(), B.getInt64Ty());
    auto Borrow =
        M.getOrInsertFunction("borrow_record", B.getVoidTy(), B.getPtrTy());
    rewrite_source::setOriginalVA(
        *llvm::cast<llvm::Function>(Opaque.getCallee()), 0x2000);
    rewrite_source::setOriginalVA(
        *llvm::cast<llvm::Function>(Borrow.getCallee()), 0x3000);
    B.CreateCall(Opaque, {Base});
    B.CreateCall(Borrow,
                 {B.CreateIntToPtr(B.CreateSub(Base, B.getInt64(Distance)),
                                   B.getPtrTy())});
    B.CreateRet(B.getInt64(0));
    EXPECT_TRUE(LLVMCEmitter().emit(M, OS, Options, &Debug, nullptr, F));
  } else {
    HighFunc F;
    F.Name = "record_view";
    F.Entry = 0x1000;
    F.FrameSize = 64;
    F.ReturnType = NdType::makeInt(8, false);
    MedVar SP;
    SP.Kind = MedVar::Reg;
    SP.RegOff = getTargetRegInfo(Arch::X64).StackPointer;
    SP.Size = 8;
    SP.TheArch = Arch::X64;
    auto Base = HighExpr::makeVar(SP, NdType::makeInt(8, false));
    HighStmt Opaque, Borrow, Return;
    Opaque.Kind = Borrow.Kind = StmtKind::Call;
    Opaque.CallExpr = HighExpr::makeCall("opaque_frame", 0x2000, {Base});
    Borrow.CallExpr = HighExpr::makeCall(
        "borrow_record", 0x3000,
        {HighExpr::makeBinop(NdOp::INT_SUB, Base,
                             HighExpr::makeConst(Distance, 8))});
    Return.Kind = StmtKind::Return;
    Return.RetVal = HighExpr::makeConst(0, 8);
    F.Body = {Opaque, Borrow, Return};
    EXPECT_TRUE(HighCEmitter().emit({F}, OS, Options, &Debug));
  }
  OS.flush();
  return Text;
}

TEST(CDisplayFrameRecord, BothWritersKeepRecordAddressAndFrameOffsets) {
  for (unsigned Variant = 0; Variant != 4; ++Variant) {
    const bool LLVM = Variant & 1;
    SCOPED_TRACE(LLVM);
    SCOPED_TRACE(Variant);
    RecordContext Debug(Variant & 2);
    EXPECT_TRUE(sourceAggregateMembers(Debug.Record).empty());
    EXPECT_EQ(Debug.Record->Alignment, 0u);
    const std::string Text = renderView(LLVM, Debug, 16);
    EXPECT_NE(Text.find("DISPLAY_RECORD var_"), std::string::npos) << Text;
    executeView(Text);
  }
}

TEST(CDisplayFrameRecord, OveralignedRecordKeepsFrameBytes) {
  // The frame storage is declared with the 16-byte synthetic stack alignment;
  // C rejects that declaration around a member aligned to 32. The record's
  // aligned bytes stay part of the frame's own storage in both writers.
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    RecordContext Debug(false, "ALIGNED_RECORD", 32, 32);
    const std::string Text = renderView(LLVM, Debug, 40, 16);
    EXPECT_EQ(Text.find("ALIGNED_RECORD var_"), std::string::npos) << Text;
    source_call_execution_test::checkSyntax(
        "#include <stdint.h>\n"
        "typedef struct ALIGNED_RECORD { _Alignas(32) uint64_t first; "
        "uint64_t second, third, fourth; } ALIGNED_RECORD;\n"
        "void opaque_frame(uint64_t); void borrow_record(ALIGNED_RECORD *);\n" +
        Text);
  }
}
} // namespace

TEST(CDisplayFrameRecord, CopyTypesKeepDuplicateOwnersAndObserveNewTypes) {
  for (bool DuplicateDestination : {false, true}) {
    SCOPED_TRACE(DuplicateDestination);
    HighFunc F;
    F.Entry = 0x1000;
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    std::string Text;
    llvm::raw_string_ostream Out(Text);
    HighCWriter Writer(Out, Options, nullptr);
    Writer.CurrentFunc = &F;
    auto Value = [](int Id) {
      MedVar V;
      V.Kind = MedVar::Temp;
      V.Id = Id;
      V.Size = 8;
      V.TheArch = Arch::X64;
      return HighExpr::makeVar(V, NdType::makeInt(8, false));
    };
    auto Source = Value(1), Bridge = Value(2), Destination = Value(3);
    const auto Record = NdType::makeNamedRecord("COPIED_RECORD", 8);
    auto Slot = [&](int64_t Offset, const ExprPtr &V, TypeRef Type) {
      Writer.FrameSlots[Offset].Name = Writer.varName(V->Var);
      Writer.FrameSlots[Offset].Type = std::move(Type);
    };
    Slot(-64, Source, NdType::makeInt(8, false));
    Slot(-56, Source, Record);
    Slot(-48, Bridge, NdType::makeInt(8, false));
    Slot(-40, Destination, NdType::makeInt(8, false));
    if (DuplicateDestination)
      Slot(-32, Destination, NdType::makeInt(8, false));
    HighStmt First, Second;
    First.Kind = Second.Kind = StmtKind::Assign;
    First.Dst = Bridge;
    First.Val = Source;
    Second.Dst = Destination;
    Second.Val = Bridge;
    F.Body = {First, Second};
    Writer.propagateFrameSlotCopyTypes(F);
    ASSERT_TRUE(Writer.FrameSlots.at(-48).Type);
    EXPECT_EQ(Writer.FrameSlots.at(-48).Type->SourceName, "COPIED_RECORD");
    EXPECT_EQ(Writer.FrameSlots.at(-40).Type->Kind,
              DuplicateDestination ? NdTypeKind::Int : NdTypeKind::Struct);
    if (DuplicateDestination)
      EXPECT_EQ(Writer.FrameSlots.at(-32).Type->Kind, NdTypeKind::Int);
  }
}

TEST(CDisplayFrameRecord, ProjectedReadsUseCurrentSlotsAndFirstNameOwner) {
  HighFunc F;
  F.Entry = 0x1000;
  F.FrameSize = 64;
  MedVar Value;
  Value.Kind = MedVar::Temp;
  Value.Id = 1;
  Value.Size = 8;
  Value.TheArch = Arch::X64;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeVar(Value, NdType::makeInt(8, false));
  F.Body = {Return};
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::COFF;
  std::string Text;
  llvm::raw_string_ostream Out(Text);
  HighCWriter Writer(Out, Options, nullptr);
  Writer.CurrentFunc = &F;
  Writer.ProjectFrameAliasesIntoStorage = true;
  const std::string Name = Writer.varName(Value);
  std::map<int64_t, HighCWriter::NamedFrameSlot> Slots;
  Slots[-32].Name = Name;
  Slots[-16].Name = Name;
  Slots[-16].Type = NdType::makeInt(4, false);
  Writer.setFrameStorageSlots(Slots);
  Writer.collectFrameRecordMembers(F, 64, 64);
  ASSERT_EQ(Writer.FrameRecordMembers.size(), 1u);
  ASSERT_TRUE(Writer.FrameRecordMembers.count(-32));
  EXPECT_EQ(Writer.FrameRecordMembers.at(-32).Size, 8u);

  // A new collection owns a new layout, even when the variable keeps its name.
  Slots.clear();
  Slots[-8].Name = Name;
  Slots[-8].Type = NdType::makeInt(4, false);
  Writer.setFrameStorageSlots(Slots);
  Writer.collectFrameRecordMembers(F, 64, 64);
  ASSERT_EQ(Writer.FrameRecordMembers.size(), 1u);
  ASSERT_TRUE(Writer.FrameRecordMembers.count(-8));
  EXPECT_EQ(Writer.FrameRecordMembers.at(-8).Size, 4u);

  Writer.ProjectFrameAliasesIntoStorage = false;
  Writer.collectFrameRecordMembers(F, 64, 64);
  EXPECT_TRUE(Writer.FrameRecordMembers.empty());
}

namespace {
struct FrameDisplacementCase {
  HighFunc Function;
  CEmitterOptions Options;
  llvm::raw_null_ostream Out;
  HighCWriter Writer{Out, Options, nullptr};
  ExprPtr Stack;

  explicit FrameDisplacementCase(Arch TheArch = Arch::X64)
      : Options{.TheArch = TheArch,
                .Format =
                    isMIPS(TheArch) ? BinaryFormat::ELF : BinaryFormat::COFF} {
    Function.FrameSize = 64;
    Writer.CurrentFunc = &Function;
    MedVar V;
    V.Kind = MedVar::Reg;
    V.RegOff = getTargetRegInfo(TheArch).StackPointer;
    V.Size = getTargetRegInfo(TheArch).PointerSize;
    V.TheArch = TheArch;
    Stack = HighExpr::makeVar(V, NdType::makeInt(V.Size, false));
  }

  ExprPtr step(NdOp Op, ExprPtr Base, uint64_t Bits) const {
    auto E = HighExpr::makeBinop(Op, std::move(Base),
                                 HighExpr::makeConst(Bits, Stack->Var.Size));
    E->Type = Stack->Type;
    return E;
  }
};
} // namespace

TEST(CDisplayFrameRecord, ZeroMaskIsNotAStackAlignment) {
  for (Arch A : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64, Arch::MIPS32,
                 Arch::MIPS32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    SCOPED_TRACE(static_cast<unsigned>(A));
    FrameDisplacementCase C(A);
    const auto Zero = C.step(NdOp::INT_AND, C.Stack, 0);
    EXPECT_FALSE(C.Writer.frameDisplacement(*Zero));
    EXPECT_FALSE(C.Writer.certifiedFrameStorageDisplacement(*Zero));
    EXPECT_EQ(C.Writer.FrameDisplacementDepth, 0u);
    EXPECT_EQ(C.Writer.certifiedFrameStorageDisplacement(*C.Stack), 0);
  }
}

TEST(CDisplayFrameRecord, BothWritersExecuteZeroMaskedFrameAddresses) {
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    FrameDisplacementCase C;
    C.Options.EmitIncludes = false;
    std::string Text;
    llvm::raw_string_ostream Out(Text);
    if (LLVM) {
      llvm::LLVMContext Context;
      llvm::Module Module("zero-mask", Context);
      Module.setDataLayout("e-p:64:64");
      llvm::IRBuilder<> B(Context);
      auto *Function = llvm::Function::Create(
          llvm::FunctionType::get(B.getInt64Ty(), false),
          llvm::GlobalValue::ExternalLinkage, "zero_mask", Module);
      auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
      B.SetInsertPoint(Entry);
      auto *Frame = B.CreateAlloca(B.getInt64Ty());
      B.CreateStore(B.getInt64(7), Frame);
      auto *Address = B.CreatePtrToInt(Frame, B.getInt64Ty());
      // Keep the instruction for the writer instead of IRBuilder folding it.
      auto *Masked =
          llvm::BinaryOperator::CreateAnd(Address, B.getInt64(0), "", Entry);
      B.CreateRet(Masked);
      ASSERT_TRUE(LLVMCEmitter().emit(Module, Out, C.Options, nullptr, nullptr,
                                      Function));
    } else {
      C.Function.Name = "zero_mask";
      C.Function.ReturnType = C.Stack->Type;
      HighStmt Store, Return;
      Store.Kind = StmtKind::Store;
      Store.StoreAddr = C.step(NdOp::INT_SUB, C.Stack, 16);
      Store.StoreVal = HighExpr::makeConst(7, 8);
      Return.Kind = StmtKind::Return;
      Return.RetVal = C.step(NdOp::INT_AND, C.Stack, 0);
      C.Function.Body = {Store, Return};
      ASSERT_TRUE(HighCEmitter().emit({C.Function}, Out, C.Options));
    }
    Out.flush();
    for (llvm::StringRef Optimization : {"-O0", "-O2"})
      source_call_execution_test::compileAndRun(
          "#include <stdint.h>\n" + Text +
              "\nint main(void) { return zero_mask() != 0; }\n",
          {Optimization, "-fsanitize=undefined", "-fsanitize-trap=undefined"});
  }
}

TEST(CDisplayFrameRecord, StorageOffsetsRequireCurrentKnownRoots) {
  for (Arch A : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64, Arch::MIPS32,
                 Arch::MIPS32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    SCOPED_TRACE(static_cast<unsigned>(A));
    FrameDisplacementCase C(A);
    const auto Address =
        C.step(NdOp::INT_ADD, C.step(NdOp::INT_SUB, C.Stack, 16), 4);
    EXPECT_EQ(C.Writer.certifiedFrameStorageDisplacement(*Address), -12);
    MedVar Unknown;
    Unknown.Kind = MedVar::Temp;
    Unknown.Id = 17;
    Unknown.Size = C.Stack->Var.Size;
    Unknown.TheArch = A;
    const auto Root = HighExpr::makeVar(Unknown, C.Stack->Type);
    const auto Unproved = C.step(NdOp::INT_SUB, Root, 12);
    // Display heuristics cannot redirect a real address into frame storage.
    EXPECT_EQ(C.Writer.frameDisplacement(*Unproved), -12);
    EXPECT_FALSE(C.Writer.certifiedFrameStorageDisplacement(*Unproved));
    auto &Alias = C.Writer.FrameAliases[C.Writer.varName(Unknown)];
    Alias = -16;
    EXPECT_EQ(C.Writer.certifiedFrameStorageDisplacement(*Unproved), -28);
    Alias = -32;
    EXPECT_EQ(C.Writer.certifiedFrameStorageDisplacement(*Unproved), -44);
    C.Writer.FrameAliases.clear();
    EXPECT_FALSE(C.Writer.certifiedFrameStorageDisplacement(*Unproved));
    EXPECT_FALSE(C.Writer.frameDisplacement(
        *C.step(NdOp::INT_AND, Unproved, uint64_t(-16))));
  }
}

TEST(CDisplayFrameRecord, FrameAlignmentKeepsTheEntryResidue) {
  for (Arch A : {Arch::X64, Arch::AArch64}) {
    FrameDisplacementCase C(A);
    const auto Address = C.step(
        NdOp::INT_AND, C.step(NdOp::INT_SUB, C.Stack, 20), uint64_t(-16));
    // Win64 CALL leaves an eight-byte return cell; AArch64 leaves SP aligned.
    EXPECT_EQ(C.Writer.certifiedFrameStorageDisplacement(*Address),
              A == Arch::X64 ? -24 : -32);
    const auto Invalid = C.step(NdOp::INT_AND, C.Stack, uint64_t(-3));
    EXPECT_FALSE(C.Writer.certifiedFrameStorageDisplacement(*Invalid));
  }
}

TEST(CDisplayFrameRecord, UnrepresentableOffsetsCannotWrapIntoFrameSlots) {
  FrameDisplacementCase C;
  constexpr int64_t Low = std::numeric_limits<int64_t>::min();
  constexpr int64_t High = std::numeric_limits<int64_t>::max();
  const ExprPtr Invalid[] = {
      C.step(NdOp::INT_ADD, C.step(NdOp::INT_ADD, C.Stack, High), 1),
      C.step(NdOp::INT_SUB, C.step(NdOp::INT_ADD, C.Stack, Low), 1),
      C.step(NdOp::INT_SUB, C.Stack, Low)};
  for (const auto &E : Invalid) {
    EXPECT_FALSE(C.Writer.frameDisplacement(*E));
    EXPECT_FALSE(C.Writer.certifiedFrameStorageDisplacement(*E));
  }
  // Subtraction must not first negate INT64_MIN: the combined difference fits.
  const auto Difference =
      C.step(NdOp::INT_SUB, C.step(NdOp::INT_SUB, C.Stack, Low), 1);
  EXPECT_EQ(C.Writer.certifiedFrameStorageDisplacement(*Difference), High);
  const auto Sum =
      C.step(NdOp::INT_ADD, C.step(NdOp::INT_ADD, C.Stack, High), Low);
  EXPECT_EQ(C.Writer.certifiedFrameStorageDisplacement(*Sum), -1);
}

TEST(CDisplayFrameRecord, AliasAndParentFrameRebasesCannotOverflow) {
  FrameDisplacementCase C;
  constexpr int64_t Low = std::numeric_limits<int64_t>::min();
  constexpr int64_t High = std::numeric_limits<int64_t>::max();
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = 17;
  V.Size = 8;
  V.TheArch = Arch::X64;
  const auto Alias = HighExpr::makeVar(V, C.Stack->Type);
  C.Writer.FrameAliases[C.Writer.varName(V)] = High;
  EXPECT_FALSE(C.Writer.certifiedFrameStorageDisplacement(
      *C.step(NdOp::INT_ADD, Alias, 1)));
  V.Kind = MedVar::Param;
  V.Id = 1;
  const auto Parent = HighExpr::makeVar(V, C.Stack->Type);
  EXPECT_EQ(C.Writer.certifiedFrameStorageDisplacement(*Parent), -64);
  EXPECT_FALSE(C.Writer.certifiedFrameStorageDisplacement(
      *C.step(NdOp::INT_ADD, Parent, Low)));
  EXPECT_EQ(C.Writer.certifiedFrameStorageDisplacement(
                *C.step(NdOp::INT_ADD, Parent, Low + 64)),
            Low);
}

TEST(CDisplayFrameRecord,
     DisplayFrameNeighborsDoNotOverflowOrAuthorizeStorage) {
  FrameDisplacementCase C;
  C.Function.ExceptionMetadata.emplace();
  MedVar FP = C.Stack->Var;
  FP.RegOff = getTargetRegInfo(Arch::X64).FramePointer;
  const auto Root = HighExpr::makeVar(FP, C.Stack->Type);
  for (int64_t Offset : {std::numeric_limits<int64_t>::min(),
                         std::numeric_limits<int64_t>::max()}) {
    C.Writer.FrameSlots.clear();
    C.Writer.FrameSlots[Offset].Name = "edge";
    const auto Address = C.step(NdOp::INT_ADD, Root, Offset);
    EXPECT_EQ(C.Writer.frameDisplacement(*Address), Offset);
    EXPECT_FALSE(C.Writer.certifiedFrameStorageDisplacement(*Address));
  }
}

TEST(CDisplayFrameRecord, OffsetWalkStopsAtTheExistingBound) {
  FrameDisplacementCase C;
  auto Address = C.Stack;
  for (unsigned I = 0; I != limits::kMaxFrameDisplacementDepth; ++I)
    Address = C.step(NdOp::INT_ADD, Address, 1);
  EXPECT_FALSE(C.Writer.frameDisplacement(*Address));
  EXPECT_FALSE(C.Writer.certifiedFrameStorageDisplacement(*Address));
  EXPECT_EQ(C.Writer.FrameDisplacementDepth, 0u);
  EXPECT_EQ(C.Writer.certifiedFrameStorageDisplacement(*C.Stack), 0);
}

TEST(CDisplayFrameRecord, TypedOffsetsAndOverlaysCannotWrapIntoMembers) {
  FrameDisplacementCase C;
  constexpr int64_t Low = std::numeric_limits<int64_t>::min();
  constexpr int64_t High = std::numeric_limits<int64_t>::max();
  for (const auto &E :
       {C.step(NdOp::INT_ADD, C.step(NdOp::INT_ADD, C.Stack, High), 1),
        C.step(NdOp::INT_SUB, C.step(NdOp::INT_ADD, C.Stack, Low), 1),
        C.step(NdOp::INT_SUB, C.Stack, Low)})
    EXPECT_FALSE(C.Writer.typedPointerOffset(*E));
  const auto Difference =
      C.step(NdOp::INT_SUB, C.step(NdOp::INT_SUB, C.Stack, Low), 1);
  const auto Offset = C.Writer.typedPointerOffset(*Difference);
  ASSERT_TRUE(Offset);
  EXPECT_EQ(Offset->first, C.Stack.get());
  EXPECT_EQ(Offset->second, static_cast<uint64_t>(High));
  const auto Last = C.step(NdOp::INT_ADD, C.Stack, High);
  EXPECT_FALSE(C.Writer.frameOverlayDisplacement(*Last, 1));
  EXPECT_EQ(C.Writer.frameOverlayDisplacement(*Last, 0), High);
  const auto First = C.step(NdOp::INT_ADD, C.Stack, Low);
  EXPECT_EQ(C.Writer.frameOverlayDisplacement(*First, UINT64_MAX), High);
}

TEST(CDisplayFrameRecord, DistantFrameSlotsCannotBecomeTypedMembers) {
  FrameDisplacementCase C;
  RecordContext Debug;
  constexpr int64_t Low = std::numeric_limits<int64_t>::min();
  auto &Slot = C.Writer.FrameSlots[Low];
  Slot.Name = "edge";
  Slot.Type = Debug.Record;
  EXPECT_TRUE(C.Writer.frameTypedMemberType(Low + 8, 8));
  EXPECT_FALSE(C.Writer.frameTypedMemberType(0, 8));
  EXPECT_FALSE(C.Writer.frameTypedMemberAccess(0, 8));
  EXPECT_FALSE(
      C.Writer.frameTypedMemberType(std::numeric_limits<int64_t>::max(), 8));
}

TEST(CDisplayFrameRecord, AlignmentChecksUseOffsetResiduesWithoutOverflow) {
  for (Arch A : {Arch::X64, Arch::AArch64}) {
    FrameDisplacementCase C(A);
    C.Writer.FrameStorageActive = true;
    C.Writer.ProjectFrameAliasesIntoStorage = true;
    const uint64_t Residue = A == Arch::X64 ? 8 : 0;
    for (const int64_t Offset :
         {std::numeric_limits<int64_t>::max(),
          std::numeric_limits<int64_t>::max() - 7,
          std::numeric_limits<int64_t>::min(), int64_t(-8), int64_t(-9)}) {
      for (const uint64_t Alignment : {1, 2, 4, 8, 16}) {
        HighExpr Check;
        Check.Kind = ExprKind::Call;
        Check.IntrinsicId = Intrinsic::RequireAligned;
        Check.Operands = {C.step(NdOp::INT_ADD, C.Stack, Offset),
                          HighExpr::makeConst(Alignment, 8)};
        EXPECT_EQ(
            C.Writer.frameStorageAlignmentHolds(Check),
            ((static_cast<uint64_t>(Offset) + Residue) & (Alignment - 1)) == 0);
      }
    }
  }
}
