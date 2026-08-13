//===- MedLLVMEmitter.cpp - MedIR to LLVM IR emitter core ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Core MedIR to LLVM IR emission: type conversion, the memory-pointer
/// primitive, and function/module emission.  The rest of the emitter is split
/// by concern: operation dispatch in MedLLVMOpEmitter.cpp, CALL/RETURN in
/// MedLLVMCall.cpp and MedLLVMReturn.cpp, INTRINSIC in MedLLVMIntrinsic.cpp,
/// floating-point ops in MedLLVMFloatEmitter.cpp, shared address tracing in
/// MedLLVMAddrResolve.cpp, specialized address resolvers in
/// MedLLVMLiteralTable.cpp, MedLLVMIndexedGlobal.cpp, and
/// MedLLVMCodePtrResolve.cpp, variable access in MedLLVMVarAccess.cpp,
/// global-data resolution in MedLLVMGlobalData.cpp, and jump-table switch
/// lowering in MedLLVMSwitch.cpp.  Exception metadata and native EH lowering
/// live in the MedLLVMExceptionMetadata.cpp and MedLLVMNative*EH.cpp translation
/// units.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"

#include "neverd/Common.h"
#include "neverd/ir/TargetRegInfo.h"

#define DEBUG_TYPE "neverd-med-llvm-emitter"
#include "neverd/ArchSupport.h"
#include "neverd/Limits.h"
#include "neverd/support/Diagnostic.h"

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
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

namespace {

uint64_t checkedSyntheticStackAdd(uint64_t Left, uint64_t Right) {
  if (Left > std::numeric_limits<uint64_t>::max() - Right)
    llvm::report_fatal_error("LLVM synthetic stack size overflow");
  return Left + Right;
}

uint64_t checkedSyntheticStackMul(uint64_t Left, uint64_t Right) {
  if (Right != 0 && Left > std::numeric_limits<uint64_t>::max() / Right)
    llvm::report_fatal_error("LLVM synthetic stack size overflow");
  return Left * Right;
}

uint64_t alignSyntheticStack(uint64_t Size) {
  constexpr uint64_t Mask = kSyntheticStackAlignment - 1;
  return checkedSyntheticStackAdd(Size, Mask) & ~Mask;
}

uint64_t positiveRangeEnd(int64_t Start, uint64_t Size) {
  if (Start >= 0)
    return checkedSyntheticStackAdd(static_cast<uint64_t>(Start), Size);
  uint64_t Distance = static_cast<uint64_t>(-(Start + 1)) + 1;
  return Size > Distance ? Size - Distance : 0;
}

} // anonymous namespace

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
  if (auto *Existing = Mod->getFunction(Func.Name))
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
      FuncTy, llvm::GlobalValue::ExternalLinkage, Func.Name, Mod);
  // A lifted function is defined in this same module, so its address is fixed
  // at link time — mark it dso_local so a `ptrtoint @func` reference (a
  // function pointer) under the PIC relocation model is materialized by direct
  // page-relative addressing (`lea rip` / `adrp+add`) instead of a GOT load.
  // The recompiled object carries no GOT, so a GOT-indirected function pointer
  // would read unmapped memory.  Mirrors the dso_local data globals.  External
  // call targets keep default visibility (declared elsewhere, may be imports).
  LLVMFunc->setDSOLocal(true);
  LLVMFunc->addFnAttr(llvm::Attribute::NullPointerIsValid);
  return LLVMFunc;
}

llvm::Function *MedLLVMEmitter::emitFunc(const MedFunc &Func) {
  if (Func.Blocks.empty())
    return nullptr;

  CurMedFunc = &Func;

  auto *LLVMFunc = declareFunc(Func);
  emitExceptionMetadata(Func, *LLVMFunc);
  CurFunc = LLVMFunc;
  VarAllocs.clear();
  CallSiteAddrs.clear();
  ParamArgs.clear();
  ParamRegoffMap.clear();
  DynVlaBases.clear();
  PendingDispatchStores.clear();
  FrameAlloca = nullptr;
  FrameBaseInt = nullptr;
  EHExceptionAlloca = nullptr;
  EHSelectorAlloca = nullptr;

  unsigned PI = 0;
  for (auto &Arg : LLVMFunc->args()) {
    if (PI < Func.Params.size()) {
      std::string PName;
      if (Func.hasTypeInfo() && PI < Func.TypedParams.size())
        PName = Func.TypedParams[PI].Name;
      else
        PName = Func.Params[PI].display();
      Arg.setName(PName);
      ParamArgs[PName] = &Arg;
      std::string OrigName = Func.Params[PI].display();
      if (OrigName != PName)
        ParamArgs[OrigName] = &Arg;
      if ((Func.Params[PI].Kind == MedVar::Param ||
           Func.Params[PI].Kind == MedVar::Reg) &&
          Func.Params[PI].RegOff != kNoParamReg)
        ParamRegoffMap[Func.Params[PI].RegOff] = &Arg;
    }
    ++PI;
  }

  const bool NeedsWindowsEHPrologue =
      TargetArch == Arch::X64 && TargetFormat == BinaryFormat::COFF &&
      Func.ExceptionMetadata &&
      (Func.ExceptionMetadata->Personality ==
           ExceptionPersonality::CSpecificHandler ||
       Func.ExceptionMetadata->Personality ==
           ExceptionPersonality::CxxFrameHandler3);
  const bool NeedsFrameSetup = Func.FrameSize > 0 || Func.FrameHeadroom > 0 ||
                               Func.IsVariadic ||
                               !Func.MutableStackParamHomes.empty();
  llvm::BasicBlock *FrameSetupBB = nullptr;
  if (NeedsFrameSetup || NeedsWindowsEHPrologue) {
    FrameSetupBB = llvm::BasicBlock::Create(*Ctx, kFrameSetupBlock, LLVMFunc);
    if (NeedsFrameSetup) {
      const auto &TRI = getTargetRegInfo(TargetArch);
      llvm::IRBuilder<> FrameB(FrameSetupBB);
      // Preserve the target ABI's entry-SP residue: AArch64 enters aligned,
      // x86-64 is 8 mod 16, and Darwin i386 is 12 mod 16.
      uint64_t AlignedFrameSize = alignSyntheticStack(
          Func.FrameSize > 0 ? static_cast<uint64_t>(Func.FrameSize) : 0);
      uint64_t EntryResidue =
          syntheticEntryStackResidue(TargetArch, TargetFormat);
      uint64_t FrameBaseOffset =
          checkedSyntheticStackAdd(AlignedFrameSize, EntryResidue);
      // A variadic function reads its overflow (incoming-stack) arguments at
      // entry_sp + base + i*slot, above frame_end.  Reserve headroom there
      // (kept separate from frame_end so the SP self-copy stays at frame_end)
      // and spill the recovered overflow stack parameters into it below.
      uint64_t Headroom = 0;
      // If no overflow arguments were recovered as LLVM parameters, the
      // variadic walk still reads the caller's native entry-stack area. Putting
      // generic positive stack slots in local headroom would shadow that area
      // with uninitialised storage.  Explicitly seeded homes below remain safe.
      uint64_t MaxHomeEnd = 0;
      if (!(Func.IsVariadic && Func.VariadicOverflowCount == 0) &&
          Func.FrameHeadroom > 0)
        MaxHomeEnd = static_cast<uint64_t>(Func.FrameHeadroom);
      if (Func.IsVariadic && Func.VariadicOverflowCount > 0) {
        uint64_t OverflowBytes = checkedSyntheticStackMul(
            static_cast<uint64_t>(Func.VariadicOverflowCount),
            static_cast<uint64_t>(TRI.PointerSize));
        MaxHomeEnd =
            std::max(MaxHomeEnd, positiveRangeEnd(Func.VariadicOverflowBase,
                                                  OverflowBytes));
      }
      // Written incoming stack-argument home slots (a parameter updated in
      // place) also live above frame_end and are seeded at entry below, so the
      // headroom must cover them too.
      for (const auto &Home : Func.MutableStackParamHomes)
        MaxHomeEnd = std::max(MaxHomeEnd,
                              positiveRangeEnd(Home.second, TRI.PointerSize));
      if (MaxHomeEnd > 0) {
        Headroom = alignSyntheticStack(checkedSyntheticStackAdd(
            MaxHomeEnd, static_cast<uint64_t>(limits::kVariadicOverflowSlop)));
      }
      uint64_t StorageSize =
          checkedSyntheticStackAdd(FrameBaseOffset, Headroom);
      auto *FrameTy =
          llvm::ArrayType::get(llvm::Type::getInt8Ty(*Ctx), StorageSize);
      FrameAlloca = FrameB.CreateAlloca(FrameTy, nullptr, "frame");
      FrameAlloca->setAlignment(llvm::Align(16));
      auto *FrameEnd = FrameB.CreateInBoundsGEP(
          llvm::Type::getInt8Ty(*Ctx), FrameAlloca,
          llvm::ConstantInt::get(llvm::Type::getInt64Ty(*Ctx), FrameBaseOffset),
          "frame_end");
      FrameBaseInt = FrameB.CreatePtrToInt(
          FrameEnd, llvm::Type::getInt64Ty(*Ctx), kRspInitValue);

      // Spill the variadic overflow stack parameters (the trailing parameters)
      // into the headroom at entry_sp + base + i*slot, so the unchanged va_arg
      // walk — which the LLVM optimizer resolves to those addresses — reads the
      // caller's overflow arguments instead of uninitialised frame memory.
      if (Func.IsVariadic && Func.VariadicOverflowCount > 0) {
        const int K = Func.VariadicOverflowCount;
        const int NumParams = static_cast<int>(Func.Params.size());
        std::vector<llvm::Argument *> OverflowArgs;
        int PIdx = 0;
        for (auto &A : LLVMFunc->args()) {
          if (PIdx >= NumParams - K && PIdx < NumParams)
            OverflowArgs.push_back(&A);
          ++PIdx;
        }
        for (int I = 0; I < static_cast<int>(OverflowArgs.size()); ++I) {
          int64_t Off = Func.VariadicOverflowBase +
                        static_cast<int64_t>(I) * TRI.PointerSize;
          auto *AddrInt = FrameB.CreateAdd(
              FrameBaseInt, llvm::ConstantInt::get(llvm::Type::getInt64Ty(*Ctx),
                                                   static_cast<uint64_t>(Off)));
          auto *AddrPtr =
              FrameB.CreateIntToPtr(AddrInt, llvm::PointerType::get(*Ctx, 0));
          FrameB.CreateStore(OverflowArgs[I], AddrPtr);
        }
      }

      // Seed each written incoming stack-argument home slot with its parameter
      // so the in-IR memory loads/stores through [frame_end + Off] read the
      // argument and observe later in-place writes (mirrors the variadic
      // overflow spill).
      if (!Func.MutableStackParamHomes.empty()) {
        const int NumParams = static_cast<int>(Func.Params.size());
        std::vector<llvm::Argument *> ArgPtrs;
        for (auto &A : LLVMFunc->args())
          ArgPtrs.push_back(&A);
        for (const auto &[PIdx, Off] : Func.MutableStackParamHomes) {
          if (PIdx < 0 || PIdx >= NumParams ||
              PIdx >= static_cast<int>(ArgPtrs.size()))
            continue;
          auto *AddrInt = FrameB.CreateAdd(
              FrameBaseInt, llvm::ConstantInt::get(llvm::Type::getInt64Ty(*Ctx),
                                                   static_cast<uint64_t>(Off)));
          auto *AddrPtr =
              FrameB.CreateIntToPtr(AddrInt, llvm::PointerType::get(*Ctx, 0));
          FrameB.CreateStore(ArgPtrs[PIdx], AddrPtr);
        }
      }
    }
  }

  DataSizeHints.clear();
  {
    constexpr uint64_t kMin = limits::kMinGlobalDataAddr;
    std::map<std::pair<int, int>, uint64_t> ConstMap;
    for (const auto &Blk : Func.Blocks) {
      for (const auto &Op : Blk.Ops) {
        if (Op.Opcode == NdOp::COPY && Op.NumInputs >= 1 &&
            Op.Inputs[0].isConst() && Op.Inputs[0].ConstVal > kMin) {
          ConstMap[{Op.Output.Id, Op.Output.SSAVer}] = Op.Inputs[0].ConstVal;
        }
        uint64_t AddrVal = 0;
        uint16_t DataWidth = 0;
        if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1) {
          DataWidth = Op.Output.Size;
          if (Op.Inputs[0].isConst() && Op.Inputs[0].ConstVal > kMin)
            AddrVal = Op.Inputs[0].ConstVal;
          else {
            auto It = ConstMap.find({Op.Inputs[0].Id, Op.Inputs[0].SSAVer});
            if (It != ConstMap.end())
              AddrVal = It->second;
          }
        } else if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2) {
          DataWidth = Op.Inputs[1].Size;
          if (Op.Inputs[0].isConst() && Op.Inputs[0].ConstVal > kMin)
            AddrVal = Op.Inputs[0].ConstVal;
          else {
            auto It = ConstMap.find({Op.Inputs[0].Id, Op.Inputs[0].SSAVer});
            if (It != ConstMap.end())
              AddrVal = It->second;
          }
        }
        if (AddrVal && DataWidth > 0) {
          auto &Cur = DataSizeHints[AddrVal];
          if (DataWidth > Cur)
            Cur = DataWidth;
        }
      }
    }
  }

  std::map<int, llvm::BasicBlock *> BBMap;
  for (auto &Blk : Func.Blocks) {
    auto *BB = llvm::BasicBlock::Create(*Ctx, "bb_" + std::to_string(Blk.Id),
                                        LLVMFunc);
    BBMap[Blk.Id] = BB;
  }

  if (!Func.Blocks.empty()) {
    int EntryId = Func.Blocks.front().Id;
    if (FrameSetupBB) {
      llvm::IRBuilder<> FB(FrameSetupBB);
      FB.CreateBr(BBMap[EntryId]);
    } else {
      bool EntryHasPreds = false;
      for (auto &Blk : Func.Blocks) {
        for (int SId : Blk.Succs) {
          if (SId == EntryId) {
            EntryHasPreds = true;
            break;
          }
        }
        if (EntryHasPreds)
          break;
      }
      if (EntryHasPreds) {
        auto *RealEntry = &LLVMFunc->getEntryBlock();
        auto *LoopTarget = BBMap[EntryId];
        auto *NewEntry =
            llvm::BasicBlock::Create(*Ctx, "entry", LLVMFunc, RealEntry);
        llvm::IRBuilder<> EntryB(NewEntry);
        EntryB.CreateBr(LoopTarget);
        LoopTarget->moveAfter(NewEntry);
      }
    }
  }

  SubRegPropMap.clear();

  // Pre-pass: create allocas for ALL phi outputs *and* all op outputs across
  // ALL blocks *before* emitting any ops.  A block that appears earlier in
  // Func.Blocks order (e.g. a loop latch) can use a value defined in a later
  // block (the loop header) — this is valid SSA whenever the defining block
  // dominates the using block.  If that value's alloca hasn't been created yet
  // when the earlier block's ops are emitted, getVar() fails the (Id,SSAVer)
  // lookup and wrongly falls back to reading the overlapping *wide* register (a
  // stale, loop-invariant value), silently dropping loop-carried values.
  //
  // This previously only covered phi outputs (bug #210: `add w0, w10, w0` where
  // w0 is both an argument sub-register and the accumulator).  It must also
  // cover non-phi op outputs: e.g. a `cmovg`-derived running max defined in the
  // loop header (`SUBBYTES EDX.5 RDX.2`) but read in the latch's `add edx, ecx`
  // — when the latch is ordered before the header, EDX.5's alloca is missing
  // and getVar falls back to the wide RDX register, which still holds the
  // loop-entry INT_MIN, folding the max back to its initial value.
  // Op outputs that are live-ins (SSAVer == 0) must NOT be pre-created here:
  // those are resolved through the parameter / entry-register path in getVar
  // (which keys off `!HasLocalDef && SSAVer == 0`), and creating an alloca for
  // them would shadow the incoming argument with an uninitialized slot.
  //
  // For op outputs we also zero-initialize the slot (like getVar's no-def
  // fallback at the bottom of getVar): the previous wide-register fallback used
  // to mask non-dominating cross-block reads by returning an already-defined
  // wide register; reading a bare uninitialized alloca instead would yield
  // poison and let the optimizer fold the whole function to `unreachable`.
  auto preCreatePhiAlloca = [&](const MedVar &Out) {
    if (Out.isConst() || Out.Size == 0)
      return;
    auto Key = std::make_pair(Out.Id, Out.SSAVer);
    if (VarAllocs.find(Key) != VarAllocs.end())
      return;
    auto &Entry = CurFunc->getEntryBlock();
    llvm::IRBuilder<> AllocBuilder(&Entry, Entry.begin());
    auto *Ty = sizeToType(Out.Size);
    VarAllocs[Key] = AllocBuilder.CreateAlloca(Ty, nullptr, Out.display());
  };
  auto preCreateOpAlloca = [&](const MedVar &Out) {
    if (Out.isConst() || Out.Size == 0 || Out.SSAVer == 0)
      return;
    auto Key = std::make_pair(Out.Id, Out.SSAVer);
    if (VarAllocs.find(Key) != VarAllocs.end())
      return;
    auto &Entry = CurFunc->getEntryBlock();
    llvm::IRBuilder<> AllocBuilder(&Entry, Entry.begin());
    auto *Ty = sizeToType(Out.Size);
    auto *Alloca = AllocBuilder.CreateAlloca(Ty, nullptr, Out.display());
    AllocBuilder.CreateStore(llvm::ConstantInt::get(Ty, 0), Alloca);
    VarAllocs[Key] = Alloca;
  };
  for (auto &Blk : Func.Blocks) {
    for (auto &Phi : Blk.Phis)
      preCreatePhiAlloca(Phi.Output);
    for (auto &Op : Blk.Ops)
      preCreateOpAlloca(Op.Output);
  }
  for (const MedCallClobber &Clobber : Func.CallClobbers)
    preCreatePhiAlloca(Clobber.Value);

  for (auto &Blk : Func.Blocks) {
    auto *BB = BBMap[Blk.Id];
    llvm::IRBuilder<> Builder(BB);

    for (size_t OI = 0; OI < Blk.Ops.size(); ++OI) {
      auto &Op = Blk.Ops[OI];
      if (Op.Opcode == NdOp::BRANCH) {
        if (!Blk.Succs.empty()) {
          auto SuccIt = BBMap.find(Blk.Succs[0]);
          if (SuccIt != BBMap.end())
            Builder.CreateBr(SuccIt->second);
        }
        break;
      }
      if (Op.Opcode == NdOp::COND_BR) {
        if (Op.NumInputs >= 2) {
          auto *Cond = getVar(Op.Inputs[1], Builder);
          if (Cond->getType() != llvm::Type::getInt1Ty(*Ctx)) {
            auto *Zero = llvm::ConstantInt::get(Cond->getType(), 0);
            Cond = Builder.CreateICmpNE(Cond, Zero, "cond");
          }
          llvm::BasicBlock *TakenBB = nullptr;
          llvm::BasicBlock *FallthroughBB = nullptr;
          if (Blk.Succs.size() >= 2) {
            // Use the COND_BR target address to determine which
            // successor is "taken" (cond=true). The ARM32 lifter
            // emits COND_BR with the fallthrough address and an
            // inverted condition, so the target may correspond to
            // Succs[0] rather than Succs[1].
            uint64_t CbrTarget =
                Op.Inputs[0].isConst() ? Op.Inputs[0].ConstVal : 0;
            int TakenId = Blk.Succs[1], FallId = Blk.Succs[0];
            if (CbrTarget != 0) {
              for (auto &B : CurMedFunc->Blocks) {
                if (B.Id == Blk.Succs[0] && B.Ops.size() > 0 &&
                    B.Ops[0].Addr == CbrTarget) {
                  TakenId = Blk.Succs[0];
                  FallId = Blk.Succs[1];
                  break;
                }
              }
            }
            auto ItFall = BBMap.find(FallId);
            auto ItTaken = BBMap.find(TakenId);
            if (ItFall != BBMap.end())
              FallthroughBB = ItFall->second;
            if (ItTaken != BBMap.end())
              TakenBB = ItTaken->second;
          } else if (Blk.Succs.size() == 1) {
            auto It0 = BBMap.find(Blk.Succs[0]);
            llvm::BasicBlock *ContBB =
                (It0 != BBMap.end()) ? It0->second : nullptr;
            // Conditional return idiom (`bxCC lr` / predicated `pop {pc}` used
            // as a loop exit): the block ends in COND_BR(target, cond) followed
            // by a RETURN, but the CFG gives it a single successor (the COND_BR
            // target) because the return edge has no block.  Synthesize a
            // return block for the not-taken (cond-false) edge so the
            // predicated return survives — otherwise both CondBr edges aimed at
            // the lone successor and the RETURN was dropped, turning the loop
            // exit into an infinite loop (#385 swfall).
            int RetOpIdx = -1;
            for (size_t RI = OI + 1; RI < Blk.Ops.size(); ++RI)
              if (Blk.Ops[RI].Opcode == NdOp::RETURN) {
                RetOpIdx = static_cast<int>(RI);
                break;
              }
            if (ContBB && RetOpIdx >= 0) {
              auto *RetBB = llvm::BasicBlock::Create(*Ctx, "condret", CurFunc);
              Builder.CreateCondBr(Cond, ContBB, RetBB);
              llvm::IRBuilder<> RetBuilder(RetBB);
              emitOp(Blk.Ops[RetOpIdx], RetBuilder, Blk.Id, RetOpIdx);
            } else if (ContBB) {
              TakenBB = FallthroughBB = ContBB;
            }
          }
          if (TakenBB && FallthroughBB)
            Builder.CreateCondBr(Cond, TakenBB, FallthroughBB);
        }
        break;
      }
      if (Op.Opcode == NdOp::INDIR_BR) {
        if (!emitJumpTableSwitch(Blk, Op, BBMap, Builder) &&
            Blk.Succs.size() > 1) {
          // A INDIR_BR that survived convertIndirectTailCalls has a resolved
          // jump table.  If its switch could not be rebuilt (e.g. a shared
          // multi-site -O0 computed-goto dispatch whose per-predecessor index
          // could not be recovered), trap loudly instead of silently falling
          // through to the first successor (the always-first-target
          // miscompile).
          Builder.CreateIntrinsic(llvm::Type::getVoidTy(*Ctx),
                                  llvm::Intrinsic::trap, {});
          Builder.CreateUnreachable();
        }
        break;
      }
      if (Op.Opcode == NdOp::RETURN) {
        emitOp(Op, Builder, Blk.Id, static_cast<int>(OI));
        break;
      }
      emitOp(Op, Builder, Blk.Id, static_cast<int>(OI));
      // Architecture side-effect emitters may terminate the block themselves
      // (for example AArch64 BRK/HLT).  Do not append later lifted operations
      // or the CFG's conservative fallthrough branch after an LLVM terminator.
      if (!BB->empty() && BB->back().isTerminator())
        break;
    }

    if (BB->empty() || !BB->back().isTerminator()) {
      auto EmitDefaultRet = [&]() {
        if (CurFunc->getReturnType()->isVoidTy())
          Builder.CreateRetVoid();
        else
          Builder.CreateRet(
              llvm::ConstantInt::get(CurFunc->getReturnType(), 0));
      };

      if (Blk.Succs.size() == 1) {
        auto SuccIt = BBMap.find(Blk.Succs[0]);
        if (SuccIt != BBMap.end())
          Builder.CreateBr(SuccIt->second);
        else
          EmitDefaultRet();
      } else if (Blk.Succs.empty()) {
        EmitDefaultRet();
      } else {
        auto SuccIt = BBMap.find(Blk.Succs[0]);
        if (SuccIt != BBMap.end())
          Builder.CreateBr(SuccIt->second);
        else
          EmitDefaultRet();
      }
    }
  }

  // Emit the deferred per-predecessor index stores for shared -O0 computed-goto
  // dispatches recovered as switches (synthesizeSharedDispatchIndex): each
  // predecessor stores its own index into the common slot before its
  // terminator. All op-output allocas exist and each predecessor's body is
  // fully emitted, so getVar reads each index correctly (its def dominates the
  // predecessor's terminator) and the store reaches the dispatch's slot load on
  // that edge. Done before the phi/critical-edge pass: the store stays in the
  // predecessor block regardless of any later edge split, so the dispatch load
  // still observes it on the taken edge.
  for (auto &PD : PendingDispatchStores) {
    for (auto &PredIdx : PD.Preds) {
      auto PIt = BBMap.find(PredIdx.first);
      if (PIt == BBMap.end())
        continue;
      auto *Term = PIt->second->getTerminator();
      if (!Term)
        continue;
      llvm::IRBuilder<> SB(Term);
      llvm::Value *V = getVar(PredIdx.second, SB);
      if (!V || !V->getType()->isIntegerTy())
        continue;
      unsigned Have = V->getType()->getIntegerBitWidth();
      unsigned Want = PD.Ty->getIntegerBitWidth();
      if (Have > Want)
        V = SB.CreateTrunc(V, PD.Ty);
      else if (Have < Want)
        V = SB.CreateZExt(V, PD.Ty);
      SB.CreateStore(V, PD.Slot);
    }
  }

  // Phi emission with critical-edge splitting.  When a predecessor has
  // multiple successors (conditional branch), phi copies for one edge
  // must not clobber values needed by another edge.  We split such
  // edges by inserting an intermediate block that holds the copies.
  {
    // True when the predecessor block writes a register that overlaps `Narrow`
    // at the same offset but is wider — i.e. the narrow phi must re-resolve
    // through the wider register on this edge rather than self-reference.
    auto widerRegWrittenInBlock = [&](int PredId,
                                      const MedVar &Narrow) -> bool {
      if (!CurMedFunc || Narrow.Kind != MedVar::Reg || Narrow.Size == 0)
        return false;
      for (auto &Blk : CurMedFunc->Blocks) {
        if (Blk.Id != PredId)
          continue;
        for (auto &Op : Blk.Ops)
          if (Op.Output.Kind == MedVar::Reg &&
              Op.Output.RegOff == Narrow.RegOff && Op.Output.Size > Narrow.Size)
            return true;
        return false;
      }
      return false;
    };

    // Group: (PredId, TargetBlockId) → list of (PhiOutput, PhiArg)
    using EdgeKey = std::pair<int, int>;
    std::map<EdgeKey, std::vector<std::pair<MedVar, MedVar>>> EdgePhis;
    auto IsExceptionalEdge = [&](int PredId, int TargetId) {
      for (const MedBlock &Pred : Func.Blocks) {
        if (Pred.Id != PredId)
          continue;
        return std::any_of(Pred.ExceptionalSuccs.begin(),
                           Pred.ExceptionalSuccs.end(),
                           [&](const ExceptionalEdge &Edge) {
                             return Edge.BlockId == TargetId;
                           });
      }
      return false;
    };
    for (auto &Blk : Func.Blocks) {
      for (auto &Phi : Blk.Phis) {
        for (auto &[PredId, Var] : Phi.Args) {
          // A throwing call never reaches its ordinary block terminator.
          // These copies are emitted immediately before the call when it is
          // converted to an invoke by emitNativeItaniumEH().
          if (IsExceptionalEdge(PredId, Blk.Id))
            continue;
          EdgePhis[{PredId, Blk.Id}].emplace_back(Phi.Output, Var);
        }
      }
    }

    for (auto &[Edge, Copies] : EdgePhis) {
      auto [PredId, TargetId] = Edge;
      auto PredIt = BBMap.find(PredId);
      auto TargetIt = BBMap.find(TargetId);
      if (PredIt == BBMap.end() || TargetIt == BBMap.end())
        continue;
      auto *PredBB = PredIt->second;
      auto *TargetBB = TargetIt->second;
      auto *Term = PredBB->getTerminator();
      if (!Term)
        continue;

      if (!llvm::isa<llvm::UncondBrInst, llvm::CondBrInst>(Term) &&
          !llvm::isa<llvm::SwitchInst>(Term) &&
          !llvm::isa<llvm::IndirectBrInst>(Term))
        continue;

      bool NeedSplit = Term->getNumSuccessors() > 1;

      llvm::BasicBlock *InsertBB = PredBB;
      if (NeedSplit) {
        auto *SplitBB = llvm::BasicBlock::Create(PredBB->getContext(),
                                                 PredBB->getName() + ".phi." +
                                                     TargetBB->getName(),
                                                 CurFunc, TargetBB);

        for (unsigned I = 0; I < Term->getNumSuccessors(); ++I) {
          if (Term->getSuccessor(I) == TargetBB) {
            Term->setSuccessor(I, SplitBB);
          }
        }

        llvm::UncondBrInst::Create(TargetBB, SplitBB);
        InsertBB = SplitBB;
      }

      auto *InsTerm = InsertBB->getTerminator();
      if (!InsTerm)
        continue;
      llvm::IRBuilder<> InsBuilder(InsTerm);

      std::vector<std::pair<MedVar, llvm::Value *>> Pending;
      Pending.reserve(Copies.size());
      for (auto &[Dst, Src] : Copies) {
        llvm::Value *Val;
        if (Src.isConst()) {
          Val = llvm::ConstantInt::get(sizeToType(Dst.Size),
                                       static_cast<int64_t>(Src.ConstVal),
                                       /*isSigned=*/true);
        } else {
          auto SrcKey = std::make_pair(Src.Id, Src.SSAVer);
          auto DstKey = std::make_pair(Dst.Id, Dst.SSAVer);
          llvm::AllocaInst *HiddenAlloca = nullptr;
          // A self-edge X=X normally means X is loop-invariant on this edge, so
          // the copy is a no-op self-reference.  Only when a *wider*
          // overlapping register is written in this predecessor block must the
          // narrow view re-resolve through it (hide the alloca so getVar
          // reaches the wide register).  Hiding unconditionally would, for a
          // register that is genuinely loop-invariant in an inner loop, force
          // getVar's wide fallback to pull a different (outer-loop) wide
          // version and corrupt the value.
          if (SrcKey == DstKey && Src.Kind == MedVar::Reg &&
              widerRegWrittenInBlock(PredId, Src)) {
            auto It = VarAllocs.find(SrcKey);
            if (It != VarAllocs.end()) {
              HiddenAlloca = It->second;
              VarAllocs.erase(It);
            }
          }
          Val = getVar(Src, InsBuilder);
          if (HiddenAlloca)
            VarAllocs[SrcKey] = HiddenAlloca;
        }
        Pending.emplace_back(Dst, Val);
      }

      for (auto &[Dst, Val] : Pending)
        setVar(Dst, Val, InsBuilder);
    }
  }

  // The models are mutually exclusive by target: the two Windows lowerings
  // need an x64 COFF frame with a Windows personality, and the Itanium one
  // needs an LSDA, so the first that recognizes the function is the only one
  // that can.
  if (!emitNativeSEH(Func, *LLVMFunc, BBMap) &&
      !emitNativeCxxEH(Func, *LLVMFunc, BBMap))
    emitNativeItaniumEH(Func, *LLVMFunc, BBMap);

  for (auto &BB : *CurFunc) {
    if (BB.empty() || !BB.back().isTerminator()) {
      llvm::IRBuilder<> FixBuilder(&BB);
      if (CurFunc->getReturnType()->isVoidTy())
        FixBuilder.CreateRetVoid();
      else
        FixBuilder.CreateRet(
            llvm::ConstantInt::get(CurFunc->getReturnType(), 0));
    }
  }

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
  GlobalDataCache.clear();
  SegmentDataGlobals.clear();
  GlobalStrCounter = 0;

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

  FuncNames.clear();
  for (auto &F : Funcs)
    FuncNames[F.Entry] = F.Name;
  for (auto &[Addr, Name] : Imports)
    FuncNames[Addr] = Name;

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
