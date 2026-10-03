#include "../../../lib/ir/high/pass/HighDCEDetail.h"
#include "../../../lib/loader/MachO/ImmutableNativeFrame.h"
#include "../../../lib/sdk/capi/ObjCCFunctionParameterSources.h"
#include "../../../lib/sdk/capi/ObjCImmutableNativeSources.h"
#include "../../../lib/sdk/capi/ObjCNativeDependencies.h"
#include "../../../lib/sdk/capi/ObjCSourceBindings.h"
#include "../../../lib/sdk/capi/ObjCSourceInputs.h"
#include "../../../lib/sdk/capi/SourceSwiftWitnessFrameProjection.h"
#include "../core/CFunctionParameterCallFixture.h"
#include "../core/ImmutableNativeCallFixture.h"
#include "../core/RuntimeFunctionAddressFixture.h"
#include "../core/SwiftWitnessFrameFixture.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

#include <array>
#include <filesystem>
#include <fstream>

using namespace neverd;
using namespace neverd::sdk;

namespace {
void frameTableFixture(immutable_native_call_test::Fixture &F);
}

TEST(ImmutableNativeCalls,
     ProvesOneOriginalIndirectOccurrenceAcrossRuntimeCall) {
  using namespace immutable_native_call_test;
  Fixture F;
  ASSERT_TRUE(F.Result.Success) << F.Result.Error;
  ASSERT_TRUE(F.low());
  EXPECT_EQ(readImmutableImageCodePointer(F.Image, Slot), Target);
  EXPECT_FALSE(readImmutableImagePointer(F.Image, Slot));
  EXPECT_FALSE(readInitialImagePointer(F.Image, Slot));
  EXPECT_FALSE(readImmutableChainedImageValue(F.Image, Slot));
  EXPECT_FALSE(readImmutableImageBytes(F.Image, Slot, 8));
  const auto Targets = immutableNativeCallTargets(F.Image, *F.low());
  ASSERT_EQ(Targets.size(), 1U);
  ASSERT_TRUE(Targets.count(Call));
  EXPECT_EQ(Targets.at(Call).Slot, Slot);
  EXPECT_EQ(Targets.at(Call).Target, Target);
  EXPECT_EQ(Targets.at(Call).FunctionEntry, Entry);
  EXPECT_EQ(Targets.at(Call).Site.Opcode, NdOp::INDIR_CALL);
  EXPECT_FALSE(Targets.at(Call).Site.StaticTarget);
  NativeSourceDependencyEvidence Evidence;
  const auto Dependencies =
      walkObjCNativeDependencies(F.Image, F.Result, &Evidence, {Entry});
  EXPECT_TRUE(Dependencies.count(Target));
  EXPECT_TRUE(std::any_of(
      Evidence.Calls.begin(), Evidence.Calls.end(), [](const auto &Edge) {
        return Edge.Caller == Entry && Edge.Instruction == Call &&
               Edge.Target == Target && Edge.Indirect;
      }));
}

TEST(ImmutableNativeCalls, BindsTheCurrentCalleeABIAtTheOriginalOccurrence) {
  using namespace immutable_native_call_test;
  Fixture F;
  ASSERT_TRUE(F.Result.Success) << F.Result.Error;
  unsigned Calls = 0;
  for (const auto &Function : F.Result.MedFuncs)
    if (Function.Entry == Entry)
      for (const auto &Block : Function.Blocks)
        for (const auto &Op : Block.Ops)
          if (Op.Opcode == NdOp::INDIR_CALL) {
            ++Calls;
            ASSERT_TRUE(Op.SourceCallHint);
            EXPECT_EQ(Op.Addr, Call);
            EXPECT_EQ(Op.OriginSeq, 1);
            EXPECT_FALSE(Op.Inputs[0].isConst());
            EXPECT_EQ(Op.SourceCallHint->TargetAddress, Target);
            EXPECT_TRUE(
                equalSourceABIs(Op.SourceCallHint->Signature, F.Signature));
          }
  EXPECT_EQ(Calls, 1U);
}

namespace {
ExprPtr immutableNativeExpression(const HighFunc &Function) {
  ExprPtr Found;
  walkStmts(Function.Body, [&](const HighStmt &Statement) {
    forEachExpr(Statement, [&](const ExprPtr &Root) {
      std::vector<ExprPtr> Pending{Root};
      while (!Pending.empty()) {
        auto Expression = Pending.back();
        Pending.pop_back();
        if (!Expression)
          continue;
        if (Expression->SourceCallHint &&
            Expression->SourceCallHint->ImmutableNativeCall)
          Found = Expression;
        Expression->forEachChildExpr(
            [&](const ExprPtr &Child) { Pending.push_back(Child); });
      }
    });
  });
  return Found;
}
} // namespace

TEST(ImmutableNativeCalls, RequiresAnExplicitCurrentScalarCalleeDeclaration) {
  using namespace immutable_native_call_test;
  for (unsigned Case = 0; Case < 9; ++Case) {
    SCOPED_TRACE(Case);
    Fixture F;
    auto Signature = F.Signature;
    std::map<va_t, SourceFunctionTypeHint> Callees{{Target, Signature}};
    switch (Case) {
    case 0:
      break;
    case 1:
      Callees.clear();
      break;
    case 2:
      Signature.HasExplicitABI = false;
      break;
    case 3:
      Signature.Architecture = Arch::X64;
      break;
    case 4:
      Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
      break;
    case 5:
      Signature.ReturnType.reset();
      break;
    case 6:
      Signature.ReturnLocation.ValueBytes = 4;
      break;
    case 7:
      Callees = {{Target + 4, Signature}};
      break;
    case 8:
      Signature.Parameters.push_back({"missing_carrier", NdType::makeInt(8)});
      break;
    }
    if (Case != 1 && Case != 7)
      Callees[Target] = Signature;
    const auto Hints =
        buildImmutableNativeCallHints(F.Image, *F.low(), Callees);
    EXPECT_EQ(Hints.size(), Case == 0 ? 1U : 0U);
  }
}

TEST(ImmutableNativeCalls, PublicationRepeatsTheCurrentCallerAndCalleeProof) {
  using namespace immutable_native_call_test;
  Fixture F;
  F.bindCaller();
  ASSERT_TRUE(F.Result.Success) << F.Result.Error;
  ASSERT_TRUE(F.high());
  const auto Expression = immutableNativeExpression(*F.high());
  ASSERT_TRUE(Expression);
  EXPECT_TRUE(isImmutableNativeSourceCall(*Expression, Entry, F.Image.Arch));
  EXPECT_FALSE(objcSourceCallBound(*Expression, F.Image, {{Target, F.high()}}));
  EXPECT_TRUE(objCImmutableNativeSourceCallBound(*Expression, F.Image, F.Result,
                                                 *F.high()));
  EXPECT_FALSE(Expression->IsIndirectCall);
  EXPECT_FALSE(Expression->IndirectTarget);
  ASSERT_TRUE(F.med());
  EXPECT_EQ(boundNativeBooleanCallees(*F.med()).size(), 1U);
}

TEST(ImmutableNativeCalls, PublicationRejectsStaleOrDuplicatedEvidence) {
  using namespace immutable_native_call_test;
  for (unsigned Case = 0; Case < 30; ++Case) {
    SCOPED_TRACE(Case);
    Fixture F;
    F.bindCaller();
    ASSERT_TRUE(F.high());
    auto Expression = immutableNativeExpression(*F.high());
    ASSERT_TRUE(Expression);
    auto Hint = *Expression->SourceCallHint;
    switch (Case) {
    case 0:
      F.Result.SourceImage = nullptr;
      break;
    case 1:
      F.Result.Success = false;
      break;
    case 2:
      F.Result.LowFuncs.push_back(*F.low());
      break;
    case 3:
      F.Result.MedFuncs.push_back(*F.med());
      break;
    case 4:
      F.Result.HighFuncs.push_back(*F.high());
      break;
    case 5:
      for (auto &Audit : F.Result.FunctionAudits)
        if (Audit.Entry == Entry)
          ++Audit.DecodedInstructions;
      break;
    case 6:
      for (auto &Audit : F.Result.FunctionAudits)
        if (Audit.Entry == Target)
          Audit.MedIRVerified = false;
      break;
    case 7:
      for (auto &Function : F.Result.HighFuncs)
        if (Function.Entry == Target)
          Function.SourceTypeHint.reset();
      break;
    case 8:
      for (auto &Function : F.Result.MedFuncs)
        if (Function.Entry == Target)
          Function.SourceParametersBound = false;
      break;
    case 9:
      for (auto &Function : F.Result.HighFuncs)
        if (Function.Entry == Target)
          Function.SourceTypeHint->ReturnType = NdType::makeInt(4);
      break;
    case 10:
      F.Image.CodePtrRelocSlots.clear();
      break;
    case 11:
      F.Image.Segments.back().ReadOnlyAfterRelocations = false;
      break;
    case 12:
      F.word(7, 0xd63f0260);
      break;
    case 13:
      for (auto &Low : F.Result.LowFuncs)
        if (Low.Entry == Entry)
          for (auto &Block : Low.Blocks)
            for (auto &Op : Block.Ops)
              if (Op.Opcode == NdOp::INDIR_CALL)
                Op.Inputs[0].Offset = a64reg::X19;
      break;
    case 14:
      Hint.ImmutableNativeCall->Site.Instruction += 4;
      break;
    case 15:
      Hint.ImmutableNativeCall->Site.Sequence = 0;
      break;
    case 16:
      Hint.ImmutableNativeCall->Slot += 8;
      break;
    case 17:
      Hint.ImmutableNativeCall->Target += 4;
      break;
    case 18:
      Hint.ImmutableNativeCall->FunctionEntry += 4;
      break;
    case 19:
      Hint.DoesNotReturn = true;
      break;
    case 20:
      Hint.WeakImport = true;
      break;
    case 21:
      Hint.Signature.Convention = SourceFunctionTypeHint::ConventionKind::Swift;
      break;
    case 22:
      Expression->IsIndirectCall = true;
      break;
    case 23:
      Expression->IndirectTarget = HighExpr::makeConst(Slot, 8);
      break;
    case 24:
      Expression->CallAddr = Target + 4;
      break;
    case 25:
      Expression->Operands.push_back(HighExpr::makeConst(0, 8));
      break;
    case 26: {
      HighStmt S;
      S.Kind = StmtKind::Call;
      S.CallExpr = Expression;
      F.high()->Body.push_back(S);
    } break;
    case 27:
      for (auto &Block : F.med()->Blocks)
        for (auto &Op : Block.Ops)
          if (Op.Opcode == NdOp::INDIR_CALL)
            Op.OriginSeq = 0;
      break;
    case 28:
      for (auto &Block : F.med()->Blocks)
        for (auto &Op : Block.Ops)
          if (Op.Opcode == NdOp::INDIR_CALL)
            Op.SourceCallHint.reset();
      break;
    case 29:
      F.med()->SourceParametersBound = false;
      break;
    }
    Expression->SourceCallHint =
        std::make_shared<const SourceCallTypeHint>(Hint);
    EXPECT_FALSE(objCImmutableNativeSourceCallBound(*Expression, F.Image,
                                                    F.Result, *F.high()));
  }
}

TEST(ImmutableNativeCalls, NativeInferenceRechecksTheMachineOccurrence) {
  using namespace immutable_native_call_test;
  for (unsigned Case = 0; Case < 8; ++Case) {
    SCOPED_TRACE(Case);
    Fixture F;
    ASSERT_TRUE(F.med());
    ASSERT_TRUE(F.high());
    if (Case == 1)
      F.word(7, 0xd63f0260);
    if (Case == 2)
      F.Image.CodePtrRelocSlots.clear();
    for (auto &Block : F.med()->Blocks)
      for (auto &Op : Block.Ops) {
        if (Op.Opcode != NdOp::INDIR_CALL)
          continue;
        ASSERT_TRUE(Op.SourceCallHint);
        auto Hint = *Op.SourceCallHint;
        if (Case == 3)
          ++Hint.ImmutableNativeCall->Site.Sequence;
        if (Case == 4)
          Hint.ImmutableNativeCall->Slot += 8;
        if (Case == 5)
          Hint.CallKind = SourceCallTypeHint::Kind::SwiftRuntimeCall;
        if (Case == 6)
          --Op.NumInputs;
        if (Case == 7)
          Op.DoesNotReturn = true;
        Op.SourceCallHint = std::make_shared<const SourceCallTypeHint>(Hint);
      }
    const auto Audit = std::find_if(
        F.Result.FunctionAudits.begin(), F.Result.FunctionAudits.end(),
        [](const auto &A) { return A.Entry == Entry; });
    ASSERT_NE(Audit, F.Result.FunctionAudits.end());
    std::string Reason;
    const auto Hint = inferNativeSourceTypeHint(F.Image, *F.med(), *F.high(),
                                                *Audit, Reason, F.low());
    EXPECT_EQ(bool(Hint), Case == 0) << Reason;
  }
}

TEST(ImmutableNativeCalls, GeneratedSourceMatchesOriginalARM64CallsAtO0AndO2) {
#if defined(NEVERD_TEST_CLANG) && defined(__APPLE__) && defined(__aarch64__)
  for (bool FrameReload : {false, true}) {
    SCOPED_TRACE(FrameReload);
    using namespace immutable_native_call_test;
    Fixture F;
    if (FrameReload)
      frameTableFixture(F);
    F.bindCaller();
    ASSERT_TRUE(F.high());
    std::map<va_t, const HighFunc *> Functions;
    for (const auto &Function : F.Result.HighFuncs)
      Functions.emplace(Function.Entry, &Function);
    auto Bound =
        bindObjCSourceReferences(*F.high(), F.Image, nullptr, &Functions);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    elimUnreadPrivateFrameStores(Bound.Function, F.Image.Arch);
    const auto Audit = std::find_if(
        F.Result.FunctionAudits.begin(), F.Result.FunctionAudits.end(),
        [](const auto &A) { return A.Entry == Entry; });
    ASSERT_NE(Audit, F.Result.FunctionAudits.end());
    const auto Allowed = [&](const HighExpr &Expression) {
      return objcSourceCallBound(Expression, F.Image, Functions, nullptr,
                                 nullptr, &Bound.Function) ||
             objCImmutableNativeSourceCallBound(Expression, F.Image, F.Result,
                                                Bound.Function);
    };
    ASSERT_TRUE(sourceBodyLimitation(Bound.Function,
                                     *Bound.Function.SourceTypeHint, &*Audit,
                                     Allowed)
                    .empty());
    CEmitterOptions Options;
    Options.TheArch = Arch::AArch64;
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    ASSERT_TRUE(HighCEmitter().emit({Bound.Function, *Functions.at(Target)}, OS,
                                    Options));
    EXPECT_EQ(Source.find("0x3028"), std::string::npos);
    llvm::SmallString<128> Directory;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-immutable-native",
                                                      Directory));
    const std::filesystem::path Work(Directory.str().str());
    struct Cleanup {
      std::filesystem::path Work;
      ~Cleanup() {
        std::error_code Error;
        std::filesystem::remove_all(Work, Error);
      }
    } Cleanup{Work};
    const auto Path = (Work / "calls.c").string();
    std::ofstream(Path) << (FrameReload ? "#define FRAME_RELOAD 1\n" : "")
                        << Source << R"(
#include <stddef.h>
static unsigned calls;
static uintptr_t observed;
void swift_release(void *object) { ++calls; observed = (uintptr_t)object; }
extern uint64_t original_indirect_native(void *);
__asm__(".text\n.p2align 2\n.globl _original_indirect_native\n"
        "_original_indirect_native:\n"
#ifdef FRAME_RELOAD
        "sub sp,sp,#64\nstp x19,x30,[sp,#48]\n"
        "adrp x19,_original_target_table@PAGE\n"
        "add x19,x19,_original_target_table@PAGEOFF\nstr x19,[sp,#16]\n"
        "bl _swift_release\nldr x19,[sp,#16]\nldr x8,[x19,#8]\nblr x8\n"
        "ldp x19,x30,[sp,#48]\nadd sp,sp,#64\nret\n"
#else
        "stp x19,x20,[sp,#-32]!\nstp x29,x30,[sp,#16]\nadd x29,sp,#16\n"
        "adrp x19,_original_target_table@PAGE\n"
        "add x19,x19,_original_target_table@PAGEOFF\nldr x20,[x19,#8]\n"
        "bl _swift_release\nblr x20\n"
        "ldp x29,x30,[sp,#16]\nldp x19,x20,[sp],#32\nret\n"
#endif
        "_original_constant_result:\nmov x0,#42\nret\n"
        ".section __DATA_CONST,__const\n.p2align 3\n_original_target_table:\n"
        ".quad 0\n.quad _original_constant_result\n.text\n");
int main(void) {
  unsigned char objects[512];
  for (unsigned i = 0; i != 512; ++i) {
    void *p = i % 7 ? objects + i : NULL;
    calls = 0; observed = UINTPTR_MAX;
    uint64_t expected = original_indirect_native(p);
    if (calls != 1 || observed != (uintptr_t)p || expected != 42) return 1;
    calls = 0; observed = UINTPTR_MAX;
    uint64_t actual = indirect_native(p);
    if (calls != 1 || observed != (uintptr_t)p || actual != expected) return 2;
  }
  return 0;
}
)";
    for (const auto *Level : {"-O0", "-O2"}) {
      SCOPED_TRACE(Level);
      const auto Output = (Work / (std::string("calls") + Level)).string();
      const std::vector<llvm::StringRef> Args{NEVERD_TEST_CLANG, Level, Path,
                                              "-o", Output};
      EXPECT_EQ(llvm::sys::ExecuteAndWait(NEVERD_TEST_CLANG, Args), 0);
      EXPECT_EQ(llvm::sys::ExecuteAndWait(Output, {Output}), 0);
    }
  }
#else
  GTEST_SKIP() << "Original ARM64 comparison requires Apple ARM64 and Clang";
#endif
}

TEST(ImmutableNativeCalls, RejectsMutableAmbiguousAndNonCodePointerStorage) {
  using namespace immutable_native_call_test;
  for (unsigned Case = 0; Case < 28; ++Case) {
    SCOPED_TRACE(Case);
    Fixture F;
    switch (Case) {
    case 0:
      F.Image.Segments.back().ReadOnlyAfterRelocations = false;
      break;
    case 1:
      F.Image.CodePtrRelocSlots.clear();
      break;
    case 2:
      F.Image.MachOResolvedChainedPointerSlots.clear();
      break;
    case 3:
      F.Image.MachOHasChainedFixups = false;
      break;
    case 4:
      F.Image.MachOChainedFixupsAmbiguous = true;
      break;
    case 5:
      F.Image.DataPtrRelocSlots.insert(Slot);
      break;
    case 6:
      F.Image.DataPtrRelocTargetOwners[Slot] = Table;
      break;
    case 7:
      F.Image.RelCodeRelocSlots.insert(Slot);
      break;
    case 8:
      F.Image.RelDataPtrRelocSlots.insert(Slot);
      break;
    case 9:
      F.Image.CodePtrRelocSlots.insert(Slot + 4);
      break;
    case 10:
      F.Image.MachOResolvedChainedPointerSlots.insert(Slot - 4);
      break;
    case 11:
      F.Image.ImportPtrSlots[Slot] = "_unknown";
      break;
    case 12:
      F.Image.DyldBindSlots[Slot] = {"_unknown", 0, "unknown", false};
      break;
    case 13:
      F.Image.ConflictingImportStorageSlots.insert(Slot);
      break;
    case 14:
      F.Image.Sections.push_back(F.Image.Sections.back());
      break;
    case 15:
      F.Image.Segments.push_back(F.Image.Segments.back());
      break;
    case 16:
      F.Image.Sections.back().FileSz = 0x2c;
      break;
    case 17:
      F.Image.Segments.back().FileSz = 0x2c;
      break;
    case 18:
      F.Image.Sections.back().Type = llvm::MachO::S_ZEROFILL;
      break;
    case 19:
      F.Image.Sections.back().FileOff += 8;
      break;
    case 20:
      F.Image.Symbols.pop_back();
      break;
    case 21:
      F.Image.IsRelocatable = true;
      break;
    case 22:
      F.Image.Bits = Bitness::Bits32;
      break;
    case 23:
      F.Image.Arch = Arch::ARM;
      break;
    case 24:
      F.Image.Format = BinaryFormat::ELF;
      break;
    case 25:
      llvm::support::endian::write64le(
          F.Image.Segments.back().Data.data() + 0x28, Target + 4);
      break;
    case 26:
      llvm::support::endian::write64le(
          F.Image.Segments.back().Data.data() + 0x28, Table);
      break;
    case 27:
      F.Image.Segments[0].Flags = SegmentFlags::Readable |
                                  SegmentFlags::Writable |
                                  SegmentFlags::Executable;
      break;
    }
    EXPECT_FALSE(readImmutableImageCodePointer(F.Image, Slot));
    EXPECT_TRUE(immutableNativeCallTargets(F.Image, *F.low()).empty());
  }
}

TEST(ImmutableNativeCalls, RechecksInstructionBytesAndLowIRAgainstTheSameSite) {
  using namespace immutable_native_call_test;
  for (unsigned Case = 0; Case < 18; ++Case) {
    SCOPED_TRACE(Case);
    Fixture F;
    auto Low = *F.low();
    auto Change = [&](va_t Address, int Seq, auto Mutate) {
      for (auto &Block : Low.Blocks)
        for (auto &Op : Block.Ops)
          if (Op.Addr == Address && Op.Seq == Seq)
            Mutate(Op);
    };
    switch (Case) {
    case 0:
      F.word(3, 0xd0000012);
      break;
    case 1:
      F.word(4, 0x9100a273);
      break;
    case 2:
      F.word(5, 0xb9400674);
      break;
    case 3:
      F.word(7, 0xd63f0260);
      break;
    case 4:
      F.word(6, 0x94000039);
      break;
    case 5:
      F.Image.DyldBindSlots[0x2000].Module = "unknown";
      break;
    case 6:
      Change(Entry + 12, 0, [](auto &Op) { Op.Inputs[0].Offset += 0x1000; });
      break;
    case 7:
      Change(Entry + 16, 2, [](auto &Op) { Op.Inputs[1].Offset += 8; });
      break;
    case 8:
      Change(Entry + 20, 1, [](auto &Op) { Op.Inputs[1].Offset += 8; });
      break;
    case 9:
      Change(Entry + 20, 2, [](auto &Op) { Op.Output.Size = 4; });
      break;
    case 10:
      Change(Entry + 20, 2,
             [](auto &Op) { Op.MemoryOrdering = NdMemoryOrdering::Acquire; });
      break;
    case 11:
      Change(Entry + 20, 3, [](auto &Op) { Op.Inputs[0].Offset += 8; });
      break;
    case 12:
      Change(Call, 1, [](auto &Op) { Op.Inputs[0].Size = 4; });
      break;
    case 13:
      Low.DecodedInstructionCount++;
      break;
    case 14:
      Low.LiftedInstructionCount--;
      break;
    case 15:
      Low.Blocks.front().InstructionBoundaries.clear();
      break;
    case 16:
      Low.Blocks.push_back(Low.Blocks.front());
      break;
    case 17:
      Change(Entry + 16, 0, [](auto &Op) { Op.Inputs[0].Offset += 8; });
      break;
    }
    EXPECT_TRUE(immutableNativeCallTargets(F.Image, Low).empty());
  }
}

TEST(ImmutableNativeCalls,
     UsesExactARCCallABIWithoutRebindingItsFunctionAddress) {
  using namespace immutable_native_call_test;
  Fixture F;
  F.Image.ImportPtrSlots[0x2000] = "_objc_retain_x20";
  F.Image.DyldBindSlots[0x2000] = {"_objc_retain_x20", 0,
                                   "/usr/lib/libobjc.A.dylib", false};
  F.Image.DynInfo.NeededLibs = {"/usr/lib/libobjc.A.dylib"};
  F.run();
  ASSERT_TRUE(F.low());
  EXPECT_FALSE(runtimeCFunctionAddressHint(F.Image, 0x2000));
  EXPECT_EQ(immutableNativeCallTargets(F.Image, *F.low()).size(), 1U);
  for (unsigned Case = 0; Case < 4; ++Case) {
    SCOPED_TRACE(Case);
    auto Image = F.Image;
    switch (Case) {
    case 0:
      Image.DyldBindSlots[0x2000].Module = "unknown";
      break;
    case 1:
      Image.DyldBindSlots[0x2000].WeakImport = true;
      break;
    case 2:
      Image.DyldBindSlots[0x2000].Addend = 8;
      break;
    case 3:
      Image.ImportPtrSlots[0x2000] = "_objc_retain_x19";
      break;
    }
    EXPECT_TRUE(immutableNativeCallTargets(Image, *F.low()).empty());
  }
}

TEST(ImmutableNativeCalls, RequiresCurrentExplicitNativePreservationContract) {
  using namespace immutable_native_call_test;
  Fixture F;
  F.word(6, 0x9400007a); // BL Target, not a runtime import.
  F.run();
  ASSERT_TRUE(F.Result.Success);
  ASSERT_TRUE(F.low());
  EXPECT_TRUE(immutableNativeCallTargets(F.Image, *F.low()).empty());
  std::map<va_t, SourceFunctionTypeHint> Callees{{Target, F.Signature}};
  EXPECT_EQ(immutableNativeCallTargets(F.Image, *F.low(), &Callees).size(), 1U);
  const auto Dependencies =
      walkObjCNativeDependencies(F.Image, F.Result, nullptr, {Entry});
  EXPECT_TRUE(Dependencies.count(Target));
  for (unsigned Case = 0; Case < 4; ++Case) {
    SCOPED_TRACE(Case);
    auto Bad = Callees;
    switch (Case) {
    case 0:
      Bad[Target].HasExplicitABI = false;
      break;
    case 1:
      Bad[Target].Architecture = Arch::X64;
      break;
    case 2:
      Bad[Target].Origin = SourceFunctionTypeHint::OriginKind::ObjCSDK;
      break;
    case 3:
      Bad[Target].ReturnLocation.RegisterOffset = a64reg::X20;
      break;
    }
    EXPECT_TRUE(immutableNativeCallTargets(F.Image, *F.low(), &Bad).empty());
  }
}

TEST(ImmutableNativeCalls, InventoryRechecksCurrentNativeCalleeEvidence) {
  using namespace immutable_native_call_test;
  for (unsigned Case = 0; Case < 7; ++Case) {
    SCOPED_TRACE(Case);
    Fixture F;
    F.word(6, 0x9400007a);
    F.run();
    auto &Result = F.Result;
    auto High = std::find_if(Result.HighFuncs.begin(), Result.HighFuncs.end(),
                             [](const auto &F) { return F.Entry == Target; });
    auto Audit =
        std::find_if(Result.FunctionAudits.begin(), Result.FunctionAudits.end(),
                     [](const auto &A) { return A.Entry == Target; });
    ASSERT_NE(High, Result.HighFuncs.end());
    ASSERT_NE(Audit, Result.FunctionAudits.end());
    switch (Case) {
    case 0:
      High->SourceTypeHint.reset();
      break;
    case 1:
      Result.HighFuncs.push_back(*High);
      break;
    case 2:
      Result.FunctionAudits.push_back(*Audit);
      break;
    case 3:
      Audit->LiftedInstructions++;
      break;
    case 4:
      Audit->MedIRVerified = false;
      break;
    case 5:
      High->SourceTypeHint->Architecture = Arch::X64;
      break;
    case 6:
      Result.LowFuncs.push_back(*F.low());
      break;
    }
    NativeSourceDependencyEvidence Evidence;
    walkObjCNativeDependencies(F.Image, Result, &Evidence, {Entry});
    EXPECT_FALSE(std::any_of(Evidence.Calls.begin(), Evidence.Calls.end(),
                             [](const auto &Edge) {
                               return Edge.Caller == Entry &&
                                      Edge.Instruction == Call && Edge.Target;
                             }));
    EXPECT_FALSE(Evidence.TargetsComplete);
  }
}

TEST(ImmutableNativeCalls, KeepsTraceWithinOneBlockAndRejectsFrameReloads) {
  using namespace immutable_native_call_test;
  for (unsigned Case = 0; Case < 4; ++Case) {
    SCOPED_TRACE(Case);
    Fixture F;
    switch (Case) {
    case 0:
      F.word(6, 0x14000001);
      break; // branch starts another block
    case 1:
      F.word(3, 0xf94003f3);
      break; // table reloaded from a frame
    case 2:
      F.word(6, 0x52800014);
      break; // partial overwrite of x20
    case 3:
      F.word(6, 0xd63f0100);
      break; // unknown intervening BLR x8
    }
    F.run();
    ASSERT_TRUE(F.low());
    EXPECT_TRUE(immutableNativeCallTargets(F.Image, *F.low()).empty());
  }
  Fixture F;
  F.word(4, 0x9100a273); // Complete table+40 before a zero-displacement LDR.
  F.word(5, 0xf9400274);
  F.run();
  const auto Targets = immutableNativeCallTargets(F.Image, *F.low());
  ASSERT_EQ(Targets.size(), 1U);
  EXPECT_EQ(Targets.at(Call).Slot, Slot);
}

TEST(ImmutableNativeCalls, BoundsFullWidthCopyChainsAndRejectsMixedRoots) {
  using namespace immutable_native_call_test;
  for (unsigned Copies : {35U, 50U}) {
    SCOPED_TRACE(Copies);
    Fixture F;
    unsigned I = 6;
    for (unsigned Copy = 0; Copy < Copies; ++Copy)
      F.word(I++,
             0xaa1403f4); // MOV X20,X20, each with its original occurrence.
    F.word(I++, 0xd63f0280);
    F.word(I++, 0xa9417bfd);
    F.word(I++, 0xa8c253f3);
    F.word(I++, 0xd65f03c0);
    F.Image.Symbols[0].Size = I * 4;
    F.run();
    ASSERT_TRUE(F.low());
    EXPECT_EQ(immutableNativeCallTargets(F.Image, *F.low()).size(),
              Copies == 35 ? 1U : 0U);
  }
  Fixture F;
  auto Low = *F.low();
  Low.ModuleAnalysisRoots = {Entry, Target};
  EXPECT_TRUE(immutableNativeCallTargets(F.Image, Low).empty());
  Low.ModuleAnalysisRoots.clear();
  Low.OrdinaryModuleAnalysisRoots = {Entry, Target};
  EXPECT_TRUE(immutableNativeCallTargets(F.Image, Low).empty());
}

namespace {
void frameTableFixture(immutable_native_call_test::Fixture &F) {
  // A complete table base survives swift_release in a private frame slot.
  const uint32_t Words[] = {0xd10103ff, 0xa9037bf3, 0xd0000013, 0x91008273,
                            0xf9000bf3, 0x9400003b, 0xf9400bf3, 0xf9400668,
                            0xd63f0100, 0xa9437bf3, 0x910103ff, 0xd65f03c0};
  for (unsigned I = 0; I < std::size(Words); ++I)
    F.word(I, Words[I]);
  F.Image.Symbols[0].Size = sizeof(Words);
  F.run();
}
} // namespace

TEST(ImmutableNativeCalls, RecoversAnAuthenticatedFrameTableDefinition) {
  using namespace immutable_native_call_test;
  Fixture F;
  frameTableFixture(F);
  ASSERT_TRUE(F.low());
  const auto Targets = immutableNativeCallTargets(F.Image, *F.low());
  ASSERT_EQ(Targets.size(), 1U);
  EXPECT_EQ(Targets.at(Entry + 32).Slot, Slot);
  EXPECT_EQ(Targets.at(Entry + 32).Target, Target);
  EXPECT_FALSE(readImmutableImagePointer(F.Image, Slot));
}

TEST(ImmutableNativeCalls, RevalidatesLoopInvariantFrameOriginsAtPublication) {
  using namespace immutable_native_call_test;
  Fixture F;
  loopFrameTableFixture(F);
  ASSERT_TRUE(F.Result.Success) << F.Result.Error;
  ASSERT_TRUE(F.low());
  ASSERT_TRUE(F.high());
  const auto Targets = immutableNativeCallTargets(F.Image, *F.low());
  ASSERT_EQ(Targets.size(), 1U);
  EXPECT_EQ(Targets.at(Entry + 52).Slot, Slot);
  EXPECT_EQ(Targets.at(Entry + 52).Target, Target);
  auto Low = *F.low();
  std::reverse(Low.Blocks.begin(), Low.Blocks.end());
  const auto Reordered = immutableNativeCallTargets(F.Image, Low);
  ASSERT_EQ(Reordered.size(), 1U);
  EXPECT_EQ(Reordered.at(Entry + 52).Target, Target);
  ExprPtr Call;
  walkStmts(F.high()->Body, [&](const HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &E) {
      if (E && E->Kind == ExprKind::Call && E->SourceCallHint &&
          E->SourceCallHint->ImmutableNativeCall)
        Call = E;
    });
  });
  ASSERT_TRUE(Call);
  ASSERT_TRUE(
      objCImmutableNativeSourceCallBound(*Call, F.Image, F.Result, *F.high()));
  // The hint cannot authorize publication after changing any part of the
  // loop, stored bytes, allocation, reload, or original CFG.
  const auto Original = F.Image.Segments[0].Data;
  for (const auto [Index, Word] :
       {std::pair{0U, 0xa9bd53f3U}, std::pair{7U, 0xb90013f3U},
        std::pair{8U, 0xf9000bffU}, std::pair{10U, 0xb5ffffb4U},
        std::pair{11U, 0xf9400ff3U}}) {
    SCOPED_TRACE(Index);
    F.Image.Segments[0].Data = Original;
    F.word(Index, Word);
    EXPECT_TRUE(immutableNativeCallTargets(F.Image, *F.low()).empty());
    EXPECT_FALSE(objCImmutableNativeSourceCallBound(*Call, F.Image, F.Result,
                                                    *F.high()));
  }
  F.Image.Segments[0].Data = Original;
  F.word(8, 0xf9000bff); // Every reached loop iteration overwrites the slot.
  F.run();
  ASSERT_TRUE(F.low());
  EXPECT_TRUE(immutableNativeCallTargets(F.Image, *F.low()).empty());
}

TEST(ImmutableNativeCalls, GeneratedLoopFrameSourceMatchesOriginalARM64) {
#if defined(NEVERD_TEST_CLANG) && defined(__APPLE__) && defined(__aarch64__)
  using namespace immutable_native_call_test;
  Fixture F;
  loopFrameTableFixture(F);
  ASSERT_TRUE(F.high());
  std::map<va_t, const HighFunc *> Functions;
  for (const auto &Function : F.Result.HighFuncs)
    Functions.emplace(Function.Entry, &Function);
  auto Bound =
      bindObjCSourceReferences(*F.high(), F.Image, nullptr, &Functions);
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  elimUnreadPrivateFrameStores(Bound.Function, F.Image.Arch);
  const auto Audit = std::find_if(
      F.Result.FunctionAudits.begin(), F.Result.FunctionAudits.end(),
      [](const auto &A) { return A.Entry == Entry; });
  ASSERT_NE(Audit, F.Result.FunctionAudits.end());
  const auto Allowed = [&](const HighExpr &E) {
    return objcSourceCallBound(E, F.Image, Functions, nullptr, nullptr,
                               &Bound.Function) ||
           objCImmutableNativeSourceCallBound(E, F.Image, F.Result,
                                              Bound.Function);
  };
  const auto Limitation = sourceBodyLimitation(
      Bound.Function, *Bound.Function.SourceTypeHint, &*Audit, Allowed);
  ASSERT_TRUE(Limitation.empty()) << Limitation;
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit({Bound.Function, *Functions.at(Target)}, OS,
                                  Options));
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-frame-loop", Directory));
  const std::filesystem::path Work(Directory.str().str());
  struct Cleanup {
    std::filesystem::path Work;
    ~Cleanup() {
      std::error_code E;
      std::filesystem::remove_all(Work, E);
    }
  } Cleanup{Work};
  const auto Path = (Work / "loop.c").string();
  std::ofstream(Path) << Source << R"(
#include <stdio.h>
extern uint64_t original_loop(uint64_t, uint64_t);
__asm__(".text\n.p2align 2\n.globl _original_loop\n_original_loop:\n"
        ".long 0xa9bc53f3, 0xa9037bf5, 0x92401414, 0xaa0103f5, 0x91000694\n"
        "adrp x19, Lloop_table@PAGE\nadd x19, x19, Lloop_table@PAGEOFF\n"
        ".long 0xf9000bf3, 0x8b1402b5, 0xd1000694\n"
        ".long 0xb5ffffd4, 0xf9400bf3, 0xf9400668, 0xd63f0100\n"
        ".long 0x8b150000, 0xa9437bf5, 0xa8c453f3, 0xd65f03c0\n"
        "Lloop_callee:\n.long 0xd2800540, 0xd65f03c0\n"
        ".section __DATA_CONST,__const\n.p2align 3\n"
        "Lloop_table:\n.quad 0\n.quad Lloop_callee\n.text\n");
int main(void) {
  const uint32_t *machine = (const uint32_t *)(uintptr_t)original_loop;
  const uint32_t words[] = {
    0xa9bc53f3,0xa9037bf5,0x92401414,0xaa0103f5,0x91000694,0xd0000013,
    0x91008273,0xf9000bf3,0x8b1402b5,0xd1000694,0xb5ffffd4,0xf9400bf3,
    0xf9400668,0xd63f0100,0x8b150000,0xa9437bf5,0xa8c453f3,0xd65f03c0,
    0xd2800540,0xd65f03c0};
  for (unsigned i=0;i<20;++i) {
    uint32_t mask=i==5 ? 0x9f00001fU : i==6 ? 0xffc003ffU : UINT32_MAX;
    if ((machine[i]&mask)!=(words[i]&mask)) return 1;
  }
  uint64_t random=UINT64_C(0xfedcba9876543210);
  for (unsigned i=0;i<2048;++i) {
    random=random*UINT64_C(6364136223846793005)+1;
    uint64_t count=i<65 ? i : random, seed=i<4 ? (uint64_t[]){0,1,UINT64_MAX,
      UINT64_C(0x8000000000000000)}[i] : random;
    uint64_t iterations=(count&63)+1;
    uint64_t expected=seed+iterations*(iterations+1)/2+42;
    uint64_t native=original_loop(count,seed), generated=indirect_native(count,seed);
    if (native!=expected || generated!=expected) {
      fprintf(stderr,"loop case %u: native=%llx generated=%llx expected=%llx\n",
        i,(unsigned long long)native,(unsigned long long)generated,
        (unsigned long long)expected); return 2;
    }
  }
  return 0;
}
)";
  for (const auto *Level : {"-O0", "-O2"}) {
    SCOPED_TRACE(Level);
    const auto Output = (Work / (std::string("loop") + Level)).string();
    const std::vector<llvm::StringRef> Args{NEVERD_TEST_CLANG, Level, Path,
                                            "-o", Output};
    ASSERT_EQ(llvm::sys::ExecuteAndWait(NEVERD_TEST_CLANG, Args), 0);
    EXPECT_EQ(llvm::sys::ExecuteAndWait(Output, {Output}), 0) << Source;
  }
#else
  GTEST_SKIP() << "Original ARM64 comparison requires Apple ARM64 and Clang";
#endif
}

TEST(ImmutableNativeCalls, FrameMachineReplaysDirectTailWrapper) {
  using namespace immutable_native_call_test;
  Fixture F;
  // Two independently forwarded address arguments and a real B, which the
  // CFG owner represents as CALL + RETURN without writing the link register.
  const uint32_t Words[] = {0xd0000003, 0x9100a063, 0xd0000004, 0x9100c084,
                            0x1400007c};
  for (unsigned I = 0; I < std::size(Words); ++I)
    F.word(I, Words[I]);
  F.Image.Symbols.front().Size = sizeof(Words);
  F.run();
  ASSERT_TRUE(F.Result.Success);
  ASSERT_TRUE(F.low());
  ASSERT_EQ(F.low()->Blocks.size(), 1U);
  ASSERT_EQ(F.low()->Blocks[0].InstructionBoundaries.back().Control,
            LowInstructionControl::TailCall);
  size_t Budget = 4096;
  EXPECT_TRUE(immutableNativeFrameMachineMatches(F.Image, *F.low(), Budget));
  // Machine equivalence alone grants neither a code-pointer identity nor a
  // current source declaration for the additional forwarded parameters.
  EXPECT_TRUE(immutableNativeCallTargets(F.Image, *F.low()).empty());
}

TEST(ImmutableNativeCalls, TailReplayRejectsChangedMachineAndControlFacts) {
  using namespace immutable_native_call_test;
  for (unsigned Case = 0; Case < 31; ++Case) {
    SCOPED_TRACE(Case);
    Fixture F;
    const uint32_t Words[] = {0xd0000003, 0x9100a063, 0xd0000004, 0x9100c084,
                              0x1400007c};
    for (unsigned I = 0; I < std::size(Words); ++I)
      F.word(I, Words[I]);
    F.Image.Symbols.front().Size = sizeof(Words);
    F.run();
    ASSERT_TRUE(F.low());
    auto Low = *F.low();
    auto &Block = Low.Blocks.front();
    auto &Boundary = Block.InstructionBoundaries.back();
    auto &Call = Block.Ops.at(Boundary.FirstOp);
    auto &Return = Block.Ops.at(Boundary.FirstOp + 1);
    size_t Budget = 4096;
    switch (Case) {
    case 0:
      F.word(4, 0x9400007c);
      break; // BL writes x30, unlike B.
    case 1:
      F.word(4, 0x54000f80);
      break; // Conditional transfer.
    case 2:
      F.word(4, 0xd65f03c0);
      break; // RET is not a direct tail.
    case 3:
      F.word(4, 0xd61f0100);
      break; // Unproven indirect branch.
    case 4:
      F.word(4, 0x1400003c);
      break; // Different destination.
    case 5:
      F.word(0, 0xd0000004);
      break; // Changed forwarded argument.
    case 6:
      Call.Inputs[0].Offset = Veneer;
      break;
    case 7:
      Call.addInput(NdVar::reg(a64reg::X3, 8));
      break;
    case 8:
      Call.Output.Size = 4;
      break;
    case 9:
      Return.Inputs[0] = NdVar::reg(a64reg::X1, 8);
      break;
    case 10:
      ++Call.Seq;
      break;
    case 11:
      Return.Addr += 4;
      break;
    case 12:
      Call.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
      break;
    case 13:
      Call.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 14:
      Boundary.Immediate = Veneer;
      break;
    case 15:
      Boundary.ControlFlags |= LowInstructionControlFlag::NoReturn;
      break;
    case 16:
      Boundary.Control = LowInstructionControl::Call;
      break;
    case 17:
      Boundary.ControlFlags |= LowInstructionControlFlag::Indirect;
      break;
    case 18:
      Block.Succs.push_back(Block.Id);
      break;
    case 19:
      F.Image.Symbols.back().IsFunc = false;
      break;
    case 20:
      // Machine and LowIR agree, but this is an interior instruction rather
      // than an independently authenticated callable entry.
      F.word(4, 0x1400007d);
      Boundary.Immediate = Target + 4;
      Call.Inputs[0].Offset = Target + 4;
      break;
    case 21:
      Block.EndAddr += 4;
      break;
    case 22:
      --Low.LiftedInstructionCount;
      break;
    case 23:
      Low.TruncatedPathAddresses.push_back(Entry);
      break;
    case 24:
      Low.JumpTables.emplace_back();
      break;
    case 25:
      Budget = 5;
      break;
    case 26:
      F.Image.Segments.front().Flags = SegmentFlags::Readable |
                                       SegmentFlags::Writable |
                                       SegmentFlags::Executable;
      break;
    case 27:
      F.word(4, 0x17fffffc);
      Boundary.Immediate = Entry;
      Call.Inputs[0].Offset = Entry;
      break;
    case 28:
      F.word(4, 0x17fffffd);
      Boundary.Immediate = Entry + 4;
      Call.Inputs[0].Offset = Entry + 4;
      F.Image.Symbols.push_back({"internal_label", Entry + 4, 4, true});
      break;
    case 29:
      Return.Output = NdVar::reg(a64reg::X0, 8);
      break;
    case 30:
      Call.Inputs[0].Provenance = ConstantAddressProvenance::DataAddress;
      break;
    }
    EXPECT_FALSE(immutableNativeFrameMachineMatches(F.Image, Low, Budget));
  }
}

TEST(ImmutableNativeCalls, TailSourceMatchesOriginalARM64ForwardedArguments) {
#if defined(NEVERD_TEST_CLANG) && defined(__APPLE__) && defined(__aarch64__)
  using namespace immutable_native_call_test;
  Fixture F;
  const uint32_t Wrapper[] = {0xaa0103e3, 0xaa0203e4, 0xca020002, 0x91000400,
                              0x1400007c};
  const uint32_t Body[] = {0xca010000, 0x8b020000, 0xca030000, 0x8b040000,
                           0xd65f03c0};
  for (unsigned I = 0; I < std::size(Wrapper); ++I)
    F.word(I, Wrapper[I]);
  for (unsigned I = 0; I < std::size(Body); ++I)
    F.word((Target - Entry) / 4 + I, Body[I]);
  F.Image.Symbols.front().Size = sizeof(Wrapper);
  F.Image.Symbols.back().Size = sizeof(Body);
  F.Signature.Parameters.resize(5);
  for (auto &P : F.Signature.Parameters)
    P.Type = NdType::makeInt(8, false);
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(F.Signature, Arch::AArch64, Error));
  F.EntrySignature = F.Signature;
  F.EntrySignature.Parameters.resize(3);
  ASSERT_TRUE(
      assignDarwinScalarSourceABI(F.EntrySignature, Arch::AArch64, Error));
  F.run();
  ASSERT_TRUE(F.Result.Success);
  ASSERT_TRUE(F.low());
  ASSERT_TRUE(F.high());
  size_t Budget = 4096;
  ASSERT_TRUE(immutableNativeFrameMachineMatches(F.Image, *F.low(), Budget));
  std::map<va_t, const HighFunc *> Functions;
  for (const auto &Function : F.Result.HighFuncs)
    Functions.emplace(Function.Entry, &Function);
  const auto Bound =
      bindObjCSourceReferences(*F.high(), F.Image, nullptr, &Functions);
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  const auto Audit = std::find_if(
      F.Result.FunctionAudits.begin(), F.Result.FunctionAudits.end(),
      [](const auto &A) { return A.Entry == Entry; });
  ASSERT_NE(Audit, F.Result.FunctionAudits.end());
  const auto Allowed = [&](const HighExpr &E) {
    return objcSourceCallBound(E, F.Image, Functions);
  };
  EXPECT_TRUE(sourceBodyLimitation(Bound.Function,
                                   *Bound.Function.SourceTypeHint, &*Audit,
                                   Allowed)
                  .empty());
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit({Bound.Function, *Functions.at(Target)}, OS,
                                  Options));
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-tail-replay", Directory));
  const std::filesystem::path Work(Directory.str().str());
  struct Cleanup {
    std::filesystem::path Work;
    ~Cleanup() {
      std::error_code E;
      std::filesystem::remove_all(Work, E);
    }
  } Cleanup{Work};
  const auto Path = (Work / "tail.c").string();
  std::ofstream(Path) << Source << R"(
#include <stdio.h>
extern uint64_t original_tail_wrapper(uint64_t, uint64_t, uint64_t);
__asm__(".text\n.p2align 2\n.globl _original_tail_wrapper\n"
        "_original_tail_wrapper:\n"
        ".long 0xaa0103e3, 0xaa0203e4, 0xca020002, 0x91000400, 0x1400007c\n"
        ".space 0x1ec\n"
        ".long 0xca010000, 0x8b020000, 0xca030000, 0x8b040000, 0xd65f03c0\n");
int main(void) {
  const uint32_t *machine = (const uint32_t *)(uintptr_t)original_tail_wrapper;
  const uint32_t wrapper[] = {0xaa0103e3,0xaa0203e4,0xca020002,0x91000400,0x1400007c};
  const uint32_t body[] = {0xca010000,0x8b020000,0xca030000,0x8b040000,0xd65f03c0};
  for (unsigned i=0;i<5;++i)
    if (machine[i]!=wrapper[i] || machine[128+i]!=body[i]) return 1;
  uint64_t state=UINT64_C(0xfedcba9876543210);
  for (unsigned i=0;i<2048;++i) {
    state=state*UINT64_C(6364136223846793005)+1;
    uint64_t a=i<4 ? (uint64_t[]){0,1,UINT64_MAX,UINT64_C(0x8000000000000000)}[i] : state;
    uint64_t b=~(state>>1), c=(state<<7)^(state>>11);
    uint64_t expected=((((a+1)^b)+(a^c))^b)+c;
    uint64_t native=original_tail_wrapper(a,b,c), generated=indirect_native(a,b,c);
    if (native!=expected || generated!=expected) {
      fprintf(stderr,"case=%u a=%llx b=%llx c=%llx expected=%llx native=%llx generated=%llx\n",
              i,(unsigned long long)a,(unsigned long long)b,(unsigned long long)c,
              (unsigned long long)expected,(unsigned long long)native,(unsigned long long)generated);
      return native!=expected ? 2 : 3;
    }
  }
  return 0;
}
)";
  for (const auto *Level : {"-O0", "-O2"}) {
    SCOPED_TRACE(Level);
    const auto Output = (Work / (std::string("tail") + Level)).string();
    const std::vector<llvm::StringRef> Args{NEVERD_TEST_CLANG, Level, Path,
                                            "-o", Output};
    ASSERT_EQ(llvm::sys::ExecuteAndWait(NEVERD_TEST_CLANG, Args), 0);
    EXPECT_EQ(llvm::sys::ExecuteAndWait(Output, {Output}), 0) << Source;
  }
#else
  GTEST_SKIP() << "Original ARM64 comparison requires Apple ARM64 and Clang";
#endif
}

TEST(ImmutableNativeCalls, FrameTargetsRejectStaleMachineAndLowIRFacts) {
  using namespace immutable_native_call_test;
  for (unsigned Case = 0; Case < 14; ++Case) {
    SCOPED_TRACE(Case);
    Fixture F;
    frameTableFixture(F);
    auto Low = *F.low();
    for (auto &Block : Low.Blocks)
      for (auto &Op : Block.Ops) {
        if (Case == 1 && Op.Addr == Entry + 16 && Op.Opcode == NdOp::STORE)
          Op.Inputs[1] = NdVar::reg(a64reg::X0, 8);
        if (Case == 2 && Op.Addr == Entry + 16 && Op.Opcode == NdOp::INT_ADD)
          Op.Inputs[1] = NdVar::scalar(24, 8);
        if (Op.Addr == Entry + 24 && Op.Opcode == NdOp::LOAD) {
          if (Case == 3)
            Op.Output.Size = 4;
          if (Case == 4)
            Op.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
          if (Case == 5)
            Op.Opcode = NdOp::COPY;
        }
      }
    switch (Case) {
    case 0:
      F.word(4, 0xf9000be0); // Machine stores x0, saved LowIR claims x19.
      break;
    case 6:
      F.word(0, 0xd10083ff); // Changed frame allocation, stale LowIR.
      break;
    case 7:
      F.word(4, 0xb90013f3); // Fresh partial store cannot identify eight bytes.
      break;
    case 8:
      F.word(4, 0xf9000ff3); // Store a different frame slot.
      break;
    case 9:
      F.word(6, 0xf9400ff3); // Load a different frame slot.
      break;
    case 10:
      F.word(5, 0xd63f0140); // Unknown intervening call.
      break;
    case 11:
      F.word(5, 0x910043ff); // Expired frame slot / changed stack base.
      break;
    case 12:
      F.word(4, 0xf9000013); // Table stored externally, not to the frame.
      break;
    case 13:
      F.Image.CodePtrRelocSlots.erase(Slot);
      break;
    default:
      break;
    }
    if (Case >= 7 && Case <= 12) {
      F.run();
      Low = *F.low();
    }
    EXPECT_TRUE(immutableNativeCallTargets(F.Image, Low).empty());
  }
}

TEST(ImmutableNativeCalls, FrameTargetsCheckEveryMachineCFGPath) {
  using namespace immutable_native_call_test;
  for (unsigned Case = 0; Case < 5; ++Case) {
    SCOPED_TRACE(Case);
    Fixture F;
    const uint32_t Words[] = {0xd10103ff, 0xa9037bf3, 0xd0000013, 0x91008273,
                              0xf9000bf3, 0xb4000060, 0x9400003a, 0x14000002,
                              0xd503201f, 0xf9400bf3, 0xf9400668, 0xd63f0100,
                              0xa9437bf3, 0x910103ff, 0xd65f03c0};
    for (unsigned I = 0; I < std::size(Words); ++I)
      F.word(I, Words[I]);
    F.Image.Symbols[0].Size = sizeof(Words);
    if (Case == 1)
      F.word(8, 0xb90013e0); // One reaching arm replaces bytes with w0.
    if (Case == 2)
      F.word(7, 0x17fffffe); // Backedge preserves the original private bytes.
    F.run();
    auto Low = *F.low();
    if (Case == 3) {
      // Keep reciprocal edges but hide the encoded taken arm.
      auto &EntryBlock = Low.Blocks.front();
      ASSERT_EQ(EntryBlock.Succs.size(), 2U);
      const int Removed = EntryBlock.Succs.back();
      EntryBlock.Succs.pop_back();
      for (auto &Block : Low.Blocks)
        if (Block.Id == Removed)
          std::erase(Block.Preds, EntryBlock.Id);
    }
    if (Case == 4)
      std::reverse(Low.Blocks.begin(), Low.Blocks.end());
    const auto Targets = immutableNativeCallTargets(F.Image, Low);
    EXPECT_EQ(Targets.size(), Case == 0 || Case == 2 || Case == 4 ? 1U : 0U);
    if (!Targets.empty())
      EXPECT_EQ(Targets.at(Entry + 44).Target, Target);
  }
}

TEST(ImmutableNativeCalls, FrameCallDependenciesUseCompletedProofRounds) {
  using namespace immutable_native_call_test;
  Fixture F;
  const uint32_t Words[] = {0xd10103ff, 0xa9037bf3, 0xd0000013, 0x91008273,
                            0xf9000bf3, 0xf9400668, 0xd63f0100, 0xf9400bf3,
                            0xf9400668, 0xd63f0100, 0xf9400bf3, 0xf9400668,
                            0xd63f0100, 0xa9437bf3, 0x910103ff, 0xd65f03c0};
  for (unsigned I = 0; I < std::size(Words); ++I)
    F.word(I, Words[I]);
  F.Image.Symbols[0].Size = sizeof(Words);
  F.run();
  std::map<va_t, SourceFunctionTypeHint> Callees{{Target, F.Signature}};
  ASSERT_EQ(immutableNativeCallTargets(F.Image, *F.low()).size(), 1U);
  const auto Targets = immutableNativeCallTargets(F.Image, *F.low(), &Callees);
  ASSERT_EQ(Targets.size(), 3U);
  for (unsigned Offset : {24U, 36U, 48U})
    EXPECT_EQ(Targets.at(Entry + Offset).Target, Target);
  for (unsigned Case = 0; Case < 3; ++Case) {
    SCOPED_TRACE(Case);
    auto Bad = Callees;
    if (Case == 0)
      Bad.at(Target).HasExplicitABI = false;
    if (Case == 1)
      Bad.at(Target).Parameters.push_back({"unbound", NdType::makeInt(8)});
    if (Case == 2)
      Bad.at(Target).Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
    EXPECT_EQ(immutableNativeCallTargets(F.Image, *F.low(), &Bad).size(), 1U);
  }
}

TEST(ImmutableNativeCalls, FrameLoadsKeepBothPairLanesAndRejectEarlierEscape) {
  using namespace immutable_native_call_test;
  for (unsigned Case = 0; Case < 4; ++Case) {
    SCOPED_TRACE(Case);
    Fixture F;
    frameTableFixture(F);
    if (Case < 2) {
      F.word(4, 0xa9014ff3); // stp x19,x19,[sp,#16]
      F.word(6, 0xa94123f3); // ldp x19,x8,[sp,#16]
      if (Case == 1)
        F.word(7, 0xf9400508); // Read the code slot through the second word.
    } else if (Case == 2) {
      // Both possible machine paths still initialize the table, but release
      // would receive a private frame address before the query.
      F.word(5, 0x910003e0); // mov x0,sp
      F.word(6, 0x9400003a); // bl release
      F.word(7, 0xf9400bf3);
      F.word(8, 0xf9400668);
      F.word(9, 0xd63f0100);
    } else {
      F.word(9, 0xd63f0120); // An unknown suffix grants no publication proof.
    }
    F.run();
    const auto Targets = immutableNativeCallTargets(F.Image, *F.low());
    EXPECT_EQ(Targets.size(), Case == 2 ? 0U : 1U);
    if (!Targets.empty())
      EXPECT_EQ(Targets.at(Entry + 32).Target, Target);
  }
}

TEST(ImmutableNativeCalls, FrameQueriesRetainAuthenticatedAccessConditions) {
  using namespace immutable_native_call_test;
  for (unsigned Case = 0; Case < 12; ++Case) {
    SCOPED_TRACE(Case);
    Fixture F;
    F.Image.ImportPtrSlots[0x2000] = "_swift_beginAccess";
    F.Image.DyldBindSlots[0x2000].Name = "_swift_beginAccess";
    F.Image.ImportPtrSlots[0x2008] = "_swift_endAccess";
    F.Image.DyldBindSlots[0x2008] = {
        "_swift_endAccess", 0, "/usr/lib/swift/libswiftCore.dylib", false};
    F.Image.Segments[1].Size = F.Image.Segments[1].FileSz = 16;
    F.Image.Segments[1].Data.resize(16);
    F.Image.Sections[0].Size = F.Image.Sections[0].FileSz = 16;
    F.Image.Symbols.push_back({"_end_access_veneer", Veneer + 12, 12, true});
    F.word((Veneer - Entry) / 4 + 3, 0xb0000010);
    F.word((Veneer - Entry) / 4 + 4, 0xf9400610);
    F.word((Veneer - Entry) / 4 + 5, 0xd61f0200);
    const unsigned Flags[] = {0, 1, 32, 33, 32, 33, 2, 34, 64, 0, 0, 0};
    const uint32_t Words[] = {0xd10103ff,
                              0xa9037bf3,
                              0xd0000013,
                              0x91008273,
                              0xf90013f3,
                              0x910003e1,
                              0xd2800002u | (Flags[Case] << 5),
                              0xd2800003,
                              0x94000038,
                              0x910003e0,
                              0x94000039,
                              0xf94013f3,
                              0xf9400668,
                              0xd63f0100,
                              0xa9437bf3,
                              0x910103ff,
                              0xd65f03c0};
    for (unsigned I = 0; I < std::size(Words); ++I)
      F.word(I, Words[I]);
    F.Image.Symbols[0].Size = sizeof(Words);
    if (Case == 4 || Case == 5)
      F.word(10, 0xd503201f); // Tracked lifetime still open at the query.
    if (Case == 9)
      F.word(8, 0xd503201f); // End without initialized scratch.
    if (Case == 10)
      F.word(5, 0x910043e1); // Scratch overlaps the table's first eight bytes.
    if (Case == 11)
      F.Image.DyldBindSlots.at(0x2000).WeakImport = true;
    F.run();
    const auto Targets = immutableNativeCallTargets(F.Image, *F.low());
    EXPECT_EQ(Targets.size(), Case < 4 ? 1U : 0U);
    if (!Targets.empty())
      EXPECT_EQ(Targets.at(Entry + 52).Target, Target);
  }
}

TEST(ImmutableNativeCalls, PublicationRepeatsTheFrameMachineProof) {
  using namespace immutable_native_call_test;
  for (bool ChangeMachine : {false, true}) {
    SCOPED_TRACE(ChangeMachine);
    Fixture F;
    frameTableFixture(F);
    F.bindCaller();
    ASSERT_TRUE(F.high());
    const auto Expression = immutableNativeExpression(*F.high());
    ASSERT_TRUE(Expression);
    ASSERT_TRUE(objCImmutableNativeSourceCallBound(*Expression, F.Image,
                                                   F.Result, *F.high()));
    if (ChangeMachine) {
      F.word(4, 0xf9000be0);
    } else {
      auto &Low = *const_cast<LowFunc *>(F.low());
      for (auto &Block : Low.Blocks)
        for (auto &Op : Block.Ops)
          if (Op.Addr == Entry + 16 && Op.Opcode == NdOp::STORE)
            Op.Inputs[1] = NdVar::reg(a64reg::X0, 8);
    }
    EXPECT_FALSE(objCImmutableNativeSourceCallBound(*Expression, F.Image,
                                                    F.Result, *F.high()));
  }
}

TEST(ObjCSourceBindings, CFunctionParameterCallRepeatsTheCurrentMachineProof) {
  using namespace c_function_parameter_test;
  Fixture F;
  ASSERT_TRUE(F.Result.Success) << F.Result.Error;
  ASSERT_TRUE(F.high());
  ExprPtr CallExpr;
  size_t StatementIndex = 0;
  for (size_t I = 0; I < F.high()->Body.size(); ++I) {
    const auto &Statement = F.high()->Body[I];
    if (Statement.CallExpr && Statement.CallExpr->SourceCallHint &&
        Statement.CallExpr->SourceCallHint->FunctionParameterCall) {
      CallExpr = Statement.CallExpr;
      StatementIndex = I;
    }
  }
  ASSERT_TRUE(CallExpr);
  EXPECT_FALSE(objcSourceCallBound(*CallExpr, F.Image, {}));
  EXPECT_TRUE(objCCFunctionParameterSourceCallBound(*CallExpr, F.Image,
                                                    F.Result, *F.high()));
  for (unsigned Case = 0; Case < 12; ++Case) {
    SCOPED_TRACE(Case);
    PipelineResult Result;
    Result.Success = F.Result.Success;
    Result.SourceImage = F.Result.SourceImage;
    Result.LowFuncs = F.Result.LowFuncs;
    Result.MedFuncs = F.Result.MedFuncs;
    Result.HighFuncs = F.Result.HighFuncs;
    Result.FunctionAudits = F.Result.FunctionAudits;
    auto Function = *F.high();
    auto Expression = std::make_shared<HighExpr>(*CallExpr);
    auto Hint = *CallExpr->SourceCallHint;
    Function.Body[StatementIndex].CallExpr = Expression;
    switch (Case) {
    case 0:
      Result.SourceImage = nullptr;
      break;
    case 1:
      Result.LowFuncs.push_back(*F.low());
      break;
    case 2:
      for (auto &Audit : Result.FunctionAudits)
        if (Audit.Entry == Entry)
          ++Audit.DecodedInstructions;
      break;
    case 3:
      for (auto &Low : Result.LowFuncs)
        if (Low.Entry == Entry)
          for (auto &Block : Low.Blocks)
            for (auto &Op : Block.Ops)
              if (Op.Addr == Entry + 12)
                Op.Output.Offset = a64reg::X20;
      break;
    case 4:
      Hint.FunctionParameterCall->Site.Instruction += 4;
      break;
    case 5:
      Hint.FunctionParameterCall->Parameter = 0;
      break;
    case 6:
      Expression->IndirectTarget =
          std::make_shared<HighExpr>(*Expression->IndirectTarget);
      Expression->IndirectTarget->Var.Id = 0;
      break;
    case 7:
      Hint.DoesNotReturn = true;
      break;
    case 8:
      Function.Body.push_back(Function.Body[StatementIndex]);
      break;
    case 9:
      for (auto &Med : Result.MedFuncs)
        if (Med.Entry == Entry)
          Med.SourceParametersBound = false;
      break;
    case 10:
      Hint.Signature.Convention = SourceFunctionTypeHint::ConventionKind::Swift;
      break;
    case 11:
      Hint.Signature.Parameters[0].Type = NdType::makeInt(8, false);
      break;
    }
    Expression->SourceCallHint =
        std::make_shared<const SourceCallTypeHint>(std::move(Hint));
    EXPECT_FALSE(objCCFunctionParameterSourceCallBound(*Expression, F.Image,
                                                       Result, Function));
  }
}

TEST(ObjCSourceBindings, RuntimeCFunctionAddressRetainsImportIdentityAndType) {
  using namespace runtime_function_address_test;
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = image(Architecture);
    const auto Hint = runtimeCFunctionAddressHint(Image, Slot);
    ASSERT_TRUE(Hint);
    ASSERT_TRUE(Hint->AddressedFunctionABI);
    EXPECT_EQ(Hint->TargetAddress, Slot);
    EXPECT_EQ(Hint->TargetName, "swift_release");
    EXPECT_EQ(Hint->Signature.ReturnType->Pointee->Kind, NdTypeKind::Func);
    EXPECT_EQ(Hint->AddressedFunctionABI->Convention,
              SourceFunctionTypeHint::ConventionKind::C);
    EXPECT_EQ(Hint->AddressedFunctionABI->ReturnType->Kind, NdTypeKind::Void);
    ASSERT_EQ(Hint->AddressedFunctionABI->Parameters.size(), 1U);
    EXPECT_EQ(Hint->AddressedFunctionABI->Parameters[0].Type->Kind,
              NdTypeKind::Ptr);
    HighFunc Getter;
    Getter.Entry = 0x1000;
    Getter.Name = "runtime_callback";
    Getter.ReturnType = Hint->Signature.ReturnType;
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = HighExpr::makeLoad(
        HighExpr::makeConst(Slot, 8, ConstantAddressProvenance::DataAddress),
        Getter.ReturnType);
    Getter.Body.push_back(Return);
    const auto Projection = bindObjCSourceReferences(Getter, Image);
    ASSERT_TRUE(Projection.Limitation.empty()) << Projection.Limitation;
    const auto &Address = Projection.Function.Body[0].RetVal;
    ASSERT_TRUE(Address);
    ASSERT_EQ(Address->Kind, ExprKind::Call);
    ASSERT_TRUE(Address->SourceCallHint);
    EXPECT_EQ(Address->SourceCallHint->CallKind,
              SourceCallTypeHint::Kind::RuntimeCFunctionAddress);
    EXPECT_TRUE(Address->Operands.empty());
    EXPECT_TRUE(objcSourceCallBound(*Address, Image, {}));
    // Binding changes the clone, and names the loaded value rather than the
    // import cell's storage address.
    EXPECT_EQ(Getter.Body[0].RetVal->Kind, ExprKind::Load);
    Getter.Body[0].RetVal = Getter.Body[0].RetVal->Operands[0];
    EXPECT_FALSE(bindObjCSourceReferences(Getter, Image).Limitation.empty());
    Image.DyldBindSlots[Slot].Module = "/untrusted/libswiftCore.dylib";
    EXPECT_FALSE(objcSourceCallBound(*Address, Image, {}));
  }
}

TEST(ObjCSourceBindings,
     RuntimeCFunctionAddressRejectsStaleOrUnsupportedProofs) {
  using namespace runtime_function_address_test;
  const auto Original = image(Arch::AArch64);
  const auto Hint = runtimeCFunctionAddressHint(Original, Slot);
  ASSERT_TRUE(Hint);
  for (unsigned Case = 0; Case < 13; ++Case) {
    SCOPED_TRACE(Case);
    auto Image = Original;
    switch (Case) {
    case 0:
      Image.DyldBindSlots[Slot].WeakImport = true;
      break;
    case 1:
      Image.DyldBindSlots[Slot].Addend = 4;
      break;
    case 2:
      Image.ImportPtrSlots[Slot] = "_swift_retain";
      break;
    case 3:
      Image.Segments[0].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      break;
    case 4:
      Image.Sections.push_back(Image.Sections[0]);
      break;
    case 5:
      Image.CodePtrRelocSlots.insert(Slot);
      break;
    case 6:
      Image.ConflictingImportStorageSlots.insert(Slot);
      break;
    case 7:
      Image.Format = BinaryFormat::ELF;
      break;
    case 8:
      Image.IsRelocatable = true;
      break;
    case 9:
      Image.ImportPtrSlots[Slot] = Image.DyldBindSlots[Slot].Name =
          "_$s10Foundation3URLV19_bridgeToObjectiveCSo5NSURLCyF";
      Image.DyldBindSlots[Slot].Module =
          "/usr/lib/swift/libswiftFoundation.dylib";
      break;
    case 10:
      Image.ImportPtrSlots[Slot] = Image.DyldBindSlots[Slot].Name = "_printf";
      Image.DyldBindSlots[Slot].Module = "/usr/lib/libSystem.B.dylib";
      break;
    case 11:
    case 12:
      Image.ImportPtrSlots[Slot] = Image.DyldBindSlots[Slot].Name =
          Case == 11 ? "_objc_release_x0" : "_objc_release_x1";
      Image.DyldBindSlots[Slot].Module = "/usr/lib/libobjc.A.dylib";
      break;
    }
    EXPECT_FALSE(runtimeCFunctionAddressHint(Image, Slot));
  }
  auto Address = HighExpr::makeCall({}, 0, {});
  Address->Type = Hint->Signature.ReturnType;
  const auto Valid = std::make_shared<SourceCallTypeHint>(*Hint);
  Address->SourceCallHint = Valid;
  ASSERT_TRUE(objcSourceCallBound(*Address, Original, {}));
  for (unsigned Case = 0; Case < 5; ++Case) {
    SCOPED_TRACE(Case);
    auto Forged = std::make_shared<SourceCallTypeHint>(*Hint);
    Address->SourceCallHint = Forged;
    switch (Case) {
    case 0:
      Forged->AddressedFunctionABI.reset();
      break;
    case 1:
      Forged->AddressedFunctionABI->ReturnType = NdType::makeInt(8, false);
      break;
    case 2:
      Forged->TargetName = "swift_retain";
      break;
    case 3:
      Forged->ReturnedArgument = 0;
      break;
    case 4:
      Forged->CallKind = SourceCallTypeHint::Kind::Native;
      break;
    }
    EXPECT_FALSE(objcSourceCallBound(*Address, Original, {}));
  }
}
TEST(ObjCSourceBindings, NativeTerminationRequiresExactCalleeAndCompleteFlow) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
      SCOPED_TRACE(Mutation);
      BinaryImage Image;
      Image.Arch = Architecture;
      Image.Format = BinaryFormat::MachO;
      Image.Bits = Bitness::Bits64;
      HighFunc Callee;
      Callee.Entry = 0x2000;
      Callee.DoesNotReturn = true;
      Callee.ReturnType = NdType::makeVoid();
      SourceFunctionTypeHint Signature;
      Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      Signature.ReturnType = Callee.ReturnType;
      std::string Error;
      ASSERT_TRUE(assignDarwinScalarSourceABI(Signature, Architecture, Error));
      Callee.SourceTypeHint = Signature;
      HighStmt Trap;
      Trap.Kind = StmtKind::Call;
      Trap.CallExpr = HighExpr::makeCall("trap", 0, {});
      Trap.CallExpr->IntrinsicId =
          Architecture == Arch::AArch64 ? Intrinsic::Brk : Intrinsic::Ud2;
      Callee.Body = {Trap};
      auto Call = HighExpr::makeCall("native_terminator", Callee.Entry, {});
      Call->Type = NdType::makeVoid();
      auto Binding = std::make_shared<SourceCallTypeHint>();
      Binding->TargetAddress = Callee.Entry;
      Binding->Signature = Signature;
      Binding->DoesNotReturn = true;
      Call->SourceCallHint = Binding;
      if (Mutation == 1)
        Callee.DoesNotReturn = false;
      if (Mutation == 2)
        Callee.Body.clear();
      if (Mutation == 3) {
        Callee.Body[0] = HighStmt{};
        Callee.Body[0].Kind = StmtKind::Return;
      }
      if (Mutation == 4)
        Call->CallAddr += 4;
      if (Mutation == 5)
        Call->IsIndirectCall = true;
      if (Mutation == 6)
        Binding->Signature.ReturnType = NdType::makeInt(8);
      EXPECT_EQ(objcSourceCallBound(*Call, Image, {{Callee.Entry, &Callee}}),
                Mutation == 0);
    }
}

namespace {
struct EntryInputFixture {
  BinaryImage Image;
  HighFunc Caller, Callee;
  std::shared_ptr<SourceCallTypeHint> Hint;
  ExprPtr Pointer, Call, Load;
  EntryInputFixture(Arch Architecture = Arch::AArch64, unsigned Width = 4) {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Architecture;
    Image.Bits = Bitness::Bits64;
    Image.ObjCSourceReferences.emplace(
        0x1010, ObjCSourceReference{ObjCSourceReference::Kind::IvarOffset,
                                    0x1010, 8, "field", "Owner"});
    auto Type = NdType::makeInt(Width, false);
    Callee.Entry = 0x2000;
    Callee.Name = "readField";
    Callee.ReturnType = Type;
    Callee.Params = {{"offset", NdType::makePtr(Type)}};
    SourceFunctionTypeHint Signature;
    Signature.ReturnType = Type;
    Signature.Parameters = {{"offset", NdType::makePtr(Type)}};
    std::string Reason;
    if (!assignDarwinScalarSourceABI(Signature, Architecture, Reason))
      ADD_FAILURE() << Reason;
    Callee.SourceTypeHint = Signature;
    Hint = std::make_shared<SourceCallTypeHint>();
    Hint->CallKind = SourceCallTypeHint::Kind::Native;
    Hint->TargetAddress = Callee.Entry;
    Hint->Signature = Signature;
    MedVar Parameter;
    Parameter.Kind = MedVar::Param;
    Parameter.Id = 0;
    Parameter.Size = 8;
    Pointer = HighExpr::makeVar(Parameter, NdType::makeInt(8));
    Load = HighExpr::makeLoad(Pointer, Type);
    MedVar Temporary;
    Temporary.Kind = MedVar::Temp;
    Temporary.Id = 10;
    Temporary.Size = Width;
    auto Local = HighExpr::makeVar(Temporary, Type);
    HighStmt Assignment;
    Assignment.Kind = StmtKind::Assign;
    Assignment.Dst = Local;
    Assignment.Val = Load;
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = Local;
    Callee.Body = {Assignment, Return};
    Call = HighExpr::makeCall(
        "readField", Callee.Entry,
        {HighExpr::makeConst(0x1010, 8,
                             ConstantAddressProvenance::DataAddress)});
    Call->SourceCallHint = Hint;
    Call->Type = Type;
    Return.RetVal = Call;
    Caller.ReturnType = Type;
    Caller.Body = {Return};
  }
  HighFunc project() {
    return snapshotObjCEntryInputs(Caller, Image, {{Callee.Entry, &Callee}});
  }
};

TEST(ObjCSourceBindings, SwiftValueWitnessRequiresCanonicalIndirectCall) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  const std::map<va_t, const HighFunc *> Functions;
  for (const auto Operation :
       {SourceCallTypeHint::SwiftValueWitnessKind::Destroy,
        SourceCallTypeHint::SwiftValueWitnessKind::InitializeWithCopy,
        SourceCallTypeHint::SwiftValueWitnessKind::
            InitializeBufferWithCopyOfBuffer,
        SourceCallTypeHint::SwiftValueWitnessKind::AssignWithCopy,
        SourceCallTypeHint::SwiftValueWitnessKind::InitializeWithTake,
        SourceCallTypeHint::SwiftValueWitnessKind::AssignWithTake,
        SourceCallTypeHint::SwiftValueWitnessKind::GetEnumTagSinglePayload,
        SourceCallTypeHint::SwiftValueWitnessKind::StoreEnumTagSinglePayload}) {
    const auto Hint = swiftValueWitnessSourceCallHint(Image.Arch, Operation);
    ASSERT_TRUE(Hint);
    std::vector<ExprPtr> Arguments;
    for (const auto &Parameter : Hint->Signature.Parameters) {
      auto Argument = HighExpr::makeConst(0, 8);
      Argument->Type = Parameter.Type;
      Arguments.push_back(std::move(Argument));
    }
    auto Call = HighExpr::makeCall("indirect_call", 0, std::move(Arguments));
    Call->IsIndirectCall = true;
    Call->SourceCallHint = std::make_shared<const SourceCallTypeHint>(*Hint);
    EXPECT_TRUE(objcSourceCallBound(*Call, Image, Functions));
    Call->IsIndirectCall = false;
    EXPECT_FALSE(objcSourceCallBound(*Call, Image, Functions));
    Call->IsIndirectCall = true;
    auto Forged = *Hint;
    Forged.Signature.Parameters.back().Location.RegisterOffset += 8;
    Call->SourceCallHint =
        std::make_shared<const SourceCallTypeHint>(std::move(Forged));
    EXPECT_FALSE(objcSourceCallBound(*Call, Image, Functions));
    Forged = *Hint;
    Forged.CallKind = SourceCallTypeHint::Kind::Native;
    Call->SourceCallHint =
        std::make_shared<const SourceCallTypeHint>(std::move(Forged));
    EXPECT_FALSE(objcSourceCallBound(*Call, Image, Functions));
  }
}

TEST(ObjCSourceInputs, EntrySnapshotUsesRuntimeOffsetWithoutChangingNativeABI) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Width : {4U, 8U}) {
      EntryInputFixture F(Architecture, Width);
      ASSERT_TRUE(entryScalarLoadInput(F.Callee, 0));
      auto Projected = F.project();
      ASSERT_EQ(Projected.Body.size(), 2U);
      ASSERT_EQ(Projected.Body[0].Val->Kind, ExprKind::Load);
      EXPECT_EQ(Projected.Body[0].Val->Type->Size, Width);
      const auto Address = Projected.Body[1].RetVal->Operands[0];
      ASSERT_EQ(Address->Kind, ExprKind::Addr);
      EXPECT_TRUE(Address->Operands[0]->structuralEq(*Projected.Body[0].Dst));
      EXPECT_EQ(Projected.Body[1].RetVal->SourceCallHint, F.Hint);
      EXPECT_EQ(F.Caller.Body.size(), 1U);
      EXPECT_EQ(F.Call->Operands[0]->Kind, ExprKind::Const);
      EXPECT_EQ(F.Callee.Body[0].Val, F.Load);
      const auto Bound = bindObjCSourceReferences(Projected, F.Image);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      ASSERT_TRUE(Bound.Function.Body[0].Val->SourceCallHint);
      EXPECT_EQ(Bound.Function.Body[0].Val->SourceCallHint->CallKind,
                SourceCallTypeHint::Kind::RuntimeIvarOffset);
      EXPECT_EQ(Bound.InstanceLayoutClasses, std::set<std::string>{"Owner"});
    }
  }
}

namespace {
struct MergedIvarOffsetFixture {
  BinaryImage Image;
  HighFunc Function;
  ExprPtr First, Second, Load;
  MedVar Selected;
  explicit MergedIvarOffsetFixture(Arch Architecture = Arch::AArch64) {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Architecture;
    Image.Bits = Bitness::Bits64;
    Image.ObjCSourceReferences.emplace(
        0x1100, ObjCSourceReference{ObjCSourceReference::Kind::IvarOffset,
                                    0x1100, 4, "left", "Owner"});
    Image.ObjCSourceReferences.emplace(
        0x1110, ObjCSourceReference{ObjCSourceReference::Kind::IvarOffset,
                                    0x1110, 4, "right", "Owner"});
    Function.ReturnType = NdType::makeInt(4, false);
    Function.Params = {{"choose", NdType::makeInt(4, false)}};
    MedVar Choice;
    Choice.Kind = MedVar::Param;
    Choice.Id = 0;
    Choice.Size = 4;
    Selected.Kind = MedVar::Temp;
    Selected.Id = 20;
    Selected.Size = 8;
    const auto SelectedValue = [&] {
      return HighExpr::makeVar(Selected, NdType::makeInt(8, false));
    };
    First =
        HighExpr::makeConst(0x1100, 8, ConstantAddressProvenance::DataAddress);
    Second =
        HighExpr::makeConst(0x1110, 8, ConstantAddressProvenance::DataAddress);
    HighStmt Choose;
    Choose.Kind = StmtKind::IfElse;
    Choose.Cond = HighExpr::makeVar(Choice, NdType::makeInt(4, false));
    HighStmt Left, Right;
    Left.Kind = Right.Kind = StmtKind::Assign;
    Left.Dst = SelectedValue();
    Left.Val = First;
    Right.Dst = SelectedValue();
    Right.Val = Second;
    Choose.Body = {Left};
    Choose.ElseBody = {Right};
    Load = HighExpr::makeLoad(SelectedValue(), Function.ReturnType);
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = Load;
    Function.Body = {Choose, Return};
  }
};
} // namespace

TEST(ObjCSourceBindings,
     MergedIvarOffsetAddressesBecomeRuntimeValuesBeforeTheMerge) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    MergedIvarOffsetFixture F(Architecture);
    const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    EXPECT_EQ(Bound.InstanceLayoutClasses, std::set<std::string>{"Owner"});
    const auto &If = Bound.Function.Body[0];
    for (const auto *Arm : {&If.Body, &If.ElseBody}) {
      ASSERT_EQ(Arm->size(), 1U);
      const auto Value = Arm->front().Val;
      ASSERT_EQ(Value->Kind, ExprKind::Call);
      ASSERT_TRUE(Value->SourceCallHint);
      EXPECT_EQ(Value->SourceCallHint->CallKind,
                SourceCallTypeHint::Kind::RuntimeIvarOffset);
      EXPECT_TRUE(objcSourceCallBound(*Value, F.Image, {}));
    }
    const auto Value = Bound.Function.Body[1].RetVal;
    ASSERT_EQ(Value->Kind, ExprKind::Cast);
    ASSERT_EQ(Value->Operands.size(), 1U);
    EXPECT_EQ(Value->Type->Size, 4U);
    EXPECT_EQ(Value->Operands[0]->Kind, ExprKind::Var);
  }
}

TEST(ObjCSourceBindings, MergedIvarOffsetProofRejectsMixedAndEscapingUses) {
  for (unsigned Case = 0; Case < 8; ++Case) {
    SCOPED_TRACE(Case);
    MergedIvarOffsetFixture F;
    if (Case == 0)
      F.Image.ObjCSourceReferences.at(0x1110).ClassName = "Other";
    if (Case == 1)
      F.Load->MemoryOrdering = NdMemoryOrdering::Acquire;
    if (Case == 2)
      F.Function.Body[0].ElseBody[0].Val = HighExpr::makeBinop(
          NdOp::INT_ADD, F.Second, HighExpr::makeConst(0, 8));
    if (Case == 3) {
      HighStmt Escape;
      Escape.Kind = StmtKind::ExprStmt;
      Escape.Val = HighExpr::makeVar(F.Selected, NdType::makeInt(8, false));
      F.Function.Body.insert(F.Function.Body.end() - 1, Escape);
    }
    if (Case == 4) {
      HighStmt Extra;
      Extra.Kind = StmtKind::ExprStmt;
      Extra.Val = HighExpr::makeLoad(
          HighExpr::makeVar(F.Selected, NdType::makeInt(8, false)),
          NdType::makeInt(4, false));
      F.Function.Body.insert(F.Function.Body.end() - 1, Extra);
    }
    if (Case == 5)
      F.Image.ObjCSourceReferences.at(0x1110).Size = 8;
    if (Case == 6)
      F.Image.ObjCSourceReferences.erase(0x1110);
    if (Case == 7) {
      HighStmt Store;
      Store.Kind = StmtKind::Store;
      Store.StoreAddr =
          HighExpr::makeVar(F.Selected, NdType::makeInt(8, false));
      Store.StoreVal = HighExpr::makeConst(0, 4);
      F.Function.Body.insert(F.Function.Body.end() - 1, Store);
    }
    const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_TRUE(Bound.InstanceLayoutClasses.empty());
    EXPECT_EQ(Bound.Function.Body.back().RetVal->Kind, ExprKind::Load);
  }
}

TEST(ObjCSourceInputs, EntryReadRejectsEffectsEscapesControlAndMalformedUses) {
  for (unsigned Case = 0; Case < 22; ++Case) {
    SCOPED_TRACE(Case);
    EntryInputFixture F;
    switch (Case) {
    case 0:
      F.Callee.Body[1].RetVal = F.Load;
      break;
    case 1:
      F.Callee.Body[1].RetVal = F.Pointer;
      break;
    case 2:
      F.Callee.Body[1].Kind = StmtKind::Store;
      F.Callee.Body[1].StoreAddr = F.Pointer;
      F.Callee.Body[1].RetVal.reset();
      F.Callee.Body[1].StoreVal = HighExpr::makeConst(0, 4);
      break;
    case 3:
      F.Callee.Body.insert(F.Callee.Body.begin(), F.Callee.Body.back());
      break;
    case 4:
      F.Callee.Body[1].Kind = StmtKind::Goto;
      F.Callee.Body[1].GotoTarget = F.Callee.Entry;
      break;
    case 5:
      F.Callee.Body[1].Kind = StmtKind::If;
      break;
    case 6:
      F.Callee.Body[1].Body = {F.Callee.Body.front()};
      break;
    case 7:
      F.Callee.UnstructuredExceptionRegions = 1;
      break;
    case 8:
      F.Load->MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 9:
      F.Pointer->Var.SSAVer = 1;
      break;
    case 10:
      F.Pointer->Var.RenameTag = 3;
      break;
    case 11:
      F.Pointer->Type = NdType::makeInt(4);
      break;
    case 12:
      F.Load->Type = NdType::makeFloat(4);
      break;
    case 13:
      F.Callee.Body[0].Dst = F.Pointer;
      break;
    case 14:
      F.Callee.Body.insert(F.Callee.Body.begin(), F.Callee.Body.front());
      F.Callee.Body[0].Val = HighExpr::makeCall("effect", 0x3000, {});
      break;
    case 15:
      F.Callee.Body[1].RetVal =
          HighExpr::makeCall("escape", 0x3000, {F.Pointer});
      break;
    case 16:
      F.Load->Operands[0] = HighExpr::makeBinop(NdOp::INT_ADD, F.Pointer,
                                                HighExpr::makeConst(4, 8));
      break;
    case 17:
      F.Callee.Body.resize(257);
      break;
    case 18:
      F.Callee.Body.insert(F.Callee.Body.begin(), F.Callee.Body.front());
      F.Callee.Body[0].Kind = StmtKind::Nop;
      F.Callee.Body[0].Dst.reset();
      F.Callee.Body[0].Val = HighExpr::makeCall("effect", 0x3000, {});
      break;
    case 19:
      F.Callee.Body[1].RetVal = HighExpr::makeCall("effects", 0x3000, {});
      F.Callee.Body[1].RetVal->IntrinsicOutputs = {F.Pointer->Var};
      break;
    case 20:
      F.Callee.Body[1].RetVal = HighExpr::makeCall(
          "large", 0x3000,
          std::vector<ExprPtr>(4096, HighExpr::makeConst(0, 8)));
      break;
    case 21:
      F.Callee.Params[0].Type = NdType::makeInt(8);
      break;
    }
    EXPECT_FALSE(entryScalarLoadInput(F.Callee, 0));
    EXPECT_EQ(F.project().Body.size(), 1U);
  }
}

TEST(ObjCSourceInputs, SnapshotRequiresExactTargetABIStorageAndArgumentOrder) {
  for (unsigned Case = 0; Case < 18; ++Case) {
    SCOPED_TRACE(Case);
    EntryInputFixture F;
    auto Argument = F.Call->Operands[0];
    switch (Case) {
    case 0:
      F.Call->CallAddr += 4;
      break;
    case 1:
      F.Hint->Signature.ReturnLocation.RegisterOffset += 8;
      break;
    case 2:
      F.Call->IsIndirectCall = true;
      break;
    case 3:
      F.Hint->CallKind = SourceCallTypeHint::Kind::ObjCMessage;
      break;
    case 4:
      Argument->ConstProvenance = ConstantAddressProvenance::Scalar;
      break;
    case 5:
      Argument->ConstProvenance = ConstantAddressProvenance::Unknown;
      break;
    case 6:
      Argument->ConstProvenance = ConstantAddressProvenance::CodeAddress;
      break;
    case 7:
      Argument->AddressOwnerVA = 0x1000;
      break;
    case 8:
      Argument->Type = NdType::makeInt(4);
      break;
    case 9:
      F.Image.ObjCSourceReferences.at(0x1010).Size = 2;
      break;
    case 10:
      F.Image.ObjCSourceReferences.at(0x1010).TheKind =
          ObjCSourceReference::Kind::Class;
      break;
    case 11:
      F.Image.ObjCSourceReferences.clear();
      break;
    case 12:
      F.Caller.Body[0].MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 13:
      F.Caller.Body[0].Kind = StmtKind::While;
      break;
    case 14:
      F.Call->Operands[0] = HighExpr::makeCall("effect", 0x3000, {});
      break;
    case 15:
      F.Callee.SourceTypeHint.reset();
      break;
    case 16:
      F.Callee.Params[0].Name = "different";
      break;
    case 17:
      F.Callee.Params[0].Type = NdType::makePtr(NdType::makeInt(8));
      break;
    }
    EXPECT_EQ(F.project().Body.size(), 1U);
    EXPECT_EQ(F.Caller.Body[0].RetVal, F.Call);
  }
}

struct Fixture {
  BinaryImage Image;
  HighFunc Function;
  Fixture() {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Segment Mapping;
    Mapping.VA = 0x1000;
    Mapping.Size = Mapping.FileSz = 0x100;
    Mapping.Flags = SegmentFlags::Readable;
    Mapping.Data.resize(0x100);
    Image.Segments.push_back(Mapping);
    Section Data;
    Data.VA = 0x1000;
    Data.Size = Data.FileSz = 0x100;
    Data.Flags = SegmentFlags::Readable;
    Image.Sections.push_back(Data);
    Image.ObjCSourceReferences.emplace(
        0x1010,
        ObjCSourceReference{
            ObjCSourceReference::Kind::Selector, 0x1010, 8, "step:", {}});
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal =
        HighExpr::makeLoad(HighExpr::makeConst(0x1010, 8), NdType::makeInt(8));
    Function.Body.push_back(Return);
  }
};

TEST(ObjCSourceBindings, ZeroBasedDylibWideScalarRequiresExactOccurrence) {
  using P = ConstantAddressProvenance;
  for (unsigned Mutation = 0; Mutation < 19; ++Mutation) {
    SCOPED_TRACE(Mutation);
    Fixture F;
    F.Image.MachOIsDylib = true;
    Segment Code;
    Code.VA = 0x2000;
    Code.Size = Code.FileSz = 8;
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Code.Data.resize(8);
    const uint32_t Low = 0x52800000u | (0x1040u << 5) | 9u;
    const uint32_t High = 0x72800000u | (1u << 21) | 9u;
    llvm::support::endian::write32le(Code.Data.data(), Low);
    llvm::support::endian::write32le(Code.Data.data() + 4, High);
    F.Image.Segments.push_back(Code);
    Section Text;
    Text.VA = 0x2000;
    Text.Size = Text.FileSz = 8;
    Text.Flags = Code.Flags;
    F.Image.Sections.push_back(Text);

    F.Function.Entry = 0x2000;
    HighStmt Assign;
    Assign.Kind = StmtKind::Assign;
    Assign.Addr = 0x2004;
    MedVar Temp;
    Temp.Kind = MedVar::Temp;
    Temp.Id = 1;
    Temp.Size = 8;
    Assign.Dst = HighExpr::makeVar(Temp, NdType::makeInt(8));
    Assign.Val = HighExpr::makeConst(0x1040, 8);
    F.Function.Body = {Assign};

    switch (Mutation) {
    case 1:
      F.Image.MachOIsDylib = false;
      break;
    case 2:
      F.Image.Base = 0x1000;
      break;
    case 3:
      F.Image.Segments.back().Data[4] ^= 0x80;
      break;
    case 4:
      F.Image.Segments.back().Data[4] ^= 1;
      break;
    case 5:
      F.Image.Segments.back().Data[6] ^= 0x20;
      break;
    case 6:
      F.Function.Body[0].Val->ConstVal = 0x1041;
      break;
    case 7:
      F.Function.Body[0].Val->ConstProvenance = P::DataAddress;
      break;
    case 8:
      F.Function.Body[0].Val->AddressOwnerVA = 0x1000;
      break;
    case 9:
      F.Image.ObjectRelocationWriteBytes.emplace();
      F.Image.ObjectRelocationWriteBytes->insert(0x2001);
      break;
    case 10:
      F.Image.Sections.back().Flags = SegmentFlags::Readable;
      break;
    case 11:
      F.Function.Entry = 0x2004;
      break;
    case 12:
      F.Function.Body[0].Val->Type = NdType::makePtr(NdType::makeVoid());
      break;
    case 13:
      F.Image.Segments.back().Data[3] |= 0x80;
      F.Image.Segments.back().Data[7] |= 0x80;
      break;
    case 14:
      F.Image.MachOChainedFixupsAmbiguous = true;
      break;
    case 15:
      F.Image.Segments.back().Flags = SegmentFlags::Readable;
      break;
    case 16:
      F.Image.MachOResolvedChainedPointerSlots.insert(0x1ffc);
      break;
    case 17:
      F.Function.Body[0].Dst = HighExpr::makeConst(1, 8);
      break;
    case 18:
      F.Function.Body[0].Val->MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    default:
      break;
    }
    const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_EQ(Bound.Limitation.empty(), Mutation == 0) << Bound.Limitation;
    if (Mutation == 0) {
      ASSERT_EQ(Bound.Function.Body[0].Val->Kind, ExprKind::Const);
      EXPECT_EQ(Bound.Function.Body[0].Val->ConstVal, 0x1040u);
      EXPECT_EQ(Bound.Function.Body[0].Val->ConstProvenance, P::Scalar);
      EXPECT_EQ(F.Function.Body[0].Val->ConstProvenance, P::Unknown);
    }
  }
}

TEST(ObjCSourceBindings,
     AuthenticatedSwiftIntegerCallKeepsAddressShapedScalarLiteral) {
  for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    Fixture F;
    F.Image.ObjCSourceReferences.clear();
    auto Literal = HighExpr::makeConst(0x1040, 8);
    auto Binding = std::make_shared<SourceCallTypeHint>();
    Binding->CallKind = SourceCallTypeHint::Kind::Native;
    Binding->TargetAddress = 0x2000;
    auto &Signature = Binding->Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
    Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
    Signature.Parameters = {{"color", NdType::makeInt(8, true)},
                            {"self", NdType::makePtr(NdType::makeVoid())}};
    Signature.Parameters[1].TheRole =
        SourceParameterTypeHint::Role::SwiftContext;
    std::string Error;
    ASSERT_TRUE(assignDarwinSwiftSourceABI(Signature, F.Image.Arch, Error))
        << Error;
    auto Call = HighExpr::makeCall("color_init", 0x2000,
                                   {Literal, HighExpr::makeConst(0, 8)});
    Call->Type = Signature.ReturnType;
    Call->SourceCallHint = Binding;
    F.Function.ReturnType = Signature.ReturnType;
    F.Function.Body[0].RetVal = Call;
    if (Mutation == 1)
      Literal->ConstProvenance = ConstantAddressProvenance::Scalar;
    if (Mutation == 2)
      Literal->ConstProvenance = ConstantAddressProvenance::DataAddress;
    if (Mutation == 3)
      Literal->AddressOwnerVA = 0x1040;
    if (Mutation == 4)
      Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    if (Mutation == 5)
      Signature.Parameters[0].Type = NdType::makePtr(NdType::makeVoid());
    if (Mutation == 6)
      Call->CallAddr += 4;
    const auto Result = bindObjCSourceReferences(F.Function, F.Image);
    if (Mutation < 2) {
      EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
      ASSERT_EQ(Result.Function.Body[0].RetVal->Operands.size(), 2U);
      EXPECT_EQ(Result.Function.Body[0].RetVal->Operands[0]->ConstVal, 0x1040U);
    } else {
      EXPECT_FALSE(Result.Limitation.empty());
    }
  }
}

TEST(ObjCSourceBindings,
     BoundIntegerMessagePreservesOnlyProvenScalarCollision) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  Segment Mapping;
  Mapping.VA = 0x1000;
  Mapping.Size = Mapping.FileSz = 0x2000;
  Mapping.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Mapping.Data.resize(0x2000);
  Image.Segments.push_back(Mapping);
  Section Text;
  Text.Name = "__objc_stubs";
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x1000;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Image.Sections.push_back(Text);
  Section Data;
  Data.VA = 0x2000;
  Data.FileOff = 0x1000;
  Data.Size = Data.FileSz = 0x1000;
  Data.Flags = SegmentFlags::Readable;
  Image.Sections.push_back(Data);
  Image.ImportPtrSlots[0x2180] = "_objc_msgSend";
  Image.ObjCSourceReferences[0x2100] = {ObjCSourceReference::Kind::Selector,
                                        0x2100,
                                        8,
                                        "setAnimationOptions:",
                                        {}};
  ObjCClass Class;
  Class.Name = "Transition";
  Class.RootClass = true;
  Class.InheritanceStatus = "root";
  Image.ObjCClasses.push_back(Class);
  ObjCMethod Method;
  Method.ClassName = Class.Name;
  Method.Selector = "setAnimationOptions:";
  Method.Implementation = 0x1200;
  Method.TypeEncoding = "v24@0:8Q16";
  Method.TypeHint =
      parseObjCMethodEncoding(Method.Selector, Method.TypeEncoding);
  ASSERT_TRUE(Method.TypeHint);
  Image.ObjCMethods.push_back(Method);
  const uint32_t Stub[] = {0xb0000001, 0xf9408021, 0xb0000010, 0xf940c210,
                           0xd61f0200};
  for (size_t I = 0; I < 5; ++I)
    llvm::support::endian::write32le(
        Image.Segments[0].Data.data() + 0x100 + I * 4, Stub[I]);
  const auto Receiver =
      objcMethodReceiverTypeHint(Image, Method.Implementation);
  ASSERT_TRUE(Receiver);
  const auto Declaration =
      objcReceiverSourceTypeHint(Image, Method.Selector, *Receiver);
  ASSERT_TRUE(Declaration.Signature);
  ASSERT_TRUE(objcSelectorStubMatches(Image, 0x1100, 0x2100, Method.Selector));

  SourceCallTypeHint Binding;
  Binding.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Binding.TargetAddress = 0x1100;
  Binding.TargetName = "objc_msgSend";
  Binding.Selector = Method.Selector;
  Binding.SelectorReferenceAddress = 0x2100;
  Binding.Receiver = *Receiver;
  Binding.Signature = *Declaration.Signature;
  for (unsigned Mutation = 0; Mutation < 11; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Current = Binding;
    auto Literal =
        HighExpr::makeConst(0x1040, 8, ConstantAddressProvenance::Scalar);
    auto Call = HighExpr::makeCall(
        "objc_msgSend", 0x1100,
        {HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8), Literal});
    Call->Type = NdType::makeVoid();
    switch (Mutation) {
    case 1:
      Literal->ConstProvenance = ConstantAddressProvenance::Unknown;
      break;
    case 2:
      Literal->ConstProvenance = ConstantAddressProvenance::DataAddress;
      break;
    case 3:
      Literal->AddressOwnerVA = 0x1000;
      break;
    case 4:
      Current.Signature.Parameters[2].Type =
          NdType::makePtr(NdType::makeVoid());
      break;
    case 5:
      Current.TargetAddress = Call->CallAddr = 0x1104;
      break;
    case 6:
      Current.Selector = "other:";
      break;
    case 7:
      Current.Signature.Origin =
          SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      break;
    case 8:
      Call->IsIndirectCall = true;
      break;
    case 9:
      Literal->Type = NdType::makeInt(4, false);
      break;
    case 10:
      Current.Receiver.reset();
      break;
    }
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Current);
    HighFunc Function;
    Function.Entry = Method.Implementation;
    Function.ReturnType = NdType::makeVoid();
    HighStmt Statement;
    Statement.Kind = StmtKind::ExprStmt;
    Statement.Val = Call;
    Function.Body.push_back(Statement);
    const auto Bound = bindObjCSourceReferences(Function, Image);
    EXPECT_EQ(Bound.Limitation.empty(), Mutation == 0) << Bound.Limitation;
    EXPECT_EQ(Bound.Function.Body[0].Val->Operands[2]->Kind, ExprKind::Const);
  }
}

TEST(ObjCSourceBindings,
     IntegerStorePreservesOnlyProvenScalarAddressCollision) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  Segment Mapping;
  Mapping.VA = 0x1000;
  Mapping.Size = Mapping.FileSz = 0x100;
  Mapping.Flags = SegmentFlags::Readable;
  Mapping.Data.resize(0x100);
  Image.Segments.push_back(Mapping);
  Section Data;
  Data.VA = 0x1000;
  Data.Size = Data.FileSz = 0x100;
  Data.Flags = SegmentFlags::Readable;
  Image.Sections.push_back(Data);
  for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    HighFunc Function;
    Function.ReturnType = NdType::makeVoid();
    MedVar Destination;
    Destination.Kind = MedVar::Param;
    Destination.Id = 0;
    Destination.Size = 8;
    HighStmt Store;
    Store.Kind = StmtKind::Store;
    Store.StoreAddr = HighExpr::makeVar(
        Destination, NdType::makePtr(NdType::makeInt(4, false)));
    Store.StoreVal =
        HighExpr::makeConst(0x1010, 4, ConstantAddressProvenance::Scalar);
    if (Mutation == 1)
      Store.StoreVal->ConstProvenance = ConstantAddressProvenance::Unknown;
    if (Mutation == 2)
      Store.StoreVal->ConstProvenance = ConstantAddressProvenance::Address;
    if (Mutation == 3)
      Store.StoreVal->AddressOwnerVA = 0x1000;
    if (Mutation == 4)
      Store.StoreVal->Type = NdType::makePtr(NdType::makeVoid());
    if (Mutation == 5)
      Store.StoreVal->Type = NdType::makeInt(8, false);
    Function.Body.push_back(std::move(Store));
    const auto Bound = bindObjCSourceReferences(Function, Image);
    EXPECT_EQ(Bound.Limitation.empty(), Mutation == 0) << Bound.Limitation;
    EXPECT_EQ(Bound.Function.Body[0].StoreVal->Kind, ExprKind::Const);
  }
}

struct SwiftTypeMetadataFixture {
  BinaryImage Image;
  HighFunc Function;
  static constexpr va_t Reference = 0x1020;
  static constexpr va_t TypeReference = 0x1080;
  static constexpr va_t Cache = 0x2020;
  static constexpr va_t DescriptorSlot = 0x3020;
  static constexpr va_t NestedDescriptorSlot = 0x3028;
  static constexpr va_t LocalDescriptor = 0x4020;

  explicit SwiftTypeMetadataFixture(
      Arch Architecture, bool LocalProtocol = false, bool LeadingValue = false,
      bool CombineNominal = false, bool LocalNominal = false,
      bool NestedLocalNominal = false, bool NestedFoundationNominal = false) {
    EXPECT_LE(unsigned(LocalProtocol) + unsigned(CombineNominal) +
                  unsigned(LocalNominal) + unsigned(NestedLocalNominal) +
                  unsigned(NestedFoundationNominal),
              1U);
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Architecture;
    Image.Bits = Bitness::Bits64;
    Image.MachOTwoLevelNamespace = true;
    auto AddMapping = [&](llvm::StringRef Name, va_t Address,
                          SegmentFlags Flags, bool ReadOnlyAfterRelocations) {
      Segment Mapping;
      Mapping.Name = Name.str();
      Mapping.VA = Mapping.FileOff = Address;
      Mapping.Size = Mapping.FileSz = 0x100;
      Mapping.Flags = Flags;
      Mapping.ReadOnlyAfterRelocations = ReadOnlyAfterRelocations;
      Mapping.Data.resize(0x100);
      Image.Segments.push_back(Mapping);
      Section Data;
      Data.Name = Name.str();
      Data.SegmentName = Name.str();
      Data.VA = Data.FileOff = Address;
      Data.Size = Data.FileSz = 0x100;
      Data.Flags = Flags;
      Image.Sections.push_back(Data);
    };
    AddMapping("__swift_reference", 0x1000, SegmentFlags::Readable, false);
    // The loader preserves the executable __TEXT flags on __const and
    // __swift5_typeref. Mach-O section attributes remain the authoritative
    // code/data distinction.
    Image.Segments[0].Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Image.Sections[0].Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    AddMapping("__swift_cache", 0x2000,
               SegmentFlags::Readable | SegmentFlags::Writable, false);
    AddMapping("__swift_import", 0x3000,
               SegmentFlags::Readable | SegmentFlags::Writable, true);
    if (LocalProtocol || LocalNominal || NestedLocalNominal)
      AddMapping("__swift_descriptor", 0x4000, SegmentFlags::Readable, false);
    auto &ReferenceData = Image.Segments[0].Data;
    llvm::support::endian::write32le(
        ReferenceData.data() + Reference - 0x1000,
        static_cast<uint32_t>(static_cast<int32_t>(TypeReference - Reference)));
    const bool Nested = NestedLocalNominal || NestedFoundationNominal;
    const llvm::StringRef Suffix = LocalProtocol    ? "_pSg"
                                   : CombineNominal ? "ySbG"
                                                    : "Sg";
    const llvm::StringRef NestedSuffix = NestedLocalNominal ? "SgG" : "G";
    const uint32_t Length =
        Nested ? 5 + 1 + 5 + NestedSuffix.size() : 5 + Suffix.size();
    llvm::support::endian::write32le(
        ReferenceData.data() + Reference + 4 - 0x1000, Length);
    auto *Type = ReferenceData.data() + TypeReference - 0x1000;
    Type[0] = 2;
    llvm::support::endian::write32le(
        Type + 1, static_cast<uint32_t>(static_cast<int32_t>(
                      DescriptorSlot - (TypeReference + 1))));
    if (Nested) {
      Type[5] = 'y';
      Type[6] = 2;
      llvm::support::endian::write32le(
          Type + 7, static_cast<uint32_t>(static_cast<int32_t>(
                        NestedDescriptorSlot - (TypeReference + 7))));
      std::memcpy(Type + 11, NestedSuffix.data(), NestedSuffix.size());
      Type[11 + NestedSuffix.size()] = 0;
      const std::string Base =
          NestedLocalNominal ? "_$"
                               "s7Combine9PublishedVy7WMFData33WMFDonationRemin"
                               "derDataControllerC20ExperimentAssignmentOSgG"
                             : "_$s7Combine9PublishedVy10Foundation4DateVG";
      Image.Symbols.push_back({Base + "MR", Reference, 8, false});
      Image.Symbols.push_back({Base + "Md", Cache, 8, false});
      EXPECT_TRUE(Image.recordDyldBindSlot(
          DescriptorSlot, "_$s7Combine9PublishedVMn", 0,
          "/System/Library/Frameworks/Combine.framework/Combine", false));
      if (NestedLocalNominal) {
        constexpr llvm::StringLiteral Descriptor =
            "_$"
            "s7WMFData33WMFDonationReminderDataControllerC20ExperimentAssignmen"
            "tOMn";
        Image.Symbols.push_back({Descriptor.str(), LocalDescriptor, 0, false});
        Image.Exports.push_back({Descriptor.str(), 0, LocalDescriptor});
        llvm::support::endian::write64le(Image.Segments[2].Data.data() +
                                             NestedDescriptorSlot - 0x3000,
                                         LocalDescriptor);
        Image.DataPtrRelocSlots.insert(NestedDescriptorSlot);
        Image.DataPtrRelocTargetOwners[NestedDescriptorSlot] = 0x4000;
        Image.MachOResolvedChainedPointerSlots.insert(NestedDescriptorSlot);
      } else {
        EXPECT_TRUE(Image.recordDyldBindSlot(
            NestedDescriptorSlot, "_$s10Foundation4DateVMn", 0,
            "/System/Library/Frameworks/Foundation.framework/Foundation",
            false));
      }
    } else {
      std::memcpy(Type + 5, Suffix.data(), Suffix.size());
      Type[5 + Suffix.size()] = 0;
    }
    if (!Nested && (LocalProtocol || LocalNominal)) {
      const std::string Base = LocalNominal
                                   ? "_$s7WMFData24WMFFeatureConfigResponseVSg"
                                   : "_$s7WMFData10WMFService_pSg";
      const std::string Descriptor =
          LocalNominal ? "_$s7WMFData24WMFFeatureConfigResponseVMn"
                       : "_$s7WMFData10WMFServiceMp";
      Image.Symbols.push_back({Base + "MR", Reference, 8, false});
      Image.Symbols.push_back({Base + "Md", Cache, 8, false});
      Image.Symbols.push_back({Descriptor, LocalDescriptor, 0, false});
      Image.Exports.push_back({Descriptor, 0, LocalDescriptor});
      llvm::support::endian::write64le(Image.Segments[2].Data.data() +
                                           DescriptorSlot - 0x3000,
                                       LocalDescriptor);
      Image.DataPtrRelocSlots.insert(DescriptorSlot);
      Image.DataPtrRelocTargetOwners[DescriptorSlot] = 0x4000;
      Image.MachOResolvedChainedPointerSlots.insert(DescriptorSlot);
    } else if (!Nested && CombineNominal) {
      Image.Symbols.push_back(
          {"_$s7Combine9PublishedVySbGMR", Reference, 8, false});
      Image.Symbols.push_back(
          {"_$s7Combine9PublishedVySbGMd", Cache, 8, false});
      EXPECT_TRUE(Image.recordDyldBindSlot(
          DescriptorSlot, "_$s7Combine9PublishedVMn", 0,
          "/System/Library/Frameworks/Combine.framework/Combine", false));
    } else if (!Nested) {
      Image.Symbols.push_back(
          {"_$s10Foundation3URLVSgMR", Reference, 8, false});
      Image.Symbols.push_back({"_$s10Foundation3URLVSgMd", Cache, 8, false});
      EXPECT_TRUE(Image.recordDyldBindSlot(
          DescriptorSlot, "_$s10Foundation3URLVMn", 0,
          "/System/Library/Frameworks/Foundation.framework/Foundation", false));
    }

    SourceFunctionTypeHint Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"cache", NdType::makePtr(NdType::makeVoid())},
                            {"reference", NdType::makePtr(NdType::makeVoid())}};
    if (LeadingValue)
      Signature.Parameters.insert(
          Signature.Parameters.begin(),
          {"value", NdType::makePtr(NdType::makeVoid())});
    std::string Error;
    EXPECT_TRUE(assignDarwinScalarSourceABI(Signature, Architecture, Error))
        << Error;
    auto Hint = std::make_shared<SourceCallTypeHint>();
    Hint->CallKind = SourceCallTypeHint::Kind::Native;
    Hint->TargetAddress = 0x5000;
    Hint->Signature = Signature;
    std::vector<ExprPtr> Arguments{
        HighExpr::makeConst(Cache, 8, ConstantAddressProvenance::DataAddress),
        HighExpr::makeConst(Reference, 8,
                            ConstantAddressProvenance::DataAddress)};
    if (LeadingValue) {
      auto Value = HighExpr::makeConst(0, 8);
      Value->Type = NdType::makePtr(NdType::makeVoid());
      Arguments.insert(Arguments.begin(), std::move(Value));
    }
    auto Call =
        HighExpr::makeCall("metadata_user", 0x5000, std::move(Arguments));
    Call->Type = NdType::makeVoid();
    Call->SourceCallHint = std::move(Hint);
    HighStmt Statement;
    Statement.Kind = StmtKind::ExprStmt;
    Statement.Val = std::move(Call);
    Function.ReturnType = NdType::makeVoid();
    Function.Body = {std::move(Statement)};
  }
};

SwiftTypeMetadataFixture swiftStdlibTypeMetadataFixture() {
  SwiftTypeMetadataFixture F(Arch::AArch64);
  F.Image.ImportPtrSlots.clear();
  F.Image.ImportStorageSlots.clear();
  F.Image.DyldBindSlots.clear();
  EXPECT_TRUE(
      F.Image.recordDyldBindSlot(SwiftTypeMetadataFixture::DescriptorSlot,
                                 "_$ss23_ContiguousArrayStorageCMn", 0,
                                 "/usr/lib/swift/libswiftCore.dylib", false));
  F.Image.Symbols[0].Name = "_$ss23_ContiguousArrayStorageCyypGMR";
  F.Image.Symbols[1].Name = "_$ss23_ContiguousArrayStorageCyypGMd";
  auto &Data = F.Image.Segments[0].Data;
  llvm::support::endian::write32le(
      Data.data() + SwiftTypeMetadataFixture::Reference + 4 - 0x1000, 9);
  std::memcpy(Data.data() + SwiftTypeMetadataFixture::TypeReference + 5 -
                  0x1000,
              "yypG", 5);
  return F;
}

SwiftTypeMetadataFixture swiftDispatchTypeMetadataFixture(bool TimerFlags) {
  SwiftTypeMetadataFixture F(Arch::AArch64);
  F.Image.ImportPtrSlots.clear();
  F.Image.ImportStorageSlots.clear();
  F.Image.DyldBindSlots.clear();
  const std::string Descriptor =
      TimerFlags ? "_$sSo18OS_dispatch_sourceC8DispatchE10TimerFlagsVMn"
                 : "_$s8Dispatch0A13WorkItemFlagsVMn";
  const std::string Base =
      TimerFlags ? "_$sSaySo18OS_dispatch_sourceC8DispatchE10TimerFlagsVG"
                 : "_$sSay8Dispatch0A13WorkItemFlagsVG";
  EXPECT_TRUE(F.Image.recordDyldBindSlot(
      SwiftTypeMetadataFixture::DescriptorSlot, Descriptor, 0,
      "/usr/lib/swift/libswiftDispatch.dylib", false));
  F.Image.Symbols[0].Name = Base + "MR";
  F.Image.Symbols[1].Name = Base + "Md";
  auto &Data = F.Image.Segments[0].Data;
  llvm::support::endian::write32le(
      Data.data() + SwiftTypeMetadataFixture::Reference + 4 - 0x1000, 9);
  auto *Type = Data.data() + SwiftTypeMetadataFixture::TypeReference - 0x1000;
  Type[0] = 'S';
  Type[1] = 'a';
  Type[2] = 'y';
  Type[3] = 2;
  llvm::support::endian::write32le(
      Type + 4, static_cast<uint32_t>(static_cast<int32_t>(
                    SwiftTypeMetadataFixture::DescriptorSlot -
                    (SwiftTypeMetadataFixture::TypeReference + 4))));
  Type[8] = 'G';
  Type[9] = 0;
  return F;
}

SwiftTypeMetadataFixture swiftDictionaryTypeMetadataFixture() {
  auto F = swiftStdlibTypeMetadataFixture();
  F.Image.ImportPtrSlots.clear();
  F.Image.ImportStorageSlots.clear();
  F.Image.DyldBindSlots.clear();
  EXPECT_TRUE(F.Image.recordDyldBindSlot(
      SwiftTypeMetadataFixture::DescriptorSlot, "_$ss18_DictionaryStorageCMn",
      0, "/usr/lib/swift/libswiftCore.dylib", false));
  F.Image.Symbols[0].Name = "_$ss18_DictionaryStorageCySSSo8NSBundleCGMR";
  F.Image.Symbols[1].Name = "_$ss18_DictionaryStorageCySSSo8NSBundleCGMd";
  auto &Data = F.Image.Segments[0].Data;
  llvm::support::endian::write32le(
      Data.data() + SwiftTypeMetadataFixture::Reference + 4 - 0x1000, 21);
  std::memcpy(Data.data() + SwiftTypeMetadataFixture::TypeReference + 5 -
                  0x1000,
              "ySSSo8NSBundleCG", 17);
  return F;
}

SwiftTypeMetadataFixture swiftManagedBufferTypeMetadataFixture() {
  auto F = swiftStdlibTypeMetadataFixture();
  F.Image.ImportPtrSlots.clear();
  F.Image.ImportStorageSlots.clear();
  F.Image.DyldBindSlots.clear();
  EXPECT_TRUE(F.Image.recordDyldBindSlot(
      SwiftTypeMetadataFixture::DescriptorSlot, "_$ss13ManagedBufferCMn", 0,
      "/usr/lib/swift/libswiftCore.dylib", false));
  F.Image.Symbols[0].Name = "_$ss13ManagedBufferCyypGMR";
  F.Image.Symbols[1].Name = "_$ss13ManagedBufferCyypGMd";
  return F;
}

SwiftTypeMetadataFixture swiftImportedLockTypeMetadataFixture() {
  auto F = swiftManagedBufferTypeMetadataFixture();
  constexpr va_t Module = 0x4060;
  constexpr va_t Name = 0x4080;
  constexpr va_t Accessor = 0x5020;
  auto AddMapping = [&](llvm::StringRef SectionName, va_t Address,
                        SegmentFlags Flags) {
    Segment Mapping;
    Mapping.Name = SectionName.str();
    Mapping.VA = Mapping.FileOff = Address;
    Mapping.Size = Mapping.FileSz = 0x100;
    Mapping.Flags = Flags;
    Mapping.Data.resize(0x100);
    F.Image.Segments.push_back(std::move(Mapping));
    Section Data;
    Data.Name = SectionName.str();
    Data.SegmentName = SectionName.str();
    Data.VA = Data.FileOff = Address;
    Data.Size = Data.FileSz = 0x100;
    Data.Flags = Flags;
    if (Flags == (SegmentFlags::Readable | SegmentFlags::Executable))
      Data.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    F.Image.Sections.push_back(std::move(Data));
  };
  AddMapping("__constg_swiftt", 0x4000, SegmentFlags::Readable);
  AddMapping("__text", 0x5000,
             SegmentFlags::Readable | SegmentFlags::Executable);

  constexpr llvm::StringLiteral Payload = "ySDySSSo8NSBundleCG";
  constexpr llvm::StringLiteral Base =
      "_$ss13ManagedBufferCySDySSSo8NSBundleCGSo16os_unfair_lock_sVG";
  F.Image.Symbols[0].Name = Base.str() + "MR";
  F.Image.Symbols[1].Name = Base.str() + "Md";
  auto &Reference = F.Image.Segments[0].Data;
  const uint32_t DirectOffset = 5 + Payload.size();
  const uint32_t Length = DirectOffset + 6;
  llvm::support::endian::write32le(
      Reference.data() + SwiftTypeMetadataFixture::Reference + 4 - 0x1000,
      Length);
  auto *Type =
      Reference.data() + SwiftTypeMetadataFixture::TypeReference - 0x1000;
  std::memcpy(Type + 5, Payload.data(), Payload.size());
  Type[DirectOffset] = 1;
  llvm::support::endian::write32le(
      Type + DirectOffset + 1,
      static_cast<uint32_t>(static_cast<int32_t>(
          SwiftTypeMetadataFixture::LocalDescriptor -
          (SwiftTypeMetadataFixture::TypeReference + DirectOffset + 1))));
  Type[DirectOffset + 5] = 'G';
  Type[Length] = 0;

  auto &Descriptor = F.Image.Segments[3].Data;
  llvm::support::endian::write32le(Descriptor.data() + 0x20, 0x20011);
  llvm::support::endian::write32le(
      Descriptor.data() + 0x24,
      static_cast<uint32_t>(static_cast<int32_t>(
          Module - (SwiftTypeMetadataFixture::LocalDescriptor + 4))));
  llvm::support::endian::write32le(
      Descriptor.data() + 0x28,
      static_cast<uint32_t>(static_cast<int32_t>(
          Name - (SwiftTypeMetadataFixture::LocalDescriptor + 8))));
  llvm::support::endian::write32le(
      Descriptor.data() + 0x2c,
      static_cast<uint32_t>(static_cast<int32_t>(
          Accessor - (SwiftTypeMetadataFixture::LocalDescriptor + 12))));
  std::memcpy(Descriptor.data() + 0x80, "os_unfair_lock_s", 17);
  F.Image.Symbols.push_back({"_$sSo16os_unfair_lock_sVMn",
                             SwiftTypeMetadataFixture::LocalDescriptor, 0,
                             false});
  F.Image.Symbols.push_back({"_$sSoMXM", Module, 0, false});
  F.Image.Symbols.push_back({"_$sSo16os_unfair_lock_sVMa", Accessor, 0, true});
  return F;
}

SwiftTypeMetadataFixture
printableSwiftTypeMetadataFixture(Arch Architecture,
                                  llvm::StringRef TypeReference) {
  SwiftTypeMetadataFixture F(Architecture);
  F.Image.ImportPtrSlots.clear();
  F.Image.ImportStorageSlots.clear();
  F.Image.DyldBindSlots.clear();
  F.Image.ConflictingImportStorageSlots.clear();
  const std::string Base = "_$s" + TypeReference.str();
  F.Image.Symbols[0].Name = Base + "MR";
  F.Image.Symbols[1].Name = Base + "Md";
  auto &Data = F.Image.Segments[0].Data;
  llvm::support::endian::write32le(
      Data.data() + SwiftTypeMetadataFixture::Reference + 4 - 0x1000,
      TypeReference.size());
  auto *Type = Data.data() + SwiftTypeMetadataFixture::TypeReference - 0x1000;
  std::fill(Type, Type + 0x80, 0);
  std::memcpy(Type, TypeReference.data(), TypeReference.size());
  return F;
}

struct SwiftWitnessAccessorFixture {
  BinaryImage Image;
  HighFunc Accessor, Caller;
  ExprPtr CacheLoad, WitnessCall;
  HighStmt *CacheStore = nullptr;
  static constexpr va_t Cache = 0x2020;
  static constexpr va_t AccessorAddress = 0x3020;
  static constexpr va_t ConformanceSlot = 0x4020;
  static constexpr va_t MetadataSlot = 0x4028;
  static constexpr va_t RuntimeSlot = 0x4030;

  explicit SwiftWitnessAccessorFixture(Arch Architecture,
                                       bool SubstringSequence = false) {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Architecture;
    Image.Bits = Bitness::Bits64;
    Image.MachOTwoLevelNamespace = true;
    Segment Mapping;
    Mapping.Name = "__swift_cache";
    Mapping.VA = Mapping.FileOff = 0x2000;
    Mapping.Size = Mapping.FileSz = 0x100;
    Mapping.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Mapping.Data.resize(0x100);
    Image.Segments.push_back(Mapping);
    Section Data;
    Data.Name = Data.SegmentName = "__swift_cache";
    Data.VA = Data.FileOff = 0x2000;
    Data.Size = Data.FileSz = 0x100;
    Data.Flags = Mapping.Flags;
    Image.Sections.push_back(Data);
    Mapping.Name = "__swift_imports";
    Mapping.VA = Mapping.FileOff = 0x4000;
    Mapping.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Mapping.ReadOnlyAfterRelocations = true;
    Mapping.Data.assign(0x100, 0);
    Image.Segments.push_back(Mapping);
    Data.Name = Data.SegmentName = "__swift_imports";
    Data.VA = Data.FileOff = 0x4000;
    Data.Flags = Mapping.Flags;
    Image.Sections.push_back(Data);
    const char *CacheName = SubstringSequence ? "_$sS2sSTsWL" : "_$sS2SSysWL";
    const char *AccessorName =
        SubstringSequence ? "_$sS2sSTsWl" : "_$sS2SSysWl";
    const char *Conformance = SubstringSequence ? "_$sSsSTsMc" : "_$sSSSysMc";
    const char *Metadata = SubstringSequence ? "_$sSsN" : "_$sSSN";
    Image.Symbols.push_back({CacheName, Cache, 8, false});
    Image.Symbols.push_back({AccessorName, AccessorAddress, 0x40, true});
    Image.ImportPtrSlots[ConformanceSlot] = Conformance;
    Image.ImportPtrSlots[MetadataSlot] = Metadata;
    Image.ImportPtrSlots[RuntimeSlot] = "_swift_getWitnessTable";
    EXPECT_TRUE(Image.recordDyldBindSlot(ConformanceSlot, Conformance, 0,
                                         "/usr/lib/swift/libswiftCore.dylib",
                                         false));
    EXPECT_TRUE(Image.recordDyldBindSlot(
        MetadataSlot, Metadata, 0, "/usr/lib/swift/libswiftCore.dylib", false));
    EXPECT_TRUE(Image.recordDyldBindSlot(RuntimeSlot, "_swift_getWitnessTable",
                                         0, "/usr/lib/swift/libswiftCore.dylib",
                                         false));

    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    Accessor.Entry = AccessorAddress;
    Accessor.Name = AccessorName;
    Accessor.ReturnType = Pointer;
    MedVar CachedVar;
    CachedVar.Kind = MedVar::Temp;
    CachedVar.Id = 1;
    CachedVar.Size = 8;
    auto Cached = HighExpr::makeVar(CachedVar, Pointer);
    CacheLoad = HighExpr::makeLoad(
        HighExpr::makeConst(Cache, 8, ConstantAddressProvenance::DataAddress),
        Pointer);
    HighStmt Load;
    Load.Kind = StmtKind::Assign;
    Load.Dst = Cached;
    Load.Val = CacheLoad;
    HighStmt FastReturn;
    FastReturn.Kind = StmtKind::Return;
    FastReturn.RetVal = HighExpr::makeVar(CachedVar, Pointer);
    HighStmt FastPath;
    FastPath.Kind = StmtKind::If;
    FastPath.Cond = HighExpr::makeBinop(NdOp::INT_NOTEQUAL,
                                        HighExpr::makeVar(CachedVar, Pointer),
                                        HighExpr::makeConst(0, 8));
    FastPath.Body = {FastReturn};

    auto Runtime = swiftRuntimeSourceCallHint(Image, RuntimeSlot);
    EXPECT_TRUE(Runtime);
    std::vector<ExprPtr> Arguments;
    for (const va_t Slot : {ConformanceSlot, MetadataSlot})
      Arguments.push_back(HighExpr::makeLoad(
          HighExpr::makeConst(Slot, 8, ConstantAddressProvenance::DataAddress),
          Pointer));
    MedVar Incidental;
    Incidental.Kind = MedVar::Param;
    Incidental.Id = 0;
    Incidental.RegOff = Architecture == Arch::AArch64 ? 16 : 16;
    Incidental.Size = 8;
    Incidental.TheArch = Architecture;
    Arguments.push_back(HighExpr::makeVar(Incidental, Pointer));
    WitnessCall = HighExpr::makeCall("swift_getWitnessTable", RuntimeSlot,
                                     std::move(Arguments));
    WitnessCall->Type = Pointer;
    WitnessCall->SourceCallHint =
        std::make_shared<SourceCallTypeHint>(std::move(*Runtime));
    MedVar WitnessVar;
    WitnessVar.Kind = MedVar::Temp;
    WitnessVar.Id = 2;
    WitnessVar.Size = 8;
    HighStmt Build;
    Build.Kind = StmtKind::Assign;
    Build.Dst = HighExpr::makeVar(WitnessVar, Pointer);
    Build.Val = WitnessCall;
    HighStmt Store;
    Store.Kind = StmtKind::Store;
    Store.StoreAddr =
        HighExpr::makeConst(Cache, 8, ConstantAddressProvenance::DataAddress);
    Store.StoreVal = HighExpr::makeVar(WitnessVar, Pointer);
    Store.MemoryOrdering = NdMemoryOrdering::Release;
    HighStmt SlowReturn;
    SlowReturn.Kind = StmtKind::Return;
    SlowReturn.RetVal = HighExpr::makeVar(WitnessVar, Pointer);
    Accessor.Body = {Load, FastPath, Build, Store, SlowReturn};
    CacheStore = &Accessor.Body[3];

    SourceFunctionTypeHint NativeSignature;
    NativeSignature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    NativeSignature.ReturnType = Pointer;
    NativeSignature.Parameters = {{"incidental", Pointer}};
    std::string Error;
    EXPECT_TRUE(
        assignDarwinScalarSourceABI(NativeSignature, Architecture, Error))
        << Error;
    auto Native = std::make_shared<SourceCallTypeHint>();
    Native->CallKind = SourceCallTypeHint::Kind::Native;
    Native->TargetAddress = AccessorAddress;
    Native->TargetName = Accessor.Name;
    Native->Signature = NativeSignature;
    auto Call = HighExpr::makeCall(
        Accessor.Name, AccessorAddress,
        {HighExpr::makeConst(0xfeed, 8, ConstantAddressProvenance::Scalar)});
    Call->Type = Pointer;
    Call->SourceCallHint = std::move(Native);
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = std::move(Call);
    Caller.Entry = 0x5000;
    Caller.ReturnType = Pointer;
    Caller.Body = {std::move(Return)};
  }
};
} // namespace

TEST(ObjCSourceBindings, SwiftWitnessUndefRequiresGenericDescriptorContract) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    SwiftWitnessAccessorFixture F(Architecture);
    constexpr auto Descriptor =
        "_$s7Combine19CurrentValueSubjectCyxq_GAA9PublisherAAMc";
    constexpr auto Module =
        "/System/Library/Frameworks/Combine.framework/Combine";
    F.Image.ImportStorageSlots.erase(F.ConformanceSlot);
    F.Image.ImportPtrSlots[F.ConformanceSlot] = Descriptor;
    F.Image.DyldBindSlots[F.ConformanceSlot] = {Descriptor, 0, Module, false};
    F.WitnessCall->Operands[2] = HighExpr::makeUndef(8);
    // This test isolates the runtime contract; a real generic metadata value
    // remains unchanged and requires its ordinary independent source binding.
    F.Accessor.Body = {F.Accessor.Body[2], F.Accessor.Body[4]};
    ASSERT_TRUE(
        swiftWitnessInstantiationArgumentUnused(F.Image, F.ConformanceSlot));
    auto Flow = analyzeHighSourceFlow(F.Accessor, true);
    for (auto &Issue : Flow.Items)
      ADD_FAILURE() << Issue.Reason;
    ASSERT_TRUE(sdk::objc_binding_detail::swiftWitnessUndefDescriptor(
        F.Accessor, *F.WitnessCall, F.Image));
    const auto Result = bindObjCSourceReferences(F.Accessor, F.Image);
    const auto Call = Result.Function.Body[0].Val;
    ASSERT_TRUE(Call && Call->SourceCallHint);
    EXPECT_EQ(Call->Operands[2]->Kind, ExprKind::Const);
    EXPECT_EQ(Call->Operands[2]->ConstVal, 0U);
    EXPECT_EQ(Call->SourceCallHint->SwiftWitnessUndefDescriptor,
              F.ConformanceSlot);
    EXPECT_EQ(F.WitnessCall->Operands[2]->Kind, ExprKind::Undef);
    EXPECT_EQ(Call->SourceCallHint->Signature.Parameters.size(), 3U);
    EXPECT_TRUE(objcSourceCallBound(*Call, F.Image, {}, nullptr, nullptr,
                                    &Result.Function));
  }
}

namespace {
struct GenericWitnessFixture : SwiftWitnessAccessorFixture {
  explicit GenericWitnessFixture(Arch Architecture, bool Alias = false)
      : SwiftWitnessAccessorFixture(Architecture) {
    constexpr auto Descriptor =
        "_$s7Combine19CurrentValueSubjectCyxq_GAA9PublisherAAMc";
    Image.ImportStorageSlots.erase(ConformanceSlot);
    Image.ImportPtrSlots[ConformanceSlot] = Descriptor;
    Image.DyldBindSlots[ConformanceSlot] = {
        Descriptor, 0, "/System/Library/Frameworks/Combine.framework/Combine",
        false};
    WitnessCall->Operands[2] = HighExpr::makeUndef(8);
    Accessor.Body = {Accessor.Body[2], Accessor.Body[4]};
    if (Alias) {
      MedVar Variable;
      Variable.Kind = MedVar::Temp;
      Variable.Id = 3;
      Variable.Size = 8;
      HighStmt Load;
      Load.Kind = StmtKind::Assign;
      Load.Dst = HighExpr::makeVar(Variable, WitnessCall->Operands[0]->Type);
      Load.Val = WitnessCall->Operands[0];
      WitnessCall->Operands[0] = HighExpr::makeVar(Variable, Load.Dst->Type);
      Accessor.Body.insert(Accessor.Body.begin(), Load);
    }
  }
};
} // namespace

TEST(ObjCSourceBindings, SwiftWitnessUndefRejectsUnprovedInputAndABI) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation != 33; ++Mutation) {
      SCOPED_TRACE(Mutation);
      GenericWitnessFixture F(Architecture);
      auto Hint =
          std::make_shared<SourceCallTypeHint>(*F.WitnessCall->SourceCallHint);
      F.WitnessCall->SourceCallHint = Hint;
      auto &Descriptor = F.Image.DyldBindSlots[F.ConformanceSlot];
      auto &Runtime = F.Image.DyldBindSlots[F.RuntimeSlot];
      switch (Mutation) {
      case 0:
        Descriptor.WeakImport = true;
        break;
      case 1:
        Descriptor.Addend = 8;
        break;
      case 2:
        Descriptor.Module = "/tmp/Combine";
        break;
      case 3:
        Descriptor.Name += "other";
        break;
      case 4:
        F.Image.ImportPtrSlots[F.ConformanceSlot] += "other";
        break;
      case 5:
        Runtime.WeakImport = true;
        break;
      case 6:
        Runtime.Module = "/tmp/libswiftCore.dylib";
        break;
      case 7:
        Runtime.Name = "_swift_getWitnessTableRelative";
        break;
      case 8:
        Hint->Signature.Parameters[2].Location.ValueBytes = 4;
        break;
      case 9:
        F.WitnessCall->Operands[1]->Type = NdType::makeFloat(8);
        break;
      case 10:
        F.WitnessCall->Operands[0]->Type = NdType::makeInt(4, false);
        break;
      case 11:
        F.WitnessCall->Operands[0]->Operands[0]->ConstProvenance =
            ConstantAddressProvenance::Scalar;
        break;
      case 12:
        F.WitnessCall->IsIndirectCall = true;
        break;
      case 13:
        F.WitnessCall->CallAddr += 4;
        break;
      case 14: {
        auto Cast = std::make_shared<HighExpr>();
        Cast->Kind = ExprKind::Cast;
        Cast->Type = NdType::makeInt(8, false);
        Cast->Operands = {F.WitnessCall->Operands[2]};
        F.WitnessCall->Operands[2] = Cast;
        break;
      }
      case 15:
        F.WitnessCall->Operands[2] = HighExpr::makeConst(123, 8);
        break;
      case 16:
        F.WitnessCall->Operands[2] = HighExpr::makeCall("effect", 0x7770, {});
        F.WitnessCall->Operands[2]->Type = NdType::makePtr(NdType::makeVoid());
        break;
      case 17:
        F.WitnessCall->Operands[2]->Type = NdType::makeInt(4, false);
        break;
      case 18:
        F.WitnessCall->Operands[0] = HighExpr::makeConst(
            F.ConformanceSlot, 8, ConstantAddressProvenance::DataAddress);
        break;
      case 19:
        F.WitnessCall->Operands[0]->MemoryOrdering = NdMemoryOrdering::Acquire;
        break;
      case 20:
        F.WitnessCall->Operands[0]->Type = NdType::makeFloat(8);
        break;
      case 21:
        F.WitnessCall->MemoryOrdering = NdMemoryOrdering::Acquire;
        break;
      case 22:
        Hint->SwiftWitnessUndefDescriptor = F.ConformanceSlot;
        break;
      case 23:
        F.Image.IsRelocatable = true;
        break;
      case 24:
        F.Image.Bits = Bitness::Bits32;
        break;
      case 25:
        F.Image.Format = BinaryFormat::ELF;
        break;
      case 26:
        F.Image.ConflictingImportStorageSlots.insert(F.ConformanceSlot);
        break;
      case 27:
        Descriptor.Name = "_$sSSSysMc";
        F.Image.ImportPtrSlots[F.ConformanceSlot] = Descriptor.Name;
        Descriptor.Module = "/usr/lib/swift/libswiftCore.dylib";
        break;
      case 28:
        Hint->Signature.ReturnLocation.ValueBytes = 4;
        break;
      case 29:
        Hint->DoesNotReturn = true;
        break;
      case 30:
        Hint->BorrowedByteInputs = {{0, 1}};
        break;
      case 31:
        Hint->TargetName += "Relative";
        break;
      case 32:
        F.WitnessCall->Type = NdType::makeFloat(8);
        break;
      }
      const auto Before = F.WitnessCall->Operands[2];
      const auto Result = bindObjCSourceReferences(F.Accessor, F.Image);
      const auto &Call = Result.Function.Body[0].Val;
      ASSERT_TRUE(Call && Call->SourceCallHint);
      EXPECT_EQ(Call->Operands[2]->Kind, Before->Kind);
      if (Mutation != 22)
        EXPECT_FALSE(Call->SourceCallHint->SwiftWitnessUndefDescriptor);
    }
  }
}

TEST(ObjCSourceBindings,
     SwiftWitnessUndefRevalidatesCurrentAliasesAndPublication) {
  for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
    SCOPED_TRACE(Mutation);
    GenericWitnessFixture F(Arch::AArch64, true);
    auto Result = bindObjCSourceReferences(F.Accessor, F.Image);
    auto Call = Result.Function.Body[1].Val;
    ASSERT_TRUE(Call->SourceCallHint->SwiftWitnessUndefDescriptor);
    ASSERT_TRUE(objcSourceCallBound(*Call, F.Image, {}, nullptr, nullptr,
                                    &Result.Function));
    auto Hint = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
    Call->SourceCallHint = Hint;
    switch (Mutation) {
    case 0:
      Hint->SwiftWitnessUndefDescriptor = F.MetadataSlot;
      break;
    case 1:
      Call->Operands[2]->ConstVal = 1;
      break;
    case 2:
      Call->Operands[2] = HighExpr::makeUndef(8);
      break;
    case 3:
      Result.Function.Body[0].Val = HighExpr::makeConst(0, 8);
      break;
    case 4:
      F.Image.DyldBindSlots[F.ConformanceSlot].WeakImport = true;
      break;
    case 5:
      F.Image.DyldBindSlots[F.RuntimeSlot].Module = "/tmp/runtime";
      break;
    case 6:
      Hint->Signature.Parameters[0].Location.ValueBytes = 4;
      break;
    case 7:
      Call->CallAddr += 4;
      break;
    case 8:
      Result.Function.Body.push_back(Result.Function.Body[1]);
      break;
    case 9:
      Result.Function.Body[0].Val->SourceCallHint.reset();
      break;
    case 10:
      Hint->CallKind = SourceCallTypeHint::Kind::Native;
      break;
    }
    EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {}, nullptr, nullptr,
                                     &Result.Function));
  }
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    GenericWitnessFixture F(Arch::AArch64, true);
    auto Load = F.Accessor.Body[0];
    switch (Mutation) {
    case 0:
      F.Accessor.Body.insert(F.Accessor.Body.begin(), Load);
      break;
    case 1:
      std::swap(F.Accessor.Body[0], F.Accessor.Body[1]);
      break;
    case 2:
      F.Accessor.Body[0].Val = F.WitnessCall->Operands[0];
      break;
    case 3: {
      HighStmt Branch;
      Branch.Kind = StmtKind::If;
      Branch.Cond = HighExpr::makeUndef(1);
      Branch.Body = {Load};
      F.Accessor.Body[0] = Branch;
      break;
    }
    case 4: {
      auto Escape = std::make_shared<HighExpr>();
      Escape->Kind = ExprKind::Addr;
      Escape->Type = NdType::makePtr(NdType::makeVoid());
      Escape->Operands = {Load.Dst};
      HighStmt Effect;
      Effect.Kind = StmtKind::Call;
      Effect.CallExpr = HighExpr::makeCall("escape", 0x7770, {Escape});
      F.Accessor.Body.insert(F.Accessor.Body.begin() + 1, Effect);
      break;
    }
    case 5:
      F.Accessor.Body[0].Dst->Var.Size = 4;
      break;
    case 6:
      F.Accessor.Body[0].Val->MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    }
    const auto Result = bindObjCSourceReferences(F.Accessor, F.Image);
    size_t Normalized = 0;
    walkStmts(Result.Function.Body, [&](const HighStmt &Statement) {
      forEachExpr(Statement, [&](const ExprPtr &Value) {
        if (Value && Value->SourceCallHint &&
            Value->SourceCallHint->SwiftWitnessUndefDescriptor)
          ++Normalized;
      });
    });
    EXPECT_EQ(Normalized, 0U);
  }
}

TEST(ObjCSourceBindings, SwiftWitnessAccessorRebuildsZeroArgumentCacheHelper) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    SwiftWitnessAccessorFixture F(Architecture);
    const std::map<va_t, const HighFunc *> Functions{
        {F.Accessor.Entry, &F.Accessor}};
    const auto Result =
        bindObjCSourceReferences(F.Caller, F.Image, nullptr, &Functions);
    ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
    EXPECT_TRUE(Result.Dependencies.empty());
    EXPECT_EQ(Result.SwiftWitnessCaches,
              (std::map<va_t, va_t>{{F.Cache, F.Accessor.Entry}}));
    const auto Call = Result.Function.Body[0].RetVal;
    ASSERT_TRUE(Call->SourceCallHint);
    EXPECT_EQ(Call->SourceCallHint->CallKind,
              SourceCallTypeHint::Kind::RuntimeSwiftWitnessAccessor);
    EXPECT_TRUE(Call->Operands.empty());
    EXPECT_TRUE(objcSourceCallBound(*Call, F.Image, Functions));

    std::set<std::string> Helpers;
    const auto Source = renderObjCSwiftWitnessCacheHelpers(
        F.Image, Result.SwiftWitnessCaches, Functions, Helpers);
    EXPECT_EQ(Helpers,
              (std::set<std::string>{"neverd_swift_witness_cache_2020_address",
                                     "neverd_swift_witness_accessor_3020"}));
    EXPECT_NE(Source.find("__asm__(\"_$sSSSysMc\")"), std::string::npos);
    EXPECT_NE(Source.find("__asm__(\"_$sSSN\")"), std::string::npos);
    EXPECT_NE(Source.find("__asm__(\"_swift_getWitnessTable\")"),
              std::string::npos);
    EXPECT_NE(Source.find("neverd_swift_witness_accessor_3020(void)"),
              std::string::npos);
    EXPECT_NE(Source.find("(void *)0"), std::string::npos);
    EXPECT_NE(Source.find("__ATOMIC_RELEASE"), std::string::npos);
  }
}

TEST(ObjCSourceBindings,
     SwiftWitnessAccessorIgnoresOnlyDetachedPostReturnCode) {
  SwiftWitnessAccessorFixture F(Arch::AArch64);
  auto &Body = F.Accessor.Body;
  Body[0].Addr = 0x3024;
  Body[1].Addr = 0x3028;
  Body[1].Body[0].Addr = 0x302c;
  Body[2].Addr = 0x3040;
  Body[3].Addr = 0x3044;
  Body[4].Addr = 0x3048;
  HighStmt Detached;
  Detached.Kind = StmtKind::Block;
  Detached.Addr = 0x3050;
  HighStmt AlienReturn;
  AlienReturn.Kind = StmtKind::Return;
  AlienReturn.Addr = 0x3054;
  AlienReturn.RetVal = HighExpr::makeConst(0, 8);
  Body.push_back(Detached);
  Body.push_back(AlienReturn);

  const auto Hint = [&] {
    return sdk::objc_binding_detail::swiftWitnessAccessorCallHint(F.Accessor,
                                                                  F.Image);
  };
  EXPECT_TRUE(Hint());
  const std::map<va_t, const HighFunc *> Functions{
      {F.Accessor.Entry, &F.Accessor}};
  const auto Result =
      bindObjCSourceReferences(F.Caller, F.Image, nullptr, &Functions);
  EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  EXPECT_TRUE(Result.Dependencies.empty());
  EXPECT_EQ(Result.SwiftWitnessCaches,
            (std::map<va_t, va_t>{{F.Cache, F.Accessor.Entry}}));

  Body[5].Addr = Body[4].Addr;
  EXPECT_FALSE(Hint());
  Body[5].Addr = 0x3050;
  Body[4].Kind = StmtKind::Goto;
  Body[4].GotoTarget = Body[5].Addr;
  EXPECT_FALSE(Hint());
  Body[4].Kind = StmtKind::Return;
  F.Accessor.StructuredExceptionRegions = true;
  EXPECT_FALSE(Hint());
}

TEST(ObjCSourceBindings, SubstringSequenceWitnessKeepsExactMetadataPair) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    SwiftWitnessAccessorFixture F(Architecture, true);
    const std::map<va_t, const HighFunc *> Functions{
        {F.Accessor.Entry, &F.Accessor}};
    const auto Result =
        bindObjCSourceReferences(F.Caller, F.Image, nullptr, &Functions);
    ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
    EXPECT_TRUE(Result.Dependencies.empty());
    EXPECT_EQ(Result.SwiftWitnessCaches,
              (std::map<va_t, va_t>{{F.Cache, F.Accessor.Entry}}));
    std::set<std::string> Helpers;
    const auto Source = renderObjCSwiftWitnessCacheHelpers(
        F.Image, Result.SwiftWitnessCaches, Functions, Helpers);
    EXPECT_NE(Source.find("__asm__(\"_$sSsSTsMc\")"), std::string::npos);
    EXPECT_NE(Source.find("__asm__(\"_$sSsN\")"), std::string::npos);
    EXPECT_NE(Source.find("(void *)0"), std::string::npos);

    F.Image.ImportPtrSlots[F.MetadataSlot] = "_$sSSN";
    F.Image.ImportStorageSlots[F.MetadataSlot].Name = "_$sSSN";
    F.Image.DyldBindSlots[F.MetadataSlot].Name = "_$sSSN";
    EXPECT_FALSE(sdk::objc_binding_detail::swiftWitnessAccessorCallHint(
        F.Accessor, F.Image));
  }
}

TEST(ObjCSourceBindings, LocalWitnessRequiresMatchingExportedNominalType) {
  constexpr va_t Conformance = 0x5020;
  constexpr va_t Metadata = 0x6020;
  constexpr va_t NominalDescriptor = 0x7020;
  auto Fixture = [&] {
    SwiftWitnessAccessorFixture F(Arch::AArch64);
    const auto AddReadOnly = [&](va_t Base) {
      Segment Mapping;
      Mapping.Name = "__const";
      Mapping.VA = Mapping.FileOff = Base;
      Mapping.Size = Mapping.FileSz = 0x100;
      Mapping.Flags = SegmentFlags::Readable;
      Mapping.ReadOnlyAfterRelocations = true;
      Mapping.Data.resize(0x100);
      F.Image.Segments.push_back(std::move(Mapping));
      Section Data;
      Data.Name = Data.SegmentName = "__const";
      Data.VA = Data.FileOff = Base;
      Data.Size = Data.FileSz = 0x100;
      Data.Flags = SegmentFlags::Readable;
      F.Image.Sections.push_back(std::move(Data));
    };
    AddReadOnly(0x5000);
    AddReadOnly(0x6000);
    AddReadOnly(0x7000);
    constexpr llvm::StringLiteral ConformanceName =
        "_$s3WMF12RequestErrorOs0C0AAMc";
    constexpr llvm::StringLiteral MetadataName = "_$s3WMF12RequestErrorON";
    constexpr llvm::StringLiteral NominalName = "_$s3WMF12RequestErrorOMn";
    F.Image.Symbols.push_back(
        {ConformanceName.str(), Conformance, 0x40, false});
    F.Image.Symbols.push_back({MetadataName.str(), Metadata, 0x20, false});
    F.Image.Symbols.push_back(
        {NominalName.str(), NominalDescriptor, 0x40, false});
    F.Image.Exports.push_back({ConformanceName.str(), 0, Conformance});
    F.Image.Exports.push_back({MetadataName.str(), 0, Metadata});
    F.Image.Exports.push_back({NominalName.str(), 0, NominalDescriptor});
    llvm::support::endian::write64le(F.Image.Segments[3].Data.data() + 0x20,
                                     0x201);
    llvm::support::endian::write64le(F.Image.Segments[3].Data.data() + 0x28,
                                     NominalDescriptor);
    F.Image.DataPtrRelocSlots.insert(Metadata + 8);
    F.Image.DataPtrRelocTargetOwners[Metadata + 8] = 0x7000;
    F.Image.MachOResolvedChainedPointerSlots.insert(Metadata + 8);
    F.WitnessCall->Operands[0] = HighExpr::makeConst(
        Conformance, 8, ConstantAddressProvenance::DataAddress);
    F.WitnessCall->Operands[1] = HighExpr::makeConst(
        Metadata, 8, ConstantAddressProvenance::DataAddress);
    return F;
  };
  auto F = Fixture();
  std::array<std::string, 2> Globals;
  const auto Cache = sdk::objc_binding_detail::swiftWitnessCacheAddressHint(
      F.Accessor, F.Image, F.Cache, &Globals);
  ASSERT_TRUE(Cache);
  EXPECT_EQ(Globals[0], "$s3WMF12RequestErrorOs0C0AAMc");
  EXPECT_EQ(Globals[1], "$s3WMF12RequestErrorON");
  const std::map<va_t, const HighFunc *> Functions{
      {F.Accessor.Entry, &F.Accessor}};
  const auto Result =
      bindObjCSourceReferences(F.Caller, F.Image, nullptr, &Functions);
  EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  EXPECT_EQ(Result.SwiftWitnessCaches,
            (std::map<va_t, va_t>{{F.Cache, F.Accessor.Entry}}));
  std::set<std::string> Helpers;
  const auto Source = renderObjCSwiftWitnessCacheHelpers(
      F.Image, Result.SwiftWitnessCaches, Functions, Helpers);
  EXPECT_NE(Source.find("__asm__(\"_$s3WMF12RequestErrorOs0C0AAMc\")"),
            std::string::npos);
  EXPECT_NE(Source.find("__asm__(\"_$s3WMF12RequestErrorON\")"),
            std::string::npos);

  HighFunc Direct;
  Direct.Entry = 0x3040;
  Direct.ReturnType = NdType::makePtr(NdType::makeVoid());
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeConst(Conformance, 8,
                                      ConstantAddressProvenance::DataAddress);
  Direct.Body = {Return};
  const auto Bound = bindObjCSourceReferences(Direct, F.Image);
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  EXPECT_EQ(Bound.SwiftConformanceDescriptors,
            (std::map<va_t, std::string>{
                {Conformance, "_$s3WMF12RequestErrorOs0C0AAMc"}}));
  const auto Descriptor = Bound.Function.Body[0].RetVal;
  ASSERT_TRUE(Descriptor->SourceCallHint);
  EXPECT_EQ(Descriptor->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeSwiftConformanceDescriptorAddress);
  EXPECT_TRUE(objcSourceCallBound(*Descriptor, F.Image, {}));
  Helpers.clear();
  const auto DescriptorSource = renderObjCSwiftConformanceDescriptorHelpers(
      F.Image, Bound.SwiftConformanceDescriptors, Helpers);
  EXPECT_NE(
      DescriptorSource.find("neverd_swift_conformance_descriptor_5020_address"),
      std::string::npos);
  EXPECT_NE(
      DescriptorSource.find("__asm__(\"_$s3WMF12RequestErrorOs0C0AAMc\")"),
      std::string::npos);
  Direct.Body[0].RetVal->ConstProvenance = ConstantAddressProvenance::Scalar;
  EXPECT_TRUE(bindObjCSourceReferences(Direct, F.Image)
                  .SwiftConformanceDescriptors.empty());
  F.Image.Exports.erase(F.Image.Exports.begin());
  EXPECT_FALSE(objcSourceCallBound(*Descriptor, F.Image, {}));
  EXPECT_THROW(renderObjCSwiftConformanceDescriptorHelpers(
                   F.Image, Bound.SwiftConformanceDescriptors, Helpers),
               std::runtime_error);

  auto WrongType = Fixture();
  WrongType.Image.Symbols[2].Name = "_$s3WMF12DifferentErrorOs0C0AAMc";
  WrongType.Image.Exports[0].Name = WrongType.Image.Symbols[2].Name;
  EXPECT_FALSE(sdk::objc_binding_detail::swiftWitnessCacheAddressHint(
      WrongType.Accessor, WrongType.Image, WrongType.Cache));
  auto Unexported = Fixture();
  Unexported.Image.Exports.erase(Unexported.Image.Exports.begin());
  EXPECT_FALSE(sdk::objc_binding_detail::swiftWitnessCacheAddressHint(
      Unexported.Accessor, Unexported.Image, Unexported.Cache));
  auto Unproven = Fixture();
  Unproven.WitnessCall->Operands[0]->ConstProvenance =
      ConstantAddressProvenance::Scalar;
  EXPECT_FALSE(sdk::objc_binding_detail::swiftWitnessCacheAddressHint(
      Unproven.Accessor, Unproven.Image, Unproven.Cache));
  auto WrongOwner = Fixture();
  WrongOwner.WitnessCall->Operands[0]->AddressOwnerVA = Conformance - 8;
  EXPECT_FALSE(sdk::objc_binding_detail::swiftWitnessCacheAddressHint(
      WrongOwner.Accessor, WrongOwner.Image, WrongOwner.Cache));
}

TEST(ObjCSourceBindings, SwiftWitnessAccessorRejectsIncompleteOrStaleEvidence) {
  for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    SwiftWitnessAccessorFixture F(Arch::AArch64);
    if (Mutation == 0)
      F.Image.Segments[0].Data[F.Cache - 0x2000] = 1;
    if (Mutation == 1)
      F.CacheStore->MemoryOrdering = NdMemoryOrdering::None;
    if (Mutation == 2)
      F.Accessor.Name = "_$sS2SSysWx";
    if (Mutation == 3)
      F.Image.ImportPtrSlots.erase(F.ConformanceSlot);
    if (Mutation == 4)
      F.WitnessCall->IsIndirectCall = true;
    if (Mutation == 5)
      F.Accessor.Body.pop_back();
    if (Mutation == 6)
      F.Caller.Body[0].RetVal->IsIndirectCall = true;
    if (Mutation == 7)
      F.Caller.Body[0].RetVal->CallAddr += 4;
    const std::map<va_t, const HighFunc *> Functions{
        {F.Accessor.Entry, &F.Accessor}};
    const auto Result =
        bindObjCSourceReferences(F.Caller, F.Image, nullptr, &Functions);
    EXPECT_TRUE(Result.SwiftWitnessCaches.empty());
    EXPECT_FALSE(Result.Dependencies.empty());
  }

  SwiftWitnessAccessorFixture F(Arch::AArch64);
  const std::map<va_t, const HighFunc *> Functions{
      {F.Accessor.Entry, &F.Accessor}};
  auto Result =
      bindObjCSourceReferences(F.Caller, F.Image, nullptr, &Functions);
  ASSERT_EQ(Result.SwiftWitnessCaches.size(), 1U);
  F.CacheStore->MemoryOrdering = NdMemoryOrdering::None;
  EXPECT_FALSE(
      objcSourceCallBound(*Result.Function.Body[0].RetVal, F.Image, Functions));
  std::set<std::string> Helpers;
  EXPECT_THROW(renderObjCSwiftWitnessCacheHelpers(
                   F.Image, Result.SwiftWitnessCaches, Functions, Helpers),
               std::runtime_error);
}

struct SwiftSingletonDescriptorFixture : SwiftTypeMetadataFixture {
  static constexpr va_t RuntimeSlot = 0x3040;
  static constexpr va_t CallAddress = 0x5010;

  SwiftSingletonDescriptorFixture()
      : SwiftTypeMetadataFixture(Arch::AArch64, false, false, false, true) {
    Segment Text;
    Text.VA = Text.FileOff = 0x5000;
    Text.Size = Text.FileSz = 0x100;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(0x100);
    Image.Segments.push_back(Text);
    Section Code;
    Code.VA = Code.FileOff = 0x5000;
    Code.Size = Code.FileSz = 0x100;
    Code.Flags = Text.Flags;
    Code.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    Image.Sections.push_back(Code);
    Image.DynInfo.NeededLibs.push_back("/usr/lib/swift/libswiftCore.dylib");
    EXPECT_TRUE(
        Image.recordDyldBindSlot(RuntimeSlot, "_swift_getSingletonMetadata", 0,
                                 "/usr/lib/swift/libswiftCore.dylib", false));
    Image.ImportPtrSlots[RuntimeSlot] = "_swift_getSingletonMetadata";
    auto Runtime = swiftRuntimeSourceCallHint(Image, RuntimeSlot);
    EXPECT_TRUE(Runtime);
    if (!Runtime)
      return;
    auto Call = HighExpr::makeCall(
        "swift_getSingletonMetadata", CallAddress,
        {HighExpr::makeConst(0, 8, ConstantAddressProvenance::Scalar),
         HighExpr::makeConst(LocalDescriptor, 8,
                             ConstantAddressProvenance::DataAddress)});
    Call->Type = Runtime->Signature.ReturnType;
    Call->SourceCallHint =
        std::make_shared<SourceCallTypeHint>(std::move(*Runtime));
    Function = {};
    Function.Entry = CallAddress;
    Function.ReturnType = Call->Type;
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = std::move(Call);
    Function.Body = {std::move(Return)};
  }
};

TEST(ObjCSourceBindings,
     SwiftValueWitnessUsesOnlyMatchingExportedStructMetadataIdentity) {
  constexpr va_t Metadata = 0x6020;
  const std::string Symbol = "_$s7WMFData24WMFFeatureConfigResponseVN";
  auto Fixture = [&] {
    SwiftTypeMetadataFixture F(Arch::AArch64, false, false, false, true);
    Segment Data;
    Data.VA = Data.FileOff = 0x6000;
    Data.Size = Data.FileSz = 0x100;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.ReadOnlyAfterRelocations = true;
    Data.Data.resize(0x100);
    llvm::support::endian::write64le(Data.Data.data() + 0x20, 0x200);
    llvm::support::endian::write64le(Data.Data.data() + 0x28,
                                     F.LocalDescriptor);
    F.Image.Segments.push_back(Data);
    Section Section;
    Section.VA = Section.FileOff = 0x6000;
    Section.Size = Section.FileSz = 0x100;
    Section.Flags = Data.Flags;
    F.Image.Sections.push_back(Section);
    F.Image.MachOHasChainedFixups = true;
    F.Image.DataPtrRelocSlots.insert(Metadata + 8);
    F.Image.MachOResolvedChainedPointerSlots.insert(Metadata + 8);
    F.Image.DataPtrRelocTargetOwners[Metadata + 8] = 0x4000;
    F.Image.Symbols.push_back({Symbol, Metadata, 0, false});
    F.Image.Exports.push_back({Symbol, 0, Metadata});
    const auto Hint = swiftValueWitnessSourceCallHint(
        Arch::AArch64, SourceCallTypeHint::SwiftValueWitnessKind::Destroy);
    EXPECT_TRUE(Hint);
    auto Value = HighExpr::makeConst(0, 8);
    Value->Type = NdType::makePtr(NdType::makeVoid());
    auto Identity = HighExpr::makeConst(Metadata, 8,
                                        ConstantAddressProvenance::DataAddress);
    Identity->Type = NdType::makePtr(NdType::makeVoid());
    auto Call = HighExpr::makeCall("indirect_call", 0,
                                   {std::move(Value), std::move(Identity)});
    Call->Type = NdType::makeVoid();
    Call->IsIndirectCall = true;
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    HighStmt Statement;
    Statement.Kind = StmtKind::ExprStmt;
    Statement.Val = std::move(Call);
    F.Function.ReturnType = NdType::makeVoid();
    F.Function.Body = {std::move(Statement)};
    return F;
  };

  auto F = Fixture();
  const auto Expected =
      objc_binding_detail::swiftNominalMetadataAddressHint(F.Image, Metadata);
  ASSERT_TRUE(Expected);
  EXPECT_EQ(Expected->TargetName, Symbol);
  const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  ASSERT_EQ(Bound.SwiftNominalMetadata.size(), 1U);
  EXPECT_EQ(Bound.SwiftNominalMetadata.at(Metadata), Symbol);
  const auto Identity = Bound.Function.Body[0].Val->Operands.back();
  ASSERT_TRUE(Identity->SourceCallHint);
  EXPECT_EQ(Identity->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeSwiftNominalMetadataAddress);
  EXPECT_TRUE(objcSourceCallBound(*Identity, F.Image, {}));
  std::set<std::string> Helpers;
  const auto Source = renderObjCSwiftNominalMetadataHelpers(
      F.Image, Bound.SwiftNominalMetadata, Helpers);
  EXPECT_NE(Source.find("__asm__(\"" + Symbol + "\")"), std::string::npos);
  EXPECT_TRUE(Helpers.count("neverd_swift_nominal_metadata_6020_address"));

  F.Image.Exports.pop_back();
  EXPECT_FALSE(objcSourceCallBound(*Identity, F.Image, {}));
  EXPECT_THROW(renderObjCSwiftNominalMetadataHelpers(
                   F.Image, Bound.SwiftNominalMetadata, Helpers),
               std::runtime_error);
  auto Forged = Fixture();
  llvm::support::endian::write64le(
      Forged.Image.Segments.back().Data.data() + 0x28, 0x4028);
  EXPECT_TRUE(bindObjCSourceReferences(Forged.Function, Forged.Image)
                  .SwiftNominalMetadata.empty());
  Forged = Fixture();
  Forged.Image.Segments.back().ReadOnlyAfterRelocations = false;
  EXPECT_TRUE(bindObjCSourceReferences(Forged.Function, Forged.Image)
                  .SwiftNominalMetadata.empty());
  Forged = Fixture();
  Forged.Image.MachOResolvedChainedPointerSlots.clear();
  EXPECT_TRUE(bindObjCSourceReferences(Forged.Function, Forged.Image)
                  .SwiftNominalMetadata.empty());
  Forged = Fixture();
  Forged.Function.Body[0].Val->Operands.back()->ConstProvenance =
      ConstantAddressProvenance::Scalar;
  EXPECT_TRUE(bindObjCSourceReferences(Forged.Function, Forged.Image)
                  .SwiftNominalMetadata.empty());
}

TEST(ObjCSourceBindings,
     SwiftPrivateNominalMetadataUsesOnlyExactExportedAccessor) {
  constexpr va_t Metadata = 0x6020;
  constexpr va_t Accessor = 0x7020;
  const std::string MetadataName = "_$s7WMFData24WMFFeatureConfigResponseVN";
  const std::string AccessorName = "_$s7WMFData24WMFFeatureConfigResponseVMa";
  auto Fixture = [&] {
    SwiftTypeMetadataFixture F(Arch::AArch64, false, false, false, true);
    F.Image.Exports.clear(); // The descriptor and metadata stay image-private.
    llvm::support::endian::write32le(F.Image.Segments[3].Data.data() + 0x20,
                                     0x51);
    Segment Data;
    Data.VA = Data.FileOff = 0x6000;
    Data.Size = Data.FileSz = 0x100;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.ReadOnlyAfterRelocations = true;
    Data.Data.resize(0x100);
    llvm::support::endian::write64le(Data.Data.data() + 0x20, 0x200);
    llvm::support::endian::write64le(Data.Data.data() + 0x28,
                                     F.LocalDescriptor);
    F.Image.Segments.push_back(Data);
    Section DataSection;
    DataSection.VA = DataSection.FileOff = 0x6000;
    DataSection.Size = DataSection.FileSz = 0x100;
    DataSection.Flags = Data.Flags;
    F.Image.Sections.push_back(DataSection);
    F.Image.MachOHasChainedFixups = true;
    F.Image.DataPtrRelocSlots.insert(Metadata + 8);
    F.Image.MachOResolvedChainedPointerSlots.insert(Metadata + 8);
    F.Image.DataPtrRelocTargetOwners[Metadata + 8] = 0x4000;
    F.Image.Symbols.push_back({MetadataName, Metadata, 0, false});

    Segment Code;
    Code.VA = Code.FileOff = 0x7000;
    Code.Size = Code.FileSz = 0x100;
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Code.Data.resize(0x100);
    llvm::support::endian::write32le(Code.Data.data() + 0x20, 0xf0ffffe0);
    llvm::support::endian::write32le(Code.Data.data() + 0x24, 0x91008000);
    llvm::support::endian::write32le(Code.Data.data() + 0x28, 0xd2800001);
    llvm::support::endian::write32le(Code.Data.data() + 0x2c, 0xd65f03c0);
    F.Image.Segments.push_back(Code);
    Section CodeSection;
    CodeSection.VA = CodeSection.FileOff = 0x7000;
    CodeSection.Size = CodeSection.FileSz = 0x100;
    CodeSection.Flags = Code.Flags;
    CodeSection.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    F.Image.Sections.push_back(CodeSection);
    F.Image.Symbols.push_back({AccessorName, Accessor, 0, true});
    F.Image.Exports.push_back({AccessorName, 0, Accessor});

    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = HighExpr::makeConst(Metadata, 8,
                                        ConstantAddressProvenance::DataAddress);
    F.Function.ReturnType = Return.RetVal->Type;
    F.Function.Body = {std::move(Return)};
    return F;
  };

  auto F = Fixture();
  ASSERT_TRUE(readImmutableImageBytes(F.Image, Metadata, 8));
  ASSERT_EQ(readImmutableImagePointer(F.Image, Metadata + 8),
            F.LocalDescriptor);
  ASSERT_TRUE(readImmutableImageBytes(F.Image, F.LocalDescriptor, 20));
  ASSERT_TRUE(readImmutableCodeBytes(F.Image, Accessor, 16));
  ASSERT_TRUE(objc_binding_detail::swiftPrivateNominalMetadataAccessorHint(
      F.Image, Metadata));
  auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  ASSERT_EQ(Result.SwiftPrivateNominalMetadataAccessors.size(), 1U);
  EXPECT_EQ(Result.SwiftPrivateNominalMetadataAccessors.at(Metadata),
            AccessorName);
  const auto Bound = Result.Function.Body[0].RetVal;
  ASSERT_TRUE(Bound->SourceCallHint);
  EXPECT_EQ(
      Bound->SourceCallHint->CallKind,
      SourceCallTypeHint::Kind::RuntimeSwiftPrivateNominalMetadataAddress);
  EXPECT_TRUE(objcSourceCallBound(*Bound, F.Image, {}));
  std::set<std::string> Helpers;
  const auto Source = renderObjCSwiftPrivateNominalMetadataHelpers(
      F.Image, Result.SwiftPrivateNominalMetadataAccessors, Helpers);
  EXPECT_NE(Source.find("__asm__(\"" + AccessorName + "\")"),
            std::string::npos);
  EXPECT_NE(Source.find("_accessor(0)"), std::string::npos);
  EXPECT_TRUE(
      Helpers.count("neverd_swift_private_nominal_metadata_6020_address"));

  // Value-witness arguments take a dedicated binding path before the general
  // expression walker. They must acquire the same private identity helper.
  auto WitnessFixture = Fixture();
  const auto Witness = swiftValueWitnessSourceCallHint(
      Arch::AArch64, SourceCallTypeHint::SwiftValueWitnessKind::Destroy);
  ASSERT_TRUE(Witness);
  auto Value = HighExpr::makeConst(0, 8);
  Value->Type = NdType::makePtr(NdType::makeVoid());
  auto Identity =
      HighExpr::makeConst(Metadata, 8, ConstantAddressProvenance::DataAddress);
  Identity->Type = NdType::makePtr(NdType::makeVoid());
  auto Call = HighExpr::makeCall("indirect_call", 0,
                                 {std::move(Value), std::move(Identity)});
  Call->Type = NdType::makeVoid();
  Call->IsIndirectCall = true;
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Witness);
  HighStmt Statement;
  Statement.Kind = StmtKind::ExprStmt;
  Statement.Val = std::move(Call);
  WitnessFixture.Function.ReturnType = NdType::makeVoid();
  WitnessFixture.Function.Body = {std::move(Statement)};
  const auto WitnessBound =
      bindObjCSourceReferences(WitnessFixture.Function, WitnessFixture.Image);
  ASSERT_TRUE(WitnessBound.Limitation.empty()) << WitnessBound.Limitation;
  ASSERT_EQ(WitnessBound.SwiftPrivateNominalMetadataAccessors.size(), 1U);
  const auto WitnessIdentity =
      WitnessBound.Function.Body[0].Val->Operands.back();
  ASSERT_TRUE(WitnessIdentity->SourceCallHint);
  EXPECT_EQ(
      WitnessIdentity->SourceCallHint->CallKind,
      SourceCallTypeHint::Kind::RuntimeSwiftPrivateNominalMetadataAddress);
  EXPECT_TRUE(objcSourceCallBound(*WitnessIdentity, WitnessFixture.Image, {}));

  // The published binding is invalidated by a changed machine return.
  llvm::support::endian::write32le(F.Image.Segments.back().Data.data() + 0x24,
                                   0x91008400);
  EXPECT_FALSE(objcSourceCallBound(*Bound, F.Image, {}));
  EXPECT_THROW(
      renderObjCSwiftPrivateNominalMetadataHelpers(
          F.Image, Result.SwiftPrivateNominalMetadataAccessors, Helpers),
      std::runtime_error);
  auto Forged = Fixture();
  Forged.Image.Exports.clear();
  EXPECT_TRUE(bindObjCSourceReferences(Forged.Function, Forged.Image)
                  .SwiftPrivateNominalMetadataAccessors.empty());
  Forged = Fixture();
  Forged.Image.Exports.push_back({MetadataName, 0, Metadata});
  EXPECT_TRUE(bindObjCSourceReferences(Forged.Function, Forged.Image)
                  .SwiftPrivateNominalMetadataAccessors.empty());
  Forged = Fixture();
  llvm::support::endian::write32le(Forged.Image.Segments[3].Data.data() + 0x20,
                                   0x52);
  EXPECT_TRUE(bindObjCSourceReferences(Forged.Function, Forged.Image)
                  .SwiftPrivateNominalMetadataAccessors.empty());
  Forged = Fixture();
  Forged.Image.MachOResolvedChainedPointerSlots.clear();
  EXPECT_TRUE(bindObjCSourceReferences(Forged.Function, Forged.Image)
                  .SwiftPrivateNominalMetadataAccessors.empty());
  Forged = Fixture();
  Forged.Image.CodePtrRelocSlots.insert(Accessor);
  EXPECT_TRUE(bindObjCSourceReferences(Forged.Function, Forged.Image)
                  .SwiftPrivateNominalMetadataAccessors.empty());
  Forged = Fixture();
  Forged.Image.Symbols.push_back({"_other", Accessor + 4, 0, true});
  EXPECT_TRUE(bindObjCSourceReferences(Forged.Function, Forged.Image)
                  .SwiftPrivateNominalMetadataAccessors.empty());
}

TEST(ObjCSourceBindings, SwiftEnumMetadataNeedsMatchingExportedDescriptor) {
  constexpr va_t Metadata = 0x6020;
  const std::string MetadataName = "_$s7WMFData24WMFFeatureConfigResponseON";
  const std::string DescriptorName = "_$s7WMFData24WMFFeatureConfigResponseOMn";
  auto Fixture = [&] {
    SwiftTypeMetadataFixture F(Arch::AArch64, false, false, false, true);
    for (auto &Symbol : F.Image.Symbols)
      if (Symbol.Addr == F.LocalDescriptor)
        Symbol.Name = DescriptorName;
    for (auto &Export : F.Image.Exports)
      if (Export.Addr == F.LocalDescriptor)
        Export.Name = DescriptorName;
    Segment Data;
    Data.VA = Data.FileOff = 0x6000;
    Data.Size = Data.FileSz = 0x100;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.ReadOnlyAfterRelocations = true;
    Data.Data.resize(0x100);
    llvm::support::endian::write64le(Data.Data.data() + 0x20, 0x201);
    llvm::support::endian::write64le(Data.Data.data() + 0x28,
                                     F.LocalDescriptor);
    F.Image.Segments.push_back(Data);
    Section Section;
    Section.VA = Section.FileOff = 0x6000;
    Section.Size = Section.FileSz = 0x100;
    Section.Flags = Data.Flags;
    F.Image.Sections.push_back(Section);
    F.Image.MachOHasChainedFixups = true;
    F.Image.DataPtrRelocSlots.insert(Metadata + 8);
    F.Image.MachOResolvedChainedPointerSlots.insert(Metadata + 8);
    F.Image.DataPtrRelocTargetOwners[Metadata + 8] = 0x4000;
    F.Image.Symbols.push_back({MetadataName, Metadata, 0, false});
    F.Image.Exports.push_back({MetadataName, 0, Metadata});
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = HighExpr::makeConst(Metadata, 8,
                                        ConstantAddressProvenance::DataAddress);
    F.Function.ReturnType = NdType::makeInt(8, false);
    F.Function.Body = {std::move(Return)};
    return F;
  };

  auto F = Fixture();
  const auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  ASSERT_EQ(Result.SwiftNominalMetadata.size(), 1U);
  EXPECT_EQ(Result.SwiftNominalMetadata.at(Metadata), MetadataName);
  const auto Bound = Result.Function.Body[0].RetVal;
  ASSERT_TRUE(Bound->SourceCallHint);
  EXPECT_EQ(Bound->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeSwiftNominalMetadataAddress);
  EXPECT_TRUE(objcSourceCallBound(*Bound, F.Image, {}));
  std::set<std::string> Helpers;
  const auto Source = renderObjCSwiftNominalMetadataHelpers(
      F.Image, Result.SwiftNominalMetadata, Helpers);
  EXPECT_NE(Source.find("__asm__(\"" + MetadataName + "\")"),
            std::string::npos);

  F.Image.Exports.pop_back();
  EXPECT_FALSE(objcSourceCallBound(*Bound, F.Image, {}));
  F = Fixture();
  F.Image.Exports.push_back({"_alias", 0, Metadata});
  EXPECT_TRUE(bindObjCSourceReferences(F.Function, F.Image)
                  .SwiftNominalMetadata.empty());
  F = Fixture();
  llvm::support::endian::write64le(F.Image.Segments.back().Data.data() + 0x20,
                                   0x200);
  EXPECT_TRUE(bindObjCSourceReferences(F.Function, F.Image)
                  .SwiftNominalMetadata.empty());
  F = Fixture();
  F.Image.Symbols.back().Name = "_$s7WMFData24WMFFeatureConfigResponseVN";
  EXPECT_TRUE(bindObjCSourceReferences(F.Function, F.Image)
                  .SwiftNominalMetadata.empty());
  F = Fixture();
  llvm::support::endian::write64le(F.Image.Segments.back().Data.data() + 0x28,
                                   F.LocalDescriptor + 8);
  EXPECT_TRUE(bindObjCSourceReferences(F.Function, F.Image)
                  .SwiftNominalMetadata.empty());
  F = Fixture();
  F.Image.Segments.back().ReadOnlyAfterRelocations = false;
  EXPECT_TRUE(bindObjCSourceReferences(F.Function, F.Image)
                  .SwiftNominalMetadata.empty());
  F = Fixture();
  F.Function.Body[0].RetVal->ConstProvenance =
      ConstantAddressProvenance::Scalar;
  EXPECT_TRUE(bindObjCSourceReferences(F.Function, F.Image)
                  .SwiftNominalMetadata.empty());
}

TEST(ObjCSourceBindings,
     SwiftSingletonMetadataBindsExportedNominalDescriptorAddress) {
  SwiftSingletonDescriptorFixture F;
  EXPECT_TRUE(F.Image.isCodeAddress(F.CallAddress));
  EXPECT_TRUE(objc_binding_detail::swiftNominalDescriptorAddressHint(
      F.Image, F.LocalDescriptor));
  ASSERT_TRUE(F.Function.Body[0].RetVal->SourceCallHint);
  EXPECT_EQ(F.Function.Body[0].RetVal->SourceCallHint->TargetName,
            "swift_getSingletonMetadata");
  EXPECT_EQ(objc_binding_detail::constantAddress(
                *F.Function.Body[0].RetVal->Operands[1]),
            F.LocalDescriptor);
  auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  ASSERT_EQ(Result.SwiftNominalDescriptors.size(), 1U);
  EXPECT_EQ(Result.SwiftNominalDescriptors.at(F.LocalDescriptor),
            "_$s7WMFData24WMFFeatureConfigResponseVMn");
  const auto Call = Result.Function.Body[0].RetVal;
  ASSERT_EQ(Call->Operands.size(), 2U);
  const auto Descriptor = Call->Operands[1];
  ASSERT_TRUE(Descriptor->SourceCallHint);
  EXPECT_EQ(Descriptor->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeSwiftNominalDescriptorAddress);
  EXPECT_TRUE(objcSourceCallBound(*Descriptor, F.Image, {}));
  std::set<std::string> Helpers;
  const auto Source = renderObjCSwiftNominalDescriptorHelpers(
      F.Image, Result.SwiftNominalDescriptors, Helpers);
  EXPECT_NE(
      Source.find("__asm__(\"_$s7WMFData24WMFFeatureConfigResponseVMn\")"),
      std::string::npos);
  EXPECT_NE(Source.find("neverd_swift_nominal_descriptor_4020_address"),
            std::string::npos);
  F.Image.Exports.clear();
  EXPECT_FALSE(objcSourceCallBound(*Descriptor, F.Image, {}));
  EXPECT_THROW(renderObjCSwiftNominalDescriptorHelpers(
                   F.Image, Result.SwiftNominalDescriptors, Helpers),
               std::runtime_error);
}

TEST(ObjCSourceBindings, SwiftWitnessTableNeedsOneExactReadOnlyExport) {
  constexpr va_t Address = 0x6020;
  const std::string Symbol = "_$sSo12NSURLSessionC7WMFData13WMFURLSessionACWP";
  auto Fixture = [&] {
    BinaryImage Image;
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.MachOTwoLevelNamespace = true;
    Segment Data;
    Data.VA = Data.FileOff = 0x6000;
    Data.Size = Data.FileSz = 0x100;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.ReadOnlyAfterRelocations = true;
    Data.Data.resize(0x100);
    Image.Segments.push_back(Data);
    Section Section;
    Section.VA = Section.FileOff = 0x6000;
    Section.Size = Section.FileSz = 0x100;
    Section.Flags = Data.Flags;
    Image.Sections.push_back(Section);
    Image.Symbols.push_back({Symbol, Address, 0, false});
    Image.Exports.push_back({Symbol, 0, Address});
    return Image;
  };
  auto Image = Fixture();
  HighFunc Function;
  Function.ReturnType = NdType::makeInt(8, false);
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal =
      HighExpr::makeConst(Address, 8, ConstantAddressProvenance::DataAddress);
  Function.Body = {Return};
  auto Result = bindObjCSourceReferences(Function, Image);
  ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  ASSERT_EQ(Result.SwiftWitnessTables.size(), 1U);
  EXPECT_EQ(Result.SwiftWitnessTables.at(Address), Symbol);
  const auto Bound = Result.Function.Body[0].RetVal;
  ASSERT_TRUE(Bound->SourceCallHint);
  EXPECT_EQ(Bound->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeSwiftWitnessTableAddress);
  EXPECT_TRUE(objcSourceCallBound(*Bound, Image, {}));
  std::set<std::string> Helpers;
  const auto Source = renderObjCSwiftWitnessTableHelpers(
      Image, Result.SwiftWitnessTables, Helpers);
  EXPECT_NE(Source.find("__asm__(\"" + Symbol + "\")"), std::string::npos);
  EXPECT_TRUE(Helpers.count("neverd_swift_witness_table_6020_address"));

  Image.Exports.clear();
  EXPECT_FALSE(objcSourceCallBound(*Bound, Image, {}));
  EXPECT_FALSE(
      objc_binding_detail::swiftWitnessTableAddressHint(Image, Address));
  Image = Fixture();
  Image.Exports.push_back({"_alias", 0, Address});
  EXPECT_FALSE(
      objc_binding_detail::swiftWitnessTableAddressHint(Image, Address));
  Image = Fixture();
  Image.Segments[0].ReadOnlyAfterRelocations = false;
  EXPECT_FALSE(
      objc_binding_detail::swiftWitnessTableAddressHint(Image, Address));
  Image = Fixture();
  Image.Symbols[0].Name = "_$s7WMFData13WMFURLSessionMp";
  EXPECT_FALSE(
      objc_binding_detail::swiftWitnessTableAddressHint(Image, Address));
  Image = Fixture();
  Function.Body[0].RetVal->ConstProvenance = ConstantAddressProvenance::Scalar;
  EXPECT_TRUE(
      bindObjCSourceReferences(Function, Image).SwiftWitnessTables.empty());
}

namespace {
struct PrivateSwiftWitnessFixture {
  static constexpr va_t Conformance = 0x4060, Protocol = 0x40a0;
  static constexpr va_t Type = 0x4100, Module = 0x4140, Registration = 0x4300;
  static constexpr va_t Metadata = 0x6000, Table = 0x6420,
                        ProtocolSlot = 0x6470;
  const std::string Name = "_$s14WitnessFixture5StoreCAA5ProtoAAWP";
  const std::string ProtocolName = "_$s14WitnessFixture5ProtoMp";
  const std::string ClassName = "_TtC14WitnessFixture5Store";
  BinaryImage Image;
  HighFunc Function;

  explicit PrivateSwiftWitnessFixture(bool IndirectProtocol = true) {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.MachOTwoLevelNamespace = true;
    for (va_t Base : {va_t(0x4000), va_t(0x6000)}) {
      Segment Mapping;
      Mapping.VA = Mapping.FileOff = Base;
      Mapping.Size = Mapping.FileSz = Base == 0x4000 ? 0x400 : 0x1000;
      Mapping.Flags = SegmentFlags::Readable;
      if (Base == 0x6000)
        Mapping.Flags = Mapping.Flags | SegmentFlags::Writable;
      Mapping.ReadOnlyAfterRelocations = true;
      Mapping.Data.resize(Mapping.Size);
      Image.Segments.push_back(Mapping);
      Section S;
      S.VA = S.FileOff = Base;
      S.Size = S.FileSz = Base == 0x4000 ? 0x300 : 0x1000;
      S.Flags = Mapping.Flags;
      Image.Sections.push_back(S);
    }
    Section Records;
    Records.Name = "__swift5_proto";
    Records.VA = Records.FileOff = Registration;
    Records.Size = Records.FileSz = 4;
    Records.Flags = SegmentFlags::Readable;
    Image.Sections.push_back(Records);
    relative(Conformance, IndirectProtocol ? ProtocolSlot : Protocol,
             IndirectProtocol);
    relative(Conformance + 4, Type);
    relative(Conformance + 8, Table);
    put32(Conformance + 12, 0);
    put32(Protocol, 0x10043);
    relative(Protocol + 4, Module);
    relative(Protocol + 8, 0x41d0);
    put32(Type, 0x80000050);
    relative(Type + 4, Module);
    relative(Type + 8, 0x41e0);
    put32(Module, 0);
    relative(Module + 8, 0x41f0);
    text(0x41d0, "Proto");
    text(0x41e0, "Store");
    text(0x41f0, "WitnessFixture");
    relative(Registration, Conformance);
    pointer(Table, Conformance);
    pointer(ProtocolSlot, Protocol);
    pointer(Metadata + 64, Type);
    pointer(Metadata + 32, 0x6100);
    pointer(0x6118, 0x6300);
    text(0x6300, ClassName);
    ObjCClass Class;
    Class.Name = ClassName;
    Class.Address = Metadata;
    Image.ObjCClasses.push_back(Class);
    Image.Symbols.push_back({Name, Table, 0, false});
    Image.Symbols.push_back(
        {Name.substr(0, Name.size() - 2) + "Mc", Conformance, 0, false});
    Image.Symbols.push_back({ProtocolName, Protocol, 0, false});
    Image.Exports.push_back({ProtocolName, 0, Protocol});
    Function.ReturnType = NdType::makePtr(NdType::makeVoid());
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal =
        HighExpr::makeConst(Table, 8, ConstantAddressProvenance::DataAddress);
    Function.Body = {Return};
  }
  uint8_t *bytes(va_t Address) {
    for (auto &Segment : Image.Segments)
      if (Address >= Segment.VA && Address < Segment.VA + Segment.Size)
        return Segment.Data.data() + Address - Segment.VA;
    return nullptr;
  }
  void put32(va_t Address, uint32_t Value) {
    llvm::support::endian::write32le(bytes(Address), Value);
  }
  void relative(va_t Address, va_t Target, bool Indirect = false) {
    put32(Address, uint32_t(Target - Address) | unsigned(Indirect));
  }
  void pointer(va_t Address, va_t Target) {
    llvm::support::endian::write64le(bytes(Address), Target);
    Image.DataPtrRelocSlots.insert(Address);
    Image.DataPtrRelocTargetOwners[Address] = Image.getSectionFor(Target)->VA;
  }
  void text(va_t Address, const std::string &Value) {
    std::memcpy(bytes(Address), Value.c_str(), Value.size() + 1);
  }
};
} // namespace

TEST(ObjCSourceBindings, PrivateSwiftWitnessUsesRegisteredClassConformance) {
  for (bool Indirect : {false, true}) {
    PrivateSwiftWitnessFixture F(Indirect);
    const auto Result = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
    ASSERT_EQ(Result.SwiftWitnessTables.size(), 1U);
    const auto Bound = Result.Function.Body[0].RetVal;
    ASSERT_TRUE(Bound->SourceCallHint);
    EXPECT_TRUE(objcSourceCallBound(*Bound, F.Image, {}));
    std::set<std::string> Helpers;
    const auto Source = renderObjCSwiftWitnessTableHelpers(
        F.Image, Result.SwiftWitnessTables, Helpers);
    EXPECT_NE(Source.find("_swift_conformsToProtocol"), std::string::npos);
    EXPECT_NE(Source.find(F.ProtocolName), std::string::npos);
    EXPECT_NE(Source.find("objc_getClass(\"" + F.ClassName + "\")"),
              std::string::npos);
    EXPECT_EQ(Source.find("__asm__(\"" + F.Name + "\")"), std::string::npos);
    F.Image.Exports.clear();
    EXPECT_FALSE(objcSourceCallBound(*Bound, F.Image, {}));
    EXPECT_THROW(renderObjCSwiftWitnessTableHelpers(
                     F.Image, Result.SwiftWitnessTables, Helpers),
                 std::runtime_error);
  }
}

TEST(ObjCSourceBindings, PrivateSwiftWitnessFindsRegisteredInternalProtocol) {
  for (bool Indirect : {false, true}) {
    PrivateSwiftWitnessFixture F(Indirect);
    F.Image.Exports.clear();
    auto Records = F.Image.Sections.back();
    Records.Name = "__swift5_protos";
    Records.VA = Records.FileOff = 0x4310;
    F.Image.Sections.push_back(Records);
    F.relative(Records.VA, F.Protocol);
    const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_EQ(Bound.SwiftWitnessTables.size(), 1U);
    const auto &Expression = Bound.Function.Body.front().RetVal;
    ASSERT_TRUE(Expression->SourceCallHint);
    EXPECT_TRUE(objcSourceCallBound(*Expression, F.Image, {}));
    std::set<std::string> Helpers;
    const auto Source = renderObjCSwiftWitnessTableHelpers(
        F.Image, Bound.SwiftWitnessTables, Helpers);
    EXPECT_NE(Source.find("swift_getTypeByMangledNameInContext"),
              std::string::npos);
    EXPECT_NE(Source.find("14WitnessFixture5Proto_p"), std::string::npos);
    EXPECT_EQ(Source.find("__asm__(\"" + F.ProtocolName + "\")"),
              std::string::npos);
    F.relative(Records.VA, F.Type);
    EXPECT_FALSE(objcSourceCallBound(*Expression, F.Image, {}));
    EXPECT_THROW(renderObjCSwiftWitnessTableHelpers(
                     F.Image, Bound.SwiftWitnessTables, Helpers),
                 std::runtime_error);
  }
}

TEST(ObjCSourceBindings,
     PrivateSwiftWitnessRejectsChangedProtocolRegistration) {
  for (unsigned Mutation = 0; Mutation < 20; ++Mutation) {
    SCOPED_TRACE(Mutation);
    PrivateSwiftWitnessFixture F;
    F.Image.Exports.clear();
    auto Records = F.Image.Sections.back();
    Records.Name = "__swift5_protos";
    Records.VA = Records.FileOff = 0x4310;
    F.Image.Sections.push_back(Records);
    F.relative(Records.VA, F.Protocol);
    const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_EQ(Bound.SwiftWitnessTables.size(), 1U);
    switch (Mutation) {
    case 0:
      F.Image.Sections.pop_back();
      break;
    case 1:
      F.relative(Records.VA, F.Type);
      break;
    case 2:
      F.relative(Records.VA, F.Protocol, true);
      break;
    case 3:
      F.Image.Sections.back().Size = F.Image.Sections.back().FileSz = 8;
      F.relative(Records.VA + 4, 0x4180);
      F.put32(0x4180, 0x10043);
      F.relative(0x4184, F.Module);
      F.relative(0x4188, 0x41d0);
      break;
    case 4:
      F.Image.Symbols.push_back({F.ProtocolName, 0x4180, 0, false});
      break;
    case 5:
      F.text(0x41d0, "Other");
      break;
    case 6:
      F.text(0x41f0, "OtherFixture");
      break;
    case 7:
      F.put32(F.Protocol, 0x100c3);
      break;
    case 8:
      F.put32(F.Protocol + 16, 513);
      break;
    case 9:
      F.put32(F.Protocol + 12, 1);
      break;
    case 10:
      F.relative(F.Protocol + 20, 0x41d0);
      break;
    case 11:
      F.Image.Segments[0].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
      F.Image.Segments[0].ReadOnlyAfterRelocations = false;
      break;
    case 12:
      F.Image.Exports.push_back({F.ProtocolName, 0, F.Protocol + 4});
      break;
    case 13:
      F.Image.Sections.push_back(Records);
      break;
    case 14:
      F.Image.Sections.back().FileSz = 3;
      break;
    case 15:
      F.relative(Records.VA, 0x2000);
      break;
    case 16:
      F.put32(F.Protocol, 0x10143);
      break;
    case 17:
      F.put32(F.Protocol, 0x50043);
      break;
    case 18:
      F.Image.Symbols.push_back(
          {"_$s14WitnessFixture5OtherMp", F.Protocol, 0, false});
      break;
    case 19:
      F.Image.MachOChainedFixupsAmbiguous = true;
      break;
    }
    EXPECT_FALSE(
        objc_binding_detail::swiftWitnessTableAddressHint(F.Image, F.Table));
    EXPECT_FALSE(
        objcSourceCallBound(*Bound.Function.Body.front().RetVal, F.Image, {}));
    std::set<std::string> Helpers;
    EXPECT_THROW(renderObjCSwiftWitnessTableHelpers(
                     F.Image, Bound.SwiftWitnessTables, Helpers),
                 std::runtime_error);
  }
}

TEST(ObjCSourceBindings, PrivateSwiftWitnessRejectsIncompleteIdentityProofs) {
  for (unsigned Mutation = 0; Mutation < 32; ++Mutation) {
    SCOPED_TRACE(Mutation);
    PrivateSwiftWitnessFixture F;
    switch (Mutation) {
    case 0:
      F.put32(F.Conformance + 12, 0x100);
      break;
    case 1:
      F.relative(F.Conformance + 8, F.Table + 8);
      break;
    case 2:
      F.pointer(F.Table, F.Conformance + 16);
      break;
    case 3:
      F.pointer(F.Metadata + 64, F.Type + 4);
      break;
    case 4:
      F.put32(F.Type, 0xd0);
      break;
    case 5:
      F.text(0x41e0, "Other");
      break;
    case 6:
      F.text(0x41d0, "Other");
      break;
    case 7:
      F.text(0x41f0, "ForeignModule");
      break;
    case 8:
      F.Image.ObjCClasses.push_back(F.Image.ObjCClasses[0]);
      break;
    case 9:
      F.Image.Exports.clear();
      break;
    case 10:
      F.Image.Exports.push_back({"_alias", 0, F.Protocol});
      break;
    case 11:
      F.Image.Sections.back().Name = "__other";
      break;
    case 12:
      F.relative(F.Registration, F.Conformance + 16);
      break;
    case 13:
      F.Image.Sections.back().Size = F.Image.Sections.back().FileSz = 8;
      F.relative(F.Registration + 4, F.Conformance);
      break;
    case 14:
      F.Image.Segments[1].ReadOnlyAfterRelocations = false;
      break;
    case 15:
      F.Image.DataPtrRelocTargetOwners.erase(F.ProtocolSlot);
      break;
    case 16:
      F.put32(F.Conformance + 12, 0x80);
      break;
    case 17:
      F.put32(F.Conformance + 12, 0x40);
      break;
    case 18:
      F.put32(F.Conformance + 12, 0x20000);
      break;
    case 19:
      F.put32(F.Type + 4, 1);
      break;
    case 20:
      F.Image.Sections.push_back(F.Image.Sections[0]);
      break;
    case 21:
      F.Image.Symbols.back().Name = "_$s14WitnessFixture5OtherMp";
      F.Image.Exports[0].Name = F.Image.Symbols.back().Name;
      break;
    case 22:
      F.text(0x6300, "_TtC14WitnessFixture5Other");
      break;
    case 23:
    case 24:
    case 25:
    case 26:
      F.Image.Sections.back().Size = F.Image.Sections.back().FileSz = 8;
      F.relative(F.Registration + 4, 0x4080);
      F.relative(0x4080, F.Protocol);
      F.relative(0x4084, F.Type);
      F.relative(0x4088, F.Table + 16);
      F.put32(0x408c, (Mutation - 23) << 3);
      if (Mutation == 24 || Mutation == 26) {
        F.relative(0x4084, 0x64a0);
        F.pointer(0x64a0, Mutation == 24 ? F.Type : F.Metadata);
      } else if (Mutation == 25) {
        F.relative(0x4084, 0x41a0);
        F.text(0x41a0, F.ClassName);
      }
      break;
    case 27:
      F.Image.Sections.back().FileSz = 3;
      break;
    case 28:
      F.Image.MachOChainedFixupsAmbiguous = true;
      break;
    case 29:
      F.put32(F.Module + 4, 4);
      break;
    case 30:
      F.put32(F.Type, 0x80000150);
      break;
    case 31:
      F.Image.DataPtrRelocSlots.insert(F.Conformance);
      break;
    }
    EXPECT_FALSE(
        objc_binding_detail::swiftWitnessTableAddressHint(F.Image, F.Table));
  }
}

TEST(ObjCSourceBindings, PrivateSwiftWitnessDistinguishesImportedTypeRecords) {
  PrivateSwiftWitnessFixture F;
  F.Image.Sections[2].Size = F.Image.Sections[2].FileSz = 8;
  F.relative(F.Registration + 4, 0x4080);
  F.relative(0x4080, F.Protocol);
  F.relative(0x4084, 0x64a0);
  F.relative(0x4088, F.Table + 16);
  F.put32(0x408c, 8); // Indirect nominal descriptor.
  F.Image.Sections[1].Size = F.Image.Sections[1].FileSz = 0x4a0;
  Section Got;
  Got.Name = "__got";
  Got.VA = Got.FileOff = 0x64a0;
  Got.Size = Got.FileSz = 8;
  Got.Flags = SegmentFlags::Readable;
  Got.Type = llvm::MachO::S_NON_LAZY_SYMBOL_POINTERS;
  F.Image.Sections.push_back(Got);
  const std::string Name = "_$s10Foundation3URLVMn";
  F.Image.ImportPtrSlots[Got.VA] = Name;
  F.Image.DyldBindSlots[Got.VA] = {
      Name, 0, "/System/Library/Frameworks/Foundation.framework/Foundation",
      false};
  ASSERT_TRUE(
      objc_binding_detail::swiftWitnessTableAddressHint(F.Image, F.Table));
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    auto Image = F.Image;
    if (Mutation == 0)
      Image.Exports.push_back({"_exported_type", 0, F.Type});
    if (Mutation == 1)
      Image.Exports.push_back({"_exported_metadata", 0, F.Metadata});
    if (Mutation == 2)
      Image.DyldBindSlots[Got.VA].WeakImport = true;
    if (Mutation == 3)
      Image.ConflictingImportStorageSlots.insert(Got.VA);
    EXPECT_FALSE(
        objc_binding_detail::swiftWitnessTableAddressHint(Image, F.Table))
        << Mutation;
  }
}

static void checkPrivateSwiftWitnessHelper(bool InternalProtocol) {
  PrivateSwiftWitnessFixture F;
  if (InternalProtocol) {
    F.Image.Exports.clear();
    auto Records = F.Image.Sections.back();
    Records.Name = "__swift5_protos";
    Records.VA = Records.FileOff = 0x4310;
    F.Image.Sections.push_back(Records);
    F.relative(Records.VA, F.Protocol);
  }
  const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_EQ(Bound.SwiftWitnessTables.size(), 1U);
  std::set<std::string> Helpers;
  std::string Source = "#include <stdint.h>\n#include <string.h>\n" +
                       renderObjCSwiftWitnessTableHelpers(
                           F.Image, Bound.SwiftWitnessTables, Helpers);
  Source +=
      "\n#define INTERNAL_PROTOCOL " + std::to_string(InternalProtocol) + "\n";
  Source += R"(
static unsigned char metadata, table;
_Alignas(8) unsigned char protocol[8] __asm__("_$s14WitnessFixture5ProtoMp") = {0};
static unsigned queries, classes, lookups;
static struct {
  uintptr_t kind;
  uint32_t flags, count;
  const void *protocol;
} existential = {0x303, 0x80000001, 1, protocol};
const void *lookup(const char *, uintptr_t, const void *, const void *)
    __asm__("_swift_getTypeByMangledNameInContext");
const void *lookup(const char *name, uintptr_t length, const void *context,
                   const void *arguments) {
  const char expected[] = "14WitnessFixture5Proto_p";
  if (length != sizeof(expected) - 1 || memcmp(name, expected, length) ||
      context || arguments) __builtin_trap();
  ++lookups;
  return &existential;
}
void *objc_getClass(const char *name) {
  if (strcmp(name, "_TtC14WitnessFixture5Store")) __builtin_trap();
  ++classes;
  return &metadata;
}
const void *query(const void *, const void *) __asm__("_swift_conformsToProtocol");
const void *query(const void *type, const void *descriptor) {
  if (type != &metadata || descriptor != protocol) __builtin_trap();
  ++queries;
  return &table;
}
int main(void) {
  for (unsigned i = 0; i < 1024; ++i)
    if ((void *)neverd_swift_witness_table_6420_address() != &table) return 1;
  return queries == 1024 && classes == 1024 &&
         lookups == (INTERNAL_PROTOCOL ? 1024 : 0) ? 0 : 2;
}
)";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-private-witness",
                                                    Directory));
  const std::filesystem::path Work(Directory.str().str());
  struct Cleanup {
    std::filesystem::path Work;
    ~Cleanup() {
      std::error_code Error;
      std::filesystem::remove_all(Work, Error);
    }
  } Cleanup{Work};
  std::filesystem::create_directories(Work / "objc");
  std::ofstream(Work / "objc/runtime.h")
      << "void *objc_getClass(const char *);\n";
  const auto Path = (Work / "witness.c").string();
  const auto Executable = (Work / "witness").string();
  const auto ErrorPath = (Work / "stderr").string();
  std::ofstream(Path) << Source;
  const std::string Compiler = NEVERD_TEST_CLANG;
  for (const char *Optimization : {"-O0", "-O2"}) {
    const std::vector<std::string> Arguments{
        Compiler,      "-std=c11", Optimization, "-Werror", "-I",
        Work.string(), Path,       "-o",         Executable};
    std::vector<llvm::StringRef> Refs(Arguments.begin(), Arguments.end());
    const std::optional<llvm::StringRef> Redirects[] = {
        std::nullopt, std::nullopt, ErrorPath};
    std::string Error;
    const auto Status = llvm::sys::ExecuteAndWait(Compiler, Refs, std::nullopt,
                                                  Redirects, 60, 0, &Error);
    auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Status, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "");
    EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable}, std::nullopt,
                                        Redirects, 30, 0, &Error),
              0)
        << Error;
  }
}

TEST(ObjCSourceBindings, PrivateSwiftWitnessHelperPreservesRuntimeIdentity) {
  checkPrivateSwiftWitnessHelper(false);
}

TEST(ObjCSourceBindings,
     PrivateSwiftWitnessInternalProtocolHelperPreservesRuntimeIdentity) {
  checkPrivateSwiftWitnessHelper(true);
}

TEST(ObjCSourceBindings, SwiftSingletonMetadataRejectsForgedProvider) {
  SwiftSingletonDescriptorFixture F;
  F.Image.DyldBindSlots[F.RuntimeSlot].Module = "/tmp/foreign.dylib";
  const auto Result = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_TRUE(Result.SwiftNominalDescriptors.empty());
}

TEST(ObjCSourceBindings,
     SwiftSingletonMetadataBindsDescriptorThroughProvenNativeHelper) {
  SwiftSingletonDescriptorFixture F;
  constexpr va_t HelperAddress = 0x5050;
  HighFunc Helper;
  Helper.Entry = HelperAddress;
  Helper.ReturnType = NdType::makePtr(NdType::makeVoid());
  Helper.Params = {{"request", NdType::makeInt(8)},
                   {"cache", NdType::makePtr(NdType::makeVoid())},
                   {"descriptor", NdType::makePtr(NdType::makeVoid())}};
  SourceFunctionTypeHint Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Signature.ReturnType = Helper.ReturnType;
  for (const auto &Parameter : Helper.Params)
    Signature.Parameters.push_back({Parameter.Name, Parameter.Type});
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(Signature, F.Image.Arch, Error))
      << Error;
  Helper.SourceTypeHint = Signature;
  auto Parameter = [](int Id, TypeRef Type) {
    MedVar Variable;
    Variable.Kind = MedVar::Param;
    Variable.Id = Id;
    Variable.Size = 8;
    return HighExpr::makeVar(Variable, Type);
  };
  auto Runtime = swiftRuntimeSourceCallHint(F.Image, F.RuntimeSlot);
  ASSERT_TRUE(Runtime);
  auto RuntimeCall =
      HighExpr::makeCall("swift_getSingletonMetadata", F.CallAddress,
                         {Parameter(0, Helper.Params[0].Type),
                          Parameter(2, Helper.Params[2].Type)});
  RuntimeCall->Type = Runtime->Signature.ReturnType;
  RuntimeCall->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Runtime);
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeRecordField(RuntimeCall, 0, 8);
  Helper.Body = {Return};
  const auto HelperBody = Helper.Body;
  auto Native = std::make_shared<SourceCallTypeHint>();
  Native->CallKind = SourceCallTypeHint::Kind::Native;
  Native->TargetAddress = HelperAddress;
  Native->Signature = Signature;
  auto CallerCall = HighExpr::makeCall(
      "native_metadata_helper", HelperAddress,
      {HighExpr::makeConst(0, 8, ConstantAddressProvenance::Scalar),
       HighExpr::makeConst(F.Cache, 8, ConstantAddressProvenance::DataAddress),
       HighExpr::makeConst(F.LocalDescriptor, 8,
                           ConstantAddressProvenance::DataAddress)});
  CallerCall->Type = Helper.ReturnType;
  CallerCall->SourceCallHint = Native;
  HighFunc Caller;
  Caller.Entry = 0x5070;
  Caller.ReturnType = Helper.ReturnType;
  Return.RetVal = CallerCall;
  Caller.Body = {Return};
  const std::map<va_t, const HighFunc *> Functions{{HelperAddress, &Helper}};
  const auto Bind = [&] {
    return bindObjCSourceReferences(Caller, F.Image, nullptr, &Functions);
  };
  auto Result = Bind();
  ASSERT_EQ(Result.SwiftNominalDescriptors.size(), 1U);
  EXPECT_EQ(Result.SwiftNominalDescriptors.at(F.LocalDescriptor),
            "_$s7WMFData24WMFFeatureConfigResponseVMn");
  auto Descriptor = Result.Function.Body[0].RetVal->Operands[2];
  ASSERT_TRUE(Descriptor->SourceCallHint);
  EXPECT_EQ(Descriptor->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeSwiftNominalDescriptorAddress);

  RuntimeCall->Operands[1] = Parameter(1, Helper.Params[1].Type);
  EXPECT_TRUE(Bind().SwiftNominalDescriptors.empty());
  RuntimeCall->Operands[1] = Parameter(2, Helper.Params[2].Type);
  Native->Signature.Parameters[2].Location.RegisterOffset =
      Native->Signature.Parameters[1].Location.RegisterOffset;
  EXPECT_TRUE(Bind().SwiftNominalDescriptors.empty());
  Native->Signature = Signature;
  Helper.Body.clear();
  EXPECT_TRUE(Bind().SwiftNominalDescriptors.empty());
  Helper.Body = HelperBody;
  F.Image.DyldBindSlots[F.RuntimeSlot].Module = "/tmp/foreign.dylib";
  EXPECT_TRUE(Bind().SwiftNominalDescriptors.empty());
  F.Image.DyldBindSlots[F.RuntimeSlot].Module =
      "/usr/lib/swift/libswiftCore.dylib";
  F.Image.Exports.clear();
  EXPECT_TRUE(Bind().SwiftNominalDescriptors.empty());
}

TEST(ObjCSourceBindings,
     SwiftConcreteTypeMetadataPairsRebuildFreshCacheAndRelativeReference) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (const bool CombineNominal : {false, true}) {
      SCOPED_TRACE(CombineNominal);
      SwiftTypeMetadataFixture F(Architecture, false, false, CombineNominal);
      // Swift private-linkage symbols repeat across compilation units. The
      // typed native call, not global symbol-name uniqueness, pairs the exact
      // cache and reference addresses used by this function.
      const std::string ReferenceName = CombineNominal
                                            ? "_$s7Combine9PublishedVySbGMR"
                                            : "_$s10Foundation3URLVSgMR";
      const std::string CacheName = CombineNominal
                                        ? "_$s7Combine9PublishedVySbGMd"
                                        : "_$s10Foundation3URLVSgMd";
      F.Image.Symbols.push_back({ReferenceName, 0x1090, 8, false});
      F.Image.Symbols.push_back({CacheName, 0x2030, 8, false});
      const auto Result = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
      ASSERT_EQ(Result.SwiftTypeMetadataPairs.size(), 1U);
      const auto Pair =
          Result.SwiftTypeMetadataPairs.at(SwiftTypeMetadataFixture::Cache);
      EXPECT_EQ(Pair.ReferenceAddress, SwiftTypeMetadataFixture::Reference);
      EXPECT_EQ(Pair.TypeReferenceAddress,
                SwiftTypeMetadataFixture::TypeReference);
      EXPECT_EQ(Pair.DescriptorSlot, SwiftTypeMetadataFixture::DescriptorSlot);
      const std::string Descriptor = CombineNominal ? "_$s7Combine9PublishedVMn"
                                                    : "_$s10Foundation3URLVMn";
      const std::string Suffix = CombineNominal ? "ySbG" : "Sg";
      EXPECT_EQ(Pair.DescriptorSymbol, Descriptor);
      EXPECT_EQ(Pair.Suffix, Suffix);

      const auto Call = Result.Function.Body[0].Val;
      ASSERT_EQ(Call->Operands.size(), 2U);
      for (size_t I = 0; I < 2; ++I) {
        const auto &Address = Call->Operands[I];
        ASSERT_TRUE(Address->SourceCallHint);
        EXPECT_EQ(Address->SourceCallHint->CallKind,
                  SourceCallTypeHint::Kind::RuntimeSwiftTypeMetadataAddress);
        EXPECT_EQ(Address->SourceCallHint->TargetAddress,
                  I ? SwiftTypeMetadataFixture::Reference
                    : SwiftTypeMetadataFixture::Cache);
        EXPECT_TRUE(objcSourceCallBound(*Address, F.Image, {}));
      }

      std::set<std::string> Helpers;
      const auto Source = renderObjCSwiftTypeMetadataHelpers(
          F.Image, Result.SwiftTypeMetadataPairs, Helpers);
      EXPECT_EQ(Helpers,
                (std::set<std::string>{
                    "neverd_swift_type_metadata_2020_1020_cache_address",
                    "neverd_swift_type_metadata_2020_1020_reference_address"}));
      EXPECT_NE(Source.find("__asm__(\"" + Descriptor + "\")"),
                std::string::npos);
      EXPECT_NE(Source.find(".type_reference[0] = 2"), std::string::npos);
      for (size_t I = 0; I < Suffix.size(); ++I)
        EXPECT_NE(Source.find(".type_reference[" + std::to_string(5 + I) +
                              "] = " + std::to_string(uint8_t(Suffix[I]))),
                  std::string::npos);
      EXPECT_NE(Source.find(".reference.length = " +
                            std::to_string(5 + Suffix.size())),
                std::string::npos);
      EXPECT_NE(Source.find("void *cache"), std::string::npos);
    }
}

TEST(ObjCSourceBindings,
     SwiftConcreteTypeMetadataPairsFollowUniqueLocalCallArguments) {
  SwiftTypeMetadataFixture F(Arch::AArch64);
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  MedVar CacheLocal;
  CacheLocal.Kind = MedVar::Temp;
  CacheLocal.Id = 501;
  CacheLocal.Size = 8;
  MedVar ReferenceLocal = CacheLocal;
  ReferenceLocal.Id = 502;
  auto Assign = [&](MedVar Local, va_t Address) {
    HighStmt Statement;
    Statement.Kind = StmtKind::Assign;
    Statement.Dst = HighExpr::makeVar(Local, Pointer);
    Statement.Val =
        HighExpr::makeConst(Address, 8, ConstantAddressProvenance::DataAddress);
    return Statement;
  };
  F.Function.Body.insert(F.Function.Body.begin(),
                         Assign(CacheLocal, SwiftTypeMetadataFixture::Cache));
  F.Function.Body.insert(
      F.Function.Body.begin() + 1,
      Assign(ReferenceLocal, SwiftTypeMetadataFixture::Reference));
  auto &Arguments = F.Function.Body.back().Val->Operands;
  Arguments[0] = HighExpr::makeVar(CacheLocal, Pointer);
  Arguments[1] = HighExpr::makeVar(ReferenceLocal, Pointer);
  const auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  ASSERT_EQ(Result.SwiftTypeMetadataPairs.size(), 1U);
  for (size_t I = 0; I < 2; ++I) {
    const auto &Value = Result.Function.Body[I].Val;
    ASSERT_TRUE(Value->SourceCallHint);
    EXPECT_EQ(Value->SourceCallHint->CallKind,
              SourceCallTypeHint::Kind::RuntimeSwiftTypeMetadataAddress);
    EXPECT_TRUE(objcSourceCallBound(*Value, F.Image, {}));
  }

  F.Function.Body.insert(F.Function.Body.end(),
                         Assign(CacheLocal, SwiftTypeMetadataFixture::Cache));
  const auto Reassigned = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_FALSE(Reassigned.Function.Body[0].Val->SourceCallHint);
  F.Function.Body.pop_back();

  HighStmt OtherUse;
  OtherUse.Kind = StmtKind::ExprStmt;
  OtherUse.Val = HighExpr::makeVar(CacheLocal, Pointer);
  F.Function.Body.push_back(OtherUse);
  const auto Escaped = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_FALSE(Escaped.Function.Body[0].Val->SourceCallHint);
  F.Function.Body.pop_back();

  std::swap(F.Function.Body[0], F.Function.Body[2]);
  const auto UseBeforeDefinition =
      bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_FALSE(UseBeforeDefinition.Function.Body[2].Val->SourceCallHint);
}

TEST(ObjCSourceBindings, SwiftStdlibDescriptorRebuildsExactMetadataRecipe) {
  auto F = swiftStdlibTypeMetadataFixture();
  const auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  ASSERT_EQ(Result.SwiftTypeMetadataPairs.size(), 1U);
  const auto &Pair = Result.SwiftTypeMetadataPairs.at(F.Cache);
  EXPECT_EQ(Pair.DescriptorSymbol, "_$ss23_ContiguousArrayStorageCMn");
  EXPECT_EQ(Pair.Suffix, "yypG");
  std::set<std::string> Helpers;
  const auto Source = renderObjCSwiftTypeMetadataHelpers(
      F.Image, Result.SwiftTypeMetadataPairs, Helpers);
  EXPECT_NE(Source.find(".reference.length = 9"), std::string::npos);
  EXPECT_NE(Source.find("_$ss23_ContiguousArrayStorageCMn"), std::string::npos);
  F.Image.DyldBindSlots[F.DescriptorSlot].WeakImport = true;
  EXPECT_FALSE(objcSourceCallBound(*Result.Function.Body[0].Val->Operands[0],
                                   F.Image, {}));
  EXPECT_THROW(renderObjCSwiftTypeMetadataHelpers(
                   F.Image, Result.SwiftTypeMetadataPairs, Helpers),
               std::runtime_error);
}

TEST(ObjCSourceBindings, SwiftManagedBufferDescriptorNeedsExactCoreBind) {
  auto F = swiftManagedBufferTypeMetadataFixture();
  const auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  ASSERT_EQ(Result.SwiftTypeMetadataPairs.size(), 1U);
  EXPECT_EQ(Result.SwiftTypeMetadataPairs.at(F.Cache).DescriptorSymbol,
            "_$ss13ManagedBufferCMn");
  const auto Address = Result.Function.Body[0].Val->Operands[1];
  ASSERT_TRUE(Address->SourceCallHint);
  EXPECT_TRUE(objcSourceCallBound(*Address, F.Image, {}));
  std::set<std::string> Helpers;
  const auto Source = renderObjCSwiftTypeMetadataHelpers(
      F.Image, Result.SwiftTypeMetadataPairs, Helpers);
  EXPECT_NE(Source.find("_$ss13ManagedBufferCMn"), std::string::npos);
  F.Image.DyldBindSlots[F.DescriptorSlot].Module = "/tmp/foreign.dylib";
  EXPECT_FALSE(objcSourceCallBound(*Address, F.Image, {}));
  EXPECT_THROW(renderObjCSwiftTypeMetadataHelpers(
                   F.Image, Result.SwiftTypeMetadataPairs, Helpers),
               std::runtime_error);
}

TEST(ObjCSourceBindings, SwiftStdlibDescriptorRejectsUnprovenImports) {
  for (unsigned Mutation = 0; Mutation != 18; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = swiftStdlibTypeMetadataFixture();
    auto &Bind = F.Image.DyldBindSlots[F.DescriptorSlot];
    switch (Mutation) {
    case 0:
      Bind.WeakImport = true;
      break;
    case 1:
      Bind.Addend = 8;
      break;
    case 2:
      Bind.Module = "/tmp/libswiftCore.dylib";
      break;
    case 3:
      Bind.Name = "_$ss22_ContiguousArrayBufferVMn";
      F.Image.ImportStorageSlots[F.DescriptorSlot].Name = Bind.Name;
      break;
    case 4:
      F.Image.DyldBindSlots.erase(F.DescriptorSlot);
      break;
    case 5:
      F.Image.ImportPtrSlots[F.DescriptorSlot] = "_different";
      break;
    case 6:
      F.Image.Arch = Arch::X64;
      break;
    case 7:
      F.Image.Segments[2].ReadOnlyAfterRelocations = false;
      break;
    case 8:
      F.Image.Sections[2].FileSz = 0x27;
      break;
    case 9:
      F.Image.Sections.push_back(F.Image.Sections[2]);
      break;
    case 10:
      F.Image.DataPtrRelocSlots.insert(F.DescriptorSlot);
      break;
    case 11:
      F.Image.CodePtrRelocSlots.insert(F.DescriptorSlot - 1);
      break;
    case 12:
      F.Image.MachOChainedFixupsAmbiguous = true;
      break;
    case 13:
      F.Image.Segments[1].Data[F.Cache - 0x2000] = 1;
      break;
    case 14:
      F.Image.Symbols[0].Name = "_$ss23_ContiguousArrayStorageCySiGMR";
      break;
    case 15:
      F.Image.Segments[0].Data[F.TypeReference + 5 - 0x1000] = 'x';
      break;
    case 16:
      F.Image.MachOTwoLevelNamespace = false;
      break;
    case 17:
      F.Image.ImportStorageSlots[F.DescriptorSlot].Addend = 8;
      break;
    }
    const auto Result = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_TRUE(Result.SwiftTypeMetadataPairs.empty());
    EXPECT_FALSE(Result.Limitation.empty());
  }
}

namespace {
SwiftTypeMetadataFixture swiftCoreDescriptorFixture(llvm::StringRef Descriptor,
                                                    llvm::StringRef Suffix) {
  auto F = swiftStdlibTypeMetadataFixture();
  F.Image.ImportPtrSlots.clear();
  F.Image.ImportStorageSlots.clear();
  F.Image.DyldBindSlots.clear();
  EXPECT_TRUE(F.Image.recordDyldBindSlot(F.DescriptorSlot, Descriptor.str(), 0,
                                         "/usr/lib/swift/libswiftCore.dylib",
                                         false));
  const std::string Base = Descriptor.drop_back(2).str() + Suffix.str();
  F.Image.Symbols[0].Name = Base + "MR";
  F.Image.Symbols[1].Name = Base + "Md";
  auto &Data = F.Image.Segments[0].Data;
  llvm::support::endian::write32le(Data.data() + F.Reference + 4 - 0x1000,
                                   5 + Suffix.size());
  auto *Type = Data.data() + F.TypeReference - 0x1000;
  std::memcpy(Type + 5, Suffix.data(), Suffix.size());
  Type[5 + Suffix.size()] = 0;
  return F;
}
} // namespace

TEST(ObjCSourceBindings,
     SwiftCoreDescriptorIdentitySupportsStructEnumClassAndProtocolRecipes) {
  for (const auto &[Descriptor, Suffix] :
       std::vector<std::pair<std::string, std::string>>{
           {"_$ss5UInt8VMn", "Sg"},
           {"_$ss5Int32VMn", "Sg"},
           {"_$ss6ResultOMn", "ys5UInt8Vs5NeverOG"},
           {"_$ss23_ContiguousArrayStorageCMn", "yypG"},
           {"_$ss5ErrorMp", "_pSg"},
           {"_$ss7CVarArgMp", "_pSg"}}) {
    SCOPED_TRACE(Descriptor);
    auto F = swiftCoreDescriptorFixture(Descriptor, Suffix);
    const auto Proof = objc_binding_detail::swiftTypeMetadataPairProof(
        F.Image, F.Cache, F.Reference);
    ASSERT_TRUE(Proof);
    ASSERT_EQ(Proof->Descriptors.size(), 1U);
    EXPECT_EQ(Proof->Descriptors[0].Symbol, Descriptor);
    const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    ASSERT_EQ(Bound.SwiftTypeMetadataPairs.size(), 1U);
    for (const auto &Arg : Bound.Function.Body[0].Val->Operands)
      EXPECT_TRUE(objcSourceCallBound(*Arg, F.Image, {}));
    std::set<std::string> Names;
    const auto Source = renderObjCSwiftTypeMetadataHelpers(
        F.Image, Bound.SwiftTypeMetadataPairs, Names);
    EXPECT_NE(Source.find("__asm__(\"" + Descriptor + "\")"),
              std::string::npos);
    EXPECT_EQ(Proof->Address.Suffix, Suffix);
  }
}

TEST(ObjCSourceBindings,
     SwiftCoreDescriptorIdentityRejectsWrongDeclarationOrStorage) {
  for (unsigned Mutation = 0; Mutation != 18; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = swiftCoreDescriptorFixture("_$ss5UInt8VMn", "Sg");
    const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_EQ(Bound.SwiftTypeMetadataPairs.size(), 1U);
    switch (Mutation) {
    case 0:
      F.Image.DyldBindSlots[F.DescriptorSlot].WeakImport = true;
      break;
    case 1:
      F.Image.DyldBindSlots[F.DescriptorSlot].Addend = 8;
      break;
    case 2:
      F.Image.DyldBindSlots[F.DescriptorSlot].Module =
          "/tmp/libswiftCore.dylib";
      break;
    case 3:
      F.Image.Segments[2].ReadOnlyAfterRelocations = false;
      break;
    case 4:
      F.Image.Sections.push_back(F.Image.Sections[2]);
      break;
    case 5:
      F.Image.DataPtrRelocSlots.insert(F.DescriptorSlot);
      break;
    case 6:
      F.Image.RelCodeRelocSlots.insert(F.DescriptorSlot + 1);
      break;
    case 7:
      F.Image.ImportPtrSlots[F.DescriptorSlot] = "_$ss5Int32VMn";
      break;
    case 8:
      F.Image.ImportStorageSlots[F.DescriptorSlot].Addend = 8;
      break;
    case 9:
      F.Image.MachOTwoLevelNamespace = false;
      break;
    case 10:
      F.Image.Arch = Arch::X64;
      break;
    case 11:
      F.Image.Segments[1].Data[0x20] = 1;
      break;
    case 12:
      F.Image.Symbols[0].Name = "_$ss5Int32VSgMR";
      break;
    case 13:
      F.Image.Segments[0].Data[0x85] = 'x';
      break;
    case 14:
      F.Image.DyldBindSlots.erase(F.DescriptorSlot);
      break;
    case 15:
      F.Image.ConflictingImportStorageSlots.insert(F.DescriptorSlot);
      break;
    case 16:
      F.Image.Sections[2].FileSz = 0x27;
      break;
    case 17:
      F.Image.MachOChainedFixupsAmbiguous = true;
      break;
    }
    EXPECT_FALSE(objc_binding_detail::swiftTypeMetadataPairProof(
        F.Image, F.Cache, F.Reference));
    EXPECT_TRUE(bindObjCSourceReferences(F.Function, F.Image)
                    .SwiftTypeMetadataPairs.empty());
    for (const auto &Arg : Bound.Function.Body[0].Val->Operands)
      EXPECT_FALSE(objcSourceCallBound(*Arg, F.Image, {}));
    std::set<std::string> Names;
    EXPECT_THROW(renderObjCSwiftTypeMetadataHelpers(
                     F.Image, Bound.SwiftTypeMetadataPairs, Names),
                 std::runtime_error);
  }
  for (const auto &Symbol :
       {"_$ss5UInt8VMa", "_$ss5UInt8VN", "_$ss5UInt8VMnX", "_$s4Demo5UInt8VMn",
        "_$sSo7CGPointVMn", "_$ss5UInt8V5InnerVMn"}) {
    auto F = swiftCoreDescriptorFixture(Symbol, "Sg");
    EXPECT_FALSE(objc_binding_detail::swiftTypeMetadataDescriptor(
        F.Image, F.DescriptorSlot))
        << Symbol;
    EXPECT_FALSE(objc_binding_detail::swiftTypeMetadataPairProof(
        F.Image, F.Cache, F.Reference))
        << Symbol;
  }
}

TEST(ObjCSourceBindings,
     SwiftCoreArrayRecipePreservesBothDescriptorIdentities) {
  auto F = swiftStdlibTypeMetadataFixture();
  EXPECT_TRUE(
      F.Image.recordDyldBindSlot(F.NestedDescriptorSlot, "_$ss5UInt8VMn", 0,
                                 "/usr/lib/swift/libswiftCore.dylib", false));
  F.Image.Symbols[0].Name = "_$ss23_ContiguousArrayStorageCys5UInt8VGMR";
  F.Image.Symbols[1].Name = "_$ss23_ContiguousArrayStorageCys5UInt8VGMd";
  auto &Data = F.Image.Segments[0].Data;
  llvm::support::endian::write32le(Data.data() + F.Reference + 4 - 0x1000, 12);
  auto *Type = Data.data() + F.TypeReference - 0x1000;
  Type[5] = 'y';
  Type[6] = 2;
  llvm::support::endian::write32le(
      Type + 7, uint32_t(F.NestedDescriptorSlot - (F.TypeReference + 7)));
  Type[11] = 'G';
  Type[12] = 0;
  const auto Proof = objc_binding_detail::swiftTypeMetadataPairProof(
      F.Image, F.Cache, F.Reference);
  ASSERT_TRUE(Proof);
  ASSERT_EQ(Proof->Descriptors.size(), 2U);
  EXPECT_EQ(Proof->Descriptors[0].Symbol, "_$ss23_ContiguousArrayStorageCMn");
  EXPECT_EQ(Proof->Descriptors[1].Symbol, "_$ss5UInt8VMn");
  EXPECT_EQ(Proof->Descriptors[0].Offset, 0U);
  EXPECT_EQ(Proof->Descriptors[1].Offset, 6U);
  F.Image.DyldBindSlots[F.NestedDescriptorSlot].Module =
      "/System/Library/Frameworks/Foundation.framework/Foundation";
  EXPECT_FALSE(objc_binding_detail::swiftTypeMetadataPairProof(F.Image, F.Cache,
                                                               F.Reference));
}

namespace {
SwiftTypeMetadataFixture swiftStringKeyedNestedTypeFixture(
    llvm::StringRef Outer = "_$ss18_DictionaryStorageCMn") {
  SwiftTypeMetadataFixture F(Arch::AArch64, false, false, false, false, true);
  const std::string Inner = "_$s3WMF23WikipediaSiteInfoLookupV09NamespaceD0VMn";
  const std::string Base = Outer.drop_back(2).str() +
                           "ySS3WMF23WikipediaSiteInfoLookupV13NamespaceInfoVG";
  F.Image.Symbols[0].Name = Base + "MR";
  F.Image.Symbols[1].Name = Base + "Md";
  F.Image.Symbols[2].Name = Inner;
  F.Image.Exports[0].Name = Inner;
  F.Image.ImportPtrSlots.clear();
  F.Image.ImportStorageSlots.clear();
  F.Image.DyldBindSlots.clear();
  EXPECT_TRUE(F.Image.recordDyldBindSlot(F.DescriptorSlot, Outer.str(), 0,
                                         "/usr/lib/swift/libswiftCore.dylib",
                                         false));
  auto &Data = F.Image.Segments[0].Data;
  llvm::support::endian::write32le(Data.data() + F.Reference + 4 - 0x1000, 14);
  auto *Type = Data.data() + F.TypeReference - 0x1000;
  std::memcpy(Type + 5, "ySS", 3);
  Type[8] = 2;
  llvm::support::endian::write32le(
      Type + 9, uint32_t(F.NestedDescriptorSlot - (F.TypeReference + 9)));
  Type[13] = 'G';
  Type[14] = 0;
  return F;
}
} // namespace

TEST(ObjCSourceBindings, NestedSwiftGenericIdentityComparesCompleteTypeTrees) {
  for (const auto &Outer : {"_$ss18_DictionaryStorageCMn",
                            "_$ss17_NativeDictionaryVMn", "_$ss6ResultOMn"}) {
    auto F = swiftStringKeyedNestedTypeFixture(Outer);
    for (const bool CompressedName : {false, true}) {
      if (CompressedName) {
        if (llvm::StringRef(Outer) != "_$ss18_DictionaryStorageCMn")
          continue;
        F.Image.Symbols[0].Name = "_$ss18_"
                                  "DictionaryStorageCySS3WMF23WikipediaSiteInfo"
                                  "LookupV09NamespaceF0VGMR";
        F.Image.Symbols[1].Name = "_$ss18_"
                                  "DictionaryStorageCySS3WMF23WikipediaSiteInfo"
                                  "LookupV09NamespaceF0VGMd";
      }
      const auto Proof = objc_binding_detail::swiftTypeMetadataPairProof(
          F.Image, F.Cache, F.Reference);
      ASSERT_TRUE(Proof) << Outer << CompressedName;
      ASSERT_EQ(Proof->Descriptors.size(), 2U);
      EXPECT_EQ(Proof->Descriptors[1].Symbol, F.Image.Symbols[2].Name);
      const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      ASSERT_EQ(Bound.SwiftTypeMetadataPairs.size(), 1U);
      for (const auto &Arg : Bound.Function.Body[0].Val->Operands)
        EXPECT_TRUE(objcSourceCallBound(*Arg, F.Image, {}));
      std::set<std::string> Names;
      const auto Source = renderObjCSwiftTypeMetadataHelpers(
          F.Image, Bound.SwiftTypeMetadataPairs, Names);
      EXPECT_NE(
          Source.find(
              "__asm__(\"_$s3WMF23WikipediaSiteInfoLookupV09NamespaceD0VMn\")"),
          std::string::npos);
      EXPECT_EQ(Source.find("NamespaceF0"), std::string::npos);
    }
  }
}

TEST(ObjCSourceBindings,
     NestedSwiftGenericIdentityRejectsDifferentTypesAndRecipes) {
  for (unsigned Mutation = 0; Mutation != 15; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = swiftStringKeyedNestedTypeFixture();
    const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_EQ(Bound.SwiftTypeMetadataPairs.size(), 1U);
    auto Base = llvm::StringRef(F.Image.Symbols[0].Name).drop_back(2).str();
    const auto Replace = [&](const std::string &From, const std::string &To) {
      const auto Pos = Base.find(From);
      ASSERT_NE(Pos, std::string::npos);
      Base.replace(Pos, From.size(), To);
    };
    switch (Mutation) {
    case 0:
      Replace("ySS", "ySi");
      break;
    case 1:
      Replace("3WMF", "3WMG");
      break;
    case 2:
      Replace("13NamespaceInfoV", "14NamespaceOtherV");
      break;
    case 3:
      Replace("23WikipediaSiteInfoLookupV", "10OtherOwnerV");
      break;
    case 4:
      Replace("s18_DictionaryStorageC", "s17_NativeDictionaryV");
      break;
    case 5:
      F.Image.Segments[0].Data[0x87] = 'i';
      break;
    case 6:
      F.Image.Segments[0].Data[0x24] = 13;
      break;
    case 7:
      F.Image.Segments[0].Data[0x88] = 1;
      break;
    case 8:
      F.Image.Symbols[2].Name =
          "_$s3WMG23WikipediaSiteInfoLookupV09NamespaceD0VMn";
      break;
    case 9:
      F.Image.Exports.clear();
      break;
    case 10:
      F.Image.DyldBindSlots[F.DescriptorSlot].WeakImport = true;
      break;
    case 11:
      F.Image.DataPtrRelocTargetOwners[F.NestedDescriptorSlot] = 0x2000;
      break;
    case 12:
      Replace("13NamespaceInfoV", "13NamespaceInfoC");
      break;
    case 13:
      Base.insert(Base.size() - 1, "Si");
      break;
    case 14:
      F.Image.Symbols[2].Name = F.Image.Exports[0].Name =
          "_$s3WMF23WikipediaSiteInfoLookupV09NamespaceZ0VMn";
      break;
    }
    F.Image.Symbols[0].Name = Base + "MR";
    F.Image.Symbols[1].Name = Base + "Md";
    EXPECT_FALSE(objc_binding_detail::swiftTypeMetadataPairProof(
        F.Image, F.Cache, F.Reference));
    EXPECT_TRUE(bindObjCSourceReferences(F.Function, F.Image)
                    .SwiftTypeMetadataPairs.empty());
    for (const auto &Arg : Bound.Function.Body[0].Val->Operands)
      EXPECT_FALSE(objcSourceCallBound(*Arg, F.Image, {}));
    std::set<std::string> Names;
    EXPECT_THROW(renderObjCSwiftTypeMetadataHelpers(
                     F.Image, Bound.SwiftTypeMetadataPairs, Names),
                 std::runtime_error);
  }
}

namespace {
SwiftTypeMetadataFixture swiftNominalTupleTypeFixture(Arch Architecture) {
  SwiftTypeMetadataFixture F(Architecture);
  const std::string Base = "_$s10Foundation3URLV_AA4DateVt";
  F.Image.Symbols[0].Name = Base + "MR";
  F.Image.Symbols[1].Name = Base + "Md";
  EXPECT_TRUE(F.Image.recordDyldBindSlot(
      F.NestedDescriptorSlot, "_$s10Foundation4DateVMn", 0,
      "/System/Library/Frameworks/Foundation.framework/Foundation", false));
  auto &Data = F.Image.Segments[0].Data;
  llvm::support::endian::write32le(Data.data() + F.Reference + 4 - 0x1000, 12);
  auto *Type = Data.data() + F.TypeReference - 0x1000;
  Type[5] = '_';
  Type[6] = 2;
  llvm::support::endian::write32le(
      Type + 7, uint32_t(F.NestedDescriptorSlot - (F.TypeReference + 7)));
  Type[11] = 't';
  Type[12] = 0;
  return F;
}
} // namespace

TEST(ObjCSourceBindings, SwiftNominalTupleIdentityKeepsElementOrder) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto F = swiftNominalTupleTypeFixture(Architecture);
    const auto Proof = objc_binding_detail::swiftTypeMetadataPairProof(
        F.Image, F.Cache, F.Reference);
    ASSERT_TRUE(Proof);
    ASSERT_EQ(Proof->Descriptors.size(), 2U);
    EXPECT_EQ(Proof->Descriptors[0].Offset, 0U);
    EXPECT_EQ(Proof->Descriptors[0].Symbol, "_$s10Foundation3URLVMn");
    EXPECT_EQ(Proof->Descriptors[1].Offset, 6U);
    EXPECT_EQ(Proof->Descriptors[1].Symbol, "_$s10Foundation4DateVMn");
    const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    ASSERT_EQ(Bound.SwiftTypeMetadataPairs.size(), 1U);
    for (const auto &Arg : Bound.Function.Body[0].Val->Operands)
      EXPECT_TRUE(objcSourceCallBound(*Arg, F.Image, {}));
    std::set<std::string> Names;
    const auto Source = renderObjCSwiftTypeMetadataHelpers(
        F.Image, Bound.SwiftTypeMetadataPairs, Names);
    EXPECT_NE(Source.find("__asm__(\"_$s10Foundation3URLVMn\")"),
              std::string::npos);
    EXPECT_NE(Source.find("__asm__(\"_$s10Foundation4DateVMn\")"),
              std::string::npos);
    EXPECT_EQ(Source.find("AA4Date"), std::string::npos);
  }
}

TEST(ObjCSourceBindings,
     SwiftNominalTupleRejectsIncompleteOrDifferentIdentity) {
  for (unsigned Mutation = 0; Mutation != 18; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = swiftNominalTupleTypeFixture(Arch::AArch64);
    const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_EQ(Bound.SwiftTypeMetadataPairs.size(), 1U);
    std::string Base = "_$s10Foundation3URLV_AA4DateVt";
    auto *Type = F.Image.Segments[0].Data.data() + F.TypeReference - 0x1000;
    switch (Mutation) {
    case 0:
      Base = "_$s10Foundation4DateV_AA3URLVt";
      break;
    case 1:
      Base = "_$s10Foundation3URLV_AA4DataVt";
      break;
    case 2:
      Base = "_$s10Foundation3URLV3url_AA4DateV4datet";
      break;
    case 3:
      Base = "_$s10Foundation3URLV_AA4DateVSgt";
      break;
    case 4:
      Base = "_$s10Foundation3URLV_AA4DateVSit";
      break;
    case 5:
      F.Image.DyldBindSlots[F.NestedDescriptorSlot].WeakImport = true;
      break;
    case 6:
      F.Image.DyldBindSlots[F.DescriptorSlot].Module = "/tmp/foreign.dylib";
      break;
    case 7:
      F.Image.DyldBindSlots[F.NestedDescriptorSlot].Addend = 8;
      break;
    case 8:
      F.Image.ConflictingImportStorageSlots.insert(F.NestedDescriptorSlot);
      break;
    case 9:
      Type[5] = 'y';
      break;
    case 10:
      Type[11] = 'G';
      break;
    case 11:
      Type[6] = 3;
      break;
    case 12:
      Type[12] = 't';
      break;
    case 13:
      F.Image.Segments[0].Data[F.Reference + 4 - 0x1000] = 11;
      break;
    case 14:
      llvm::support::endian::write32le(
          Type + 7, uint32_t(F.DescriptorSlot - (F.TypeReference + 7)));
      break;
    case 15:
      Base = "_$s10Foundation3URLC_AA4DateVt";
      break;
    case 16:
      Base = "_$s10Foundation3URLV_ZZ4DateVt";
      break;
    case 17:
      F.Image.DyldBindSlots.erase(F.NestedDescriptorSlot);
      break;
    }
    F.Image.Symbols[0].Name = Base + "MR";
    F.Image.Symbols[1].Name = Base + "Md";
    EXPECT_FALSE(objc_binding_detail::swiftTypeMetadataPairProof(
        F.Image, F.Cache, F.Reference));
    EXPECT_TRUE(bindObjCSourceReferences(F.Function, F.Image)
                    .SwiftTypeMetadataPairs.empty());
    for (const auto &Arg : Bound.Function.Body[0].Val->Operands)
      EXPECT_FALSE(objcSourceCallBound(*Arg, F.Image, {}));
    std::set<std::string> Names;
    EXPECT_THROW(renderObjCSwiftTypeMetadataHelpers(
                     F.Image, Bound.SwiftTypeMetadataPairs, Names),
                 std::runtime_error);
  }
}

namespace {
SwiftTypeMetadataFixture swiftTupleContainerRecipeFixture(Arch Architecture) {
  auto F = swiftNominalTupleTypeFixture(Architecture);
  const bool CoreStorage = Architecture == Arch::AArch64;
  const std::string Outer = CoreStorage ? "_$ss23_ContiguousArrayStorageCMn"
                                        : "_$s7Combine9PublishedVMn";
  const std::string Base = CoreStorage
                               ? "_$ss23_ContiguousArrayStorageCy10Foundation"
                                 "3URLV_AC4DateVtG"
                               : "_$s7Combine9PublishedVy10Foundation3URLV_"
                                 "AD4DateVtG";
  F.Image.Symbols[0].Name = Base + "MR";
  F.Image.Symbols[1].Name = Base + "Md";
  F.Image.ImportPtrSlots.clear();
  F.Image.ImportStorageSlots.clear();
  F.Image.DyldBindSlots.clear();
  EXPECT_TRUE(F.Image.recordDyldBindSlot(
      F.DescriptorSlot, Outer, 0,
      CoreStorage ? "/usr/lib/swift/libswiftCore.dylib"
                  : "/System/Library/Frameworks/Combine.framework/Combine",
      false));
  EXPECT_TRUE(F.Image.recordDyldBindSlot(
      F.NestedDescriptorSlot, "_$s10Foundation3URLVMn", 0,
      "/System/Library/Frameworks/Foundation.framework/Foundation", false));
  EXPECT_TRUE(F.Image.recordDyldBindSlot(
      F.NestedDescriptorSlot + 8, "_$s10Foundation4DateVMn", 0,
      "/System/Library/Frameworks/Foundation.framework/Foundation", false));
  auto &Data = F.Image.Segments[0].Data;
  llvm::support::endian::write32le(Data.data() + F.Reference + 4 - 0x1000, 19);
  auto *Type = Data.data() + F.TypeReference - 0x1000;
  Type[5] = 'y';
  Type[11] = '_';
  Type[12] = 2;
  llvm::support::endian::write32le(
      Type + 13, uint32_t(F.NestedDescriptorSlot + 8 - (F.TypeReference + 13)));
  Type[17] = 't';
  Type[18] = 'G';
  Type[19] = 0;
  return F;
}
} // namespace

TEST(ObjCSourceBindings, SwiftTupleContainerRecipeKeepsAllThreeIdentities) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto F = swiftTupleContainerRecipeFixture(Architecture);
    const auto Proof = objc_binding_detail::swiftTypeMetadataPairProof(
        F.Image, F.Cache, F.Reference);
    ASSERT_TRUE(Proof);
    ASSERT_EQ(Proof->Descriptors.size(), 3U);
    EXPECT_EQ(Proof->Descriptors[0].Offset, 0U);
    EXPECT_EQ(Proof->Descriptors[1].Offset, 6U);
    EXPECT_EQ(Proof->Descriptors[2].Offset, 12U);
    EXPECT_EQ(Proof->Descriptors[1].Symbol, "_$s10Foundation3URLVMn");
    EXPECT_EQ(Proof->Descriptors[2].Symbol, "_$s10Foundation4DateVMn");
    const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    ASSERT_EQ(Bound.SwiftTypeMetadataPairs.size(), 1U);
    for (const auto &Arg : Bound.Function.Body[0].Val->Operands)
      EXPECT_TRUE(objcSourceCallBound(*Arg, F.Image, {}));
    std::set<std::string> Names;
    const auto Source = renderObjCSwiftTypeMetadataHelpers(
        F.Image, Bound.SwiftTypeMetadataPairs, Names);
    for (const auto &Descriptor : Proof->Descriptors)
      EXPECT_NE(Source.find("__asm__(\"" + Descriptor.Symbol + "\")"),
                std::string::npos);
  }
}

TEST(ObjCSourceBindings,
     SwiftTupleContainerRecipeRejectsDifferentTreesAndEdges) {
  for (unsigned Mutation = 0; Mutation != 13; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = swiftTupleContainerRecipeFixture(Arch::AArch64);
    const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_EQ(Bound.SwiftTypeMetadataPairs.size(), 1U);
    std::string Base = "_$ss23_ContiguousArrayStorageCy10Foundation3URLV_"
                       "AC4DateVtG";
    auto *Type = F.Image.Segments[0].Data.data() + F.TypeReference - 0x1000;
    switch (Mutation) {
    case 0:
      Base = "_$ss23_ContiguousArrayStorageCy10Foundation4DateV_AC3URLVtG";
      break;
    case 1:
      Base = "_$ss23_ContiguousArrayStorageCy10Foundation3URLV3url_"
             "AC4DateV4datetG";
      break;
    case 2:
      Base = "_$ss23_ContiguousArrayStorageCy10Foundation3URLV_AC4DateVSgtG";
      break;
    case 3:
      Base = "_$ss23_ContiguousArrayStorageCy10Foundation3URLV_AC4DateVSiGt";
      break;
    case 4:
      Base = "_$ss23_ContiguousArrayStorageCy10Foundation3URLV_AC4DateVtSiG";
      break;
    case 5:
      Type[11] = 'y';
      break;
    case 6:
      Type[17] = 'G';
      break;
    case 7:
      Type[18] = 't';
      break;
    case 8:
      F.Image.DyldBindSlots[F.NestedDescriptorSlot + 8].Module =
          "/tmp/foreign.dylib";
      break;
    case 9:
      F.Image.DyldBindSlots[F.NestedDescriptorSlot + 8].WeakImport = true;
      break;
    case 10:
      llvm::support::endian::write32le(
          Type + 13, uint32_t(F.NestedDescriptorSlot - (F.TypeReference + 13)));
      break;
    case 11:
      F.Image.ConflictingImportStorageSlots.insert(F.NestedDescriptorSlot + 8);
      break;
    case 12:
      Type[19] = 'G';
      break;
    }
    F.Image.Symbols[0].Name = Base + "MR";
    F.Image.Symbols[1].Name = Base + "Md";
    EXPECT_FALSE(objc_binding_detail::swiftTypeMetadataPairProof(
        F.Image, F.Cache, F.Reference));
    EXPECT_TRUE(bindObjCSourceReferences(F.Function, F.Image)
                    .SwiftTypeMetadataPairs.empty());
    for (const auto &Arg : Bound.Function.Body[0].Val->Operands)
      EXPECT_FALSE(objcSourceCallBound(*Arg, F.Image, {}));
    std::set<std::string> Names;
    EXPECT_THROW(renderObjCSwiftTypeMetadataHelpers(
                     F.Image, Bound.SwiftTypeMetadataPairs, Names),
                 std::runtime_error);
  }
}

TEST(ObjCSourceBindings,
     SwiftConcreteTypeInstantiatorAcceptsIntegerReferenceCarrier) {
  SwiftTypeMetadataFixture F(Arch::AArch64);
  auto Call = F.Function.Body.front().Val;
  ASSERT_TRUE(Call && Call->SourceCallHint);
  auto Native = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
  Native->TargetName = "___swift_instantiateConcreteTypeFromMangledNameV2";
  Native->Signature.Parameters[1].Type = NdType::makeInt(8, false);
  std::string Diagnostic;
  ASSERT_TRUE(
      assignDarwinScalarSourceABI(Native->Signature, F.Image.Arch, Diagnostic))
      << Diagnostic;
  Call->SourceCallHint = Native;
  Call->Operands[1]->Type = NdType::makeInt(8, false);

  const auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  ASSERT_EQ(Result.SwiftTypeMetadataPairs.size(), 1U);
  auto Reference = Result.Function.Body.front().Val->Operands[1];
  ASSERT_TRUE(Reference->SourceCallHint);
  EXPECT_EQ(Reference->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeSwiftTypeMetadataAddress);
  EXPECT_EQ(Reference->SourceCallHint->TargetAddress,
            SwiftTypeMetadataFixture::Reference);
  EXPECT_TRUE(objcSourceCallBound(*Reference, F.Image, {}));

  MedVar ReferenceLocal;
  ReferenceLocal.Kind = MedVar::Temp;
  ReferenceLocal.Id = 503;
  ReferenceLocal.Size = 8;
  HighStmt Assignment;
  Assignment.Kind = StmtKind::Assign;
  Assignment.Dst = HighExpr::makeVar(ReferenceLocal, NdType::makeInt(8, false));
  Assignment.Val = HighExpr::makeConst(SwiftTypeMetadataFixture::Reference, 8,
                                       ConstantAddressProvenance::DataAddress);
  F.Function.Body.insert(F.Function.Body.begin(), Assignment);
  Call->Operands[1] =
      HighExpr::makeVar(ReferenceLocal, NdType::makeInt(8, false));
  EXPECT_TRUE(objc_binding_detail::swiftTypeMetadataPairCallCarrier(
      *Call, F.Image.Arch));
  const auto AliasFlow = analyzeHighSourceFlow(F.Function, false);
  EXPECT_TRUE(AliasFlow.Complete);
  EXPECT_TRUE(AliasFlow.Items.empty());
  const auto Aliased = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_EQ(Aliased.SwiftTypeMetadataPairs.size(), 1U);
  EXPECT_TRUE(Aliased.Function.Body.front().Val->SourceCallHint);
  ASSERT_TRUE(Aliased.Limitation.empty()) << Aliased.Limitation;
  ASSERT_EQ(Aliased.SwiftTypeMetadataPairs.size(), 1U);
  ASSERT_TRUE(Aliased.Function.Body.front().Val->SourceCallHint);
  F.Function.Body.erase(F.Function.Body.begin());
  Call->Operands[1] =
      HighExpr::makeConst(SwiftTypeMetadataFixture::Reference, 8,
                          ConstantAddressProvenance::DataAddress);
  Call->Operands[1]->Type = NdType::makeInt(8, false);

  Native->TargetName = "unrelated_helper";
  const auto Unrelated = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_TRUE(Unrelated.SwiftTypeMetadataPairs.empty());
  EXPECT_FALSE(Unrelated.Limitation.empty());
  Native->TargetName = "___swift_instantiateConcreteTypeFromMangledNameV2";
  Native->Signature.Parameters[1].Type = NdType::makeInt(4, false);
  const auto Narrow = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_TRUE(Narrow.SwiftTypeMetadataPairs.empty());
}

namespace {
struct ForwardedMetadataFixture : SwiftTypeMetadataFixture {
  HighFunc Helper, Instantiator;
  static constexpr va_t SecondCache = 0x2040, SecondReference = 0x1040;
  ExprPtr CallerCall, FirstUse, SecondUse;

  static ExprPtr parameter(unsigned Index, TypeRef Type) {
    MedVar Var;
    Var.Kind = MedVar::Param;
    Var.Id = Index;
    Var.Size = Type->Size;
    return HighExpr::makeVar(Var, Type);
  }

  ForwardedMetadataFixture() : SwiftTypeMetadataFixture(Arch::AArch64) {
    Segment Text;
    Text.Name = "__TEXT";
    Text.VA = Text.FileOff = 0x5000;
    Text.Size = Text.FileSz = 0x100;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(0x100);
    Image.Segments.push_back(Text);
    Section Code;
    Code.Name = "__text";
    Code.SegmentName = "__TEXT";
    Code.VA = Code.FileOff = 0x5000;
    Code.Size = Code.FileSz = 0x100;
    Code.Flags = Text.Flags;
    Code.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    Image.Sections.push_back(Code);
    auto &Bytes = Image.Segments[0].Data;
    llvm::support::endian::write32le(Bytes.data() + 0x40, 0xb0 - 0x40);
    llvm::support::endian::write32le(Bytes.data() + 0x44, 2);
    std::memcpy(Bytes.data() + 0xb0, "Si", 3);
    Image.Symbols.push_back({"_$sSiMR", SecondReference, 8, false});
    Image.Symbols.push_back({"_$sSiMd", SecondCache, 8, false});

    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    const auto Word = NdType::makeInt(8);
    auto SetSignature = [&](HighFunc &Func, std::vector<TypeRef> Types) {
      SourceFunctionTypeHint Signature;
      Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      Signature.ReturnType = Word;
      for (size_t I = 0; I < Types.size(); ++I) {
        const auto Name = "arg" + std::to_string(I);
        Signature.Parameters.push_back({Name, Types[I]});
        Func.Params.push_back({Name, Types[I]});
      }
      std::string Error;
      EXPECT_TRUE(assignDarwinScalarSourceABI(Signature, Image.Arch, Error));
      Func.ReturnType = Word;
      Func.SourceTypeHint = Signature;
    };
    Instantiator.Entry = 0x5000;
    Instantiator.Name = "___swift_instantiateConcreteTypeFromMangledNameV2";
    Image.Symbols.push_back(
        {Instantiator.Name, Instantiator.Entry, 0x40, true});
    SetSignature(Instantiator, {Pointer, Pointer});
    Helper.Entry = 0x5080;
    Helper.Name = "outlined_array_helper";
    SetSignature(Helper, {NdType::makeInt(4), Word, NdType::makeInt(4), Pointer,
                          Word, Word, Word, Word});
    auto Call = [&](const HighFunc &Target, std::vector<ExprPtr> Operands) {
      auto Value = HighExpr::makeCall(Target.Name, Target.Entry, Operands);
      auto Hint = std::make_shared<SourceCallTypeHint>();
      Hint->CallKind = SourceCallTypeHint::Kind::Native;
      Hint->TargetAddress = Target.Entry;
      Hint->Signature = *Target.SourceTypeHint;
      Value->SourceCallHint = Hint;
      Value->Type = Target.ReturnType;
      return Value;
    };
    FirstUse = Call(Instantiator, {parameter(4, Word), parameter(5, Word)});
    SecondUse = Call(Instantiator, {parameter(6, Word), parameter(7, Word)});
    for (const auto &Use : {FirstUse, SecondUse, SecondUse}) {
      HighStmt Statement;
      Statement.Kind = StmtKind::ExprStmt;
      Statement.Val = Use;
      Helper.Body.push_back(Statement);
    }
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = HighExpr::makeConst(0, 8);
    Helper.Body.push_back(Return);
    Instantiator.Body = {Return};
    std::vector<ExprPtr> Arguments;
    for (unsigned I = 0; I < 4; ++I) {
      auto Value = HighExpr::makeConst(I, Helper.Params[I].Type->Size,
                                       ConstantAddressProvenance::Scalar);
      Value->Type = Helper.Params[I].Type;
      Arguments.push_back(Value);
    }
    for (const auto Address : {Cache, Reference, SecondCache, SecondReference})
      Arguments.push_back(HighExpr::makeConst(
          Address, 8, ConstantAddressProvenance::DataAddress));
    CallerCall = Call(Helper, std::move(Arguments));
    Function.Body[0].Val = CallerCall;
  }

  ObjCSourceBindingResult bind() {
    const std::map<va_t, const HighFunc *> Functions{
        {Helper.Entry, &Helper}, {Instantiator.Entry, &Instantiator}};
    return bindObjCSourceReferences(Function, Image, nullptr, &Functions);
  }
};
} // namespace

TEST(ObjCSourceBindings, SwiftMetadataForwardingKeepsTwoIndependentPairs) {
  ForwardedMetadataFixture F;
  F.Image.Symbols.push_back(F.Image.Symbols.back());
  const auto Bound = F.bind();
  ASSERT_EQ(Bound.SwiftTypeMetadataPairs.size(), 2U);
  EXPECT_EQ(Bound.SwiftTypeMetadataPairs.at(F.Cache).ReferenceAddress,
            F.Reference);
  EXPECT_EQ(Bound.SwiftTypeMetadataPairs.at(F.SecondCache).ReferenceAddress,
            F.SecondReference);
  const auto &Args = Bound.Function.Body[0].Val->Operands;
  for (size_t I = 0; I < Args.size(); ++I) {
    if (I < 4)
      EXPECT_FALSE(Args[I]->SourceCallHint);
    else {
      ASSERT_TRUE(Args[I]->SourceCallHint);
      EXPECT_TRUE(objcSourceCallBound(*Args[I], F.Image, {}));
    }
  }
  EXPECT_TRUE(bindObjCSourceReferences(F.Function, F.Image)
                  .SwiftTypeMetadataPairs.empty());
}

TEST(ObjCSourceBindings, SwiftMetadataForwardingRejectsUnprovedUsesAndEdges) {
  for (unsigned Mutation = 0; Mutation < 24; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ForwardedMetadataFixture F;
    const auto Word = NdType::makeInt(8);
    auto Param = [&](unsigned I) {
      return ForwardedMetadataFixture::parameter(I, Word);
    };
    HighStmt Extra;
    Extra.Kind = StmtKind::ExprStmt;
    Extra.Val = Param(4);
    switch (Mutation) {
    case 0:
      F.Helper.Body.push_back(Extra);
      break;
    case 1:
      Extra.Kind = StmtKind::Assign;
      Extra.Dst = Param(4);
      Extra.Val = HighExpr::makeConst(0, 8);
      F.Helper.Body.insert(F.Helper.Body.begin(), Extra);
      break;
    case 2:
      F.FirstUse->Operands[0] = HighExpr::makeBinop(NdOp::INT_ADD, Param(4),
                                                    HighExpr::makeConst(1, 8));
      break;
    case 3:
      F.FirstUse->Operands[0]->Var.SSAVer = 1;
      break;
    case 4:
      std::swap(F.FirstUse->Operands[0], F.FirstUse->Operands[1]);
      break;
    case 5:
      F.FirstUse->Operands[1] = Param(7);
      break;
    case 6:
      F.FirstUse->IsIndirectCall = true;
      break;
    case 7:
      F.FirstUse->CallAddr += 4;
      break;
    case 8:
      F.Helper.SourceTypeHint->Parameters[4].Location.ValueBytes = 4;
      break;
    case 9:
      F.Helper.Params[4].Type = NdType::makeFloat(8);
      break;
    case 10:
      F.Helper.Body.clear();
      break;
    case 11:
      F.Instantiator.SourceTypeHint.reset();
      break;
    case 12:
      F.Image.Symbols.back().Name = "different_native_function";
      break;
    case 13:
      F.CallerCall->IsIndirectCall = true;
      break;
    case 14:
      F.FirstUse->IntrinsicOutputs.push_back(Param(4)->Var);
      break;
    case 15:
      F.FirstUse->IndirectTarget = Param(4);
      break;
    case 16:
      F.Helper.StructuredExceptionRegions = 1;
      break;
    case 17:
      F.Helper.SourceTypeHint->HasExplicitABI = false;
      break;
    case 18:
      F.Image.Symbols.push_back(F.Image.Symbols.back());
      F.Image.Symbols.back().IsFunc = false;
      break;
    case 19:
      F.FirstUse->SourceCallHint.reset();
      break;
    case 20:
      Extra.Val = HighExpr::makeConst(0, 8);
      F.Helper.Body.insert(F.Helper.Body.begin(), 20000, Extra);
      break;
    case 21:
      F.FirstUse->Operands[0] = HighExpr::makeBitCast(Param(4), Word);
      F.FirstUse->Operands[0]->Operands[0] = F.FirstUse->Operands[0];
      break;
    case 22:
      F.CallerCall->Operands[4]->Type = NdType::makeFloat(8);
      break;
    case 23:
      F.CallerCall->Operands[4]->Type = NdType::makeInt(4);
      break;
    }
    EXPECT_FALSE(F.bind().SwiftTypeMetadataPairs.count(F.Cache));
    if (Mutation == 21)
      F.FirstUse->Operands[0]->Operands.clear();
  }
}

TEST(ObjCSourceBindings, SwiftMetadataForwardingSharesOnlyProvedCallerAliases) {
  ForwardedMetadataFixture F;
  const auto Word = NdType::makeInt(8);
  for (unsigned I = 4; I < 8; ++I) {
    MedVar Local;
    Local.Kind = MedVar::Temp;
    Local.Id = 500 + I;
    Local.Size = 8;
    HighStmt Assignment;
    Assignment.Kind = StmtKind::Assign;
    Assignment.Dst = HighExpr::makeVar(Local, Word);
    Assignment.Val = F.CallerCall->Operands[I];
    F.Function.Body.insert(F.Function.Body.end() - 1, Assignment);
    F.CallerCall->Operands[I] = HighExpr::makeVar(Local, Word);
  }
  F.CallerCall->Operands[1] =
      HighExpr::makeConst(F.Reference, 8, ConstantAddressProvenance::Scalar);
  const auto Bound = F.bind();
  ASSERT_EQ(Bound.SwiftTypeMetadataPairs.size(), 2U);
  for (unsigned I = 0; I < 4; ++I) {
    const auto &Value = Bound.Function.Body[I].Val;
    ASSERT_TRUE(Value->SourceCallHint);
    EXPECT_TRUE(objcSourceCallBound(*Value, F.Image, {}));
  }
  const auto Scalar = Bound.Function.Body.back().Val->Operands[1];
  EXPECT_EQ(Scalar->Kind, ExprKind::Const);
  EXPECT_EQ(Scalar->ConstVal, F.Reference);
  EXPECT_FALSE(Scalar->SourceCallHint);
  HighStmt Escape;
  Escape.Kind = StmtKind::ExprStmt;
  Escape.Val = F.CallerCall->Operands[4];
  F.Function.Body.push_back(Escape);
  EXPECT_FALSE(F.bind().Function.Body[0].Val->SourceCallHint);
}

TEST(ObjCSourceBindings, SwiftStdlibMetadataRecipeExecutesWithSharedCache) {
  auto F = swiftStdlibTypeMetadataFixture();
  const auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_EQ(Result.SwiftTypeMetadataPairs.size(), 1U);
  std::set<std::string> Shared;
  std::string Source = "#include <stdint.h>\n#include <string.h>\n" +
                       renderObjCSwiftTypeMetadataHelpers(
                           F.Image, Result.SwiftTypeMetadataPairs, Shared);
  Source += R"(
unsigned char descriptor[1] __asm__("_$ss23_ContiguousArrayStorageCMn") = { 0 };
int main(void) {
  void **cache = (void **)neverd_swift_type_metadata_2020_1020_cache_address();
  const unsigned char *reference = (const void *)
      neverd_swift_type_metadata_2020_1020_reference_address();
  int32_t relative; uint32_t length;
  memcpy(&relative, reference, 4);
  memcpy(&length, reference + 4, 4);
  if (length != 9 || *cache) return 1;
  const unsigned char *type = reference + relative;
  if (type[0] != 2 || memcmp(type + 5, "yypG", 5)) return 2;
  memcpy(&relative, type + 1, 4);
  const void *resolved;
  memcpy(&resolved, type + 1 + relative, sizeof(resolved));
  if (resolved != descriptor) return 3;
  *cache = descriptor;
  if ((void **)neverd_swift_type_metadata_2020_1020_cache_address() != cache ||
      *cache != descriptor) return 4;
  if ((const void *)neverd_swift_type_metadata_2020_1020_reference_address() !=
      reference) return 5;
  return 0;
}
)";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-stdlib-metadata",
                                                    Directory));
  const std::filesystem::path Work(Directory.str().str());
  struct Cleanup {
    std::filesystem::path Work;
    ~Cleanup() {
      std::error_code Error;
      std::filesystem::remove_all(Work, Error);
    }
  } Cleanup{Work};
  const auto Path = (Work / "objects.c").string();
  const auto Executable = (Work / "objects").string();
  const auto ErrorPath = (Work / "stderr").string();
  std::ofstream(Path) << Source;
  const std::string Compiler = NEVERD_TEST_CLANG;
  for (const char *Optimization : {"-O0", "-O2"}) {
    const std::vector<std::string> Arguments{
        Compiler, "-std=c11", Optimization, "-Werror", Path, "-o", Executable};
    std::vector<llvm::StringRef> Refs(Arguments.begin(), Arguments.end());
    const std::optional<llvm::StringRef> Redirects[] = {
        std::nullopt, std::nullopt, ErrorPath};
    std::string Error;
    const auto Status = llvm::sys::ExecuteAndWait(Compiler, Refs, std::nullopt,
                                                  Redirects, 60, 0, &Error);
    auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Status, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "");
    EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable}, std::nullopt,
                                        Redirects, 30, 0, &Error),
              0)
        << Error;
  }
}

TEST(ObjCSourceBindings, SwiftDictionaryStorageMetadataUsesExactStrongImport) {
  auto F = swiftDictionaryTypeMetadataFixture();
  auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  ASSERT_EQ(Result.SwiftTypeMetadataPairs.size(), 1U);
  EXPECT_EQ(Result.SwiftTypeMetadataPairs.at(F.Cache).Suffix,
            "ySSSo8NSBundleCG");
  std::set<std::string> Helpers;
  const auto Source = renderObjCSwiftTypeMetadataHelpers(
      F.Image, Result.SwiftTypeMetadataPairs, Helpers);
  EXPECT_NE(Source.find("_$ss18_DictionaryStorageCMn"), std::string::npos);

  F.Image.DyldBindSlots[F.DescriptorSlot].Module = "/tmp/foreign.dylib";
  Result = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_TRUE(Result.SwiftTypeMetadataPairs.empty());
  EXPECT_FALSE(Result.Limitation.empty());
}

TEST(ObjCSourceBindings,
     SwiftNativeDictionaryMetadataRequiresExactImportIdentity) {
  for (unsigned Mutation = 0; Mutation < 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = swiftDictionaryTypeMetadataFixture();
    const std::string Descriptor = "_$ss17_NativeDictionaryVMn";
    F.Image.ImportPtrSlots[F.DescriptorSlot] = Descriptor;
    F.Image.ImportStorageSlots[F.DescriptorSlot].Name = Descriptor;
    F.Image.DyldBindSlots[F.DescriptorSlot].Name = Descriptor;
    F.Image.Symbols[0].Name = "_$ss17_NativeDictionaryVySSSo8NSBundleCGMR";
    F.Image.Symbols[1].Name = "_$ss17_NativeDictionaryVySSSo8NSBundleCGMd";
    if (Mutation == 1)
      F.Image.DyldBindSlots[F.DescriptorSlot].WeakImport = true;
    if (Mutation == 2)
      F.Image.DyldBindSlots[F.DescriptorSlot].Addend = 8;
    if (Mutation == 3)
      F.Image.DyldBindSlots[F.DescriptorSlot].Module =
          "/tmp/libswiftCore.dylib";
    if (Mutation == 4)
      F.Image.Segments[2].ReadOnlyAfterRelocations = false;
    if (Mutation == 5)
      F.Image.Symbols[0].Name = "_$ss17_NativeDictionaryVySiSo8NSBundleCGMR";
    if (Mutation == 6)
      F.Image.ImportPtrSlots[F.DescriptorSlot] = "_$ss18_DictionaryStorageCMn";
    if (Mutation == 7)
      F.Image.DyldBindSlots.erase(F.DescriptorSlot);
    if (Mutation == 8)
      F.Image.DataPtrRelocSlots.insert(F.DescriptorSlot);
    if (Mutation == 9)
      F.Image.MachOChainedFixupsAmbiguous = true;
    const auto Result = bindObjCSourceReferences(F.Function, F.Image);
    if (Mutation) {
      EXPECT_TRUE(Result.SwiftTypeMetadataPairs.empty());
      EXPECT_FALSE(Result.Limitation.empty());
      continue;
    }
    ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
    ASSERT_EQ(Result.SwiftTypeMetadataPairs.size(), 1U);
    EXPECT_EQ(Result.SwiftTypeMetadataPairs.at(F.Cache).DescriptorSymbol,
              Descriptor);
    std::set<std::string> Shared;
    const auto Source = renderObjCSwiftTypeMetadataHelpers(
        F.Image, Result.SwiftTypeMetadataPairs, Shared);
    EXPECT_NE(Source.find(Descriptor), std::string::npos);
  }
}

TEST(ObjCSourceBindings,
     SwiftImportedLockMetadataRebuildsPrivateDescriptorAsText) {
  auto F = swiftImportedLockTypeMetadataFixture();
  EXPECT_TRUE(objc_binding_detail::swiftLocalImportedLockType(
      F.Image, SwiftTypeMetadataFixture::LocalDescriptor));
  EXPECT_TRUE(objc_binding_detail::swiftTypeMetadataDescriptor(
      F.Image, SwiftTypeMetadataFixture::DescriptorSlot));
  const auto Proof = objc_binding_detail::swiftTypeMetadataPairProof(
      F.Image, F.Cache, F.Reference);
  ASSERT_TRUE(Proof);
  ASSERT_EQ(Proof->Descriptors.size(), 1U);
  EXPECT_EQ(Proof->Descriptors.front().Symbol, "_$ss13ManagedBufferCMn");
  EXPECT_EQ(Proof->Address.Suffix, "ySDySSSo8NSBundleCGSo16os_unfair_lock_sVG");
  EXPECT_EQ(Proof->TypeReference.size(),
            5U + std::string("ySDySSSo8NSBundleCG"
                             "So16os_unfair_lock_sV"
                             "G")
                     .size());
  const auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  ASSERT_EQ(Result.SwiftTypeMetadataPairs.size(), 1U);
  std::set<std::string> Shared;
  const auto Source = renderObjCSwiftTypeMetadataHelpers(
      F.Image, Result.SwiftTypeMetadataPairs, Shared);
  EXPECT_NE(Source.find("_$ss13ManagedBufferCMn"), std::string::npos);
  EXPECT_EQ(Source.find("_$sSo16os_unfair_lock_sVMn"), std::string::npos);
  EXPECT_NE(Source.find(".reference.length = " +
                        std::to_string(Proof->TypeReference.size())),
            std::string::npos);
}

TEST(ObjCSourceBindings, SwiftImportedLockMetadataRecipeExecutes) {
  auto F = swiftImportedLockTypeMetadataFixture();
  const auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_EQ(Result.SwiftTypeMetadataPairs.size(), 1U);
  std::set<std::string> Shared;
  std::string Source = "#include <stdint.h>\n#include <string.h>\n" +
                       renderObjCSwiftTypeMetadataHelpers(
                           F.Image, Result.SwiftTypeMetadataPairs, Shared);
  Source += R"(
unsigned char descriptor[1] __asm__("_$ss13ManagedBufferCMn") = { 0 };
int main(void) {
  void **cache = (void **)neverd_swift_type_metadata_2020_1020_cache_address();
  const unsigned char *reference = (const void *)
      neverd_swift_type_metadata_2020_1020_reference_address();
  int32_t relative; uint32_t length;
  memcpy(&relative, reference, 4);
  memcpy(&length, reference + 4, 4);
  const unsigned char *type = reference + relative;
  const char *suffix = "ySDySSSo8NSBundleCGSo16os_unfair_lock_sVG";
  if (length != 5 + strlen(suffix) || type[0] != 2 || *cache) return 1;
  if (memcmp(type + 5, suffix, strlen(suffix))) return 2;
  memcpy(&relative, type + 1, 4);
  const void *resolved;
  memcpy(&resolved, type + 1 + relative, sizeof(resolved));
  if (resolved != descriptor) return 3;
  *cache = descriptor;
  if ((void **)neverd_swift_type_metadata_2020_1020_cache_address() != cache ||
      *cache != descriptor) return 4;
  return 0;
}
)";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-lock-metadata", Directory));
  const std::filesystem::path Work(Directory.str().str());
  struct Cleanup {
    std::filesystem::path Work;
    ~Cleanup() {
      std::error_code Error;
      std::filesystem::remove_all(Work, Error);
    }
  } Cleanup{Work};
  const auto Path = (Work / "metadata.c").string();
  const auto Executable = (Work / "metadata").string();
  const auto ErrorPath = (Work / "stderr").string();
  std::ofstream(Path) << Source;
  const std::string Compiler = NEVERD_TEST_CLANG;
  for (const char *Optimization : {"-O0", "-O2"}) {
    const std::vector<std::string> Arguments{
        Compiler, "-std=c11", Optimization, "-Werror", Path, "-o", Executable};
    std::vector<llvm::StringRef> Refs(Arguments.begin(), Arguments.end());
    const std::optional<llvm::StringRef> Redirects[] = {
        std::nullopt, std::nullopt, ErrorPath};
    std::string Error;
    const auto Status = llvm::sys::ExecuteAndWait(Compiler, Refs, std::nullopt,
                                                  Redirects, 60, 0, &Error);
    auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Status, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "");
    EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable}, std::nullopt,
                                        Redirects, 30, 0, &Error),
              0)
        << Error;
  }
}

TEST(ObjCSourceBindings,
     SwiftImportedLockMetadataRejectsChangedPrivateDescriptor) {
  for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = swiftImportedLockTypeMetadataFixture();
    auto &Descriptor = F.Image.Segments[3].Data;
    if (Mutation == 0)
      Descriptor[0x20] = 0;
    if (Mutation == 1)
      Descriptor[0x80] = 'x';
    if (Mutation == 2)
      F.Image.Symbols.back().Name = "_$sSo16os_unfair_lock_sVMr";
    if (Mutation == 3)
      F.Image.Symbols[F.Image.Symbols.size() - 2].Name = "_$sBadModuleMXM";
    if (Mutation == 4)
      F.Image.Exports.push_back(
          {"_$sOther", 0, SwiftTypeMetadataFixture::LocalDescriptor});
    if (Mutation == 5)
      llvm::support::endian::write32le(Descriptor.data() + 0x2c, 0);
    if (Mutation == 6)
      llvm::support::endian::write32le(Descriptor.data() + 0x24, 0);
    if (Mutation == 7)
      F.Image.DataPtrRelocSlots.insert(
          SwiftTypeMetadataFixture::LocalDescriptor);
    EXPECT_FALSE(objc_binding_detail::swiftTypeMetadataPairProof(
        F.Image, F.Cache, F.Reference));
    const auto Result = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_TRUE(Result.SwiftTypeMetadataPairs.empty());
    EXPECT_FALSE(Result.Limitation.empty());
  }
}

TEST(ObjCSourceBindings, SwiftMetadataPairInFourArgumentValueHelper) {
  auto F = swiftStdlibTypeMetadataFixture();
  auto Call = F.Function.Body.front().Val;
  ASSERT_TRUE(Call && Call->SourceCallHint);
  auto Native = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
  Native->Signature.Parameters.insert(
      Native->Signature.Parameters.begin(),
      {{"destination", NdType::makePtr(NdType::makeVoid())},
       {"source", NdType::makePtr(NdType::makeVoid())}});
  std::string Diagnostic;
  ASSERT_TRUE(
      assignDarwinScalarSourceABI(Native->Signature, F.Image.Arch, Diagnostic))
      << Diagnostic;
  Call->SourceCallHint = Native;
  Call->Operands.insert(
      Call->Operands.begin(),
      {HighExpr::makeConst(0, 8, ConstantAddressProvenance::Scalar),
       HighExpr::makeConst(0, 8, ConstantAddressProvenance::Scalar)});

  auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  ASSERT_EQ(Result.SwiftTypeMetadataPairs.size(), 1U);

  std::swap(Call->Operands[0], Call->Operands[2]);
  std::swap(Call->Operands[1], Call->Operands[3]);
  Result = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_TRUE(Result.SwiftTypeMetadataPairs.empty());
}

TEST(ObjCSourceBindings,
     SwiftSystemFrameworkDescriptorsRequireMatchingInstallNames) {
  constexpr llvm::StringLiteral Descriptor = "_$s7Combine9PublishedVMn";
  EXPECT_TRUE(objc_binding_detail::swiftSystemFrameworkNominalDescriptor(
      Descriptor, "/System/Library/Frameworks/Combine.framework/Combine"));
  EXPECT_TRUE(objc_binding_detail::swiftSystemFrameworkNominalDescriptor(
      Descriptor,
      "/System/Library/Frameworks/Combine.framework/Versions/A/Combine"));
  EXPECT_FALSE(objc_binding_detail::swiftSystemFrameworkNominalDescriptor(
      Descriptor,
      "/System/Library/Frameworks/Foundation.framework/Foundation"));
  EXPECT_FALSE(objc_binding_detail::swiftSystemFrameworkNominalDescriptor(
      Descriptor, "/tmp/Combine.framework/Combine"));
  EXPECT_FALSE(objc_binding_detail::swiftSystemFrameworkNominalDescriptor(
      "_$s7Combine9PublishedVMa",
      "/System/Library/Frameworks/Combine.framework/Combine"));
  EXPECT_FALSE(objc_binding_detail::swiftSystemFrameworkNominalDescriptor(
      "_$s7Combine9PublisherMp",
      "/System/Library/Frameworks/Combine.framework/Combine"));
}

TEST(ObjCSourceBindings,
     SwiftDispatchMetadataPairsRequireExactStrongRuntimeDescriptorBind) {
  for (const bool TimerFlags : {false, true}) {
    SCOPED_TRACE(TimerFlags);
    auto F = swiftDispatchTypeMetadataFixture(TimerFlags);
    const auto Proof = objc_binding_detail::swiftTypeMetadataPairProof(
        F.Image, SwiftTypeMetadataFixture::Cache,
        SwiftTypeMetadataFixture::Reference);
    ASSERT_TRUE(Proof);
    EXPECT_EQ(Proof->Descriptors.size(), 1U);
    const auto Result = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
    ASSERT_EQ(Result.SwiftTypeMetadataPairs.size(), 1U);
    EXPECT_EQ(Result.SwiftTypeMetadataPairs.at(SwiftTypeMetadataFixture::Cache)
                  .DescriptorSymbol,
              TimerFlags ? "_$sSo18OS_dispatch_sourceC8DispatchE10TimerFlagsVMn"
                         : "_$s8Dispatch0A13WorkItemFlagsVMn");

    auto Rejected = [&](auto Change) {
      auto Modified = swiftDispatchTypeMetadataFixture(TimerFlags);
      Change(Modified.Image);
      EXPECT_FALSE(objc_binding_detail::swiftTypeMetadataPairProof(
          Modified.Image, SwiftTypeMetadataFixture::Cache,
          SwiftTypeMetadataFixture::Reference));
    };
    Rejected([](BinaryImage &Image) {
      Image.DyldBindSlots[SwiftTypeMetadataFixture::DescriptorSlot].Module =
          "/tmp/libswiftDispatch.dylib";
    });
    Rejected([](BinaryImage &Image) {
      Image.DyldBindSlots[SwiftTypeMetadataFixture::DescriptorSlot].WeakImport =
          true;
    });
    Rejected([](BinaryImage &Image) {
      Image.DyldBindSlots[SwiftTypeMetadataFixture::DescriptorSlot].Addend = 8;
    });
    Rejected([](BinaryImage &Image) {
      Image.Segments[2].ReadOnlyAfterRelocations = false;
    });
  }
}

TEST(ObjCSourceBindings,
     SwiftProtocolTypeMetadataPairsAcceptExactLocalDescriptorRebases) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    SwiftTypeMetadataFixture F(Architecture, true, true);
    const auto Result = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
    ASSERT_EQ(Result.SwiftTypeMetadataPairs.size(), 1U);
    const auto Pair =
        Result.SwiftTypeMetadataPairs.at(SwiftTypeMetadataFixture::Cache);
    EXPECT_EQ(Pair.DescriptorSlot, SwiftTypeMetadataFixture::DescriptorSlot);
    EXPECT_EQ(Pair.DescriptorSymbol, "_$s7WMFData10WMFServiceMp");
    EXPECT_EQ(Pair.Suffix, "_pSg");

    std::set<std::string> Helpers;
    const auto Source = renderObjCSwiftTypeMetadataHelpers(
        F.Image, Result.SwiftTypeMetadataPairs, Helpers);
    EXPECT_NE(Source.find("__asm__(\"_$s7WMFData10WMFServiceMp\")"),
              std::string::npos);
    EXPECT_NE(Source.find(".type_reference[5] = 95"), std::string::npos);
    const auto &Call = Result.Function.Body[0].Val;
    ASSERT_EQ(Call->Operands.size(), 3U);
    EXPECT_FALSE(Call->Operands[0]->SourceCallHint);
    EXPECT_EQ(Call->Operands[1]->SourceCallHint->CallKind,
              SourceCallTypeHint::Kind::RuntimeSwiftTypeMetadataAddress);
    EXPECT_EQ(Call->Operands[2]->SourceCallHint->CallKind,
              SourceCallTypeHint::Kind::RuntimeSwiftTypeMetadataAddress);
  }
}

TEST(ObjCSourceBindings,
     SwiftNominalTypeMetadataPairsAcceptExactLocalDescriptorRebases) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    SwiftTypeMetadataFixture F(Architecture, false, false, false, true);
    const auto Result = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
    ASSERT_EQ(Result.SwiftTypeMetadataPairs.size(), 1U);
    const auto Pair =
        Result.SwiftTypeMetadataPairs.at(SwiftTypeMetadataFixture::Cache);
    EXPECT_EQ(Pair.DescriptorSlot, SwiftTypeMetadataFixture::DescriptorSlot);
    EXPECT_EQ(Pair.DescriptorSymbol,
              "_$s7WMFData24WMFFeatureConfigResponseVMn");
    EXPECT_EQ(Pair.Suffix, "Sg");

    std::set<std::string> Helpers;
    const auto Source = renderObjCSwiftTypeMetadataHelpers(
        F.Image, Result.SwiftTypeMetadataPairs, Helpers);
    EXPECT_NE(
        Source.find("__asm__(\"_$s7WMFData24WMFFeatureConfigResponseVMn\")"),
        std::string::npos);
  }
}

namespace {
SwiftTypeMetadataFixture
swiftRegisteredInternalTypeFixture(Arch Architecture, char TypeKind = 'C',
                                   bool Nested = false, bool Indirect = false) {
  SwiftTypeMetadataFixture F(Architecture, false, false, false, true);
  constexpr va_t Module = 0x4040, Name = 0x4080, ModuleName = 0x40a0;
  constexpr va_t Records = 0x6000;
  const std::string Type =
      "13WMFComponents10ArticleTab" + std::string(1, TypeKind);
  const std::string Base =
      Nested ? "_$s7Combine9PublishedVySay" + Type + "GG" : "_$s" + Type + "Sg";
  F.Image.Symbols[0].Name = Base + "MR";
  F.Image.Symbols[1].Name = Base + "Md";
  F.Image.Symbols[2].Name = "_$s" + Type + "Mn";
  F.Image.Exports.clear();
  auto &Data = F.Image.Segments[3].Data;
  const auto Relative = [](uint8_t *Field, va_t Location, va_t Target) {
    llvm::support::endian::write32le(
        Field, static_cast<uint32_t>(static_cast<int32_t>(Target - Location)));
  };
  llvm::support::endian::write32le(Data.data() + F.LocalDescriptor - 0x4000,
                                   0x40 | (TypeKind == 'C'   ? 16
                                           : TypeKind == 'V' ? 17
                                                             : 18));
  Relative(Data.data() + F.LocalDescriptor + 4 - 0x4000, F.LocalDescriptor + 4,
           Module);
  Relative(Data.data() + F.LocalDescriptor + 8 - 0x4000, F.LocalDescriptor + 8,
           Name);
  Relative(Data.data() + Module + 8 - 0x4000, Module + 8, ModuleName);
  std::memcpy(Data.data() + Name - 0x4000, "ArticleTab", 11);
  std::memcpy(Data.data() + ModuleName - 0x4000, "WMFComponents", 14);
  Segment Mapping;
  Mapping.Name = "__TEXT";
  Mapping.VA = Mapping.FileOff = Records;
  Mapping.Size = Mapping.FileSz = 0x100;
  Mapping.Flags = SegmentFlags::Readable;
  Mapping.Data.resize(0x100);
  Relative(Mapping.Data.data(), Records, F.LocalDescriptor);
  F.Image.Segments.push_back(std::move(Mapping));
  Section Registration;
  Registration.Name = "__swift5_types";
  Registration.SegmentName = "__TEXT";
  Registration.VA = Registration.FileOff = Records;
  Registration.Size = Registration.FileSz = 4;
  Registration.Flags = SegmentFlags::Readable;
  F.Image.Sections.push_back(Registration);
  auto &Reference = F.Image.Segments[0].Data;
  auto *Bytes = Reference.data() + F.TypeReference - 0x1000;
  const unsigned Offset = Nested ? 9 : 0;
  if (Nested) {
    Bytes[0] = 2;
    Relative(Bytes + 1, F.TypeReference + 1, F.NestedDescriptorSlot);
    std::memcpy(Bytes + 5, "ySay", 4);
    EXPECT_TRUE(F.Image.recordDyldBindSlot(
        F.NestedDescriptorSlot, "_$s7Combine9PublishedVMn", 0,
        "/System/Library/Frameworks/Combine.framework/Combine", false));
  }
  Bytes[Offset] = Indirect ? 2 : 1;
  Relative(Bytes + Offset + 1, F.TypeReference + Offset + 1,
           Indirect ? F.DescriptorSlot : F.LocalDescriptor);
  std::memcpy(Bytes + Offset + 5, Nested ? "GG" : "Sg", 3);
  llvm::support::endian::write32le(Reference.data() + F.Reference + 4 - 0x1000,
                                   Offset + 7);
  return F;
}
} // namespace

namespace {
SwiftTypeMetadataFixture
swiftRegisteredProtocolRecipeFixture(Arch Architecture, bool Indirect,
                                     llvm::StringRef Container = "optional") {
  auto F =
      swiftRegisteredInternalTypeFixture(Architecture, 'C', false, Indirect);
  const std::string Type = "13WMFComponents10ArticleTab";
  const std::string Prefix = Container == "array" ? "Say" : "";
  const std::string Suffix = Container == "array"      ? "_pG"
                             : Container == "optional" ? "_pSg"
                                                       : "_p";
  const std::string Base = "_$s" + Prefix + Type + Suffix;
  F.Image.Symbols[0].Name = Base + "MR";
  F.Image.Symbols[1].Name = Base + "Md";
  F.Image.Symbols[2].Name = "_$s" + Type + "Mp";
  auto &Descriptor = F.Image.Segments[3].Data;
  llvm::support::endian::write32le(
      Descriptor.data() + F.LocalDescriptor - 0x4000, 0x10043);
  F.Image.Sections.back().Name = "__swift5_protos";
  auto &Reference = F.Image.Segments[0].Data;
  auto *Bytes = Reference.data() + F.TypeReference - 0x1000;
  std::memcpy(Bytes, Prefix.data(), Prefix.size());
  Bytes[Prefix.size()] = Indirect ? 2 : 1;
  llvm::support::endian::write32le(
      Bytes + Prefix.size() + 1,
      uint32_t((Indirect ? F.DescriptorSlot : F.LocalDescriptor) -
               (F.TypeReference + Prefix.size() + 1)));
  std::memcpy(Bytes + Prefix.size() + 5, Suffix.c_str(), Suffix.size() + 1);
  llvm::support::endian::write32le(Reference.data() + F.Reference + 4 - 0x1000,
                                   Prefix.size() + 5 + Suffix.size());
  return F;
}
} // namespace

TEST(ObjCSourceBindings, RegisteredProtocolRecipesKeepTheirExactRuntimeType) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (const bool Indirect : {false, true})
      for (const auto Container : {"optional", "plain", "array"}) {
        SCOPED_TRACE(Container);
        auto F = swiftRegisteredProtocolRecipeFixture(Architecture, Indirect,
                                                      Container);
        const auto Proof = objc_binding_detail::swiftTypeMetadataPairProof(
            F.Image, F.Cache, F.Reference);
        ASSERT_TRUE(Proof);
        EXPECT_TRUE(Proof->Descriptors.empty());
        EXPECT_EQ("_$s" + Proof->TypeReference + "MR", F.Image.Symbols[0].Name);
        const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
        ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
        ASSERT_EQ(Bound.SwiftTypeMetadataPairs.size(), 1U);
        for (const auto &Arg : Bound.Function.Body[0].Val->Operands)
          EXPECT_TRUE(objcSourceCallBound(*Arg, F.Image, {}));
        std::set<std::string> Names;
        const auto Source = renderObjCSwiftTypeMetadataHelpers(
            F.Image, Bound.SwiftTypeMetadataPairs, Names);
        EXPECT_EQ(Source.find("__asm__("), std::string::npos);
      }
}

TEST(ObjCSourceBindings,
     RegisteredProtocolRecipesRejectStaleOrIncompleteProof) {
  for (unsigned Mutation = 0; Mutation != 17; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = swiftRegisteredProtocolRecipeFixture(Arch::AArch64, true);
    const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_EQ(Bound.SwiftTypeMetadataPairs.size(), 1U);
    auto &Data = F.Image.Segments[3].Data;
    const auto Word = [&](unsigned Offset, uint32_t Value) {
      llvm::support::endian::write32le(Data.data() + Offset, Value);
    };
    switch (Mutation) {
    case 0:
      F.Image.Sections.back().Name = "__swift5_types";
      break;
    case 1:
      Word(0x2c, 1); // Requirement signature.
      break;
    case 2:
      Word(0x34, 0x4080 - 0x4034); // Associated type names.
      break;
    case 3:
      Word(0x30, 513);
      break;
    case 4:
      Word(0x20, 0x50043); // Unsupported special protocol.
      break;
    case 5:
      Word(0x20, 0x10143); // Unknown descriptor version.
      break;
    case 6:
      Data[0x80] = 'X';
      break;
    case 7:
      Data[0xa0] = 'X';
      break;
    case 8:
      F.Image.MachOResolvedChainedPointerSlots.erase(F.DescriptorSlot);
      break;
    case 9:
      F.Image.DataPtrRelocTargetOwners[F.DescriptorSlot] = 0x5000;
      break;
    case 10:
      F.Image.Exports.push_back({F.Image.Symbols[2].Name, 0, 0x4080});
      break;
    case 11:
      F.Image.Symbols.push_back({F.Image.Symbols[2].Name, 0x4080, 0, false});
      break;
    case 12:
      F.Image.Symbols[0].Name = "_$s13WMFComponents10ArticleTab_pMR";
      F.Image.Symbols[1].Name = "_$s13WMFComponents10ArticleTab_pMd";
      break;
    case 13:
      F.Image.Segments[0].Data[0x89] = 'X';
      break;
    case 14:
      Word(0x30, 32); // Incomplete requirement records.
      break;
    case 15:
      F.Image.Symbols[2].IsFunc = true;
      break;
    case 16:
      F.Image.Segments[3].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
      break;
    }
    EXPECT_FALSE(objc_binding_detail::swiftTypeMetadataPairProof(
        F.Image, F.Cache, F.Reference));
    EXPECT_TRUE(bindObjCSourceReferences(F.Function, F.Image)
                    .SwiftTypeMetadataPairs.empty());
    for (const auto &Arg : Bound.Function.Body[0].Val->Operands)
      EXPECT_FALSE(objcSourceCallBound(*Arg, F.Image, {}));
    std::set<std::string> Names;
    EXPECT_THROW(renderObjCSwiftTypeMetadataHelpers(
                     F.Image, Bound.SwiftTypeMetadataPairs, Names),
                 std::runtime_error);
  }
}

namespace {
SwiftTypeMetadataFixture
swiftPrivateFieldTypeFixture(Arch Architecture, bool Indirect = false,
                             bool ImportedTypedef = false) {
  SwiftTypeMetadataFixture F(Architecture, false, false, false, true);
  const std::string Type =
      ImportedTypedef ? "_$sSo21NSAttributedStringKeya"
                      : "_$s4Demo6Hidden33_0123456789ABCDEF0123456789ABCDEFLLV";
  F.Image.Symbols[0].Name = Type + "SgMR";
  F.Image.Symbols[1].Name = Type + "SgMd";
  F.Image.Symbols[2].Name = Type + "Mn";
  F.Image.Exports = {{"_$s4Demo6HolderCMn", 0, 0x40a0}};
  F.Image.Symbols.push_back({"_$s4Demo6HolderCMn", 0x40a0, 20, false});
  auto &Data = F.Image.Segments[3].Data;
  const auto Word = [&](unsigned Offset, uint32_t Value) {
    llvm::support::endian::write32le(Data.data() + Offset, Value);
  };
  Word(0x20, 0x51);            // Private struct descriptor.
  Word(0xa0, 0x50);            // Exported class descriptor.
  Word(0xb0, 0x40c0 - 0x40b0); // Class -> fields.
  Word(0xc8, (12u << 16) | 7); // ObjC class, 12-byte field record.
  Word(0xcc, 1);
  Word(0xd0, 2); // Mutable stored property.
  Word(0xd4, uint32_t(F.TypeReference - 0x40d4));
  auto &Reference = F.Image.Segments[0].Data;
  Reference[0x80] = Indirect ? 2 : 1;
  llvm::support::endian::write32le(
      Reference.data() + 0x81,
      uint32_t((Indirect ? F.DescriptorSlot : F.LocalDescriptor) - 0x1081));
  if (ImportedTypedef) {
    Word(0x20, 0x60011);
    Word(0x24, 0x4040 - 0x4024);
    Word(0x28, 0x4060 - 0x4028);
    Word(0x48, 0x4090 - 0x4048);
    std::memcpy(Data.data() + 0x60, "Key\0NNSAttributedStringKey\0St\0", 31);
    std::memcpy(Data.data() + 0x90, "__C", 4);
  }
  return F;
}
} // namespace

TEST(ObjCSourceBindings,
     PrivateSwiftTypeKeepsDescriptorIdentityThroughExportedFieldMetadata) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (const bool Indirect : {false, true}) {
      auto F = swiftPrivateFieldTypeFixture(Architecture, Indirect);
      const auto Proof = objc_binding_detail::swiftTypeMetadataPairProof(
          F.Image, F.Cache, F.Reference);
      ASSERT_TRUE(Proof);
      ASSERT_EQ(Proof->Descriptors.size(), 1U);
      ASSERT_TRUE(Proof->Descriptors[0].FieldPath);
      EXPECT_EQ(Proof->Descriptors[0].FieldPath->ExportSymbol,
                "_$s4Demo6HolderCMn");
      EXPECT_EQ(Proof->Descriptors[0].FieldPath->RelativeOffsets,
                (std::array<uint32_t, 3>{16, 20, 1}));
      EXPECT_EQ(Proof->Descriptors[0].FieldPath->IndirectDescriptor, Indirect);
      const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      ASSERT_EQ(Bound.SwiftTypeMetadataPairs.size(), 1U);
      for (const auto &Arg : Bound.Function.Body[0].Val->Operands)
        EXPECT_TRUE(objcSourceCallBound(*Arg, F.Image, {}));
      std::set<std::string> Shared;
      auto Source = "#include <stdint.h>\n#include <string.h>\n" +
                    renderObjCSwiftTypeMetadataHelpers(
                        F.Image, Bound.SwiftTypeMetadataPairs, Shared);
      EXPECT_NE(Source.find("__asm__(\"_$s4Demo6HolderCMn\")"),
                std::string::npos);
      EXPECT_EQ(Source.find("Hidden33_"), std::string::npos);
      Source += std::string("#define INDIRECT ") + (Indirect ? "1\n" : "0\n");
      Source += R"(
unsigned char anchor[1024] __asm__("_$s4Demo6HolderCMn");
static void relative(unsigned offset, unsigned target) {
  int32_t delta = (int32_t)target - (int32_t)offset;
  memcpy(anchor + offset, &delta, 4);
}
int main(void) {
  relative(16, 256);
  relative(256 + 20, 128);
  relative(128 + 1, INDIRECT ? 640 : 512);
  const void *original = anchor + 512;
  memcpy(anchor + 640, &original, sizeof(original));
  void **cache = (void **)neverd_swift_type_metadata_2020_1020_cache_address();
  const unsigned char *ref = (const void *)
      neverd_swift_type_metadata_2020_1020_reference_address();
  int32_t delta; uint32_t length;
  memcpy(&delta, ref, 4); memcpy(&length, ref + 4, 4);
  const unsigned char *type = ref + delta;
  if (*cache || length != 7 || type[0] != 2 || memcmp(type + 5, "Sg", 3))
    return 1;
  memcpy(&delta, type + 1, 4);
  const void *descriptor;
  memcpy(&descriptor, type + 1 + delta, sizeof(descriptor));
  if (descriptor != anchor + 512) return 2;
  *cache = anchor + 768;
  for (unsigned i = 0; i != 128; ++i) {
    if ((void **)neverd_swift_type_metadata_2020_1020_cache_address() != cache ||
        *cache != anchor + 768 ||
        (const void *)neverd_swift_type_metadata_2020_1020_reference_address() != ref)
      return 3;
  }
  return 0;
}
)";
      llvm::SmallString<128> Directory;
      ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-field-metadata",
                                                        Directory));
      const std::filesystem::path Work(Directory.str().str());
      struct Cleanup {
        std::filesystem::path Work;
        ~Cleanup() {
          std::error_code Error;
          std::filesystem::remove_all(Work, Error);
        }
      } Cleanup{Work};
      const auto Path = (Work / "field.c").string();
      const auto Executable = (Work / "field").string();
      const auto ErrorPath = (Work / "stderr").string();
      std::ofstream(Path) << Source;
      const std::string Compiler = NEVERD_TEST_CLANG;
      for (const char *Optimization : {"-O0", "-O2"}) {
        const std::vector<std::string> Arguments{
            Compiler, "-std=c11", Optimization, "-Werror",
            Path,     "-o",       Executable};
        std::vector<llvm::StringRef> Refs(Arguments.begin(), Arguments.end());
        const std::optional<llvm::StringRef> Redirects[] = {
            std::nullopt, std::nullopt, ErrorPath};
        std::string Error;
        const auto Status = llvm::sys::ExecuteAndWait(
            Compiler, Refs, std::nullopt, Redirects, 30, 0, &Error);
        auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
        ASSERT_EQ(Status, 0)
            << Error << (Errors ? (*Errors)->getBuffer().str() : "");
        EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable},
                                            std::nullopt, Redirects, 30, 0,
                                            &Error),
                  0)
            << Error;
      }
    }
}

TEST(ObjCSourceBindings,
     PrivateSwiftTypedefIdentityRequiresCompleteImportInfo) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (const bool Indirect : {false, true}) {
      auto F = swiftPrivateFieldTypeFixture(Architecture, Indirect, true);
      // A same-named descriptor elsewhere does not replace this field's target.
      auto Copy = F.Image.Symbols[2];
      Copy.Addr = 0x40e0;
      F.Image.Symbols.push_back(Copy);
      for (const bool Renamed : {true, false}) {
        if (!Renamed) {
          auto &Data = F.Image.Segments[3].Data;
          std::memcpy(Data.data() + 0x60, "NSAttributedStringKey\0St\0", 26);
        }
        const auto Proof = objc_binding_detail::swiftTypeMetadataPairProof(
            F.Image, F.Cache, F.Reference);
        ASSERT_TRUE(Proof);
        ASSERT_EQ(Proof->Descriptors.size(), 1U);
        EXPECT_EQ(Proof->Descriptors[0].Symbol,
                  "_$sSo21NSAttributedStringKeyaMn");
        ASSERT_TRUE(Proof->Descriptors[0].FieldPath);
        EXPECT_EQ(Proof->Descriptors[0].FieldPath->IndirectDescriptor,
                  Indirect);
        const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
        ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
        ASSERT_EQ(Bound.SwiftTypeMetadataPairs.size(), 1U);
        for (const auto &Arg : Bound.Function.Body[0].Val->Operands)
          EXPECT_TRUE(objcSourceCallBound(*Arg, F.Image, {}));
        std::set<std::string> Shared;
        const auto Source = renderObjCSwiftTypeMetadataHelpers(
            F.Image, Bound.SwiftTypeMetadataPairs, Shared);
        EXPECT_EQ(Source.find("NSAttributedStringKeyaMn"), std::string::npos);
        EXPECT_NE(Source.find("__asm__(\"_$s4Demo6HolderCMn\")"),
                  std::string::npos);
      }
    }
}

TEST(ObjCSourceBindings, PrivateSwiftTypedefRejectsChangedIdentityAndStorage) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation != 23; ++Mutation) {
      SCOPED_TRACE(Mutation);
      auto F = swiftPrivateFieldTypeFixture(Architecture, true, true);
      const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_EQ(Bound.SwiftTypeMetadataPairs.size(), 1U);
      auto &Data = F.Image.Segments[3].Data;
      const auto Word = [&](unsigned Offset, uint32_t Value) {
        llvm::support::endian::write32le(Data.data() + Offset, Value);
      };
      switch (Mutation) {
      case 0:
        Word(0x20, 0x20011);
        break; // Missing import info.
      case 1:
        Word(0x20, 0x60012);
        break; // Wrong descriptor kind.
      case 2:
        Word(0x20, 0x60111);
        break; // Unknown version.
      case 3:
        Word(0x20, 0x60091);
        break; // Generic type.
      case 4:
        Word(0x24, 0x1d);
        break; // Indirect parent.
      case 5:
        Word(0x40, 0x40);
        break; // Unproved module flags.
      case 6:
        Word(0x44, 4);
        break; // Nested module.
      case 7:
        Data[0x92] = 'D';
        break;
      case 8:
        Word(0x48, 0);
        break;
      case 9:
        Data[0x64] = 'R';
        break; // Related entity, not ABI name.
      case 10:
        Data[0x65] = 'X';
        break; // Wrong ABI name.
      case 11:
        Data[0x7c] = 'x';
        break; // Wrong namespace.
      case 12:
        Data[0x7b] = 'N';
        break; // Duplicate ABI name.
      case 13:
        Data[0x7e] = 'R';
        break; // Trailing component.
      case 14:
        Data[0x60] = 0;
        break; // Empty formal name.
      case 15:
        Word(0x28, 0);
        break;
      case 16:
        F.Image.DataPtrRelocSlots.insert(0x4024);
        break;
      case 17:
        F.Image.DataPtrRelocSlots.insert(0x4028);
        break;
      case 18:
        F.Image.DataPtrRelocSlots.insert(0x407b);
        break;
      case 19:
        F.Image.DataPtrRelocSlots.insert(0x4090);
        break;
      case 20: {
        for (unsigned I = 0; I != 3; ++I)
          F.Image.Symbols[I].Name.replace(3, 2, "4Demo");
        break;
      }
      case 21: {
        // Equal cache and descriptor names cannot override a different ABI
        // name.
        for (unsigned I = 0; I != 3; ++I)
          F.Image.Symbols[I].Name.replace(5, 23, "5Other");
        break;
      }
      case 22:
        std::fill(Data.begin() + 0x60, Data.end(), 'A');
        break;
      }
      EXPECT_FALSE(objc_binding_detail::swiftTypeMetadataPairProof(
          F.Image, F.Cache, F.Reference));
      EXPECT_TRUE(bindObjCSourceReferences(F.Function, F.Image)
                      .SwiftTypeMetadataPairs.empty());
      for (const auto &Arg : Bound.Function.Body[0].Val->Operands)
        EXPECT_FALSE(objcSourceCallBound(*Arg, F.Image, {}));
      std::set<std::string> Shared;
      EXPECT_THROW(renderObjCSwiftTypeMetadataHelpers(
                       F.Image, Bound.SwiftTypeMetadataPairs, Shared),
                   std::runtime_error);
    }
}

TEST(ObjCSourceBindings,
     PrivateSwiftIndirectFieldRejectsUnprovenPointerStorage) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
      SCOPED_TRACE(Mutation);
      auto F = swiftPrivateFieldTypeFixture(Architecture, true);
      const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_EQ(Bound.SwiftTypeMetadataPairs.size(), 1U);
      switch (Mutation) {
      case 0:
        F.Image.MachOResolvedChainedPointerSlots.clear();
        break;
      case 1:
        F.Image.Segments[2].ReadOnlyAfterRelocations = false;
        break;
      case 2:
        F.Image.ConflictingImportStorageSlots.insert(F.DescriptorSlot);
        break;
      case 3:
        F.Image.MachOChainedFixupsAmbiguous = true;
        break;
      case 4:
        ASSERT_TRUE(F.Image.recordDyldBindSlot(F.DescriptorSlot,
                                               "_$s4Demo5OtherVMn", 0,
                                               "/tmp/other.dylib", false));
        break;
      case 5:
        F.Image.Segments[2].Data.resize(0x27);
        break;
      case 6:
        F.Image.Sections.push_back(F.Image.Sections[2]);
        break;
      case 7:
        llvm::support::endian::write64le(F.Image.Segments[2].Data.data() + 0x20,
                                         F.LocalDescriptor + 4);
        break;
      case 8:
        llvm::support::endian::write32le(
            F.Image.Segments[0].Data.data() + 0x81,
            uint32_t(F.DescriptorSlot + 1 - 0x1081));
        break;
      case 9:
        F.Image.Symbols.push_back(F.Image.Symbols[2]);
        break;
      }
      EXPECT_FALSE(objc_binding_detail::swiftTypeMetadataPairProof(
          F.Image, F.Cache, F.Reference));
      for (const auto &Arg : Bound.Function.Body[0].Val->Operands)
        EXPECT_FALSE(objcSourceCallBound(*Arg, F.Image, {}));
      std::set<std::string> Shared;
      EXPECT_THROW(renderObjCSwiftTypeMetadataHelpers(
                       F.Image, Bound.SwiftTypeMetadataPairs, Shared),
                   std::runtime_error);
    }
}

TEST(ObjCSourceBindings, PrivateSwiftFieldCanWrapTheSameOriginalDescriptor) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (const bool Indirect : {false, true}) {
      auto F = swiftPrivateFieldTypeFixture(Architecture, false, true);
      auto &Reference = F.Image.Segments[0].Data;
      auto &Data = F.Image.Segments[3].Data;
      llvm::support::endian::write32le(Data.data() + 0xd4,
                                       uint32_t(0x10a0 - 0x40d4));
      std::memcpy(Reference.data() + 0xa0, "SDy", 3);
      Reference[0xa3] = Indirect ? 2 : 1;
      llvm::support::endian::write32le(
          Reference.data() + 0xa4,
          uint32_t((Indirect ? F.DescriptorSlot : F.LocalDescriptor) - 0x10a4));
      std::memcpy(Reference.data() + 0xa8, "ypGSg", 6);
      const auto Proof = objc_binding_detail::swiftTypeMetadataPairProof(
          F.Image, F.Cache, F.Reference);
      ASSERT_TRUE(Proof);
      ASSERT_EQ(Proof->Descriptors.size(), 1U);
      ASSERT_TRUE(Proof->Descriptors[0].FieldPath);
      EXPECT_EQ(Proof->Descriptors[0].FieldPath->RelativeOffsets,
                (std::array<uint32_t, 3>{16, 20, 4}));
      EXPECT_EQ(Proof->Descriptors[0].FieldPath->IndirectDescriptor, Indirect);
      auto Copy = F.Image.Symbols[2];
      Copy.Addr = 0x40e0;
      F.Image.Symbols.push_back(Copy);
      const auto Original = Reference;
      for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
        Reference = Original;
        if (Mutation == 0)
          Reference[0xad] = 3; // Unknown reference kind.
        if (Mutation == 1)
          Reference[0xa3] = 3;
        if (Mutation == 2)
          llvm::support::endian::write32le(Reference.data() + 0xa4, 0);
        if (Mutation == 3)
          std::fill(Reference.begin() + 0xad, Reference.end(), 'G');
        if (Mutation == 4)
          F.Image.DataPtrRelocSlots.insert(0x10a4);
        if (Mutation == 5) {
          F.Image.DataPtrRelocSlots.erase(0x10a4);
          llvm::support::endian::write32le(Reference.data() + 0x81,
                                           uint32_t(Copy.Addr - 0x1081));
        }
        EXPECT_FALSE(objc_binding_detail::swiftTypeMetadataPairProof(
            F.Image, F.Cache, F.Reference))
            << Mutation;
      }
    }
}

TEST(ObjCSourceBindings,
     PrivateSwiftFieldPathBoundsWorkSeparatelyFromOtherExports) {
  auto F = swiftPrivateFieldTypeFixture(Arch::AArch64);
  const auto Root = F.Image.Exports.front();
  F.Image.Exports.assign(75000, {});
  F.Image.Exports.back() = Root;
  const auto Proof = objc_binding_detail::swiftTypeMetadataPairProof(
      F.Image, F.Cache, F.Reference);
  ASSERT_TRUE(Proof);
  ASSERT_EQ(Proof->Descriptors.size(), 1U);
  ASSERT_TRUE(Proof->Descriptors[0].FieldPath);
  EXPECT_EQ(Proof->Descriptors[0].FieldPath->ExportSymbol, Root.Name);
}

TEST(ObjCSourceBindings,
     PrivateSwiftFieldPathRejectsChangedEdgesExportsAndStorage) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation != 24; ++Mutation) {
      SCOPED_TRACE(Mutation);
      auto F = swiftPrivateFieldTypeFixture(Architecture);
      const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_EQ(Bound.SwiftTypeMetadataPairs.size(), 1U);
      auto &Data = F.Image.Segments[3].Data;
      const auto Word = [&](unsigned Offset, uint32_t Value) {
        llvm::support::endian::write32le(Data.data() + Offset, Value);
      };
      switch (Mutation) {
      case 0:
        F.Image.Exports.clear();
        break;
      case 1:
        F.Image.Exports[0].Name = "_$s4Demo5OtherCMn";
        break;
      case 2:
        F.Image.Exports[0].Addr += 4;
        break;
      case 3:
        F.Image.Exports.push_back(F.Image.Exports[0]);
        break;
      case 4:
        F.Image.Symbols.push_back(F.Image.Symbols.back());
        break;
      case 5:
        F.Image.DataPtrRelocSlots.insert(0x40b0);
        break;
      case 6:
        F.Image.DataPtrRelocSlots.insert(0x40d4);
        break;
      case 7:
        F.Image.DataPtrRelocSlots.insert(0x1081);
        break;
      case 8:
        Word(0xb0, 0);
        break;
      case 9:
        Word(0xd4, 0);
        break;
      case 10:
        Word(0xc8, (16u << 16) | 7);
        break;
      case 11:
        Word(0xcc, 4097);
        break;
      case 12:
        Word(0xcc, 0);
        break;
      case 13:
        Word(0xd0, 4);
        break;
      case 14:
        Word(0xa0, 0x150);
        break;
      case 15:
        Word(0x20, 0x50);
        break;
      case 16:
        F.Image.Symbols[2].Name += "x";
        break;
      case 17:
        F.Image.Symbols[1].Name = "_$s4Demo5OtherVSgMd";
        break;
      case 18:
        F.Image.Segments[3].Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
        F.Image.Sections[3].Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
        break;
      case 19:
        F.Image.Segments[3].Data.resize(0xd7);
        break;
      case 20:
        F.Image.Sections.push_back(F.Image.Sections[3]);
        break;
      case 21:
        F.Image.Symbols.push_back(F.Image.Symbols[2]);
        break;
      case 22:
        F.Image.Exports.push_back(
            {F.Image.Symbols[2].Name, 0, F.LocalDescriptor});
        break;
      case 23:
        F.Image.Exports.resize(262145);
        break;
      }
      EXPECT_FALSE(objc_binding_detail::swiftTypeMetadataPairProof(
          F.Image, F.Cache, F.Reference));
      EXPECT_TRUE(bindObjCSourceReferences(F.Function, F.Image)
                      .SwiftTypeMetadataPairs.empty());
      for (const auto &Arg : Bound.Function.Body[0].Val->Operands)
        EXPECT_FALSE(objcSourceCallBound(*Arg, F.Image, {}));
      std::set<std::string> Shared;
      EXPECT_THROW(renderObjCSwiftTypeMetadataHelpers(
                       F.Image, Bound.SwiftTypeMetadataPairs, Shared),
                   std::runtime_error);
    }
}

TEST(ObjCSourceBindings,
     RegisteredNestedSwiftTypesRequireTheirCompleteNamedContext) {
  const auto Fixture = [](Arch Architecture, char ParentKind, char ChildKind,
                          bool Deep = false) {
    auto F = swiftRegisteredInternalTypeFixture(Architecture, ChildKind);
    const std::string Type = "13WMFComponents" +
                             std::string(Deep ? "5GroupC" : "") + "9Container" +
                             ParentKind + "10ArticleTab" + ChildKind;
    F.Image.Symbols[0].Name = "_$s" + Type + "SgMR";
    F.Image.Symbols[1].Name = "_$s" + Type + "SgMd";
    F.Image.Symbols[2].Name = "_$s" + Type + "Mn";
    auto &Data = F.Image.Segments[3].Data;
    const auto Relative = [&](va_t Field, va_t Target) {
      llvm::support::endian::write32le(Data.data() + Field - 0x4000,
                                       uint32_t(Target - Field));
    };
    llvm::support::endian::write32le(Data.data() + 0xc0,
                                     0x40 | (ParentKind == 'C'   ? 16
                                             : ParentKind == 'V' ? 17
                                                                 : 18));
    Relative(F.LocalDescriptor + 4, 0x40c0);
    Relative(0x40c4, Deep ? 0x4060 : 0x4040);
    Relative(0x40c8, 0x40e0);
    std::memcpy(Data.data() + 0xe0, "Container", 10);
    if (Deep) {
      llvm::support::endian::write32le(Data.data() + 0x60, 0x50);
      Relative(0x4064, 0x4040);
      Relative(0x4068, 0x4090);
      std::memcpy(Data.data() + 0x90, "Group", 6);
    }
    return F;
  };
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const char ParentKind : {'C', 'V', 'O'})
      for (const char ChildKind : {'C', 'V', 'O'})
        for (const bool Deep : {false, true}) {
          auto F = Fixture(Architecture, ParentKind, ChildKind, Deep);
          const auto Identity =
              objc_binding_detail::swiftLocalRegisteredNominalType(
                  F.Image, F.LocalDescriptor);
          ASSERT_TRUE(Identity) << ParentKind << ChildKind << Deep;
          const auto Proof = objc_binding_detail::swiftTypeMetadataPairProof(
              F.Image, F.Cache, F.Reference);
          ASSERT_TRUE(Proof);
          EXPECT_TRUE(Proof->Descriptors.empty());
          EXPECT_EQ(Proof->Address.Suffix, *Identity + "Sg");
          const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
          ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
          ASSERT_EQ(Bound.SwiftTypeMetadataPairs.size(), 1U);
          for (const auto &Arg : Bound.Function.Body[0].Val->Operands)
            EXPECT_TRUE(objcSourceCallBound(*Arg, F.Image, {}));
        }
    for (unsigned Mutation = 0; Mutation != 14; ++Mutation) {
      auto F = Fixture(Architecture, 'C', 'V', true);
      auto &Data = F.Image.Segments[3].Data;
      const auto Word = [&](unsigned Offset, uint32_t Value) {
        llvm::support::endian::write32le(Data.data() + Offset, Value);
      };
      switch (Mutation) {
      case 0:
        Data[0xe0] = 'X';
        break; // Different immediate parent name.
      case 1:
        Data[0x90] = 'X';
        break; // Different outer name.
      case 2:
        Data[0xa0] = 'X';
        break; // Different module.
      case 3:
        Word(0xc0, 0x51);
        break; // Wrong parent nominal kind.
      case 4:
        Word(0xc0, 0x150);
        break; // Unknown parent version.
      case 5:
        Word(0xc0, 0xd0);
        break; // Generic parent needs context arguments.
      case 6:
        Word(0xc0, 0x10);
        break; // Parent has no unique identity.
      case 7:
        Word(0xc4, uint32_t(0x4060 - 0x40c4) | 1);
        break;
      case 8:
        Word(0xc4, uint32_t(0x40c0 - 0x40c4));
        break; // Parent cycle.
      case 9:
        Word(0x64, uint32_t(F.LocalDescriptor - 0x4064));
        break;
      case 10:
        Word(0x60, 0x42);
        break; // Anonymous outer context.
      case 11:
        F.Image.Sections[3].Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
        break;
      case 12:
        F.Image.RelDataPtrRelocSlots.insert(0x40c8);
        break;
      case 13: {
        // A different registered leaf with the identical complete path vetoes
        // the identity even if no symbol gives that duplicate a name.
        std::memcpy(Data.data(), Data.data() + 0x20, 12);
        Word(4, uint32_t(0x40c0 - 0x4004));
        Word(8, uint32_t(0x4080 - 0x4008));
        F.Image.Sections.back().Size = F.Image.Sections.back().FileSz = 8;
        llvm::support::endian::write32le(
            F.Image.Segments.back().Data.data() + 4, uint32_t(0x4000 - 0x6004));
        break;
      }
      }
      EXPECT_FALSE(objc_binding_detail::swiftLocalRegisteredNominalType(
          F.Image, F.LocalDescriptor))
          << Mutation;
      EXPECT_FALSE(objc_binding_detail::swiftTypeMetadataPairProof(
          F.Image, F.Cache, F.Reference))
          << Mutation;
    }
  }
}

TEST(ObjCSourceBindings,
     RegisteredInternalSwiftTypesRebuildOnlyTheirStableTextualIdentity) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (const char Kind : {'C', 'V', 'O'})
      for (const bool Nested : {false, true}) {
        SCOPED_TRACE(static_cast<int>(Architecture));
        SCOPED_TRACE(Kind);
        SCOPED_TRACE(Nested);
        auto F = swiftRegisteredInternalTypeFixture(Architecture, Kind, Nested);
        const auto Proof = objc_binding_detail::swiftTypeMetadataPairProof(
            F.Image, F.Cache, F.Reference);
        ASSERT_TRUE(Proof);
        EXPECT_EQ(Proof->Descriptors.size(), Nested ? 1U : 0U);
        const std::string Type =
            "13WMFComponents10ArticleTab" + std::string(1, Kind);
        EXPECT_EQ(Proof->Address.Suffix,
                  Nested ? "ySay" + Type + "GG" : Type + "Sg");
        const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
        ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
        ASSERT_EQ(Bound.SwiftTypeMetadataPairs.size(), 1U);
        const auto &Call = Bound.Function.Body[0].Val;
        for (const auto &Arg : Call->Operands)
          ASSERT_TRUE(objcSourceCallBound(*Arg, F.Image, {}));
        std::set<std::string> Helpers;
        const auto Source = renderObjCSwiftTypeMetadataHelpers(
            F.Image, Bound.SwiftTypeMetadataPairs, Helpers);
        EXPECT_EQ(Source.find("ArticleTabCMn"), std::string::npos);
        EXPECT_EQ(Source.find("ArticleTabVMn"), std::string::npos);
        EXPECT_EQ(Source.find("ArticleTabOMn"), std::string::npos);
        if (Nested)
          EXPECT_NE(Source.find("_$s7Combine9PublishedVMn"), std::string::npos);
        else
          EXPECT_EQ(Source.find("__asm__("), std::string::npos);
      }
}

TEST(ObjCSourceBindings,
     RegisteredSwiftGOTReferencesKeepTheDirectTypeIdentity) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (const char Kind : {'C', 'V', 'O'})
      for (const bool Nested : {false, true}) {
        SCOPED_TRACE(static_cast<int>(Architecture));
        SCOPED_TRACE(Kind);
        SCOPED_TRACE(Nested);
        auto Direct =
            swiftRegisteredInternalTypeFixture(Architecture, Kind, Nested);
        auto Indirect = swiftRegisteredInternalTypeFixture(Architecture, Kind,
                                                           Nested, true);
        const auto Bound =
            bindObjCSourceReferences(Indirect.Function, Indirect.Image);
        ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
        ASSERT_EQ(Bound.SwiftTypeMetadataPairs.size(), 1U);
        for (const auto &Arg : Bound.Function.Body[0].Val->Operands)
          EXPECT_TRUE(objcSourceCallBound(*Arg, Indirect.Image, {}));
        const auto Original =
            bindObjCSourceReferences(Direct.Function, Direct.Image);
        std::set<std::string> DirectNames, IndirectNames;
        EXPECT_EQ(
            renderObjCSwiftTypeMetadataHelpers(
                Direct.Image, Original.SwiftTypeMetadataPairs, DirectNames),
            renderObjCSwiftTypeMetadataHelpers(
                Indirect.Image, Bound.SwiftTypeMetadataPairs, IndirectNames));
      }
}

TEST(ObjCSourceBindings,
     RegisteredSwiftGOTReferencesRejectUnprovenEdgesAndChangedIdentity) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation != 21; ++Mutation) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(Mutation);
      auto F =
          swiftRegisteredInternalTypeFixture(Architecture, 'V', true, true);
      const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_EQ(Bound.SwiftTypeMetadataPairs.size(), 1U);
      switch (Mutation) {
      case 0:
        F.Image.MachOResolvedChainedPointerSlots.erase(F.DescriptorSlot);
        break;
      case 1:
        F.Image.DataPtrRelocSlots.erase(F.DescriptorSlot);
        break;
      case 2:
        F.Image.DataPtrRelocTargetOwners[F.DescriptorSlot] = 0x6000;
        break;
      case 3:
        F.Image.Segments[2].ReadOnlyAfterRelocations = false;
        break;
      case 4:
        F.Image.ConflictingImportStorageSlots.insert(F.DescriptorSlot);
        break;
      case 5:
        F.Image.DyldBindSlots[F.DescriptorSlot] = {
            "_$s13WMFComponents10ArticleTabVMn", 0, "/tmp/WMFComponents",
            false};
        break;
      case 6:
        F.Image.ImportPtrSlots[F.DescriptorSlot] = "_other";
        break;
      case 7:
        F.Image.CodePtrRelocSlots.insert(F.DescriptorSlot);
        break;
      case 8:
        F.Image.RelDataPtrRelocSlots.insert(F.DescriptorSlot);
        break;
      case 9:
        F.Image.MachOResolvedChainedPointerSlots.insert(F.DescriptorSlot - 1);
        break;
      case 10:
        F.Image.DataPtrRelocSlots.insert(F.DescriptorSlot + 1);
        break;
      case 11:
        F.Image.Sections.push_back(F.Image.Sections[2]);
        break;
      case 12:
        F.Image.Segments.push_back(F.Image.Segments[2]);
        break;
      case 13:
        F.Image.Sections[2].FileSz = 0x27;
        break;
      case 14:
        F.Image.Segments[2].Data.resize(0x27);
        break;
      case 15:
        llvm::support::endian::write64le(F.Image.Segments[2].Data.data() + 0x20,
                                         F.LocalDescriptor + 4);
        break;
      case 16:
        F.Image.Segments[3].Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
        break;
      case 17:
        F.Image.Segments[3].Data[0xa0] = 'X';
        break;
      case 18:
        F.Image.Sections.back().Name = "__other_types";
        break;
      case 19:
        F.Image.MachOChainedFixupsAmbiguous = true;
        break;
      case 20:
        F.Image.Segments[3].Data[0x40] = 2;
        break;
      }
      EXPECT_FALSE(objc_binding_detail::swiftTypeMetadataPairProof(
          F.Image, F.Cache, F.Reference));
      EXPECT_TRUE(bindObjCSourceReferences(F.Function, F.Image)
                      .SwiftTypeMetadataPairs.empty());
      for (const auto &Arg : Bound.Function.Body[0].Val->Operands)
        EXPECT_FALSE(objcSourceCallBound(*Arg, F.Image, {}));
      std::set<std::string> Names;
      EXPECT_THROW(renderObjCSwiftTypeMetadataHelpers(
                       F.Image, Bound.SwiftTypeMetadataPairs, Names),
                   std::runtime_error);
    }
}

TEST(ObjCSourceBindings,
     ImportedLockGOTReferenceUsesTheExistingPrivateIdentityProof) {
  auto F = swiftImportedLockTypeMetadataFixture();
  const auto Direct = objc_binding_detail::swiftTypeMetadataPairProof(
      F.Image, F.Cache, F.Reference);
  ASSERT_TRUE(Direct);
  constexpr unsigned Offset = 5 + sizeof("ySDySSSo8NSBundleCG") - 1;
  auto *Type = F.Image.Segments[0].Data.data() + F.TypeReference - 0x1000;
  Type[Offset] = 2;
  llvm::support::endian::write32le(
      Type + Offset + 1,
      uint32_t(F.NestedDescriptorSlot - (F.TypeReference + Offset + 1)));
  llvm::support::endian::write64le(F.Image.Segments[2].Data.data() + 0x28,
                                   F.LocalDescriptor);
  F.Image.DataPtrRelocSlots.insert(F.NestedDescriptorSlot);
  F.Image.DataPtrRelocTargetOwners[F.NestedDescriptorSlot] = 0x4000;
  F.Image.MachOResolvedChainedPointerSlots.insert(F.NestedDescriptorSlot);
  const auto Indirect = objc_binding_detail::swiftTypeMetadataPairProof(
      F.Image, F.Cache, F.Reference);
  ASSERT_TRUE(Indirect);
  EXPECT_EQ(Direct->TypeReference, Indirect->TypeReference);
  EXPECT_EQ(Direct->Address, Indirect->Address);
  F.Image.Segments[3].Data[0x80] = 'X';
  EXPECT_FALSE(objc_binding_detail::swiftTypeMetadataPairProof(F.Image, F.Cache,
                                                               F.Reference));
}

TEST(ObjCSourceBindings,
     InternalSwiftIdentityRequiresExactImmutableRegistrationAndModule) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation != 21; ++Mutation) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(Mutation);
      auto F = swiftRegisteredInternalTypeFixture(Architecture);
      auto &Data = F.Image.Segments[3].Data;
      auto *Header = Data.data() + F.LocalDescriptor - 0x4000;
      switch (Mutation) {
      case 0:
        F.Image.Sections.back().Name = "__other_types";
        break;
      case 1:
        F.Image.Sections.back().Size = 3;
        break;
      case 2:
        F.Image.Sections.back().Size = 65537 * 4;
        break;
      case 3:
        F.Image.Sections.back().Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
        break;
      case 4:
        F.Image.Segments.back().Data[0] = 1;
        break;
      case 5:
        F.Image.Segments.back().Data.assign(0x100, 0);
        break;
      case 6:
        Data[0x80] = 'B';
        break;
      case 7:
        Data[0xa0] = 'B';
        break;
      case 8:
        Data[0x40] = 2;
        break; // Anonymous private context.
      case 9:
        llvm::support::endian::write32le(Header, 0x51);
        break;
      case 10:
        llvm::support::endian::write32le(Header, 0xd0);
        break;
      case 11:
        llvm::support::endian::write32le(Header, 0x150);
        break;
      case 12:
        llvm::support::endian::write32le(Header, 0x40050);
        break;
      case 13:
        Header[4] |= 1;
        break;
      case 14:
        F.Image.Symbols.push_back(F.Image.Symbols[2]);
        break;
      case 15:
        F.Image.Symbols.push_back({F.Image.Symbols[2].Name, 0x40c0, 0, false});
        break;
      case 16:
        F.Image.Exports.push_back({F.Image.Symbols[2].Name, 0, 0x40c0});
        break;
      case 17:
        F.Image.RelDataPtrRelocSlots.insert(F.LocalDescriptor + 8);
        break;
      case 18:
        F.Image.Sections.push_back(F.Image.Sections.back());
        break;
      case 19:
        F.Image.Sections[3].Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
        break;
      case 20: {
        // A second registered descriptor with the same runtime name cannot
        // be silently selected by its record order or by the symbol table.
        std::memcpy(Data.data() + 0xc0, Header, 12);
        llvm::support::endian::write32le(Data.data() + 0xc4,
                                         uint32_t(0x4040 - 0x40c4));
        llvm::support::endian::write32le(Data.data() + 0xc8,
                                         uint32_t(0x4080 - 0x40c8));
        F.Image.Sections.back().Size = F.Image.Sections.back().FileSz = 8;
        llvm::support::endian::write32le(
            F.Image.Segments.back().Data.data() + 4, uint32_t(0x40c0 - 0x6004));
        break;
      }
      }
      EXPECT_FALSE(objc_binding_detail::swiftLocalRegisteredNominalType(
          F.Image, F.LocalDescriptor));
      const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
      EXPECT_FALSE(Bound.Limitation.empty());
      EXPECT_TRUE(Bound.SwiftTypeMetadataPairs.empty());
    }
}

TEST(ObjCSourceBindings,
     OtherRegisteredTypeNamesDoNotBorrowAdjacentPointerStorage) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto F = swiftRegisteredInternalTypeFixture(Architecture);
    auto &Data = F.Image.Segments[3].Data;
    llvm::support::endian::write32le(Data.data() + 0xc0, 0x50);
    llvm::support::endian::write32le(Data.data() + 0xc4,
                                     uint32_t(0x4040 - 0x40c4));
    llvm::support::endian::write32le(Data.data() + 0xc8,
                                     uint32_t(0x40e0 - 0x40c8));
    std::memcpy(Data.data() + 0xe0, "Other", 6);
    F.Image.RelDataPtrRelocSlots.insert(0x40e8);
    F.Image.Sections.back().Size = F.Image.Sections.back().FileSz = 8;
    llvm::support::endian::write32le(F.Image.Segments.back().Data.data() + 4,
                                     uint32_t(0x40c0 - 0x6004));
    // Looking at sizeof("ArticleTab") bytes of "Other" would cross the
    // pointer. The runtime name differs before that storage is touched.
    ASSERT_FALSE(readImmutableImageBytes(F.Image, 0x40e0, 11));
    const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    ASSERT_EQ(Bound.SwiftTypeMetadataPairs.size(), 1U);
    F.Image.RelDataPtrRelocSlots.insert(0x4088);
    EXPECT_FALSE(objcSourceCallBound(*Bound.Function.Body[0].Val->Operands[0],
                                     F.Image, {}));
  }
}

TEST(ObjCSourceBindings, InternalSwiftTypeNamesAreRevalidatedAtPublication) {
  auto F = swiftRegisteredInternalTypeFixture(Arch::AArch64, 'C', true);
  const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_EQ(Bound.SwiftTypeMetadataPairs.size(), 1U);
  auto Cache = Bound.Function.Body[0].Val->Operands[0];
  F.Image.Segments[3].Data[0xa0] = 'B';
  EXPECT_FALSE(objcSourceCallBound(*Cache, F.Image, {}));
  std::set<std::string> Helpers;
  EXPECT_THROW(renderObjCSwiftTypeMetadataHelpers(
                   F.Image, Bound.SwiftTypeMetadataPairs, Helpers),
               std::runtime_error);
}

TEST(ObjCSourceBindings,
     SwiftTypeMetadataPairsAcceptExactPrintableTypeReferences) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (const llvm::StringRef TypeReference : {"yXlXp", "ypSg", "ScPSg"}) {
      SCOPED_TRACE(TypeReference.str());
      auto F = printableSwiftTypeMetadataFixture(Architecture, TypeReference);
      const auto Result = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
      ASSERT_EQ(Result.SwiftTypeMetadataPairs.size(), 1U);
      const auto Pair =
          Result.SwiftTypeMetadataPairs.at(SwiftTypeMetadataFixture::Cache);
      EXPECT_EQ(Pair.DescriptorSlot, 0U);
      EXPECT_TRUE(Pair.DescriptorSymbol.empty());
      EXPECT_EQ(Pair.Suffix, TypeReference);

      std::set<std::string> Helpers;
      const auto Source = renderObjCSwiftTypeMetadataHelpers(
          F.Image, Result.SwiftTypeMetadataPairs, Helpers);
      EXPECT_EQ(Source.find("__asm__("), std::string::npos);
      EXPECT_EQ(Source.find("const void *descriptor"), std::string::npos);
      for (size_t I = 0; I < TypeReference.size(); ++I)
        EXPECT_NE(Source.find(".type_reference[" + std::to_string(I) + "] = " +
                              std::to_string(uint8_t(TypeReference[I]))),
                  std::string::npos);
      EXPECT_NE(Source.find(".reference.length = " +
                            std::to_string(TypeReference.size())),
                std::string::npos);
    }
}

TEST(ObjCSourceBindings,
     SwiftTypeMetadataPairsRejectMalformedPrintableTypeReferences) {
  for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
    auto F = printableSwiftTypeMetadataFixture(
        Arch::AArch64, Mutation == 5 ? "!!!!!" : "ScPSg");
    auto &Data = F.Image.Segments[0].Data;
    auto *Type = Data.data() + SwiftTypeMetadataFixture::TypeReference - 0x1000;
    if (Mutation == 0)
      llvm::support::endian::write32le(
          Data.data() + SwiftTypeMetadataFixture::Reference + 4 - 0x1000, 0);
    if (Mutation == 1)
      Type[1] = 1;
    if (Mutation == 2)
      F.Image.Symbols[0].Name = "_$sScPSg_GMR";
    if (Mutation == 3)
      Type[5] = '!';
    if (Mutation == 4)
      F.Image.DataPtrRelocSlots.insert(SwiftTypeMetadataFixture::TypeReference +
                                       1);
    if (Mutation == 6) {
      llvm::support::endian::write32le(
          Data.data() + SwiftTypeMetadataFixture::Reference + 4 - 0x1000, 3);
      Type[0] = 1;
      Type[3] = 0;
    }
    const auto Result = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_FALSE(Result.Limitation.empty()) << Mutation;
    EXPECT_TRUE(Result.SwiftTypeMetadataPairs.empty()) << Mutation;
  }
}

TEST(ObjCSourceBindings,
     SwiftTypeMetadataPairsAcceptExactNestedIndirectDescriptors) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (const bool LocalNestedDescriptor : {false, true}) {
      SCOPED_TRACE(LocalNestedDescriptor);
      SwiftTypeMetadataFixture F(Architecture, false, false, false, false,
                                 LocalNestedDescriptor, !LocalNestedDescriptor);
      const auto Result = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
      ASSERT_EQ(Result.SwiftTypeMetadataPairs.size(), 1U);
      const auto Pair =
          Result.SwiftTypeMetadataPairs.at(SwiftTypeMetadataFixture::Cache);
      EXPECT_EQ(Pair.DescriptorSlot, SwiftTypeMetadataFixture::DescriptorSlot);
      EXPECT_EQ(Pair.DescriptorSymbol, "_$s7Combine9PublishedVMn");
      ASSERT_EQ(Pair.Suffix.size(), LocalNestedDescriptor ? 9U : 7U);
      EXPECT_EQ(static_cast<unsigned char>(Pair.Suffix[1]), 2U);

      std::set<std::string> Helpers;
      const auto Source = renderObjCSwiftTypeMetadataHelpers(
          F.Image, Result.SwiftTypeMetadataPairs, Helpers);
      EXPECT_NE(Source.find("__asm__(\"_$s7Combine9PublishedVMn\")"),
                std::string::npos);
      const llvm::StringRef NestedDescriptor =
          LocalNestedDescriptor ? "_$"
                                  "s7WMFData33WMFDonationReminderDataController"
                                  "C20ExperimentAssignmentOMn"
                                : "_$s10Foundation4DateVMn";
      EXPECT_NE(Source.find("__asm__(\"" + NestedDescriptor.str() + "\")"),
                std::string::npos);
      EXPECT_NE(Source.find("const void *descriptor_1"), std::string::npos);
      EXPECT_NE(Source.find(".type_reference[6] = 2"), std::string::npos);
      EXPECT_NE(Source.find("descriptor_delta_1"), std::string::npos);
      EXPECT_NE(Source.find(".reference.length = " +
                            std::to_string(LocalNestedDescriptor ? 14 : 12)),
                std::string::npos);
    }
}

TEST(ObjCSourceBindings,
     SwiftTypeMetadataPairsRebuildDirectLocalDescriptorIndirectly) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    SwiftTypeMetadataFixture F(Architecture, false, false, false, false, true);
    auto *Type = F.Image.Segments[0].Data.data() +
                 SwiftTypeMetadataFixture::TypeReference - 0x1000;
    Type[6] = 1;
    llvm::support::endian::write32le(
        Type + 7, static_cast<uint32_t>(static_cast<int32_t>(
                      SwiftTypeMetadataFixture::LocalDescriptor -
                      (SwiftTypeMetadataFixture::TypeReference + 7))));
    const auto Result = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
    ASSERT_EQ(Result.SwiftTypeMetadataPairs.size(), 1U);
    std::set<std::string> Helpers;
    const auto Source = renderObjCSwiftTypeMetadataHelpers(
        F.Image, Result.SwiftTypeMetadataPairs, Helpers);
    EXPECT_NE(Source.find("_$s7WMFData33WMFDonationReminderDataControllerC"
                          "20ExperimentAssignmentOMn"),
              std::string::npos);
    EXPECT_NE(Source.find(".type_reference[6] = 2"), std::string::npos);

    F.Image.Exports.clear();
    EXPECT_THROW(renderObjCSwiftTypeMetadataHelpers(
                     F.Image, Result.SwiftTypeMetadataPairs, Helpers),
                 std::runtime_error);
  }
}

TEST(ObjCSourceBindings,
     SwiftTypeMetadataPairsRejectUnprovenNestedIndirectDescriptors) {
  for (unsigned Mutation = 0; Mutation < 9; ++Mutation) {
    const bool SystemDescriptor = Mutation >= 5;
    SwiftTypeMetadataFixture F(Arch::AArch64, false, false, false, false,
                               !SystemDescriptor, SystemDescriptor);
    auto *Type = F.Image.Segments[0].Data.data() +
                 SwiftTypeMetadataFixture::TypeReference - 0x1000;
    if (Mutation == 0)
      Type[6] = 3;
    if (Mutation == 1)
      Type[7] = 0;
    if (Mutation == 2)
      F.Image.Exports.clear();
    if (Mutation == 3)
      F.Image.DataPtrRelocSlots.erase(
          SwiftTypeMetadataFixture::NestedDescriptorSlot);
    if (Mutation == 4)
      F.Image.DataPtrRelocTargetOwners
          [SwiftTypeMetadataFixture::NestedDescriptorSlot] = 0x1000;
    if (Mutation == 5)
      F.Image.DyldBindSlots[SwiftTypeMetadataFixture::NestedDescriptorSlot]
          .WeakImport = true;
    if (Mutation == 6)
      F.Image.DyldBindSlots[SwiftTypeMetadataFixture::NestedDescriptorSlot]
          .Module = "/System/Library/Frameworks/Combine.framework/Combine";
    if (Mutation == 7)
      F.Image.DyldBindSlots[SwiftTypeMetadataFixture::NestedDescriptorSlot]
          .Name = "_$s10Foundation3URLVMn";
    if (Mutation == 8) {
      constexpr uint32_t Length = 9 * 5;
      for (uint32_t I = 0; I < 9; ++I) {
        Type[I * 5] = 2;
        llvm::support::endian::write32le(
            Type + I * 5 + 1,
            static_cast<uint32_t>(static_cast<int32_t>(
                SwiftTypeMetadataFixture::DescriptorSlot -
                (SwiftTypeMetadataFixture::TypeReference + I * 5 + 1))));
      }
      Type[Length] = 0;
      llvm::support::endian::write32le(F.Image.Segments[0].Data.data() +
                                           SwiftTypeMetadataFixture::Reference +
                                           4 - 0x1000,
                                       Length);
    }
    const auto Result = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_FALSE(Result.Limitation.empty()) << Mutation;
    EXPECT_TRUE(Result.SwiftTypeMetadataPairs.empty()) << Mutation;
  }
}

TEST(ObjCSourceBindings,
     SwiftProtocolTypeMetadataPairsRejectIncompleteOrStaleLocalRebases) {
  for (unsigned Mutation = 0; Mutation < 13; ++Mutation) {
    SCOPED_TRACE(Mutation);
    SwiftTypeMetadataFixture F(Arch::AArch64, true);
    if (Mutation == 0)
      F.Image.MachOResolvedChainedPointerSlots.clear();
    if (Mutation == 1)
      F.Image.DataPtrRelocSlots.clear();
    if (Mutation == 2)
      F.Image.DataPtrRelocTargetOwners.clear();
    if (Mutation == 3)
      F.Image
          .DataPtrRelocTargetOwners[SwiftTypeMetadataFixture::DescriptorSlot] =
          0x4010;
    if (Mutation == 4)
      F.Image.Segments[2].ReadOnlyAfterRelocations = false;
    if (Mutation == 5) {
      F.Image.Segments[3].Flags =
          SegmentFlags::Readable | SegmentFlags::Executable;
      F.Image.Sections[3].Flags =
          SegmentFlags::Readable | SegmentFlags::Executable;
      F.Image.Sections[3].Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    }
    if (Mutation == 6)
      F.Image.Exports.clear();
    if (Mutation == 7)
      F.Image.Exports[0].Name = "_$s7WMFData10OtherThingMp";
    if (Mutation == 8)
      llvm::support::endian::write64le(
          F.Image.Segments[2].Data.data() +
              SwiftTypeMetadataFixture::DescriptorSlot - 0x3000,
          0x4080);
    if (Mutation == 9) {
      F.Image.Symbols[2].Name = "_$s7WMFData10WMFServiceMn";
      F.Image.Exports[0].Name = F.Image.Symbols[2].Name;
    }
    if (Mutation == 10)
      F.Image.Symbols.push_back(F.Image.Symbols[2]);
    if (Mutation == 11)
      F.Image.ConflictingImportStorageSlots.insert(
          SwiftTypeMetadataFixture::DescriptorSlot);
    if (Mutation == 12) {
      F.Image.Segments[3].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
      F.Image.Sections[3].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
    }
    const auto Result = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_FALSE(Result.Limitation.empty());
    EXPECT_TRUE(Result.SwiftTypeMetadataPairs.empty());
  }

  SwiftTypeMetadataFixture F(Arch::AArch64, true);
  auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_EQ(Result.SwiftTypeMetadataPairs.size(), 1U);
  auto Cache = Result.Function.Body[0].Val->Operands[0];
  F.Image.Exports.clear();
  EXPECT_FALSE(objcSourceCallBound(*Cache, F.Image, {}));
  std::set<std::string> Helpers;
  EXPECT_THROW(renderObjCSwiftTypeMetadataHelpers(
                   F.Image, Result.SwiftTypeMetadataPairs, Helpers),
               std::runtime_error);

  SwiftTypeMetadataFixture NonPointer(Arch::AArch64, true, true);
  auto Changed = std::make_shared<SourceCallTypeHint>(
      *NonPointer.Function.Body[0].Val->SourceCallHint);
  Changed->Signature.Parameters[0].Type = NdType::makeInt(8, false);
  NonPointer.Function.Body[0].Val->SourceCallHint = std::move(Changed);
  Result = bindObjCSourceReferences(NonPointer.Function, NonPointer.Image);
  EXPECT_FALSE(Result.Limitation.empty());
  EXPECT_TRUE(Result.SwiftTypeMetadataPairs.empty());
}

TEST(ObjCSourceBindings,
     SwiftConcreteTypeMetadataPairsRejectIncompleteOrStaleEvidence) {
  for (unsigned Mutation = 0; Mutation < 14; ++Mutation) {
    SCOPED_TRACE(Mutation);
    SwiftTypeMetadataFixture F(Arch::AArch64);
    auto &ReferenceBytes = F.Image.Segments[0].Data;
    auto &CacheBytes = F.Image.Segments[1].Data;
    if (Mutation == 0)
      CacheBytes[SwiftTypeMetadataFixture::Cache - 0x2000] = 1;
    if (Mutation == 1)
      F.Image.Symbols.pop_back();
    if (Mutation == 2)
      F.Image.Sections[0].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
    if (Mutation == 3)
      llvm::support::endian::write32le(ReferenceBytes.data() +
                                           SwiftTypeMetadataFixture::Reference +
                                           4 - 0x1000,
                                       8);
    if (Mutation == 4)
      ReferenceBytes[SwiftTypeMetadataFixture::TypeReference - 0x1000] = 1;
    if (Mutation == 5)
      llvm::support::endian::write32le(
          ReferenceBytes.data() + SwiftTypeMetadataFixture::TypeReference + 1 -
              0x1000,
          1);
    if (Mutation == 6)
      F.Image.DyldBindSlots[SwiftTypeMetadataFixture::DescriptorSlot].Module =
          "/tmp/Foundation";
    if (Mutation == 7)
      F.Image.DyldBindSlots[SwiftTypeMetadataFixture::DescriptorSlot]
          .WeakImport = true;
    if (Mutation == 8) {
      F.Image.DyldBindSlots[SwiftTypeMetadataFixture::DescriptorSlot].Name =
          "_$s10Foundation3URLVMa";
      F.Image.ImportStorageSlots[SwiftTypeMetadataFixture::DescriptorSlot]
          .Name = "_$s10Foundation3URLVMa";
    }
    if (Mutation == 9)
      F.Image.Symbols[0].Name = "_$s10Foundation3URLVSdMR";
    if (Mutation == 10)
      F.Image.Symbols.push_back(F.Image.Symbols.back());
    if (Mutation == 11)
      F.Image.DataPtrRelocSlots.insert(SwiftTypeMetadataFixture::Reference);
    if (Mutation == 12)
      F.Image.MachOTwoLevelNamespace = false;
    if (Mutation == 13)
      F.Image.Sections[0].Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    const auto Result = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_FALSE(Result.Limitation.empty());
    EXPECT_TRUE(Result.SwiftTypeMetadataPairs.empty());
  }

  SwiftTypeMetadataFixture F(Arch::AArch64);
  auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_EQ(Result.SwiftTypeMetadataPairs.size(), 1U);
  auto Cache = Result.Function.Body[0].Val->Operands[0];
  F.Image.Segments[1].Data[SwiftTypeMetadataFixture::Cache - 0x2000] = 1;
  EXPECT_FALSE(objcSourceCallBound(*Cache, F.Image, {}));
  std::set<std::string> Helpers;
  EXPECT_THROW(renderObjCSwiftTypeMetadataHelpers(
                   F.Image, Result.SwiftTypeMetadataPairs, Helpers),
               std::runtime_error);
}

namespace {
Fixture readonlyTableFixture(uint16_t Width = 8) {
  Fixture F;
  F.Image.ObjCSourceReferences.clear();
  F.Function.ReturnType = NdType::makeInt(Width, false);
  MedVar V;
  V.Kind = MedVar::Param;
  V.Size = 4;
  auto Index = HighExpr::makeVar(V, NdType::makeInt(4, false));
  auto Guard =
      HighExpr::makeBinop(NdOp::INT_LESS, Index, HighExpr::makeConst(3, 4));
  Guard->Type = NdType::makeInt(4, false);
  auto Wide = HighExpr::makeUnary(NdOp::INT_ZEXT, Index);
  Wide->Type = NdType::makeInt(8, false);
  auto Offset =
      HighExpr::makeBinop(NdOp::INT_MULT, Wide, HighExpr::makeConst(Width, 8));
  auto Address = HighExpr::makeBinop(NdOp::INT_ADD,
                                     HighExpr::makeConst(0x1040, 8), Offset);
  HighStmt Load;
  Load.Kind = StmtKind::Return;
  Load.RetVal = HighExpr::makeLoad(Address, NdType::makeInt(Width, false));
  HighStmt Other;
  Other.Kind = StmtKind::Return;
  Other.RetVal = HighExpr::makeConst(0, Width);
  HighStmt Branch;
  Branch.Kind = StmtKind::IfElse;
  Branch.Cond = Guard;
  Branch.Body = {Load};
  Branch.ElseBody = {Other};
  F.Function.Body = {Branch};
  const uint64_t Values[] = {7, 99, 253};
  for (unsigned I = 0; I < 3; ++I)
    for (unsigned B = 0; B < Width; ++B)
      F.Image.Segments[0].Data[0x40 + I * Width + B] = Values[I] >> (B * 8);
  return F;
}
} // namespace

TEST(ObjCSourceBindings, ReadOnlyTablesBindEveryBoundedScalarLoadOccurrence) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (uint16_t Width : {1, 2, 4, 8}) {
      auto F = readonlyTableFixture(Width);
      F.Image.Arch = Architecture;
      const auto Original = F.Function.Body[0].Body[0].RetVal;
      const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      ASSERT_EQ(Bound.BorrowedBytes.size(), 1U);
      EXPECT_EQ(Bound.BorrowedBytes.begin()->first, 0x1040U);
      EXPECT_EQ(Bound.BorrowedBytes.begin()->second, Width * 3U);
      const auto &Load = Bound.Function.Body[0].Body[0].RetVal;
      EXPECT_EQ(Load->Kind, ExprKind::Load);
      EXPECT_EQ(Load->Type->Size, Width);
      const auto &Helper = Load->Operands[0]->Operands[0];
      ASSERT_TRUE(Helper->SourceCallHint);
      EXPECT_EQ(Helper->SourceCallHint->CallKind,
                SourceCallTypeHint::Kind::RuntimeReadOnlyBytes);
      const auto Allowed = readOnlyScalarSourceHelpers(Bound.Function, F.Image);
      ASSERT_EQ(Allowed.size(), 1U);
      EXPECT_TRUE(objcSourceCallBound(*Helper, F.Image, {}, nullptr, &Allowed));
      EXPECT_FALSE(objcSourceCallBound(*Helper, F.Image, {}));
      EXPECT_EQ(Original->Operands[0]->Operands[0]->Kind, ExprKind::Const);
    }
}

namespace {
Fixture countdownByteTableFixture() {
  Fixture F;
  F.Image.ObjCSourceReferences.clear();
  F.Image.Segments[0].Size = F.Image.Segments[0].FileSz = 0x400;
  F.Image.Segments[0].Data.resize(0x400);
  F.Image.Sections[0].Size = F.Image.Sections[0].FileSz = 0x400;
  for (unsigned I = 0; I < 720; ++I)
    F.Image.Segments[0].Data[0x40 + I] = uint8_t(I % 251);
  F.Function.ReturnType = NdType::makeVoid();
  auto Var = [](int Id, uint16_t Width = 8) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.Id = Id;
    V.Size = Width;
    return HighExpr::makeVar(V, NdType::makeInt(Width, false));
  };
  auto Assign = [](ExprPtr Dst, ExprPtr Value) {
    HighStmt S;
    S.Kind = StmtKind::Assign;
    S.Dst = std::move(Dst);
    S.Val = std::move(Value);
    return S;
  };
  const auto Count = Var(1), Pointer = Var(2);
  const auto Decrement = Var(3), Predicate = Var(4, 1), Increment = Var(5);
  HighStmt Loop;
  Loop.Kind = StmtKind::While;
  Loop.Cond = HighExpr::makeConst(1, 1);
  for (unsigned Bias : {2U, 1U, 0U}) {
    auto Address = Bias ? HighExpr::makeBinop(NdOp::INT_SUB, Pointer,
                                              HighExpr::makeConst(Bias, 8))
                        : Pointer;
    Loop.Body.push_back(
        Assign(Var(10 + Bias, 1),
               HighExpr::makeLoad(Address, NdType::makeInt(1, false))));
  }
  Loop.Body.push_back(
      Assign(Decrement, HighExpr::makeBinop(NdOp::INT_SUB, Count,
                                            HighExpr::makeConst(1, 8))));
  auto Test =
      HighExpr::makeBinop(NdOp::INT_NOTEQUAL, Count, HighExpr::makeConst(1, 8));
  Test->Type = NdType::makeInt(1, false);
  Loop.Body.push_back(Assign(Predicate, Test));
  HighStmt Break;
  Break.Kind = StmtKind::Break;
  HighStmt Exit;
  Exit.Kind = StmtKind::If;
  Exit.Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, Predicate);
  Exit.Cond->Type = NdType::makeInt(1, false);
  Exit.Body = {Break};
  Loop.Body.push_back(Exit);
  Loop.Body.push_back(
      Assign(Increment, HighExpr::makeBinop(NdOp::INT_ADD, Pointer,
                                            HighExpr::makeConst(3, 8))));
  Loop.Body.push_back(Assign(Count, Decrement));
  Loop.Body.push_back(Assign(Pointer, Increment));
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  F.Function.Body = {Assign(Count, HighExpr::makeConst(240, 8)),
                     Assign(Pointer, HighExpr::makeConst(0x1042, 8)), Loop,
                     Return};
  return F;
}
} // namespace

TEST(ObjCSourceBindings, CountdownByteTableRebasesOnlyBoundedPrivatePointer) {
  auto F = countdownByteTableFixture();
  const auto Original = F.Function.Body[1].Val;
  auto Bound = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  ASSERT_EQ(Bound.BorrowedBytes.size(), 1U);
  EXPECT_EQ(*Bound.BorrowedBytes.begin(), (BorrowedByteRange{0x1040, 720}));
  EXPECT_EQ(Original->ConstVal, 0x1042U);
  EXPECT_EQ(Bound.Function.Body[1].Val->ConstVal, 2U);
  const auto Allowed = readOnlyScalarSourceHelpers(Bound.Function, F.Image);
  ASSERT_EQ(Allowed.size(), 3U);
  for (unsigned I = 0; I < 3; ++I) {
    const auto &Load = Bound.Function.Body[2].Body[I].Val;
    const auto &Helper = Load->Operands[0]->Operands[0];
    EXPECT_TRUE(Allowed.count(Helper.get()));
    EXPECT_TRUE(objcSourceCallBound(*Helper, F.Image, {}, nullptr, &Allowed));
  }
  Bound.Function.Body[1].Val->ConstVal = 3;
  EXPECT_TRUE(readOnlyScalarSourceHelpers(Bound.Function, F.Image).empty());
}

TEST(ObjCSourceBindings, CountdownByteTableMayTestBeforeDecrementing) {
  auto F = countdownByteTableFixture();
  auto &Steps = F.Function.Body[2].Body;
  // The last iteration exits before either induction local advances.
  std::rotate(Steps.begin() + 3, Steps.begin() + 4, Steps.begin() + 6);
  auto Bound = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  ASSERT_EQ(Bound.BorrowedBytes.size(), 1U);
  EXPECT_EQ(*Bound.BorrowedBytes.begin(), (BorrowedByteRange{0x1040, 720}));
  EXPECT_EQ(readOnlyScalarSourceHelpers(Bound.Function, F.Image).size(), 3U);

  // The publication proof must reject a modified exit condition, even after
  // the helper was already installed by the binder.
  auto &Exit = Bound.Function.Body[2].Body[4];
  Exit.Cond->Op = NdOp::INT_NEGATE;
  EXPECT_TRUE(readOnlyScalarSourceHelpers(Bound.Function, F.Image).empty());
}

TEST(ObjCSourceBindings, CountdownByteTableAcceptsDirectFinalIterationExit) {
  auto F = countdownByteTableFixture();
  auto &Steps = F.Function.Body[2].Body;
  const auto Count = F.Function.Body[0].Dst;
  Steps.erase(Steps.begin() + 4); // no predicate temporary
  std::swap(Steps[3], Steps[4]);  // break before decrement
  auto Exit =
      HighExpr::makeBinop(NdOp::INT_EQUAL, Count, HighExpr::makeConst(1, 8));
  Exit->Type = NdType::makeInt(1, false);
  Steps[3].Cond = Exit;
  auto Bound = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  ASSERT_EQ(Bound.BorrowedBytes.size(), 1U);
  EXPECT_EQ(*Bound.BorrowedBytes.begin(), (BorrowedByteRange{0x1040, 720}));
  EXPECT_EQ(Bound.Function.Body[1].Val->ConstVal, 2U);
  EXPECT_EQ(readOnlyScalarSourceHelpers(Bound.Function, F.Image).size(), 3U);

  Bound.Function.Body[2].Body[3].Cond->Operands[1]->ConstVal = 2;
  EXPECT_TRUE(readOnlyScalarSourceHelpers(Bound.Function, F.Image).empty());
}

TEST(ObjCSourceBindings, CountdownByteTablePublicationRechecksLoopAndHelper) {
  for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = countdownByteTableFixture();
    auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Bound.Limitation.empty());
    auto &Loop = Bound.Function.Body[2];
    ASSERT_EQ(readOnlyScalarSourceHelpers(Bound.Function, F.Image).size(), 3U);
    if (Mutation == 0)
      Bound.Function.Body[0].Val->ConstVal = 241;
    if (Mutation == 1)
      Loop.Body[6].Val->Operands[1]->ConstVal = 4;
    if (Mutation == 2)
      Loop.Body.erase(Loop.Body.begin() + 1);
    if (Mutation == 3) {
      auto &Helper = Loop.Body[0].Val->Operands[0]->Operands[0];
      auto Changed =
          std::make_shared<SourceCallTypeHint>(*Helper->SourceCallHint);
      Changed->ByteCount = 719;
      Helper->SourceCallHint = std::move(Changed);
    }
    EXPECT_TRUE(readOnlyScalarSourceHelpers(Bound.Function, F.Image).empty());
  }
}

TEST(ObjCSourceBindings, CountdownByteTableRejectsUnboundedOrEscapingWalks) {
  for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = countdownByteTableFixture();
    auto &Loop = F.Function.Body[2];
    if (Mutation == 0)
      F.Function.Body[0].Val = HighExpr::makeConst(0, 8);
    if (Mutation == 1)
      Loop.Body[6].Val->Operands[1] = HighExpr::makeConst(4, 8);
    if (Mutation == 2)
      std::swap(Loop.Body[5], Loop.Body[6]);
    if (Mutation == 3) {
      MedVar V;
      V.Kind = MedVar::Temp;
      V.Id = 20;
      V.Size = 8;
      HighStmt Escape;
      Escape.Kind = StmtKind::Assign;
      Escape.Dst = HighExpr::makeVar(V, NdType::makeInt(8, false));
      Escape.Val = Loop.Body[2].Val->Operands[0];
      Loop.Body.insert(Loop.Body.begin(), Escape);
    }
    if (Mutation == 4) {
      HighStmt Overwrite;
      Overwrite.Kind = StmtKind::Assign;
      Overwrite.Dst = Loop.Body[2].Val->Operands[0];
      Overwrite.Val = HighExpr::makeConst(0x1042, 8);
      F.Function.Body.insert(F.Function.Body.begin(), Overwrite);
    }
    if (Mutation == 5)
      F.Image.Sections[0].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
    if (Mutation == 6)
      F.Image.Sections[0].FileSz = 0x100;
    if (Mutation == 7)
      F.Function.Body[0].Val->Type = NdType::makeInt(1, true);
    const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_FALSE(Bound.Limitation.empty());
    EXPECT_TRUE(Bound.BorrowedBytes.empty());
  }
}

TEST(ObjCSourceBindings, ReadOnlyTablesNormalizeBoundedFixedAddressBias) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Subtract : {false, true}) {
      auto F = readonlyTableFixture(1);
      F.Image.Arch = Architecture;
      auto &Load = F.Function.Body[0].Body[0].RetVal;
      auto &Address = Load->Operands[0];
      auto Indexed = Address->Operands[1];
      Address->Operands[0] = HighExpr::makeConst(Subtract ? 0x1042 : 0x103e, 8);
      Address->Operands[1] =
          HighExpr::makeBinop(Subtract ? NdOp::INT_SUB : NdOp::INT_ADD, Indexed,
                              HighExpr::makeConst(2, 8));

      auto Bound = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      ASSERT_EQ(Bound.BorrowedBytes.size(), 1U);
      EXPECT_EQ(*Bound.BorrowedBytes.begin(), (BorrowedByteRange{0x1040, 3}));
      auto Result = Bound.Function.Body[0].Body[0].RetVal;
      ASSERT_EQ(Result->Kind, ExprKind::Load);
      ASSERT_EQ(Result->Operands[0]->Operands.size(), 2U);
      const auto &Helper = Result->Operands[0]->Operands[0];
      ASSERT_TRUE(Helper->SourceCallHint);
      EXPECT_EQ(Helper->SourceCallHint->TargetAddress, 0x1040U);
      EXPECT_EQ(Result->Operands[0]->Operands[1]->Op, NdOp::INT_MULT);
      const auto Allowed = readOnlyScalarSourceHelpers(Bound.Function, F.Image);
      ASSERT_EQ(Allowed.size(), 1U);
      EXPECT_TRUE(objcSourceCallBound(*Helper, F.Image, {}, nullptr, &Allowed));
      Result->Operands[0]->Operands[1] =
          HighExpr::makeBinop(NdOp::INT_ADD, Result->Operands[0]->Operands[1],
                              HighExpr::makeConst(2, 8));
      EXPECT_TRUE(readOnlyScalarSourceHelpers(Bound.Function, F.Image).empty());

      auto Invalid = readonlyTableFixture(1);
      Invalid.Image.Arch = Architecture;
      auto &InvalidAddress =
          Invalid.Function.Body[0].Body[0].RetVal->Operands[0];
      InvalidAddress->Operands[1] = HighExpr::makeBinop(
          Subtract ? NdOp::INT_SUB : NdOp::INT_ADD, InvalidAddress->Operands[1],
          HighExpr::makeConst(65537, 8));
      const auto Rejected =
          bindObjCSourceReferences(Invalid.Function, Invalid.Image);
      EXPECT_FALSE(Rejected.Limitation.empty());
      EXPECT_TRUE(Rejected.BorrowedBytes.empty());
    }
}

TEST(ObjCSourceBindings, ReadOnlyTablesRejectUnprovenRangesStorageAndUses) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 14; ++Mutation) {
      auto F = readonlyTableFixture();
      F.Image.Arch = Architecture;
      auto &Branch = F.Function.Body[0];
      auto &Load = Branch.Body[0].RetVal;
      auto &Offset = Load->Operands[0]->Operands[1];
      if (Mutation == 0)
        Branch.Cond = HighExpr::makeConst(1, 1);
      if (Mutation == 1)
        Branch.Cond->Op = NdOp::INT_SLESS;
      if (Mutation == 2)
        F.Image.Sections[0].Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
      if (Mutation == 3)
        F.Image.Sections.push_back(F.Image.Sections[0]);
      if (Mutation == 4)
        F.Image.Sections[0].FileSz = 0x47;
      if (Mutation == 5)
        F.Image.DataPtrRelocSlots.insert(0x1048);
      if (Mutation == 6)
        Load->MemoryOrdering = NdMemoryOrdering::Acquire;
      if (Mutation == 7)
        Load->MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      if (Mutation == 8)
        Offset->Operands[1] = HighExpr::makeConst(65537, 8);
      if (Mutation == 9)
        llvm::support::endian::write64le(F.Image.Segments[0].Data.data() + 0x48,
                                         0x1080);
      if (Mutation == 10)
        F.Function.UnstructuredExceptionRegions = 1;
      if (Mutation == 11)
        F.Function.ReturnType = NdType::makePtr(NdType::makeVoid());
      if (Mutation == 12)
        F.Function.Body.push_back(Branch.Body[0]);
      if (Mutation == 13) {
        HighStmt Overwrite;
        Overwrite.Kind = StmtKind::Assign;
        Overwrite.Dst = Offset->Operands[0]->Operands[0];
        Overwrite.Val = HighExpr::makeConst(99, 4);
        HighStmt Loop;
        Loop.Kind = StmtKind::DoWhile;
        Loop.Cond = Load;
        Loop.Body = {Overwrite};
        Branch.Body = {Loop};
      }
      const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
      EXPECT_FALSE(Bound.Limitation.empty()) << Mutation;
      EXPECT_TRUE(Bound.BorrowedBytes.empty()) << Mutation;
    }
}

TEST(ObjCSourceBindings, ReadOnlyTablePublicationRechecksFlowBytesAndEscapes) {
  for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
    auto F = readonlyTableFixture();
    auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Bound.Limitation.empty());
    auto &Branch = Bound.Function.Body[0];
    const auto &Load = Branch.Body[0].RetVal;
    const auto Helper = Load->Operands[0]->Operands[0];
    ASSERT_EQ(readOnlyScalarSourceHelpers(Bound.Function, F.Image).size(), 1U);
    if (Mutation == 0)
      Branch.Cond = HighExpr::makeConst(1, 1);
    if (Mutation == 1) {
      HighStmt Escape;
      Escape.Kind = StmtKind::Return;
      Escape.RetVal = Helper;
      Bound.Function.Body.push_back(Escape);
    }
    if (Mutation == 2)
      F.Image.Segments[0].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
    if (Mutation == 3)
      F.Image.DataPtrRelocSlots.insert(0x1048);
    if (Mutation == 4 || Mutation == 5) {
      auto Changed =
          std::make_shared<SourceCallTypeHint>(*Helper->SourceCallHint);
      Changed->ByteCount = Mutation == 4 ? 8 : 65537;
      Helper->SourceCallHint = std::move(Changed);
    }
    if (Mutation == 6)
      Load->Operands[0]->Operands[1]->Operands[1] =
          HighExpr::makeConst(4096, 8);
    const auto Allowed = readOnlyScalarSourceHelpers(Bound.Function, F.Image);
    EXPECT_TRUE(Allowed.empty()) << Mutation;
    EXPECT_FALSE(objcSourceCallBound(*Helper, F.Image, {}, nullptr, &Allowed))
        << Mutation;
  }
}

TEST(ObjCSourceBindings,
     ScalarPointerAmbiguityRequiresWidthAndSectionOwnership) {
  for (uint16_t Width : {1, 2, 4, 8}) {
    auto F = readonlyTableFixture(Width);
    EXPECT_EQ(isImagePointerBitPattern(F.Image, 0x1040, Width), Width == 8);
    EXPECT_FALSE(isImagePointerBitPattern(F.Image, 0, Width));
    F.Image.Sections[0].VA += 0x20;
    F.Image.Sections[0].FileOff += 0x20;
    F.Image.Sections[0].Size -= 0x20;
    F.Image.Sections[0].FileSz -= 0x20;
    EXPECT_FALSE(isImagePointerBitPattern(F.Image, 0x1010, Width));
    EXPECT_TRUE(F.Image.getSegmentFor(0x1010));
    EXPECT_FALSE(F.Image.getSectionFor(0x1010));
  }
  for (uint16_t Width : {2, 4}) {
    auto F = readonlyTableFixture(Width);
    llvm::support::endian::write64le(F.Image.Segments[0].Data.data() + 0x40,
                                     0x1040);
    F.Function.Body = {F.Function.Body[0].Body[0]};
    F.Function.Body[0].RetVal = HighExpr::makeLoad(
        HighExpr::makeConst(0x1040, 8), NdType::makeInt(Width, false));
    const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Bound.Limitation.empty());
    const auto &Value = Bound.Function.Body[0].RetVal;
    ASSERT_EQ(Value->Kind, ExprKind::BitCast);
    EXPECT_EQ(Value->Operands[0]->ConstVal, 0x1040U);
  }
}

TEST(ObjCSourceBindings,
     CStringPoolsPreserveInteriorOffsetsAndRevalidateStorage) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    Fixture F;
    F.Image.Arch = Architecture;
    F.Image.ObjCSourceReferences.clear();
    F.Image.Sections[0].Type = llvm::MachO::S_CSTRING_LITERALS;
    F.Function.ReturnType = NdType::makePtr(NdType::makeInt(1));
    const std::array<uint8_t, 9> Bytes{'a', 0,   'b',  '?', '?',
                                       '/', '"', '\\', 0xff};
    std::copy(Bytes.begin(), Bytes.end(), F.Image.Segments[0].Data.begin());
    for (va_t Address : {0x1000, 0x1001, 0x1002, 0x10ff}) {
      F.Function.Body[0].RetVal = HighExpr::makeConst(Address, 8);
      const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      EXPECT_EQ(Bound.CStringSections, std::set<va_t>{0x1000});
      auto Helper = Bound.Function.Body[0].RetVal;
      if (Address != 0x1000) {
        ASSERT_EQ(Helper->Kind, ExprKind::BinOp);
        EXPECT_EQ(Helper->Operands[1]->ConstVal, Address - 0x1000);
        Helper = Helper->Operands[0];
      }
      ASSERT_TRUE(Helper->SourceCallHint);
      EXPECT_EQ(Helper->SourceCallHint->CallKind,
                SourceCallTypeHint::Kind::RuntimeCStringStorage);
      EXPECT_TRUE(objcSourceCallBound(*Helper, F.Image, {}));
      auto Forged =
          std::make_shared<SourceCallTypeHint>(*Helper->SourceCallHint);
      Forged->ByteCount--;
      Helper->SourceCallHint = std::move(Forged);
      EXPECT_FALSE(objcSourceCallBound(*Helper, F.Image, {}));
    }
    std::set<std::string> Helpers;
    const auto Source = renderCStringStorageHelpers(F.Image, {0x1000}, Helpers);
    EXPECT_NE(Source.find("a\\000b\\077\\077/\\042\\134\\377"),
              std::string::npos);
    EXPECT_NE(Source.find("storage[256]"), std::string::npos);
    EXPECT_FALSE(objc_binding_detail::associationKeyHint(F.Image, 0x1002));
    F.Image.DataPtrRelocSlots.insert(0x10f8);
    EXPECT_FALSE(cstringStorageSourceHint(F.Image, 0x1000));
    EXPECT_THROW(renderCStringStorageHelpers(F.Image, {0x1000}, Helpers),
                 std::runtime_error);
  }
}

TEST(ObjCSourceBindings, CStringPointerSlotsSharePoolsAndRevalidateFixups) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Chained : {false, true})
      for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
        SCOPED_TRACE(Mutation);
        Fixture F;
        F.Image.Arch = Architecture;
        F.Image.ObjCSourceReferences.clear();
        F.Image.Sections[0].Type = llvm::MachO::S_CSTRING_LITERALS;
        auto Segment = F.Image.Segments[0];
        Segment.VA = Segment.FileOff = 0x2000;
        Segment.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
        Segment.ReadOnlyAfterRelocations = true;
        llvm::support::endian::write64le(Segment.Data.data(), 0x1009);
        F.Image.Segments.push_back(Segment);
        auto Section = F.Image.Sections[0];
        Section.VA = Section.FileOff = 0x2000;
        Section.Type = llvm::MachO::S_REGULAR;
        Section.Flags = Segment.Flags;
        F.Image.Sections.push_back(Section);
        F.Image.DataPtrRelocSlots.insert(0x2000);
        F.Image.DataPtrRelocTargetOwners[0x2000] = 0x1000;
        F.Image.MachOHasChainedFixups = Chained;
        F.Image.MachOResolvedChainedPointerSlots.insert(0x2000);
        const auto Pointer = NdType::makePtr(NdType::makeInt(1));
        F.Function.ReturnType = Pointer;
        F.Function.Body[0].RetVal =
            HighExpr::makeLoad(HighExpr::makeConst(0x2000, 8), Pointer);
        if (Mutation == 1)
          F.Image.DataPtrRelocSlots.clear();
        if (Mutation == 2)
          F.Image.DataPtrRelocTargetOwners[0x2000] = 0x2000;
        if (Mutation == 3)
          F.Image.Segments[1].ReadOnlyAfterRelocations = false;
        if (Mutation == 4)
          F.Image.ImportPtrSlots[0x2000] = "_unresolved";
        if (Mutation == 5)
          F.Image.DataPtrRelocSlots.insert(0x10f8);
        if (Mutation == 6)
          F.Image.Segments[0].Flags =
              SegmentFlags::Readable | SegmentFlags::Writable;
        if (Mutation == 7)
          F.Image.Sections[1].FileSz = 4;
        if (Mutation == 8)
          F.Image.MachOChainedFixupsAmbiguous = true;
        if (Mutation == 9) {
          F.Image.MachOHasChainedFixups = true;
          F.Image.MachOResolvedChainedPointerSlots.clear();
        }
        if (Mutation == 10)
          F.Function.Body[0].RetVal->Type = NdType::makeInt(4);
        if (Mutation == 11)
          F.Function.Body[0].RetVal = HighExpr::makeConst(0x2000, 8);
        EXPECT_EQ(bool(cstringStorageSourceHint(F.Image, 0x1000, 0x2000)),
                  Mutation == 0 || Mutation >= 10);
        const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
        if (Mutation) {
          EXPECT_TRUE(Bound.CStringPointerSlots.empty());
          continue;
        }
        ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
        EXPECT_EQ(Bound.CStringPointerSlots, std::set<va_t>{0x2000});
        const auto Helper = Bound.Function.Body[0].RetVal;
        ASSERT_TRUE(Helper->SourceCallHint);
        EXPECT_TRUE(objcSourceCallBound(*Helper, F.Image, {}));
        std::set<std::string> Shared;
        const auto Source =
            renderCStringStorageHelpers(F.Image, {0x1000}, Shared, {0x2000});
        EXPECT_EQ(Shared.size(), 2U);
        EXPECT_NE(Source.find("neverd_cstring_storage_1000_address() + 9"),
                  std::string::npos);
        // The helper represents the slot load, so re-rendering after a new
        // proved interior target must use that target, not a cached offset.
        llvm::support::endian::write64le(F.Image.Segments[1].Data.data(),
                                         0x1011);
        EXPECT_TRUE(objcSourceCallBound(*Helper, F.Image, {}));
        Shared.clear();
        const auto Updated =
            renderCStringStorageHelpers(F.Image, {}, Shared, {0x2000});
        EXPECT_NE(Updated.find("neverd_cstring_storage_1000_address() + 17"),
                  std::string::npos);
        auto Forged =
            std::make_shared<SourceCallTypeHint>(*Helper->SourceCallHint);
        Forged->ImmutablePointerSlot += 8;
        Helper->SourceCallHint = Forged;
        EXPECT_FALSE(objcSourceCallBound(*Helper, F.Image, {}));
        F.Image.DataPtrRelocSlots.clear();
        EXPECT_THROW(renderCStringStorageHelpers(F.Image, {}, Shared, {0x2000}),
                     std::runtime_error);
      }
}

TEST(ObjCSourceBindings, TaggedCStringWordsKeepTheTagAndSharedPoolIdentity) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool TagFirst : {false, true}) {
      Fixture F;
      F.Image.Arch = Architecture;
      F.Image.ObjCSourceReferences.clear();
      F.Image.Sections[0].Type = llvm::MachO::S_CSTRING_LITERALS;
      F.Function.ReturnType = NdType::makeVoid();
      const auto Address = HighExpr::makeConst(
          0x1040, 8, ConstantAddressProvenance::DataAddress, 0x1040);
      const auto Tag = HighExpr::makeConst(SwiftLiteralString::ImmortalTag, 8,
                                           ConstantAddressProvenance::Scalar);
      const auto Value = HighExpr::makeBinop(
          NdOp::INT_OR, TagFirst ? Tag : Address, TagFirst ? Address : Tag);
      HighStmt Store;
      Store.Kind = StmtKind::Store;
      Store.StoreAddr =
          HighExpr::makeVar(MedVar{.Kind = MedVar::Param, .Size = 8});
      Store.StoreVal = Value;
      F.Function.Body = {Store};
      const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      EXPECT_EQ(Bound.CStringSections, std::set<va_t>{0x1000});
      EXPECT_TRUE(Bound.BorrowedBytes.empty());
      const auto Word = Bound.Function.Body[0].StoreVal;
      ASSERT_EQ(Word->Kind, ExprKind::BinOp);
      EXPECT_EQ(Word->Op, NdOp::INT_OR);
      EXPECT_EQ(Word->Type->Kind, NdTypeKind::Int);
      EXPECT_EQ(Word->Type->Size, 8U);
      EXPECT_EQ(Word->Operands[TagFirst ? 0 : 1]->ConstVal,
                SwiftLiteralString::ImmortalTag);
      const auto Relocated = Word->Operands[TagFirst ? 1 : 0];
      ASSERT_EQ(Relocated->Kind, ExprKind::BinOp);
      EXPECT_EQ(Relocated->Op, NdOp::INT_ADD);
      EXPECT_EQ(Relocated->Operands[1]->ConstVal, 0x40U);
      const auto Helper = Relocated->Operands[0];
      ASSERT_TRUE(Helper->SourceCallHint);
      EXPECT_EQ(Helper->SourceCallHint->CallKind,
                SourceCallTypeHint::Kind::RuntimeCStringStorage);
      EXPECT_TRUE(objcSourceCallBound(*Helper, F.Image, {}));
      EXPECT_EQ(Address->Kind, ExprKind::Const);
      EXPECT_EQ(Address->ConstVal, 0x1040U);
      EXPECT_EQ(Value->Operands[TagFirst ? 1 : 0], Address);
      F.Image.DataPtrRelocSlots.insert(0x1080);
      EXPECT_FALSE(objcSourceCallBound(*Helper, F.Image, {}));
    }
}

TEST(ObjCSourceBindings, TaggedCStringInPointerCarrierKeepsSharedPool) {
  Fixture F;
  F.Image.ObjCSourceReferences.clear();
  F.Image.Sections[0].Type = llvm::MachO::S_CSTRING_LITERALS;
  F.Function.ReturnType = NdType::makeVoid();
  const auto Address = HighExpr::makeConst(
      0x1040, 8, ConstantAddressProvenance::DataAddress, 0x1040);
  const auto Tagged = HighExpr::makeBinop(
      NdOp::INT_OR, Address,
      HighExpr::makeConst(SwiftLiteralString::ImmortalTag, 8,
                          ConstantAddressProvenance::Scalar));
  auto PointerCarrier = std::make_shared<HighExpr>();
  PointerCarrier->Kind = ExprKind::Cast;
  PointerCarrier->Type = NdType::makePtr(NdType::makeVoid());
  PointerCarrier->CastTo = PointerCarrier->Type;
  PointerCarrier->Operands = {Tagged};
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr = HighExpr::makeVar(MedVar{.Kind = MedVar::Param, .Size = 8});
  Store.StoreVal = PointerCarrier;
  F.Function.Body = {Store};

  const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  EXPECT_EQ(Bound.CStringSections, std::set<va_t>{0x1000});
  const auto Relocated = Bound.Function.Body[0].StoreVal->Operands[0];
  ASSERT_EQ(Relocated->Kind, ExprKind::BinOp);
  EXPECT_EQ(Relocated->Op, NdOp::INT_OR);
  EXPECT_EQ(Relocated->Operands[1]->ConstVal, SwiftLiteralString::ImmortalTag);
  ASSERT_EQ(Relocated->Operands[0]->Kind, ExprKind::BinOp);
  EXPECT_EQ(Relocated->Operands[0]->Op, NdOp::INT_ADD);
  EXPECT_EQ(Relocated->Operands[0]->Operands[1]->ConstVal, 0x40U);
}

TEST(ObjCSourceBindings, TaggedCStringWordsRejectUnprovenAddressOccurrences) {
  for (unsigned Mutation = 0; Mutation != 15; ++Mutation) {
    SCOPED_TRACE(Mutation);
    Fixture F;
    F.Image.ObjCSourceReferences.clear();
    F.Image.Sections[0].Type = llvm::MachO::S_CSTRING_LITERALS;
    F.Function.ReturnType = NdType::makeVoid();
    const auto Address = HighExpr::makeConst(
        0x1040, 8, ConstantAddressProvenance::DataAddress, 0x1040);
    const auto Tag = HighExpr::makeConst(SwiftLiteralString::ImmortalTag, 8,
                                         ConstantAddressProvenance::Scalar);
    auto Value = HighExpr::makeBinop(NdOp::INT_OR, Address, Tag);
    HighStmt Store;
    Store.Kind = StmtKind::Store;
    Store.StoreAddr =
        HighExpr::makeVar(MedVar{.Kind = MedVar::Param, .Size = 8});
    Store.StoreVal = Value;
    switch (Mutation) {
    case 0:
      Address->ConstProvenance = ConstantAddressProvenance::Unknown;
      break;
    case 1:
      Address->ConstProvenance = ConstantAddressProvenance::Scalar;
      break;
    case 2:
      Address->ConstProvenance = ConstantAddressProvenance::AddressFragment;
      break;
    case 3:
      Address->ConstProvenance = ConstantAddressProvenance::CodeAddress;
      break;
    case 4:
      Address->AddressOwnerVA = 0x1020;
      break;
    case 5:
      Address->Type = NdType::makeInt(4);
      break;
    case 6:
      Tag->ConstProvenance = ConstantAddressProvenance::DataAddress;
      break;
    case 7:
      Tag->AddressOwnerVA = 0x1000;
      break;
    case 8:
      Tag->ConstVal >>= 1;
      break;
    case 9:
      Value->Op = NdOp::INT_ADD;
      break;
    case 10:
      Value->Type = NdType::makeInt(4);
      break;
    case 11:
      F.Image.Sections[0].Type = llvm::MachO::S_REGULAR;
      break;
    case 12:
      F.Image.Sections[0].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
      break;
    case 13:
      F.Image.DataPtrRelocSlots.insert(0x1080);
      break;
    case 14:
      Store.StoreAddr = Value;
      Store.StoreVal = HighExpr::makeConst(7, 8);
      break;
    }
    F.Function.Body = {Store};
    const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_FALSE(Bound.Limitation.empty());
    EXPECT_TRUE(Bound.CStringSections.empty());
  }
}

TEST(ObjCSourceBindings, CStringPoolsRejectPartialAmbiguousAndMutableStorage) {
  for (unsigned Case = 0; Case != 9; ++Case) {
    SCOPED_TRACE(Case);
    Fixture F;
    F.Image.ObjCSourceReferences.clear();
    F.Image.Sections[0].Type = llvm::MachO::S_CSTRING_LITERALS;
    switch (Case) {
    case 0:
      F.Image.Sections[0].Type = llvm::MachO::S_REGULAR;
      break;
    case 1:
      F.Image.Segments[0].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
      break;
    case 2:
      F.Image.Sections[0].FileSz--;
      break;
    case 3:
      F.Image.Sections.push_back(F.Image.Sections[0]);
      break;
    case 4:
      F.Image.ImportPtrSlots[0x10f8] = "_other";
      break;
    case 5:
      F.Image.MachOChainedFixupsAmbiguous = true;
      break;
    case 6:
      F.Image.IsRelocatable = true;
      break;
    case 7:
      F.Image.Sections[0].Size = 1024 * 1024 + 1;
      break;
    case 8:
      F.Image.Segments[0].Data.pop_back();
      break;
    }
    EXPECT_FALSE(cstringStorageSourceHint(F.Image, 0x1000));
  }
}

TEST(ObjCSourceBindings, CStringPoolsExecuteEveryByteWithoutChangingIdentity) {
  Fixture F;
  F.Image.ObjCSourceReferences.clear();
  F.Image.Sections[0].Type = llvm::MachO::S_CSTRING_LITERALS;
  for (unsigned I = 0; I != 256; ++I)
    F.Image.Segments[0].Data[I] = uint8_t(I);
  std::set<std::string> Helpers;
  std::string Source = "#include <stdint.h>\n" +
                       renderCStringStorageHelpers(F.Image, {0x1000}, Helpers);
  Source += R"(
int main(void) {
  const unsigned char *bytes =
      (const unsigned char *)neverd_cstring_storage_1000_address();
  for (unsigned i = 0; i != 256; ++i) {
    if (bytes[i] != i) return 1;
    const unsigned char *alias =
        (const unsigned char *)neverd_cstring_storage_1000_address() + i;
    if (alias != bytes + i || *alias != i) return 2;
  }
  return 0;
}
)";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-cstring-storage",
                                                    Directory));
  const std::filesystem::path Work(Directory.str().str());
  struct Cleanup {
    std::filesystem::path Work;
    ~Cleanup() {
      std::error_code Error;
      std::filesystem::remove_all(Work, Error);
    }
  } Cleanup{Work};
  const auto Path = (Work / "literal.c").string();
  const auto Executable = (Work / "literal").string();
  const auto ErrorPath = (Work / "stderr").string();
  std::ofstream(Path) << Source;
  const std::string Compiler = NEVERD_TEST_CLANG;
  const std::vector<std::string> Arguments{
      Compiler, "-std=c11", "-O3", "-Werror", Path, "-o", Executable};
  std::vector<llvm::StringRef> Refs(Arguments.begin(), Arguments.end());
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt,
                                                      std::nullopt, ErrorPath};
  std::string Error;
  const auto Status = llvm::sys::ExecuteAndWait(Compiler, Refs, std::nullopt,
                                                Redirects, 60, 0, &Error);
  auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
  ASSERT_EQ(Status, 0) << Error << (Errors ? (*Errors)->getBuffer().str() : "");
  EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable}, std::nullopt,
                                      Redirects, 30, 0, &Error),
            0)
      << Error;
}

TEST(ObjCSourceBindings, ImmutableScalarsPreserveWidthSignAndFloatingBits) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (const auto &Type :
         {NdType::makeInt(1, false), NdType::makeInt(2, true),
          NdType::makeInt(4, true), NdType::makeInt(8, false),
          NdType::makeFloat(4), NdType::makeFloat(8)}) {
      for (uint64_t Bits : std::array<uint64_t, 8>{
               0, 7, 0x8000000000000000ULL, 0x41323456789abcdeULL,
               0x7ff8000001234567ULL, 0x80000000, 0x7fc12345, UINT64_MAX}) {
        SCOPED_TRACE(Type->str());
        SCOPED_TRACE(Bits);
        Fixture F;
        F.Image.Arch = Architecture;
        F.Image.ObjCSourceReferences.clear();
        llvm::support::endian::write64le(F.Image.Segments[0].Data.data() + 0x40,
                                         Bits);
        F.Function.ReturnType = Type;
        auto Load = HighExpr::makeLoad(HighExpr::makeConst(0x1040, 8), Type);
        F.Function.Body[0].RetVal = Load;
        const auto Result = bindObjCSourceReferences(F.Function, F.Image);
        ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
        const auto Value = Result.Function.Body[0].RetVal;
        ASSERT_EQ(Value->Kind, ExprKind::BitCast);
        EXPECT_EQ(Value->Type->str(), Type->str());
        ASSERT_EQ(Value->Operands.size(), 1U);
        EXPECT_EQ(Value->Operands[0]->Kind, ExprKind::Const);
        EXPECT_EQ(Value->Operands[0]->Type->Size, Type->Size);
        EXPECT_EQ(Value->Operands[0]->ConstProvenance,
                  ConstantAddressProvenance::Scalar);
        const uint64_t Mask = Type->Size == 8
                                  ? UINT64_MAX
                                  : (uint64_t(1) << (Type->Size * 8)) - 1;
        EXPECT_EQ(Value->Operands[0]->ConstVal, Bits & Mask);
        EXPECT_EQ(Load->Kind, ExprKind::Load);
        EXPECT_EQ(Load->Operands[0]->ConstVal, 0x1040U);
      }
    }
  }
}

TEST(ObjCSourceBindings, BooleanConditionsDoNotTurnScalarReadsIntoAddresses) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    Fixture F;
    F.Image.Arch = Architecture;
    F.Image.ObjCSourceReferences.clear();
    llvm::support::endian::write64le(F.Image.Segments[0].Data.data() + 0x40, 7);
    MedVar Parameter;
    Parameter.Kind = MedVar::Param;
    Parameter.Id = 0;
    Parameter.Size = 8;
    auto Pointer = HighExpr::makeVar(Parameter, NdType::makePtr());
    auto Load =
        HighExpr::makeLoad(HighExpr::makeConst(0x1040, 8), NdType::makeInt(8));
    auto Compare =
        HighExpr::makeBinop(NdOp::INT_EQUAL, Load, HighExpr::makeConst(7, 8));
    auto Condition = HighExpr::makeBinop(NdOp::BOOL_OR, Pointer, Compare);
    ASSERT_EQ(Compare->Type->Kind, NdTypeKind::Int);
    ASSERT_EQ(Compare->Type->Size, 1U);
    ASSERT_EQ(Condition->Type->Kind, NdTypeKind::Int);
    ASSERT_EQ(Condition->Type->Size, 1U);
    F.Function.ReturnType = Condition->Type;
    F.Function.Body[0].RetVal = Condition;

    const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    const auto Scalar = Bound.Function.Body[0].RetVal->Operands[1]->Operands[0];
    ASSERT_EQ(Scalar->Kind, ExprKind::BitCast);
    ASSERT_EQ(Scalar->Operands[0]->Kind, ExprKind::Const);
    EXPECT_EQ(Scalar->Operands[0]->ConstVal, 7U);
    EXPECT_EQ(Load->Kind, ExprKind::Load);
  }
}

TEST(ObjCSourceBindings,
     ImmutableWideScalarsPreserveBothLanesAndRejectPointers) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Case = 0; Case != 7; ++Case) {
      SCOPED_TRACE(Case);
      Fixture F;
      F.Image.Arch = Architecture;
      F.Image.ObjCSourceReferences.clear();
      uint64_t Low = 0xfedcba9876543210ULL, High = 0x8123456789abcdefULL;
      if (Case == 1)
        Low = 0x1040;
      if (Case == 2)
        High = 0x1040;
      llvm::support::endian::write64le(F.Image.Segments[0].Data.data() + 0x40,
                                       Low);
      llvm::support::endian::write64le(F.Image.Segments[0].Data.data() + 0x48,
                                       High);
      if (Case == 3)
        F.Image.DataPtrRelocSlots.insert(0x1048);
      if (Case == 4)
        F.Image.Sections[0].FileSz = 0x4f;
      if (Case == 5)
        F.Image.Sections[0].Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
      F.Function.ReturnType = NdType::makeInt(16, Case == 6);
      auto Load = HighExpr::makeLoad(HighExpr::makeConst(0x1040, 8),
                                     F.Function.ReturnType);
      F.Function.Body[0].RetVal = Load;
      const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
      if (Case >= 1 && Case <= 5) {
        EXPECT_FALSE(Bound.Limitation.empty());
        continue;
      }
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      const auto Value = Bound.Function.Body[0].RetVal;
      ASSERT_EQ(Value->Kind, ExprKind::BitCast);
      EXPECT_EQ(Value->Type->str(), F.Function.ReturnType->str());
      const auto Joined = Value->Operands[0];
      ASSERT_EQ(Joined->Kind, ExprKind::BinOp);
      ASSERT_EQ(Joined->Op, NdOp::CONCAT);
      EXPECT_EQ(Joined->Type->Size, 16U);
      EXPECT_EQ(Joined->Operands[0]->ConstVal, High);
      EXPECT_EQ(Joined->Operands[1]->ConstVal, Low);
      for (const auto &Lane : Joined->Operands)
        EXPECT_EQ(Lane->ConstProvenance, ConstantAddressProvenance::Scalar);
      EXPECT_EQ(Load->Kind, ExprKind::Load);
    }
  }
}

TEST(ObjCSourceBindings, ImmutableScalarsRejectUnprovedStorageAndAddressUses) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Case = 0; Case < 22; ++Case) {
      SCOPED_TRACE(Case);
      Fixture F;
      F.Image.Arch = Architecture;
      F.Image.ObjCSourceReferences.clear();
      const auto Type = NdType::makeInt(8, false);
      F.Function.ReturnType = Type;
      auto Load = HighExpr::makeLoad(HighExpr::makeConst(0x1040, 8), Type);
      F.Function.Body[0].RetVal = Load;
      llvm::support::endian::write64le(F.Image.Segments[0].Data.data() + 0x40,
                                       7);
      switch (Case) {
      case 0:
        F.Image.Sections[0].Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
        break;
      case 1:
        F.Image.Segments[0].Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
        break;
      case 2:
        F.Image.Sections[0].FileSz = 0x47;
        break;
      case 3:
        F.Image.Segments[0].Data.resize(0x47);
        break;
      case 4:
        F.Image.Sections.push_back(F.Image.Sections[0]);
        break;
      case 5:
        F.Image.Segments.push_back(F.Image.Segments[0]);
        break;
      case 6:
        F.Image.Sections[0].Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
        F.Image.Sections[0].Flags =
            SegmentFlags::Readable | SegmentFlags::Executable;
        break;
      case 7:
        F.Image.BaseRelocations.push_back({0x1039, 0});
        break;
      case 8:
        F.Image.MachOResolvedChainedPointerSlots.insert(0x1040);
        break;
      case 9:
        F.Image.MachOChainedFixupsAmbiguous = true;
        break;
      case 10:
        Load->Type = NdType::makePtr(NdType::makeVoid());
        break;
      case 11:
        Load->Type = NdType::makeFloat(16);
        break;
      case 12:
        Load->Type = NdType::makeInt(32, false);
        break;
      case 13:
        Load->MemoryOrdering = NdMemoryOrdering::Acquire;
        break;
      case 14:
        Load->MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
        break;
      case 15:
        llvm::support::endian::write64le(F.Image.Segments[0].Data.data() + 0x40,
                                         0x1040);
        break;
      case 16:
        F.Function.ReturnType = NdType::makePtr(NdType::makeVoid());
        break;
      case 17:
        // A shared load may fold in a numeric occurrence, but that proof
        // cannot authorize the same node as a memory address.
        F.Function.Body[0].RetVal = HighExpr::makeBinop(
            NdOp::INT_ADD, Load, HighExpr::makeLoad(Load, Type));
        break;
      case 18:
        F.Image.IsRelocatable = true;
        break;
      case 19:
        F.Image.Segments[0].FileSz = 0x47;
        break;
      case 20:
        F.Image.DataPtrRelocSlots.insert(0x1047);
        break;
      case 21:
        Load->Type = NdType::makeInt(3, false);
        break;
      }
      const auto Result = bindObjCSourceReferences(F.Function, F.Image);
      EXPECT_FALSE(Result.Limitation.empty());
      EXPECT_EQ(Load->Kind, ExprKind::Load);
      if (Case == 17)
        EXPECT_EQ(Result.Function.Body[0].RetVal->Operands[0]->Kind,
                  ExprKind::BitCast);
    }
  }
}

namespace {
struct SwiftLiteralFixture : Fixture {
  static constexpr va_t ImportSlot = 0x10e0;
  static constexpr va_t Contents = 0x1040;
  std::string Text;
  ExprPtr Call;
  SwiftLiteralFixture(Arch Architecture, bool Unicode = false) {
    Image.Arch = Architecture;
    Image.ObjCSourceReferences.clear();
    Text = Unicode ? "literal caf\xc3\xa9 bytes"
                   : "immutable compiler literal bytes";
    std::copy(Text.begin(), Text.end(), Image.Segments[0].Data.begin() + 0x40);
    const std::string Name =
        "_$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF";
    Image.ImportPtrSlots[ImportSlot] = Name;
    Image.recordDyldBindSlot(
        ImportSlot, Name, 0,
        "/System/Library/Frameworks/Foundation.framework/Foundation", false);
    const uint64_t Flags =
        Unicode ? UINT64_C(0x1000000000000000) : UINT64_C(0xd000000000000000);
    const auto Storage = HighExpr::makeBinop(
        NdOp::INT_OR, HighExpr::makeConst(Contents - 32, 8),
        HighExpr::makeConst(UINT64_C(0x8000000000000000), 8));
    Call = HighExpr::makeCall(
        Name, ImportSlot,
        {HighExpr::makeConst(Flags | Text.size(), 8), Storage});
    Call->Type = NdType::makePtr(NdType::makeVoid());
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(
        *swiftStringSourceCallHint(Image, ImportSlot));
    Function.ReturnType = Call->Type;
    Function.Body[0].RetVal = Call;
  }
};
} // namespace

TEST(ObjCSourceBindings, SwiftLiteralStoragePreservesBytesAndConsumerIdentity) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (bool Unicode : {false, true}) {
      SwiftLiteralFixture F(Architecture, Unicode);
      const auto Result = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
      const BorrowedByteRange Range{F.Contents, F.Text.size() + 1};
      EXPECT_EQ(Result.BorrowedBytes, std::set<BorrowedByteRange>{Range});
      const auto Bound = Result.Function.Body[0].RetVal;
      EXPECT_EQ(Bound->SourceCallHint->CallKind,
                SourceCallTypeHint::Kind::SwiftStringBridge);
      EXPECT_TRUE(objcSourceCallBound(*Bound, F.Image, {}));
      ASSERT_NE(
          objc_binding_detail::swiftLiteralStorageHelper(Bound->Operands[1]),
          nullptr);
      EXPECT_EQ(F.Call->Operands[1]->Operands[0]->Kind, ExprKind::Const);
      std::set<std::string> Shared;
      const auto Helpers =
          renderBorrowedByteHelpers(F.Image, Result.BorrowedBytes, Shared);
      EXPECT_EQ(Shared, std::set<std::string>{borrowedByteHelperName(Range)});
      EXPECT_NE(Helpers.find("static const unsigned char bytes[]"),
                std::string::npos);
      // A raw occurrence outside the established bridge must retain its own
      // address-binding failure, even when it shares the original node.
      F.Function.Body[0].RetVal =
          HighExpr::makeBinop(NdOp::INT_OR, F.Call, F.Call->Operands[1]);
      EXPECT_FALSE(
          bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
    }
  }
}

TEST(ObjCSourceBindings, SwiftInlineStringValidatesBothWords) {
  for (const std::string Text :
       {std::string(), std::string("url"), std::string("abcdefgh"),
        std::string("abcdefghijklmno"), std::string("caf\xc3\xa9"),
        std::string("\xe4\xb8\xad\xf0\x9f\x8c\x8d"), std::string("a\0b", 3)}) {
    std::array<uint8_t, 16> Bytes{};
    std::copy(Text.begin(), Text.end(), Bytes.begin());
    const bool ASCII = std::all_of(Text.begin(), Text.end(), [](char C) {
      return static_cast<unsigned char>(C) < 0x80;
    });
    Bytes[15] = (ASCII ? 0xe0 : 0xa0) | Text.size();
    const auto Word = llvm::support::endian::read64le(Bytes.data());
    const auto Storage = llvm::support::endian::read64le(Bytes.data() + 8);
    EXPECT_TRUE(isCanonicalSwiftSmallString(Word, Storage));
    EXPECT_FALSE(
        isCanonicalSwiftSmallString(Word, Storage ^ (UINT64_C(1) << 62)));
    EXPECT_FALSE(
        isCanonicalSwiftSmallString(Word, Storage | (UINT64_C(1) << 60)));
    if (Text.size() < 15) {
      Bytes[Text.size()] = 1;
      EXPECT_FALSE(isCanonicalSwiftSmallString(
          llvm::support::endian::read64le(Bytes.data()),
          llvm::support::endian::read64le(Bytes.data() + 8)));
    }
  }
  for (const auto InvalidUTF8 :
       {UINT64_C(0x80), UINT64_C(0xc3), UINT64_C(0xafc0), UINT64_C(0x80a0ed)}) {
    SCOPED_TRACE(InvalidUTF8);
    const unsigned Count = InvalidUTF8 <= 0xff     ? 1
                           : InvalidUTF8 <= 0xffff ? 2
                                                   : 3;
    EXPECT_FALSE(isCanonicalSwiftSmallString(InvalidUTF8,
                                             (UINT64_C(0xa0) | Count) << 56));
  }
}

TEST(ObjCSourceBindings, SwiftInlineStringPayloadIsLocalToItsExactConsumer) {
  using P = ConstantAddressProvenance;
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (const auto Provenance : {P::Unknown, P::Scalar}) {
      SwiftLiteralFixture F(Architecture);
      // Two ASCII bytes happen to occupy an unrelated mapped-image address.
      const auto Word = HighExpr::makeConst(0x1041, 8, Provenance);
      const auto Storage =
          HighExpr::makeConst(UINT64_C(0xe200000000000000), 8, Provenance);
      F.Call->Operands = {Word, Storage};
      const auto Result = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
      EXPECT_TRUE(Result.BorrowedBytes.empty());
      const auto Bound = Result.Function.Body[0].RetVal;
      ASSERT_TRUE(objcSourceCallBound(*Bound, F.Image, {}));
      for (unsigned I = 0; I < 2; ++I) {
        EXPECT_EQ(Bound->Operands[I]->Kind, ExprKind::Const);
        EXPECT_EQ(Bound->Operands[I]->ConstVal, F.Call->Operands[I]->ConstVal);
        EXPECT_EQ(Bound->Operands[I]->ConstProvenance, P::Scalar);
        EXPECT_EQ(F.Call->Operands[I]->ConstProvenance, Provenance);
      }
      F.Function.Body[0].RetVal =
          HighExpr::makeBinop(NdOp::INT_OR, F.Call, Word);
      EXPECT_FALSE(
          bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
    }
}

TEST(ObjCSourceBindings,
     SwiftInlineStringRejectsUnprovenConsumerAndAddressWords) {
  using P = ConstantAddressProvenance;
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 19; ++Mutation) {
      SCOPED_TRACE(Mutation);
      SwiftLiteralFixture F(Architecture);
      auto Word = HighExpr::makeConst(0x1041, 8, P::Scalar);
      auto Storage =
          HighExpr::makeConst(UINT64_C(0xe200000000000000), 8, P::Scalar);
      F.Call->Operands = {Word, Storage};
      auto Hint = std::make_shared<SourceCallTypeHint>(*F.Call->SourceCallHint);
      F.Call->SourceCallHint = Hint;
      switch (Mutation) {
      case 0:
        Word->ConstProvenance = P::DataAddress;
        break;
      case 1:
        Word->ConstProvenance = P::CodeAddress;
        break;
      case 2:
        Word->AddressOwnerVA = Word->ConstVal;
        break;
      case 3:
        Storage->ConstProvenance = P::DataAddress;
        break;
      case 4:
        Storage->AddressOwnerVA = 0x1041;
        break;
      case 5:
        Storage->ConstVal = UINT64_C(0xe100000000000000);
        break;
      case 6:
        Storage->ConstVal = UINT64_C(0xa200000000000000);
        break;
      case 7:
        F.Image.DyldBindSlots[F.ImportSlot].Module = "/tmp/Foundation";
        break;
      case 8:
        F.Image.DyldBindSlots[F.ImportSlot].WeakImport = true;
        break;
      case 9:
        F.Image.DyldBindSlots[F.ImportSlot].Addend = 8;
        break;
      case 10:
        Hint->SwiftStringInputs = {{1, 0}};
        break;
      case 11:
        Hint->SwiftStringInputs.clear();
        break;
      case 12:
        F.Call->IsIndirectCall = true;
        break;
      case 13:
        Word->Type = NdType::makeInt(4, false);
        break;
      case 14:
        Storage->Operands.push_back(HighExpr::makeConst(0, 8));
        break;
      case 15:
        Storage->MemoryOrdering = NdMemoryOrdering::Acquire;
        break;
      case 16:
        Storage->IndirectTarget = HighExpr::makeConst(0, 8);
        break;
      case 17:
        F.Call->Operands.push_back(HighExpr::makeConst(0, 8));
        break;
      case 18:
        Hint->Signature.Parameters[0].Type = NdType::makeFloat(8);
        break;
      }
      EXPECT_FALSE(
          bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
    }
}

TEST(ObjCSourceBindings,
     UIKitImageLiteralInitializerRebuildsExactSwiftStringStorage) {
  SwiftLiteralFixture F(Arch::AArch64);
  const std::string Name =
      "_$sSo7UIImageC5UIKitE24imageLiteralResourceNameABSS_tcfC";
  F.Image.ImportPtrSlots[F.ImportSlot] = Name;
  F.Image.ImportStorageSlots[F.ImportSlot].Name = Name;
  F.Image.DyldBindSlots[F.ImportSlot].Name = Name;
  F.Image.DyldBindSlots[F.ImportSlot].Module =
      "/System/Library/Frameworks/UIKit.framework/UIKit";
  const auto Hint = swiftRuntimeSourceCallHint(F.Image, F.ImportSlot);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->SwiftStringInputs,
            (std::vector<std::pair<uint32_t, uint32_t>>{{0, 1}}));
  F.Call->CallTarget = Name;
  F.Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
  const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  EXPECT_EQ(Bound.BorrowedBytes,
            (std::set<BorrowedByteRange>{{F.Contents, F.Text.size() + 1}}));
  ASSERT_TRUE(objc_binding_detail::swiftLiteralStorageHelper(
      Bound.Function.Body[0].RetVal->Operands[1]));
  EXPECT_TRUE(objcSourceCallBound(*Bound.Function.Body[0].RetVal, F.Image, {}));

  F.Image.DyldBindSlots[F.ImportSlot].Module = "/tmp/UIKit";
  EXPECT_FALSE(
      objcSourceCallBound(*Bound.Function.Body[0].RetVal, F.Image, {}));
  EXPECT_TRUE(
      bindObjCSourceReferences(F.Function, F.Image).BorrowedBytes.empty());
}

TEST(ObjCSourceBindings,
     FoundationURLInitializerRebuildsExactSwiftStringStorage) {
  SwiftLiteralFixture F(Arch::AArch64);
  const std::string Name = "_$s10Foundation3URLV6stringACSgSSh_tcfC";
  F.Image.ImportPtrSlots[F.ImportSlot] = Name;
  F.Image.ImportStorageSlots[F.ImportSlot].Name = Name;
  F.Image.DyldBindSlots[F.ImportSlot].Name = Name;
  F.Call->CallTarget = Name;
  const auto Hint = swiftRuntimeSourceCallHint(F.Image, F.ImportSlot);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->SwiftStringInputs,
            (std::vector<std::pair<uint32_t, uint32_t>>{{1, 2}}));
  MedVar Result;
  Result.Kind = MedVar::Param;
  Result.Id = 0;
  Result.Size = 8;
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  F.Function.Params.push_back({"result", Pointer});
  F.Call->Operands.insert(F.Call->Operands.begin(),
                          HighExpr::makeVar(Result, Pointer));
  F.Call->Type = Hint->Signature.ReturnType;
  F.Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
  F.Function.ReturnType = NdType::makeVoid();
  F.Function.Body[0].Kind = StmtKind::Call;
  F.Function.Body[0].CallExpr = F.Call;
  F.Function.Body[0].RetVal.reset();
  const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  EXPECT_EQ(Bound.BorrowedBytes,
            (std::set<BorrowedByteRange>{{F.Contents, F.Text.size() + 1}}));
  ASSERT_TRUE(objc_binding_detail::swiftLiteralStorageHelper(
      Bound.Function.Body[0].CallExpr->Operands[2]));
  EXPECT_TRUE(
      objcSourceCallBound(*Bound.Function.Body[0].CallExpr, F.Image, {}));

  F.Image.DyldBindSlots[F.ImportSlot].Module = "/tmp/Foundation";
  EXPECT_FALSE(
      objcSourceCallBound(*Bound.Function.Body[0].CallExpr, F.Image, {}));
  EXPECT_TRUE(
      bindObjCSourceReferences(F.Function, F.Image).BorrowedBytes.empty());
}

TEST(ObjCSourceBindings,
     SwiftLiteralStorageRejectsChangedWordsStorageAndImports) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 17; ++Mutation) {
      SCOPED_TRACE(Mutation);
      SwiftLiteralFixture F(Architecture);
      auto &Word = F.Call->Operands[0]->ConstVal;
      switch (Mutation) {
      case 0:
        Word |= UINT64_C(0x2000000000000000);
        break;
      case 1:
        F.Call->Operands[1]->Operands[1]->ConstVal = 0;
        break;
      case 2:
        ++Word;
        break;
      case 3:
        --Word;
        break;
      case 4:
        F.Image.Segments[0].Data[0x44] = 0;
        break;
      case 5:
        F.Image.Segments[0].Data[0x44] = 0xff;
        break;
      case 6:
        F.Image.Sections[0].Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
        break;
      case 7:
        F.Image.Segments[0].Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
        break;
      case 8:
        F.Image.DataPtrRelocSlots.insert(F.Contents - 1);
        break;
      case 9:
        F.Image.Sections[0].FileSz = 0x48;
        break;
      case 10:
        F.Image.DyldBindSlots[F.ImportSlot].Module =
            "/tmp/libswiftFoundation.dylib";
        break;
      case 11:
        F.Image.DyldBindSlots[F.ImportSlot].WeakImport = true;
        break;
      case 12:
        F.Image.DyldBindSlots[F.ImportSlot].Addend = 8;
        break;
      case 13:
        F.Image.MachOChainedFixupsAmbiguous = true;
        break;
      case 14:
        F.Call->Operands[0]->Type = NdType::makeFloat(8);
        break;
      case 15:
        F.Call->IsIndirectCall = true;
        break;
      case 16:
        F.Call->MemoryOrdering = NdMemoryOrdering::Acquire;
        break;
      }
      const auto Result = bindObjCSourceReferences(F.Function, F.Image);
      EXPECT_FALSE(Result.Limitation.empty());
      EXPECT_TRUE(Result.BorrowedBytes.empty());
    }
    for (unsigned Mutation = 0; Mutation < 9; ++Mutation) {
      SCOPED_TRACE(Mutation);
      SwiftLiteralFixture F(Architecture);
      auto Result = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_TRUE(Result.Limitation.empty());
      auto Bound = Result.Function.Body[0].RetVal;
      auto Helper = Bound->Operands[1]->Operands[0]->Operands[0];
      auto ChangedHint =
          std::make_shared<SourceCallTypeHint>(*Helper->SourceCallHint);
      Helper->SourceCallHint = ChangedHint;
      switch (Mutation) {
      case 0:
        ++Bound->Operands[0]->ConstVal;
        break;
      case 1:
        ++ChangedHint->TargetAddress;
        break;
      case 2:
        --ChangedHint->ByteCount;
        break;
      case 3:
        Bound->Operands[1]->Operands[1]->ConstVal = 0;
        break;
      case 4:
        Bound->Operands[1]->Operands[0]->Operands[1]->ConstVal = 16;
        break;
      case 5:
        F.Image.DyldBindSlots[F.ImportSlot].Module = "/tmp/other.dylib";
        break;
      case 6:
        Helper->IsIndirectCall = true;
        break;
      case 7:
        Bound->IsIndirectCall = true;
        break;
      case 8:
        Bound->Operands[1]->Type = NdType::makeFloat(8);
        break;
      }
      EXPECT_FALSE(objcSourceCallBound(*Bound, F.Image, {}));
    }
  }
}

namespace {
struct ConstantStringFixture {
  BinaryImage Image;
  HighFunc Function;
  ConstantStringFixture() {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Segment Objects;
    Objects.Name = "__DATA_CONST";
    Objects.VA = 0x2000;
    Objects.Size = Objects.FileSz = 64;
    Objects.Flags = SegmentFlags::Readable;
    Objects.Data.resize(64);
    Image.Segments.push_back(Objects);
    Section Records;
    Records.Name = "__cfstring";
    Records.SegmentName = Objects.Name;
    Records.VA = Objects.VA;
    Records.Size = Records.FileSz = Objects.Size;
    Records.Flags = Objects.Flags;
    Image.Sections.push_back(Records);
    Segment Text;
    Text.Name = "__TEXT";
    Text.VA = Text.FileOff = 0x1000;
    Text.Size = Text.FileSz = 0x200;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(0x200);
    Image.Segments.push_back(Text);
    Section ASCII;
    ASCII.Name = "__cstring";
    ASCII.SegmentName = Text.Name;
    ASCII.VA = ASCII.FileOff = Text.VA;
    ASCII.Size = ASCII.FileSz = 0x100;
    ASCII.Flags = Text.Flags;
    ASCII.Type = llvm::MachO::S_CSTRING_LITERALS;
    Image.Sections.push_back(ASCII);
    Section Unicode = ASCII;
    Unicode.Name = "__ustring";
    Unicode.VA = Unicode.FileOff = 0x1100;
    Unicode.Type = llvm::MachO::S_REGULAR;
    Image.Sections.push_back(Unicode);
    for (unsigned I = 0; I < 2; ++I) {
      const va_t Address = 0x2000 + I * 32;
      Image.recordDyldBindSlot(
          Address, "___CFConstantStringClassReference", 0,
          "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",
          false);
      auto *Record = Image.Segments[0].Data.data() + I * 32;
      llvm::support::endian::write64le(Record + 8, I ? 0x7d0 : 0x7c8);
      llvm::support::endian::write64le(Record + 16, I ? 0x1100 : 0x1000);
      llvm::support::endian::write64le(Record + 24, I ? 4 : 3);
    }
    Image.Segments[1].Data[0] = 'a';
    Image.Segments[1].Data[1] = '\n';
    Image.Segments[1].Data[2] = '"';
    const uint16_t Units[] = {0x767e, 0, 0xd83d, 0xde00, 0};
    for (unsigned I = 0; I < 5; ++I)
      llvm::support::endian::write16le(
          Image.Segments[1].Data.data() + 0x100 + I * 2, Units[I]);
    Function.ReturnType = NdType::makePtr(NdType::makeVoid());
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = HighExpr::makeConst(0x2000, 8);
    Function.Body.push_back(Return);
  }
};
} // namespace

namespace {
ConstantStringFixture immutablePointerFixture(Arch Architecture, bool Chained) {
  ConstantStringFixture F;
  F.Image.Arch = Architecture;
  F.Image.MachOHasChainedFixups = Chained;
  F.Image.MachOResolvedChainedPointerSlots = {0x2000, 0x2010, 0x2020,
                                              0x2030, 0x3000, 0x3008};
  Segment Slots;
  // Deliberately unrelated names: the loader flag, not spelling, is proof.
  Slots.Name = "pointer_storage";
  Slots.VA = Slots.FileOff = 0x3000;
  Slots.Size = Slots.FileSz = 32;
  Slots.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Slots.ReadOnlyAfterRelocations = true;
  Slots.Data.resize(32);
  llvm::support::endian::write64le(Slots.Data.data(), 0x2020);
  llvm::support::endian::write64le(Slots.Data.data() + 8, 0x2020);
  Slots.Data[16] = 42;
  F.Image.Segments.push_back(Slots);
  Section Section;
  Section.Name = "pointers";
  Section.SegmentName = Slots.Name;
  Section.VA = Section.FileOff = Slots.VA;
  Section.Size = Section.FileSz = Slots.Size;
  Section.Flags = Slots.Flags;
  F.Image.Sections.push_back(Section);
  F.Image.DataPtrRelocSlots = {0x3000, 0x3008};
  F.Image.DataPtrRelocTargetOwners = {{0x3000, 0x2000}, {0x3008, 0x2000}};
  F.Function.Body[0].RetVal = HighExpr::makeLoad(
      HighExpr::makeConst(0x3000, 8), NdType::makePtr(NdType::makeVoid()));
  return F;
}

ConstantStringFixture objectPointerTableFixture(Arch Architecture,
                                                bool Chained) {
  auto F = immutablePointerFixture(Architecture, Chained);
  llvm::support::endian::write64le(F.Image.Segments[2].Data.data() + 8, 0);
  F.Image.DataPtrRelocSlots.erase(0x3008);
  F.Image.DataPtrRelocTargetOwners.erase(0x3008);
  F.Image.MachOResolvedChainedPointerSlots.erase(0x3008);

  MedVar V;
  V.Kind = MedVar::Param;
  V.Size = 4;
  auto Index = HighExpr::makeVar(V, NdType::makeInt(4, false));
  auto Wide = HighExpr::makeUnary(NdOp::INT_ZEXT, Index);
  Wide->Type = NdType::makeInt(8, false);
  MedVar WideV;
  WideV.Kind = MedVar::Temp;
  WideV.Id = 88;
  WideV.Size = 8;
  auto WideIndex = HighExpr::makeVar(WideV, NdType::makeInt(8, false));
  HighStmt Assign;
  Assign.Kind = StmtKind::Assign;
  Assign.Dst = WideIndex;
  Assign.Val = Wide;
  auto Guard =
      HighExpr::makeBinop(NdOp::INT_LESS, HighExpr::makeConst(1, 4), WideIndex);
  Guard->Type = NdType::makeInt(4, false);
  auto Offset =
      HighExpr::makeBinop(NdOp::INT_MULT, WideIndex, HighExpr::makeConst(8, 8));
  auto Address = HighExpr::makeBinop(NdOp::INT_ADD,
                                     HighExpr::makeConst(0x3000, 8), Offset);
  HighStmt Load;
  Load.Kind = StmtKind::Assign;
  // Machine pointer loads retain their integer carrier until the surrounding
  // pointer consumer supplies source-level type context.
  MedVar LoadedV;
  LoadedV.Kind = MedVar::Temp;
  LoadedV.Id = 89;
  LoadedV.Size = 8;
  Load.Dst = HighExpr::makeVar(LoadedV, NdType::makeInt(8, false));
  Load.Val = HighExpr::makeLoad(Address, NdType::makeInt(8, false));
  MedVar CopyV = LoadedV;
  CopyV.Id = 90;
  HighStmt Copy;
  Copy.Kind = StmtKind::Assign;
  Copy.Dst = HighExpr::makeVar(CopyV, NdType::makeInt(8, false));
  Copy.Val = HighExpr::makeVar(LoadedV, NdType::makeInt(8, false));
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeVar(CopyV, NdType::makeInt(8, false));
  auto Null = HighExpr::makeConst(0, 8, ConstantAddressProvenance::Scalar);
  Null->Type = NdType::makePtr(NdType::makeVoid());
  HighStmt Other;
  Other.Kind = StmtKind::Return;
  Other.RetVal = Null;
  HighStmt Branch;
  Branch.Kind = StmtKind::IfElse;
  Branch.Cond = Guard;
  Branch.Body = {Other};
  Branch.ElseBody = {Load, Copy, Return};
  F.Function.Body = {Assign, Branch};
  F.Function.ReturnType = NdType::makePtr(NdType::makeVoid());
  return F;
}
} // namespace

TEST(ObjCSourceBindings,
     BoundedObjectPointerTablesRebuildConstantObjectsAndNulls) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64})
    for (bool Chained : {false, true}) {
      auto F = objectPointerTableFixture(Architecture, Chained);
      const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      ASSERT_EQ(Bound.ConstantObjectTables,
                (std::map<va_t, uint32_t>{{0x3000, 16}}));
      EXPECT_EQ(Bound.ConstantStrings, std::set<va_t>{0x2020});
      EXPECT_TRUE(Bound.ConstantObjects.empty());
      const auto &Load = Bound.Function.Body[1].ElseBody[0].Val;
      ASSERT_EQ(Load->Kind, ExprKind::Load);
      EXPECT_EQ(Load->Type->Kind, NdTypeKind::Ptr);
      const auto &Helper = Load->Operands[0]->Operands[0];
      ASSERT_TRUE(Helper->SourceCallHint);
      EXPECT_EQ(Helper->SourceCallHint->CallKind,
                SourceCallTypeHint::Kind::RuntimeConstantObjectTable);
      EXPECT_EQ(Helper->SourceCallHint->TargetAddress, 0x3000U);
      EXPECT_EQ(Helper->SourceCallHint->ByteCount, 16U);
      auto Allowed =
          readOnlyObjectPointerSourceHelpers(Bound.Function, F.Image);
      ASSERT_EQ(Allowed.size(), 1U);
      EXPECT_TRUE(objcSourceCallBound(*Helper, F.Image, {}, nullptr, &Allowed));
      EXPECT_FALSE(objcSourceCallBound(*Helper, F.Image, {}));

      std::set<std::string> Helpers;
      const auto Source = renderObjCConstantObjectTableHelpers(
          F.Image, Bound.ConstantObjectTables, Helpers);
      EXPECT_TRUE(
          Helpers.count("neverd_objc_constant_object_table_3000_address"));
      EXPECT_NE(Source.find("entries[0] = (const void *)"
                            "neverd_objc_constant_string_2020_address()"),
                std::string::npos);
      EXPECT_NE(Source.find("entries[1] = 0"), std::string::npos);
    }
}

TEST(ObjCSourceBindings,
     BoundedObjectPointerTablesRejectUnprovedSlotsAndStalePublication) {
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = objectPointerTableFixture(Arch::AArch64, true);
    auto &Branch = F.Function.Body[1];
    auto &Load = Branch.ElseBody[0].Val;
    auto &Offset = Load->Operands[0]->Operands[1];
    if (Mutation == 0)
      Branch.Cond = HighExpr::makeConst(1, 1);
    if (Mutation == 1)
      Offset->Operands[1] = HighExpr::makeConst(4, 8);
    if (Mutation == 2)
      F.Image.Segments[2].ReadOnlyAfterRelocations = false;
    if (Mutation == 3)
      F.Image.DataPtrRelocSlots.erase(0x3000);
    if (Mutation == 4)
      F.Image.DataPtrRelocTargetOwners[0x3000] = 0x1000;
    if (Mutation == 5)
      llvm::support::endian::write64le(F.Image.Segments[2].Data.data() + 8, 1);
    if (Mutation == 6)
      Load->MemoryOrdering = NdMemoryOrdering::Acquire;
    if (Mutation == 7) {
      auto &Value = Branch.ElseBody[2].RetVal;
      Value =
          HighExpr::makeBinop(NdOp::INT_ADD, Value, HighExpr::makeConst(1, 8));
    }
    if (Mutation == 8)
      Branch.ElseBody.pop_back();
    const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_TRUE(Bound.ConstantObjectTables.empty());
  }

  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = objectPointerTableFixture(Arch::X64, false);
    auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    const auto Helper =
        Bound.Function.Body[1].ElseBody[0].Val->Operands[0]->Operands[0];
    ASSERT_TRUE(Helper->SourceCallHint);
    if (Mutation == 0) {
      HighStmt Escape;
      Escape.Kind = StmtKind::Return;
      Escape.RetVal = Helper;
      Bound.Function.Body.push_back(Escape);
    }
    if (Mutation == 1) {
      auto Changed =
          std::make_shared<SourceCallTypeHint>(*Helper->SourceCallHint);
      Changed->ByteCount = 8;
      Helper->SourceCallHint = std::move(Changed);
    }
    if (Mutation == 2)
      llvm::support::endian::write64le(F.Image.Segments[2].Data.data() + 8, 1);
    const auto Allowed =
        readOnlyObjectPointerSourceHelpers(Bound.Function, F.Image);
    EXPECT_TRUE(Allowed.empty());
    EXPECT_FALSE(objcSourceCallBound(*Helper, F.Image, {}, nullptr, &Allowed));
  }
}

TEST(ObjCSourceBindings, MutableStringPointersKeepTheirSharedStorage) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64})
    for (bool Chained : {false, true}) {
      auto F = immutablePointerFixture(Architecture, Chained);
      F.Image.Segments[2].ReadOnlyAfterRelocations = false;
      F.Image.Symbols.push_back({"_mutableString", 0x3000, 8, false});
      F.Function.Body[0].RetVal->Type = NdType::makeInt(8, false);
      EXPECT_FALSE(readImmutableImagePointer(F.Image, 0x3000));
      EXPECT_EQ(readInitialImagePointer(F.Image, 0x3000), 0x2020U);
      const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      const auto Load = Bound.Function.Body[0].RetVal;
      ASSERT_EQ(Load->Kind, ExprKind::Load);
      const auto Helper = Load->Operands[0];
      ASSERT_TRUE(Helper->SourceCallHint);
      EXPECT_EQ(Helper->SourceCallHint->CallKind,
                SourceCallTypeHint::Kind::RuntimeLocalStorageAddress);
      EXPECT_EQ(Helper->SourceCallHint->TargetAddress, 0x3000U);
      EXPECT_TRUE(objcSourceCallBound(*Helper, F.Image, {}));
      EXPECT_EQ(Bound.LocalStorageExtents,
                (std::map<va_t, uint64_t>{{0x3000, 8}}));
      std::set<std::string> Helpers;
      const auto Source = renderObjCLocalStorageHelpers(
          F.Image, Bound.LocalStorageExtents, Helpers);
      EXPECT_NE(
          Source.find("storage = neverd_objc_constant_string_2020_address()"),
          std::string::npos);
      EXPECT_NE(Source.find("return (uintptr_t)&storage"), std::string::npos);
      F.Image.DataPtrRelocTargetOwners.erase(0x3000);
      EXPECT_FALSE(objcSourceCallBound(*Helper, F.Image, {}));
      EXPECT_THROW(renderObjCLocalStorageHelpers(
                       F.Image, Bound.LocalStorageExtents, Helpers),
                   std::runtime_error);
    }
}

TEST(ObjCSourceBindings,
     SelectedMutableStringCellsKeepSeparateAuthenticatedInitializers) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 3; ++Mutation) {
      SCOPED_TRACE(static_cast<unsigned>(Architecture));
      SCOPED_TRACE(Mutation);
      auto F = immutablePointerFixture(Architecture, true);
      F.Image.Segments[2].ReadOnlyAfterRelocations = false;
      F.Image.Symbols.push_back({"_firstString", 0x3000, 8, false});
      F.Image.Symbols.push_back({"_secondString", 0x3008, 8, false});
      MedVar Pointer;
      Pointer.Kind = Architecture == Arch::AArch64 ? MedVar::Reg : MedVar::Temp;
      Pointer.Id = 41;
      Pointer.Size = 8;
      Pointer.TheArch = Architecture;
      auto Var = HighExpr::makeVar(Pointer, NdType::makeInt(8, false));
      HighStmt First, Second, Return;
      First.Kind = Second.Kind = StmtKind::Assign;
      First.Dst = Second.Dst = Var;
      First.Val = HighExpr::makeConst(0x3000, 8,
                                      ConstantAddressProvenance::DataAddress);
      Second.Val = HighExpr::makeConst(0x3008, 8,
                                       ConstantAddressProvenance::DataAddress);
      Return.Kind = StmtKind::Return;
      Return.RetVal = HighExpr::makeLoad(Var, NdType::makeInt(8, false));
      F.Function.ReturnType = NdType::makeInt(8, false);
      if (Mutation == 1)
        Return.RetVal = Var;
      if (Mutation == 2)
        F.Image.DataPtrRelocTargetOwners.erase(0x3008);
      F.Function.Body = {First, Second, Return};
      const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
      if (Mutation) {
        EXPECT_FALSE(Bound.Limitation.empty());
        EXPECT_TRUE(Bound.LocalStorageExtents.empty());
        continue;
      }
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      EXPECT_EQ(Bound.LocalStorageExtents,
                (std::map<va_t, uint64_t>{{0x3000, 8}, {0x3008, 8}}));
      for (unsigned I = 0; I < 2; ++I) {
        const auto Value = Bound.Function.Body[I].Val;
        ASSERT_TRUE(Value->SourceCallHint);
        EXPECT_EQ(Value->SourceCallHint->CallKind,
                  SourceCallTypeHint::Kind::RuntimeLocalStorageAddress);
        EXPECT_TRUE(objcSourceCallBound(*Value, F.Image, {}));
      }
      std::set<std::string> Helpers;
      const auto Source = renderObjCLocalStorageHelpers(
          F.Image, Bound.LocalStorageExtents, Helpers);
      EXPECT_NE(Source.find("neverd_objc_constant_string_2020_address()"),
                std::string::npos);
    }
}

TEST(ObjCSourceBindings, MutableStringPointersRejectUnprovedInitializers) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Case = 0; Case != 13; ++Case) {
      SCOPED_TRACE(Case);
      auto F = immutablePointerFixture(Architecture, true);
      F.Image.Segments[2].ReadOnlyAfterRelocations = false;
      F.Image.Symbols.push_back({"_mutableString", 0x3000, 8, false});
      F.Function.Body[0].RetVal->Type = NdType::makeInt(8, false);
      switch (Case) {
      case 0:
        F.Image.Symbols.clear();
        break;
      case 1:
        F.Image.DataPtrRelocSlots.erase(0x3000);
        break;
      case 2:
        F.Image.DataPtrRelocTargetOwners.erase(0x3000);
        break;
      case 3:
        F.Image.DataPtrRelocTargetOwners[0x3000] = 0x1000;
        break;
      case 4:
        F.Image.MachOResolvedChainedPointerSlots.erase(0x3000);
        break;
      case 5:
        F.Image.DyldBindSlots[0x3000] = {};
        break;
      case 6:
        F.Image.DataPtrRelocSlots.insert(0x3004);
        break;
      case 7:
        F.Image.Segments[2].FileSz = 7;
        break;
      case 8:
        F.Image.Sections.push_back(F.Image.Sections[3]);
        break;
      case 9:
        F.Function.Body[0].RetVal->Type = NdType::makeInt(4);
        break;
      case 10:
        F.Function.Body[0].RetVal->Type = NdType::makeInt(16);
        break;
      case 11:
        F.Image.Symbols[0].Size = 4;
        break;
      case 12:
        llvm::support::endian::write64le(F.Image.Segments[2].Data.data(),
                                         0x2040);
        break;
      }
      const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
      EXPECT_FALSE(Bound.Limitation.empty());
      EXPECT_TRUE(Bound.LocalStorageExtents.empty());
    }
}

TEST(ObjCSourceBindings, StrongStoresAuthenticateTheirSharedPointerCell) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    auto F = immutablePointerFixture(Architecture, true);
    F.Image.Segments[2].ReadOnlyAfterRelocations = false;
    F.Image.Symbols.push_back({"_mutableString", 0x3000, 8, false});
    F.Image.recordDyldBindSlot(0x3100, "_objc_storeStrong", 0,
                               "/usr/lib/libobjc.A.dylib", false);
    F.Image.ImportPtrSlots[0x3100] = "_objc_storeStrong";
    const auto Hint = objcRuntimeSourceCallHint(F.Image, 0x3100);
    ASSERT_TRUE(Hint);
    auto Call = HighExpr::makeCall(
        "objc_storeStrong", 0x3100,
        {HighExpr::makeConst(0x3000, 8), HighExpr::makeConst(0, 8)});
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    Call->Type = NdType::makeVoid();
    F.Function.Body[0].RetVal = Call;
    auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    const auto Storage = Bound.Function.Body[0].RetVal->Operands[0];
    ASSERT_TRUE(Storage->SourceCallHint);
    EXPECT_EQ(Storage->SourceCallHint->TargetAddress, 0x3000U);
    EXPECT_EQ(Storage->SourceCallHint->ByteCount, 8U);
    EXPECT_TRUE(objcSourceCallBound(*Storage, F.Image, {}));
    auto Forged = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
    Forged->Signature.Parameters[0].Location.RegisterOffset += 8;
    Call->SourceCallHint = std::move(Forged);
    Bound = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_FALSE(
        objcSourceCallBound(*Bound.Function.Body[0].RetVal, F.Image, {}));
    EXPECT_EQ(Bound.Function.Body[0].RetVal->Operands[0]->Kind,
              ExprKind::Const);
    EXPECT_TRUE(Bound.LocalStorageExtents.empty());
  }
}

TEST(ObjCSourceBindings, ImmutablePointerLoadsShareTheTargetObjectIdentity) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64})
    for (bool Chained : {false, true}) {
      auto F = immutablePointerFixture(Architecture, Chained);
      EXPECT_EQ(readImmutableImagePointer(F.Image, 0x3000), 0x2020U);
      EXPECT_FALSE(readImmutableImageBytes(F.Image, 0x3000, 8));
      const auto Scalar = readImmutableImageBytes(F.Image, 0x3010, 8);
      ASSERT_TRUE(Scalar);
      EXPECT_EQ((*Scalar)[0], 42U);
      for (bool Reversed : {false, true}) {
        F.Function.Body.resize(1);
        for (auto Address : {0x3000U, 0x3008U, 0x2020U}) {
          HighStmt Return;
          Return.Kind = StmtKind::Return;
          Return.RetVal = HighExpr::makeConst(Address, 8);
          if (Address != 0x2020)
            Return.RetVal =
                HighExpr::makeLoad(Return.RetVal, NdType::makeInt(8));
          F.Function.Body.push_back(Return);
        }
        if (Reversed)
          std::reverse(F.Function.Body.begin(), F.Function.Body.end());
        const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
        ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
        EXPECT_EQ(Bound.ConstantStrings, std::set<va_t>{0x2020});
        for (const auto &Statement : Bound.Function.Body) {
          ASSERT_TRUE(Statement.RetVal->SourceCallHint);
          EXPECT_EQ(Statement.RetVal->SourceCallHint->TargetAddress, 0x2020U);
          EXPECT_TRUE(objcSourceCallBound(*Statement.RetVal, F.Image, {}));
        }
        EXPECT_EQ(F.Function.Body[1].RetVal->Kind, ExprKind::Load);
      }
    }
}

TEST(ObjCSourceBindings, ImmutablePointerLoadsRejectIncompleteAndStaleProofs) {
  using Mutation = std::function<void(BinaryImage &)>;
  const std::vector<Mutation> Mutations = {
      [](auto &I) { I.Segments[2].ReadOnlyAfterRelocations = false; },
      [](auto &I) {
        I.Segments[2].ReadOnlyAfterRelocations = false;
        I.Segments[2].Name = I.Sections[3].SegmentName = "__DATA_CONST";
      },
      [](auto &I) { I.Format = BinaryFormat::ELF; },
      [](auto &I) { I.IsRelocatable = true; },
      [](auto &I) { I.Bits = Bitness::Bits32; },
      [](auto &I) { I.MachOChainedFixupsAmbiguous = true; },
      [](auto &I) { I.MachOResolvedChainedPointerSlots.erase(0x3000); },
      [](auto &I) { I.DataPtrRelocSlots.erase(0x3000); },
      [](auto &I) { I.DataPtrRelocTargetOwners.erase(0x3000); },
      [](auto &I) { I.DataPtrRelocTargetOwners[0x3000] = 0x2020; },
      [](auto &I) { I.DataPtrRelocTargetOwners[0x3000] = 0x1000; },
      [](auto &I) { I.DataPtrRelocSlots.insert(0x2ff9); },
      [](auto &I) { I.DataPtrRelocSlots.insert(0x3007); },
      [](auto &I) { I.MachOResolvedChainedPointerSlots.insert(0x3001); },
      [](auto &I) { I.CodePtrRelocSlots.insert(0x3000); },
      [](auto &I) { I.RelDataPtrRelocSlots.insert(0x3000); },
      [](auto &I) { I.RelCodeRelocSlots.insert(0x3000); },
      [](auto &I) { I.ConflictingImportStorageSlots.insert(0x3000); },
      [](auto &I) { I.ImportPtrSlots[0x3000] = "_other"; },
      [](auto &I) { I.ImportStorageSlots[0x3000] = {}; },
      [](auto &I) { I.DyldBindSlots[0x3000] = {}; },
      [](auto &I) { I.ObjCSourceReferences[0x3000] = {}; },
      [](auto &I) { I.DataAddressRelocOperands[0x3000] = {}; },
      [](auto &I) { I.CodeAddressRelocOperands[0x3000] = {}; },
      [](auto &I) { I.Relocations.push_back({0x3000}); },
      [](auto &I) { I.BaseRelocations.push_back({0x3000}); },
      [](auto &I) { I.Sections[3].Flags = SegmentFlags::None; },
      [](auto &I) { I.Segments[2].Flags = SegmentFlags::None; },
      [](auto &I) { I.Sections[3].FileSz = 7; },
      [](auto &I) { I.Segments[2].FileSz = 7; },
      [](auto &I) { I.Segments[2].Data.resize(7); },
      [](auto &I) { I.Sections[3].FileOff++; },
      [](auto &I) { I.Sections[3].Type = llvm::MachO::S_ZEROFILL; },
      [](auto &I) {
        I.Sections[3].Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
      },
      [](auto &I) { I.Sections.push_back(I.Sections[3]); },
      [](auto &I) { I.Segments.push_back(I.Segments[2]); },
      [](auto &I) { I.Sections.push_back(I.Sections[0]); },
      [](auto &I) { I.Segments.push_back(I.Segments[0]); },
      [](auto &I) { I.Segments[2].Data[0] = 0x40; },
      [](auto &I) { I.Segments[2].Data[0] = 0x21; },
      [](auto &I) { I.Segments[0].Data[40] = 0; },
      [](auto &I) {
        I.Raw.resize(32);
        llvm::support::endian::write32le(I.Raw.data(),
                                         llvm::MachO::MH_MAGIC_64);
        llvm::support::endian::write32le(I.Raw.data() + 8,
                                         llvm::MachO::CPU_SUBTYPE_ARM64E);
      },
  };
  for (size_t Index = 0; Index < Mutations.size(); ++Index) {
    SCOPED_TRACE(Index);
    auto F = immutablePointerFixture(Arch::AArch64, true);
    const auto Before = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Before.Limitation.empty());
    Mutations[Index](F.Image);
    // Projection may encounter a different runtime-reference binding or
    // defer an unmapped address to source validation. Neither can authorize
    // the constant object supplied by this immutable-pointer proof.
    EXPECT_TRUE(
        bindObjCSourceReferences(F.Function, F.Image).ConstantStrings.empty());
    EXPECT_FALSE(
        objcSourceCallBound(*Before.Function.Body[0].RetVal, F.Image, {}));
  }
  auto F = immutablePointerFixture(Arch::X64, false);
  const auto Before = bindObjCSourceReferences(F.Function, F.Image);
  // A new valid target in the same owner range must invalidate the old hint.
  llvm::support::endian::write64le(F.Image.Segments[2].Data.data(), 0x2000);
  EXPECT_FALSE(
      objcSourceCallBound(*Before.Function.Body[0].RetVal, F.Image, {}));
  const auto After = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_TRUE(After.Limitation.empty());
  EXPECT_EQ(After.ConstantStrings, std::set<va_t>{0x2000});
}

TEST(ObjCSourceBindings, ImmutablePointerProofAppliesOnlyToOrdinaryFullLoads) {
  for (unsigned Variant = 0; Variant < 6; ++Variant) {
    SCOPED_TRACE(Variant);
    auto F = immutablePointerFixture(Arch::AArch64, true);
    auto &Load = F.Function.Body[0].RetVal;
    if (Variant == 0)
      Load = Load->Operands[0];
    if (Variant == 1)
      Load->Type = NdType::makeInt(4);
    if (Variant == 2)
      Load->Type = NdType::makeFloat(8);
    if (Variant == 3)
      Load->MemoryOrdering = NdMemoryOrdering::Acquire;
    if (Variant == 4)
      Load->MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
    if (Variant == 5)
      Load = HighExpr::makeLoad(Load, NdType::makeInt(8));
    EXPECT_FALSE(
        bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
  }
}

namespace {
struct ConstantObjectFixture : ConstantStringFixture {
  void storage(va_t Address, size_t Size, const char *Name) {
    Segment S;
    S.Name = "__DATA_CONST";
    S.VA = S.FileOff = Address;
    S.Size = S.FileSz = Size;
    S.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    S.ReadOnlyAfterRelocations = true;
    S.Data.resize(Size);
    Image.Segments.push_back(S);
    Section R;
    R.Name = Name;
    R.SegmentName = S.Name;
    R.VA = R.FileOff = Address;
    R.Size = R.FileSz = Size;
    R.Flags = S.Flags;
    Image.Sections.push_back(R);
  }
  void word(va_t Address, uint64_t Value) {
    for (auto &S : Image.Segments)
      if (Address >= S.VA && Address - S.VA < S.Size) {
        llvm::support::endian::write64le(S.Data.data() + Address - S.VA, Value);
        return;
      }
    FAIL() << "unmapped fixture address";
  }
  void pointer(va_t Slot, va_t Target) {
    word(Slot, Target);
    Image.DataPtrRelocSlots.insert(Slot);
    Image.DataPtrRelocTargetOwners[Slot] = Image.getSectionFor(Target)->VA;
    Image.MachOResolvedChainedPointerSlots.insert(Slot);
  }
  ConstantObjectFixture(Arch Architecture = Arch::AArch64,
                        bool Chained = false) {
    Image.Arch = Architecture;
    Image.MachOHasChainedFixups = Chained;
    Image.MachOResolvedChainedPointerSlots = {0x2000, 0x2010, 0x2020, 0x2030};
    pointer(0x2010, 0x1000);
    pointer(0x2030, 0x1100);
    storage(0x3000, 48, "__objc_arrayobj");
    storage(0x4000, 48, "__objc_intobj");
    storage(0x5000, 40, "__objc_dictobj");
    storage(0x6000, 512, "__const");
    for (auto Address : {0x3000U, 0x3018U, 0x4000U, 0x4018U, 0x5000U}) {
      const char *Name = Address < 0x4000 ? "_OBJC_CLASS_$_NSConstantArray"
                         : Address < 0x5000
                             ? "_OBJC_CLASS_$_NSConstantIntegerNumber"
                             : "_OBJC_CLASS_$_NSConstantDictionary";
      EXPECT_TRUE(Image.recordDyldBindSlot(
          Address, Name, 0,
          "/System/Library/Frameworks/Foundation.framework/Foundation", false));
      Image.MachOResolvedChainedPointerSlots.insert(Address);
    }
    Image.Segments[1].Data[8] = 'q';
    Image.Segments[1].Data[10] = 'Q';
    pointer(0x4008, 0x1008);
    word(0x4010, uint64_t(-12345));
    pointer(0x4020, 0x100a);
    word(0x4028, UINT64_C(0xfedcba9876543210));
    word(0x3008, 3);
    pointer(0x3010, 0x6000);
    pointer(0x6000, 0x2000);
    pointer(0x6008, 0x2020);
    pointer(0x6010, 0x2000);
    word(0x3020, 3);
    pointer(0x3028, 0x6018);
    pointer(0x6018, 0x3000);
    pointer(0x6020, 0x4000);
    pointer(0x6028, 0x5000);
    word(0x5008, 1);
    word(0x5010, 2);
    pointer(0x5018, 0x6030);
    pointer(0x6030, 0x2000);
    pointer(0x6038, 0x2020);
    pointer(0x5020, 0x6040);
    pointer(0x6040, 0x3000);
    pointer(0x6048, 0x4000);
    pointer(0x6050, 0x3018);
    Function.Body[0].RetVal = HighExpr::makeConst(0x3018, 8);
  }
};
} // namespace

TEST(ObjCSourceBindings, ConstantObjectGraphsPreserveNestedAliasesAndStrings) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Chained : {false, true}) {
      ConstantObjectFixture F(Architecture, Chained);
      const auto Graph = readObjCConstantObjectGraph(F.Image, 0x3018);
      ASSERT_TRUE(Graph);
      EXPECT_EQ(Graph->size(), 6U);
      EXPECT_EQ(Graph->at(0x3000).Elements,
                (std::vector<va_t>{0x2000, 0x2020, 0x2000}));
      EXPECT_EQ(Graph->at(0x5000).Keys, (std::vector<va_t>{0x2000, 0x2020}));
      EXPECT_EQ(Graph->at(0x5000).Elements,
                (std::vector<va_t>{0x3000, 0x4000}));
      EXPECT_EQ(Graph->at(0x4000).Encoding, 'q');
      EXPECT_EQ(Graph->at(0x4000).Bits, uint64_t(-12345));
      EXPECT_EQ(Graph->at(0x2020).String.Units,
                (std::vector<uint16_t>{0x767e, 0, 0xd83d, 0xde00}));
      const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      EXPECT_EQ(Bound.ConstantObjects, std::set<va_t>{0x3018});
      EXPECT_TRUE(
          objcSourceCallBound(*Bound.Function.Body[0].RetVal, F.Image, {}));
      std::set<std::string> Shared;
      const auto Source = renderObjCConstantObjectHelpers(
          F.Image, {0x3018, 0x4018}, {0x2000}, Shared);
      EXPECT_EQ(Shared.size(), 8U);
      EXPECT_EQ(Shared.count("neverd_cstring_storage_1000_address"), 1U);
      EXPECT_EQ(Shared.count("neverd_objc_constant_string_2000_address"), 1U);
      EXPECT_NE(Source.find("UINT64_C(0xfedcba9876543210)"), std::string::npos);
      EXPECT_EQ(F.Function.Body[0].RetVal->Kind, ExprKind::Const);
    }
}

TEST(ObjCSourceBindings,
     ConstantObjectGraphsRejectConflictingOrMutableStorage) {
  using Mutation = std::function<void(ConstantObjectFixture &)>;
  const std::vector<Mutation> Mutations = {
      [](auto &F) { F.Image.DyldBindSlots.at(0x3018).WeakImport = true; },
      [](auto &F) { F.Image.DyldBindSlots.at(0x3018).Addend = 8; },
      [](auto &F) {
        F.Image.DyldBindSlots.at(0x3018).Module = "/tmp/Foundation";
      },
      [](auto &F) { F.Image.DyldBindSlots.at(0x3018).Name = "_unknown"; },
      [](auto &F) { F.Image.ImportStorageSlots.at(0x3018).Addend = 8; },
      [](auto &F) { F.Image.ImportPtrSlots[0x3018] = "_unknown"; },
      [](auto &F) { F.Image.CodePtrRelocSlots.insert(0x3014); },
      [](auto &F) { F.Image.RelDataPtrRelocSlots.insert(0x3018); },
      [](auto &F) { F.Image.DataPtrRelocSlots.insert(0x3018); },
      [](auto &F) { F.Image.DataPtrRelocTargetOwners[0x3018] = 0x3000; },
      [](auto &F) {
        F.Image.DyldBindSlots[0x3017] = F.Image.DyldBindSlots.at(0x3018);
      },
      [](auto &F) { F.Image.ConflictingImportStorageSlots.insert(0x301a); },
      [](auto &F) { F.Image.Relocations.push_back({0x3018}); },
      [](auto &F) { F.Image.BaseRelocations.push_back({0x3014}); },
      [](auto &F) { F.Image.RuntimeCallablePointerSlots.push_back({0x3014}); },
      [](auto &F) { F.Image.DyldBindSlots.erase(0x3018); },
      [](auto &F) { F.Image.MachOResolvedChainedPointerSlots.erase(0x6028); },
      [](auto &F) { F.Image.DataPtrRelocTargetOwners.erase(0x6028); },
      [](auto &F) { F.Image.DataPtrRelocTargetOwners[0x6028] = 0x4000; },
      [](auto &F) { F.Image.ImportPtrSlots[0x6028] = "_foreignObject"; },
      [](auto &F) { F.Image.Segments[2].ReadOnlyAfterRelocations = false; },
      [](auto &F) { F.Image.Segments[5].ReadOnlyAfterRelocations = false; },
      [](auto &F) { F.Image.Sections[3].Type = llvm::MachO::S_ZEROFILL; },
      [](auto &F) { F.Image.Sections[3].FileSz = 47; },
      [](auto &F) { F.Image.Sections[3].FileOff++; },
      [](auto &F) { F.Image.Sections.push_back(F.Image.Sections[3]); },
      [](auto &F) { F.Image.Segments.push_back(F.Image.Segments[2]); },
      [](auto &F) { F.word(0x3020, UINT64_MAX); },
      [](auto &F) { F.word(0x3028, 0x6001); },
      [](auto &F) { F.pointer(0x6028, 0x3018); }, // Cycle through the root.
      [](auto &F) { F.word(0x6028, 0x7000); },
      [](auto &F) { F.word(0x5008, 3); },
      [](auto &F) {
        F.pointer(0x6030, 0x2020);
        F.pointer(0x6038, 0x2000);
      },
      [](auto &F) { F.pointer(0x6030, 0x4000); },
      [](auto &F) {
        F.Image.Segments[1].Data[0x107] = 0xd8;
      }, // Lone surrogate.
      [](auto &F) { F.Image.Segments[1].Data[8] = 'f'; },
      [](auto &F) { F.Image.Segments[1].Data[9] = 'q'; },
      [](auto &F) {
        F.Image.Segments[1].Data[8] = 'I';
      }, // Invalid zero extension.
      [](auto &F) { F.Image.MachOChainedFixupsAmbiguous = true; },
      [](auto &F) { F.Image.IsRelocatable = true; },
      [](auto &F) { F.Image.DataPtrRelocTargetOwners.erase(0x2010); },
      [](auto &F) { F.Image.CodePtrRelocSlots.insert(0x1fff); },
      [](auto &F) {
        F.Image.Segments[0].Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
      },
  };
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (size_t I = 0; I < Mutations.size(); ++I) {
      SCOPED_TRACE(I);
      ConstantObjectFixture F(Architecture, true);
      Mutations[I](F);
      EXPECT_FALSE(readObjCConstantObjectGraph(F.Image, 0x3018));
      EXPECT_FALSE(
          bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
    }
}

TEST(ObjCSourceBindings,
     ConstantObjectGraphsBoundDepthAndPreserveSharedFanout) {
  // Visit a shared suffix first through a short edge, then through a longer
  // prefix. Reusing its proof must not hide the complete longest path.
  for (unsigned Count : {63U, 64U}) {
    ConstantObjectFixture F;
    F.storage(0x7000, Count * 24, "__objc_arrayobj");
    F.storage(0x9000, Count * 8, "__const");
    F.word(0x3008, 2);
    F.pointer(0x6000, 0x7000);
    F.pointer(0x6008, 0x7000 + 32 * 24);
    for (unsigned I = 0; I < Count; ++I) {
      const va_t Address = 0x7000 + I * 24;
      ASSERT_TRUE(F.Image.recordDyldBindSlot(
          Address, "_OBJC_CLASS_$_NSConstantArray", 0,
          "/System/Library/Frameworks/Foundation.framework/Foundation", false));
      F.word(Address + 8, 1);
      F.pointer(Address + 16, 0x9000 + I * 8);
      F.pointer(0x9000 + I * 8, I == 31          ? 0x2000
                                : I + 1 == Count ? 0x7000
                                                 : Address + 24);
    }
    EXPECT_EQ(bool(readObjCConstantObjectGraph(F.Image, 0x3000)), Count == 63);
    F.pointer(0x6000, 0x7000 + 32 * 24);
    F.pointer(0x6008, 0x7000);
    EXPECT_EQ(bool(readObjCConstantObjectGraph(F.Image, 0x3000)), Count == 63);
  }
  for (unsigned Depth : {64U, 65U}) {
    ConstantObjectFixture F;
    F.storage(0x7000, Depth * 24, "__objc_arrayobj");
    F.storage(0x9000, Depth * 8, "__const");
    for (unsigned I = 0; I < Depth; ++I) {
      const va_t Address = 0x7000 + I * 24;
      ASSERT_TRUE(F.Image.recordDyldBindSlot(
          Address, "_OBJC_CLASS_$_NSConstantArray", 0,
          "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",
          false));
      F.word(Address + 8, 1);
      F.pointer(Address + 16, 0x9000 + I * 8);
      F.pointer(0x9000 + I * 8, I + 1 == Depth ? 0x2000 : Address + 24);
    }
    const auto Graph = readObjCConstantObjectGraph(F.Image, 0x7000);
    EXPECT_EQ(bool(Graph), Depth == 64);
    if (Graph)
      EXPECT_EQ(Graph->size(), 65U);
  }
  for (unsigned Count : {16384U, 16385U}) {
    ConstantObjectFixture F;
    F.storage(0xa000, Count * 8, "__const");
    F.word(0x3008, Count);
    F.pointer(0x3010, 0xa000);
    for (unsigned I = 0; I < Count; ++I)
      F.pointer(0xa000 + I * 8, 0x2000);
    const auto Graph = readObjCConstantObjectGraph(F.Image, 0x3000);
    EXPECT_EQ(bool(Graph), Count == 16384);
    if (Graph) {
      EXPECT_EQ(Graph->size(), 2U);
      EXPECT_EQ(Graph->at(0x3000).Elements.size(), Count);
    }
  }
}

namespace {
ConstantObjectFixture booleanConstantObjectFixture(Arch Architecture,
                                                   bool Chained) {
  ConstantObjectFixture F(Architecture, Chained);
  for (va_t Slot : {0x6000U, 0x6008U, 0x6010U}) {
    F.Image.DataPtrRelocSlots.erase(Slot);
    F.Image.DataPtrRelocTargetOwners.erase(Slot);
    const char *Name =
        Slot == 0x6008 ? "___kCFBooleanFalse" : "___kCFBooleanTrue";
    EXPECT_TRUE(F.Image.recordDyldBindSlot(
        Slot, Name, 0,
        "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",
        false));
    // Chained binds carry symbolic loader authority, not a resolved local
    // pointer and not necessarily a legacy GOT pointer-table entry.
    F.Image.ImportPtrSlots.erase(Slot);
    F.Image.MachOResolvedChainedPointerSlots.erase(Slot);
  }
  return F;
}
} // namespace

TEST(ObjCSourceBindings, ConstantObjectsPreserveImportedBooleanIdentities) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Chained : {false, true}) {
      auto F = booleanConstantObjectFixture(Architecture, Chained);
      const auto Graph = readObjCConstantObjectGraph(F.Image, 0x3000);
      ASSERT_TRUE(Graph);
      EXPECT_EQ(Graph->size(), 4U);
      EXPECT_EQ(Graph->at(0x3000).Elements,
                (std::vector<va_t>{0x6000, 0x6008, 0x6010}));
      EXPECT_EQ(Graph->at(0x6000).TheKind,
                ObjCConstantObject::Kind::ImportedBoolean);
      EXPECT_EQ(Graph->at(0x6000).ImportName, "___kCFBooleanTrue");
      EXPECT_EQ(Graph->at(0x6010).ImportName, "___kCFBooleanTrue");
      EXPECT_EQ(Graph->at(0x6008).ImportName, "___kCFBooleanFalse");
      EXPECT_TRUE(isImmutableImageImportSlot(F.Image, 0x6000));
      EXPECT_FALSE(readImmutableImageBytes(F.Image, 0x6000, 8));
      EXPECT_FALSE(readImmutableImagePointer(F.Image, 0x6000));
      EXPECT_FALSE(readObjCConstantObjectGraph(F.Image, 0x6000));
      F.Function.Body[0].RetVal = HighExpr::makeConst(0x3000, 8);
      const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      EXPECT_TRUE(
          objcSourceCallBound(*Bound.Function.Body[0].RetVal, F.Image, {}));
      F.Image.DyldBindSlots.at(0x6000).WeakImport = true;
      EXPECT_FALSE(
          objcSourceCallBound(*Bound.Function.Body[0].RetVal, F.Image, {}));
    }
}

TEST(ObjCSourceBindings, ImportedBooleanEdgesRejectAliasesAndStaleImports) {
  using Mutation = std::function<void(ConstantObjectFixture &)>;
  const std::vector<Mutation> Mutations{
      [](auto &F) { F.Image.DyldBindSlots.at(0x6000).WeakImport = true; },
      [](auto &F) { F.Image.DyldBindSlots.at(0x6000).Addend = 8; },
      [](auto &F) { F.Image.DyldBindSlots.at(0x6000).Module = "/tmp/fake"; },
      [](auto &F) { F.Image.ImportPtrSlots[0x6000] = "_kCFBooleanTrue"; },
      [](auto &F) { F.Image.ImportStorageSlots.at(0x6000).Addend = 8; },
      [](auto &F) { F.Image.DyldBindSlots.erase(0x6000); },
      [](auto &F) { F.Image.DataPtrRelocSlots.insert(0x6000); },
      [](auto &F) { F.Image.DataPtrRelocTargetOwners[0x6000] = 0x6000; },
      [](auto &F) { F.Image.CodePtrRelocSlots.insert(0x5ffc); },
      [](auto &F) { F.Image.ImportStorageSlots[0x6004] = {"_other", 0}; },
      [](auto &F) { F.Image.MachOResolvedChainedPointerSlots.insert(0x6004); },
      [](auto &F) { F.Image.ConflictingImportStorageSlots.insert(0x6000); },
      [](auto &F) { F.Image.Segments.back().ReadOnlyAfterRelocations = false; },
      [](auto &F) { F.Image.Sections.back().FileSz = 7; },
      [](auto &F) { F.Image.Sections.push_back(F.Image.Sections.back()); },
      [](auto &F) { F.Image.Relocations.push_back({0x6000}); },
      [](auto &F) { F.Image.BaseRelocations.push_back({0x5ffc}); },
      [](auto &F) { F.Image.RuntimeCallablePointerSlots.push_back({0x6000}); },
      [](auto &F) {
        F.pointer(0x6020, 0x6000);
      }, // Address of the slot is not its value.
      [](auto &F) {
        F.pointer(0x6030, 0x3000);
      }, // Boolean arrays are not string keys.
  };
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (size_t I = 0; I < Mutations.size(); ++I) {
      SCOPED_TRACE(I);
      auto F = booleanConstantObjectFixture(Architecture, true);
      Mutations[I](F);
      EXPECT_FALSE(readObjCConstantObjectGraph(F.Image, 0x3018));
    }
}

TEST(ObjCSourceBindings, ImportedBooleanElementsExecuteWithSharedIdentity) {
  auto F = booleanConstantObjectFixture(Arch::AArch64, true);
  std::set<std::string> Shared;
  std::string Source =
      "#include <stdint.h>\n" +
      renderObjCConstantObjectHelpers(F.Image, {0x3000}, {}, Shared);
  Source += R"(
const unsigned char true_object[1] __asm__("___kCFBooleanTrue") = { 1 };
const unsigned char false_object[1] __asm__("___kCFBooleanFalse") = { 0 };
unsigned char array_class[1] __asm__("_OBJC_CLASS_$_NSConstantArray") = { 0 };
struct array_record { const void *isa; uintptr_t count; const void *const *elements; };
int main(void) {
  const struct array_record *a = (const void *)neverd_objc_constant_object_3000_address();
  if (a->isa != array_class || a->count != 3) return 1;
  if (a->elements[0] != true_object || a->elements[1] != false_object ||
      a->elements[2] != true_object) return 2;
  if (a != (const void *)neverd_objc_constant_object_3000_address()) return 3;
  return 0;
}
)";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-boolean-objects",
                                                    Directory));
  const std::filesystem::path Work(Directory.str().str());
  struct Cleanup {
    std::filesystem::path Work;
    ~Cleanup() {
      std::error_code Error;
      std::filesystem::remove_all(Work, Error);
    }
  } Cleanup{Work};
  const auto Path = (Work / "objects.c").string();
  const auto Executable = (Work / "objects").string();
  const auto ErrorPath = (Work / "stderr").string();
  std::ofstream(Path) << Source;
  const std::string Compiler = NEVERD_TEST_CLANG;
  for (const char *Optimization : {"-O0", "-O2"}) {
    const std::vector<std::string> Arguments{
        Compiler, "-std=c11", Optimization, "-Werror", Path, "-o", Executable};
    std::vector<llvm::StringRef> Refs(Arguments.begin(), Arguments.end());
    const std::optional<llvm::StringRef> Redirects[] = {
        std::nullopt, std::nullopt, ErrorPath};
    std::string Error;
    const auto Status = llvm::sys::ExecuteAndWait(Compiler, Refs, std::nullopt,
                                                  Redirects, 60, 0, &Error);
    auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Status, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "");
    EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable}, std::nullopt,
                                        Redirects, 30, 0, &Error),
              0)
        << Error;
  }
}

TEST(ObjCSourceBindings, ConstantIntegerObjectsRetainSignednessAndExactBits) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (char Encoding : std::string("cCsSiIlLqQ")) {
      const unsigned Width = Encoding == 'c' || Encoding == 'C'   ? 8
                             : Encoding == 's' || Encoding == 'S' ? 16
                             : Encoding == 'i' || Encoding == 'I' ? 32
                                                                  : 64;
      const bool Signed = Encoding >= 'a' && Encoding <= 'z';
      const uint64_t Mask =
          Width == 64 ? UINT64_MAX : (UINT64_C(1) << Width) - 1;
      for (uint64_t Bits :
           {UINT64_C(0), UINT64_C(1), UINT64_C(1) << (Width - 1), Mask}) {
        ConstantObjectFixture F(Architecture);
        F.Image.Segments[1].Data[8] = Encoding;
        const uint64_t Extended =
            Signed && (Bits & (UINT64_C(1) << (Width - 1))) ? Bits | ~Mask
                                                            : Bits;
        F.word(0x4010, Extended);
        const auto Graph = readObjCConstantObjectGraph(F.Image, 0x4000);
        ASSERT_TRUE(Graph);
        EXPECT_EQ(Graph->at(0x4000).Bits, Extended);
        EXPECT_EQ(Graph->at(0x4000).Encoding, Encoding);
        if (Width < 64) {
          F.word(0x4010, Extended ^ (UINT64_C(1) << Width));
          EXPECT_FALSE(readObjCConstantObjectGraph(F.Image, 0x4000));
        }
      }
    }
}

TEST(ObjCSourceBindings,
     ConstantObjectBindingsRevalidateSlotsAndRejectExtraEffects) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    ConstantObjectFixture F(Architecture, true);
    F.Function.Body[0].RetVal = HighExpr::makeLoad(
        HighExpr::makeConst(0x6050, 8), NdType::makePtr(NdType::makeVoid()));
    auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    const auto Call = Bound.Function.Body[0].RetVal;
    ASSERT_TRUE(Call->SourceCallHint);
    EXPECT_EQ(Call->SourceCallHint->ImmutablePointerSlot, 0x6050U);
    EXPECT_TRUE(objcSourceCallBound(*Call, F.Image, {}));
    for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
      SCOPED_TRACE(Mutation);
      auto Changed = *Call;
      auto Hint = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
      Changed.SourceCallHint = Hint;
      auto &H = *Hint;
      switch (Mutation) {
      case 0:
        H.ByteCount = 8;
        break;
      case 1:
        H.BorrowedByteInputs = {{0, 1}};
        break;
      case 2:
        H.DoesNotReturn = true;
        break;
      case 3:
        H.ReturnedArgument = 0;
        break;
      case 4:
        H.ImmutablePointerSlot = 0x6058;
        break;
      case 5:
        H.TargetAddress = 0x2000;
        break;
      case 6:
        H.TargetName = "forged";
        break;
      case 7:
        H.SelectorReferenceAddress = 0x1000;
        break;
      case 8:
        Changed.IsIndirectCall = true;
        break;
      case 9:
        Changed.MemoryOrdering = NdMemoryOrdering::Acquire;
        break;
      case 10:
        Changed.Operands.push_back(HighExpr::makeConst(0, 8));
        break;
      case 11:
        H.Signature.ReturnType = NdType::makeInt(8);
        break;
      }
      EXPECT_FALSE(objcSourceCallBound(Changed, F.Image, {}));
    }
    F.pointer(0x6050, 0x3000);
    EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {}));
    F.Function.Body[0].RetVal =
        HighExpr::makeLoad(HighExpr::makeConst(0x3018, 8), NdType::makeInt(8));
    EXPECT_FALSE(
        bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
    F.Function.ReturnType = NdType::makeInt(8);
    auto Scalar = HighExpr::makeConst(0x3018, 8);
    Scalar->ConstProvenance = ConstantAddressProvenance::Scalar;
    F.Function.Body[0].RetVal = Scalar;
    Bound = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_TRUE(Bound.ConstantObjects.empty());
    EXPECT_EQ(Bound.Function.Body[0].RetVal->Kind, ExprKind::Const);
  }
}

TEST(ObjCSourceBindings, ConstantStringsKeepBytesUnicodeAndObjectIdentity) {
  ConstantStringFixture F;
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    F.Image.Arch = Architecture;
    for (bool Chained : {false, true}) {
      F.Image.MachOHasChainedFixups = Chained;
      F.Image.MachOResolvedChainedPointerSlots = {0x2000, 0x2010, 0x2020,
                                                  0x2030};
      auto A = readObjCConstantString(F.Image, 0x2000);
      auto U = readObjCConstantString(F.Image, 0x2020);
      ASSERT_TRUE(A && U);
      EXPECT_FALSE(A->UTF16);
      EXPECT_TRUE(U->UTF16);
      EXPECT_EQ(A->ContentsAddress, 0x1000U);
      EXPECT_EQ(U->ContentsAddress, 0x1100U);
      EXPECT_EQ(A->Units, (std::vector<uint16_t>{'a', '\n', '"'}));
      EXPECT_EQ(U->Units, (std::vector<uint16_t>{0x767e, 0, 0xd83d, 0xde00}));
      auto Result = bindObjCSourceReferences(F.Function, F.Image);
      EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
      EXPECT_EQ(Result.ConstantStrings, std::set<va_t>{0x2000});
      const auto Call = Result.Function.Body[0].RetVal;
      ASSERT_TRUE(Call->SourceCallHint);
      EXPECT_TRUE(objcSourceCallBound(*Call, F.Image, {}));
      std::set<std::string> Shared;
      const auto Helpers =
          renderObjCConstantStringHelpers(F.Image, {0x2000, 0x2020}, Shared);
      EXPECT_EQ(Shared.size(), 3U);
      EXPECT_NE(Helpers.find("a\\012\\042\\000"), std::string::npos);
      EXPECT_NE(Helpers.find("30334, 0, 55357, 56832, 0"), std::string::npos);
      EXPECT_EQ(F.Function.Body[0].RetVal->Kind, ExprKind::Const);
    }
  }
}

TEST(ObjCSourceBindings, ConstantStringsRejectUnprovedRecordsAndContents) {
  using Mutation = std::function<void(BinaryImage &)>;
  const std::vector<Mutation> Mutations = {
      [](auto &I) { I.IsRelocatable = true; },
      [](auto &I) { I.MachOChainedFixupsAmbiguous = true; },
      [](auto &I) {
        I.Raw.resize(32);
        llvm::support::endian::write32le(I.Raw.data(),
                                         llvm::MachO::MH_MAGIC_64);
        llvm::support::endian::write32le(I.Raw.data() + 8,
                                         llvm::MachO::CPU_SUBTYPE_ARM64E);
      },
      [](auto &I) { I.DyldBindSlots.clear(); },
      [](auto &I) { I.DyldBindSlots.at(0x2000).Name = "_otherClass"; },
      [](auto &I) { I.DyldBindSlots.at(0x2000).Addend = 1; },
      [](auto &I) { I.DyldBindSlots.at(0x2000).WeakImport = true; },
      [](auto &I) { I.ConflictingImportStorageSlots.insert(0x2010); },
      [](auto &I) { I.ImportStorageSlots.at(0x2000).Addend = 8; },
      [](auto &I) { I.ImportPtrSlots[0x2010] = "_otherData"; },
      [](auto &I) { I.MachOHasChainedFixups = true; },
      [](auto &I) { I.DataPtrRelocSlots.insert(0x2008); },
      [](auto &I) { I.CodePtrRelocSlots.insert(0x2010); },
      [](auto &I) { I.DataPtrRelocSlots.insert(0xfff); },
      [](auto &I) { I.DataPtrRelocSlots.insert(0x1002); },
      [](auto &I) { I.Sections[0].Size = 63; },
      [](auto &I) { I.Sections[0].FileSz = 31; },
      [](auto &I) { I.Sections[0].FileOff = 1; },
      [](auto &I) { I.Sections[1].FileSz = 2; },
      [](auto &I) {
        I.Sections[1].Type |= llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
      },
      [](auto &I) {
        I.Segments[1].Flags = I.Segments[1].Flags | SegmentFlags::Writable;
      },
      [](auto &I) {
        I.Sections[1].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      },
      [](auto &I) { I.Sections.push_back(I.Sections[1]); },
      [](auto &I) { I.Segments.push_back(I.Segments[1]); },
      [](auto &I) { I.Segments[0].Data[8] = 0xc9; },
      [](auto &I) { I.Segments[0].Data[12] = 1; },
      [](auto &I) { I.Segments[0].Data[31] = 0xff; },
      [](auto &I) { I.Segments[1].Data[3] = 1; },
      [](auto &I) { I.Segments[1].Data[0] = 0x80; },
  };
  for (size_t Index = 0; Index < Mutations.size(); ++Index) {
    SCOPED_TRACE(Index);
    ConstantStringFixture F;
    Mutations[Index](F.Image);
    EXPECT_FALSE(readObjCConstantString(F.Image, 0x2000));
    EXPECT_FALSE(
        bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
  }
  ConstantStringFixture F;
  EXPECT_FALSE(readObjCConstantString(F.Image, 0x2008));
  EXPECT_FALSE(readObjCConstantString(F.Image, 0x2040));
}

TEST(ObjCSourceBindings, ConstantStringObjectsCannotAuthorizeRawMemoryAccess) {
  ConstantStringFixture F;
  auto Address = F.Function.Body[0].RetVal;
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr = Address;
  Store.StoreVal = HighExpr::makeConst(0, 8);
  F.Function.Body.insert(F.Function.Body.begin(), Store);
  for (bool Reversed : {false, true}) {
    if (Reversed)
      std::reverse(F.Function.Body.begin(), F.Function.Body.end());
    EXPECT_FALSE(
        bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
  }
  F.Function.Body.resize(1);
  F.Function.Body[0].Kind = StmtKind::Return;
  F.Function.Body[0].RetVal = HighExpr::makeLoad(Address, NdType::makeInt(8));
  EXPECT_FALSE(
      bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
}

TEST(ObjCSourceBindings, ConstantStringPointersRetainStoredAddressProvenance) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (auto Provenance : {ConstantAddressProvenance::Address,
                            ConstantAddressProvenance::DataAddress}) {
      for (bool ExpressionStore : {false, true}) {
        ConstantStringFixture F;
        F.Image.Arch = Architecture;
        auto Value = HighExpr::makeConst(0x2000, 8, Provenance);
        auto Destination =
            HighExpr::makeVar(MedVar{.Kind = MedVar::Param, .Size = 8});
        HighStmt Store;
        if (ExpressionStore) {
          Store.Kind = StmtKind::ExprStmt;
          Store.Val = std::make_shared<HighExpr>();
          Store.Val->Kind = ExprKind::Store;
          Store.Val->Operands = {Destination, Value};
        } else {
          Store.Kind = StmtKind::Store;
          Store.StoreAddr = Destination;
          Store.StoreVal = Value;
        }
        F.Function.Body.insert(F.Function.Body.begin(), Store);
        auto Result = bindObjCSourceReferences(F.Function, F.Image);
        ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
        EXPECT_EQ(Result.ConstantStrings, std::set<va_t>{0x2000});
        const auto Bound = ExpressionStore
                               ? Result.Function.Body[0].Val->Operands[1]
                               : Result.Function.Body[0].StoreVal;
        ASSERT_TRUE(Bound && Bound->SourceCallHint);
        EXPECT_TRUE(objcSourceCallBound(*Bound, F.Image, {}));
        EXPECT_EQ(Value->Kind, ExprKind::Const);
        EXPECT_EQ(Value->ConstProvenance, Provenance);
        // A shared node used to access the object's private bytes is still
        // unbound, independently of which occurrence is visited first.
        HighStmt Read;
        Read.Kind = StmtKind::Return;
        Read.RetVal = HighExpr::makeLoad(Value, NdType::makeInt(8));
        F.Function.Body.push_back(Read);
        for (bool Reversed : {false, true}) {
          if (Reversed)
            std::reverse(F.Function.Body.begin(), F.Function.Body.end());
          EXPECT_FALSE(
              bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
        }
      }
    }
  }
}

TEST(ObjCSourceBindings, StoredStringsRejectIncompleteAndNumericProvenance) {
  for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ConstantStringFixture F;
    auto Value =
        HighExpr::makeConst(0x2000, 8, ConstantAddressProvenance::DataAddress);
    HighStmt Store;
    Store.Kind = StmtKind::Store;
    Store.StoreAddr =
        HighExpr::makeVar(MedVar{.Kind = MedVar::Param, .Size = 8});
    Store.StoreVal = Value;
    switch (Mutation) {
    case 0:
      Value->ConstProvenance = ConstantAddressProvenance::Unknown;
      break;
    case 1:
      Value->ConstProvenance = ConstantAddressProvenance::Scalar;
      break;
    case 2:
      Value->ConstProvenance = ConstantAddressProvenance::AddressFragment;
      break;
    case 3:
      Value->ConstProvenance = ConstantAddressProvenance::CodeAddress;
      break;
    case 4:
      Value->Type = NdType::makeInt(4);
      break;
    case 5:
      Value->Type = NdType::makeFloat(8);
      break;
    case 6:
      Value->AddressOwnerVA = 0x2020;
      break;
    case 7:
      Store.StoreVal =
          HighExpr::makeBinop(NdOp::INT_ADD, Value, HighExpr::makeConst(1, 8));
      break;
    }
    F.Function.Body = {Store};
    auto Result = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_FALSE(Result.Limitation.empty());
    EXPECT_TRUE(Result.ConstantStrings.empty());
  }
}

TEST(ObjCSourceBindings, NarrowSwiftInlineStringStoreKeepsNumericPayload) {
  for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ConstantStringFixture F;
    Segment Collision;
    Collision.Name = "__TEXT";
    Collision.VA = 0x626b00;
    Collision.Size = Collision.FileSz = 0x100;
    Collision.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Collision.Data.resize(0x100);
    F.Image.Segments.push_back(Collision);
    Section CollisionSection;
    CollisionSection.Name = "__text";
    CollisionSection.SegmentName = Collision.Name;
    CollisionSection.VA = Collision.VA;
    CollisionSection.Size = CollisionSection.FileSz = Collision.Size;
    CollisionSection.Flags = Collision.Flags;
    F.Image.Sections.push_back(CollisionSection);

    F.Function.Entry = 0x1080;
    const uint32_t Code[] = {0x528d6c69, 0x72a00c49, 0xd2fc600a, 0xa9002909};
    for (unsigned Index = 0; Index < 4; ++Index)
      llvm::support::endian::write32le(
          F.Image.Segments[1].Data.data() + 0x80 + Index * 4, Code[Index]);
    MedVar BaseVar;
    BaseVar.Kind = MedVar::Temp;
    BaseVar.Id = 1;
    BaseVar.Size = 8;
    MedVar IndexVar = BaseVar;
    IndexVar.Id = 2;
    const auto Word = NdType::makeInt(8, false);
    const auto Address = [&] {
      return HighExpr::makeBinop(NdOp::INT_ADD,
                                 HighExpr::makeVar(BaseVar, Word),
                                 HighExpr::makeVar(IndexVar, Word));
    };
    HighStmt Payload;
    Payload.Kind = StmtKind::Store;
    Payload.Addr = 0x108c;
    Payload.StoreAddr = Address();
    Payload.StoreVal =
        HighExpr::makeConst(0x626b63, 8, ConstantAddressProvenance::Scalar);
    HighStmt Marker;
    Marker.Kind = StmtKind::Store;
    Marker.Addr = Payload.Addr;
    Marker.StoreAddr = HighExpr::makeBinop(
        NdOp::INT_ADD, Address(),
        HighExpr::makeConst(8, 8, ConstantAddressProvenance::Scalar));
    Marker.StoreVal = HighExpr::makeConst(0xe300000000000000, 8);
    F.Function.Body = {Payload, Marker};
    switch (Mutation) {
    case 1:
      F.Function.Body[1].StoreVal->ConstVal = 0xe400000000000000;
      break;
    case 2:
      F.Function.Body[0].StoreVal->ConstProvenance =
          ConstantAddressProvenance::Unknown;
      break;
    case 3:
      F.Function.Body[0].StoreVal->AddressOwnerVA = 0x626b63;
      break;
    case 4:
      F.Function.Body[1].StoreAddr->Operands[1]->ConstVal = 16;
      break;
    case 5:
      F.Function.Body[1].Addr += 4;
      break;
    case 6:
      llvm::support::endian::write32le(F.Image.Segments[1].Data.data() + 0x84,
                                       0x72a00c4a);
      break;
    case 7:
      F.Image.CodePtrRelocSlots.insert(0x1080);
      break;
    case 8:
      F.Function.Body[1].StoreVal->ConstProvenance =
          ConstantAddressProvenance::DataAddress;
      break;
    case 9:
      F.Function.Body.pop_back();
      break;
    case 10:
      std::swap(F.Function.Body[0], F.Function.Body[1]);
      break;
    case 11: {
      HighStmt Extra = Payload;
      Extra.Addr = 0x1090;
      F.Function.Body.push_back(Extra);
      break;
    }
    }
    const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    if (Mutation == 0) {
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      ASSERT_EQ(Bound.Function.Body[0].StoreVal->Kind, ExprKind::Const);
      EXPECT_EQ(Bound.Function.Body[0].StoreVal->ConstVal, 0x626b63U);
    } else {
      EXPECT_FALSE(Bound.Limitation.empty());
    }
  }
}

TEST(ObjCSourceBindings, ReplacesLoadedSelectorAndKeepsOriginalProjection) {
  Fixture F;
  auto Result = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  auto Call = Result.Function.Body[0].RetVal;
  ASSERT_EQ(Call->Kind, ExprKind::Call);
  ASSERT_TRUE(Call->SourceCallHint);
  EXPECT_EQ(Call->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeSelector);
  EXPECT_EQ(Call->SourceCallHint->TargetName, "step:");
  EXPECT_TRUE(Call->Operands.empty());
  EXPECT_EQ(F.Function.Body[0].RetVal->Kind, ExprKind::Load);
  EXPECT_EQ(F.Function.Body[0].RetVal->Operands[0]->ConstVal, 0x1010u);
  EXPECT_TRUE(objcSourceCallBound(*Call, F.Image, {}));
}

TEST(ObjCSourceBindings, SlotAddressIsNotTheRuntimeValue) {
  Fixture F;
  F.Function.Body[0].RetVal = HighExpr::makeConst(0x1010, 8);
  auto Result = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_FALSE(Result.Limitation.empty());
  EXPECT_EQ(Result.Function.Body[0].RetVal->Kind, ExprKind::Const);
  ASSERT_EQ(Result.Diagnostics.Items.size(), 1U);
  EXPECT_TRUE(Result.Diagnostics.Complete);
  EXPECT_EQ(Result.Diagnostics.Items[0].Issue,
            SourceProjectionIssue::DataBinding);
  EXPECT_EQ(Result.Diagnostics.Items[0].RelatedAddress, 0x1010U);
  EXPECT_EQ(Result.Diagnostics.Items[0].Expression,
            Result.Function.Body[0].RetVal.get());
  F.Function.Body[0].RetVal =
      HighExpr::makeLoad(HighExpr::makeConst(0x1014, 8), NdType::makeInt(4));
  EXPECT_FALSE(
      bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
}

TEST(ObjCSourceBindings, NumericMasksKeepOccurrenceProvenanceThroughHighIR) {
  using P = ConstantAddressProvenance;
  MedFunc Med;
  Med.Entry = 0x2000;
  Med.Name = "constant_origin";
  SourceFunctionTypeHint Hint;
  Hint.ReturnType = NdType::makeInt(8, false);
  Hint.Parameters = {{"objc_self", NdType::makePtr(NdType::makeVoid())},
                     {"objc_cmd", NdType::makePtr(NdType::makeVoid())}};
  Med.SourceTypeHint = Hint;
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Med.Entry;
  MedOp Set;
  Set.Opcode = NdOp::COPY;
  Set.Output.Kind = MedVar::Reg;
  Set.Output.TheArch = Arch::AArch64;
  Set.Output.Id = 1;
  Set.Output.SSAVer = 1;
  Set.Output.Size = 8;
  Set.Output.RegOff = getTargetRegInfo(Arch::AArch64).IntReturnReg;
  Set.addInput(MedVar::makeConst(0x1040, 8, P::DataAddress, 0x1000));
  Block.Ops.push_back(Set);
  MedOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Block.Ops.push_back(Ret);
  Med.Blocks.push_back(Block);
  for (P Provenance : {P::Scalar, P::Unknown, P::AddressFragment, P::Address,
                       P::DataAddress, P::CodeAddress}) {
    const va_t Owner = Provenance == P::Scalar ? InvalidVA : 0x1000;
    Med.Blocks[0].Ops[0].Inputs[0] =
        MedVar::makeConst(0x1040, 8, Provenance, Owner);
    const auto High = MedToHighConverter().convert(Med, Arch::AArch64);
    ASSERT_FALSE(High.Body.empty());
    const auto Value = High.Body.back().RetVal;
    ASSERT_TRUE(Value);
    ASSERT_EQ(Value->Kind, ExprKind::Const);
    EXPECT_EQ(Value->ConstProvenance, Provenance);
    EXPECT_EQ(Value->AddressOwnerVA, Owner);

    Fixture F;
    F.Function.ReturnType = NdType::makeInt(8, false);
    MedVar Input;
    Input.Kind = MedVar::Param;
    Input.Size = 8;
    F.Function.Body[0].RetVal =
        HighExpr::makeBinop(NdOp::INT_OR, HighExpr::makeVar(Input), Value);
    const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_EQ(Bound.Limitation.empty(), Provenance == P::Scalar);
    EXPECT_EQ(Bound.Function.Body[0].RetVal->Operands[1]->ConstVal, 0x1040U);
  }
}

TEST(ObjCSourceBindings, ScalarMaskIdentityCannotAuthorizeAnAddressConsumer) {
  using P = ConstantAddressProvenance;
  Fixture F;
  F.Function.ReturnType = NdType::makeInt(8, false);
  const auto Scalar = HighExpr::makeConst(0x1040, 8, P::Scalar);
  const auto Unknown = HighExpr::makeConst(0x1040, 8);
  const auto Address = HighExpr::makeConst(0x1040, 8, P::DataAddress, 0x1000);
  const auto OtherOwner =
      HighExpr::makeConst(0x1040, 8, P::DataAddress, 0x1020);
  EXPECT_FALSE(Scalar->structuralEq(*Unknown));
  EXPECT_FALSE(Scalar->structuralEq(*Address));
  EXPECT_FALSE(Address->structuralEq(*OtherOwner));
  MedVar Input;
  Input.Kind = MedVar::Param;
  Input.Size = 8;
  const auto Mask =
      HighExpr::makeBinop(NdOp::INT_OR, HighExpr::makeVar(Input), Scalar);
  F.Function.Body[0].RetVal = Mask;
  ASSERT_TRUE(bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
  // Reuse both nodes in a different context, including a pointer expression
  // whose root is not a constant. Neither the DAG cache nor a scalar leaf can
  // turn a memory address into a numeric-only use.
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr = Mask;
  Store.StoreVal = HighExpr::makeConst(7, 8);
  for (bool AddressFirst : {false, true}) {
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = Mask;
    F.Function.Body = AddressFirst ? std::vector<HighStmt>{Store, Return}
                                   : std::vector<HighStmt>{Return, Store};
    EXPECT_FALSE(
        bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
  }
  F.Function.Body.resize(1);
  F.Function.Body[0].Kind = StmtKind::Return;
  F.Function.Body[0].RetVal = HighExpr::makeLoad(Mask, NdType::makeInt(8));
  EXPECT_FALSE(
      bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
  F.Function.Body[0].RetVal = Mask;
  F.Function.ReturnType = NdType::makePtr(NdType::makeVoid());
  EXPECT_FALSE(
      bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
  F.Function.ReturnType = NdType::makeInt(8, false);
  Mask->Operands[1] = HighExpr::makeConst(0x1040, 8, P::Scalar, 0x1000);
  EXPECT_FALSE(
      bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
}

TEST(ObjCSourceBindings, OrderedOrWrongWidthLoadsDoNotBecomeQueries) {
  Fixture F;
  F.Function.Body[0].RetVal->Type = NdType::makeInt(4);
  EXPECT_FALSE(
      bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
  F.Function.Body[0].RetVal->Type = NdType::makeInt(8);
  F.Function.Body[0].RetVal->MemoryOrdering = NdMemoryOrdering::Acquire;
  EXPECT_FALSE(
      bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
}

TEST(ObjCSourceBindings, IvarCarrierPreservesReadWidthAndDeclaringClass) {
  Fixture F;
  F.Image.ObjCSourceReferences[0x1010] = {ObjCSourceReference::Kind::IvarOffset,
                                          0x1010, 8, "_wide", "Base"};
  for (uint16_t Width : {4, 8}) {
    F.Function.Body[0].RetVal->Type = NdType::makeInt(Width);
    auto Result = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
    ASSERT_TRUE(Result.Function.Body[0].RetVal->SourceCallHint);
    EXPECT_EQ(Result.Function.Body[0]
                  .RetVal->SourceCallHint->Signature.ReturnType->Size,
              Width);
    EXPECT_EQ(Result.InstanceLayoutClasses, std::set<std::string>{"Base"});
    EXPECT_TRUE(
        objcSourceCallBound(*Result.Function.Body[0].RetVal, F.Image, {}));
  }
  F.Image.ObjCSourceReferences[0x1010].Size = 4;
  EXPECT_FALSE(
      bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
}

TEST(ObjCSourceBindings, ForgedRuntimeIdentityCannotReuseAnotherSlot) {
  Fixture F;
  auto Result = bindObjCSourceReferences(F.Function, F.Image);
  auto Expression = Result.Function.Body[0].RetVal;
  auto Hint = std::make_shared<SourceCallTypeHint>(*Expression->SourceCallHint);
  Hint->TargetName = "different:";
  Expression->SourceCallHint = Hint;
  EXPECT_FALSE(objcSourceCallBound(*Expression, F.Image, {}));
}

TEST(ObjCSourceBindings, RuntimeCallsRevalidateImportedSignatureAndRegister) {
  Fixture F;
  F.Image.ImportPtrSlots[0x1020] = "_objc_retain_x19";
  const auto Original = objcRuntimeSourceCallHint(F.Image, 0x1020);
  ASSERT_TRUE(Original);
  auto Call = HighExpr::makeCall("objc_retain", 0x1020,
                                 {HighExpr::makeConst(0x5678, 8)});
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Original);
  EXPECT_TRUE(objcSourceCallBound(*Call, F.Image, {}));
  HighFunc Function;
  HighStmt Statement;
  Statement.Kind = StmtKind::Return;
  Statement.RetVal = Call;
  Function.Body.push_back(Statement);
  EXPECT_TRUE(bindObjCSourceReferences(Function, F.Image).Dependencies.empty());
  auto Changed = *Original;
  Changed.Signature.Parameters[0].Location.RegisterOffset = 0;
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Changed);
  EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {}));
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Original);
  F.Image.ImportPtrSlots[0x1020] = "_objc_release_x19";
  EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {}));
}

TEST(ObjCSourceBindings, SwiftMetadataCallsRevalidateDeclarationAndConvention) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    Fixture F;
    F.Image.Arch = Architecture;
    constexpr va_t Slot = 0x10e0;
    const std::string Symbol = "_$s10Foundation3URLVMa";
    F.Image.ImportPtrSlots[Slot] = Symbol;
    ASSERT_TRUE(F.Image.recordDyldBindSlot(
        Slot, Symbol, 0,
        "/System/Library/Frameworks/Foundation.framework/Foundation", false));
    const auto Hint = swiftRuntimeSourceCallHint(F.Image, Slot);
    ASSERT_TRUE(Hint);
    EXPECT_EQ(Hint->Signature.Origin,
              SourceFunctionTypeHint::OriginKind::SwiftSDK);
    EXPECT_EQ(Hint->Signature.Convention,
              SourceFunctionTypeHint::ConventionKind::Swift);
    ASSERT_EQ(Hint->Signature.ReturnComponents.size(), 2U);
    auto Call = HighExpr::makeCall(Symbol, Slot, {HighExpr::makeConst(0, 8)});
    Call->Type = Hint->Signature.ReturnType;
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    ASSERT_TRUE(objcSourceCallBound(*Call, F.Image, {}));
    for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
      auto Bad = std::make_shared<SourceCallTypeHint>(*Hint);
      if (Mutation == 0)
        Bad->Signature.Convention = SourceFunctionTypeHint::ConventionKind::C;
      else if (Mutation == 1)
        Bad->Signature.ReturnComponents.pop_back();
      else if (Mutation == 2)
        Bad->Signature.Origin =
            SourceFunctionTypeHint::OriginKind::SwiftRuntime;
      else
        Bad->Signature.Parameters[0].Location.RegisterOffset += 8;
      Call->SourceCallHint = Bad;
      EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {})) << Mutation;
    }
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    const auto Bind = F.Image.DyldBindSlots.at(Slot);
    for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
      F.Image.DyldBindSlots[Slot] = Bind;
      F.Image.ImportPtrSlots[Slot] = Symbol;
      if (Mutation == 0)
        F.Image.DyldBindSlots[Slot].Module =
            "/tmp/Foundation.framework/Foundation";
      else if (Mutation == 1)
        F.Image.DyldBindSlots[Slot].Addend = 1;
      else if (Mutation == 2)
        F.Image.DyldBindSlots[Slot].WeakImport = true;
      else if (Mutation == 3)
        F.Image.DyldBindSlots.erase(Slot);
      else {
        const std::string Unknown = Mutation == 4   ? Symbol + ".fake"
                                    : Mutation == 5 ? "_$s4Test3BoxVMa"
                                                    : "_$s10Foundation4FakeVMa";
        F.Image.ImportPtrSlots[Slot] = Unknown;
        F.Image.DyldBindSlots[Slot].Name = Unknown;
      }
      EXPECT_FALSE(swiftRuntimeSourceCallHint(F.Image, Slot)) << Mutation;
      EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {})) << Mutation;
    }
  }
}

TEST(ObjCSourceBindings, SwiftConcurrencyMetadataRequiresExactProvider) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    Fixture F;
    F.Image.Arch = Architecture;
    constexpr va_t Slot = 0x10e0;
    const std::string Symbol = "_$sScMMa";
    F.Image.ImportPtrSlots[Slot] = Symbol;
    ASSERT_TRUE(F.Image.recordDyldBindSlot(
        Slot, Symbol, 0, "/usr/lib/swift/libswift_Concurrency.dylib", false));
    const auto Hint = swiftRuntimeSourceCallHint(F.Image, Slot);
    ASSERT_TRUE(Hint);
    EXPECT_EQ(Hint->Signature.Origin,
              SourceFunctionTypeHint::OriginKind::SwiftSDK);
    EXPECT_EQ(Hint->Signature.Convention,
              SourceFunctionTypeHint::ConventionKind::Swift);
    ASSERT_EQ(Hint->Signature.Parameters.size(), 1U);
    ASSERT_EQ(Hint->Signature.ReturnComponents.size(), 2U);
    F.Image.DyldBindSlots[Slot].Module = "/usr/lib/swift/libswiftCore.dylib";
    EXPECT_FALSE(swiftRuntimeSourceCallHint(F.Image, Slot));
  }
}

TEST(ObjCSourceBindings, FixedCRecordsRevalidateExportsTypesAndCarriers) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (bool Floating : {false, true}) {
      Fixture F;
      F.Image.Arch = Architecture;
      constexpr va_t Slot = 0x10e0;
      const std::string Name =
          Floating ? "_CGRectStandardize" : "_NSUnionRange";
      const std::string Module =
          Floating
              ? "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics"
              : "/System/Library/Frameworks/Foundation.framework/Foundation";
      F.Image.ImportPtrSlots[Slot] = Name;
      ASSERT_TRUE(F.Image.recordDyldBindSlot(Slot, Name, 0, Module, false));
      const auto Hint = darwinRuntimeSourceCallHint(F.Image, Slot);
      if (Floating && Architecture == Arch::X64) {
        EXPECT_FALSE(Hint);
        continue;
      }
      ASSERT_TRUE(Hint);
      EXPECT_EQ(Hint->Signature.Origin,
                SourceFunctionTypeHint::OriginKind::DarwinSDK);
      ASSERT_EQ(Hint->Signature.Parameters.size(), Floating ? 1U : 2U);
      ASSERT_EQ(Hint->Signature.ReturnComponents.size(), Floating ? 4U : 2U);
      std::vector<ExprPtr> Arguments;
      for (const auto &Parameter : Hint->Signature.Parameters) {
        std::vector<ExprPtr> Leaves;
        for (const auto &Member : sourceAggregateMembers(Parameter.Type))
          Leaves.push_back(
              HighExpr::makeBitCast(HighExpr::makeConst(0, 8), Member.Type));
        Arguments.push_back(HighExpr::makeRecord(Parameter.Type, Leaves));
      }
      auto Call = HighExpr::makeCall(Name, Slot, Arguments);
      Call->Type = Hint->Signature.ReturnType;
      Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
      ASSERT_TRUE(objcSourceCallBound(*Call, F.Image, {}));
      for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
        auto Bad = std::make_shared<SourceCallTypeHint>(*Hint);
        if (Mutation == 0)
          Bad->Signature.ReturnComponents.pop_back();
        else if (Mutation == 1)
          Bad->Signature.Parameters[0].Components[0].RegisterOffset += 8;
        else if (Mutation == 2) {
          auto Record = std::make_shared<NdType>(*Bad->Signature.ReturnType);
          Record->FieldOffsets[1] = 0;
          Bad->Signature.ReturnType = Record;
        } else
          Bad->Signature.Origin = SourceFunctionTypeHint::OriginKind::ObjCSDK;
        Call->SourceCallHint = Bad;
        EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {})) << Mutation;
      }
      Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
      const auto Bind = F.Image.DyldBindSlots.at(Slot);
      for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
        F.Image.DyldBindSlots[Slot] = Bind;
        F.Image.ImportPtrSlots[Slot] = Name;
        if (Mutation == 0)
          F.Image.DyldBindSlots[Slot].Module = "/tmp/Other.framework/Other";
        else if (Mutation == 1)
          F.Image.DyldBindSlots[Slot].Addend = 8;
        else if (Mutation == 2)
          F.Image.DyldBindSlots[Slot].WeakImport = true;
        else if (Mutation == 3)
          F.Image.DyldBindSlots.erase(Slot);
        else
          F.Image.ImportPtrSlots.erase(Slot);
        const auto Current = darwinRuntimeSourceCallHint(F.Image, Slot);
        EXPECT_EQ(bool(Current), Mutation == 2) << Mutation;
        if (Mutation == 2 && Current)
          EXPECT_TRUE(Current->WeakImport);
        EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {})) << Mutation;
      }
      F.Image.DyldBindSlots[Slot] = Bind;
      F.Image.ImportPtrSlots[Slot] = Name;
      EXPECT_TRUE(objcSourceCallBound(*Call, F.Image, {}));
    }
  }
}

TEST(ObjCSourceBindings, StaticAssociationKeysKeepExactContextAndIdentity) {
  Fixture F;
  F.Image.Sections[0].Type = llvm::MachO::S_CSTRING_LITERALS;
  F.Image.ImportPtrSlots[0x1020] = "_objc_getAssociatedObject";
  const auto Hint = objcRuntimeSourceCallHint(F.Image, 0x1020);
  ASSERT_TRUE(Hint);
  auto Key = HighExpr::makeConst(0x1031, 8);
  auto Call = HighExpr::makeCall("objc_getAssociatedObject", 0x1020,
                                 {HighExpr::makeConst(0, 8), Key});
  Call->Type = NdType::makePtr(NdType::makeVoid());
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
  F.Function.Body[0].RetVal = Call;
  for (va_t Address : {0x1031, 0x1032, 0x1031}) {
    Key->ConstVal = Address;
    const auto Result = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
    EXPECT_EQ(Result.AssociationKeys, std::set<va_t>{Address});
    const auto Bound = Result.Function.Body[0].RetVal->Operands[1];
    ASSERT_TRUE(Bound->SourceCallHint);
    EXPECT_EQ(Bound->SourceCallHint->CallKind,
              SourceCallTypeHint::Kind::RuntimeAssociationKey);
    EXPECT_EQ(Bound->SourceCallHint->TargetAddress, Address);
    EXPECT_TRUE(objcSourceCallBound(*Bound, F.Image, {}));
    EXPECT_EQ(Key->Kind, ExprKind::Const);
  }
  // Reusing the same expression as an ordinary address must not reuse the
  // contextual key substitution through the projection's clone cache.
  F.Function.Body[0].RetVal = HighExpr::makeBinop(NdOp::INT_ADD, Call, Key);
  const auto Mixed = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_FALSE(Mixed.Limitation.empty());
  EXPECT_EQ(Mixed.Function.Body[0].RetVal->Operands[1]->Kind, ExprKind::Const);

  F.Image.Arch = Arch::X64;
  const auto X64Hint = objcRuntimeSourceCallHint(F.Image, 0x1020);
  ASSERT_TRUE(X64Hint);
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*X64Hint);
  F.Function.Body[0].RetVal = Call;
  const auto X64 = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_TRUE(X64.Limitation.empty()) << X64.Limitation;
  const auto X64Key = X64.Function.Body[0].RetVal->Operands[1];
  ASSERT_TRUE(X64Key->SourceCallHint);
  EXPECT_EQ(X64Key->SourceCallHint->Signature.Architecture, Arch::X64);
  EXPECT_TRUE(objcSourceCallBound(*X64Key, F.Image, {}));
}

TEST(ObjCSourceBindings,
     StaticAssociationKeysRejectUnprovedConsumersAndStorage) {
  Fixture F;
  F.Image.Sections[0].Type = llvm::MachO::S_CSTRING_LITERALS;
  F.Image.ImportPtrSlots[0x1020] = "_objc_getAssociatedObject";
  const auto Hint = objcRuntimeSourceCallHint(F.Image, 0x1020);
  ASSERT_TRUE(Hint);
  auto Call = HighExpr::makeCall(
      "objc_getAssociatedObject", 0x1020,
      {HighExpr::makeConst(0, 8), HighExpr::makeConst(0x1031, 8)});
  Call->Type = NdType::makePtr(NdType::makeVoid());
  F.Function.Body[0].RetVal = Call;
  for (unsigned Case = 0; Case < 5; ++Case) {
    SCOPED_TRACE(Case);
    F.Image.Sections[0].Type = llvm::MachO::S_CSTRING_LITERALS;
    F.Image.Segments[0].Flags = SegmentFlags::Readable;
    auto Changed = *Hint;
    if (Case == 0)
      Changed.TargetName = "objc_setAssociatedObject";
    else if (Case == 1)
      Changed.Signature.Parameters[1].Location.RegisterOffset += 8;
    else if (Case == 2)
      F.Image.Sections[0].Type = 0;
    else if (Case == 3)
      F.Image.Segments[0].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
    else
      Call->Operands[1] = HighExpr::makeConst(0x1031, 4);
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Changed);
    const auto Result = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_FALSE(Result.Limitation.empty());
    EXPECT_TRUE(Result.AssociationKeys.empty());
  }
}

TEST(ObjCSourceBindings, WritableAssociationKeysRequireExactNamedIdentity) {
  constexpr va_t Slot = 0x1020;
  constexpr va_t Address = 0x1040;
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (const char *Name :
         {"objc_getAssociatedObject", "objc_setAssociatedObject"}) {
      SCOPED_TRACE(static_cast<unsigned>(Architecture));
      SCOPED_TRACE(Name);
      Fixture F;
      F.Image.Arch = Architecture;
      F.Image.ObjCSourceReferences.clear();
      F.Image.Segments[0].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
      F.Image.Sections[0].Flags = F.Image.Segments[0].Flags;
      F.Image.Symbols.push_back({"_AssociationKey", Address, 1, false});
      F.Image.ImportPtrSlots[Slot] = std::string("_") + Name;
      const auto Hint = objcRuntimeSourceCallHint(F.Image, Slot);
      ASSERT_TRUE(Hint);
      std::vector<ExprPtr> Arguments;
      for (size_t Index = 0; Index < Hint->Signature.Parameters.size(); ++Index)
        Arguments.push_back(HighExpr::makeConst(
            Index == 1 ? Address : 0, 8,
            Index == 1 ? ConstantAddressProvenance::DataAddress
                       : ConstantAddressProvenance::Unknown));
      auto Call = HighExpr::makeCall(Name, Slot, Arguments);
      Call->Type = Hint->Signature.ReturnType;
      Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
      F.Function.Body[0].RetVal = Call;

      auto Bound = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      EXPECT_EQ(Bound.AssociationKeys, std::set<va_t>{Address});
      const auto Key = Bound.Function.Body[0].RetVal->Operands[1];
      ASSERT_TRUE(Key->SourceCallHint);
      EXPECT_EQ(Key->SourceCallHint->TargetName, "_AssociationKey");
      EXPECT_EQ(Key->SourceCallHint->ByteCount, 1U);
      EXPECT_TRUE(objcSourceCallBound(*Key, F.Image, {}));

      auto Forged = *Key->SourceCallHint;
      Forged.ByteCount = 0;
      Key->SourceCallHint = std::make_shared<SourceCallTypeHint>(Forged);
      EXPECT_FALSE(objcSourceCallBound(*Key, F.Image, {}));

      for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
        auto Image = F.Image;
        if (Mutation == 0)
          Image.Symbols.clear();
        else if (Mutation == 1)
          Image.Symbols.push_back({"_AliasKey", Address, 1, false});
        else if (Mutation == 2)
          Image.Sections[0].Flags = SegmentFlags::Readable;
        else
          Image.Segments[0].Flags = SegmentFlags::Readable |
                                    SegmentFlags::Writable |
                                    SegmentFlags::Executable;
        const auto Rejected = bindObjCSourceReferences(F.Function, Image);
        EXPECT_FALSE(Rejected.Limitation.empty()) << Mutation;
        EXPECT_TRUE(Rejected.AssociationKeys.empty()) << Mutation;
      }
    }
}

TEST(ObjCSourceBindings, DispatchSpecificKeysKeepExactNamedReadonlyIdentity) {
  struct Consumer {
    const char *Name;
    size_t KeyIndex;
  };
  constexpr Consumer Consumers[] = {
      {"dispatch_get_specific", 0},
      {"dispatch_queue_get_specific", 1},
  };
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const auto &[Name, KeyIndex] : Consumers) {
      SCOPED_TRACE(static_cast<unsigned>(Architecture));
      SCOPED_TRACE(Name);
      Fixture F;
      F.Image.Arch = Architecture;
      F.Image.ObjCSourceReferences.clear();
      F.Image.Segments[0].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
      F.Image.Segments[0].ReadOnlyAfterRelocations = true;
      F.Image.Sections[0].Flags = F.Image.Segments[0].Flags;
      constexpr va_t Slot = 0x1020;
      constexpr va_t Address = 0x1040;
      const std::string Symbol = std::string("_") + Name;
      F.Image.ImportPtrSlots[Slot] = Symbol;
      ASSERT_TRUE(F.Image.recordDyldBindSlot(
          Slot, Symbol, 0, "/usr/lib/system/libdispatch.dylib", false));
      F.Image.Symbols.push_back({"_GlobalQueueIdentityKey", Address, 1, false});
      const auto Hint = darwinRuntimeSourceCallHint(F.Image, Slot);
      ASSERT_TRUE(Hint);
      ASSERT_LT(KeyIndex, Hint->Signature.Parameters.size());
      std::vector<ExprPtr> Arguments;
      for (size_t Index = 0; Index < Hint->Signature.Parameters.size(); ++Index)
        Arguments.push_back(HighExpr::makeConst(
            Index == KeyIndex ? Address : 0, 8,
            Index == KeyIndex ? ConstantAddressProvenance::DataAddress
                              : ConstantAddressProvenance::Unknown));
      auto Call = HighExpr::makeCall(Name, Slot, Arguments);
      Call->Type = Hint->Signature.ReturnType;
      Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
      F.Function.Body[0].RetVal = Call;

      const auto Result = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
      EXPECT_EQ(Result.AssociationKeys, std::set<va_t>{Address});
      const auto Bound = Result.Function.Body[0].RetVal->Operands[KeyIndex];
      ASSERT_TRUE(Bound->SourceCallHint);
      EXPECT_EQ(Bound->SourceCallHint->CallKind,
                SourceCallTypeHint::Kind::RuntimeAssociationKey);
      EXPECT_EQ(Bound->SourceCallHint->TargetAddress, Address);
      EXPECT_EQ(Bound->SourceCallHint->TargetName, "_GlobalQueueIdentityKey");
      EXPECT_TRUE(objcSourceCallBound(*Bound, F.Image, {}));

      auto Forged = *Bound->SourceCallHint;
      Forged.TargetName = "_DifferentIdentityKey";
      Bound->SourceCallHint = std::make_shared<SourceCallTypeHint>(Forged);
      EXPECT_FALSE(objcSourceCallBound(*Bound, F.Image, {}));
    }
  }
}

TEST(ObjCSourceBindings, DispatchSpecificKeysRejectUnprovedIdentityOrContext) {
  Fixture F;
  F.Image.ObjCSourceReferences.clear();
  constexpr va_t Slot = 0x1020;
  constexpr va_t Address = 0x1040;
  F.Image.ImportPtrSlots[Slot] = "_dispatch_get_specific";
  ASSERT_TRUE(F.Image.recordDyldBindSlot(Slot, "_dispatch_get_specific", 0,
                                         "/usr/lib/system/libdispatch.dylib",
                                         false));
  F.Image.Symbols.push_back({"_GlobalQueueIdentityKey", Address, 1, false});
  const auto Hint = darwinRuntimeSourceCallHint(F.Image, Slot);
  ASSERT_TRUE(Hint);
  auto Call = HighExpr::makeCall(
      "dispatch_get_specific", Slot,
      {HighExpr::makeConst(Address, 8,
                           ConstantAddressProvenance::DataAddress)});
  Call->Type = Hint->Signature.ReturnType;
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
  F.Function.Body[0].RetVal = Call;
  for (unsigned Case = 0; Case < 6; ++Case) {
    SCOPED_TRACE(Case);
    auto Image = F.Image;
    auto Changed = *Hint;
    Call->Operands[0] =
        HighExpr::makeConst(Address, 8, ConstantAddressProvenance::DataAddress);
    if (Case == 0)
      Image.Symbols.clear();
    else if (Case == 1)
      Image.Symbols.push_back({"_AliasIdentityKey", Address, 1, false});
    else if (Case == 2)
      Image.Segments[0].Flags = Image.Sections[0].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
    else if (Case == 3)
      Changed.Signature.Parameters[0].Location.RegisterOffset += 8;
    else if (Case == 4)
      Image.DyldBindSlots[Slot].Module = "/tmp/libdispatch.dylib";
    else
      Call->Operands[0] = HighExpr::makeConst(
          Address + 1, 8, ConstantAddressProvenance::DataAddress);
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Changed);
    const auto Result = bindObjCSourceReferences(F.Function, Image);
    EXPECT_FALSE(Result.Limitation.empty());
    EXPECT_TRUE(Result.AssociationKeys.empty());
  }

  // The address is identity-only only in the authenticated key argument.
  F.Function.Body[0].RetVal =
      HighExpr::makeConst(Address, 8, ConstantAddressProvenance::DataAddress);
  const auto Ordinary = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_FALSE(Ordinary.Limitation.empty());
  EXPECT_TRUE(Ordinary.AssociationKeys.empty());
  EXPECT_EQ(Ordinary.Function.Body[0].RetVal->Kind, ExprKind::Const);
}

TEST(ObjCSourceBindings,
     KVOContextsBindRegistrationAndMatchingCallbackComparison) {
  constexpr va_t Address = 0x1040;
  constexpr llvm::StringLiteral Registration =
      "addObserver:forKeyPath:options:context:";
  constexpr llvm::StringLiteral Callback =
      "observeValueForKeyPath:ofObject:change:context:";
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<unsigned>(Architecture));
    Fixture F;
    F.Image.Arch = Architecture;
    F.Image.ObjCSourceReferences.clear();
    F.Image.Segments[0].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    F.Image.Sections[0].Flags = F.Image.Segments[0].Flags;
    F.Image.Symbols.push_back({"_ObserverContext", Address, 1, false});
    F.Image.DynInfo.NeededLibs = {
        "/System/Library/Frameworks/Foundation.framework/Foundation"};

    const auto RegistrationSignature =
        objcSelectorSourceTypeHint(F.Image, Registration);
    ASSERT_TRUE(RegistrationSignature);
    auto RegistrationHint = std::make_shared<SourceCallTypeHint>();
    RegistrationHint->CallKind = SourceCallTypeHint::Kind::ObjCMessage;
    RegistrationHint->TargetName = "objc_msgSend";
    RegistrationHint->Selector = Registration.str();
    RegistrationHint->Signature = *RegistrationSignature;
    std::vector<ExprPtr> Arguments;
    for (size_t I = 0; I < RegistrationSignature->Parameters.size(); ++I)
      Arguments.push_back(
          HighExpr::makeConst(I == 5 ? Address : 0, 8,
                              I == 5 ? ConstantAddressProvenance::DataAddress
                                     : ConstantAddressProvenance::Unknown));
    auto Register = HighExpr::makeCall("objc_msgSend", 0, Arguments);
    Register->Type = RegistrationSignature->ReturnType;
    Register->SourceCallHint = RegistrationHint;
    F.Function.Body[0].RetVal = Register;
    auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    EXPECT_EQ(Bound.KVOContexts, std::set<va_t>{Address});
    const auto RegisteredContext = Bound.Function.Body[0].RetVal->Operands[5];
    ASSERT_TRUE(RegisteredContext->SourceCallHint);
    EXPECT_EQ(RegisteredContext->SourceCallHint->CallKind,
              SourceCallTypeHint::Kind::RuntimeKVOContext);
    EXPECT_TRUE(objcSourceCallBound(*RegisteredContext, F.Image, {}));

    ObjCMethod Method;
    Method.Implementation = 0x2000;
    Method.ClassName = "Observer";
    Method.Selector = Callback.str();
    Method.TypeEncoding = "v48@0:8@16@24@32^v40";
    Method.Status = "supported";
    Method.TypeHint =
        parseObjCMethodEncoding(Method.Selector, Method.TypeEncoding);
    ASSERT_TRUE(Method.TypeHint);
    std::string Reason;
    ASSERT_TRUE(
        assignDarwinObjCSourceABI(*Method.TypeHint, Architecture, Reason))
        << Reason;
    F.Image.ObjCMethods = {Method};
    F.Function.Entry = Method.Implementation;
    F.Function.SourceTypeHint = Method.TypeHint;
    F.Function.Params.clear();
    for (const auto &Parameter : Method.TypeHint->Parameters)
      F.Function.Params.push_back({Parameter.Name, Parameter.Type});
    MedVar ContextParameter;
    ContextParameter.Kind = MedVar::Param;
    ContextParameter.Id = 5;
    ContextParameter.Size = 8;
    auto Context = HighExpr::makeVar(ContextParameter,
                                     Method.TypeHint->Parameters[5].Type);
    auto Token =
        HighExpr::makeConst(Address, 8, ConstantAddressProvenance::DataAddress);
    F.Function.Body[0].RetVal =
        HighExpr::makeBinop(NdOp::INT_EQUAL, Context, Token);
    Bound = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    EXPECT_EQ(Bound.KVOContexts, std::set<va_t>{Address});
    const auto ComparedContext = Bound.Function.Body[0].RetVal->Operands[1];
    ASSERT_TRUE(ComparedContext->SourceCallHint);
    EXPECT_EQ(ComparedContext->SourceCallHint->CallKind,
              SourceCallTypeHint::Kind::RuntimeKVOContext);
    EXPECT_TRUE(objcSourceCallBound(*ComparedContext, F.Image, {}));

    MedVar Local;
    Local.Kind = MedVar::Temp;
    Local.Id = 50001;
    Local.Size = 8;
    HighStmt Define;
    Define.Kind = StmtKind::Assign;
    Define.Dst = HighExpr::makeVar(Local, NdType::makePtr());
    Define.Val =
        HighExpr::makeConst(Address, 8, ConstantAddressProvenance::DataAddress);
    F.Function.Body.insert(F.Function.Body.begin(), Define);
    auto LocalValue = HighExpr::makeVar(Local, NdType::makePtr());
    F.Function.Body[1].RetVal =
        HighExpr::makeBinop(NdOp::INT_EQUAL, Context, LocalValue);
    Bound = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    EXPECT_EQ(Bound.KVOContexts, std::set<va_t>{Address});
    ASSERT_TRUE(Bound.Function.Body[0].Val->SourceCallHint);
    EXPECT_EQ(Bound.Function.Body[0].Val->SourceCallHint->CallKind,
              SourceCallTypeHint::Kind::RuntimeKVOContext);

    F.Function.Body[1].RetVal = HighExpr::makeBinop(
        NdOp::BOOL_OR,
        HighExpr::makeBinop(NdOp::INT_EQUAL, Context, LocalValue), LocalValue);
    Bound = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_FALSE(Bound.Limitation.empty());
    EXPECT_TRUE(Bound.KVOContexts.empty());
  }
}

TEST(ObjCSourceBindings, KVOContextsRejectUnprovedStorageAndUses) {
  constexpr va_t Address = 0x1040;
  constexpr llvm::StringLiteral Callback =
      "observeValueForKeyPath:ofObject:change:context:";
  Fixture F;
  F.Image.ObjCSourceReferences.clear();
  F.Image.Segments[0].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  F.Image.Sections[0].Flags = F.Image.Segments[0].Flags;
  F.Image.Symbols.push_back({"_ObserverContext", Address, 1, false});
  ObjCMethod Method;
  Method.Implementation = 0x2000;
  Method.ClassName = "Observer";
  Method.Selector = Callback.str();
  Method.TypeEncoding = "v48@0:8@16@24@32^v40";
  Method.Status = "supported";
  Method.TypeHint =
      parseObjCMethodEncoding(Method.Selector, Method.TypeEncoding);
  ASSERT_TRUE(Method.TypeHint);
  std::string Reason;
  ASSERT_TRUE(
      assignDarwinObjCSourceABI(*Method.TypeHint, F.Image.Arch, Reason));
  F.Image.ObjCMethods = {Method};
  F.Function.Entry = Method.Implementation;
  F.Function.SourceTypeHint = Method.TypeHint;
  for (const auto &Parameter : Method.TypeHint->Parameters)
    F.Function.Params.push_back({Parameter.Name, Parameter.Type});
  MedVar ContextParameter;
  ContextParameter.Kind = MedVar::Param;
  ContextParameter.Id = 5;
  ContextParameter.Size = 8;
  auto Context =
      HighExpr::makeVar(ContextParameter, Method.TypeHint->Parameters[5].Type);
  auto Comparison = HighExpr::makeBinop(
      NdOp::INT_EQUAL, Context,
      HighExpr::makeConst(Address, 8, ConstantAddressProvenance::DataAddress));
  F.Function.Body[0].RetVal = Comparison;

  for (unsigned Case = 0; Case < 6; ++Case) {
    SCOPED_TRACE(Case);
    auto Image = F.Image;
    auto Function = F.Function;
    if (Case == 0)
      Image.Symbols.clear();
    else if (Case == 1)
      Image.Symbols.push_back({"_AliasContext", Address, 1, false});
    else if (Case == 2)
      Image.Segments[0].Flags = Image.Sections[0].Flags =
          SegmentFlags::Readable;
    else if (Case == 3)
      Image.ObjCMethods[0].Status = "ambiguous_dispatch";
    else if (Case == 4)
      Function.Body[0].RetVal->Operands[0] = HighExpr::makeConst(0, 8);
    else
      Function.Body[0].RetVal = HighExpr::makeBinop(
          NdOp::INT_ADD, Context,
          HighExpr::makeConst(Address, 8,
                              ConstantAddressProvenance::DataAddress));
    const auto Result = bindObjCSourceReferences(Function, Image);
    EXPECT_FALSE(Result.Limitation.empty());
    EXPECT_TRUE(Result.KVOContexts.empty());
  }

  F.Function.Body[0].RetVal =
      HighExpr::makeConst(Address, 8, ConstantAddressProvenance::DataAddress);
  const auto Ordinary = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_FALSE(Ordinary.Limitation.empty());
  EXPECT_TRUE(Ordinary.KVOContexts.empty());
}

namespace {
Fixture immutableSelfPointerFixture(Arch Architecture) {
  Fixture F;
  F.Image.Arch = Architecture;
  F.Image.ObjCSourceReferences.clear();
  F.Image.Segments[0].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  F.Image.Segments[0].ReadOnlyAfterRelocations = true;
  F.Image.Sections[0].Flags = F.Image.Segments[0].Flags;
  F.Image.Symbols.push_back({"_QueueKey", 0x1040, 8, false});
  F.Image.MachOHasChainedFixups = true;
  F.Image.MachOResolvedChainedPointerSlots.insert(0x1040);
  F.Image.DataPtrRelocSlots.insert(0x1040);
  F.Image.DataPtrRelocTargetOwners[0x1040] = 0x1000;
  llvm::support::endian::write64le(F.Image.Segments[0].Data.data() + 0x40,
                                   0x1040);
  F.Function.ReturnType = NdType::makeInt(8, false);
  F.Function.Body[0].RetVal =
      HighExpr::makeConst(0x1040, 8, ConstantAddressProvenance::DataAddress);
  return F;
}
} // namespace

TEST(ObjCSourceBindings, ImmutableSelfPointerAddressesShareLoadedIdentity) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto F = immutableSelfPointerFixture(Architecture);
    const auto Direct = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Direct.Limitation.empty()) << Direct.Limitation;
    ASSERT_EQ(Direct.StaticIdentities, std::set<va_t>{0x1040});
    const auto Address = Direct.Function.Body[0].RetVal;
    ASSERT_TRUE(Address->SourceCallHint);
    EXPECT_EQ(Address->SourceCallHint->ByteCount, 8U);
    EXPECT_TRUE(objcSourceCallBound(*Address, F.Image, {}));
    F.Function.Body[0].RetVal = HighExpr::makeLoad(F.Function.Body[0].RetVal,
                                                   NdType::makeInt(8, false));
    const auto Loaded = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Loaded.Limitation.empty()) << Loaded.Limitation;
    EXPECT_EQ(Direct.StaticIdentities, Loaded.StaticIdentities);
    F.Image.Segments[0].ReadOnlyAfterRelocations = false;
    EXPECT_FALSE(objcSourceCallBound(*Address, F.Image, {}));
  }
}

TEST(ObjCSourceBindings, ImmutableSelfPointerAddressesRejectStaleStorage) {
  for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = immutableSelfPointerFixture(Arch::AArch64);
    switch (Mutation) {
    case 0:
      F.Image.Segments[0].ReadOnlyAfterRelocations = false;
      break;
    case 1:
      F.Image.DataPtrRelocSlots.clear();
      break;
    case 2:
      F.Image.DataPtrRelocTargetOwners[0x1040] = 0x2000;
      break;
    case 3:
      F.Image.MachOResolvedChainedPointerSlots.clear();
      break;
    case 4:
      F.Image.Sections.push_back(F.Image.Sections[0]);
      break;
    case 5:
      F.Image.Symbols.push_back({"_Alias", 0x1040, 8, false});
      break;
    case 6:
      F.Image.CodePtrRelocSlots.insert(0x103f);
      break;
    case 7:
      F.Image.ImportPtrSlots[0x1040] = "_external";
      break;
    case 8:
      F.Image.Sections[0].FileSz = 0x47;
      break;
    case 9:
      F.Function.Body[0].RetVal->AddressOwnerVA = 0x1038;
      break;
    case 10:
      llvm::support::endian::write64le(F.Image.Segments[0].Data.data() + 0x40,
                                       0x1048);
      break;
    }
    const auto Result = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_FALSE(Result.Limitation.empty());
    EXPECT_TRUE(Result.StaticIdentities.empty());
  }
}

TEST(ObjCSourceBindings, ImmutableSelfPointerIdentityExecutesWithItsContents) {
  auto F = immutableSelfPointerFixture(Arch::AArch64);
  const auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  std::set<std::string> Shared;
  std::string Source =
      "#include <stdint.h>\n#include <string.h>\n" +
      renderObjCStaticIdentityHelpers(Result.StaticIdentities, Shared);
  Source += R"(
int main(void) {
  const void *key = (const void *)neverd_static_identity_1040_address();
  const void *loaded;
  memcpy(&loaded, key, sizeof(loaded));
  if (key != loaded) return 1;
  if ((const void *)neverd_static_identity_1040_address() != key) return 2;
  return 0;
}
)";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-self-pointer", Directory));
  const std::filesystem::path Work(Directory.str().str());
  struct Cleanup {
    std::filesystem::path Work;
    ~Cleanup() {
      std::error_code Error;
      std::filesystem::remove_all(Work, Error);
    }
  } Cleanup{Work};
  const auto Path = (Work / "objects.c").string();
  const auto Executable = (Work / "objects").string();
  const auto ErrorPath = (Work / "stderr").string();
  std::ofstream(Path) << Source;
  const std::string Compiler = NEVERD_TEST_CLANG;
  for (const char *Optimization : {"-O0", "-O2"}) {
    const std::vector<std::string> Arguments{
        Compiler, "-std=c11", Optimization, "-Werror", Path, "-o", Executable};
    std::vector<llvm::StringRef> Refs(Arguments.begin(), Arguments.end());
    const std::optional<llvm::StringRef> Redirects[] = {
        std::nullopt, std::nullopt, ErrorPath};
    std::string Error;
    const auto Status = llvm::sys::ExecuteAndWait(Compiler, Refs, std::nullopt,
                                                  Redirects, 60, 0, &Error);
    auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Status, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "");
    EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable}, std::nullopt,
                                        Redirects, 30, 0, &Error),
              0)
        << Error;
  }
}
TEST(ObjCSourceBindings, SelfPointerGlobalsBecomeSharedStaticIdentities) {
  Fixture F;
  constexpr va_t Address = 0x1040;
  F.Image.ObjCSourceReferences.clear();
  F.Image.Segments[0].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  F.Image.Sections[0].Flags = F.Image.Segments[0].Flags;
  F.Image.Symbols.push_back({"_ObserverContext", Address, 8, false});
  F.Image.MachOHasChainedFixups = true;
  F.Image.MachOResolvedChainedPointerSlots.insert(Address);
  llvm::support::endian::write64le(
      F.Image.Segments[0].Data.data() + Address - 0x1000, Address);
  F.Function.Body[0].RetVal = HighExpr::makeLoad(
      HighExpr::makeConst(Address, 8, ConstantAddressProvenance::DataAddress),
      NdType::makeInt(8, false));

  auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  EXPECT_EQ(Result.StaticIdentities, std::set<va_t>{Address});
  const auto Bound = Result.Function.Body[0].RetVal;
  ASSERT_TRUE(Bound->SourceCallHint);
  EXPECT_EQ(Bound->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeStaticIdentity);
  EXPECT_EQ(Bound->SourceCallHint->TargetName, "_ObserverContext");
  EXPECT_TRUE(objcSourceCallBound(*Bound, F.Image, {}));
  std::set<std::string> Helpers;
  const auto Source =
      renderObjCStaticIdentityHelpers(Result.StaticIdentities, Helpers);
  EXPECT_EQ(Helpers,
            std::set<std::string>{"neverd_static_identity_1040_address"});
  EXPECT_NE(Source.find("static void *identity = &identity"),
            std::string::npos);

  F.Image.MachOResolvedChainedPointerSlots.clear();
  Result = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_FALSE(Result.Limitation.empty());
  EXPECT_TRUE(Result.StaticIdentities.empty());
}

TEST(ObjCSourceBindings, NamedWritableScalarsUseSharedRebuiltStorage) {
  Fixture F;
  constexpr va_t Address = 0x1040;
  F.Image.ObjCSourceReferences.clear();
  F.Image.Segments[0].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  F.Image.Sections[0].Flags = F.Image.Segments[0].Flags;
  F.Image.Symbols.push_back({"_captureLevel", Address, 8, false});
  llvm::support::endian::write64le(
      F.Image.Segments[0].Data.data() + Address - 0x1000, 31);
  F.Function.Body[0].RetVal = HighExpr::makeLoad(
      HighExpr::makeConst(Address, 8, ConstantAddressProvenance::DataAddress),
      NdType::makeInt(8, false));

  const auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  EXPECT_EQ(Result.LocalStorageExtents,
            (std::map<va_t, uint64_t>{{Address, 8}}));
  const auto Load = Result.Function.Body[0].RetVal;
  ASSERT_EQ(Load->Kind, ExprKind::Load);
  const auto Bound = Load->Operands[0];
  ASSERT_TRUE(Bound->SourceCallHint);
  EXPECT_EQ(Bound->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeLocalStorageAddress);
  EXPECT_EQ(Bound->SourceCallHint->ByteCount, 8U);
  EXPECT_TRUE(objcSourceCallBound(*Bound, F.Image, {}));
  std::set<std::string> Helpers;
  const auto Source = renderObjCLocalStorageHelpers(
      F.Image, Result.LocalStorageExtents, Helpers);
  EXPECT_EQ(Helpers,
            std::set<std::string>{"neverd_local_storage_1040_address"});
  EXPECT_NE(Source.find("storage[8]"), std::string::npos);
  EXPECT_NE(Source.find("[0] = 31"), std::string::npos);

  F.Image.DataPtrRelocSlots.insert(Address);
  const auto Rejected = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_FALSE(Rejected.Limitation.empty());
  EXPECT_TRUE(Rejected.LocalStorageExtents.empty());
}

TEST(ObjCSourceBindings, ImmutableSwiftSmallStringKeepsSharedAddress) {
  Fixture F;
  constexpr va_t Address = 0x1040;
  constexpr llvm::StringLiteral Name =
      "_$s13WMFComponents9HtmlUtilsV17defaultListIndentSSvpZ";
  F.Image.MachOTwoLevelNamespace = true;
  F.Image.ObjCSourceReferences.clear();
  F.Image.Symbols.push_back({Name.str(), Address, 16, false});
  F.Image.Exports.push_back({Name.str(), 0, Address});
  auto *Bytes = F.Image.Segments[0].Data.data() + Address - 0x1000;
  std::fill(Bytes, Bytes + 16, 0);
  std::fill(Bytes, Bytes + 4, ' ');
  Bytes[15] = 0xe4;
  F.Function.ReturnType = NdType::makeInt(8, false);
  F.Function.Body[0].RetVal =
      HighExpr::makeConst(Address, 8, ConstantAddressProvenance::DataAddress);

  const auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  EXPECT_EQ(Result.SwiftSmallStrings, std::set<va_t>{Address});
  const auto Bound = Result.Function.Body[0].RetVal;
  ASSERT_TRUE(Bound->SourceCallHint);
  EXPECT_EQ(Bound->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeSwiftSmallStringAddress);
  EXPECT_TRUE(objcSourceCallBound(*Bound, F.Image, {}));
  std::set<std::string> Helpers;
  const auto Source = renderObjCSwiftSmallStringHelpers(
      F.Image, Result.SwiftSmallStrings, Helpers);
  EXPECT_EQ(Helpers,
            std::set<std::string>{"neverd_swift_small_string_1040_address"});
  EXPECT_NE(Source.find("static const _Alignas(16) unsigned char storage[16]"),
            std::string::npos);
  EXPECT_NE(Source.find("[15] = 228"), std::string::npos);

  for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
    auto Changed = F.Image;
    if (Mutation == 0)
      Changed.DataPtrRelocSlots.insert(Address);
    if (Mutation == 1)
      Changed.Symbols.back().Name = "_not_a_swift_string";
    if (Mutation == 2)
      Changed.Exports.clear();
    if (Mutation == 3)
      Changed.Segments[0].Data[Address - 0x1000 + 15] = 0xf4;
    if (Mutation == 4) {
      Changed.Sections[0].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
      Changed.Segments[0].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
    }
    if (Mutation == 5)
      Changed.Symbols.push_back({"_interior", Address + 8, 0, false});
    EXPECT_FALSE(objcSourceCallBound(*Bound, Changed, {})) << Mutation;
    EXPECT_THROW(renderObjCSwiftSmallStringHelpers(
                     Changed, Result.SwiftSmallStrings, Helpers),
                 std::runtime_error)
        << Mutation;
  }
}

TEST(ObjCSourceBindings, NestedSwiftSmallStringsKeepExactCellProof) {
  for (const auto *Name :
       {"_$s13WMFComponents24AccessibilityIdentifiersO6SearchO9searchBarSSvpZ",
        "_$s13WMFComponents24AccessibilityIdentifiersO4TabsO6buttonSSvpZ",
        "_$s4Test1AO1BC1CV5valueSSvpZ",
        "_$s4Test1AV1BO1CC1DV1EO1FC1GV1HO5valueSSvpZ"}) {
    SCOPED_TRACE(Name);
    Fixture F;
    constexpr va_t Address = 0x1040;
    F.Image.MachOTwoLevelNamespace = true;
    F.Image.ObjCSourceReferences.clear();
    F.Image.Symbols.push_back({Name, Address, 16, false});
    F.Image.Exports.push_back({Name, 0, Address});
    auto *Bytes = F.Image.Segments[0].Data.data() + Address - 0x1000;
    std::fill(Bytes, Bytes + 16, 0);
    const std::string Expected = "Search Bar";
    std::copy(Expected.begin(), Expected.end(), Bytes);
    Bytes[15] = 0xea;
    F.Function.ReturnType = NdType::makeInt(8, false);
    F.Function.Body[0].RetVal =
        HighExpr::makeConst(Address, 8, ConstantAddressProvenance::DataAddress);

    const auto Result = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
    EXPECT_EQ(Result.SwiftSmallStrings, std::set<va_t>{Address});
    const auto Bound = Result.Function.Body[0].RetVal;
    ASSERT_TRUE(Bound->SourceCallHint);
    EXPECT_EQ(Bound->SourceCallHint->TargetName, Name);
    EXPECT_EQ(Bound->SourceCallHint->ByteCount, 16U);
    EXPECT_TRUE(objcSourceCallBound(*Bound, F.Image, {}));
    std::set<std::string> Helpers;
    const auto Source = renderObjCSwiftSmallStringHelpers(
        F.Image, Result.SwiftSmallStrings, Helpers);
    EXPECT_NE(Source.find("[15] = 234"), std::string::npos);
    EXPECT_EQ(Helpers,
              std::set<std::string>{"neverd_swift_small_string_1040_address"});

    for (unsigned Mutation = 0; Mutation < 9; ++Mutation) {
      auto Changed = F.Image;
      if (Mutation == 0)
        Changed.DataPtrRelocSlots.insert(Address);
      if (Mutation == 1)
        Changed.Symbols.push_back({"_alias", Address, 16, false});
      if (Mutation == 2)
        Changed.Exports.clear();
      if (Mutation == 3)
        Changed.Exports.push_back({Name, 0, Address});
      if (Mutation == 4)
        Changed.Exports[0].Addr += 16;
      if (Mutation == 5)
        Changed.Symbols.back().Size = 8;
      if (Mutation == 6)
        Changed.Segments[0].Data[Address - 0x1000 + 15] = 0xfa;
      if (Mutation == 7)
        Changed.Segments[0].Data[Address - 0x1000 + 14] = 1;
      if (Mutation == 8) {
        Changed.Sections[0].Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
        Changed.Segments[0].Flags = Changed.Sections[0].Flags;
      }
      EXPECT_FALSE(objcSourceCallBound(*Bound, Changed, {})) << Mutation;
      EXPECT_THROW(renderObjCSwiftSmallStringHelpers(
                       Changed, Result.SwiftSmallStrings, Helpers),
                   std::runtime_error)
          << Mutation;
    }
  }
}

TEST(ObjCSourceBindings, SwiftSmallStringsRejectUnprovedDeclarationContexts) {
  for (const auto *Name :
       {"_$s4Test1AV1BO1CC1DV1EO1FC1GV1HO1IC5valueSSvpZ",
        "_$s4Test3fooyyF1LV5valueSSvpZ", "_$s4Test1AV5OtherE5valueSSvpZ",
        "_$s4Test1AVySiG5valueSSvpZ", "_$s4Test1AV5valueSSvp",
        "_$s4Test1AV5valueSivpZ", "_$s4Test5valueSSvpZ",
        "_$s4Test1AV5valueSSvpZbad"}) {
    SCOPED_TRACE(Name);
    Fixture F;
    constexpr va_t Address = 0x1040;
    F.Image.MachOTwoLevelNamespace = true;
    F.Image.ObjCSourceReferences.clear();
    F.Image.Symbols.push_back({Name, Address, 16, false});
    F.Image.Exports.push_back({Name, 0, Address});
    auto *Bytes = F.Image.Segments[0].Data.data() + Address - 0x1000;
    std::fill(Bytes, Bytes + 16, 0);
    Bytes[15] = 0xe0;
    EXPECT_FALSE(
        objc_binding_detail::swiftSmallStringStorageHint(F.Image, Address));
  }
}

TEST(ObjCSourceBindings, SwiftOptionalSelfStaticUsesWitnessBoundedZeroStorage) {
  constexpr va_t Address = 0x3010;
  constexpr va_t Witness = 0x4000;
  constexpr uint64_t Width = 0x400;
  constexpr const char *StorageName = "_$s4Test5ValueV7currentACSgvpZ";
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.MachOTwoLevelNamespace = true;
  Segment Code;
  Code.VA = 0x2000;
  Code.Size = Code.FileSz = 0x100;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data.resize(Code.Size);
  Image.Segments.push_back(std::move(Code));
  Section Text;
  Text.VA = 0x2000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
  Image.Sections.push_back(Text);
  Segment Common;
  Common.VA = 0x3000;
  Common.Size = 0x800;
  Common.FileSz = 0;
  Common.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Common.Data.resize(Common.Size);
  Image.Segments.push_back(std::move(Common));
  Section CommonSection;
  CommonSection.VA = 0x3000;
  CommonSection.Size = 0x800;
  CommonSection.FileSz = 0;
  CommonSection.Type = llvm::MachO::S_ZEROFILL;
  CommonSection.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Image.Sections.push_back(CommonSection);
  Segment Constants;
  Constants.VA = Witness;
  Constants.Size = Constants.FileSz = 0x100;
  Constants.Flags = SegmentFlags::Readable;
  Constants.Data.resize(Constants.Size);
  llvm::support::endian::write64le(Constants.Data.data() + 64, Width);
  llvm::support::endian::write64le(Constants.Data.data() + 72, Width);
  Image.Segments.push_back(std::move(Constants));
  Section ConstantSection;
  ConstantSection.VA = Witness;
  ConstantSection.Size = ConstantSection.FileSz = 0x100;
  ConstantSection.Flags = SegmentFlags::Readable;
  Image.Sections.push_back(ConstantSection);
  Image.Symbols.push_back({StorageName, Address, 0, false});
  Image.Symbols.push_back({"_nextStorage", Address + Width, 0, false});
  Image.Symbols.push_back({"_$s4Test5ValueVWV", Witness, 88, false});
  Image.Symbols.push_back({"_$s4Test5ValueV7currentACSgvau", 0x2000, 0, true});
  HighFunc Function;
  Function.ReturnType = NdType::makePtr(NdType::makeVoid());
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal =
      HighExpr::makeConst(Address, 8, ConstantAddressProvenance::DataAddress);
  Function.Body = {Return};

  const auto Bound = bindObjCSourceReferences(Function, Image);
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  EXPECT_EQ(Bound.LocalStorageExtents,
            (std::map<va_t, uint64_t>{{Address, Width}}));
  const auto Helper = Bound.Function.Body[0].RetVal;
  ASSERT_TRUE(Helper->SourceCallHint);
  EXPECT_EQ(Helper->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeLocalStorageAddress);
  EXPECT_EQ(Helper->SourceCallHint->ByteCount, Width);
  EXPECT_TRUE(objcSourceCallBound(*Helper, Image, {}));
  std::set<std::string> Helpers;
  EXPECT_NE(
      renderObjCLocalStorageHelpers(Image, Bound.LocalStorageExtents, Helpers)
          .find("storage[1024]"),
      std::string::npos);
  EXPECT_NE(renderObjCLocalStorageHelpers(Image, {{Address, 8}}, Helpers)
                .find("storage[1024]"),
            std::string::npos);

  for (unsigned Mutation = 0; Mutation < 11; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Image;
    switch (Mutation) {
    case 0:
      Changed.Symbols[0].Name = "_untypedStorage";
      break;
    case 1:
      Changed.Symbols[1].Addr += 16;
      break;
    case 2:
      Changed.Symbols[2].Name = "_wrongWitness";
      break;
    case 3:
      llvm::support::endian::write64le(Changed.Segments[2].Data.data() + 64,
                                       Width - 16);
      break;
    case 4:
      llvm::support::endian::write64le(Changed.Segments[2].Data.data() + 72,
                                       Width + 16);
      break;
    case 5:
      Changed.Symbols[3].Name = "_wrongAddressor";
      break;
    case 6:
      Changed.Sections[1].Type = llvm::MachO::S_REGULAR;
      break;
    case 7:
      Changed.Segments[1].Data[Address - 0x3000 + 24] = 1;
      break;
    case 8:
      Changed.DataPtrRelocSlots.insert(Address + 16);
      break;
    case 9:
      Changed.Symbols.push_back({"_interior", Address + 32, 0, false});
      break;
    case 10:
      Changed.Symbols[0].Size = Width + 16;
      break;
    }
    EXPECT_FALSE(objc_binding_detail::swiftStaticOptionalSelfStorageHint(
        Changed, Address));
    EXPECT_FALSE(objcSourceCallBound(*Helper, Changed, {}));
    if (Mutation != 0) {
      EXPECT_THROW(renderObjCLocalStorageHelpers(
                       Changed, Bound.LocalStorageExtents, Helpers),
                   std::runtime_error);
    }
  }
}

TEST(ObjCSourceBindings, SwiftOptionalURLStaticUsesExactValueBuffer) {
  constexpr va_t Address = 0x3018;
  constexpr uint64_t Width = 3 * sizeof(uint64_t);
  constexpr const char *StorageName =
      "_$s4Test5StoreC3url10Foundation3URLVSgvpZ";
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.MachOTwoLevelNamespace = true;
  Segment Code;
  Code.VA = 0x2000;
  Code.Size = Code.FileSz = 0x100;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data.resize(Code.Size);
  Image.Segments.push_back(std::move(Code));
  Section Text;
  Text.VA = 0x2000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
  Image.Sections.push_back(Text);
  Segment Common;
  Common.VA = 0x3000;
  Common.Size = 0x100;
  Common.FileSz = 0;
  Common.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Common.Data.resize(Common.Size);
  Image.Segments.push_back(std::move(Common));
  Section CommonSection;
  CommonSection.VA = 0x3000;
  CommonSection.Size = 0x100;
  CommonSection.FileSz = 0;
  CommonSection.Type = llvm::MachO::S_ZEROFILL;
  CommonSection.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Image.Sections.push_back(CommonSection);
  Image.Symbols.push_back({StorageName, Address, 0, false});
  Image.Symbols.push_back({"_nextStorage", Address + Width, 0, false});
  Image.Symbols.push_back(
      {"_$s4Test5StoreC3url10Foundation3URLVSgvau", 0x2000, 0, true});
  HighFunc Function;
  Function.ReturnType = NdType::makePtr(NdType::makeVoid());
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal =
      HighExpr::makeConst(Address, 8, ConstantAddressProvenance::DataAddress);
  Function.Body = {Return};

  EXPECT_TRUE(
      objc_binding_detail::swiftStaticOptionalURLBufferSymbol(StorageName));
  EXPECT_TRUE(objc_binding_detail::swiftStaticOptionalURLBufferSymbol(
      "_$s3WMF8LicensesC10CCBYSA4URL10Foundation0D0VSgvpZ"));
  EXPECT_FALSE(objc_binding_detail::swiftStaticOptionalURLBufferSymbol(
      "_$s4Test5StoreC3url10Foundation4DateVSgvpZ"));
  const auto Bound = bindObjCSourceReferences(Function, Image);
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  EXPECT_EQ(Bound.LocalStorageExtents,
            (std::map<va_t, uint64_t>{{Address, Width}}));
  const auto Helper = Bound.Function.Body[0].RetVal;
  ASSERT_TRUE(Helper->SourceCallHint);
  EXPECT_EQ(Helper->SourceCallHint->ByteCount, Width);
  EXPECT_TRUE(objcSourceCallBound(*Helper, Image, {}));
  std::set<std::string> Helpers;
  EXPECT_NE(
      renderObjCLocalStorageHelpers(Image, Bound.LocalStorageExtents, Helpers)
          .find("storage[24]"),
      std::string::npos);
  auto Prefix = std::make_shared<HighExpr>(*Helper);
  auto PrefixHint =
      std::make_shared<SourceCallTypeHint>(*Helper->SourceCallHint);
  PrefixHint->ByteCount = 8;
  Prefix->SourceCallHint = std::move(PrefixHint);
  EXPECT_TRUE(objcSourceCallBound(*Prefix, Image, {}));
  EXPECT_NE(renderObjCLocalStorageHelpers(Image, {{Address, 8}}, Helpers)
                .find("storage[24]"),
            std::string::npos);

  for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Image;
    switch (Mutation) {
    case 0:
      Changed.Symbols[0].Name = "_untypedStorage";
      break;
    case 1:
      Changed.Symbols[1].Addr += 8;
      break;
    case 2:
      Changed.Symbols[2].Name = "_wrongAddressor";
      break;
    case 3:
      Changed.Sections[1].Type = llvm::MachO::S_REGULAR;
      break;
    case 4:
      Changed.Segments[1].Data[Address - 0x3000] = 1;
      break;
    case 5:
      Changed.DataPtrRelocSlots.insert(Address + 8);
      break;
    case 6:
      Changed.Symbols.push_back({"_interior", Address + 8, 0, false});
      break;
    case 7:
      Changed.Symbols[0].Size = Width + 8;
      break;
    }
    EXPECT_FALSE(objc_binding_detail::swiftStaticOptionalURLBufferHint(
        Changed, Address));
    EXPECT_FALSE(objcSourceCallBound(*Prefix, Changed, {}));
    if (Mutation != 0) {
      EXPECT_FALSE(objcSourceCallBound(*Helper, Changed, {}));
      EXPECT_THROW(renderObjCLocalStorageHelpers(
                       Changed, Bound.LocalStorageExtents, Helpers),
                   std::runtime_error);
      EXPECT_THROW(
          renderObjCLocalStorageHelpers(Changed, {{Address, 8}}, Helpers),
          std::runtime_error);
    }
  }
}

TEST(ObjCSourceBindings, AnonymousInlineSwiftStringPairsKeepSharedBytes) {
  Fixture F;
  constexpr va_t Address = 0x1040;
  constexpr uint64_t Width = 64;
  F.Image.MachOTwoLevelNamespace = true;
  F.Image.ObjCSourceReferences.clear();
  F.Image.Segments[0].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  F.Image.Sections[0].Flags = F.Image.Segments[0].Flags;
  F.Image.Symbols.push_back({"_next_object", Address + Width, 0, false});
  auto *Header = F.Image.Segments[0].Data.data() + Address - 0x1000 - 16;
  llvm::support::endian::write64le(Header, 2);
  llvm::support::endian::write64le(Header + 8, 4);
  auto *Bytes = Header + 16;
  for (unsigned Word = 0; Word < 4; ++Word) {
    Bytes[Word * 16] = 'a' + Word;
    Bytes[Word * 16 + 1] = 'b';
    Bytes[Word * 16 + 15] = 0xe2;
  }
  F.Function.ReturnType = NdType::makeInt(8, false);
  F.Function.Body[0].RetVal =
      HighExpr::makeConst(Address, 8, ConstantAddressProvenance::DataAddress);

  const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  EXPECT_EQ(Bound.LocalStorageExtents,
            (std::map<va_t, uint64_t>{{Address, Width}}));
  const auto Value = Bound.Function.Body[0].RetVal;
  ASSERT_TRUE(Value->SourceCallHint);
  EXPECT_EQ(Value->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeLocalStorageAddress);
  EXPECT_TRUE(objcSourceCallBound(*Value, F.Image, {}));
  std::set<std::string> Helpers;
  const auto Source = renderObjCLocalStorageHelpers(
      F.Image, Bound.LocalStorageExtents, Helpers);
  EXPECT_NE(Source.find("storage[64]"), std::string::npos);
  EXPECT_NE(Source.find("[15] = 226"), std::string::npos);

  for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
    auto Changed = F.Image;
    if (Mutation == 0)
      Changed.Segments[0].Data[Address - 0x1000 + 15] = 0xf2;
    if (Mutation == 1)
      Changed.Symbols.push_back({"_inside", Address + 16, 0, false});
    if (Mutation == 2)
      Changed.Symbols.clear();
    if (Mutation == 3)
      Changed.DataPtrRelocSlots.insert(Address + 8);
    if (Mutation == 4)
      llvm::support::endian::write64le(
          Changed.Segments[0].Data.data() + Address - 0x1000 - 8, 3);
    if (Mutation == 5)
      Changed.Sections[0].Flags = SegmentFlags::Readable;
    if (Mutation == 6)
      Changed.Exports.push_back({"_inside", 0, Address + 16});
    EXPECT_FALSE(objcSourceCallBound(*Value, Changed, {})) << Mutation;
    EXPECT_THROW(renderObjCLocalStorageHelpers(
                     Changed, Bound.LocalStorageExtents, Helpers),
                 std::runtime_error)
        << Mutation;
  }
}

TEST(ObjCSourceBindings, PointerAccessesKeepStorageAndValueProofsSeparate) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Shape = 0; Shape < 3; ++Shape)
      for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
        if (Shape == 0 && Mutation == 7)
          continue;
        SCOPED_TRACE(Shape);
        SCOPED_TRACE(Mutation);
        Fixture F;
        F.Image.Arch = Architecture;
        F.Image.ObjCSourceReferences.clear();
        F.Image.Segments[0].Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
        F.Image.Sections[0].Flags = F.Image.Segments[0].Flags;
        constexpr va_t Address = 0x1040;
        F.Image.Symbols.push_back({"_savedObject", Address, 8, false});
        auto Type = NdType::makePtr(NdType::makeVoid());
        if (Mutation == 1)
          Type->Size = 4;
        if (Mutation == 2)
          Type->Pointee.reset();
        if (Mutation == 3)
          F.Image.DataPtrRelocSlots.insert(Address);
        if (Mutation == 4)
          F.Image.Symbols.push_back({"_conflict", Address + 4, 4, false});
        if (Mutation == 5)
          llvm::support::endian::write64le(
              F.Image.Segments[0].Data.data() + Address - 0x1000, 0x1080);
        auto Pointer = HighExpr::makeConst(
            Address, 8, ConstantAddressProvenance::DataAddress);
        auto Value = HighExpr::makeConst(0, 8);
        Value->Type = Type;
        if (Mutation == 7) {
          Value->ConstVal = 0x1080;
          Value->ConstProvenance = ConstantAddressProvenance::DataAddress;
        }
        auto &Statement = F.Function.Body[0];
        if (Shape == 0) {
          Statement.RetVal = HighExpr::makeLoad(Pointer, Type);
          if (Mutation == 6)
            Statement.RetVal->MemoryOrdering = NdMemoryOrdering::Acquire;
        } else if (Shape == 1) {
          Statement = HighStmt{};
          Statement.Kind = StmtKind::Store;
          Statement.StoreAddr = Pointer;
          Statement.StoreVal = Value;
          if (Mutation == 6)
            Statement.MemoryOrdering = NdMemoryOrdering::Release;
        } else {
          auto Store = std::make_shared<HighExpr>();
          Store->Kind = ExprKind::Store;
          Store->Type = Type;
          Store->Operands = {Pointer, Value};
          if (Mutation == 6)
            Store->MemoryOrdering = NdMemoryOrdering::Release;
          Statement.RetVal = Store;
        }
        const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
        const bool ReleaseStore = Shape != 0 && Mutation == 6;
        EXPECT_EQ(Bound.Limitation.empty(), Mutation == 0 || ReleaseStore)
            << Bound.Limitation;
        EXPECT_EQ(Bound.LocalStorageExtents.empty(),
                  Mutation != 0 && Mutation != 7 && !ReleaseStore);
        if (!Mutation || Mutation == 7 || ReleaseStore)
          EXPECT_EQ(Bound.LocalStorageExtents,
                    (std::map<va_t, uint64_t>{{Address, 8}}));
      }
}

TEST(ObjCSourceBindings, NamedWritableAggregateFieldsShareOneRebuiltStorage) {
  Fixture F;
  constexpr va_t Base = 0x1020;
  F.Image.ObjCSourceReferences.clear();
  F.Image.Segments[0].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  F.Image.Sections[0].Flags = F.Image.Segments[0].Flags;
  F.Image.Symbols.push_back({"_MergedGlobals", Base, 0, false});
  F.Image.Symbols.push_back({"_nextStorage", Base + 0x20, 0, false});
  llvm::support::endian::write32le(
      F.Image.Segments[0].Data.data() + Base + 4 - 0x1000, 17);
  llvm::support::endian::write32le(
      F.Image.Segments[0].Data.data() + Base + 20 - 0x1000, 29);
  const auto Field = [](va_t Address) {
    return HighExpr::makeLoad(
        HighExpr::makeConst(Address, 8, ConstantAddressProvenance::DataAddress),
        NdType::makeInt(4, false));
  };
  F.Function.ReturnType = NdType::makeInt(4, false);
  F.Function.Body[0].RetVal =
      HighExpr::makeBinop(NdOp::INT_ADD, Field(Base + 4), Field(Base + 20));

  const auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  EXPECT_EQ(Result.LocalStorageExtents, (std::map<va_t, uint64_t>{{Base, 24}}));
  EXPECT_TRUE(Result.ProfileCounterSections.empty());
  const auto &LeftAddress =
      Result.Function.Body[0].RetVal->Operands[0]->Operands[0];
  const auto &RightAddress =
      Result.Function.Body[0].RetVal->Operands[1]->Operands[0];
  for (const auto &Address : {LeftAddress, RightAddress}) {
    ASSERT_EQ(Address->Kind, ExprKind::BinOp);
    ASSERT_EQ(Address->Operands.size(), 2U);
    ASSERT_TRUE(Address->Operands[0]->SourceCallHint);
    EXPECT_EQ(Address->Operands[0]->SourceCallHint->TargetAddress, Base);
    EXPECT_TRUE(objcSourceCallBound(*Address->Operands[0], F.Image, {}));
  }
  EXPECT_EQ(LeftAddress->Operands[0]->SourceCallHint->ByteCount, 8U);
  EXPECT_EQ(RightAddress->Operands[0]->SourceCallHint->ByteCount, 24U);
  std::set<std::string> Helpers;
  const auto Source = renderObjCLocalStorageHelpers(
      F.Image, Result.LocalStorageExtents, Helpers);
  EXPECT_EQ(Helpers,
            std::set<std::string>{"neverd_local_storage_1020_address"});
  EXPECT_NE(Source.find("storage[24]"), std::string::npos);
  EXPECT_NE(Source.find("[4] = 17"), std::string::npos);
  EXPECT_NE(Source.find("[20] = 29"), std::string::npos);

  auto Split = F.Image;
  Split.Symbols.push_back({"_intervening", Base + 22, 0, false});
  const auto Rejected = bindObjCSourceReferences(F.Function, Split);
  EXPECT_FALSE(Rejected.Limitation.empty());
  EXPECT_EQ(Rejected.LocalStorageExtents,
            (std::map<va_t, uint64_t>{{Base, 8}}));

  auto Relocated = F.Image;
  Relocated.DataPtrRelocSlots.insert(Base + 16);
  const auto PointerRejected = bindObjCSourceReferences(F.Function, Relocated);
  EXPECT_FALSE(PointerRejected.Limitation.empty());
  EXPECT_EQ(PointerRejected.LocalStorageExtents,
            (std::map<va_t, uint64_t>{{Base, 8}}));
}

TEST(ObjCSourceBindings, SwiftImmutableScalarStoragePreservesCompleteIdentity) {
  constexpr va_t Address = 0x1040;
  constexpr const char *Name =
      "_$sSo7CALayerC12StorageProofE13sourcePadding12CoreGraphics7CGFloatVvpZ";
  Fixture F;
  F.Image.ObjCSourceReferences.clear();
  F.Image.MachOTwoLevelNamespace = true;
  F.Image.Symbols.push_back({Name, Address, 0, false});
  llvm::support::endian::write64le(F.Image.Segments[0].Data.data() + 0x40,
                                   UINT64_C(0x40ad4c4000000000));
  auto AddressValue =
      HighExpr::makeConst(Address, 8, ConstantAddressProvenance::DataAddress);
  F.Function.ReturnType = NdType::makePtr(NdType::makeVoid());
  F.Function.Body[0].RetVal = AddressValue;
  const auto Storage = swiftImmutableScalarStorage(F.Image, Address);
  ASSERT_TRUE(Storage);
  EXPECT_EQ(Storage->SymbolName, Name);
  EXPECT_EQ(Storage->ByteCount, 8U);
  const auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  ASSERT_TRUE(Result.Function.Body[0].RetVal->SourceCallHint);
  EXPECT_EQ(Result.BorrowedBytes, (std::set<BorrowedByteRange>{{Address, 8}}));
  EXPECT_TRUE(
      objcSourceCallBound(*Result.Function.Body[0].RetVal, F.Image, {}));
  EXPECT_EQ(F.Function.Body[0].RetVal, AddressValue);
  EXPECT_EQ(AddressValue->Kind, ExprKind::Const);
}

namespace {
Fixture swiftScalarStorageFixture() {
  Fixture F;
  F.Image.ObjCSourceReferences.clear();
  F.Image.MachOTwoLevelNamespace = true;
  F.Image.Symbols.push_back(
      {"_$sSo7CALayerC12StorageProofE13sourcePadding12CoreGraphics7CGFloatVvpZ",
       0x1040, 0, false});
  llvm::support::endian::write64le(F.Image.Segments[0].Data.data() + 0x40,
                                   UINT64_C(0x40ad4c4000000000));
  F.Function.ReturnType = NdType::makePtr(NdType::makeVoid());
  F.Function.Body[0].RetVal =
      HighExpr::makeConst(0x1040, 8, ConstantAddressProvenance::DataAddress);
  return F;
}
} // namespace

TEST(ObjCSourceBindings, SwiftScalarStorageNeedsCompleteStructuredDeclaration) {
  const std::vector<std::pair<std::string, unsigned>> Cases = {
      {"_$sSo7CALayerC12StorageProofE13sourcePadding12CoreGraphics7CGFloatVvpZ",
       8},
      {"_$s4Test3BoxC7enabledSbvpZ", 1},
      {"_$s4Test3BoxV5valueSfvpZ", 4},
      {"_$s4Test3BoxO5values6UInt16VvpZ", 2},
      {"_$sSo7CALayerC12StorageProofE5valueSivpZ", 8},
      {"_$s4Test3BoxC5valueSdvpZ", 8},
      {"_$s4Test3BoxC5valueSdvp", 0},
      {"_$s4Test3BoxC5valueSdvgZ", 0},
      {"_$s4Test3BoxC5valueSdvau", 0},
      {"_$s4Test3BoxC5valueSdvMZ", 0},
      {"_$s4Test3BoxC5valueSSvpZ", 0},
      {"_$s4Test3BoxC5valueSdSgvpZ", 0},
      {"_$s4Test3BoxC5value12CoreGraphics7CGFloatCvpZ", 0},
      {"_$s4Test3BoxC5value12CoreGraphics7CGPointVvpZ", 0},
      {"_$s4Test3BoxC5value4Fake7CGFloatVvpZ", 0},
      {"_$s4Test3BoxC5value14CoreFoundation7CGFloatVvpZ", 0},
      {"_$s4Test3BoxC5valueSdvpZsuffix", 0},
      {std::string(8001, 's'), 0}};
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (const auto &[Name, Width] : Cases) {
      SCOPED_TRACE(Name);
      auto F = swiftScalarStorageFixture();
      F.Image.Arch = Architecture;
      F.Image.Symbols[0].Name = Name;
      const auto Storage = swiftImmutableScalarStorage(F.Image, 0x1040);
      EXPECT_EQ(Storage ? Storage->ByteCount : 0, Width);
    }
}

TEST(ObjCSourceBindings, SwiftScalarStorageRejectsAmbiguousOrMutableImage) {
  for (unsigned Mutation = 0; Mutation < 32; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = swiftScalarStorageFixture();
    auto &I = F.Image;
    va_t Address = 0x1040;
    switch (Mutation) {
    case 0:
      I.Format = BinaryFormat::ELF;
      break;
    case 1:
      I.Format = BinaryFormat::COFF;
      break;
    case 2:
      I.Bits = Bitness::Bits32;
      break;
    case 3:
      I.Arch = Arch::ARM;
      break;
    case 4:
      I.IsRelocatable = true;
      break;
    case 5:
      I.MachOTwoLevelNamespace = false;
      break;
    case 6:
      I.MachOChainedFixupsAmbiguous = true;
      break;
    case 7:
      I.Symbols.clear();
      break;
    case 8:
      I.Symbols[0].IsFunc = true;
      break;
    case 9:
      I.Symbols[0].Size = 4;
      break;
    case 10:
      I.Symbols[0].Size = 16;
      break;
    case 11:
      I.Symbols.push_back(I.Symbols[0]);
      break;
    case 12:
      I.Symbols.push_back({"other", 0x1044, 0, false});
      break;
    case 13:
      I.Symbols.push_back({"other", 0x1044, 0, true});
      break;
    case 14:
      I.Symbols.push_back({"other", 0x1038, 16, false});
      break;
    case 15:
      I.Symbols.push_back({I.Symbols[0].Name, 0x1050, 0, false});
      break;
    case 16:
      I.Sections[0].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      break;
    case 17:
      I.Segments[0].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      break;
    case 18:
      I.Sections[0].FileSz = 0x44;
      break;
    case 19:
      I.Segments[0].FileSz = 0x44;
      break;
    case 20:
      I.Sections.push_back(I.Sections[0]);
      break;
    case 21:
      I.Segments.push_back(I.Segments[0]);
      break;
    case 22:
      I.Sections[0].Type = llvm::MachO::S_THREAD_LOCAL_REGULAR;
      break;
    case 23:
      I.DataPtrRelocSlots.insert(0x1040);
      break;
    case 24:
      I.CodePtrRelocSlots.insert(0x1040);
      break;
    case 25:
      I.ImportPtrSlots[0x1040] = "external";
      break;
    case 26:
      I.DataPtrRelocSlots.insert(0x103c);
      break;
    case 27:
      I.DataPtrRelocSlots.insert(0x1044);
      break;
    case 28:
      llvm::support::endian::write64le(I.Segments[0].Data.data() + 0x40,
                                       0x1020);
      break;
    case 29:
      Address = I.Symbols[0].Addr = 0x1041;
      break;
    case 30:
      I.Sections[0].FileOff = 1;
      break;
    case 31:
      I.RuntimeFunctionAddrs.insert(0x1044);
      break;
    }
    EXPECT_FALSE(swiftImmutableScalarStorage(I, Address));
  }
}

TEST(ObjCSourceBindings, SwiftScalarStoragePublicationRevalidatesCurrentProof) {
  auto F = swiftScalarStorageFixture();
  auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  const auto &Original = *Result.Function.Body[0].RetVal;
  ASSERT_TRUE(Original.SourceCallHint);
  for (unsigned Mutation = 0; Mutation < 23; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto I = F.Image;
    auto E = Original;
    auto Mutable = std::make_shared<SourceCallTypeHint>(*E.SourceCallHint);
    E.SourceCallHint = Mutable;
    auto &H = *Mutable;
    switch (Mutation) {
    case 0:
      I.Symbols[0].Name = "unknown";
      break;
    case 1:
      I.Symbols[0].Size = 4;
      break;
    case 2:
      I.DataPtrRelocSlots.insert(0x1040);
      break;
    case 3:
      I.Segments[0].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      break;
    case 4:
      H.TargetAddress += 8;
      break;
    case 5:
      H.TargetName += "stale";
      break;
    case 6:
      H.ByteCount = 4;
      break;
    case 7:
      H.ByteCount = 16;
      break;
    case 8:
      H.Signature.ReturnLocation.ValueBytes = 4;
      break;
    case 9:
      H.Signature.ReturnLocation.RegisterOffset += 8;
      break;
    case 10:
      H.Signature.ReturnType = NdType::makeInt(8);
      break;
    case 11:
      H.OwnerClass = "CALayer";
      break;
    case 12:
      H.Selector = "sourcePadding";
      break;
    case 13:
      H.SelectorReferenceAddress = 0x1080;
      break;
    case 14:
      H.WeakImport = true;
      break;
    case 15:
      E.Operands.push_back(HighExpr::makeConst(0, 8));
      break;
    case 16:
      E.CallAddr = 0x2000;
      break;
    case 17:
      E.CallTarget = "forged";
      break;
    case 18:
      E.IsIndirectCall = true;
      break;
    case 19:
      E.MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 20:
      E.Type = NdType::makeFloat(8);
      break;
    case 21:
      E.Type = NdType::makeInt(4);
      break;
    case 22:
      H.CallKind = SourceCallTypeHint::Kind::RuntimeReadOnlyBytes;
      H.TargetName.clear();
      break;
    }
    EXPECT_FALSE(objcSourceCallBound(E, I, {}));
  }
  std::set<std::string> Helpers;
  const auto Text =
      renderBorrowedByteHelpers(F.Image, Result.BorrowedBytes, Helpers);
  EXPECT_NE(Text.find("_Alignas(16)"), std::string::npos);
  EXPECT_EQ(Helpers.size(), 1U);
  auto Stale = F.Image;
  Stale.DataPtrRelocSlots.insert(0x1040);
  EXPECT_THROW(renderBorrowedByteHelpers(Stale, Result.BorrowedBytes, Helpers),
               std::runtime_error);
}

TEST(ObjCSourceBindings,
     SwiftBeginAccessUsesTypedOrMemoryAccessProvedLocalStorage) {
  Fixture F;
  constexpr va_t Address = 0x1040;
  constexpr va_t Slot = 0x10e0;
  F.Image.ObjCSourceReferences.clear();
  F.Image.Segments[0].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  F.Image.Sections[0].Flags = F.Image.Segments[0].Flags;
  F.Image.Symbols.push_back({"_$s4Test3BoxC7enabledSbvpZ", Address, 1, false});
  F.Image.Segments[0].Data[Address - 0x1000] = 1;
  F.Image.ImportPtrSlots[Slot] = "_swift_beginAccess";
  ASSERT_TRUE(F.Image.recordDyldBindSlot(Slot, "_swift_beginAccess", 0,
                                         "/usr/lib/swift/libswiftCore.dylib",
                                         false));
  const auto Hint = swiftRuntimeSourceCallHint(F.Image, Slot);
  ASSERT_TRUE(Hint);

  auto Call = HighExpr::makeCall(
      "swift_beginAccess", Slot,
      {HighExpr::makeConst(Address, 8, ConstantAddressProvenance::DataAddress),
       HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8),
       HighExpr::makeConst(0, 8)});
  Call->Type = NdType::makeVoid();
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
  HighStmt Access;
  Access.Kind = StmtKind::ExprStmt;
  Access.CallExpr = Call;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeLoad(
      HighExpr::makeConst(Address, 8, ConstantAddressProvenance::DataAddress),
      NdType::makeInt(1, false));
  F.Function.ReturnType = NdType::makeInt(1, false);
  F.Function.Body = {Access, Return};

  const auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  EXPECT_EQ(Result.LocalStorageExtents,
            (std::map<va_t, uint64_t>{{Address, 1}}));
  const auto Marker = Result.Function.Body[0].CallExpr->Operands[0];
  ASSERT_TRUE(Marker->SourceCallHint);
  EXPECT_EQ(Marker->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeLocalStorageAddress);
  EXPECT_EQ(Marker->SourceCallHint->TargetAddress, Address);
  EXPECT_EQ(Marker->SourceCallHint->ByteCount, 1U);
  EXPECT_TRUE(objcSourceCallBound(*Marker, F.Image, {}));
  const auto Storage = Result.Function.Body[1].RetVal->Operands[0];
  ASSERT_TRUE(Storage->SourceCallHint);
  EXPECT_EQ(Storage->SourceCallHint->TargetAddress, Address);

  auto Forged = *Hint;
  Forged.Signature.Parameters[0].Location.RegisterOffset += 8;
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Forged);
  const auto Rejected = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_FALSE(Rejected.Limitation.empty());
  EXPECT_EQ(Rejected.Function.Body[0].CallExpr->Operands[0]->Kind,
            ExprKind::Const);

  const auto RebuiltAddress = [] {
    ExprPtr Result = HighExpr::makeConst((Address >> 56) & 0xff, 1);
    for (int Shift = 48, Width = 2; Shift >= 0; Shift -= 8, ++Width) {
      Result = HighExpr::makeBinop(
          NdOp::CONCAT, Result,
          HighExpr::makeConst((Address >> Shift) & 0xff, 1));
      Result->Type = NdType::makeInt(Width, false);
    }
    return Result;
  };
  const auto MarkerFunction = [&](ExprPtr AddressExpression) {
    auto MarkerCall = HighExpr::makeCall(
        "swift_beginAccess", Slot,
        {std::move(AddressExpression), HighExpr::makeConst(0, 8),
         HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8)});
    MarkerCall->Type = NdType::makeVoid();
    MarkerCall->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    HighStmt Marker;
    Marker.Kind = StmtKind::ExprStmt;
    Marker.CallExpr = std::move(MarkerCall);
    HighFunc Function;
    Function.ReturnType = NdType::makeVoid();
    Function.Body = {std::move(Marker)};
    return Function;
  };
  const auto Typed =
      bindObjCSourceReferences(MarkerFunction(RebuiltAddress()), F.Image);
  ASSERT_TRUE(Typed.Limitation.empty()) << Typed.Limitation;
  EXPECT_EQ(Typed.LocalStorageExtents,
            (std::map<va_t, uint64_t>{{Address, 1}}));
  ASSERT_TRUE(Typed.Function.Body[0].CallExpr->Operands[0]->SourceCallHint);
  EXPECT_EQ(
      Typed.Function.Body[0].CallExpr->Operands[0]->SourceCallHint->ByteCount,
      1U);

  auto InvalidShift = HighExpr::makeBinop(
      NdOp::INT_LEFT,
      HighExpr::makeConst(Address, 8, ConstantAddressProvenance::DataAddress),
      HighExpr::makeConst(64, 8));
  InvalidShift->Type = NdType::makeInt(8, false);
  const auto UnboundedShift = bindObjCSourceReferences(
      MarkerFunction(std::move(InvalidShift)), F.Image);
  EXPECT_FALSE(UnboundedShift.Limitation.empty());
  EXPECT_TRUE(UnboundedShift.LocalStorageExtents.empty());

  for (const char *Name : {"_untypedStorage", "_$s4Test3BoxC5valueSSvpZ",
                           "_$s4Test3BoxC7enabledSbvgZ"}) {
    SCOPED_TRACE(Name);
    const auto Symbol = llvm::find_if(F.Image.Symbols, [](const auto &S) {
      return S.Addr == Address && !S.IsFunc;
    });
    ASSERT_NE(Symbol, F.Image.Symbols.end());
    Symbol->Name = Name;
    EXPECT_FALSE(swiftStaticScalarStorageWidth(Symbol->Name));
    const auto Function = MarkerFunction(RebuiltAddress());
    EXPECT_TRUE(
        objc_binding_detail::directLocalStorageAccessExtents(Function, F.Image)
            .empty());
    const auto Unproved = bindObjCSourceReferences(Function, F.Image);
    EXPECT_FALSE(Unproved.Limitation.empty());
    EXPECT_TRUE(Unproved.LocalStorageExtents.empty());
    EXPECT_EQ(Unproved.Function.Body[0].CallExpr->Operands[0]->Kind,
              ExprKind::BinOp);
  }

  const auto Symbol = llvm::find_if(F.Image.Symbols, [](const auto &S) {
    return S.Addr == Address && !S.IsFunc;
  });
  ASSERT_NE(Symbol, F.Image.Symbols.end());
  Symbol->Name = "_$s4Test3BoxC5countSivpZ";
  F.Image.Symbols.push_back({"_nextStorage", Address + 4, 1, false});
  const auto Overlapping =
      bindObjCSourceReferences(MarkerFunction(RebuiltAddress()), F.Image);
  EXPECT_FALSE(Overlapping.Limitation.empty());
  EXPECT_TRUE(Overlapping.LocalStorageExtents.empty());
}

TEST(ObjCSourceBindings, PrivateSwiftScalarAliasesKeepOneAssociationKey) {
  constexpr va_t Address = 0x1040;
  constexpr va_t SwiftSlot = 0x10d0;
  constexpr va_t ObjCSlot = 0x10e0;
  constexpr const char *Symbol = "_$s7WMFData21wmfLanguageVariantKey33_"
                                 "6ED6687F0FBB5F3BAB3A4BCD1578B756LLs5UInt8Vvp";
  ASSERT_EQ(swiftPrivateScalarStorageWidth(Symbol), 1U);
  EXPECT_FALSE(swiftStaticScalarStorageWidth(Symbol));
  EXPECT_FALSE(
      swiftPrivateScalarStorageWidth("_$s7WMFData21wmfLanguageVariantKey33_"
                                     "6ED6687F0FBB5F3BAB3A4BCD1578B756LLSSvp"));
  EXPECT_FALSE(swiftPrivateScalarStorageWidth("_$s4Test3BoxC7enabledSbvpZ"));

  Fixture F;
  F.Image.ObjCSourceReferences.clear();
  F.Image.Segments[0].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  F.Image.Sections[0].Flags = F.Image.Segments[0].Flags;
  F.Image.Symbols.push_back({Symbol, Address, 1, false});
  F.Image.ImportPtrSlots[SwiftSlot] = "_swift_beginAccess";
  ASSERT_TRUE(F.Image.recordDyldBindSlot(SwiftSlot, "_swift_beginAccess", 0,
                                         "/usr/lib/swift/libswiftCore.dylib",
                                         false));
  F.Image.ImportPtrSlots[ObjCSlot] = "_objc_setAssociatedObject";
  F.Image.DynInfo.NeededLibs.push_back(
      "/System/Library/Frameworks/Foundation.framework/Foundation");
  const auto SwiftHint = swiftRuntimeSourceCallHint(F.Image, SwiftSlot);
  const auto ObjCHint = objcRuntimeSourceCallHint(F.Image, ObjCSlot);
  ASSERT_TRUE(SwiftHint);
  ASSERT_TRUE(ObjCHint);

  MedVar Key;
  Key.Kind = MedVar::Temp;
  Key.Id = 42;
  Key.Size = 8;
  auto Local = [&] { return HighExpr::makeVar(Key, NdType::makeInt(8)); };
  HighStmt Assign;
  Assign.Kind = StmtKind::Assign;
  Assign.Dst = Local();
  Assign.Val =
      HighExpr::makeConst(Address, 8, ConstantAddressProvenance::DataAddress);
  auto Access = HighExpr::makeCall("swift_beginAccess", SwiftSlot,
                                   {Local(), HighExpr::makeConst(0, 8),
                                    HighExpr::makeConst(0, 8),
                                    HighExpr::makeConst(0, 8)});
  Access->Type = NdType::makeVoid();
  Access->SourceCallHint = std::make_shared<SourceCallTypeHint>(*SwiftHint);
  HighStmt AccessStatement;
  AccessStatement.Kind = StmtKind::ExprStmt;
  AccessStatement.CallExpr = Access;
  auto Set = HighExpr::makeCall("objc_setAssociatedObject", ObjCSlot,
                                {HighExpr::makeConst(0, 8), Local(),
                                 HighExpr::makeConst(0, 8),
                                 HighExpr::makeConst(3, 8)});
  Set->Type = NdType::makeVoid();
  Set->SourceCallHint = std::make_shared<SourceCallTypeHint>(*ObjCHint);
  HighStmt SetStatement;
  SetStatement.Kind = StmtKind::ExprStmt;
  SetStatement.CallExpr = Set;
  auto Direct = HighExpr::makeCall(
      "objc_setAssociatedObject", ObjCSlot,
      {HighExpr::makeConst(0, 8),
       HighExpr::makeConst(Address, 8, ConstantAddressProvenance::DataAddress),
       HighExpr::makeConst(0, 8), HighExpr::makeConst(3, 8)});
  Direct->Type = NdType::makeVoid();
  Direct->SourceCallHint = std::make_shared<SourceCallTypeHint>(*ObjCHint);
  HighStmt DirectStatement;
  DirectStatement.Kind = StmtKind::ExprStmt;
  DirectStatement.CallExpr = Direct;
  const auto Registration = objcSelectorSourceTypeHint(
      F.Image, "addObserver:forKeyPath:options:context:");
  ASSERT_TRUE(Registration);
  auto RegistrationHint = std::make_shared<SourceCallTypeHint>();
  RegistrationHint->CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  RegistrationHint->TargetName = "objc_msgSend";
  RegistrationHint->Selector = "addObserver:forKeyPath:options:context:";
  RegistrationHint->Signature = *Registration;
  std::vector<ExprPtr> RegistrationArgs;
  for (size_t Index = 0; Index < Registration->Parameters.size(); ++Index)
    RegistrationArgs.push_back(
        HighExpr::makeConst(Index == 5 ? Address : 0, 8,
                            Index == 5 ? ConstantAddressProvenance::DataAddress
                                       : ConstantAddressProvenance::Unknown));
  auto Register = HighExpr::makeCall("objc_msgSend", 0, RegistrationArgs);
  Register->Type = Registration->ReturnType;
  Register->SourceCallHint = RegistrationHint;
  HighStmt RegistrationStatement;
  RegistrationStatement.Kind = StmtKind::ExprStmt;
  RegistrationStatement.CallExpr = Register;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  F.Function.ReturnType = NdType::makeVoid();
  F.Function.Body = {Assign,          AccessStatement,       SetStatement,
                     DirectStatement, RegistrationStatement, Return};

  const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  EXPECT_EQ(Bound.LocalStorageExtents,
            (std::map<va_t, uint64_t>{{Address, 1}}));
  EXPECT_TRUE(Bound.AssociationKeys.empty());
  EXPECT_TRUE(Bound.KVOContexts.empty());
  const auto Storage = Bound.Function.Body[0].Val;
  ASSERT_TRUE(Storage->SourceCallHint);
  EXPECT_EQ(Storage->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeLocalStorageAddress);
  EXPECT_EQ(Storage->SourceCallHint->TargetAddress, Address);
  EXPECT_TRUE(objcSourceCallBound(*Storage, F.Image, {}));
  EXPECT_EQ(Bound.Function.Body[1].CallExpr->Operands[0]->Kind, ExprKind::Var);
  EXPECT_EQ(Bound.Function.Body[2].CallExpr->Operands[1]->Kind, ExprKind::Var);
  const auto DirectKey = Bound.Function.Body[3].CallExpr->Operands[1];
  ASSERT_TRUE(DirectKey->SourceCallHint);
  EXPECT_EQ(DirectKey->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeLocalStorageAddress);
  EXPECT_EQ(DirectKey->SourceCallHint->TargetAddress, Address);
  const auto RegisteredContext = Bound.Function.Body[4].CallExpr->Operands[5];
  ASSERT_TRUE(RegisteredContext->SourceCallHint);
  EXPECT_EQ(RegisteredContext->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeLocalStorageAddress);
  EXPECT_EQ(RegisteredContext->SourceCallHint->TargetAddress, Address);

  // ADRP+ADD can preserve an exact image address without classifying its
  // section in the IR. The uniquely named writable Swift scalar proves it.
  auto GenericAddress = F.Function;
  GenericAddress.Body[0].Val =
      HighExpr::makeConst(Address, 8, ConstantAddressProvenance::Address);
  const auto GenericBound = bindObjCSourceReferences(GenericAddress, F.Image);
  ASSERT_TRUE(GenericBound.Limitation.empty()) << GenericBound.Limitation;
  ASSERT_TRUE(GenericBound.Function.Body[0].Val->SourceCallHint);
  EXPECT_EQ(GenericBound.Function.Body[0].Val->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeLocalStorageAddress);
  EXPECT_EQ(GenericBound.LocalStorageExtents,
            (std::map<va_t, uint64_t>{{Address, 1}}));

  auto Scalar = F.Function;
  Scalar.Body[0].Val =
      HighExpr::makeConst(Address, 8, ConstantAddressProvenance::Scalar);
  EXPECT_FALSE(bindObjCSourceReferences(Scalar, F.Image).Limitation.empty());

  ObjCMethod Callback;
  Callback.Implementation = 0x2000;
  Callback.ClassName = "Observer";
  Callback.Selector = "observeValueForKeyPath:ofObject:change:context:";
  Callback.TypeEncoding = "v48@0:8@16@24@32^v40";
  Callback.Status = "supported";
  Callback.TypeHint =
      parseObjCMethodEncoding(Callback.Selector, Callback.TypeEncoding);
  ASSERT_TRUE(Callback.TypeHint);
  std::string Reason;
  ASSERT_TRUE(
      assignDarwinObjCSourceABI(*Callback.TypeHint, F.Image.Arch, Reason))
      << Reason;
  auto CallbackImage = F.Image;
  CallbackImage.ObjCMethods = {Callback};
  HighFunc CallbackFunction;
  CallbackFunction.Entry = Callback.Implementation;
  CallbackFunction.SourceTypeHint = Callback.TypeHint;
  for (const auto &Parameter : Callback.TypeHint->Parameters)
    CallbackFunction.Params.push_back({Parameter.Name, Parameter.Type});
  MedVar ContextParameter;
  ContextParameter.Kind = MedVar::Param;
  ContextParameter.Id = 5;
  ContextParameter.Size = 8;
  HighStmt Compare;
  Compare.Kind = StmtKind::Return;
  Compare.RetVal = HighExpr::makeBinop(
      NdOp::INT_EQUAL,
      HighExpr::makeVar(ContextParameter,
                        Callback.TypeHint->Parameters[5].Type),
      HighExpr::makeConst(Address, 8, ConstantAddressProvenance::DataAddress));
  CallbackFunction.Body = {Compare};
  const auto Compared =
      bindObjCSourceReferences(CallbackFunction, CallbackImage);
  ASSERT_TRUE(Compared.Limitation.empty()) << Compared.Limitation;
  EXPECT_EQ(Compared.LocalStorageExtents,
            (std::map<va_t, uint64_t>{{Address, 1}}));
  EXPECT_TRUE(Compared.KVOContexts.empty());
  const auto ComparedContext = Compared.Function.Body[0].RetVal->Operands[1];
  ASSERT_TRUE(ComparedContext->SourceCallHint);
  EXPECT_EQ(ComparedContext->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeLocalStorageAddress);
  EXPECT_EQ(ComparedContext->SourceCallHint->TargetAddress, Address);

  for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
    auto Image = F.Image;
    if (Mutation == 0)
      Image.Symbols[0].Name = "_untypedStorage";
    if (Mutation == 1)
      Image.Symbols.push_back({"_alias", Address, 1, false});
    if (Mutation == 2)
      Image.Sections[0].Flags = SegmentFlags::Readable;
    if (Mutation == 3)
      Image.DataPtrRelocSlots.insert(Address);
    const auto Rejected = bindObjCSourceReferences(F.Function, Image);
    EXPECT_FALSE(Rejected.Limitation.empty()) << Mutation;
    EXPECT_TRUE(Rejected.LocalStorageExtents.empty()) << Mutation;
  }
}

TEST(ObjCSourceBindings,
     OnceTokensBindOnlyAuthenticatedZeroInitializedStorage) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    Fixture F;
    F.Image.Arch = Architecture;
    constexpr va_t Address = 0x1040;
    constexpr va_t Slot = 0x10e0;
    F.Image.ObjCSourceReferences.clear();
    F.Image.Segments[0].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    F.Image.Sections[0].Flags = F.Image.Segments[0].Flags;
    F.Image.Symbols.push_back({"_onceToken", Address, 8, false});
    F.Image.ImportPtrSlots[Slot] = "_dispatch_once";
    ASSERT_TRUE(F.Image.recordDyldBindSlot(
        Slot, "_dispatch_once", 0, "/usr/lib/system/libdispatch.dylib", false));
    const auto Hint = darwinRuntimeSourceCallHint(F.Image, Slot);
    ASSERT_TRUE(Hint);
    auto Call = HighExpr::makeCall(
        "dispatch_once", Slot,
        {HighExpr::makeConst(Address, 8,
                             ConstantAddressProvenance::DataAddress),
         HighExpr::makeConst(0, 8)});
    Call->Type = NdType::makeVoid();
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    F.Function.Body[0].RetVal = Call;
    const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_EQ(Bound.LocalStorageExtents,
              (std::map<va_t, uint64_t>{{Address, 8}}));
    EXPECT_TRUE(objcSourceCallBound(*Bound.Function.Body[0].RetVal->Operands[0],
                                    F.Image, {}));
    for (unsigned Case = 0; Case < 4; ++Case) {
      auto Image = F.Image;
      auto Forged = *Hint;
      if (Case == 0)
        Image.Segments[0].Data[Address - Image.Segments[0].VA] = 1;
      if (Case == 1)
        Image.Symbols.clear();
      if (Case == 2)
        Forged.Signature.Parameters[0].Location.RegisterOffset += 8;
      if (Case == 3)
        Image.DyldBindSlots[Slot].Module = "/tmp/libdispatch.dylib";
      Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Forged);
      EXPECT_TRUE(bindObjCSourceReferences(F.Function, Image)
                      .LocalStorageExtents.empty())
          << Case;
    }
    EXPECT_FALSE(
        objc_binding_detail::oncePredicateStorageHint(F.Image, Address + 1));
  }
}

TEST(ObjCSourceBindings, UnfairLocksBindExactNamedFourByteStorage) {
  Fixture F;
  constexpr va_t Address = 0x1040;
  constexpr va_t Slot = 0x10e0;
  F.Image.ObjCSourceReferences.clear();
  F.Image.Segments[0].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  F.Image.Sections[0].Flags = F.Image.Segments[0].Flags;
  F.Image.Symbols.push_back({"_providerLock", Address, 4, false});
  F.Image.ImportPtrSlots[Slot] = "_os_unfair_lock_lock";
  ASSERT_TRUE(F.Image.recordDyldBindSlot(
      Slot, "_os_unfair_lock_lock", 0,
      "/usr/lib/system/libsystem_platform.dylib", false));
  const auto Hint = darwinRuntimeSourceCallHint(F.Image, Slot);
  ASSERT_TRUE(Hint);
  auto Call = HighExpr::makeCall(
      "os_unfair_lock_lock", Slot,
      {HighExpr::makeConst(Address, 8,
                           ConstantAddressProvenance::DataAddress)});
  Call->Type = NdType::makeVoid();
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
  F.Function.Body[0].RetVal = Call;

  const auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  EXPECT_EQ(Result.LocalStorageExtents,
            (std::map<va_t, uint64_t>{{Address, 4}}));
  const auto Bound = Result.Function.Body[0].RetVal->Operands[0];
  ASSERT_TRUE(Bound->SourceCallHint);
  EXPECT_EQ(Bound->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeLocalStorageAddress);
  EXPECT_TRUE(objcSourceCallBound(*Bound, F.Image, {}));

  auto Forged = *Hint;
  Forged.Signature.Parameters[0].Location.RegisterOffset += 8;
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Forged);
  const auto Rejected = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_TRUE(Rejected.LocalStorageExtents.empty());
  EXPECT_EQ(Rejected.Function.Body[0].RetVal->Operands[0]->Kind,
            ExprKind::Const);
  EXPECT_FALSE(
      objcSourceCallBound(*Rejected.Function.Body[0].RetVal, F.Image, {}));
}

namespace {
struct ProfileFixture : Fixture {
  ProfileFixture() {
    Image.ObjCSourceReferences.clear();
    Image.Segments[0].Name = "__DATA";
    Image.Segments[0].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Image.Sections[0].Name = "__llvm_prf_cnts";
    Image.Sections[0].SegmentName = "__DATA";
    Image.Sections[0].Flags = Image.Segments[0].Flags;
    for (size_t I = 0; I < 0x100; ++I)
      Image.Segments[0].Data[I] = uint8_t(I * 37 + 9);
  }
};
} // namespace

TEST(ObjCSourceBindings, ProfileCountersKeepOverlappingStorageAndAccessWidths) {
  ProfileFixture F;
  const ObjCProfileStorage Storage(F.Image);
  for (uint16_t Width : {1, 2, 4, 8, 16}) {
    auto Address = HighExpr::makeConst(0x1007, 8);
    F.Function.Body[0].RetVal =
        HighExpr::makeLoad(Address, NdType::makeInt(Width, false));
    const auto Result = bindObjCSourceReferences(F.Function, F.Image, &Storage);
    EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
    EXPECT_EQ(Result.ProfileCounterSections, std::set<va_t>{0x1000});
    const auto Load = Result.Function.Body[0].RetVal;
    EXPECT_EQ(Load->Kind, ExprKind::Load);
    EXPECT_EQ(Load->Type->Size, Width);
    const auto BoundAddress = Load->Operands[0];
    ASSERT_EQ(BoundAddress->Kind, ExprKind::BinOp);
    EXPECT_EQ(BoundAddress->Operands[1]->ConstVal, 7U);
    EXPECT_TRUE(
        objcSourceCallBound(*BoundAddress->Operands[0], F.Image, {}, &Storage));
    EXPECT_EQ(Address->ConstVal, 0x1007U);
    // A shared node used outside a memory access still has no source binding.
    F.Function.Body[0].RetVal =
        HighExpr::makeBinop(NdOp::INT_ADD, F.Function.Body[0].RetVal, Address);
    EXPECT_FALSE(bindObjCSourceReferences(F.Function, F.Image, &Storage)
                     .Limitation.empty());
  }
  EXPECT_EQ(Storage.sectionFor(0x10f0, 16), 0x1000U);
  EXPECT_FALSE(Storage.sectionFor(0x10f1, 16));
  EXPECT_FALSE(Storage.sectionFor(UINT64_MAX - 7, 16));
  EXPECT_FALSE(Storage.sectionFor(0x1000, 0));
  std::set<std::string> Names;
  const auto Source = Storage.render({0x1000}, Names);
  EXPECT_EQ(Names,
            std::set<std::string>{"neverd_profile_counters_1000_address"});
  EXPECT_NE(Source.find("[0] = 9"), std::string::npos);
  EXPECT_NE(Source.find("counters[256]"), std::string::npos);
}

TEST(ObjCSourceBindings,
     SelectedProfileCounterPointersNeedOnlyBoundedNumericUses) {
  for (unsigned Mutation = 0; Mutation < 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ProfileFixture F;
    const ObjCProfileStorage Storage(F.Image);
    MedVar Pointer;
    Pointer.Kind = Mutation == 8 ? MedVar::Param : MedVar::Reg;
    Pointer.Id = 42;
    Pointer.Size = 8;
    Pointer.TheArch = F.Image.Arch;
    auto Var = HighExpr::makeVar(Pointer, NdType::makeInt(8, false));
    auto First =
        HighExpr::makeConst(0x1020, 8, ConstantAddressProvenance::DataAddress);
    auto Second = HighExpr::makeConst(
        Mutation == 1 ? 0x10f8 : 0x1040, 8,
        Mutation == 2 ? ConstantAddressProvenance::Scalar
                      : ConstantAddressProvenance::DataAddress);
    HighStmt A, B, Return;
    A.Kind = B.Kind = StmtKind::Assign;
    A.Dst = B.Dst = Var;
    A.Val = First;
    B.Val = Second;
    Return.Kind = StmtKind::Return;
    auto Address =
        HighExpr::makeBinop(NdOp::INT_ADD, Var, HighExpr::makeConst(16, 8));
    Return.RetVal = HighExpr::makeLoad(
        Address, Mutation == 3 ? NdType::makePtr(NdType::makeVoid())
                               : NdType::makeInt(8, false));
    if (Mutation == 4)
      Return.RetVal = Var;
    if (Mutation == 5)
      Return.RetVal->MemoryOrdering = NdMemoryOrdering::Acquire;
    if (Mutation == 6)
      B.Val = HighExpr::makeVar(Pointer, NdType::makeInt(8, false));
    if (Mutation == 7)
      Return.RetVal = First;
    F.Function.Body = {A, B, Return};
    if (Mutation == 9) {
      MedVar Condition;
      Condition.Kind = MedVar::Param;
      Condition.Id = 0;
      Condition.Size = 1;
      Condition.TheArch = F.Image.Arch;
      HighStmt Branch;
      Branch.Kind = StmtKind::IfElse;
      Branch.Cond = HighExpr::makeVar(Condition, NdType::makeInt(1, false));
      Branch.Body = {A, B};
      F.Function.Body = {Branch, Return};
    }
    const auto Result = bindObjCSourceReferences(F.Function, F.Image, &Storage);
    if (Mutation == 0) {
      EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
      EXPECT_EQ(Result.ProfileCounterSections, std::set<va_t>{0x1000});
      for (unsigned I = 0; I < 2; ++I) {
        auto Bound = Result.Function.Body[I].Val;
        ASSERT_EQ(Bound->Kind, ExprKind::BinOp);
        ASSERT_TRUE(Bound->Operands[0]->SourceCallHint);
        EXPECT_TRUE(
            objcSourceCallBound(*Bound->Operands[0], F.Image, {}, &Storage));
      }
    } else {
      EXPECT_FALSE(Result.Limitation.empty());
      EXPECT_TRUE(Result.ProfileCounterSections.empty());
    }
  }
}

TEST(ObjCSourceBindings,
     NativeProfileCounterArgumentsRequireBoundedCalleeAccesses) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    ProfileFixture F;
    F.Image.Arch = Architecture;
    const ObjCProfileStorage Storage(F.Image);

    HighFunc Callee;
    Callee.Entry = 0x2000;
    Callee.Name = "increment_profile_counter";
    Callee.ReturnType = NdType::makeVoid();
    const auto PointerType = NdType::makePtr(NdType::makeVoid());
    Callee.Params = {{"counter", PointerType}};
    SourceFunctionTypeHint Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Signature.ReturnType = Callee.ReturnType;
    Signature.Parameters = {{"counter", PointerType}};
    std::string Error;
    ASSERT_TRUE(assignDarwinScalarSourceABI(Signature, Architecture, Error))
        << Error;
    Callee.SourceTypeHint = Signature;

    MedVar Parameter;
    Parameter.Kind = MedVar::Param;
    Parameter.Id = 0;
    Parameter.Size = 8;
    Parameter.TheArch = Architecture;
    const auto Counter = HighExpr::makeVar(Parameter, NdType::makeInt(8));
    const auto ValueType = NdType::makeInt(8, false);
    auto Load = HighExpr::makeLoad(Counter, ValueType);
    HighStmt Store;
    Store.Kind = StmtKind::Store;
    Store.StoreAddr = Counter;
    Store.StoreVal =
        HighExpr::makeBinop(NdOp::INT_ADD, Load, HighExpr::makeConst(1, 8));
    HighStmt CalleeReturn;
    CalleeReturn.Kind = StmtKind::Return;
    Callee.Body = {Store, CalleeReturn};

    auto Binding = std::make_shared<SourceCallTypeHint>();
    Binding->CallKind = SourceCallTypeHint::Kind::Native;
    Binding->TargetAddress = Callee.Entry;
    Binding->TargetName = Callee.Name;
    Binding->Signature = Signature;
    auto Call = HighExpr::makeCall(
        Callee.Name, Callee.Entry,
        {HighExpr::makeConst(0x1040, 8,
                             ConstantAddressProvenance::DataAddress)});
    Call->Type = NdType::makeVoid();
    Call->SourceCallHint = Binding;
    HighStmt CallerReturn;
    CallerReturn.Kind = StmtKind::Return;
    CallerReturn.RetVal = Call;
    F.Function.ReturnType = NdType::makeVoid();
    F.Function.Body = {CallerReturn};
    const std::map<va_t, const HighFunc *> Functions{{Callee.Entry, &Callee}};

    const auto Result =
        bindObjCSourceReferences(F.Function, F.Image, &Storage, &Functions);
    ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
    EXPECT_EQ(Result.ProfileCounterSections, std::set<va_t>{0x1000});
    const auto Argument = Result.Function.Body[0].RetVal->Operands[0];
    ASSERT_EQ(Argument->Kind, ExprKind::BinOp);
    ASSERT_EQ(Argument->Operands.size(), 2U);
    EXPECT_EQ(Argument->Operands[1]->ConstVal, 0x40U);
    ASSERT_TRUE(Argument->Operands[0]->SourceCallHint);
    EXPECT_EQ(Argument->Operands[0]->SourceCallHint->CallKind,
              SourceCallTypeHint::Kind::RuntimeProfileCounterStorage);
    EXPECT_TRUE(objcSourceCallBound(*Argument->Operands[0], F.Image, Functions,
                                    &Storage));
    EXPECT_TRUE(objcSourceCallBound(*Result.Function.Body[0].RetVal, F.Image,
                                    Functions, &Storage));
  }
}

TEST(ObjCSourceBindings,
     NativeNamedStorageArgumentsRequireBoundedCalleeAccesses) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
      SCOPED_TRACE(unsigned(Architecture));
      SCOPED_TRACE(Mutation);
      Fixture F;
      F.Image.Arch = Architecture;
      F.Image.ObjCSourceReferences.clear();
      F.Image.Segments[0].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
      F.Image.Sections[0].Flags = F.Image.Segments[0].Flags;
      constexpr va_t Address = 0x1040;
      F.Image.Symbols.push_back({"_fieldOffset", Address, 8, false});
      llvm::support::endian::write64le(
          F.Image.Segments[0].Data.data() + Address - 0x1000, 24);

      const auto PointerType = NdType::makePtr(NdType::makeVoid());
      const auto ValueType = NdType::makeInt(8, false);
      HighFunc Callee;
      Callee.Entry = 0x2000;
      Callee.Name = "read_field_offset";
      Callee.ReturnType = ValueType;
      Callee.Params = {{"offset", PointerType}};
      SourceFunctionTypeHint Signature;
      Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      Signature.ReturnType = ValueType;
      Signature.Parameters = {{"offset", PointerType}};
      std::string Error;
      ASSERT_TRUE(assignDarwinScalarSourceABI(Signature, Architecture, Error))
          << Error;
      Callee.SourceTypeHint = Signature;
      MedVar Parameter;
      Parameter.Kind = MedVar::Param;
      Parameter.Id = 0;
      Parameter.Size = 8;
      Parameter.TheArch = Architecture;
      const auto Pointer = HighExpr::makeVar(Parameter, NdType::makeInt(8));
      auto Load = HighExpr::makeLoad(Pointer, ValueType);
      HighStmt Return;
      Return.Kind = StmtKind::Return;
      Return.RetVal = Load;
      Callee.Body = {Return};

      auto Binding = std::make_shared<SourceCallTypeHint>();
      Binding->CallKind = SourceCallTypeHint::Kind::Native;
      Binding->TargetAddress = Callee.Entry;
      Binding->TargetName = Callee.Name;
      Binding->Signature = Signature;
      auto Call = HighExpr::makeCall(
          Callee.Name, Callee.Entry,
          {HighExpr::makeConst(Address, 8,
                               ConstantAddressProvenance::DataAddress)});
      Call->Type = ValueType;
      Call->SourceCallHint = Binding;
      F.Function.ReturnType = ValueType;
      F.Function.Body[0].RetVal = Call;

      if (Mutation == 1)
        Callee.Body[0].RetVal = Pointer;
      if (Mutation == 2)
        Load->Operands[0] = HighExpr::makeBinop(NdOp::INT_ADD, Pointer,
                                                HighExpr::makeConst(1, 8));
      if (Mutation == 3)
        Load->MemoryOrdering = NdMemoryOrdering::Acquire;
      if (Mutation == 4)
        F.Image.DataPtrRelocSlots.insert(Address);
      if (Mutation == 5)
        Binding->Signature.Parameters[0].Location.RegisterOffset += 8;

      const std::map<va_t, const HighFunc *> Present{{Callee.Entry, &Callee}};
      const std::map<va_t, const HighFunc *> Missing;
      const auto &Functions = Mutation == 6 ? Missing : Present;
      const auto Result =
          bindObjCSourceReferences(F.Function, F.Image, nullptr, &Functions);
      if (Mutation) {
        EXPECT_FALSE(Result.Limitation.empty());
        EXPECT_TRUE(Result.LocalStorageExtents.empty());
        EXPECT_EQ(Result.Function.Body[0].RetVal->Operands[0]->Kind,
                  ExprKind::Const);
        continue;
      }

      ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
      EXPECT_EQ(Result.LocalStorageExtents,
                (std::map<va_t, uint64_t>{{Address, 8}}));
      const auto Argument = Result.Function.Body[0].RetVal->Operands[0];
      ASSERT_TRUE(Argument->SourceCallHint);
      EXPECT_EQ(Argument->SourceCallHint->CallKind,
                SourceCallTypeHint::Kind::RuntimeLocalStorageAddress);
      EXPECT_EQ(Argument->SourceCallHint->TargetAddress, Address);
      EXPECT_EQ(Argument->SourceCallHint->ByteCount, 8U);
      EXPECT_TRUE(objcSourceCallBound(*Argument, F.Image, Functions));
      EXPECT_TRUE(objcSourceCallBound(*Result.Function.Body[0].RetVal, F.Image,
                                      Functions));
    }
}

TEST(ObjCSourceBindings,
     NativeStorageProofFollowsAuthenticatedCalleeParameters) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
      SCOPED_TRACE(unsigned(Architecture));
      SCOPED_TRACE(Mutation);
      const auto PointerType = NdType::makePtr(NdType::makeVoid());
      const auto Signature = [&] {
        SourceFunctionTypeHint Hint;
        Hint.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
        Hint.ReturnType = NdType::makeVoid();
        Hint.Parameters = {{"cache", PointerType}, {"reference", PointerType}};
        std::string Error;
        EXPECT_TRUE(assignDarwinScalarSourceABI(Hint, Architecture, Error))
            << Error;
        return Hint;
      }();
      const auto Parameter = [&](size_t Index) {
        MedVar Value;
        Value.Kind = MedVar::Param;
        Value.Id = Index;
        Value.Size = 8;
        Value.TheArch = Architecture;
        return HighExpr::makeVar(Value, PointerType);
      };

      HighFunc Leaf;
      Leaf.Entry = 0x2200;
      Leaf.Name = "metadata_cache_leaf";
      Leaf.ReturnType = NdType::makeVoid();
      Leaf.Params = {{"cache", PointerType}, {"reference", PointerType}};
      Leaf.SourceTypeHint = Signature;
      HighStmt CacheLoad, ReferenceLoad, CacheStore, LeafReturn;
      CacheLoad.Kind = ReferenceLoad.Kind = StmtKind::ExprStmt;
      CacheLoad.Val = HighExpr::makeLoad(Parameter(0), PointerType);
      ReferenceLoad.Val = HighExpr::makeLoad(Parameter(1), PointerType);
      CacheStore.Kind = StmtKind::Store;
      CacheStore.StoreAddr = Parameter(0);
      CacheStore.StoreVal = HighExpr::makeConst(0, 8);
      CacheStore.MemoryOrdering = NdMemoryOrdering::Release;
      LeafReturn.Kind = StmtKind::Return;
      Leaf.Body = {CacheLoad, ReferenceLoad, CacheStore, LeafReturn};

      HighFunc Forwarder;
      Forwarder.Entry = 0x2100;
      Forwarder.Name = "metadata_cache_forwarder";
      Forwarder.ReturnType = NdType::makeVoid();
      Forwarder.Params = {{"cache", PointerType}, {"reference", PointerType}};
      Forwarder.SourceTypeHint = Signature;
      auto Binding = std::make_shared<SourceCallTypeHint>();
      Binding->CallKind = SourceCallTypeHint::Kind::Native;
      Binding->TargetAddress = Leaf.Entry;
      Binding->TargetName = Leaf.Name;
      Binding->Signature = Signature;
      auto Call = HighExpr::makeCall(Leaf.Name, Leaf.Entry,
                                     {Parameter(0), Parameter(1)});
      Call->Type = NdType::makeVoid();
      Call->SourceCallHint = Binding;
      HighStmt Forward, ForwardReturn;
      Forward.Kind = StmtKind::ExprStmt;
      Forward.Val = Call;
      ForwardReturn.Kind = StmtKind::Return;
      Forwarder.Body = {Forward, ForwardReturn};

      std::map<va_t, const HighFunc *> Functions{{Forwarder.Entry, &Forwarder},
                                                 {Leaf.Entry, &Leaf}};
      if (Mutation == 1)
        Binding->Signature.Parameters[0].Location.RegisterOffset += 8;
      if (Mutation == 2)
        Call->Operands[0] = HighExpr::makeBinop(NdOp::INT_ADD, Parameter(0),
                                                HighExpr::makeConst(0, 8));
      if (Mutation == 3)
        Functions.erase(Leaf.Entry);
      if (Mutation == 4) {
        Binding->TargetAddress = Forwarder.Entry;
        Binding->TargetName = Forwarder.Name;
        Call->CallAddr = Forwarder.Entry;
      }
      if (Mutation == 5)
        Call->CallAddr += 4;
      if (Mutation == 6)
        Leaf.Body[2].MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
      if (Mutation == 7) {
        ++Binding->Signature.Parameters[0].Location.ValueBytes;
        Leaf.SourceTypeHint = Binding->Signature;
      }

      const auto Cache = objc_binding_detail::nativeScalarStorageArgumentExtent(
          Forwarder, 0, true, true, &Functions);
      const auto ReadOnlyCache =
          objc_binding_detail::nativeScalarStorageArgumentExtent(
              Forwarder, 0, true, false, &Functions);
      const auto Reference =
          objc_binding_detail::nativeScalarStorageArgumentExtent(
              Forwarder, 1, true, false, &Functions);
      if (Mutation && Mutation != 2 && Mutation != 6) {
        EXPECT_FALSE(Cache);
        EXPECT_FALSE(ReadOnlyCache);
        EXPECT_FALSE(Reference);
      } else {
        if (Mutation == 2 || Mutation == 6) {
          EXPECT_FALSE(Cache);
          EXPECT_FALSE(ReadOnlyCache);
        } else {
          ASSERT_TRUE(Cache);
          EXPECT_EQ(*Cache, 8U);
          EXPECT_FALSE(ReadOnlyCache);
        }
        ASSERT_TRUE(Reference);
        EXPECT_EQ(*Reference, 8U);
      }
    }
}

TEST(ObjCSourceBindings,
     NativeProfileCounterArgumentsRejectEscapesAndUnprovedAccesses) {
  for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ProfileFixture F;
    const ObjCProfileStorage Storage(F.Image);
    const auto PointerType = NdType::makePtr(NdType::makeVoid());
    HighFunc Callee;
    Callee.Entry = 0x2000;
    Callee.Name = "profile_counter_user";
    Callee.ReturnType = NdType::makeVoid();
    Callee.Params = {{"counter", PointerType}};
    SourceFunctionTypeHint Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Signature.ReturnType = Callee.ReturnType;
    Signature.Parameters = {{"counter", PointerType}};
    std::string Error;
    ASSERT_TRUE(assignDarwinScalarSourceABI(Signature, F.Image.Arch, Error));
    Callee.SourceTypeHint = Signature;
    MedVar Parameter;
    Parameter.Kind = MedVar::Param;
    Parameter.Id = 0;
    Parameter.Size = 8;
    Parameter.TheArch = F.Image.Arch;
    const auto Counter = HighExpr::makeVar(Parameter, NdType::makeInt(8));
    auto Load = HighExpr::makeLoad(Counter, NdType::makeInt(8, false));
    HighStmt Use;
    Use.Kind = StmtKind::Return;
    Use.RetVal = Load;
    Callee.Body = {Use};
    if (Mutation == 0)
      Use.RetVal = Counter;
    else if (Mutation == 1)
      Use.RetVal = HighExpr::makeCall("escape", 0x3000, {Counter});
    else if (Mutation == 2)
      Load->MemoryOrdering = NdMemoryOrdering::Acquire;
    else if (Mutation == 3)
      Load->Type = NdType::makePtr(NdType::makeVoid());
    else if (Mutation == 4)
      Load->Operands[0] = HighExpr::makeBinop(NdOp::INT_ADD, Counter,
                                              HighExpr::makeConst(8, 8));
    Callee.Body = {Use};

    auto Binding = std::make_shared<SourceCallTypeHint>();
    Binding->CallKind = SourceCallTypeHint::Kind::Native;
    Binding->TargetAddress = Callee.Entry;
    Binding->TargetName = Callee.Name;
    Binding->Signature = Signature;
    const va_t Address = Mutation == 5 ? 0x10f9 : 0x1040;
    auto Call = HighExpr::makeCall(
        Callee.Name, Callee.Entry,
        {HighExpr::makeConst(Address, 8,
                             ConstantAddressProvenance::DataAddress)});
    Call->Type = NdType::makeVoid();
    Call->SourceCallHint = Binding;
    F.Function.Body[0].RetVal = Call;
    const std::map<va_t, const HighFunc *> Present{{Callee.Entry, &Callee}};
    const std::map<va_t, const HighFunc *> Missing;
    const auto &Functions = Mutation == 6 ? Missing : Present;
    const auto Result =
        bindObjCSourceReferences(F.Function, F.Image, &Storage, &Functions);
    EXPECT_FALSE(Result.Limitation.empty());
    EXPECT_TRUE(Result.ProfileCounterSections.empty());
    EXPECT_EQ(Result.Function.Body[0].RetVal->Operands[0]->Kind,
              ExprKind::Const);
  }
}

TEST(ObjCSourceBindings, ProfileCountersRejectUnprovedStorageAndEffects) {
  for (unsigned Case = 0; Case < 18; ++Case) {
    SCOPED_TRACE(Case);
    ProfileFixture F;
    switch (Case) {
    case 0:
      F.Image.Sections[0].Name = "__llvm_prf_data";
      break;
    case 1:
      F.Image.Sections[0].FileSz--;
      break;
    case 2:
      F.Image.Segments[0].Data.resize(8);
      break;
    case 3:
      F.Image.Sections[0].Flags = SegmentFlags::Readable;
      break;
    case 4:
      F.Image.MachOChainedFixupsAmbiguous = true;
      break;
    case 5:
      F.Image.DataPtrRelocSlots.insert(0x1040);
      break;
    case 6:
      F.Image.MachOResolvedChainedPointerSlots.insert(0x1040);
      break;
    case 7:
      F.Image.DyldBindSlots[0x1040] = {};
      break;
    case 8:
      F.Image.BaseRelocations.push_back({0xff9, 0});
      break;
    case 9:
      F.Image.Sections.push_back(F.Image.Sections[0]);
      break;
    case 10:
      F.Image.Segments.push_back(F.Image.Segments[0]);
      break;
    case 11:
      F.Image.IsRelocatable = true;
      break;
    case 12:
      F.Image.Arch = Arch::ARM;
      break;
    case 13:
      F.Function.Body[0].RetVal->MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 14:
      F.Function.Body[0].RetVal->Type = NdType::makePtr(NdType::makeVoid());
      break;
    case 15:
      F.Function.Body[0].RetVal->Operands[0] = HighExpr::makeConst(0x10f9, 8);
      break;
    case 16:
      F.Function.Body[0].RetVal->Type = NdType::makeFloat(16);
      break;
    case 17:
      F.Image.Segments[0].FileSz = 16;
      break;
    }
    const auto Result = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_FALSE(Result.Limitation.empty());
    EXPECT_TRUE(Result.ProfileCounterSections.empty());
  }
}

TEST(ObjCSourceBindings, ProfileCountersExecuteAcrossTranslationUnits) {
  ProfileFixture F;
  const ObjCProfileStorage Storage(F.Image);
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-profile-storage",
                                                    Directory));
  const std::filesystem::path Work(Directory.str().str());
  struct Cleanup {
    std::filesystem::path Work;
    ~Cleanup() {
      std::error_code Error;
      std::filesystem::remove_all(Work, Error);
    }
  } Cleanup{Work};
  std::vector<std::string> Sources;
  std::set<va_t> Used;
  for (bool Store : {false, true}) {
    for (uint16_t Width : {1, 2, 4, 8, 16}) {
      const auto Type = NdType::makeInt(Width, false);
      HighFunc Function;
      Function.Name =
          std::string(Store ? "put" : "get") + std::to_string(Width);
      Function.Entry = 0x2000 + Sources.size() * 16;
      Function.ReturnType = Store ? NdType::makeVoid() : Type;
      const auto Address = HighExpr::makeConst(0x1007, 8);
      if (Store) {
        Function.Params.push_back({"arg0", Type});
        MedVar Value;
        Value.Kind = MedVar::Param;
        Value.Id = 0;
        Value.Size = Width;
        Value.TheArch = Arch::X64;
        HighStmt Statement;
        Statement.Kind = StmtKind::Store;
        Statement.StoreAddr = Address;
        Statement.StoreVal = HighExpr::makeVar(Value, Type);
        Function.Body.push_back(Statement);
      }
      HighStmt Return;
      Return.Kind = StmtKind::Return;
      if (!Store)
        Return.RetVal = HighExpr::makeLoad(Address, Type);
      Function.Body.push_back(Return);
      auto Bound = bindObjCSourceReferences(Function, F.Image, &Storage);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      Used.insert(Bound.ProfileCounterSections.begin(),
                  Bound.ProfileCounterSections.end());
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      CEmitterOptions Options;
      Options.TheArch = Arch::X64;
      ASSERT_TRUE(HighCEmitter().emit({Bound.Function}, OS, Options));
      const auto Path = (Work / (Function.Name + ".c")).string();
      std::ofstream(Path) << Source;
      Sources.push_back(Path);
    }
  }
  std::set<std::string> Helpers;
  std::string Harness = "#include <stdint.h>\n#include <string.h>\n" +
                        Storage.render(Used, Helpers);
  for (uint16_t Width : {1, 2, 4, 8, 16}) {
    const auto Type = Width == 16 ? "unsigned __int128"
                                  : "uint" + std::to_string(Width * 8) + "_t";
    Harness += "extern " + Type + " get" + std::to_string(Width) + "(void);\n";
    Harness += "extern void put" + std::to_string(Width) + "(" + Type + ");\n";
  }
  Harness += R"(
int main(void) {
  unsigned char expected[256];
  unsigned char *data = (unsigned char *)neverd_profile_counters_1000_address();
  for (unsigned i = 0; i != 256; ++i) expected[i] = (unsigned char)(i * 37 + 9);
  if (memcmp(data, expected, sizeof expected)) return 1;
  for (unsigned round = 0; round != 64; ++round) {
    unsigned __int128 value = ((unsigned __int128)(UINT64_MAX - round) << 64) | round;
    put16(value); memcpy(expected + 7, &value, 16);
    if (get16() != value || memcmp(data, expected, sizeof expected)) return 2;
    uint64_t a = UINT64_MAX - round; put8(a); memcpy(expected + 7, &a, 8);
    if (get8() != a || memcmp(data, expected, sizeof expected)) return 3;
    uint32_t b = UINT32_MAX - round; put4(b); memcpy(expected + 7, &b, 4);
    if (get4() != b || memcmp(data, expected, sizeof expected)) return 4;
    uint16_t c = UINT16_MAX - round; put2(c); memcpy(expected + 7, &c, 2);
    if (get2() != c || memcmp(data, expected, sizeof expected)) return 5;
    uint8_t d = (uint8_t)round; put1(d); memcpy(expected + 7, &d, 1);
    if (get1() != d || memcmp(data, expected, sizeof expected)) return 6;
    memcpy(&value, expected + 7, 16);
    if (get16() != value) return 7;
  }
  return 0;
}
)";
  const auto HarnessPath = (Work / "harness.c").string();
  std::ofstream(HarnessPath) << Harness;
  Sources.push_back(HarnessPath);
  const std::string Compiler = NEVERD_TEST_CLANG;
  const auto Executable = (Work / "test.exe").string();
  const auto ErrorPath = (Work / "stderr").string();
  std::vector<std::string> Arguments{
      Compiler,  "-std=c11", "-O3",     "-fstrict-aliasing",
      "-Werror", "-o",       Executable};
  Arguments.insert(Arguments.end(), Sources.begin(), Sources.end());
  std::vector<llvm::StringRef> Refs(Arguments.begin(), Arguments.end());
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt,
                                                      std::nullopt, ErrorPath};
  std::string Error;
  const auto Status = llvm::sys::ExecuteAndWait(Compiler, Refs, std::nullopt,
                                                Redirects, 60, 0, &Error);
  auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
  ASSERT_EQ(Status, 0) << Error << (Errors ? (*Errors)->getBuffer().str() : "");
  EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable}, std::nullopt,
                                      Redirects, 30, 0, &Error),
            0)
      << Error;
}

TEST(ObjCSourceBindings, ExplicitABIPositionDriftIsDetected) {
  SourceFunctionTypeHint Hint;
  Hint.ReturnType = NdType::makeInt(8);
  Hint.Parameters = {{"objc_self", NdType::makePtr(NdType::makeVoid())},
                     {"objc_cmd", NdType::makePtr(NdType::makeVoid())}};
  std::string Reason;
  ASSERT_TRUE(assignDarwinObjCSourceABI(Hint, Arch::AArch64, Reason));
  auto Changed = Hint;
  Changed.Parameters[1].Location.RegisterOffset += 8;
  EXPECT_FALSE(objc_projection_detail::sameHint(Hint, Changed));
  Changed = Hint;
  Changed.ReturnLocation.RegisterOffset += 8;
  EXPECT_FALSE(objc_projection_detail::sameHint(Hint, Changed));
}

TEST(ObjCSourceBindings,
     ConflictingSelectorResultUseIsRevalidatedAtPublication) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/CoreData.framework/CoreData"};
  ObjCMethod VoidSave;
  VoidSave.ClassName = "ApplicationController";
  VoidSave.Selector = "save:";
  VoidSave.TypeHint = parseObjCMethodEncoding("save:", "v24@0:8@16");
  ASSERT_TRUE(VoidSave.TypeHint);
  Image.ObjCMethods.push_back(VoidSave);

  SourceABIValueLocation Use;
  Use.Kind = SourceABICarrierKind::IntegerRegister;
  Use.RegisterOffset = a64reg::X0;
  Use.ValueBytes = 4;
  const auto Signature =
      objcSelectorSourceTypeHintForResultUse(Image, "save:", Use);
  ASSERT_TRUE(Signature);
  auto Binding = std::make_shared<SourceCallTypeHint>();
  Binding->CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Binding->TargetName = "objc_msgSend";
  Binding->Selector = "save:";
  Binding->Signature = *Signature;
  Binding->SelectorResultUse = Use;
  auto Call =
      HighExpr::makeCall("objc_msgSend", 0,
                         {HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8),
                          HighExpr::makeConst(0, 8)});
  Call->Type = Signature->ReturnType;
  Call->SourceCallHint = Binding;
  EXPECT_TRUE(objcSourceCallBound(*Call, Image, {}));

  Binding->SelectorResultTypeUse = NdTypeKind::Int;
  EXPECT_TRUE(objcSourceCallBound(*Call, Image, {}));
  Binding->SelectorResultTypeUse = NdTypeKind::Ptr;
  EXPECT_FALSE(objcSourceCallBound(*Call, Image, {}));
  Binding->SelectorResultTypeUse.reset();

  Binding->SelectorResultUse->ValueBytes = 8;
  EXPECT_FALSE(objcSourceCallBound(*Call, Image, {}));
  Binding->SelectorResultUse = Use;
  Binding->Signature.ReturnType = NdType::makeVoid();
  EXPECT_FALSE(objcSourceCallBound(*Call, Image, {}));
  Binding->Signature = *Signature;
  Binding->SelectorResultUse.reset();
  EXPECT_FALSE(objcSourceCallBound(*Call, Image, {}));
  Binding->SelectorResultUse = Use;
  Image.ObjCMethods.back().TypeHint.reset();
  EXPECT_FALSE(objcSourceCallBound(*Call, Image, {}));
}

TEST(ObjCSourceBindings,
     DeclaredConsumerTypeRevalidatesLocalSelectorResultCandidate) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/Foundation.framework/Foundation"};
  ObjCMethod ObjectCode;
  ObjectCode.ClassName = "ApplicationValue";
  ObjectCode.Selector = "code";
  ObjectCode.TypeHint = parseObjCMethodEncoding("code", "@16@0:8");
  ASSERT_TRUE(ObjectCode.TypeHint);
  Image.ObjCMethods.push_back(ObjectCode);

  SourceABIValueLocation Use{SourceABICarrierKind::IntegerRegister, a64reg::X0,
                             0, 8};
  const auto Signature = objcSelectorSourceTypeHintForResultUse(
      Image, "code", Use, NdTypeKind::Ptr);
  ASSERT_TRUE(Signature);
  EXPECT_EQ(Signature->Origin, SourceFunctionTypeHint::OriginKind::ObjCRuntime);
  auto Binding = std::make_shared<SourceCallTypeHint>();
  Binding->CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Binding->TargetName = "objc_msgSend";
  Binding->Selector = "code";
  Binding->Signature = *Signature;
  Binding->SelectorResultUse = Use;
  Binding->SelectorResultTypeUse = NdTypeKind::Ptr;
  auto Call = HighExpr::makeCall(
      "objc_msgSend", 0,
      {HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8)});
  Call->Type = Signature->ReturnType;
  Call->SourceCallHint = Binding;
  EXPECT_TRUE(objcSourceCallBound(*Call, Image, {}));

  Binding->SelectorResultTypeUse = NdTypeKind::Int;
  EXPECT_FALSE(objcSourceCallBound(*Call, Image, {}));
  Binding->SelectorResultTypeUse.reset();
  EXPECT_FALSE(objcSourceCallBound(*Call, Image, {}));
  Binding->SelectorResultTypeUse = NdTypeKind::Ptr;
  Image.ObjCMethods.back().TypeHint.reset();
  EXPECT_FALSE(objcSourceCallBound(*Call, Image, {}));
}

TEST(ObjCSourceBindings,
     ConflictingSelectorArgumentTypeIsRevalidatedAtPublication) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/CoreData.framework/CoreData"};
  ObjCMethod VoidSave;
  VoidSave.ClassName = "ApplicationController";
  VoidSave.Selector = "save:";
  VoidSave.TypeHint = parseObjCMethodEncoding("save:", "v24@0:8@16");
  ASSERT_TRUE(VoidSave.TypeHint);
  Image.ObjCMethods.push_back(VoidSave);
  ObjCMethod Caller;
  Caller.ClassName = "Migrator";
  Caller.Selector = "runWithError:";
  Caller.Implementation = 0x1200;
  Caller.TypeHint = parseObjCMethodEncoding(Caller.Selector, "v24@0:8^@16");
  ASSERT_TRUE(Caller.TypeHint);
  Image.ObjCMethods.push_back(Caller);

  SourceCallTypeHint::SelectorArgumentTypeEvidence Use;
  Use.Parameter = 2;
  Use.MethodEntry = Caller.Implementation;
  Use.Source.Kind = SourceABICarrierKind::IntegerRegister;
  Use.Source.RegisterOffset = a64reg::X2;
  Use.Source.ValueBytes = 8;
  const auto Signature =
      objcSelectorSourceTypeHintForArgumentTypeUse(Image, "save:", Use);
  ASSERT_TRUE(Signature);
  auto Binding = std::make_shared<SourceCallTypeHint>();
  Binding->CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Binding->TargetName = "objc_msgSend";
  Binding->Selector = "save:";
  Binding->Signature = *Signature;
  Binding->SelectorArgumentTypeUse = Use;
  auto Call =
      HighExpr::makeCall("objc_msgSend", 0,
                         {HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8),
                          HighExpr::makeConst(0, 8)});
  Call->Type = Signature->ReturnType;
  Call->SourceCallHint = Binding;
  EXPECT_TRUE(objcSourceCallBound(*Call, Image, {}));

  Binding->SelectorArgumentTypeUse->Source.RegisterOffset = a64reg::X3;
  EXPECT_FALSE(objcSourceCallBound(*Call, Image, {}));
  Binding->SelectorArgumentTypeUse = Use;
  Binding->SelectorArgumentTypeUse->Parameter = 1;
  EXPECT_FALSE(objcSourceCallBound(*Call, Image, {}));
  Binding->SelectorArgumentTypeUse = Use;
  Binding->Signature.ReturnType = NdType::makeVoid();
  EXPECT_FALSE(objcSourceCallBound(*Call, Image, {}));
  Binding->Signature = *Signature;
  Image.ObjCMethods.back().TypeHint =
      parseObjCMethodEncoding(Caller.Selector, "v24@0:8@16");
  EXPECT_FALSE(objcSourceCallBound(*Call, Image, {}));
}

TEST(ObjCSourceBindings, ObjectConsumedArgumentTypeIsRevalidatedAtPublication) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/UIKit.framework/UIKit"};
  ObjCMethod Setter;
  Setter.ClassName = "LoadState";
  Setter.Selector = "setProgress:";
  Setter.TypeHint = parseObjCMethodEncoding(Setter.Selector, "v24@0:8@16");
  ASSERT_TRUE(Setter.TypeHint);
  Image.ObjCMethods.push_back(Setter);
  ObjCMethod Caller;
  Caller.ClassName = "ImageView";
  Caller.Selector = "forwardProgress:";
  Caller.Implementation = 0x1200;
  Caller.TypeHint = parseObjCMethodEncoding(Caller.Selector, "v24@0:8@16");
  ASSERT_TRUE(Caller.TypeHint);
  Image.ObjCMethods.push_back(Caller);

  SourceCallTypeHint::SelectorArgumentTypeEvidence Use;
  Use.Parameter = 2;
  Use.MethodEntry = Caller.Implementation;
  Use.Source.Kind = SourceABICarrierKind::IntegerRegister;
  Use.Source.RegisterOffset = a64reg::X2;
  Use.Source.ValueBytes = 8;
  Use.ConsumedAsObject = true;
  const auto Signature =
      objcSelectorSourceTypeHintForArgumentTypeUse(Image, Setter.Selector, Use);
  ASSERT_TRUE(Signature);
  auto Binding = std::make_shared<SourceCallTypeHint>();
  Binding->CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Binding->TargetName = "objc_msgSend";
  Binding->Selector = Setter.Selector;
  Binding->Signature = *Signature;
  Binding->SelectorArgumentTypeUse = Use;
  auto Call =
      HighExpr::makeCall("objc_msgSend", 0,
                         {HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8),
                          HighExpr::makeConst(0, 8)});
  Call->Type = Signature->ReturnType;
  Call->SourceCallHint = Binding;
  EXPECT_TRUE(objcSourceCallBound(*Call, Image, {}));

  Binding->SelectorArgumentTypeUse->ConsumedAsObject = false;
  EXPECT_FALSE(objcSourceCallBound(*Call, Image, {}));
}

TEST(ObjCSourceBindings, ExactSelectorForwardingIsRevalidatedAtPublication) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  ObjCMethod Caller;
  Caller.ClassName = "DDLog";
  Caller.IsClassMethod = true;
  Caller.Selector = "setLevel:forClass:";
  Caller.Implementation = 0x1200;
  Caller.TypeHint = parseObjCMethodEncoding(Caller.Selector, "v32@0:8q16#24");
  ASSERT_TRUE(Caller.TypeHint);
  Image.ObjCMethods.push_back(Caller);

  SourceCallTypeHint::SelectorForwardingEvidence Use;
  Use.MethodEntry = Caller.Implementation;
  Use.ReceiverSourceParameter = 3;
  Use.ArgumentSourceParameters = {2};
  const auto Signature =
      objcSelectorSourceTypeHintForForwardingUse(Image, "ddSetLogLevel:", Use);
  ASSERT_TRUE(Signature);
  auto Binding = std::make_shared<SourceCallTypeHint>();
  Binding->CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Binding->TargetName = "objc_msgSend";
  Binding->Selector = "ddSetLogLevel:";
  Binding->Signature = *Signature;
  Binding->SelectorForwardingUse = Use;
  const auto CallerSignature =
      objcMethodSourceTypeHint(Image, Caller.Implementation);
  ASSERT_TRUE(CallerSignature);
  const auto Parameter = [&](unsigned Index) {
    MedVar Value;
    Value.Kind = MedVar::Param;
    Value.Id = Index;
    Value.Size = 8;
    Value.TheArch = Image.Arch;
    return HighExpr::makeVar(Value, CallerSignature->Parameters[Index].Type);
  };
  auto Call = HighExpr::makeCall(
      "objc_msgSend", 0,
      {Parameter(3), HighExpr::makeConst(0, 8), Parameter(2)});
  Call->Type = Signature->ReturnType;
  Call->SourceCallHint = Binding;
  HighFunc Owner;
  Owner.Entry = Caller.Implementation;
  Owner.ReturnType = CallerSignature->ReturnType;
  Owner.SourceTypeHint = *CallerSignature;
  for (const auto &Source : CallerSignature->Parameters)
    Owner.Params.push_back({Source.Name, Source.Type});

  EXPECT_FALSE(objcSourceCallBound(*Call, Image, {}));
  const auto Rebuilt = objcSelectorSourceTypeHintForForwardingUse(
      Image, Binding->Selector, *Binding->SelectorForwardingUse);
  ASSERT_TRUE(Rebuilt);
  EXPECT_TRUE(objc_projection_detail::sameHint(Binding->Signature, *Rebuilt));
  EXPECT_EQ(Owner.Params.size(), CallerSignature->Parameters.size());
  EXPECT_TRUE(objc_binding_detail::exactParameterValue(Call->Operands[0], 3));
  EXPECT_TRUE(objc_binding_detail::exactParameterValue(Call->Operands[2], 2));
  EXPECT_TRUE(objcSourceCallBound(*Call, Image, {}, nullptr, nullptr, &Owner));
  Call->Operands[0] = Parameter(2);
  EXPECT_FALSE(objcSourceCallBound(*Call, Image, {}, nullptr, nullptr, &Owner));
  Call->Operands[0] = Parameter(3);
  Call->Operands[2] = Parameter(3);
  EXPECT_FALSE(objcSourceCallBound(*Call, Image, {}, nullptr, nullptr, &Owner));
  Call->Operands[2] = Parameter(2);
  Image.ObjCMethods.front().TypeHint =
      parseObjCMethodEncoding(Caller.Selector, "v32@0:8Q16#24");
  EXPECT_FALSE(objcSourceCallBound(*Call, Image, {}, nullptr, nullptr, &Owner));
}

TEST(ObjCSourceBindings,
     ConflictingSelectorFrameStorageIsRevalidatedAtPublication) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/CoreData.framework/CoreData"};
  ObjCMethod VoidSave;
  VoidSave.ClassName = "ApplicationController";
  VoidSave.Selector = "save:";
  VoidSave.TypeHint = parseObjCMethodEncoding("save:", "v24@0:8@16");
  ASSERT_TRUE(VoidSave.TypeHint);
  Image.ObjCMethods.push_back(VoidSave);

  SourceCallTypeHint::SelectorArgumentStorageEvidence Use;
  Use.Parameter = 2;
  Use.FrameOffset = -32;
  const auto Signature =
      objcSelectorSourceTypeHintForArgumentStorageUse(Image, "save:", Use);
  ASSERT_TRUE(Signature);
  auto Binding = std::make_shared<SourceCallTypeHint>();
  Binding->CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Binding->TargetName = "objc_msgSend";
  Binding->Selector = "save:";
  Binding->Signature = *Signature;
  Binding->SelectorArgumentStorageUse = Use;

  MedVar SP;
  SP.Kind = MedVar::Reg;
  SP.TheArch = Arch::AArch64;
  SP.Size = 8;
  SP.RegOff = a64reg::SP;
  auto Address = HighExpr::makeBinop(
      NdOp::INT_SUB, HighExpr::makeVar(SP, NdType::makeInt(8, false)),
      HighExpr::makeConst(32, 4));
  auto Call = HighExpr::makeCall(
      "objc_msgSend", 0,
      {HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8), Address});
  Call->Type = Signature->ReturnType;
  Call->SourceCallHint = Binding;
  HighFunc Owner;
  Owner.FrameSize = 64;
  HighStmt Statement;
  Statement.Kind = StmtKind::ExprStmt;
  Statement.Val = Call;
  Owner.Body.push_back(Statement);

  EXPECT_FALSE(objcSourceCallBound(*Call, Image, {}));
  EXPECT_TRUE(objcSourceCallBound(*Call, Image, {}, nullptr, nullptr, &Owner));
  Binding->SelectorArgumentStorageUse->FrameOffset = -24;
  EXPECT_FALSE(objcSourceCallBound(*Call, Image, {}, nullptr, nullptr, &Owner));
  Binding->SelectorArgumentStorageUse = Use;
  Owner.FrameSize = 24;
  EXPECT_FALSE(objcSourceCallBound(*Call, Image, {}, nullptr, nullptr, &Owner));
  Owner.FrameSize = 64;
  Binding->SelectorArgumentStorageUse.reset();
  EXPECT_FALSE(objcSourceCallBound(*Call, Image, {}, nullptr, nullptr, &Owner));
}

TEST(ObjCSourceBindings, PrivateFramePointerTailRequiresExactStoreOnEveryPath) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  HighFunc Function;
  Function.FrameSize = 64;
  Function.ReturnType = NdType::makeVoid();
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto Integer = NdType::makeInt(8, false);
  MedVar SP;
  SP.Kind = MedVar::Reg;
  SP.TheArch = Arch::AArch64;
  SP.Size = 8;
  SP.RegOff = a64reg::SP;
  MedVar FrameVar;
  FrameVar.Kind = MedVar::Temp;
  FrameVar.Id = 1;
  FrameVar.Size = 8;
  MedVar InputVar;
  InputVar.Kind = MedVar::Param;
  InputVar.Id = 2;
  InputVar.Size = 8;
  MedVar ReloadVar;
  ReloadVar.Kind = MedVar::Temp;
  ReloadVar.Id = 2;
  ReloadVar.Size = 8;
  auto Frame = HighExpr::makeVar(FrameVar, Integer);
  auto Slot = [&] {
    return HighExpr::makeBinop(NdOp::INT_ADD, Frame,
                               HighExpr::makeConst(16, 8));
  };
  auto Assign = [](ExprPtr Dst, ExprPtr Val) {
    HighStmt S;
    S.Kind = StmtKind::Assign;
    S.Dst = std::move(Dst);
    S.Val = std::move(Val);
    return S;
  };
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr = Slot();
  Store.StoreVal = HighExpr::makeVar(InputVar, Pointer);
  auto Load = HighExpr::makeLoad(Slot(), Integer);
  auto Reload = HighExpr::makeVar(ReloadVar, Integer);
  auto Call =
      HighExpr::makeCall("objc_msgSend", 0,
                         {HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8),
                          HighExpr::makeConst(0, 8), Reload});
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Function.Body = {
      Assign(Frame,
             HighExpr::makeBinop(NdOp::INT_SUB, HighExpr::makeVar(SP, Integer),
                                 HighExpr::makeConst(64, 8))),
      Store, Assign(Reload, Load), Return};
  const auto Check = [&] {
    VarKeyMap<std::vector<ExprPtr>> Definitions;
    walkStmts(Function.Body, [&](const HighStmt &S) {
      if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
          S.Dst->Kind == ExprKind::Var)
        Definitions[varKey(S.Dst->Var)].push_back(S.Val);
    });
    const auto Loads = objc_binding_detail::provenPrivateFramePointerLoads(
        Function, Image, Definitions, *Call, 3);
    size_t Budget = 4096;
    std::set<VarKey> Active;
    return objc_binding_detail::provenSourcePointerValue(
        Reload, Definitions, Image, Budget, Active, 0, &Loads);
  };
  EXPECT_TRUE(Check());

  HighStmt Disjoint;
  Disjoint.Kind = StmtKind::Store;
  Disjoint.StoreAddr =
      HighExpr::makeBinop(NdOp::INT_ADD, Frame, HighExpr::makeConst(32, 8));
  Disjoint.StoreVal = HighExpr::makeConst(0, 8);
  Function.Body.insert(Function.Body.begin() + 2, Disjoint);
  EXPECT_TRUE(Check());
  Function.Body.erase(Function.Body.begin() + 2);

  HighStmt Clobber;
  Clobber.Kind = StmtKind::Store;
  Clobber.StoreAddr =
      HighExpr::makeBinop(NdOp::INT_ADD, Frame, HighExpr::makeConst(20, 8));
  Clobber.StoreVal = HighExpr::makeConst(0, 4);
  Function.Body.insert(Function.Body.begin() + 2, Clobber);
  EXPECT_FALSE(Check());
  Function.Body.erase(Function.Body.begin() + 2);

  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  MedVar ConditionVar;
  ConditionVar.Kind = MedVar::Param;
  ConditionVar.Id = 3;
  ConditionVar.Size = 1;
  Branch.Cond = HighExpr::makeVar(ConditionVar, NdType::makeInt(1, false));
  Branch.Body = {Clobber};
  Function.Body.insert(Function.Body.begin() + 2, Branch);
  EXPECT_FALSE(Check());
  Function.Body.erase(Function.Body.begin() + 2);

  HighStmt Escape;
  Escape.Kind = StmtKind::Call;
  Escape.CallExpr = HighExpr::makeCall("unknown", 0x3000, {Frame});
  Function.Body.insert(Function.Body.begin() + 2, Escape);
  EXPECT_FALSE(Check());
}

namespace {
struct ObjectFixture {
  BinaryImage Image;
  static constexpr va_t ClassAddress = 0x2020;
  static constexpr va_t MetaAddress = 0x2080;
  static constexpr va_t ClassSlot = 0x2010;
  static constexpr va_t RuntimeSlot = 0x2f00;
  ObjectFixture() {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Segment Segment;
    Segment.VA = 0x2000;
    Segment.Size = Segment.FileSz = 0x1000;
    Segment.Flags = SegmentFlags::Readable;
    Segment.Data.resize(0x1000);
    Image.Segments.push_back(Segment);
    Section Section;
    Section.VA = 0x2000;
    Section.Size = Section.FileSz = 0x1000;
    Section.Flags = SegmentFlags::Readable;
    Image.Sections.push_back(Section);
    put(ClassAddress, MetaAddress);
    put(ClassAddress + 32, 0x2200);
    put(MetaAddress + 32, 0x2280);
    put(0x2280, 1); // RO_META
    put(0x2218, 0x2380);
    put(0x2298, 0x2380);
    const char Name[] = "Receiver";
    std::copy(std::begin(Name), std::end(Name),
              Image.Segments[0].Data.begin() + 0x380);
    ObjCClass Class;
    Class.Address = ClassAddress;
    Class.Name = "Receiver";
    Image.ObjCClasses.push_back(Class);
    Image.ObjCSourceReferences[ClassSlot] = {
        ObjCSourceReference::Kind::Class, ClassSlot, 8, "Receiver", {}};
    Image.ImportPtrSlots[RuntimeSlot] = "_objc_opt_self";
    EXPECT_TRUE(Image.recordDyldBindSlot(RuntimeSlot, "_objc_opt_self", 0,
                                         "/usr/lib/libobjc.A.dylib", false));
  }
  void put(va_t Address, uint64_t Value) {
    llvm::support::endian::write64le(
        Image.Segments[0].Data.data() + Address - 0x2000, Value);
  }
  HighFunc message(ExprPtr Receiver) {
    auto Binding = std::make_shared<SourceCallTypeHint>();
    Binding->CallKind = SourceCallTypeHint::Kind::ObjCMessage;
    Binding->TargetName = "objc_msgSend";
    Binding->Selector = "answer";
    Binding->Signature.ReturnType = NdType::makeInt(4);
    Binding->Signature.Parameters = {
        {"objc_self", NdType::makePtr(NdType::makeVoid())},
        {"objc_cmd", NdType::makePtr(NdType::makeVoid())}};
    std::string Error;
    EXPECT_TRUE(
        assignDarwinObjCSourceABI(Binding->Signature, Image.Arch, Error));
    auto Call = HighExpr::makeCall("objc_msgSend", 0,
                                   {Receiver, HighExpr::makeConst(0, 8)});
    Call->SourceCallHint = Binding;
    HighFunc Function;
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = Call;
    Function.Body.push_back(Return);
    return Function;
  }
  HighFunc runtime(ExprPtr Object) {
    const auto Binding = objcRuntimeSourceCallHint(Image, RuntimeSlot);
    EXPECT_TRUE(Binding);
    auto Call = HighExpr::makeCall("objc_opt_self", RuntimeSlot, {Object});
    Call->Type = NdType::makePtr(NdType::makeVoid());
    if (Binding)
      Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Binding);
    HighFunc Function;
    Function.ReturnType = Call->Type;
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = Call;
    Function.Body.push_back(Return);
    return Function;
  }
};
} // namespace

TEST(ObjCSourceBindings,
     DirectClassAndMetaclassReceiverUseVerifiedObjectIdentity) {
  ObjectFixture Fixture;
  for (va_t Address :
       {ObjectFixture::ClassAddress, ObjectFixture::MetaAddress}) {
    auto Original = Fixture.message(HighExpr::makeConst(Address, 8));
    auto Result = bindObjCSourceReferences(Original, Fixture.Image);
    EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
    auto Receiver = Result.Function.Body[0].RetVal->Operands[0];
    ASSERT_TRUE(Receiver->SourceCallHint);
    EXPECT_EQ(Receiver->SourceCallHint->TargetName, "Receiver");
    EXPECT_EQ(Receiver->SourceCallHint->CallKind,
              Address == ObjectFixture::ClassAddress
                  ? SourceCallTypeHint::Kind::RuntimeClass
                  : SourceCallTypeHint::Kind::RuntimeMetaclass);
    EXPECT_TRUE(objcSourceCallBound(*Receiver, Fixture.Image, {}));
    EXPECT_EQ(Original.Body[0].RetVal->Operands[0]->Kind, ExprKind::Const);
  }
}

TEST(ObjCSourceBindings, StoredClassAddressesPreserveOriginalRuntimeIdentity) {
  for (va_t Address :
       {ObjectFixture::ClassAddress, ObjectFixture::MetaAddress}) {
    for (bool Pointer : {false, true}) {
      ObjectFixture Fixture;
      const auto Type = Pointer ? NdType::makePtr(NdType::makeVoid())
                                : NdType::makeInt(8, false);
      auto Value = HighExpr::makeConst(Address, 8,
                                       ConstantAddressProvenance::DataAddress);
      Value->Type = Type;
      MedVar Destination;
      Destination.Kind = MedVar::Param;
      Destination.Id = 0;
      Destination.Size = 8;
      Destination.TheArch = Fixture.Image.Arch;
      HighStmt Store;
      Store.Kind = StmtKind::Store;
      Store.StoreAddr =
          HighExpr::makeVar(Destination, NdType::makePtr(NdType::makeVoid()));
      Store.StoreVal = Value;
      HighFunc Function;
      Function.Body = {Store};
      auto Result = bindObjCSourceReferences(Function, Fixture.Image);
      ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
      const auto Bound = Result.Function.Body[0].StoreVal;
      ASSERT_TRUE(Bound->SourceCallHint);
      EXPECT_EQ(Bound->SourceCallHint->TargetAddress, Address);
      EXPECT_EQ(Bound->SourceCallHint->TargetName, "Receiver");
      EXPECT_EQ(Bound->SourceCallHint->CallKind,
                Address == ObjectFixture::ClassAddress
                    ? SourceCallTypeHint::Kind::RuntimeClass
                    : SourceCallTypeHint::Kind::RuntimeMetaclass);
      EXPECT_TRUE(objcSourceCallBound(*Bound, Fixture.Image, {}));
      EXPECT_EQ(Function.Body[0].StoreVal->Kind, ExprKind::Const);
      Fixture.Image.ObjCClasses.clear();
      EXPECT_FALSE(objcSourceCallBound(*Bound, Fixture.Image, {}));
    }
  }
}

TEST(ObjCSourceBindings, StoredClassAddressRequiresWholeExactAddressEvidence) {
  for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ObjectFixture Fixture;
    auto Value = HighExpr::makeConst(ObjectFixture::MetaAddress, 8,
                                     ConstantAddressProvenance::DataAddress);
    if (Mutation == 0)
      Value->ConstProvenance = ConstantAddressProvenance::Scalar;
    if (Mutation == 1)
      Value->Type = NdType::makeInt(4);
    if (Mutation == 2)
      Value->AddressOwnerVA = ObjectFixture::ClassAddress;
    if (Mutation == 3)
      Value->ConstVal = ObjectFixture::ClassSlot;
    if (Mutation == 4)
      Fixture.Image.ObjCClasses.push_back(Fixture.Image.ObjCClasses.front());
    if (Mutation == 5)
      Fixture.put(0x2280, 0);
    if (Mutation == 6)
      Fixture.Image.MachOHasChainedFixups = true;
    HighStmt Store;
    Store.Kind = StmtKind::Store;
    Store.StoreAddr = HighExpr::makeConst(0, 8);
    Store.StoreVal = Value;
    HighFunc Function;
    Function.Body = {Store};
    const auto Result = bindObjCSourceReferences(Function, Fixture.Image);
    EXPECT_FALSE(Result.Function.Body[0].StoreVal->SourceCallHint);
  }
}

TEST(ObjCSourceBindings, DirectClassRuntimeArgumentsUseVerifiedObjectIdentity) {
  ObjectFixture Fixture;
  for (va_t Address :
       {ObjectFixture::ClassAddress, ObjectFixture::MetaAddress}) {
    auto Original = Fixture.runtime(HighExpr::makeConst(Address, 8));
    auto Result = bindObjCSourceReferences(Original, Fixture.Image);
    EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
    auto Call = Result.Function.Body[0].RetVal;
    ASSERT_TRUE(Call->SourceCallHint);
    ASSERT_EQ(Call->Operands.size(), 1U);
    auto Object = Call->Operands[0];
    ASSERT_TRUE(Object->SourceCallHint);
    EXPECT_EQ(Object->SourceCallHint->TargetName, "Receiver");
    EXPECT_EQ(Object->SourceCallHint->CallKind,
              Address == ObjectFixture::ClassAddress
                  ? SourceCallTypeHint::Kind::RuntimeClass
                  : SourceCallTypeHint::Kind::RuntimeMetaclass);
    EXPECT_TRUE(objcSourceCallBound(*Object, Fixture.Image, {}));
    EXPECT_TRUE(objcSourceCallBound(*Call, Fixture.Image, {}));
    EXPECT_EQ(Original.Body[0].RetVal->Operands[0]->Kind, ExprKind::Const);
  }
}

TEST(ObjCSourceBindings,
     DirectClassRuntimeArgumentsRequireAnExactImportedBinding) {
  for (unsigned Mutation = 0; Mutation < 3; ++Mutation) {
    ObjectFixture Fixture;
    auto Function =
        Fixture.runtime(HighExpr::makeConst(ObjectFixture::ClassAddress, 8));
    if (Mutation == 0)
      Fixture.Image.DyldBindSlots[ObjectFixture::RuntimeSlot].Module =
          "/tmp/libobjc.A.dylib";
    else if (Mutation == 1)
      Fixture.Image.DyldBindSlots[ObjectFixture::RuntimeSlot].Addend = 1;
    else
      Fixture.Image.ImportPtrSlots[ObjectFixture::RuntimeSlot] =
          "_objc_opt_class";
    const auto Result = bindObjCSourceReferences(Function, Fixture.Image);
    EXPECT_FALSE(Result.Limitation.empty()) << Mutation;
    EXPECT_EQ(Result.Function.Body[0].RetVal->Operands[0]->Kind,
              ExprKind::Const)
        << Mutation;
  }
}

TEST(ObjCSourceBindings,
     ObjectReceiverRewriteDoesNotAffectSharedScalarConstant) {
  ObjectFixture Fixture;
  auto Shared = HighExpr::makeConst(ObjectFixture::ClassAddress, 8);
  auto Function = Fixture.message(Shared);
  HighStmt Scalar;
  Scalar.Kind = StmtKind::Return;
  Scalar.RetVal = Shared;
  Function.Body.push_back(Scalar);
  auto Result = bindObjCSourceReferences(Function, Fixture.Image);
  ASSERT_EQ(Result.Function.Body[0].RetVal->Operands[0]->Kind, ExprKind::Call);
  EXPECT_EQ(Result.Function.Body[1].RetVal->Kind, ExprKind::Const);
  EXPECT_FALSE(Result.Limitation.empty());
}

TEST(ObjCSourceBindings, ClassrefSlotAddressCannotBecomeClassObjectReceiver) {
  ObjectFixture Fixture;
  auto Function =
      Fixture.message(HighExpr::makeConst(ObjectFixture::ClassSlot, 8));
  auto Result = bindObjCSourceReferences(Function, Fixture.Image);
  EXPECT_FALSE(Result.Limitation.empty());
  EXPECT_EQ(Result.Function.Body[0].RetVal->Operands[0]->Kind, ExprKind::Const);
  Function = Fixture.message(HighExpr::makeLoad(
      HighExpr::makeConst(ObjectFixture::ClassSlot, 8), NdType::makeInt(8)));
  Result = bindObjCSourceReferences(Function, Fixture.Image);
  EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  auto Receiver = Result.Function.Body[0].RetVal->Operands[0];
  ASSERT_TRUE(Receiver->SourceCallHint);
  EXPECT_EQ(Receiver->SourceCallHint->TargetAddress, ObjectFixture::ClassSlot);
}

TEST(ObjCSourceBindings,
     NativeClassReferenceAddressRequiresOneReadOnlyPointerCell) {
  for (const bool Metaclass : {false, true}) {
    SCOPED_TRACE(Metaclass);
    ObjectFixture Fixture;
    auto &Reference =
        Fixture.Image.ObjCSourceReferences.at(ObjectFixture::ClassSlot);
    Reference.TheKind = Metaclass ? ObjCSourceReference::Kind::Metaclass
                                  : ObjCSourceReference::Kind::Class;

    const auto PointerType = NdType::makePtr(NdType::makeVoid());
    HighFunc Callee;
    Callee.Entry = 0x3000;
    Callee.Name = "read_class_reference";
    Callee.ReturnType = PointerType;
    Callee.Params = {{"reference", PointerType}};
    SourceFunctionTypeHint Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Signature.ReturnType = PointerType;
    Signature.Parameters = {{"reference", PointerType}};
    std::string Error;
    ASSERT_TRUE(
        assignDarwinScalarSourceABI(Signature, Fixture.Image.Arch, Error));
    Callee.SourceTypeHint = Signature;
    MedVar Parameter;
    Parameter.Kind = MedVar::Param;
    Parameter.Id = 0;
    Parameter.Size = 8;
    Parameter.TheArch = Fixture.Image.Arch;
    auto ParameterValue = HighExpr::makeVar(Parameter, NdType::makeInt(8));
    HighStmt CalleeReturn;
    CalleeReturn.Kind = StmtKind::Return;
    CalleeReturn.RetVal = HighExpr::makeLoad(ParameterValue, PointerType);
    Callee.Body = {CalleeReturn};

    auto Binding = std::make_shared<SourceCallTypeHint>();
    Binding->CallKind = SourceCallTypeHint::Kind::Native;
    Binding->TargetAddress = Callee.Entry;
    Binding->TargetName = Callee.Name;
    Binding->Signature = Signature;
    auto Call = HighExpr::makeCall(
        Callee.Name, Callee.Entry,
        {HighExpr::makeConst(ObjectFixture::ClassSlot, 8,
                             ConstantAddressProvenance::DataAddress)});
    Call->Type = PointerType;
    Call->SourceCallHint = Binding;
    HighFunc Caller;
    Caller.ReturnType = PointerType;
    HighStmt CallerReturn;
    CallerReturn.Kind = StmtKind::Return;
    CallerReturn.RetVal = Call;
    Caller.Body = {CallerReturn};
    const std::map<va_t, const HighFunc *> Functions{{Callee.Entry, &Callee}};

    const auto Result =
        bindObjCSourceReferences(Caller, Fixture.Image, nullptr, &Functions);
    ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
    EXPECT_EQ(Result.ClassReferenceCells,
              std::set<va_t>{ObjectFixture::ClassSlot});
    const auto Argument = Result.Function.Body[0].RetVal->Operands[0];
    ASSERT_TRUE(Argument->SourceCallHint);
    EXPECT_EQ(Argument->SourceCallHint->CallKind,
              Metaclass
                  ? SourceCallTypeHint::Kind::RuntimeMetaclassReferenceAddress
                  : SourceCallTypeHint::Kind::RuntimeClassReferenceAddress);
    EXPECT_TRUE(objcSourceCallBound(*Argument, Fixture.Image, Functions));
    EXPECT_TRUE(objcSourceCallBound(*Result.Function.Body[0].RetVal,
                                    Fixture.Image, Functions));

    std::set<std::string> Shared;
    const auto Helpers = renderObjCClassReferenceHelpers(
        Fixture.Image, Result.ClassReferenceCells, Shared);
    EXPECT_NE(Helpers.find(Metaclass ? "objc_getMetaClass" : "objc_getClass"),
              std::string::npos);
    const std::string Declaration =
        Metaclass ? "extern struct objc_class *objc_getMetaClass(const char *);"
                  : "extern struct objc_class *objc_getClass(const char *);";
    EXPECT_NE(Helpers.find("struct objc_class;"), std::string::npos);
    EXPECT_NE(Helpers.find(Declaration), std::string::npos);
    EXPECT_EQ(Helpers.find("extern void *objc_get"), std::string::npos);
    EXPECT_EQ(
        Helpers.find(
            Metaclass
                ? "extern struct objc_class *objc_getClass(const char *)"
                : "extern struct objc_class *objc_getMetaClass(const char *)"),
        std::string::npos);
    EXPECT_EQ(Shared, std::set<std::string>{
                          "neverd_objc_class_reference_2010_address"});

    Fixture.Image.ObjCSourceReferences.clear();
    EXPECT_FALSE(objcSourceCallBound(*Argument, Fixture.Image, Functions));
  }
}

TEST(ObjCSourceBindings,
     NativeClassReferenceAddressRejectsStoresEscapesAndWrongWidths) {
  for (unsigned Mutation = 0; Mutation < 3; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ObjectFixture Fixture;
    const auto PointerType = NdType::makePtr(NdType::makeVoid());
    HighFunc Callee;
    Callee.Entry = 0x3000;
    Callee.Name = "unsafe_class_reference";
    Callee.ReturnType = NdType::makeVoid();
    Callee.Params = {{"reference", PointerType}};
    SourceFunctionTypeHint Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Signature.ReturnType = Callee.ReturnType;
    Signature.Parameters = {{"reference", PointerType}};
    std::string Error;
    ASSERT_TRUE(
        assignDarwinScalarSourceABI(Signature, Fixture.Image.Arch, Error));
    Callee.SourceTypeHint = Signature;
    MedVar Parameter;
    Parameter.Kind = MedVar::Param;
    Parameter.Id = 0;
    Parameter.Size = 8;
    Parameter.TheArch = Fixture.Image.Arch;
    auto ParameterValue = HighExpr::makeVar(Parameter, NdType::makeInt(8));
    HighStmt Use;
    if (Mutation == 0) {
      Use.Kind = StmtKind::Store;
      Use.StoreAddr = ParameterValue;
      Use.StoreVal = HighExpr::makeConst(0, 8);
    } else {
      Use.Kind = StmtKind::ExprStmt;
      Use.Val =
          Mutation == 1
              ? HighExpr::makeCall("escape", 0x4000, {ParameterValue})
              : HighExpr::makeLoad(ParameterValue, NdType::makeInt(4, false));
    }
    Callee.Body = {Use};

    auto Binding = std::make_shared<SourceCallTypeHint>();
    Binding->CallKind = SourceCallTypeHint::Kind::Native;
    Binding->TargetAddress = Callee.Entry;
    Binding->TargetName = Callee.Name;
    Binding->Signature = Signature;
    auto Call = HighExpr::makeCall(
        Callee.Name, Callee.Entry,
        {HighExpr::makeConst(ObjectFixture::ClassSlot, 8,
                             ConstantAddressProvenance::DataAddress)});
    Call->Type = NdType::makeVoid();
    Call->SourceCallHint = Binding;
    HighFunc Caller;
    HighStmt Statement;
    Statement.Kind = StmtKind::ExprStmt;
    Statement.Val = Call;
    Caller.Body = {Statement};
    const std::map<va_t, const HighFunc *> Functions{{Callee.Entry, &Callee}};
    const auto Result =
        bindObjCSourceReferences(Caller, Fixture.Image, nullptr, &Functions);
    EXPECT_FALSE(Result.Limitation.empty());
    EXPECT_TRUE(Result.ClassReferenceCells.empty());
    EXPECT_EQ(Result.Function.Body[0].Val->Operands[0]->Kind, ExprKind::Const);
  }
}

TEST(ObjCSourceBindings, MetaObjectNeedsResolvedIsaAndMatchingMetadata) {
  for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
    ObjectFixture Fixture;
    if (Mutation == 0)
      Fixture.Image.MachOHasChainedFixups = true;
    if (Mutation == 1)
      Fixture.put(0x2280, 0); // A class is not a metaclass.
    if (Mutation == 2)
      Fixture.Image.ObjCClasses.push_back(Fixture.Image.ObjCClasses[0]);
    if (Mutation == 3)
      ++Fixture.Image.Sections[0].FileOff;
    auto Function =
        Fixture.message(HighExpr::makeConst(ObjectFixture::MetaAddress, 8));
    auto Result = bindObjCSourceReferences(Function, Fixture.Image);
    EXPECT_FALSE(Result.Limitation.empty()) << Mutation;
    EXPECT_EQ(Result.Function.Body[0].RetVal->Operands[0]->Kind,
              ExprKind::Const);
  }
  ObjectFixture Fixture;
  Fixture.Image.MachOHasChainedFixups = true;
  Fixture.Image.MachOResolvedChainedPointerSlots = {
      ObjectFixture::ClassAddress, ObjectFixture::ClassAddress + 32,
      ObjectFixture::MetaAddress + 32, 0x2218, 0x2298};
  auto Function =
      Fixture.message(HighExpr::makeConst(ObjectFixture::MetaAddress, 8));
  EXPECT_TRUE(
      bindObjCSourceReferences(Function, Fixture.Image).Limitation.empty());
}

TEST(ObjCSourceBindings,
     NativeOrSuperStructurePointersAreNotMessageObjectReceivers) {
  ObjectFixture Fixture;
  for (auto Kind : {SourceCallTypeHint::Kind::Native,
                    SourceCallTypeHint::Kind::ObjCSuper2}) {
    auto Function =
        Fixture.message(HighExpr::makeConst(ObjectFixture::ClassAddress, 8));
    auto Binding = std::make_shared<SourceCallTypeHint>(
        *Function.Body[0].RetVal->SourceCallHint);
    Binding->CallKind = Kind;
    Function.Body[0].RetVal->SourceCallHint = Binding;
    auto Result = bindObjCSourceReferences(Function, Fixture.Image);
    EXPECT_EQ(Result.Function.Body[0].RetVal->Operands[0]->Kind,
              ExprKind::Const);
    EXPECT_FALSE(Result.Limitation.empty());
  }
}

TEST(ObjCSourceBindings, FormatArgumentsRequireCompleteConsistentSlots) {
  auto Parse = [](llvm::StringRef Text) {
    std::vector<uint16_t> Units(Text.begin(), Text.end());
    return objcFormatArgumentTypes(Units);
  };
  auto Types = Parse("text %% %@ %d %hhu %lld %zu %*.*f %s %S %p");
  ASSERT_TRUE(Types);
  ASSERT_EQ(Types->size(), 11U);
  EXPECT_EQ((*Types)[0]->Kind, NdTypeKind::Ptr);
  EXPECT_EQ((*Types)[1]->Size, 4U);
  EXPECT_TRUE((*Types)[2]->IsSigned);
  EXPECT_EQ((*Types)[3]->Size, 8U);
  EXPECT_FALSE((*Types)[4]->IsSigned);
  EXPECT_EQ((*Types)[5]->Size, 4U);
  EXPECT_EQ((*Types)[6]->Size, 4U);
  EXPECT_EQ((*Types)[7]->Kind, NdTypeKind::Float);
  auto Positional = Parse("%3$*1$.*2$f / %3$f");
  ASSERT_TRUE(Positional);
  ASSERT_EQ(Positional->size(), 3U);
  EXPECT_EQ((*Positional)[2]->Kind, NdTypeKind::Float);
  EXPECT_TRUE(Parse("%1000000d"));
  for (const char *Text :
       {"%", "%q", "%n", "%Lf", "%ls", "%1$@ %1$d", "%2$@", "%0$d", "%65$d",
        "%d %2$d", "%*2$d", "%1$d %d", "%99999999999999999999$d", "%.*"})
    EXPECT_FALSE(Parse(Text)) << Text;
  EXPECT_FALSE(objcFormatArgumentTypes(std::vector<uint16_t>{'%', 0x12d, 'd'}));
  EXPECT_FALSE(
      objcFormatArgumentTypes(std::vector<uint16_t>{'a', 0, '%', 'd'}));

  auto ParsePrintf = [](llvm::StringRef Text) {
    return objcFormatArgumentTypes(
        std::vector<uint16_t>(Text.begin(), Text.end()),
        SourceCallTypeHint::FormatSyntax::Printf);
  };
  auto Printf = ParsePrintf("%02x %d %zu %*.*f %s %p");
  ASSERT_TRUE(Printf);
  ASSERT_EQ(Printf->size(), 8U);
  EXPECT_EQ((*Printf)[0]->Kind, NdTypeKind::Int);
  EXPECT_EQ((*Printf)[0]->Size, 4U);
  EXPECT_FALSE((*Printf)[0]->IsSigned);
  EXPECT_TRUE((*Printf)[1]->IsSigned);
  EXPECT_EQ((*Printf)[2]->Size, 8U);
  EXPECT_FALSE((*Printf)[2]->IsSigned);
  EXPECT_EQ((*Printf)[5]->Kind, NdTypeKind::Float);
  EXPECT_EQ((*Printf)[6]->Pointee->Size, 1U);
  EXPECT_EQ((*Printf)[7]->Pointee->Kind, NdTypeKind::Void);
  for (const char *Text : {"%@", "%C", "%S", "%n", "%Lf", "%ls"})
    EXPECT_FALSE(ParsePrintf(Text)) << Text;
}

namespace {
ConstantStringFixture formatFixture(Arch Architecture,
                                    const std::string &Format = "%@ %d %.2f") {
  ConstantStringFixture F;
  F.Image.Arch = Architecture;
  F.Image.DynInfo.NeededLibs.push_back(
      "/System/Library/Frameworks/Foundation.framework/Foundation");
  std::copy(Format.begin(), Format.end(), F.Image.Segments[1].Data.begin());
  F.Image.Segments[1].Data[Format.size()] = 0;
  llvm::support::endian::write64le(F.Image.Segments[0].Data.data() + 24,
                                   Format.size());
  return F;
}
} // namespace

TEST(ObjCSourceBindings,
     PredicateFormatsRespectQuotedTokensAndScalarPromotions) {
  auto Parse = [](llvm::StringRef Text) {
    return objcFormatArgumentTypes(
        std::vector<uint16_t>(Text.begin(), Text.end()),
        SourceCallTypeHint::FormatSyntax::Predicate);
  };
  const auto Types = Parse("%K == %@ AND n > %d AND t < %ld AND f > %f");
  ASSERT_TRUE(Types);
  ASSERT_EQ(Types->size(), 5U);
  EXPECT_EQ((*Types)[0]->Kind, NdTypeKind::Ptr);
  EXPECT_EQ((*Types)[1]->Kind, NdTypeKind::Ptr);
  EXPECT_EQ((*Types)[2]->Size, 4);
  EXPECT_TRUE((*Types)[2]->IsSigned);
  EXPECT_EQ((*Types)[3]->Size, 8);
  EXPECT_EQ((*Types)[4]->Kind, NdTypeKind::Float);
  const auto Quoted = Parse("\"%@ %d\" == '%K' OR SELF == %@");
  ASSERT_TRUE(Quoted);
  ASSERT_EQ(Quoted->size(), 1U);
  EXPECT_EQ(Quoted->front()->Kind, NdTypeKind::Ptr);
  ASSERT_TRUE(Parse("TRUEPREDICATE"));
  EXPECT_TRUE(Parse("TRUEPREDICATE")->empty());
  for (const char *Bad :
       {"'unclosed %@", "\"unclosed %K", "'escaped\\' %@'", "%2$@", "%*d",
        "%.2f", "%n", "%s", "%p", "%lK", "%Lf", "%", "%h", "%ll", "%%", "%llf"})
    EXPECT_FALSE(Parse(Bad)) << Bad;
  std::string TooMany;
  for (unsigned I = 0; I < 65; ++I)
    TooMany += "%@ ";
  EXPECT_FALSE(Parse(TooMany));
  EXPECT_FALSE(
      objcFormatArgumentTypes(std::vector<uint16_t>{'\'', 0, '\''},
                              SourceCallTypeHint::FormatSyntax::Predicate));
  EXPECT_FALSE(objcFormatArgumentTypes(
      {}, static_cast<SourceCallTypeHint::FormatSyntax>(255)));
}

TEST(ObjCSourceBindings, PredicateCallsRevalidateSyntaxDeclarationAndImage) {
  for (Arch A : {Arch::AArch64, Arch::X64})
    for (const char *Selector :
         {"predicateWithFormat:", "expressionWithFormat:"}) {
      auto F = formatFixture(A, "'quoted %@' == %@");
      const auto Declaration = objcSelectorFormatDeclaration(F.Image, Selector);
      ASSERT_TRUE(Declaration);
      EXPECT_EQ(Declaration->Syntax,
                SourceCallTypeHint::FormatSyntax::Predicate);
      EXPECT_FALSE(objcSelectorSourceTypeHint(F.Image, Selector));
      const auto Hint = objcFormattedSourceCallHint(F.Image, Selector, 0x2000);
      ASSERT_TRUE(Hint);
      ASSERT_TRUE(Hint->Format);
      ASSERT_EQ(Hint->Signature.Parameters.size(), 4U);
      auto Call = HighExpr::makeCall("objc_msgSend", 0, {});
      Call->Type = Hint->Signature.ReturnType;
      for (unsigned I = 0; I < 4; ++I)
        Call->Operands.push_back(HighExpr::makeConst(I == 2 ? 0x2000 : 0, 8));
      Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
      EXPECT_TRUE(objcSourceCallBound(*Call, F.Image, {}));
      auto Bad = std::make_shared<SourceCallTypeHint>(*Hint);
      Bad->Format->Syntax = SourceCallTypeHint::FormatSyntax::NSString;
      Call->SourceCallHint = Bad;
      EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {}));
      Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
      ObjCMethod Method;
      Method.Selector = Selector;
      Method.TypeHint = Declaration->Signature;
      F.Image.ObjCMethods.push_back(Method);
      EXPECT_TRUE(objcSourceCallBound(*Call, F.Image, {}));
      F.Image.ObjCMethods.back().TypeHint->Parameters.back().Type =
          NdType::makeInt(8);
      EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {}));
      F.Image.ObjCMethods.clear();
      F.Image.Segments[1].Data[0] = ' ';
      EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {}));
      F.Image.Segments[1].Data[0] = '\'';
      F.Image.Segments[1].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
      EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {}));
      F.Image.Segments[1].Flags = SegmentFlags::Readable;
      F.Image.DynInfo.NeededLibs.clear();
      EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {}));
    }
}

TEST(ObjCSourceBindings, FormattedMessagesRevalidateFormatAndActualArguments) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto F = formatFixture(Architecture);
    EXPECT_FALSE(objcSelectorSourceTypeHint(F.Image, "stringWithFormat:"));
    auto Hint =
        objcFormattedSourceCallHint(F.Image, "stringWithFormat:", 0x2000);
    ASSERT_TRUE(Hint);
    ASSERT_TRUE(Hint->Format);
    ASSERT_EQ(Hint->Signature.Parameters.size(), 6U);
    EXPECT_EQ(Hint->Format->FixedCount, 3U);
    auto Call = HighExpr::makeCall("objc_msgSend", 0, {});
    Call->Type = NdType::makePtr(NdType::makeVoid());
    for (unsigned I = 0; I < 6; ++I)
      Call->Operands.push_back(HighExpr::makeConst(I == 2 ? 0x2000 : 0, 8));
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    EXPECT_TRUE(objcSourceCallBound(*Call, F.Image, {}));
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      auto Bad = *Call;
      auto Binding = std::make_shared<SourceCallTypeHint>(*Hint);
      Bad.SourceCallHint = Binding;
      if (Mutation == 0)
        Binding->Format->FixedCount = 4;
      if (Mutation == 1)
        Binding->Format->FormatParameter = 1;
      if (Mutation == 2)
        Bad.Operands[2] = HighExpr::makeConst(0x2020, 8);
      if (Mutation == 3)
        Bad.Operands.pop_back();
      if (Mutation == 4)
        Binding->Signature.Parameters.back().Type = NdType::makeInt(8);
      if (Mutation == 5)
        Binding->CallKind = SourceCallTypeHint::Kind::Native;
      EXPECT_FALSE(objcSourceCallBound(Bad, F.Image, {})) << Mutation;
    }
    F.Function.Body.front().RetVal = Call;
    // A separately assigned format may be shared elsewhere; source binding
    // must prove this argument occurrence against the original object.
    auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    EXPECT_TRUE(
        objcSourceCallBound(*Bound.Function.Body.front().RetVal, F.Image, {}));
    F.Image.Segments[1].Data[1] = 'n';
    EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {}));
  }
}

TEST(ObjCSourceBindings,
     FormattedControlFlowCandidatesRequireOneExactABIAndDefinitions) {
  auto F = formatFixture(Arch::AArch64, "%@");
  const std::string Alternate = "prefix-%@";
  auto *SecondRecord = F.Image.Segments[0].Data.data() + 32;
  llvm::support::endian::write64le(SecondRecord + 8, 0x7c8);
  llvm::support::endian::write64le(SecondRecord + 16, 0x1080);
  llvm::support::endian::write64le(SecondRecord + 24, Alternate.size());
  std::copy(Alternate.begin(), Alternate.end(),
            F.Image.Segments[1].Data.begin() + 0x80);
  F.Image.Segments[1].Data[0x80 + Alternate.size()] = 0;

  const std::array<va_t, 2> Formats{0x2000, 0x2020};
  auto Hint = objcFormattedSourceCallHint(
      F.Image, "stringWithFormat:", llvm::ArrayRef<va_t>(Formats));
  ASSERT_TRUE(Hint);
  ASSERT_TRUE(Hint->Format);
  EXPECT_EQ(Hint->Format->FormatAddress, 0x2000U);
  EXPECT_EQ(Hint->Format->AlternativeFormatAddresses,
            std::vector<va_t>{0x2020});
  ASSERT_EQ(Hint->Signature.Parameters.size(), 4U);

  MedVar FormatVariable;
  FormatVariable.Kind = MedVar::Temp;
  FormatVariable.Id = 17;
  FormatVariable.Size = 8;
  auto Local = HighExpr::makeVar(FormatVariable, NdType::makeInt(8, false));
  HighStmt First;
  First.Kind = StmtKind::Assign;
  First.Dst = Local;
  First.Val = HighExpr::makeConst(0x2000, 8);
  HighStmt Second = First;
  Second.Val = HighExpr::makeConst(0x2020, 8);
  auto Call =
      HighExpr::makeCall("objc_msgSend", 0,
                         {HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8),
                          Local, HighExpr::makeConst(0, 8)});
  Call->Type = Hint->Signature.ReturnType;
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = Call;
  HighFunc Function;
  Function.ReturnType = Hint->Signature.ReturnType;
  Function.Body = {First, Second, Return};
  EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {}));
  auto Bound = bindObjCSourceReferences(Function, F.Image);
  EXPECT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  const auto &BoundCall = Bound.Function.Body.back().RetVal;
  ASSERT_TRUE(BoundCall);
  EXPECT_TRUE(objcSourceCallBound(*BoundCall, F.Image, {}, nullptr, nullptr,
                                  &Bound.Function));
  for (unsigned I : {0U, 1U}) {
    const auto &Definition = Bound.Function.Body[I].Val;
    ASSERT_TRUE(Definition && Definition->SourceCallHint);
    EXPECT_EQ(Definition->SourceCallHint->CallKind,
              SourceCallTypeHint::Kind::RuntimeConstantString);
  }

  // The same selector and exact objects are insufficient when the formats
  // disagree about the promoted variadic carrier.
  F.Image.Segments[1].Data[0x80 + Alternate.size() - 1] = 'd';
  EXPECT_FALSE(objcFormattedSourceCallHint(
      F.Image, "stringWithFormat:", llvm::ArrayRef<va_t>(Formats)));
}

TEST(ObjCSourceBindings,
     FormattedMessagesRespectEveryDeclarationAndImageProof) {
  auto F = formatFixture(Arch::AArch64);
  auto Declared = objcSelectorFormatDeclaration(F.Image, "stringWithFormat:");
  ASSERT_TRUE(Declared);
  ObjCMethod Method;
  Method.Selector = "stringWithFormat:";
  Method.TypeHint = Declared->Signature;
  F.Image.ObjCMethods.push_back(Method);
  EXPECT_TRUE(objcFormattedSourceCallHint(F.Image, Method.Selector, 0x2000));
  F.Image.ObjCMethods.back().TypeHint->Parameters.back().Type =
      NdType::makeInt(8);
  EXPECT_FALSE(objcFormattedSourceCallHint(F.Image, Method.Selector, 0x2000));
  F.Image.ObjCMethods.clear();
  ObjCProtocol Protocol;
  ObjCProtocolMethod PM;
  PM.Selector = Method.Selector;
  Protocol.Methods.push_back(PM);
  F.Image.ObjCProtocols.push_back(Protocol);
  EXPECT_FALSE(objcFormattedSourceCallHint(F.Image, Method.Selector, 0x2000));
  F.Image.ObjCProtocols.clear();
  F.Image.Segments[1].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  EXPECT_FALSE(objcFormattedSourceCallHint(F.Image, Method.Selector, 0x2000));
  F = formatFixture(Arch::AArch64);
  F.Image.DynInfo.NeededLibs.clear();
  EXPECT_FALSE(objcFormattedSourceCallHint(F.Image, Method.Selector, 0x2000));
}

TEST(ObjCSourceBindings, FormattedLowCallsReadDarwinStackArgumentsBeforeSSA) {
  auto F = formatFixture(Arch::AArch64);
  auto &Image = F.Image;
  const auto &TRI = getTargetRegInfo(Image.Arch);
  Image.ImportPtrSlots[0x3000] = "_objc_msgSend";
  Image.ObjCSourceReferences.emplace(
      0x3008, ObjCSourceReference{ObjCSourceReference::Kind::Selector,
                                  0x3008,
                                  8,
                                  "stringWithFormat:",
                                  {}});
  LowFunc Function;
  Function.Entry = 0x3000;
  Function.Blocks.resize(1);
  auto &Block = Function.Blocks.front();
  Block.StartAddr = 0x3000;
  auto Add = [&](NdOp Code, NdVar Out, std::initializer_list<NdVar> Args) {
    LowOp Op;
    Op.Opcode = Code;
    Op.Output = Out;
    Op.Addr = 0x3000 + Block.Ops.size() * 4;
    for (const auto &Arg : Args)
      Op.addInput(Arg);
    Block.Ops.push_back(Op);
  };
  Add(NdOp::LOAD, NdVar::reg(TRI.IntParamRegs[1], 8), {NdVar::cst(0x3008, 8)});
  Add(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[2], 8), {NdVar::cst(0x2000, 8)});
  Add(NdOp::INDIR_CALL, {}, {NdVar::cst(0x3000, 8)});
  Add(NdOp::RETURN, {}, {});
  const auto Hints = buildObjCSourceCallHints(Image, Function);
  ASSERT_EQ(Hints.size(), 1U);
  ASSERT_TRUE(Hints.begin()->second.Format);
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Image);
  Converter.setSourceCallHintsEnabled(true);
  auto Med = Converter.convert(Function, Image.Arch, BinaryFormat::MachO);
  const MedOp *Call = nullptr;
  for (const auto &B : Med.Blocks)
    for (const auto &Op : B.Ops)
      if (Op.SourceCallHint)
        Call = &Op;
  ASSERT_NE(Call, nullptr);
  ASSERT_EQ(Call->NumInputs, 7U);
  for (unsigned I = 3; I < 6; ++I) {
    EXPECT_EQ(Call->SourceCallHint->Signature.Parameters[I].Location.Kind,
              SourceABICarrierKind::Stack);
    EXPECT_EQ(
        Call->SourceCallHint->Signature.Parameters[I].Location.EntryStackOffset,
        (I - 3) * 8);
  }
  // Tail veneers live in another block. A single incoming edge carries the
  // format fact, independent of block storage order; joins cannot borrow it.
  LowFunc Split = Function;
  Split.Blocks[0].Id = 0;
  Split.Blocks[0].Ops.resize(2);
  Split.Blocks[0].Succs = {1};
  LowBlock Tail;
  Tail.Id = 1;
  Tail.StartAddr = 0x3010;
  Tail.Preds = {0};
  Tail.Ops = {Block.Ops[2], Block.Ops[3]};
  Split.Blocks.push_back(Tail);
  EXPECT_EQ(buildObjCSourceCallHints(Image, Split).size(), 1U);
  std::reverse(Split.Blocks.begin(), Split.Blocks.end());
  EXPECT_EQ(buildObjCSourceCallHints(Image, Split).size(), 1U);
  Split.Blocks.front().Preds.push_back(2);
  EXPECT_TRUE(buildObjCSourceCallHints(Image, Split).empty());
  // Losing the format register fact must not leave a guessed variadic call.
  Block.Ops.insert(Block.Ops.begin() + 2, Block.Ops[1]);
  Block.Ops[2].Inputs[0] = NdVar::reg(TRI.IntParamRegs[3], 8);
  EXPECT_TRUE(buildObjCSourceCallHints(Image, Function).empty());
}

TEST(ObjCSourceBindings,
     FormattedLowCallJoinsExactFormatsWithTheSameVariadicABI) {
  auto F = formatFixture(Arch::AArch64, "%@");
  const std::string Alternate = "prefix-%@";
  auto *SecondRecord = F.Image.Segments[0].Data.data() + 32;
  llvm::support::endian::write64le(SecondRecord + 8, 0x7c8);
  llvm::support::endian::write64le(SecondRecord + 16, 0x1080);
  llvm::support::endian::write64le(SecondRecord + 24, Alternate.size());
  std::copy(Alternate.begin(), Alternate.end(),
            F.Image.Segments[1].Data.begin() + 0x80);
  F.Image.Segments[1].Data[0x80 + Alternate.size()] = 0;
  F.Image.ImportPtrSlots[0x3000] = "_objc_msgSend";
  F.Image.ObjCSourceReferences.emplace(
      0x3008, ObjCSourceReference{ObjCSourceReference::Kind::Selector,
                                  0x3008,
                                  8,
                                  "stringWithFormat:",
                                  {}});
  const auto &TRI = getTargetRegInfo(F.Image.Arch);
  auto Op = [](NdOp Code, NdVar Output, std::initializer_list<NdVar> Inputs,
               va_t Address) {
    LowOp Result;
    Result.Opcode = Code;
    Result.Output = Output;
    Result.Addr = Address;
    for (const auto &Input : Inputs)
      Result.addInput(Input);
    return Result;
  };
  LowFunc Function;
  Function.Entry = 0x3000;
  Function.Blocks.resize(4);
  for (unsigned I = 0; I < 4; ++I) {
    Function.Blocks[I].Id = I;
    Function.Blocks[I].StartAddr = 0x3000 + I * 0x10;
  }
  Function.Blocks[0].Succs = {1, 2};
  for (unsigned I : {1U, 2U}) {
    Function.Blocks[I].Preds = {0};
    Function.Blocks[I].Succs = {3};
    Function.Blocks[I].Ops = {Op(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[2], 8),
                                 {NdVar::cst(I == 1 ? 0x2000 : 0x2020, 8)},
                                 0x3000 + I * 0x10)};
  }
  auto &Join = Function.Blocks[3];
  Join.Preds = {1, 2};
  Join.Ops = {Op(NdOp::LOAD, NdVar::reg(TRI.IntParamRegs[1], 8),
                 {NdVar::cst(0x3008, 8)}, 0x3030),
              Op(NdOp::INDIR_CALL, {}, {NdVar::cst(0x3000, 8)}, 0x3034),
              Op(NdOp::RETURN, {}, {}, 0x3038)};
  const auto Hints = buildObjCSourceCallHints(F.Image, Function);
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(0x3034);
  ASSERT_TRUE(Hint.Format);
  EXPECT_EQ(Hint.Format->FormatAddress, 0x2000U);
  EXPECT_EQ(Hint.Format->AlternativeFormatAddresses, std::vector<va_t>{0x2020});
  ASSERT_EQ(Hint.Signature.Parameters.size(), 4U);

  // Changing either predecessor to a different variadic type revokes it.
  const std::string Different = "%d";
  std::copy(Different.begin(), Different.end(),
            F.Image.Segments[1].Data.begin() + 0x80);
  llvm::support::endian::write64le(SecondRecord + 24, Different.size());
  EXPECT_TRUE(buildObjCSourceCallHints(F.Image, Function).empty());
}

TEST(ObjCSourceBindings,
     FormattedLowCallSelectsExactFormatsWithTheSameVariadicABI) {
  auto F = formatFixture(Arch::AArch64, "value == %@");
  const std::string Alternate = "value == %@ AND enabled == YES";
  auto *SecondRecord = F.Image.Segments[0].Data.data() + 32;
  llvm::support::endian::write64le(SecondRecord + 8, 0x7c8);
  llvm::support::endian::write64le(SecondRecord + 16, 0x1080);
  llvm::support::endian::write64le(SecondRecord + 24, Alternate.size());
  std::copy(Alternate.begin(), Alternate.end(),
            F.Image.Segments[1].Data.begin() + 0x80);
  F.Image.Segments[1].Data[0x80 + Alternate.size()] = 0;
  F.Image.ImportPtrSlots[0x3000] = "_objc_msgSend";
  F.Image.ObjCSourceReferences.emplace(
      0x3008, ObjCSourceReference{ObjCSourceReference::Kind::Selector,
                                  0x3008,
                                  8,
                                  "predicateWithFormat:",
                                  {}});
  const auto &TRI = getTargetRegInfo(F.Image.Arch);
  LowFunc Function;
  Function.Entry = 0x3000;
  Function.Blocks.resize(1);
  auto &Block = Function.Blocks.front();
  Block.StartAddr = 0x3000;
  auto Add = [&](NdOp Code, NdVar Output, std::initializer_list<NdVar> Inputs) {
    LowOp Operation;
    Operation.Opcode = Code;
    Operation.Output = Output;
    Operation.Addr = 0x3000 + Block.Ops.size() * 4;
    for (const auto &Input : Inputs)
      Operation.addInput(Input);
    Block.Ops.push_back(Operation);
  };
  const auto First = NdVar::reg(TRI.IntParamRegs[4], 8);
  const auto Second = NdVar::reg(TRI.IntParamRegs[5], 8);
  Add(NdOp::COPY, First, {NdVar::cst(0x2000, 8)});
  Add(NdOp::COPY, Second, {NdVar::cst(0x2020, 8)});
  Add(NdOp::SELECT, NdVar::reg(TRI.IntParamRegs[2], 8),
      {NdVar::reg(TRI.IntParamRegs[3], 1), Second, First});
  Add(NdOp::LOAD, NdVar::reg(TRI.IntParamRegs[1], 8), {NdVar::cst(0x3008, 8)});
  Add(NdOp::INDIR_CALL, {}, {NdVar::cst(0x3000, 8)});
  Add(NdOp::RETURN, {}, {});

  const auto Hints = buildObjCSourceCallHints(F.Image, Function);
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(0x3010);
  ASSERT_TRUE(Hint.Format);
  EXPECT_EQ(Hint.Format->FormatAddress, 0x2000U);
  EXPECT_EQ(Hint.Format->AlternativeFormatAddresses, std::vector<va_t>{0x2020});
  ASSERT_EQ(Hint.Signature.Parameters.size(), 4U);

  // A different promoted argument type, an unknown arm, or a non-constant
  // selection cannot borrow this bounded candidate set.
  const auto Conversion = 0x80 + Alternate.find('%') + 1;
  F.Image.Segments[1].Data[Conversion] = 'd';
  EXPECT_TRUE(buildObjCSourceCallHints(F.Image, Function).empty());
  F.Image.Segments[1].Data[Conversion] = '@';
  const auto SavedArm = Block.Ops[2].Inputs[1];
  Block.Ops[2].Inputs[1] = NdVar::reg(TRI.IntParamRegs[6], 8);
  EXPECT_TRUE(buildObjCSourceCallHints(F.Image, Function).empty());
  Block.Ops[2].Inputs[1] = SavedArm;

  auto Select = std::make_shared<HighExpr>();
  Select->Kind = ExprKind::BinOp;
  Select->Op = NdOp::SELECT;
  Select->Type = NdType::makeInt(8, false);
  Select->Operands = {HighExpr::makeConst(0, 1), HighExpr::makeConst(0x2020, 8),
                      HighExpr::makeConst(0x2000, 8)};
  auto Call =
      HighExpr::makeCall("objc_msgSend", 0,
                         {HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8),
                          Select, HighExpr::makeConst(0, 8)});
  Call->Type = Hint.Signature.ReturnType;
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Hint);
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = Call;
  HighFunc High;
  High.ReturnType = Hint.Signature.ReturnType;
  High.Body = {Return};
  Select->Operands[1] = HighExpr::makeConst(0x2040, 8);
  EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {}));
  Select->Operands[1] = HighExpr::makeConst(0x2020, 8);
  auto Bound = bindObjCSourceReferences(High, F.Image);
  EXPECT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  const auto &BoundSelect = Bound.Function.Body.front().RetVal->Operands[2];
  ASSERT_TRUE(BoundSelect);
  ASSERT_EQ(BoundSelect->Operands.size(), 3U);
  for (unsigned I : {1U, 2U}) {
    const auto &Arm = BoundSelect->Operands[I];
    ASSERT_TRUE(Arm && Arm->SourceCallHint);
    EXPECT_EQ(Arm->SourceCallHint->CallKind,
              SourceCallTypeHint::Kind::RuntimeConstantString);
  }
  EXPECT_TRUE(
      objcSourceCallBound(*Bound.Function.Body.front().RetVal, F.Image, {}));
}

TEST(ObjCSourceBindings, DarwinFormattedCallsRequireExactImportAndFormat) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto F = formatFixture(Architecture);
    Segment Import;
    Import.VA = Import.FileOff = 0x3000;
    Import.Size = Import.FileSz = 8;
    Import.Flags = SegmentFlags::Readable;
    Import.Data.resize(8);
    F.Image.Segments.push_back(Import);
    Section Slot;
    Slot.VA = Slot.FileOff = 0x3000;
    Slot.Size = Slot.FileSz = 8;
    Slot.Flags = SegmentFlags::Readable;
    F.Image.Sections.push_back(Slot);
    ASSERT_TRUE(F.Image.recordDyldBindSlot(
        0x3000, "_NSLog", 0,
        "/System/Library/Frameworks/Foundation.framework/Foundation", false));
    F.Image.ImportPtrSlots[0x3000] = "_NSLog";
    ASSERT_TRUE(darwinRuntimeFormatDeclaration(F.Image, 0x3000));
    ASSERT_TRUE(readObjCConstantString(F.Image, 0x2000));
    auto Hint = darwinFormattedSourceCallHint(F.Image, 0x3000, 0x2000);
    ASSERT_TRUE(Hint);
    ASSERT_TRUE(Hint->Format);
    EXPECT_EQ(Hint->Format->FixedCount, 1U);
    EXPECT_EQ(Hint->Format->FormatParameter, 0U);
    ASSERT_EQ(Hint->Signature.Parameters.size(), 4U);
    auto Call = HighExpr::makeCall(
        "_NSLog", 0x3000,
        {HighExpr::makeConst(0x2000, 8), HighExpr::makeConst(0, 8),
         HighExpr::makeConst(17, 4), HighExpr::makeConst(0, 8)});
    Call->Type = NdType::makeVoid();
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    EXPECT_TRUE(objcSourceCallBound(*Call, F.Image, {}));
    for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
      auto Bad = std::make_shared<SourceCallTypeHint>(*Hint);
      Call->SourceCallHint = Bad;
      if (Mutation == 0)
        Bad->TargetName = "NSLogv";
      if (Mutation == 1)
        Bad->TargetAddress = 0x3008;
      if (Mutation == 2)
        Bad->Format->FixedCount = 2;
      if (Mutation == 3)
        Bad->Format->FormatParameter = 1;
      if (Mutation == 4)
        Bad->Format.reset();
      EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {})) << Mutation;
    }
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    Call->Operands[0] = HighExpr::makeConst(0x2020, 8);
    EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {}));
    F.Image.DyldBindSlots[0x3000].Module = "/tmp/private/Foundation";
    EXPECT_FALSE(darwinFormattedSourceCallHint(F.Image, 0x3000, 0x2000));
    F.Image.DyldBindSlots[0x3000].Module =
        "/System/Library/Frameworks/Foundation.framework/Foundation";
    F.Image.DyldBindSlots[0x3000].WeakImport = true;
    EXPECT_FALSE(darwinFormattedSourceCallHint(F.Image, 0x3000, 0x2000));
  }
}

namespace {
BinaryImage cFormatImage(Arch Architecture, llvm::StringRef Format) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Architecture;
  Image.Bits = Bitness::Bits64;
  Segment Data;
  Data.Name = "__TEXT";
  Data.VA = Data.FileOff = 0x2000;
  Data.Size = Data.FileSz = 0x100;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(Data.Size);
  std::copy(Format.begin(), Format.end(), Data.Data.begin());
  Data.Data[Format.size()] = 0;
  Image.Segments.push_back(Data);
  Section Strings;
  Strings.Name = "__cstring";
  Strings.SegmentName = Data.Name;
  Strings.VA = Strings.FileOff = Data.VA;
  Strings.Size = Strings.FileSz = Data.Size;
  Strings.Flags = Data.Flags;
  Strings.Type = llvm::MachO::S_CSTRING_LITERALS;
  Image.Sections.push_back(Strings);
  Segment Imports;
  Imports.Name = "__DATA_CONST";
  Imports.VA = Imports.FileOff = 0x3000;
  Imports.Size = Imports.FileSz = 8;
  Imports.Flags = SegmentFlags::Readable;
  Imports.Data.resize(8);
  Image.Segments.push_back(Imports);
  Section Slots;
  Slots.Name = "__got";
  Slots.SegmentName = Imports.Name;
  Slots.VA = Slots.FileOff = Imports.VA;
  Slots.Size = Slots.FileSz = Imports.Size;
  Slots.Flags = Imports.Flags;
  Image.Sections.push_back(Slots);
  Image.ImportPtrSlots[0x3000] = "_snprintf";
  EXPECT_TRUE(Image.recordDyldBindSlot(
      0x3000, "_snprintf", 0, "/usr/lib/system/libsystem_c.dylib", false));
  return Image;
}
} // namespace

TEST(ObjCSourceBindings,
     DarwinPrintfCallsRequireExactImportAndImmutableCString) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = cFormatImage(Architecture, "%02x");
    EXPECT_FALSE(darwinRuntimeSourceCallHint(Image, 0x3000));
    const auto Declaration = darwinRuntimeFormatDeclaration(Image, 0x3000);
    ASSERT_TRUE(Declaration);
    EXPECT_EQ(Declaration->Name, "snprintf");
    EXPECT_EQ(Declaration->FormatParameter, 2U);
    EXPECT_EQ(Declaration->Syntax, SourceCallTypeHint::FormatSyntax::Printf);
    EXPECT_EQ(Declaration->Signature.ReturnType->Kind, NdTypeKind::Int);
    EXPECT_TRUE(Declaration->Signature.ReturnType->IsSigned);
    ASSERT_EQ(Declaration->Signature.Parameters.size(), 3U);
    EXPECT_EQ(Declaration->Signature.Parameters[0].Type->Pointee->Size, 1U);
    EXPECT_EQ(Declaration->Signature.Parameters[1].Type->Size, 8U);
    EXPECT_FALSE(Declaration->Signature.Parameters[1].Type->IsSigned);

    const auto Hint = darwinFormattedSourceCallHint(Image, 0x3000, 0x2000);
    ASSERT_TRUE(Hint);
    ASSERT_TRUE(Hint->Format);
    EXPECT_EQ(Hint->Format->FixedCount, 3U);
    EXPECT_EQ(Hint->Format->FormatParameter, 2U);
    EXPECT_EQ(Hint->Format->Syntax, SourceCallTypeHint::FormatSyntax::Printf);
    ASSERT_EQ(Hint->Signature.Parameters.size(), 4U);
    EXPECT_EQ(Hint->Signature.Parameters[3].Type->Size, 4U);
    EXPECT_FALSE(Hint->Signature.Parameters[3].Type->IsSigned);
    if (Architecture == Arch::AArch64) {
      EXPECT_EQ(Hint->Signature.Parameters[3].Location.Kind,
                SourceABICarrierKind::Stack);
      EXPECT_EQ(Hint->Signature.Parameters[3].Location.EntryStackOffset, 0);
    }

    auto Call = HighExpr::makeCall(
        "_snprintf", 0x3000,
        {HighExpr::makeConst(0, 8), HighExpr::makeConst(3, 8),
         HighExpr::makeConst(0x2000, 8), HighExpr::makeConst(42, 4)});
    Call->Type = Hint->Signature.ReturnType;
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    EXPECT_TRUE(objcSourceCallBound(*Call, Image, {}));
    HighFunc Function;
    Function.ReturnType = Hint->Signature.ReturnType;
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = Call;
    Function.Body = {Return};
    const auto Bound = bindObjCSourceReferences(Function, Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    EXPECT_EQ(Bound.CStringSections, std::set<va_t>{0x2000});
    const auto BoundCall = Bound.Function.Body[0].RetVal;
    ASSERT_TRUE(BoundCall);
    ASSERT_EQ(BoundCall->Operands.size(), 4U);
    ASSERT_TRUE(BoundCall->Operands[2]->SourceCallHint);
    EXPECT_EQ(BoundCall->Operands[2]->SourceCallHint->CallKind,
              SourceCallTypeHint::Kind::RuntimeCStringStorage);
    EXPECT_TRUE(objcSourceCallBound(*BoundCall, Image, {}));
    auto Bad = std::make_shared<SourceCallTypeHint>(*Hint);
    Bad->Format->Syntax = SourceCallTypeHint::FormatSyntax::NSString;
    Call->SourceCallHint = Bad;
    EXPECT_FALSE(objcSourceCallBound(*Call, Image, {}));

    auto WrongProvider = Image;
    WrongProvider.DyldBindSlots[0x3000].Module = "/tmp/libsystem_c.dylib";
    EXPECT_FALSE(darwinRuntimeFormatDeclaration(WrongProvider, 0x3000));
    auto Writable = Image;
    Writable.Segments[0].Flags =
        Writable.Segments[0].Flags | SegmentFlags::Writable;
    Writable.Sections[0].Flags =
        Writable.Sections[0].Flags | SegmentFlags::Writable;
    EXPECT_FALSE(darwinFormattedSourceCallHint(Writable, 0x3000, 0x2000));
    auto FixedUp = Image;
    FixedUp.DataPtrRelocSlots.insert(0x2000);
    EXPECT_FALSE(darwinFormattedSourceCallHint(FixedUp, 0x3000, 0x2000));
    auto Unterminated = Image;
    std::fill(Unterminated.Segments[0].Data.begin(),
              Unterminated.Segments[0].Data.end(), 'x');
    EXPECT_FALSE(darwinFormattedSourceCallHint(Unterminated, 0x3000, 0x2000));
    auto Dangerous = cFormatImage(Architecture, "%n");
    EXPECT_FALSE(darwinFormattedSourceCallHint(Dangerous, 0x3000, 0x2000));
    auto WrongName = Image;
    WrongName.ImportPtrSlots[0x3000] = "_sprintf";
    WrongName.DyldBindSlots[0x3000].Name = "_sprintf";
    EXPECT_FALSE(darwinRuntimeFormatDeclaration(WrongName, 0x3000));
  }
}

TEST(SwiftValueWitnessCalls, NonreturningRegisterWitnessNeedsNoFrameReceipt) {
  immutable_native_call_test::Fixture F;
  // A direct register-derived witness precedes a terminal trap. No value is
  // reloaded from this frame, so the frame-specific returning-body proof does
  // not apply. The ordinary witness and no-return gates remain independent.
  const uint32_t Words[] = {0xa9bf7bfd, 0xf85f8048, 0xf9400908, 0xd63f0100,
                            0xd4200020};
  for (unsigned I = 0; I < std::size(Words); ++I)
    F.word(I, Words[I]);
  F.Image.Symbols[0].Size = sizeof(Words);
  F.EntrySignature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  F.EntrySignature.ReturnType = NdType::makeVoid();
  for (unsigned I = 0; I < 3; ++I)
    F.EntrySignature.Parameters.push_back(
        {"arg" + std::to_string(I), NdType::makePtr(NdType::makeVoid())});
  std::string Error;
  ASSERT_TRUE(
      assignDarwinScalarSourceABI(F.EntrySignature, Arch::AArch64, Error))
      << Error;
  F.run();
  ASSERT_TRUE(F.low());
  ASSERT_TRUE(F.med());
  ASSERT_TRUE(F.high());
  ASSERT_TRUE(F.med()->DoesNotReturn);
  ASSERT_TRUE(F.high()->DoesNotReturn);
  unsigned Witnesses = 0;
  for (const auto &B : F.med()->Blocks)
    for (const auto &O : B.Ops)
      if (O.SourceCallHint && O.SourceCallHint->ValueWitness) {
        ++Witnesses;
        EXPECT_FALSE(O.SourceCallHint->SwiftWitnessFrame);
      }
  ASSERT_EQ(Witnesses, 1U);
  EXPECT_TRUE(validateSwiftWitnessFrameBindings(F.Image, F.low(), *F.med()));
  EXPECT_TRUE(SourceSwiftWitnessFrameProjectionValidator(F.Image, F.Result)
                  .valid(*F.high()));
}

TEST(SwiftValueWitnessCalls, PublicationReplaysCurrentFrameOccurrence) {
  immutable_native_call_test::Fixture F;
  swift_witness_frame_test::frameWitnessFixture(F);
  ASSERT_TRUE(F.high());
  ASSERT_TRUE(F.med());
  EXPECT_TRUE(validateSwiftWitnessFrameBindings(F.Image, F.low(), *F.med()));
  EXPECT_TRUE(SourceSwiftWitnessFrameProjectionValidator(F.Image, F.Result)
                  .valid(*F.high()));
  std::map<va_t, const HighFunc *> Functions;
  for (const auto &Function : F.Result.HighFuncs)
    Functions.emplace(Function.Entry, &Function);
  auto Bound =
      bindObjCSourceReferences(*F.high(), F.Image, nullptr, &Functions);
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  elimUnreadPrivateFrameStores(Bound.Function, F.Image.Arch);
  EXPECT_TRUE(SourceSwiftWitnessFrameProjectionValidator(F.Image, F.Result)
                  .valid(Bound.Function));
}

TEST(SwiftValueWitnessCalls,
     FramePublicationRejectsStaleAndRepeatedOccurrences) {
  for (unsigned Case = 0; Case < 32; ++Case) {
    SCOPED_TRACE(Case);
    immutable_native_call_test::Fixture F;
    swift_witness_frame_test::frameWitnessFixture(F);
    ASSERT_TRUE(F.high());
    ASSERT_TRUE(F.med());
    ExprPtr Call;
    walkStmts(F.high()->Body, [&](HighStmt &S) {
      forEachExpr(S, [&](ExprPtr &Root) {
        std::vector<ExprPtr> Pending{Root};
        while (!Pending.empty()) {
          auto E = Pending.back();
          Pending.pop_back();
          if (!E)
            continue;
          if (E->SourceCallHint && E->SourceCallHint->SwiftWitnessFrame)
            Call = E;
          E->forEachChildExpr([&](const ExprPtr &C) { Pending.push_back(C); });
        }
      });
    });
    ASSERT_TRUE(Call);
    auto Hint = *Call->SourceCallHint;
    MedOp *MedCall = nullptr;
    for (auto &B : F.med()->Blocks)
      for (auto &O : B.Ops)
        if (O.Opcode == NdOp::INDIR_CALL)
          MedCall = &O;
    ASSERT_TRUE(MedCall);
    switch (Case) {
    case 0:
      F.word(9, 0xb90023e8);
      break;
    case 1:
      F.Image.DyldBindSlots.at(0x2000).WeakImport = true;
      break;
    case 2:
      Hint.SwiftWitnessFrame->FunctionEntry += 4;
      break;
    case 3:
      ++Hint.SwiftWitnessFrame->Site.Sequence;
      break;
    case 4:
      Hint.SwiftWitnessFrame->Site.Instruction += 4;
      break;
    case 5:
      Hint.SwiftWitnessFrame->Site.Opcode = NdOp::CALL;
      break;
    case 6:
      Hint.SwiftWitnessFrame->Site.StaticTarget = 0x1100;
      break;
    case 7:
      Hint.SwiftWitnessFrame.reset();
      break;
    case 8:
      Hint.Signature.Parameters.back().Location.RegisterOffset += 8;
      break;
    case 9:
      Hint.Signature.Parameters.back().Type = NdType::makeInt(4);
      break;
    case 10:
      Hint.WeakImport = true;
      break;
    case 11:
      Hint.ReturnedArgument = 0;
      break;
    case 12:
      Hint.ValueWitness =
          SourceCallTypeHint::SwiftValueWitnessKind::AssignWithCopy;
      break;
    case 13:
      Call->Operands.back() = HighExpr::makeConst(0, 8);
      break;
    case 14:
      std::swap(Call->Operands[0], Call->Operands[1]);
      break;
    case 15:
      Call->IndirectTarget = HighExpr::makeConst(0x1200, 8);
      break;
    case 16:
      Call->IsIndirectCall = false;
      break;
    case 17:
      Call->Operands.pop_back();
      break;
    case 18:
      Call->Type = NdType::makeInt(4);
      break;
    case 19: {
      HighStmt S;
      S.Kind = StmtKind::Call;
      S.CallExpr = Call;
      F.high()->Body.push_back(S);
      break;
    }
    case 20: {
      auto H = *MedCall->SourceCallHint;
      H.SwiftWitnessFrame.reset();
      MedCall->SourceCallHint = std::make_shared<const SourceCallTypeHint>(H);
      break;
    }
    case 21:
      --MedCall->NumInputs;
      break;
    case 22:
      ++MedCall->OriginSeq;
      break;
    case 23:
      MedCall->PreservesCallerSaved = true;
      break;
    case 24:
      F.Result.LowFuncs.push_back(*F.low());
      break;
    case 25:
      for (auto &A : F.Result.FunctionAudits)
        if (A.Entry == 0x1000)
          A.MedIRVerified = false;
      break;
    case 26:
      F.med()->SourceParametersBound = false;
      break;
    case 27:
      F.med()->DoesNotReturn = true;
      F.high()->DoesNotReturn = true;
      break;
    case 28:
      F.med()->DoesNotReturn = true;
      break;
    case 29:
      F.high()->DoesNotReturn = true;
      break;
    case 30:
    case 31: {
      F.med()->DoesNotReturn = true;
      F.high()->DoesNotReturn = true;
      auto H = *MedCall->SourceCallHint;
      H.SwiftWitnessFrame.reset();
      MedCall->SourceCallHint = std::make_shared<const SourceCallTypeHint>(H);
      if (Case == 31)
        Hint.SwiftWitnessFrame.reset();
      break;
    }
    }
    Call->SourceCallHint = std::make_shared<const SourceCallTypeHint>(Hint);
    EXPECT_FALSE(SourceSwiftWitnessFrameProjectionValidator(F.Image, F.Result)
                     .valid(*F.high()));
  }
}

TEST(SwiftValueWitnessCalls, GeneratedFrameWitnessSourceMatchesOriginalARM64) {
#if defined(NEVERD_TEST_CLANG) && defined(__APPLE__) && defined(__aarch64__)
  using namespace immutable_native_call_test;
  immutable_native_call_test::Fixture F;
  swift_witness_frame_test::frameWitnessFixture(F);
  ASSERT_TRUE(F.high());
  std::map<va_t, const HighFunc *> Functions;
  for (const auto &Function : F.Result.HighFuncs)
    Functions.emplace(Function.Entry, &Function);
  auto Bound =
      bindObjCSourceReferences(*F.high(), F.Image, nullptr, &Functions);
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  elimUnreadPrivateFrameStores(Bound.Function, F.Image.Arch);
  ASSERT_TRUE(SourceSwiftWitnessFrameProjectionValidator(F.Image, F.Result)
                  .valid(Bound.Function));
  const auto Audit =
      std::find_if(F.Result.FunctionAudits.begin(),
                   F.Result.FunctionAudits.end(), [](const auto &A) {
                     return A.Entry == immutable_native_call_test::Entry;
                   });
  ASSERT_NE(Audit, F.Result.FunctionAudits.end());
  const auto Allowed = [&](const HighExpr &E) {
    return objcSourceCallBound(E, F.Image, Functions, nullptr, nullptr,
                               &Bound.Function);
  };
  const auto Limitation = sourceBodyLimitation(
      Bound.Function, *Bound.Function.SourceTypeHint, &*Audit, Allowed);
  ASSERT_TRUE(Limitation.empty()) << Limitation;
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit({Bound.Function}, OS, Options));
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-witness-frame", Directory));
  const std::filesystem::path Work(Directory.str().str());
  struct Cleanup {
    std::filesystem::path Work;
    ~Cleanup() {
      std::error_code E;
      std::filesystem::remove_all(Work, E);
    }
  } Cleanup{Work};
  const auto Path = (Work / "witness.c").string();
  std::ofstream(Path) << Source << R"(
#include <stdio.h>
extern void *original_witness(void *,void *,void *,uint64_t,void *);
__asm__(".text\n.p2align 2\n.globl _original_witness\n_original_witness:\n"
        ".long 0xa9bb53f3,0xa9015bf5,0xa9047bfd,0xaa0003f3,0xaa0103f4\n"
        ".long 0xaa0203f5,0x92401476,0x910006d6,0xf85f82a8,0xf90013e8,0xaa0403e0\n"
        "bl _objc_release\n"
        ".long 0xf10006d6,0x54ffffe1,0xf94013e8,0xf9400908\n"
        ".long 0xaa1303e0,0xaa1403e1,0xaa1503e2,0xd63f0100\n"
        ".long 0xa9447bfd,0xa9415bf5,0xa8c553f3,0xd65f03c0\n");
static uint64_t table[8], metadata[3], calls, selected;
static void *expected_dst, *expected_src;
static void * __attribute__((swiftcall)) copy_a(void *d,void *s,void *m) {
  if(d!=expected_dst || s!=expected_src || m!=metadata+1) __builtin_trap();
  ++calls; selected=1;
  ((uint64_t*)d)[0]=((uint64_t*)s)[0]^metadata[1];
  ((uint64_t*)d)[1]=((uint64_t*)s)[1]+metadata[2];
  return d;
}
static void * __attribute__((swiftcall)) copy_b(void *d,void *s,void *m) {
  void *r=copy_a(d,s,m); selected=2; return r;
}
int main(void) {
  const uint32_t words[]={0xa9bb53f3,0xa9015bf5,0xa9047bfd,0xaa0003f3,0xaa0103f4,
    0xaa0203f5,0x92401476,0x910006d6,0xf85f82a8,0xf90013e8,0xaa0403e0,0x94000035,
    0xf10006d6,0x54ffffe1,0xf94013e8,0xf9400908,0xaa1303e0,0xaa1403e1,
    0xaa1503e2,0xd63f0100,0xa9447bfd,0xa9415bf5,0xa8c553f3,0xd65f03c0};
  const uint32_t *machine=(const uint32_t*)(uintptr_t)original_witness;
  for(unsigned i=0;i<24;++i){uint32_t mask=i==11?0xfc000000U:UINT32_MAX;
    if((machine[i]&mask)!=(words[i]&mask))return 1;}
  uint64_t state=UINT64_C(0xb123456789abcdef);
  for(unsigned i=0;i<2048;++i){
    state=state*UINT64_C(6364136223846793005)+1;
    uint64_t s[]={state,state^UINT64_C(0x71823456abcdef98)};
    uint64_t a[]={17,0,0,31},b[]={17,0,0,31};
    metadata[0]=(uintptr_t)table;metadata[1]=state;metadata[2]=~state;
    unsigned choice=i%2+1;
    table[2]=(uintptr_t)(choice==1?copy_a:copy_b);
    expected_src=s;expected_dst=a+1;calls=0;selected=0;
    void *ra=original_witness(a+1,s,metadata+1,state,0);
    if(ra!=a+1||calls!=1||selected!=choice)return 2;
    expected_dst=b+1;calls=0;selected=0;
    void *rb=indirect_native(b+1,s,metadata+1,state,0);
    if(rb!=b+1||calls!=1||selected!=choice||memcmp(a,b,sizeof(a)))return 3;
    if(a[0]!=17||a[3]!=31||a[1]!=(s[0]^state)||a[2]!=s[1]+~state)return 4;
    if(s[0]!=state||s[1]!=(state^UINT64_C(0x71823456abcdef98)))return 5;
  }
  return 0;
}
)";
  for (const auto *Level : {"-O0", "-O2"}) {
    SCOPED_TRACE(Level);
    const auto Output = (Work / (std::string("witness") + Level)).string();
    const std::vector<llvm::StringRef> Args{NEVERD_TEST_CLANG, Level, Path,
                                            "-lobjc",          "-o",  Output};
    ASSERT_EQ(llvm::sys::ExecuteAndWait(NEVERD_TEST_CLANG, Args), 0);
    EXPECT_EQ(llvm::sys::ExecuteAndWait(Output, {Output}), 0) << Source;
  }
#else
  GTEST_SKIP() << "Original ARM64 comparison requires Apple ARM64 and Clang";
#endif
}

TEST(SwiftValueWitnessCalls, ScalarInferenceRechecksFrameBinding) {
  for (unsigned Case = 0; Case < 10; ++Case) {
    SCOPED_TRACE(Case);
    immutable_native_call_test::Fixture F;
    swift_witness_frame_test::frameWitnessFixture(F);
    F.EntrySignature = {};
    F.run();
    ASSERT_TRUE(F.high());
    ASSERT_TRUE(F.med());
    ASSERT_TRUE(F.low());
    if (Case == 1)
      F.word(9, 0xb90023e8);
    if (Case == 2)
      F.Image.DyldBindSlots.at(0x2000).WeakImport = true;
    for (auto &B : F.med()->Blocks)
      for (auto &O : B.Ops) {
        if (O.Opcode != NdOp::INDIR_CALL)
          continue;
        ASSERT_TRUE(O.SourceCallHint);
        auto H = *O.SourceCallHint;
        if (Case == 3)
          H.SwiftWitnessFrame.reset();
        if (Case == 4)
          ++H.SwiftWitnessFrame->Site.Sequence;
        if (Case == 5)
          H.ValueWitness =
              SourceCallTypeHint::SwiftValueWitnessKind::AssignWithCopy;
        if (Case == 6)
          --O.NumInputs;
        if (Case == 7)
          O.DoesNotReturn = true;
        if (Case == 8)
          O.Inputs[3].Size = 4;
        if (Case == 9)
          H.Signature.Parameters.back().Location.RegisterOffset += 8;
        O.SourceCallHint = std::make_shared<const SourceCallTypeHint>(H);
      }
    auto A = std::find_if(F.Result.FunctionAudits.begin(),
                          F.Result.FunctionAudits.end(),
                          [](const auto &A) { return A.Entry == 0x1000; });
    ASSERT_NE(A, F.Result.FunctionAudits.end());
    std::string Reason;
    const auto Hint = inferNativeSourceTypeHint(F.Image, *F.med(), *F.high(),
                                                *A, Reason, F.low());
    EXPECT_EQ(bool(Hint), Case == 0) << Reason;
  }
}
