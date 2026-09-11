#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Swift/SwiftRuntimeSource.h"

#include "llvm/Support/Endian.h"

using namespace neverd;
namespace {
using Kind = SwiftRuntimeSourceKind;
struct Fixture {
  BinaryImage Image;
  std::vector<LowFunc> Functions;
  SwiftRuntimeSourceRequest Request;
  std::string Prefix;
  uint64_t Self, SP, Return, Second, Arg;
  explicit Fixture(bool Class, Arch Architecture = Arch::AArch64) {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Architecture;
    Image.Bits = Bitness::Bits64;
    Segment Data;
    Data.VA = 0x1000;
    Data.Size = Data.FileSz = 0x1000;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(0x1000);
    Image.Segments.push_back(Data);
    Segment Code;
    Code.VA = 0x3000;
    Code.Size = Code.FileSz = 0x1000;
    Code.FileOff = 0x1000;
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Code.Data.resize(0x1000);
    Image.Segments.push_back(Code);
    Section Types;
    Types.Name = "__swift5_types";
    Types.VA = 0x1000;
    Types.Size = Types.FileSz = 4;
    Types.Flags = SegmentFlags::Readable;
    Image.Sections.push_back(Types);
    Section Storage;
    Storage.Name = "__data";
    Storage.VA = 0x1004;
    Storage.Size = Storage.FileSz = 0xffc;
    Storage.FileOff = 4;
    Storage.Flags = Data.Flags;
    Image.Sections.push_back(Storage);
    Section Text;
    Text.Name = "__text";
    Text.VA = 0x3000;
    Text.Size = Text.FileSz = 0x1000;
    Text.FileOff = 0x1000;
    Text.Flags = Code.Flags;
    Text.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    Image.Sections.push_back(Text);
    relative(0x1000, 0x1100);
    u32(0x1100, Class ? 0x50 : 0x51);
    relative(0x1104, 0x1200);
    relative(0x1108, 0x1180);
    relative(0x1110, 0x1300);
    text(0x1180, Class ? "Box" : "Cell");
    u32(0x1200, 0);
    relative(0x1208, 0x1210);
    text(0x1210, "Demo");
    u32(0x1308, (12u << 16) | (Class ? 1 : 0));
    u32(0x130c, 1);
    u32(0x1310, Class ? 0 : 2);
    relative(0x1314, 0x1340);
    relative(0x1318, 0x1350);
    text(0x1340, "s5Int64V");
    text(0x1350, "value");
    if (Class) {
      u32(0x1124, 1);
      u32(0x1128, 9);
      u32(0x1428, 2);
      u32(0x1430, 24);
      u32(0x1434, 7);
      u64(0x1440, 0x1100);
      u64(0x1448, 16);
    } else {
      u32(0x1114, 1);
      u32(0x1118, 2);
      u64(0x1400, 0x200);
      u64(0x1408, 0x1100);
      u32(0x1410, 0);
    }
    Prefix = Class ? "_$s4Demo3BoxC" : "_$s4Demo4CellV";
    Symbol Metadata;
    Metadata.Name = Prefix + "N";
    Metadata.Addr = 0x1400;
    Image.Symbols.push_back(Metadata);
    auto &S = Request.Signature;
    S.Module = "Demo";
    S.ContextKind = Class ? "class" : "struct";
    S.ContextName = Class ? "Box" : "Cell";
    S.Name = "value";
    S.DeclarationKind = "runtime";
    const auto &Regs = getTargetRegInfo(Architecture);
    Self = Architecture == Arch::AArch64 ? a64reg::X20 : reg::R13;
    SP = Regs.StackPointer;
    Return = Regs.IntReturnReg;
    Second = Regs.IntReturnRegs[1];
    Arg = Regs.IntParamRegs[0];
  }
  void u32(va_t A, uint32_t V) {
    llvm::support::endian::write32le(Image.Segments[0].Data.data() + A - 0x1000,
                                     V);
  }
  void u64(va_t A, uint64_t V) {
    llvm::support::endian::write64le(Image.Segments[0].Data.data() + A - 0x1000,
                                     V);
  }
  void relative(va_t A, va_t B) { u32(A, uint32_t(B - A)); }
  void text(va_t A, const std::string &V) {
    std::copy(V.c_str(), V.c_str() + V.size() + 1,
              Image.Segments[0].Data.begin() + A - 0x1000);
  }
  size_t function(std::string Name, va_t Entry = 0) {
    if (!Entry)
      Entry = 0x3000 + Functions.size() * 0x200;
    LowFunc F;
    F.Entry = Entry;
    LowBlock B;
    B.StartAddr = Entry;
    F.Blocks.push_back(B);
    Functions.push_back(F);
    Symbol S;
    S.Name = std::move(Name);
    S.Addr = Entry;
    S.IsFunc = true;
    Image.Symbols.push_back(S);
    return Functions.size() - 1;
  }
  SwiftRuntimeSourceIdentity identity(size_t F) const {
    auto Entry = Functions[F].Entry;
    for (const auto &S : Image.Symbols)
      if (S.IsFunc && S.Addr == Entry)
        return {Entry, S.Name};
    return {};
  }
  void select(size_t F, Kind K) {
    auto ID = identity(F);
    Request.Kind = K;
    Request.Signature.Entry = ID.Entry;
    Request.Signature.MangledSymbol = ID.MangledSymbol;
  }
  void op(size_t F, NdOp Code, NdVar Out, std::initializer_list<NdVar> Inputs) {
    auto &B = Functions[F].Blocks[0];
    LowOp O;
    O.Opcode = Code;
    O.Output = Out;
    O.Addr = B.StartAddr + B.Ops.size() * 4;
    for (auto V : Inputs)
      O.addInput(V);
    B.Ops.push_back(O);
    B.EndAddr = O.Addr + 4;
  }
  void ret(size_t F) {
    auto &B = Functions[F].Blocks[0];
    auto N = B.Ops.size();
    op(F, NdOp::RETURN, {},
       {NdVar::reg(Image.Arch == Arch::AArch64 ? a64reg::X30 : Return, 8)});
    LowInstructionBoundary Boundary;
    Boundary.Address = B.Ops[N].Addr;
    Boundary.Size = 4;
    Boundary.FirstOp = N;
    Boundary.OpCount = 1;
    Boundary.Control = LowInstructionControl::Return;
    Boundary.ControlFlags = LowInstructionControlFlag::Return;
    B.InstructionBoundaries.push_back(Boundary);
  }
  void saveFrame(size_t F) {
    op(F, NdOp::INT_SUB, NdVar::reg(SP, 8),
       {NdVar::reg(SP, 8),
        NdVar::cst(Image.Arch == Arch::AArch64 ? 16 : 24, 8)});
    if (Image.Arch == Arch::AArch64)
      op(F, NdOp::STORE, {}, {NdVar::reg(SP, 8), NdVar::reg(a64reg::X30, 8)});
  }
  void restoreFrame(size_t F) {
    if (Image.Arch == Arch::AArch64)
      op(F, NdOp::LOAD, NdVar::reg(a64reg::X30, 8), {NdVar::reg(SP, 8)});
    op(F, NdOp::INT_ADD, NdVar::reg(SP, 8),
       {NdVar::reg(SP, 8),
        NdVar::cst(Image.Arch == Arch::AArch64 ? 16 : 24, 8)});
  }
  void call(size_t F, std::string Name, bool Tail = false) {
    const va_t Stub = 0x3e00 + Image.Imports.size() * 0x10;
    Import Import;
    Import.Name = std::move(Name);
    Import.Module = Import.Name == "_objc_opt_self"
                        ? "/usr/lib/libobjc.A.dylib"
                        : "/usr/lib/swift/libswiftCore.dylib";
    Image.Imports.push_back(Import);
    Image.ImportStubIndices[Stub] = Image.Imports.size() - 1;
    auto &B = Functions[F].Blocks[0];
    const auto N = B.Ops.size();
    op(F, NdOp::CALL, NdVar::reg(Return, 8), {NdVar::cst(Stub, 8)});
    if (Tail) {
      op(F, NdOp::RETURN, {}, {NdVar::reg(Return, 8)});
      B.Ops.back().Addr = B.Ops[N].Addr;
    }
    LowInstructionBoundary Boundary;
    Boundary.Address = B.Ops[N].Addr;
    Boundary.Size = 4;
    Boundary.FirstOp = N;
    Boundary.OpCount = Tail ? 2 : 1;
    Boundary.Control =
        Tail ? LowInstructionControl::TailCall : LowInstructionControl::Call;
    Boundary.ControlFlags = Tail ? LowInstructionControlFlag::Call |
                                       LowInstructionControlFlag::Return
                                 : LowInstructionControlFlag::Call;
    Boundary.Immediate = Stub;
    B.InstructionBoundaries.push_back(Boundary);
  }
  size_t destructor() {
    auto F = function(Prefix + "fd");
    op(F, NdOp::COPY, NdVar::reg(Return, 8), {NdVar::reg(Self, 8)});
    ret(F);
    return F;
  }
  void runtimeArgs(size_t F) {
    const auto &Regs = getTargetRegInfo(Image.Arch);
    op(F, NdOp::COPY, NdVar::reg(Arg, 8), {NdVar::reg(Self, 8)});
    op(F, NdOp::COPY, NdVar::reg(Regs.IntParamRegs[1], 4), {NdVar::cst(24, 4)});
    op(F, NdOp::INT_ZEXT, NdVar::reg(Regs.IntParamRegs[1], 8),
       {NdVar::reg(Regs.IntParamRegs[1], 4)});
    op(F, NdOp::COPY, NdVar::reg(Regs.IntParamRegs[2], 8), {NdVar::cst(7, 8)});
  }
  void allocator() {
    auto Init = function(Prefix + "yACs5Int64Vcfc");
    op(Init, NdOp::INT_ADD, NdVar::tmp(TmpBase, 8),
       {NdVar::reg(Self, 8), NdVar::cst(16, 8)});
    op(Init, NdOp::STORE, {}, {NdVar::tmp(TmpBase, 8), NdVar::reg(Arg, 8)});
    op(Init, NdOp::COPY, NdVar::reg(Return, 8), {NdVar::reg(Self, 8)});
    ret(Init);
    auto InitS = Request.Signature;
    auto InitID = identity(Init);
    InitS.Entry = InitID.Entry;
    InitS.MangledSymbol = InitID.MangledSymbol;
    InitS.DeclarationKind = "initializer";
    InitS.Name = "init";
    InitS.Parameters = {
        {"arg0", {SwiftSourceType::Kind::Integer, "Int64", 64, true}}};
    InitS.Labels = {"_"};
    Request.Initializer = InitS;
    Request.Signature.Parameters = InitS.Parameters;
    Request.Signature.Labels = InitS.Labels;
    auto F = function(Prefix + "yACs5Int64VcfC");
    select(F, Kind::AllocatingInitializer);
    saveFrame(F);
    op(F, NdOp::INT_ADD, NdVar::tmp(TmpBase, 8),
       {NdVar::reg(SP, 8), NdVar::cst(8, 8)});
    op(F, NdOp::STORE, {}, {NdVar::tmp(TmpBase, 8), NdVar::reg(Arg, 8)});
    runtimeArgs(F);
    call(F, "_swift_allocObject");
    op(F, NdOp::INT_ADD, NdVar::tmp(TmpBase, 8),
       {NdVar::reg(SP, 8), NdVar::cst(8, 8)});
    op(F, NdOp::LOAD, NdVar::tmp(TmpBase + 8, 8), {NdVar::tmp(TmpBase, 8)});
    op(F, NdOp::INT_ADD, NdVar::tmp(TmpBase, 8),
       {NdVar::reg(Return, 8), NdVar::cst(16, 8)});
    op(F, NdOp::STORE, {},
       {NdVar::tmp(TmpBase, 8), NdVar::tmp(TmpBase + 8, 8)});
    restoreFrame(F);
    ret(F);
  }
  void modify() {
    auto Resume = function(Prefix + "5values5Int64VvM.resume.0");
    ret(Resume);
    auto F = function(Prefix + "5values5Int64VvM");
    select(F, Kind::ModifyAccessor);
    Request.RelatedEntry = identity(Resume);
    op(F, NdOp::COPY, NdVar::reg(Second, 8), {NdVar::reg(Self, 8)});
    op(F, NdOp::COPY, NdVar::reg(Return, 8),
       {NdVar::cst(Functions[Resume].Entry, 8)});
    ret(F);
  }
  size_t emptyInitializer(bool Frame = false) {
    u32(0x1114, 0);
    u32(0x1118, 0);
    u32(0x130c, 0);
    u64(0x13f8, 0x1500);
    u64(0x1540, 0);
    u64(0x1548, 1);
    u32(0x1550, 0);
    u32(0x1554, 0);
    const auto Accessor = function(Prefix + "Ma");
    relative(0x110c, Functions[Accessor].Entry);
    const auto F = function(Prefix + "ACycfC");
    select(F, Kind::EmptyValueInitializer);
    Request.RelatedEntry = identity(Accessor);
    Request.Signature.Name = "init";
    Request.Signature.DeclarationKind = "initializer";
    Request.Signature.IsMutatingKnown = true;
    if (Frame) {
      saveFrame(F);
      restoreFrame(F);
    }
    ret(F);
    return F;
  }
  void metadata() {
    auto F = function(Prefix + "Ma");
    select(F, Kind::TypeMetadataAccessor);
    relative(0x110c, Functions[F].Entry);
    if (Request.Signature.ContextKind == "class") {
      saveFrame(F);
      op(F, NdOp::COPY, NdVar::reg(Arg, 8), {NdVar::cst(0x1400, 8)});
      call(F, "_objc_opt_self");
      restoreFrame(F);
    } else
      op(F, NdOp::COPY, NdVar::reg(Return, 8), {NdVar::cst(0x1400, 8)});
    op(F, NdOp::COPY, NdVar::reg(Second, 8), {NdVar::cst(0, 8)});
    ret(F);
  }
  SwiftRuntimeSourceProof proof() {
    return recoverSwiftRuntimeSource(Request, Image, Functions);
  }
};
} // namespace

TEST(SwiftRuntimeSource,
     AllocatorComparesBothNativeEffectGraphsOnBothArchitectures) {
  for (auto Arch : {Arch::AArch64, Arch::X64}) {
    Fixture F(true, Arch);
    F.allocator();
    auto P = F.proof();
    ASSERT_TRUE(P.Proven) << P.Reason;
    EXPECT_EQ(P.ProjectionKind, "allocating_initializer");
    ASSERT_EQ(P.Dependencies.size(), 2u);
    EXPECT_EQ(P.Dependencies[1].Identity.Entry, F.Request.Initializer->Entry);
    F.Functions[0].Blocks[0].Ops[1].Inputs[1] = NdVar::cst(7, 8);
    EXPECT_FALSE(F.proof().Proven); // Same field and metadata are insufficient.
  }
}
TEST(SwiftRuntimeSource,
     AllocatorRejectsMissingInitializerAndWrongRuntimeLayout) {
  Fixture F(true);
  F.allocator();
  auto Init = F.Request.Initializer;
  F.Request.Initializer.reset();
  EXPECT_FALSE(F.proof().Proven);
  F.Request.Initializer = Init;
  F.Image.Imports[0].Name = "_malloc";
  EXPECT_FALSE(F.proof().Proven);
  F.Image.Imports[0].Name = "_swift_allocObject";
  F.Image.Imports[0].Module = "@rpath/CustomAllocation.dylib";
  EXPECT_FALSE(F.proof().Proven);
  F.Image.Imports[0].Module = "/usr/lib/swift/libswiftCore.dylib";
  F.u32(0x1430, 32);
  EXPECT_FALSE(F.proof().Proven);
}
TEST(SwiftRuntimeSource, TrivialDestructorStillRequiresItsExactSelfReturn) {
  for (auto Arch : {Arch::AArch64, Arch::X64}) {
    Fixture F(true, Arch);
    auto N = F.destructor();
    F.select(N, Kind::TrivialDestructor);
    ASSERT_TRUE(F.proof().Proven) << F.proof().Reason;
    F.Functions[N].Blocks[0].Ops[0].Inputs[0] = NdVar::cst(0, 8);
    EXPECT_FALSE(F.proof().Proven);
  }
}
TEST(SwiftRuntimeSource,
     DeallocatorRequiresExactTailBoundaryAndTrivialDestroyDependency) {
  for (auto Arch : {Arch::AArch64, Arch::X64}) {
    Fixture F(true, Arch);
    auto D = F.destructor();
    auto N = F.function(F.Prefix + "fD");
    F.select(N, Kind::DeallocatingDestructor);
    F.Request.RelatedEntry = F.identity(D);
    F.runtimeArgs(N);
    F.call(N, "_swift_deallocClassInstance", true);
    F.u64(0x13f0, F.Functions[N].Entry);
    auto P = F.proof();
    ASSERT_TRUE(P.Proven) << P.Reason;
    EXPECT_EQ(P.Dependencies.back().Identity.Entry, F.Functions[D].Entry);
    F.u64(0x13f0, F.Functions[D].Entry);
    EXPECT_FALSE(F.proof().Proven);
    F.u64(0x13f0, F.Functions[N].Entry);
    F.Functions[N].Blocks[0].InstructionBoundaries.back().Control =
        LowInstructionControl::Call;
    EXPECT_FALSE(F.proof().Proven);
  }
}
TEST(SwiftRuntimeSource,
     MetadataAccessorsRequireDescriptorOwnershipAndCompletePair) {
  for (auto Arch : {Arch::AArch64, Arch::X64})
    for (bool Class : {false, true}) {
      Fixture F(Class, Arch);
      F.metadata();
      auto P = F.proof();
      ASSERT_TRUE(P.Proven) << P.Reason;
      EXPECT_EQ(P.Metadata, 0x1400u);
      EXPECT_EQ(P.Descriptor, 0x1100u);
      F.relative(0x110c, 0x3800);
      EXPECT_FALSE(F.proof().Proven);
      F.relative(0x110c, F.Request.Signature.Entry);
      auto &Ops = F.Functions[0].Blocks[0].Ops;
      Ops[Ops.size() - 2].Inputs[0] = NdVar::cst(1, 8);
      EXPECT_FALSE(F.proof().Proven);
    }
}
TEST(SwiftRuntimeSource,
     ClassMetadataMustPreserveActualRuntimeReturnNotInputAddress) {
  Fixture F(true);
  F.metadata();
  auto &B = F.Functions[0].Blocks[0];
  auto &Overwrite = B.Ops[B.Ops.size() - 2];
  Overwrite.Output = NdVar::reg(F.Return, 8);
  Overwrite.Inputs[0] = NdVar::cst(0x1400, 8);
  EXPECT_FALSE(F.proof().Proven);
  F.Image.Imports[0].Name = "_objc_lookUpClass";
  EXPECT_FALSE(F.proof().Proven);
}
TEST(SwiftRuntimeSource,
     ModifyAndContinuationKeepDistinctIdentityAndPropertyDependency) {
  for (auto Arch : {Arch::AArch64, Arch::X64}) {
    Fixture F(false, Arch);
    F.modify();
    auto P = F.proof();
    ASSERT_TRUE(P.Proven) << P.Reason;
    EXPECT_EQ(P.FieldOffset, 0u);
    EXPECT_EQ(P.Continuation.Entry, F.Functions[0].Entry);
    ASSERT_EQ(P.Dependencies.size(), 3u);
    EXPECT_EQ(P.Dependencies[1].Name, "value");
    const auto Modify = F.identity(1);
    F.select(0, Kind::ModifyResume);
    F.Request.RelatedEntry = Modify;
    P = F.proof();
    ASSERT_TRUE(P.Proven) << P.Reason;
    EXPECT_EQ(P.ProjectionKind, "modify_resume");
    Symbol Alias;
    Alias.Name = "_$s4Demo8identityS2fF";
    Alias.Addr = F.Functions[0].Entry;
    Alias.IsFunc = true;
    F.Image.Symbols.push_back(Alias);
    EXPECT_TRUE(
        F.proof().Proven); // Code folding must not erase continuation identity.
    F.Request.Signature.MangledSymbol = Alias.Name;
    EXPECT_FALSE(F.proof().Proven);
  }
}
TEST(SwiftRuntimeSource,
     ModifyRejectsWrongPointerWrongContinuationAndImmutableField) {
  Fixture F(false);
  F.modify();
  auto &Ops = F.Functions[1].Blocks[0].Ops;
  Ops[0].Inputs[0] = NdVar::cst(0, 8);
  EXPECT_FALSE(F.proof().Proven);
  Ops[0].Inputs[0] = NdVar::reg(F.Self, 8);
  Ops[1].Inputs[0] = NdVar::cst(F.Functions[0].Entry + 4, 8);
  EXPECT_FALSE(F.proof().Proven);
  Ops[1].Inputs[0] = NdVar::cst(F.Functions[0].Entry, 8);
  F.u32(0x1310, 0);
  EXPECT_FALSE(F.proof().Proven);
}
TEST(SwiftRuntimeSource,
     RelatedContinuationCannotHideMemoryEffectsOrAlteredControlFlow) {
  Fixture F(false);
  F.modify();
  auto &Op = F.Functions[0].Blocks[0].Ops[0];
  Op.Opcode = NdOp::STORE;
  Op.NumInputs = 0;
  Op.addInput(NdVar::reg(F.Self, 8));
  Op.addInput(NdVar::cst(0, 8));
  EXPECT_FALSE(F.proof().Proven);
  Fixture G(false);
  G.modify();
  G.Functions[0].Blocks[0].InstructionBoundaries[0].Control =
      LowInstructionControl::Branch;
  EXPECT_FALSE(G.proof().Proven);
}
TEST(SwiftRuntimeSource,
     AmbiguousBodiesAndUnownedImportsCannotEstablishProjection) {
  Fixture F(true);
  F.metadata();
  F.Functions.push_back(F.Functions[0]);
  EXPECT_FALSE(F.proof().Proven);
  F.Functions.pop_back();
  auto Imports = F.Image.ImportStubIndices;
  F.Image.ImportStubIndices.clear();
  EXPECT_FALSE(F.proof().Proven);
  F.Image.ImportStubIndices = Imports;
  F.Functions[0].Blocks.emplace_back();
  EXPECT_FALSE(F.proof().Proven);
}
TEST(SwiftRuntimeSource, NativeMetadataOverridesUntrustedSubmittedLayout) {
  Fixture F(false);
  F.modify();
  F.Request.Signature.ContextLayoutKnown = true;
  F.Request.Signature.ContextFields = {
      {"different",
       {SwiftSourceType::Kind::Integer, "Int64", 64, true},
       32,
       true}};
  auto P = F.proof();
  ASSERT_TRUE(P.Proven) << P.Reason;
  EXPECT_EQ(P.FieldOffset, 0u);
  EXPECT_EQ(P.Dependencies[1].Name, "value");
  F.Request.Signature.ContextName = "Other";
  EXPECT_FALSE(F.proof().Proven);
}
TEST(SwiftRuntimeSource, UnbalancedFrameAndLostCalleeSavedValueAreRejected) {
  Fixture F(true);
  F.allocator();
  auto &Ops = F.Functions[1].Blocks[0].Ops;
  Ops[Ops.size() - 2].Inputs[1] = NdVar::cst(8, 8);
  EXPECT_FALSE(F.proof().Proven);
  Fixture G(true);
  auto N = G.destructor();
  G.select(N, Kind::TrivialDestructor);
  G.Functions[N].Blocks[0].Ops[0].Output = NdVar::reg(a64reg::X19, 8);
  EXPECT_FALSE(G.proof().Proven);
}

TEST(SwiftRuntimeSource,
     PreservedFPRegisterAndCallAlignmentRemainABIObligations) {
  Fixture F(true);
  auto N = F.destructor();
  F.select(N, Kind::TrivialDestructor);
  auto &Ops = F.Functions[N].Blocks[0].Ops;
  auto Mutate = Ops[0];
  Mutate.Output = NdVar::reg(a64reg::V(8), 8);
  Mutate.Inputs[0] = NdVar::cst(0, 8);
  Ops.insert(Ops.begin(), Mutate);
  ++F.Functions[N].Blocks[0].InstructionBoundaries.back().FirstOp;
  EXPECT_FALSE(F.proof().Proven);
  Fixture G(true, Arch::X64);
  G.metadata();
  auto &Gops = G.Functions[0].Blocks[0].Ops;
  Gops.front().Inputs[1] = NdVar::cst(16, 8);
  Gops[Gops.size() - 3].Inputs[1] = NdVar::cst(16, 8);
  EXPECT_FALSE(G.proof().Proven); // Balanced at return, misaligned at call.
}

TEST(SwiftRuntimeSource, NarrowDisplacementCannotInventPointerSignExtension) {
  Fixture F(false);
  F.modify();
  auto &B = F.Functions[1].Blocks[0];
  auto Add = B.Ops[0];
  Add.Opcode = NdOp::INT_ADD;
  Add.NumInputs = 2;
  Add.Inputs[1] = NdVar::cst(0xffffffff, 4);
  B.Ops[0] = Add;
  Add.Inputs[0] = NdVar::reg(F.Second, 8);
  Add.Inputs[1] = NdVar::cst(1, 8);
  B.Ops.insert(B.Ops.begin() + 1, Add);
  ++B.InstructionBoundaries.back().FirstOp;
  EXPECT_FALSE(F.proof().Proven);
}

TEST(SwiftRuntimeSource, FrameOwnershipMustHoldAtTheInstructionThatUsesIt) {
  Fixture F(true);
  auto N = F.function(F.Prefix + "fd");
  F.select(N, Kind::TrivialDestructor);
  F.op(N, NdOp::INT_SUB, NdVar::tmp(TmpBase, 8),
       {NdVar::reg(F.SP, 8), NdVar::cst(16, 8)});
  F.op(N, NdOp::STORE, {},
       {NdVar::tmp(TmpBase, 8), NdVar::reg(a64reg::X19, 8)});
  F.op(N, NdOp::COPY, NdVar::reg(F.SP, 8), {NdVar::tmp(TmpBase, 8)});
  F.op(N, NdOp::LOAD, NdVar::reg(a64reg::X19, 8), {NdVar::reg(F.SP, 8)});
  F.op(N, NdOp::INT_ADD, NdVar::reg(F.SP, 8),
       {NdVar::reg(F.SP, 8), NdVar::cst(16, 8)});
  F.op(N, NdOp::COPY, NdVar::reg(F.Return, 8), {NdVar::reg(F.Self, 8)});
  F.ret(N);
  auto &B = F.Functions[N].Blocks[0];
  B.Ops[1].Addr = B.Ops[2].Addr = B.Ops[0].Addr;
  LowInstructionBoundary PreIndex;
  PreIndex.Address = B.StartAddr;
  PreIndex.Size = 4;
  PreIndex.FirstOp = 0;
  PreIndex.OpCount = 3;
  B.InstructionBoundaries.insert(B.InstructionBoundaries.begin(), PreIndex);
  ASSERT_TRUE(F.proof().Proven) << F.proof().Reason;
  B.Ops[2].Addr += 4;
  EXPECT_FALSE(
      F.proof()
          .Proven); // A later instruction cannot retroactively own the store.
  B.Ops[2].Addr = B.Ops[0].Addr;
  auto Release = B.Ops[4];
  auto Reallocate = Release;
  Reallocate.Opcode = NdOp::INT_SUB;
  B.Ops.insert(B.Ops.begin() + 3, {Release, Reallocate});
  B.InstructionBoundaries.back().FirstOp += 2;
  auto P = F.proof();
  EXPECT_FALSE(P.Proven);
  EXPECT_NE(P.Reason.find("undefined stack slot"), std::string::npos);
}

TEST(SwiftRuntimeSource, EmptyInitializerProvesNoValueStorageAndRetainsAliases) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (bool Frame : {false, true}) {
      SCOPED_TRACE(static_cast<unsigned>(Architecture));
      SCOPED_TRACE(Frame);
      Fixture F(false, Architecture);
      const auto N = F.emptyInitializer(Frame);
      Symbol Alias;
      Alias.Name = "_$s4Demo8identityys5Int64VADF";
      Alias.Addr = F.Functions[N].Entry;
      Alias.IsFunc = true;
      F.Image.Symbols.push_back(Alias);
      const auto P = F.proof();
      ASSERT_TRUE(P.Proven) << P.Reason;
      EXPECT_EQ(P.ProjectionKind, "empty_value_initializer");
      EXPECT_EQ(P.Descriptor, 0x1100U);
      EXPECT_EQ(P.Metadata, 0x1400U);
      ASSERT_EQ(P.Dependencies.size(), 2U);
      EXPECT_EQ(P.Dependencies[0].Kind, "context");
      EXPECT_EQ(P.Dependencies[1].Kind, "compiler_entry");
      EXPECT_EQ(P.Dependencies[1].Identity.Entry,
                F.Request.RelatedEntry->Entry);
      EXPECT_EQ(P.Dependencies[1].Identity.MangledSymbol, F.Prefix + "Ma");
      EXPECT_FALSE(P.Evidence.empty());
    }
  }
}

TEST(SwiftRuntimeSource, EmptyInitializerRejectsLayoutIdentityAndNativeEffects) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 16; ++Mutation) {
      SCOPED_TRACE(static_cast<unsigned>(Architecture));
      SCOPED_TRACE(Mutation);
      Fixture F(false, Architecture);
      const auto N = F.emptyInitializer();
      auto &B = F.Functions[N].Blocks[0];
      if ((Mutation >= 7 && Mutation <= 11) || Mutation == 15) {
        B.Ops.clear();
        B.InstructionBoundaries.clear();
      }
      switch (Mutation) {
      case 0:
        F.u32(0x1550, 15); // Overaligned zero-field storage is not plain empty.
        break;
      case 1:
        F.u64(0x1540, 8);
        break;
      case 2:
        F.Request.Signature.Parameters.push_back(
            {"arg0", {SwiftSourceType::Kind::Integer, "Int64", 64, true}});
        F.Request.Signature.Labels.push_back("_");
        break;
      case 3:
        F.Request.Signature.MangledSymbol = F.Prefix + "ACycfc";
        F.Image.Symbols.back().Name = F.Request.Signature.MangledSymbol;
        break;
      case 4:
        F.Request.RelatedEntry.reset();
        break;
      case 5:
        F.relative(0x110c, F.Request.RelatedEntry->Entry + 4);
        break;
      case 6:
        B.InstructionBoundaries.clear();
        break;
      case 7:
        F.op(N, NdOp::LOAD, NdVar::reg(F.Return, 8),
             {NdVar::cst(0x1400, 8)});
        break;
      case 8:
        F.op(N, NdOp::STORE, {},
             {NdVar::cst(0x1400, 8), NdVar::cst(1, 8)});
        break;
      case 9:
        F.call(N, "_unknown_effect");
        break;
      case 10:
        F.op(N, NdOp::INT_SUB, NdVar::reg(F.SP, 8),
             {NdVar::reg(F.SP, 8), NdVar::cst(16, 8)});
        break;
      case 11:
        F.op(N, NdOp::COPY, NdVar::reg(F.Self, 8), {NdVar::cst(0, 8)});
        break;
      case 12:
        B.Ops.back().Inputs[0] = NdVar::reg(F.SP, 8);
        break;
      case 13:
        F.Functions.push_back(F.Functions[N]);
        break;
      case 14:
        F.u32(0x1550, 0x00800000); // Noncopyable declared values.
        break;
      case 15:
        F.op(N, NdOp::COPY,
             NdVar::reg(Architecture == Arch::AArch64
                            ? a64reg::V0 + 8 * 16
                            : getTargetRegInfo(Architecture).FramePointer,
                        8),
             {NdVar::cst(0, 8)});
        break;
      }
      if ((Mutation >= 7 && Mutation <= 11) || Mutation == 15)
        F.ret(N);
      const auto P = F.proof();
      EXPECT_FALSE(P.Proven);
      EXPECT_FALSE(P.Reason.empty());
    }
  }
}
