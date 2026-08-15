//===- CodeGen.cpp - LLVM IR to machine code generation ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// LLVM IR to native object code and fixed-up rewrite output.
///
/// Patch-image pointer symbolization lives in CodeGenSymbolize.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/CodeGen.h"

#include "CodeGenDetail.h"

#define DEBUG_TYPE "neverd-codegen"
#include "neverd/ArchSupport.h"
#include "neverd/object/SectionNames.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/CodeGen/CommandFlags.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/BinaryRewrite.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <set>

namespace neverd {

// True when a type is (or is a vector of) IEEE half — i.e. the module performs
// half-precision (FEAT_FP16) arithmetic and the backend needs +fullfp16 to
// select fp16 instructions instead of softening to float.
static bool typeUsesHalf(llvm::Type *T) {
  if (T->isHalfTy())
    return true;
  if (auto *VT = llvm::dyn_cast<llvm::VectorType>(T))
    return VT->getElementType()->isHalfTy();
  return false;
}

static std::set<std::string> scanIntrinsicPatterns(llvm::Module &Mod) {
  std::set<std::string> Seen;
  for (auto &Fn : Mod)
    for (auto &BB : Fn)
      for (auto &Inst : BB) {
        if (auto *CI = llvm::dyn_cast<llvm::CallInst>(&Inst))
          if (auto *Callee = CI->getCalledFunction())
            Seen.insert(Callee->getName().str());
        // Half-precision arithmetic is emitted as native `half` ops (not
        // intrinsics), so flag it by scanning instruction/operand types.
        if (typeUsesHalf(Inst.getType()))
          Seen.insert(kUsesHalfMarker);
        else
          for (auto &Op : Inst.operands())
            if (typeUsesHalf(Op->getType())) {
              Seen.insert(kUsesHalfMarker);
              break;
            }
      }
  return Seen;
}

// Scan the module once, then dispatch to the per-target feature detector.
// The architecture-specific {CPU, feature-string} logic lives in
// CodeGen{X86,AArch64,ARM}.cpp (LLVM target-dispatch pattern).
static std::pair<std::string, std::string>
detectTargetFeatures(llvm::Module &Mod, Arch TheArch) {
  auto Names = scanIntrinsicPatterns(Mod);
  switch (TheArch) {
  case Arch::X64:
  case Arch::X86:
    return detectTargetFeaturesX86(Names);
  case Arch::AArch64:
    return detectTargetFeaturesAArch64(Names);
  case Arch::ARM:
    return detectTargetFeaturesARM(Names);
  default:
    return {"", ""};
  }
}

/// Expand @llvm.fshl.iN / @llvm.fshr.iN for N < 32 into explicit
/// shift-mask-or sequences using i32, and widen narrow return types
/// (i8/i16) to i32 so the ARM/AArch64 backends don't leave upper
/// register bits undefined.
static void expandNarrowFunnelShifts(llvm::Module &Mod) {
  auto &Ctx = Mod.getContext();
  auto *I32 = llvm::Type::getInt32Ty(Ctx);

  // --- Part 1: Expand narrow fshl/fshr intrinsics ---
  llvm::SmallVector<llvm::CallInst *, 8> ToExpand;
  for (auto &Fn : Mod)
    for (auto &BB : Fn)
      for (auto &Inst : BB)
        if (auto *II = llvm::dyn_cast<llvm::IntrinsicInst>(&Inst))
          if (II->getIntrinsicID() == llvm::Intrinsic::fshl ||
              II->getIntrinsicID() == llvm::Intrinsic::fshr)
            if (II->getType()->isIntegerTy() &&
                II->getType()->getIntegerBitWidth() < 32)
              ToExpand.push_back(II);

  for (auto *CI : ToExpand) {
    unsigned BitW = CI->getType()->getIntegerBitWidth();
    bool IsFshl = CI->getIntrinsicID() == llvm::Intrinsic::fshl;
    llvm::IRBuilder<> B(CI);

    llvm::Value *A = B.CreateZExt(CI->getArgOperand(0), I32);
    llvm::Value *BV = B.CreateZExt(CI->getArgOperand(1), I32);
    llvm::Value *Amt = B.CreateZExt(CI->getArgOperand(2), I32);
    llvm::Value *ModAmt = B.CreateURem(Amt, B.getInt32(BitW));
    llvm::Value *RevAmt = B.CreateSub(B.getInt32(BitW), ModAmt);

    llvm::Value *Hi, *Lo;
    if (IsFshl) {
      Hi = B.CreateShl(A, ModAmt);
      Lo = B.CreateLShr(BV, RevAmt);
    } else {
      Hi = B.CreateShl(A, RevAmt);
      Lo = B.CreateLShr(BV, ModAmt);
    }

    llvm::Value *Or = B.CreateOr(Hi, Lo);
    llvm::Value *Masked = B.CreateAnd(Or, B.getInt32((1u << BitW) - 1));
    llvm::Value *Result = B.CreateTrunc(Masked, CI->getType());

    CI->replaceAllUsesWith(Result);
    CI->eraseFromParent();
  }

  // --- Part 2: Widen narrow return values ---
  // ARM/AArch64 backends may leave upper register bits undefined for
  // sub-32-bit return types. Replace each narrow `ret iN` with
  // `ret i32 (zext iN to i32)` using a new function clone.
  llvm::SmallVector<llvm::Function *, 4> NarrowRetFuncs;
  for (auto &Fn : Mod)
    if (Fn.getReturnType()->isIntegerTy() &&
        Fn.getReturnType()->getIntegerBitWidth() < 32 && !Fn.isDeclaration())
      NarrowRetFuncs.push_back(&Fn);

  for (auto *OldFn : NarrowRetFuncs) {
    auto *OldTy = OldFn->getFunctionType();
    auto *NewTy =
        llvm::FunctionType::get(I32, OldTy->params(), OldTy->isVarArg());
    auto *NewFn = llvm::Function::Create(NewTy, OldFn->getLinkage(),
                                         OldFn->getName() + ".wide", &Mod);
    NewFn->setAttributes(OldFn->getAttributes());

    llvm::ValueToValueMapTy VMap;
    for (auto OldI = OldFn->arg_begin(), NewI = NewFn->arg_begin();
         OldI != OldFn->arg_end(); ++OldI, ++NewI) {
      NewI->setName(OldI->getName());
      VMap[&*OldI] = &*NewI;
    }
    llvm::SmallVector<llvm::ReturnInst *, 4> Returns;
    llvm::CloneFunctionInto(NewFn, OldFn, VMap,
                            llvm::CloneFunctionChangeType::LocalChangesOnly,
                            Returns);
    for (auto *RI : Returns) {
      llvm::IRBuilder<> B(RI);
      llvm::Value *Wide = B.CreateZExt(RI->getReturnValue(), I32);
      B.CreateRet(Wide);
      RI->eraseFromParent();
    }

    OldFn->replaceAllUsesWith(NewFn);
    NewFn->takeName(OldFn);
    OldFn->eraseFromParent();
  }
}

static void ensureTargets() {
  // LLVM's target registries are process-global and their registration is not
  // thread-safe.  Every current caller (the patch/rewrite backends and the C
  // API) is serial, so a plain flag would do today; a function-local static
  // costs the same and keeps the guarantee if codegen is ever sharded the way
  // the LLVM emission phase already is.
  [[maybe_unused]] static const bool Initialized = [] {
    LLVMInitializeAArch64TargetInfo();
    LLVMInitializeAArch64Target();
    LLVMInitializeAArch64TargetMC();
    LLVMInitializeAArch64AsmPrinter();
    LLVMInitializeAArch64AsmParser();
    LLVMInitializeARMTargetInfo();
    LLVMInitializeARMTarget();
    LLVMInitializeARMTargetMC();
    LLVMInitializeARMAsmPrinter();
    LLVMInitializeARMAsmParser();
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmPrinter();
    LLVMInitializeX86AsmParser();
    return true;
  }();
}

CodegenResult Codegen::compile(llvm::Module &Mod, Arch TargetArch,
                               BinaryFormat ObjectFormat) {
  CodegenResult Result;
  ensureTargets();

  if (!archCodegenSupported(TargetArch)) {
    llvm::WithColor::error()
        << "codegen: unsupported arch " << getArchName(TargetArch) << "\n";
    return Result;
  }

  const char *TripleStr = llvmCodegenTriple(TargetArch, ObjectFormat);
  if (!TripleStr) {
    llvm::WithColor::error()
        << "codegen: no triple for arch " << getArchName(TargetArch) << "\n";
    return Result;
  }

  llvm::Triple TT(TripleStr);
  Mod.setTargetTriple(TT);

  std::string Err;
  auto *TheTarget = llvm::TargetRegistry::lookupTarget(TT, Err);
  if (!TheTarget) {
    llvm::WithColor::error()
        << "codegen: target lookup failed: " << Err << "\n";
    return Result;
  }

  llvm::TargetOptions Opt;
  auto RM = std::optional<llvm::Reloc::Model>(llvm::Reloc::PIC_);
  auto [CPU, Features] = detectTargetFeatures(Mod, TargetArch);
  if (!Features.empty())
    LLVM_DEBUG(llvm::dbgs() << "codegen: " << getArchName(TargetArch)
                            << " features: " << Features << "\n");
  // createTargetMachine() returns a raw, owning pointer; wrap it in a
  // unique_ptr so the TargetMachine (and its cached per-function Subtargets:
  // AArch64Subtarget/LegalizerInfo, X86Subtarget, etc.) is freed on every
  // return path.  Leaking it here leaks ~1MB per compile and caused the
  // semantic-test process to grow unbounded (cumulative crash on full runs).
  std::unique_ptr<llvm::TargetMachine> TM(TheTarget->createTargetMachine(
      TT, CPU, Features, Opt, RM, std::nullopt, llvm::CodeGenOptLevel::Less));
  if (!TM) {
    llvm::WithColor::error() << "codegen: cannot create TargetMachine\n";
    return Result;
  }

  Mod.setDataLayout(TM->createDataLayout());

  expandNarrowFunnelShifts(Mod);

  llvm::SmallVector<char, 4096> ObjBuf;
  llvm::raw_svector_ostream ObjStream(ObjBuf);

  llvm::legacy::PassManager PM;
  if (TM->addPassesToEmitFile(PM, ObjStream, nullptr,
                              llvm::CodeGenFileType::ObjectFile)) {
    llvm::WithColor::error() << "codegen: cannot emit object file\n";
    return Result;
  }

  PM.run(Mod);

  Result.ObjectData.assign(ObjBuf.begin(), ObjBuf.end());
  LLVM_DEBUG(llvm::dbgs() << "codegen: compiled " << Result.ObjectData.size()
                          << " bytes of object code" << "\n");

  auto Buf = llvm::MemoryBuffer::getMemBuffer(
      llvm::StringRef(reinterpret_cast<const char *>(Result.ObjectData.data()),
                      Result.ObjectData.size()),
      "", false);

  auto ObjOrErr =
      llvm::object::ObjectFile::createObjectFile(Buf->getMemBufferRef());
  if (!ObjOrErr) {
    llvm::consumeError(ObjOrErr.takeError());
    llvm::WithColor::warning()
        << "codegen: cannot parse emitted object for metadata\n";
    Result.Success = true;
    return Result;
  }
  auto &Obj = **ObjOrErr;

  for (auto &Sym : Obj.symbols()) {
    auto NameOrErr = Sym.getName();
    auto AddrOrErr = Sym.getAddress();
    if (!NameOrErr || !AddrOrErr)
      continue;
    auto FlagsOrErr = Sym.getFlags();
    if (!FlagsOrErr) {
      llvm::consumeError(FlagsOrErr.takeError());
      continue;
    }
    auto Flags = *FlagsOrErr;
    if (Flags & llvm::object::SymbolRef::SF_Undefined)
      continue;

    CodegenResult::FuncEntry FE;
    FE.Name = NameOrErr->str();
    FE.Offset = *AddrOrErr;
    llvm::StringRef FENameRef(FE.Name);
    if (FENameRef.starts_with(kAutoFuncPrefix)) {
      uint64_t ParsedVA = 0;
      if (!FENameRef.drop_front(kAutoFuncPrefix.size())
               .getAsInteger(16, ParsedVA))
        FE.OriginalVA = ParsedVA;
    }
    auto SecOrErr = Sym.getSection();
    if (SecOrErr && *SecOrErr != Obj.section_end()) {
      auto Sec = **SecOrErr;
      auto SecNameStr = Sec.getName();
      if (SecNameStr && section_names::isTextSectionName(*SecNameStr)) {
        FE.Size = 0;
        Result.Functions.push_back(FE);
      }
    }
  }

  for (auto &Sec : Obj.sections()) {
    auto SecName = Sec.getName();
    std::string SName = SecName ? SecName->str() : "";
    uint64_t SecAddr = Sec.getAddress();
    for (auto &Reloc : Sec.relocations()) {
      CodegenResult::RelocEntry RE;
      RE.Offset = Reloc.getOffset();
      RE.Type = Reloc.getType();
      RE.SectionName = SName;
      RE.SectionAddr = SecAddr;
      auto SymOrErr = Reloc.getSymbol();
      if (SymOrErr != Obj.symbol_end()) {
        auto Nm = SymOrErr->getName();
        if (Nm)
          RE.Symbol = Nm->str();
      }
      Result.Relocations.push_back(RE);
    }
  }

  LLVM_DEBUG(llvm::dbgs() << "codegen: " << Result.Functions.size()
                          << " functions, " << Result.Relocations.size()
                          << " relocations in emitted object" << "\n");

  Result.Success = true;
  return Result;
}

llvm::mc_rewrite::RewriteResult
Codegen::compileForRewrite(llvm::Module &Mod, Arch TargetArch,
                           const llvm::mc_rewrite::RewriteOptions &Opts,
                           BinaryFormat ObjectFormat) {
  llvm::mc_rewrite::RewriteResult Result;
  ensureTargets();

  if (!archCodegenSupported(TargetArch)) {
    llvm::WithColor::error()
        << "codegen: unsupported arch " << getArchName(TargetArch) << "\n";
    return Result;
  }

  const char *TripleStr = llvmCodegenTriple(TargetArch, ObjectFormat);
  if (!TripleStr) {
    llvm::WithColor::error()
        << "codegen: no triple for arch " << getArchName(TargetArch) << "\n";
    return Result;
  }

  llvm::Triple TT(TripleStr);
  Mod.setTargetTriple(TT);

  std::string Err;
  auto *TheTarget = llvm::TargetRegistry::lookupTarget(TT, Err);
  if (!TheTarget) {
    llvm::WithColor::error()
        << "codegen: target lookup failed: " << Err << "\n";
    return Result;
  }

  llvm::TargetOptions TOpt;
  // Darwin AArch64 normally omits DWARF CFI when a frame has a compact-unwind
  // encoding.  The rewrite path cannot register compact-unwind records from
  // its appended executable segment, while an existing __TEXT,__eh_frame can
  // safely host regenerated DWARF records.  Keep those records in every
  // Mach-O rewrite object so the format installer can validate and register
  // them, or fail closed when the input exposes no such region.
  if (ObjectFormat == BinaryFormat::MachO)
    TOpt.MCOptions.EmitDwarfUnwind = llvm::EmitDwarfUnwindType::Always;
  // The rewrite backend outputs final image bytes; alignment padding in text
  // sections must be NOPs, not zeros. Rather than post-processing alignment
  // fragments (which may or may not be FT_Align), disable loop/block alignment
  // entirely. This produces slightly less cache-friendly code but avoids
  // zero-filled alignment gaps that crash on x86 (where 0x00 = `add [rax],
  // al`).
  TOpt.LoopAlignment = 1;
  // i386 PIC requires GOT indirection (__x86.get_pc_thunk +
  // _GLOBAL_OFFSET_TABLE_) which the rewrite backend cannot provide. Since it
  // knows all final VAs, Static works and generates simpler absolute-address
  // code. Other ISAs use PIC: x86-64 gets RIP-relative, AArch64 gets ADRP+ADD,
  // ARM32 gets PC-relative LDR — all handled by AddressModelBackend.
  auto RM = std::optional<llvm::Reloc::Model>(
      TargetArch == Arch::X86 ? llvm::Reloc::Static : llvm::Reloc::PIC_);
  auto [CPU, Features] = detectTargetFeatures(Mod, TargetArch);
  std::unique_ptr<llvm::TargetMachine> TM(TheTarget->createTargetMachine(
      TT, CPU, Features, TOpt, RM, std::nullopt, llvm::CodeGenOptLevel::Less));
  if (!TM) {
    llvm::WithColor::error() << "codegen: cannot create TargetMachine\n";
    return Result;
  }

  Mod.setDataLayout(TM->createDataLayout());
  expandNarrowFunnelShifts(Mod);

  // Pre-pass: convert __nd_data_* defined globals to external declarations.
  // The lifter embeds data sections as large defined globals; the rewrite
  // backend should reference the original VA (via the resolve callback)
  // instead of emitting a copy of the data.
  for (auto &GV : llvm::make_early_inc_range(Mod.globals())) {
    if (!GV.hasInitializer())
      continue;
    if (!GV.getName().starts_with(kNdDataPrefix))
      continue;
    GV.setInitializer(nullptr);
    GV.setLinkage(llvm::GlobalValue::ExternalLinkage);
  }

  // Pre-pass: mark every function — declarations AND defined-in-module callees
  // — as dso_local. Without dso_local, x86/x86-64 PIC codegen assumes the
  // symbol may be interposed and routes calls through the GOT/PLT (callq
  // *fn@GOTPCREL(%rip) for declarations, callq fn@PLT for a preemptible defined
  // callee), but the rewrite backend creates neither a GOT nor a PLT. Since it
  // knows every final VA, dso_local is correct for all of them — it emits a
  // direct call rel32 the address model resolves. Definitions must be covered
  // too: a module whose *defined* callees are not dso_local (e.g. mutually
  // recursive ExternalLinkage functions, or any cross-function call in
  // host-clang IR fed via --from-ir/--from-c, which is PIC and non-dso_local)
  // would otherwise emit a PLT call the address model cannot resolve. The
  // lifter already marks its own defined functions dso_local (MedLLVMEmitter::
  // declareFunc), so this only changes external IR sources; AArch64/ARM32
  // already use direct BL, where dso_local is harmless.
  for (auto &F : Mod) {
    if (!F.isDSOLocal())
      F.setDSOLocal(true);
  }

  // Pre-pass: suppress every form of compiler-inserted stack probe. The probe
  // routine (Darwin __chkstk_darwin, Windows __chkstk) is NOT part of the
  // rewrite address model — the resolve callback cannot map it — so any probe
  // call/branch the backend emits resolves to nothing and faults at run time
  // (COFF i386 jumps to a wild VA; COFF ARM32/Thumb even errors out of the MC
  // layer with "cannot perform a PC-relative fixup with a non-zero symbol
  // offset" because the unresolved __chkstk branch can't be a COFF relocation).
  // Two independent triggers, both neutralized here:
  //   (1) The host-clang "probe-stack" attribute (Apple clang puts
  //       "probe-stack"="__chkstk_darwin" on Darwin arm64; the AArch64 backend
  //       only implements the "inline-asm" kind and report_fatal_error()s on
  //       any other value). Strip it.
  //   (2) The Windows/COFF *frame* probe: X86 (getStackProbeSize) and ARM
  //       (WindowsRequiresStackProbe) call __chkstk whenever the frame is
  //       >= "stack-probe-size" (default 4096). A large injected/obfuscated
  //       frame (e.g. control-flow flattening's reg2mem spills) crosses 4 KiB
  //       and pulls in __chkstk. Pin "stack-probe-size" beyond any real frame
  //       to disable it (the rewritten code runs at a fixed VA with a flat
  //       stack — there is no guard page to probe). "no-stack-arg-probe" does
  //       the same for the dynamic-alloca (WIN_ALLOCA) probe.
  // Lifted IR never carries probe-stack; external --from-ir / --from-c IR and
  // any COFF target with a >4 KiB frame do. Harmless no-op on ELF/Mach-O, which
  // have no Windows-style frame probe.
  for (auto &F : Mod) {
    if (F.hasFnAttribute("probe-stack"))
      F.removeFnAttr("probe-stack");
    if (F.hasFnAttribute("stack-probe-size"))
      F.removeFnAttr("stack-probe-size");
    F.addFnAttr("stack-probe-size", "4294967295");
    if (!F.hasFnAttribute("no-stack-arg-probe"))
      F.addFnAttr("no-stack-arg-probe");
  }

  // Robustness: the lifter can emit malformed bodies for auto-detected
  // functions it failed to lift (e.g. an empty entry block with no
  // terminator). Such IR crashes the whole-module codegen pipeline
  // (DominatorTree, etc.). Demote any function that fails IR verification to a
  // declaration: if it is referenced it resolves to the original VA via the
  // address model; if not, it is simply dropped. InplaceRewriter dodges this
  // by cloning + deleteBody per function, but section mode compiles the whole
  // module at once, so it must sanitize here.
  for (auto &F : Mod) {
    if (F.isDeclaration())
      continue;
    if (llvm::verifyFunction(F))
      F.deleteBody();
  }

  llvm::legacy::PassManager PM;
  if (TM->addPassesToEmitBinaryRewrite(PM, Opts, Result)) {
    llvm::WithColor::error()
        << "codegen: addPassesToEmitBinaryRewrite failed\n";
    return Result;
  }

  PM.run(Mod);

  LLVM_DEBUG({
    llvm::dbgs() << "codegen: " << Result.Sections.size() << " sections";
    uint64_t Total = 0;
    for (auto &S : Result.Sections)
      Total += S.Bytes.size();
    llvm::dbgs() << ", " << Total << " bytes, " << Result.SymbolAddrs.size()
                 << " symbols, " << Result.Unresolved.size() << " unresolved\n";
  });

  return Result;
}

} // namespace neverd
