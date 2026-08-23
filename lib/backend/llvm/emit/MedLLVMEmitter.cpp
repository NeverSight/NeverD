//===- MedLLVMEmitter.cpp - MedIR to LLVM IR emitter core ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Core MedIR to LLVM IR emission: type conversion, the memory-pointer
/// primitive, function declaration and module emission.  The per-function
/// body emission (blocks, terminators, the op dispatch loop, phi
/// placement and EH wiring) lives in MedLLVMFuncBody.cpp.  The rest of the
/// emitter is split by concern: operation dispatch in MedLLVMOpEmitter.cpp,
/// CALL/RETURN in MedLLVMCall.cpp and MedLLVMReturn.cpp, INTRINSIC in
/// MedLLVMIntrinsic.cpp, floating-point ops in MedLLVMFloatEmitter.cpp,
/// jump-table switch lowering in MedLLVMSwitch.cpp and
/// MedLLVMSwitchIndex.cpp.  Address tracing, variable access and frame
/// classification live under resolve/; global-data embedding and constant
/// classification under data/; exception metadata and native EH lowering
/// under eh/.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"

#include "neverd/Common.h"
#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/LLVMName.h"
#include "neverd/ir/TargetRegInfo.h"

#define DEBUG_TYPE "neverd-med-llvm-emitter"
#include "neverd/ArchSupport.h"
#include "neverd/Limits.h"
#include "neverd/support/Diagnostic.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

namespace {

bool hasObjectFunctionNameAt(const BinaryImage *Img, BinaryFormat Format,
                             va_t Address, llvm::StringRef Name) {
  if (!Img || Format != BinaryFormat::MachO)
    return false;
  for (const Symbol &Sym : Img->Symbols)
    if (Sym.Addr == Address && Sym.Name == Name)
      return true;
  for (const Export &Exp : Img->Exports)
    if (Exp.Addr == Address && Exp.Name == Name)
      return true;
  return false;
}

} // namespace

//===----------------------------------------------------------------------===//
// Type conversion helpers
//===----------------------------------------------------------------------===//

llvm::Type *MedLLVMEmitter::sizeToType(uint16_t Size) {
  if (Size == 0)
    return llvm::Type::getInt64Ty(*Ctx);
  unsigned Bits = static_cast<unsigned>(Size) * 8;
  if (Bits == 0)
    return llvm::Type::getInt64Ty(*Ctx);
  return llvm::IntegerType::get(*Ctx, Bits);
}

llvm::Type *MedLLVMEmitter::mapNdtype(const TypeRef &Ty) {
  if (!Ty)
    return llvm::Type::getInt64Ty(*Ctx);
  switch (Ty->Kind) {
  case NdTypeKind::Void:
    return llvm::Type::getVoidTy(*Ctx);
  case NdTypeKind::Float:
    return Ty->Size == 2    ? llvm::Type::getHalfTy(*Ctx)
           : Ty->Size == 4  ? llvm::Type::getFloatTy(*Ctx)
           : Ty->Size == 10 ? llvm::Type::getX86_FP80Ty(*Ctx)
                            : llvm::Type::getDoubleTy(*Ctx);
  case NdTypeKind::Ptr:
    return llvm::PointerType::get(*Ctx, 0);
  case NdTypeKind::Int:
  case NdTypeKind::Unknown:
  default:
    return Ty->Size > 0 ? llvm::IntegerType::get(*Ctx, Ty->Size * 8)
                        : llvm::Type::getInt64Ty(*Ctx);
  }
}

bool MedLLVMEmitter::isFrameRelativeDisplacement(uint64_t Addr,
                                                 unsigned BitWidth) const {
  if (!FrameBaseInt)
    return false;
  // Sign-interpret the low BitWidth bits of Addr.  implicitTrunc keeps a value
  // whose top bit is set (a genuine negative displacement, or an address that
  // does not fit the signed range of BitWidth) from tripping APInt's N-bit
  // signed-range assertion; getSExtValue then yields the intended signed disp.
  int64_t Disp = llvm::APInt(BitWidth, Addr, /*isSigned=*/false,
                             /*implicitTrunc=*/true)
                     .getSExtValue();
  if (Disp >= 0)
    return false;
  // When FrameSize is known, bound-check the displacement.  When it is not
  // (e.g. stack analysis missed a slot) but we still have a synthetic frame
  // alloca, accept any reasonable negative offset — bare displacements like
  // -4 must not become absolute inttoptr addresses.
  uint64_t Abs = static_cast<uint64_t>(-(Disp + 1)) + 1;
  if (!CurMedFunc || CurMedFunc->FrameSize == 0)
    return Abs <= 65536;
  return Abs <= static_cast<uint64_t>(CurMedFunc->FrameSize) + 128;
}

llvm::Value *MedLLVMEmitter::tryFrameRelativePtr(llvm::Value *Addr,
                                                 llvm::IRBuilder<> &Builder) {
  auto *CI = llvm::dyn_cast<llvm::ConstantInt>(Addr);
  if (!CI || !FrameBaseInt)
    return nullptr;
  uint64_t VA = CI->getZExtValue();
  int64_t Disp = llvm::APInt(CI->getBitWidth(), VA, /*isSigned=*/false,
                             /*implicitTrunc=*/true)
                     .getSExtValue();
  // In a lifted function with a synthetic alloca frame, a bare negative
  // constant address is always a stack displacement (never an absolute VA).
  if (Disp >= 0)
    return nullptr;
  if (!isFrameRelativeDisplacement(VA, CI->getBitWidth()))
    return nullptr;
  auto *FrameTy = FrameBaseInt->getType();
  llvm::Value *Off =
      llvm::ConstantInt::get(FrameTy, static_cast<uint64_t>(Disp), true);
  llvm::Value *PtrInt = Builder.CreateAdd(FrameBaseInt, Off);
  if (Addr->getType() != PtrInt->getType())
    PtrInt = Builder.CreateIntCast(PtrInt, Addr->getType(), true, "sp_cast");
  return Builder.CreateIntToPtr(PtrInt, llvm::PointerType::get(*Ctx, 0),
                                "stackptr");
}

llvm::Value *MedLLVMEmitter::getMemoryPtr(llvm::Value *Addr,
                                          llvm::Type *ValType,
                                          llvm::IRBuilder<> &Builder) {
  if (auto *StackPtr = tryFrameRelativePtr(Addr, Builder))
    return StackPtr;

  if (Img) {
    uint64_t VA = 0;
    bool HaveVA = false;

    if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(Addr)) {
      VA = CI->getZExtValue();
      HaveVA = true;
    }

    // Trace through PtrToInt(GlobalVariable) — produced by getVar for
    // addresses that were already resolved as global data references.
    if (!HaveVA) {
      if (auto *PTI = llvm::dyn_cast<llvm::PtrToIntOperator>(Addr)) {
        if (auto *GV =
                llvm::dyn_cast<llvm::GlobalVariable>(PTI->getPointerOperand()))
          return GV;
        if (auto *CE = llvm::dyn_cast<llvm::Constant>(PTI->getPointerOperand()))
          return const_cast<llvm::Constant *>(CE);
      }
    }

    if (HaveVA && VA != 0) {
      uint16_t Hint = 0;
      if (ValType && ValType->isSized()) {
        uint64_t Bits = Mod->getDataLayout().getTypeSizeInBits(ValType);
        if (Bits > 0 && Bits <= 64 && (Bits % 8) == 0)
          Hint = static_cast<uint16_t>(Bits / 8);
      }
      if (auto *G = tryResolveGlobalData(VA, Hint))
        return G;
    }
  }
  return Builder.CreateIntToPtr(Addr, llvm::PointerType::get(*Ctx, 0),
                                "memptr");
}

//===----------------------------------------------------------------------===//
// Function emission
//===----------------------------------------------------------------------===//

llvm::Function *MedLLVMEmitter::declareFunc(const MedFunc &Func) {
  llvm::StringRef EmittedName = Func.Name;
  if (auto It = EmittedFuncNames.find(Func.Entry); It != EmittedFuncNames.end())
    EmittedName = It->second;
  if (auto *Existing = Mod->getFunction(EmittedName))
    return Existing;

  const auto &TRI = getTargetRegInfo(TargetArch);
  auto *Vec2I64 = llvm::FixedVectorType::get(llvm::Type::getInt64Ty(*Ctx), 2);

  // The ABI type for a value carried in a floating-point/vector register: a
  // 16-byte register (x86 XMM, AArch64 V) takes the <2 x i64> vector so the
  // body's 16-byte register model flows through and LLVM's SSE/AAPCS lowering
  // assigns it to the FP register file; an 8/4-byte register (ARM D/S) takes a
  // scalar double/float so the AAPCS-VFP lowering assigns it to D/S.
  auto fpAbiType = [&](uint16_t ElemSize) -> llvm::Type * {
    if (TRI.VecRegStride >= 16)
      return Vec2I64;
    return ElemSize <= 4 ? llvm::Type::getFloatTy(*Ctx)
                         : llvm::Type::getDoubleTy(*Ctx);
  };

  llvm::Type *RetType;
  if (Func.hasTypeInfo())
    RetType = mapNdtype(Func.ReturnType);
  else
    RetType = llvm::Type::getInt64Ty(*Ctx);

  // A scalar floating-point return carried in a vector register (x86-64 XMM0,
  // AArch64 V0, ARM D0 for the internal VFP convention) is declared with the
  // register's ABI type so LLVM's calling-convention lowering places it there;
  // the caller reads the low lane.  Architectures that model the FP return in
  // the integer return register (ARM softfp external returns) keep the scalar
  // type.
  if (Func.hasTypeInfo() && Func.ReturnType &&
      Func.ReturnType->Kind == NdTypeKind::Float &&
      TRI.isVectorReg(TRI.fpReturnModelReg())) {
    // The i386 cdecl x87 (st0) return is a scalar FP type so LLVM lowers it to
    // the x87 stack; the XMM0-vector convention keeps the 16-byte vector type.
    RetType = Func.FPReturnViaX87
                  ? (Func.ReturnType->Size <= 4 ? llvm::Type::getFloatTy(*Ctx)
                                                : llvm::Type::getDoubleTy(*Ctx))
                  : fpAbiType(Func.ReturnType->Size);
  }

  // AArch64: a narrow integer return is materialized by a sub-register write
  // (`mov w0,wN`) which the ISA zero-extends into the full X0, so the original
  // defines all 64 bits.  A narrow `ret i32` lets the backend fold the value
  // into a 64-bit op (`add x0,...,lsr #32`) and leave the high half dirty —
  // legal under the i32 ABI but diverging from the original's zero-extended X0.
  // Widen to the GPR width so the return is explicitly zero-extended.  (x86-64
  // needs no such fix: its 32-bit writes auto-zero the upper EAX→RAX bits.)
  if (TargetArch == Arch::AArch64 && RetType->isIntegerTy() &&
      RetType->getIntegerBitWidth() < 64)
    RetType = llvm::Type::getInt64Ty(*Ctx);

  auto ParamIRType = [&](uint16_t Size) -> llvm::Type * {
    if (Size == 16)
      return Vec2I64;
    if (Size == 32)
      return llvm::FixedVectorType::get(llvm::Type::getInt64Ty(*Ctx), 4);
    return sizeToType(Size);
  };
  bool HaveTypes =
      Func.hasTypeInfo() && Func.TypedParams.size() == Func.Params.size();
  std::vector<llvm::Type *> ParamTypes;
  for (size_t I = 0; I < Func.Params.size(); ++I) {
    const auto &P = Func.Params[I];
    // A floating-point / vector argument register (XMM0-7, V0-7, ARM D0-7)
    // takes its register ABI type so LLVM's calling-convention lowering assigns
    // it to the FP register file; declaring it an integer would route it
    // through the integer registers instead.
    if (P.RegOff != kNoParamReg && TRI.isFPArgReg(P.RegOff)) {
      ParamTypes.push_back(fpAbiType(P.Size));
      continue;
    }
    if (HaveTypes)
      ParamTypes.push_back(mapNdtype(Func.TypedParams[I].Type));
    else
      ParamTypes.push_back(ParamIRType(P.Size));
  }

  // A small struct returned by value across multiple registers: build the LLVM
  // aggregate from the recovered field registers so the backend's calling-
  // convention lowering places each field in its return register (x86-64 SysV
  // eightbytes -> RAX/RDX/XMM0/XMM1; AArch64 non-HFA -> X0/X1, HFA -> V0..V3).
  if (!Func.MultiReturn.empty()) {
    std::vector<llvm::Type *> FieldTypes;
    for (const auto &RR : Func.MultiReturn) {
      uint16_t Sz = RR.Size ? RR.Size : 8;
      if (RR.IsFP)
        FieldTypes.push_back(Sz <= 4 ? llvm::Type::getFloatTy(*Ctx)
                                     : llvm::Type::getDoubleTy(*Ctx));
      else
        FieldTypes.push_back(llvm::Type::getIntNTy(*Ctx, Sz * 8));
    }
    RetType = llvm::StructType::get(*Ctx, FieldTypes);
  }

  auto *FuncTy = llvm::FunctionType::get(RetType, ParamTypes, Func.IsVariadic);
  auto *LLVMFunc = llvm::Function::Create(
      FuncTy, llvm::GlobalValue::ExternalLinkage, EmittedName, Mod);
  // A lifted function is defined in this same module, so its address is fixed
  // at link time — mark it dso_local so a `ptrtoint @func` reference (a
  // function pointer) under the PIC relocation model is materialized by direct
  // page-relative addressing (`lea rip` / `adrp+add`) instead of a GOT load.
  // The recompiled object carries no GOT, so a GOT-indirected function pointer
  // would read unmapped memory.  Mirrors the dso_local data globals.  External
  // call targets keep default visibility (declared elsewhere, may be imports).
  LLVMFunc->setDSOLocal(true);
  LLVMFunc->addFnAttr(llvm::Attribute::NullPointerIsValid);
  if (Func.DoesNotReturn)
    LLVMFunc->addFnAttr(llvm::Attribute::NoReturn);
  rewrite_source::setOriginalVA(*LLVMFunc, Func.Entry);
  return LLVMFunc;
}

//===----------------------------------------------------------------------===//
// Module emission
//===----------------------------------------------------------------------===//

std::unique_ptr<llvm::Module>
MedLLVMEmitter::emit(const std::vector<MedFunc> &Funcs, llvm::LLVMContext &LCtx,
                     const std::string &ModName, Arch TheArch,
                     const std::vector<std::pair<va_t, std::string>> &Imports,
                     const BinaryImage *Img_, BinaryFormat Fmt,
                     bool MergeableGlobals_,
                     const std::vector<char> *BodyMask) {

  Ctx = &LCtx;
  auto Mod_ = std::make_unique<llvm::Module>(ModName, LCtx);
  Mod = Mod_.get();
  Img = Img_;
  TargetArch = TheArch;
  TargetFormat = Fmt;
  MergeableGlobals = MergeableGlobals_;
  UnhandledValueIntrinsicCount = 0;
  AddressProvenanceWork = {};
  FrameReloadSourceWork = {};
  GlobalDataCache.clear();
  ModuleJumpTableStorageRanges.clear();
  ModuleSuppressibleJumpTableRelocationSlots.clear();
  for (const MedFunc &Func : Funcs)
    for (const JumpTable &JT : Func.JumpTables) {
      ModuleJumpTableStorageRanges.insert(ModuleJumpTableStorageRanges.end(),
                                          JT.StorageRanges.begin(),
                                          JT.StorageRanges.end());
    }
  IdentityPreservingDataAddrs.clear();
  for (const MedFunc &Func : Funcs)
    for (const MedBlock &Block : Func.Blocks) {
      for (const MedOp &Op : Block.Ops)
        for (uint8_t I = 0; I < Op.NumInputs; ++I)
          if (Op.Inputs[I].isConst() &&
              (Op.Inputs[I].Provenance ==
                   ConstantAddressProvenance::DataAddress ||
               (Op.Inputs[I].Provenance == ConstantAddressProvenance::Address &&
                (!Img || !Img->isCodeAddress(Op.Inputs[I].ConstVal)))))
            IdentityPreservingDataAddrs.insert(Op.Inputs[I].ConstVal);
      for (const PhiNode &Phi : Block.Phis)
        for (const auto &[Pred, Arg] : Phi.Args) {
          (void)Pred;
          if (Arg.isConst() &&
              (Arg.Provenance == ConstantAddressProvenance::DataAddress ||
               (Arg.Provenance == ConstantAddressProvenance::Address &&
                (!Img || !Img->isCodeAddress(Arg.ConstVal)))))
            IdentityPreservingDataAddrs.insert(Arg.ConstVal);
        }
    }
  SegmentDataGlobals.clear();
  WritableSegmentGlobals.clear();
  CodePtrTableGlobals.clear();
  PreparedFuncBlocks.clear();
  LiftedCodeBlocks.clear();

  // Every lazy analysis cache below is keyed by the current MedFunc pointer.
  // A caller may reuse one emitter and place a new function vector at the same
  // address as the previous vector, so pointer equality alone is not a valid
  // cross-emit generation key.  Drop every per-function cache at the module
  // boundary before CurMedFunc can be assigned again.
  CurMedFunc = nullptr;
  StoredBasesFor = nullptr;
  StoredConstBases.clear();
  RodataSymbolsFor = nullptr;
  RodataSymbolVAs.clear();
  InductionBasesFor = nullptr;
  InductionBaseVAs.clear();
  IndexedGlobalBaseCacheFor = nullptr;
  IndexedGlobalBaseCache.clear();
  PhiRecurrenceCacheFor = nullptr;
  PhiRecurrenceCache.clear();
  SelfRecurrenceCacheFor = nullptr;
  SelfRecurrenceCache.clear();
  StableOffsetCacheFor = nullptr;
  StableOffsetCache.clear();
  AddressFragmentTaintFor = nullptr;
  AddressFragmentTaint.clear();
  SymbolizedWritableSegsFor = nullptr;
  SymbolizedWritableSegs.clear();
  DefPhiIndexFor = nullptr;
  DefIndex.clear();
  PhiIndex.clear();
  CodeIdentityOccurrenceCacheFor = nullptr;
  CodeIdentityOccurrenceCache.clear();
  FeasibleEdgesFor = nullptr;
  FeasibleEdgeState = FeasibleEdgeCacheState::Empty;
  FeasibleEdgeBuildSawReentrantQuery = false;
  FeasibleEdgeBuildTestHook = {};
  FeasibleEdges.clear();
  FeasibleBlocks.clear();
  PhiEdgeIndexFor = nullptr;
  PhiOwnerBlocks.clear();
  StructuralEdges.clear();
  PhiEdgeClassCache.clear();
  FrameDerivedCacheFor = nullptr;
  FrameDerivedCache.clear();
  FrameAddressCacheFor = nullptr;
  FrameAddressCache.clear();
  invalidateFrameReloadSourceCache();
  AddrPredCacheFor = nullptr;
  SlotAddressEscapesCache.clear();
  SlotMatchingKeyLoadCache.clear();
  SlotReloadUsedLocallyCache.clear();
  WritableDataSegCache.clear();
  PtrTableUniqueSegCache.clear();
  ConstClassCacheFor = nullptr;
  ConstUsedAsPointerCache.clear();
  ConstValueUsedAsIntegerCache.clear();
  FatalCodePointerResolution = false;
  FatalDataPointerResolution = false;
  ImportedSymbolPlaceholders.clear();
  StringDataAddrs.clear();
  GlobalStrCounter = 0;

  // Suppression preflight invokes the same address/provenance resolvers as
  // body emission. Establish the complete function/import key domain before
  // that first query so a cached proof cannot observe names left by a prior
  // emit or a smaller domain than the later body phase. Exact emitted names
  // are rebuilt below once native-personality conflicts have been resolved.
  EmittedFuncNames.clear();
  FuncNames.clear();
  for (const MedFunc &Func : Funcs)
    FuncNames.try_emplace(Func.Entry, Func.Name);
  for (const auto &[Addr, Name] : Imports)
    FuncNames.try_emplace(Addr, Name);

  // LowIR ownership establishes which relocation slots a recovered table may
  // suppress, but the backend can actually omit those mirror bytes only when
  // every authenticated target LOAD is still present after Med rewriting and
  // its complete SSA use closure terminates exclusively at that table's
  // INDIR_BR.  A side STORE/CALL/RETURN (or a missing/ambiguous occurrence)
  // needs the real pointer bytes.  Treat that as a veto for the whole table;
  // for shared storage, one veto dominates another dispatch's request.
  std::set<va_t> RequestedSuppression;
  std::set<va_t> VetoedSuppression;
  for (const MedFunc &Func : Funcs) {
    using LoadOccurrenceKey = std::tuple<va_t, int, uint16_t>;
    std::map<LoadOccurrenceKey, std::vector<const MedOp *>> LoadsByOccurrence;
    for (const MedBlock &Block : Func.Blocks)
      for (const MedOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::LOAD && Op.OriginSeq >= 0 && Op.Output.Size != 0)
          LoadsByOccurrence[{Op.Addr, Op.OriginSeq, Op.Output.Size}].push_back(
              &Op);

    CurMedFunc = &Func;
    for (const JumpTable &JT : Func.JumpTables) {
      bool TerminalExclusive = !JT.AuthenticatedTableLoads.empty();
      for (const JumpTableOpOccurrence &Occurrence :
           JT.AuthenticatedTableLoads) {
        auto It = LoadsByOccurrence.find(
            {Occurrence.Addr, Occurrence.Seq, Occurrence.Size});
        if (It == LoadsByOccurrence.end() || It->second.size() != 1 ||
            recoveredJumpTableForLoad(*It->second.front()) != &JT) {
          TerminalExclusive = false;
          break;
        }
      }
      std::set<va_t> &Destination =
          TerminalExclusive ? RequestedSuppression : VetoedSuppression;
      Destination.insert(JT.SuppressibleRelocationSlots.begin(),
                         JT.SuppressibleRelocationSlots.end());
    }
  }
  CurMedFunc = nullptr;
  for (va_t Slot : RequestedSuppression)
    if (!VetoedSuppression.count(Slot))
      ModuleSuppressibleJumpTableRelocationSlots.insert(Slot);

  const char *Triple = llvmEmitTriple(TheArch, Fmt);
  if (Triple)
    Mod_->setTargetTriple(llvm::Triple(Triple));
  if (Fmt == BinaryFormat::COFF && Img) {
    uint32_t GuardFlags = Img->DynInfo.GuardFlags;
    if ((GuardFlags & uint32_t(llvm::COFF::GuardFlags::CF_INSTRUMENTED)) != 0)
      Mod_->addModuleFlag(llvm::Module::Warning, "cfguard", 2);
    else if ((GuardFlags &
              uint32_t(llvm::COFF::GuardFlags::CF_FUNCTION_TABLE_PRESENT)) != 0)
      Mod_->addModuleFlag(llvm::Module::Warning, "cfguard", 1);
    if ((GuardFlags &
         uint32_t(llvm::COFF::GuardFlags::EH_CONTINUATION_TABLE_PRESENT)) != 0)
      Mod_->addModuleFlag(llvm::Module::Warning, "ehcontguard", 1);
  }

  // A PE may carry its exception personality as executable code in the image,
  // so discovery legitimately lifts that address as an ordinary function.
  // Native WinEH, however, requires the canonical personality symbol to have
  // LLVM's exact variadic i32 ABI. Keep the address-backed body under its
  // stable auto name so the canonical name remains available for an external
  // ABI declaration. The COFF patcher recognizes the address alias, keeps the
  // original runtime thunk authoritative, and resolves both spellings to it.
  std::map<va_t, std::string> NativePersonalityNames;
  std::set<va_t> ConflictingPersonalityAddresses;
  if (TheArch == Arch::X64 && Fmt == BinaryFormat::COFF) {
    for (const MedFunc &F : Funcs) {
      if (!F.ExceptionMetadata || F.ExceptionMetadata->PersonalityVA == 0)
        continue;
      const ExceptionPersonality Personality = F.ExceptionMetadata->Personality;
      if (Personality != ExceptionPersonality::CSpecificHandler &&
          Personality != ExceptionPersonality::CxxFrameHandler3)
        continue;
      const va_t Address = F.ExceptionMetadata->PersonalityVA;
      const std::string Name = getExceptionPersonalityName(Personality);
      auto [It, Inserted] = NativePersonalityNames.emplace(Address, Name);
      if (!Inserted && It->second != Name)
        ConflictingPersonalityAddresses.insert(Address);
    }
  }

  EmittedFuncNames.clear();
  FuncNames.clear();
  for (const MedFunc &F : Funcs) {
    std::string EmittedName = F.Name;
    auto Personality = NativePersonalityNames.find(F.Entry);
    llvm::StringRef SourceName(F.Name);
    SourceName.consume_front("\01");
    if (Personality != NativePersonalityNames.end() &&
        !ConflictingPersonalityAddresses.count(F.Entry) &&
        SourceName == Personality->second)
      EmittedName = (kAutoFuncPrefix + llvm::utohexstr(F.Entry)).str();
    else if (hasObjectFunctionNameAt(Img, Fmt, F.Entry, F.Name))
      EmittedName = llvm_name::fromObjectSymbol(F.Name, Fmt).str();
    EmittedFuncNames[F.Entry] = EmittedName;
    FuncNames[F.Entry] = std::move(EmittedName);
  }
  for (auto &[Addr, Name] : Imports)
    FuncNames[Addr] = llvm_name::fromObjectSymbol(Name, Fmt).str();

  // Suppression preflight intentionally runs against the complete key domain
  // before exact emitted spellings exist. Once those spellings are installed,
  // begin a fresh frame-proof phase: no reload occurrence/result may flow from
  // preflight into body emission merely because the same MedFunc storage is
  // still current. Other feasible-edge-dependent caches key only on the stable
  // function/import address domain established before preflight, so retaining
  // them avoids repeating unrelated provenance work.
  invalidateFrameReloadSourceCache();

  // Import function declarations are deferred to the CALL handler so
  // they get the correct parameter types from the actual call site.

  // Pre-declare every function before emitting any body so a body can reference
  // a sibling that is emitted later — e.g. a function-pointer dispatch table
  // whose entries name leaf functions the (earlier-emitted) dispatcher
  // resolves.
  for (auto &Func : Funcs) {
    if (Func.Name.empty() || Func.Blocks.empty())
      continue;
    declareFunc(Func);
  }

  // Build every ordinary block skeleton before emitting the first operation.
  // A code-pointer mirror requested by an early consumer can then name an
  // interior label owned by a later function.  BodyMask-omitted functions stay
  // declarations: blockaddress cannot cross shard/module boundaries.
  for (size_t I = 0; I < Funcs.size(); ++I) {
    if (BodyMask && !(*BodyMask)[I])
      continue;
    const MedFunc &Func = Funcs[I];
    if (Func.Name.empty() || Func.Blocks.empty())
      continue;
    llvm::Function *LLVMFunc = declareFunc(Func);
    for (const MedBlock &Block : Func.Blocks) {
      auto *BB = llvm::BasicBlock::Create(
          *Ctx, "bb_" + std::to_string(Block.Id), LLVMFunc);
      PreparedFuncBlocks[{Func.Entry, Block.Id}] = BB;
      const va_t Address = Block.StartAddr != 0 || Block.Ops.empty()
                               ? Block.StartAddr
                               : Block.Ops.front().Addr;
      LiftedCodeBlocks.try_emplace(Address, BB);
    }
  }

  // Emit bodies only for the masked-in functions; functions the mask omits
  // stay declarations (a shard defines its slice, declares the rest so
  // cross-shard references resolve at link time).  A null mask emits all.
  for (size_t I = 0; I < Funcs.size(); ++I) {
    if (BodyMask && !(*BodyMask)[I])
      continue;
    const auto &Func = Funcs[I];
    if (Func.Name.empty())
      continue;
    emitFunc(Func);
  }

  if (FatalCodePointerResolution || FatalDataPointerResolution)
    return nullptr;

  // Mark the producer schema independently of per-function attachments.  A
  // later pass that drops one attachment must be distinguishable from a
  // genuinely external LLVM module that never carried source EH state.
  exception_rewrite::markModule(*Mod_);
  for (llvm::Function &Function : *Mod_)
    if (!Function.isDeclaration() &&
        !Function.getMetadata(exception_rewrite::FunctionAttachment))
      exception_rewrite::setContract(
          Function, exception_rewrite::SourceState::Absent,
          exception_rewrite::LoweringState::NotRequired);

  for (auto &F : *Mod_) {
    if (F.isDeclaration())
      continue;
    for (auto &BB : F) {
      if (BB.empty() || !BB.back().isTerminator()) {
        llvm::IRBuilder<> Fix(&BB);
        if (F.getReturnType()->isVoidTy())
          Fix.CreateRetVoid();
        else
          Fix.CreateRet(llvm::ConstantInt::get(F.getReturnType(), 0));
      }
    }
  }

  std::string VerifyErr;
  llvm::raw_string_ostream VES(VerifyErr);
  if (llvm::verifyModule(*Mod_, &VES))
    syncWarning() << "med_llvm_emitter: verification: " << VerifyErr << "\n";

  LLVM_DEBUG(llvm::dbgs() << "med_llvm_emitter: emitted " << Funcs.size()
                          << " functions\n");
  return Mod_;
}

} // namespace neverd
