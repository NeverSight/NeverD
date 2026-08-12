//===- MedLLVMEmitter.cpp - MedIR to LLVM IR emitter core ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Core MedIR to LLVM IR emission: type conversion, the memory-pointer
/// primitive, and function/module emission.  The rest of the emitter is split
/// by concern: operation dispatch in MedLLVMOpEmitter.cpp, INTRINSIC in
/// MedLLVMIntrinsic.cpp, floating-point ops in MedLLVMFloatEmitter.cpp, SSA
/// constant tracing and address resolution in MedLLVMAddrResolve.cpp, variable
/// access and constant classification in MedLLVMVarAccess.cpp, global-data
/// resolution in MedLLVMGlobalData.cpp, and jump-table switch lowering in
/// MedLLVMSwitch.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"

#include "neverd/Common.h"
#include "neverd/backend/llvm/LanguageEHMetadata.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/ir/TargetRegInfo.h"

#define DEBUG_TYPE "neverd-med-llvm-emitter"
#include "neverd/ArchSupport.h"
#include "neverd/Limits.h"
#include "neverd/Support/Diagnostic.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Metadata.h"
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

llvm::Metadata *mdUInt(llvm::LLVMContext &Ctx, uint64_t Value,
                       unsigned Bits = 64) {
  return llvm::ConstantAsMetadata::get(
      llvm::ConstantInt::get(llvm::IntegerType::get(Ctx, Bits), Value));
}

llvm::Metadata *mdSInt(llvm::LLVMContext &Ctx, int64_t Value,
                       unsigned Bits = 64) {
  return llvm::ConstantAsMetadata::get(
      llvm::ConstantInt::getSigned(llvm::IntegerType::get(Ctx, Bits), Value));
}

std::string hexBytes(const std::vector<uint8_t> &Bytes) {
  static constexpr char Digits[] = "0123456789abcdef";
  std::string Result;
  Result.reserve(Bytes.size() * 2);
  for (uint8_t Byte : Bytes) {
    Result.push_back(Digits[Byte >> 4]);
    Result.push_back(Digits[Byte & 0x0f]);
  }
  return Result;
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

void MedLLVMEmitter::emitExceptionMetadata(const MedFunc &Func,
                                           llvm::Function &LLVMFunc) {
  if (!Func.ExceptionMetadata)
    return;
  const ExceptionFunction &EH = *Func.ExceptionMetadata;
  auto Str = [&](llvm::StringRef Value) -> llvm::Metadata * {
    return llvm::MDString::get(*Ctx, Value);
  };
  auto Node = [&](const std::vector<llvm::Metadata *> &Values) {
    return llvm::MDNode::get(*Ctx, Values);
  };

  auto UnwindOp = [&](const UnwindOperation &Op) -> llvm::Metadata * {
    return Node({mdUInt(*Ctx, static_cast<uint8_t>(Op.Kind), 8),
                 mdUInt(*Ctx, Op.CodeOffset, 32), mdUInt(*Ctx, Op.OpInfo, 8),
                 mdUInt(*Ctx, Op.SlotCount, 8), mdUInt(*Ctx, Op.Register, 16),
                 mdUInt(*Ctx, Op.StackOffset), Str(hexBytes(Op.OperandBytes)),
                 mdUInt(*Ctx, static_cast<uint8_t>(Op.RegisterClass), 8),
                 mdUInt(*Ctx, Op.RegisterMask, 32),
                 mdUInt(*Ctx, Op.InstructionSize, 8)});
  };

  std::vector<llvm::Metadata *> UnwindOps;
  UnwindOps.reserve(EH.UnwindOperations.size());
  for (const UnwindOperation &Op : EH.UnwindOperations)
    UnwindOps.push_back(UnwindOp(Op));

  std::vector<llvm::Metadata *> Epilogs;
  Epilogs.reserve(EH.Epilogs.size());
  for (const UnwindEpilog &Epilog : EH.Epilogs) {
    std::vector<llvm::Metadata *> Ops;
    Ops.reserve(Epilog.Operations.size());
    for (const UnwindOperation &Op : Epilog.Operations)
      Ops.push_back(UnwindOp(Op));
    Epilogs.push_back(
        Node({mdSInt(*Ctx, Epilog.StartOffset), mdUInt(*Ctx, Epilog.Flags, 8),
              mdUInt(*Ctx, Epilog.FirstOperationOffset, 32),
              mdUInt(*Ctx, Epilog.LastInstructionOffset, 32), Node(Ops)}));
  }

  std::vector<llvm::Metadata *> SEHScopes;
  if (EH.SEH) {
    SEHScopes.reserve(EH.SEH->Scopes.size());
    for (const SEHScopeRecord &Scope : EH.SEH->Scopes)
      SEHScopes.push_back(Node(
          {mdUInt(*Ctx, Scope.GuardedRange.Begin),
           mdUInt(*Ctx, Scope.GuardedRange.End),
           mdUInt(*Ctx, static_cast<uint8_t>(Scope.Kind), 8),
           mdUInt(*Ctx, Scope.FilterOrFinallyVA), mdUInt(*Ctx, Scope.HandlerVA),
           mdUInt(*Ctx, Scope.ContinuationVA),
           Str(getExceptionParseStatusName(Scope.ParseStatus))}));
  }

  std::vector<llvm::Metadata *> CxxUnwind;
  std::vector<llvm::Metadata *> CxxTry;
  std::vector<llvm::Metadata *> CxxIP;
  llvm::Metadata *CxxHeader = nullptr;
  if (EH.Cxx) {
    const CxxExceptionInfo &Cxx = *EH.Cxx;
    std::vector<llvm::Metadata *> CxxSpecTypes;
    for (const CxxExceptionSpecType &Spec : Cxx.ExceptionSpecTypes)
      CxxSpecTypes.push_back(Node({mdUInt(*Ctx, Spec.Adjectives, 32),
                                   mdUInt(*Ctx, Spec.TypeDescriptorVA)}));
    CxxHeader = Node(
        {mdUInt(*Ctx, static_cast<uint8_t>(Cxx.NativeEncoding), 8),
         mdUInt(*Ctx, Cxx.Magic, 32), mdUInt(*Ctx, Cxx.Flags, 32),
         mdUInt(*Ctx, Cxx.MaxState, 32), mdSInt(*Ctx, Cxx.UnwindHelpOffset, 32),
         mdUInt(*Ctx, Cxx.ESTypeListVA), mdUInt(*Ctx, Cxx.BBTFlags, 32),
         mdUInt(*Ctx, Cxx.FrameOffset, 32), mdUInt(*Ctx, Cxx.IsCatchFunclet, 1),
         mdUInt(*Ctx, Cxx.IsSeparated, 1), mdUInt(*Ctx, Cxx.IsSynchronous, 1),
         mdUInt(*Ctx, Cxx.IsNoExcept, 1),
         mdUInt(*Ctx, static_cast<uint8_t>(Cxx.Version), 8),
         mdUInt(*Ctx, Cxx.HasDynamicStackAlignment, 1), Node(CxxSpecTypes)});
    for (const CxxUnwindAction &Action : Cxx.UnwindMap)
      CxxUnwind.push_back(
          Node({mdSInt(*Ctx, Action.ToState, 32), mdUInt(*Ctx, Action.ActionVA),
                mdUInt(*Ctx, static_cast<uint8_t>(Action.Kind), 8),
                mdSInt(*Ctx, Action.ObjectOffset, 32)}));
    for (const CxxTryBlock &Try : Cxx.TryBlocks) {
      std::vector<llvm::Metadata *> Catches;
      for (const CxxCatchHandler &Catch : Try.Handlers) {
        std::vector<llvm::Metadata *> Continuations;
        for (va_t Address : Catch.ContinuationVAs)
          Continuations.push_back(mdUInt(*Ctx, Address));
        Catches.push_back(Node({mdUInt(*Ctx, Catch.Adjectives, 32),
                                mdUInt(*Ctx, Catch.TypeDescriptorVA),
                                mdSInt(*Ctx, Catch.CatchObjectOffset, 32),
                                mdUInt(*Ctx, Catch.HandlerVA),
                                mdSInt(*Ctx, Catch.ParentFrameOffset, 32),
                                Node(Continuations)}));
      }
      CxxTry.push_back(
          Node({mdSInt(*Ctx, Try.TryLow, 32), mdSInt(*Ctx, Try.TryHigh, 32),
                mdSInt(*Ctx, Try.CatchHigh, 32), Node(Catches)}));
    }
    for (const CxxIPState &IP : Cxx.IPMap)
      CxxIP.push_back(Node({mdUInt(*Ctx, IP.IP), mdSInt(*Ctx, IP.State, 32)}));
  } else {
    CxxHeader = Node({});
  }

  llvm::Metadata *GSCookie = Node({});
  if (EH.GSCookie) {
    const GSCookieInfo &GS = *EH.GSCookie;
    GSCookie = Node(
        {Str(getExceptionParseStatusName(GS.ParseStatus)),
         mdSInt(*Ctx, GS.CookieOffset, 32),
         mdUInt(*Ctx, GS.HasExceptionHandler, 1),
         mdUInt(*Ctx, GS.HasUnwindHandler, 1), mdUInt(*Ctx, GS.HasAlignment, 1),
         mdSInt(*Ctx, GS.AlignmentBaseOffset, 32),
         mdUInt(*Ctx, GS.Alignment, 32), Str(hexBytes(GS.Payload))});
  }

  std::vector<llvm::Metadata *> Diagnostics;
  for (const std::string &Message : EH.Diagnostics)
    Diagnostics.push_back(Str(Message));

  llvm::Metadata *PrimaryFunctionIndex = Node({});
  if (EH.PrimaryFunctionIndex)
    PrimaryFunctionIndex =
        Node({mdUInt(*Ctx, static_cast<uint64_t>(*EH.PrimaryFunctionIndex))});
  llvm::Metadata *ChainedPrimaryRange = Node({});
  if (EH.ChainedPrimaryRange)
    ChainedPrimaryRange = Node({mdUInt(*Ctx, EH.ChainedPrimaryRange->Begin),
                                mdUInt(*Ctx, EH.ChainedPrimaryRange->End)});

  llvm::MDNode *Payload =
      Node({mdUInt(*Ctx, windows_eh_md::SchemaVersion, 32),
            Str(getExceptionParseStatusName(EH.ParseStatus)),
            Str(getExceptionEncodingName(EH.Encoding)),
            mdUInt(*Ctx, static_cast<uint8_t>(EH.Kind), 8),
            mdUInt(*Ctx, EH.CodeRange.Begin),
            mdUInt(*Ctx, EH.CodeRange.End),
            mdUInt(*Ctx, EH.RuntimeFunctionRVA, 32),
            mdUInt(*Ctx, EH.UnwindInfoRVA, 32),
            mdUInt(*Ctx, EH.UnwindInfoVA),
            mdUInt(*Ctx, EH.UnwindVersion, 8),
            mdUInt(*Ctx, EH.UnwindFlags, 8),
            mdUInt(*Ctx, EH.PrologueSize, 32),
            mdUInt(*Ctx, EH.FrameRegister, 16),
            mdUInt(*Ctx, EH.FrameOffset, 32),
            mdUInt(*Ctx, EH.PackedUnwindData, 32),
            Str(getExceptionPersonalityName(EH.Personality)),
            Str(EH.PersonalityName),
            mdUInt(*Ctx, EH.PersonalityVA),
            mdUInt(*Ctx, EH.HandlerDataVA),
            Str(hexBytes(EH.NativeUnwindBytes)),
            Node(UnwindOps),
            Node(Epilogs),
            Node(SEHScopes),
            CxxHeader,
            Node(CxxUnwind),
            Node(CxxTry),
            Node(CxxIP),
            GSCookie,
            PrimaryFunctionIndex,
            ChainedPrimaryRange,
            mdUInt(*Ctx, EH.ChainedUnwindInfoRVA, 32),
            Node(Diagnostics),
            mdUInt(*Ctx, EH.canRegenerateLanguageMetadata(), 1)});
  LLVMFunc.setMetadata(windows_eh_md::FunctionAttachment, Payload);
  llvm::NamedMDNode *Table =
      Mod->getOrInsertNamedMetadata(windows_eh_md::FunctionTable);
  Table->addOperand(Node({llvm::ValueAsMetadata::get(&LLVMFunc), Payload}));
}

bool MedLLVMEmitter::emitNativeSEH(
    const MedFunc &Func, llvm::Function &LLVMFunc,
    const std::map<int, llvm::BasicBlock *> &OriginalBlockMap) {
  if (TargetArch != Arch::X64 || TargetFormat != BinaryFormat::COFF ||
      !Func.ExceptionMetadata)
    return false;
  const ExceptionFunction &EH = *Func.ExceptionMetadata;
  if (EH.ParseStatus != ExceptionParseStatus::Complete ||
      EH.Personality != ExceptionPersonality::CSpecificHandler || !EH.SEH ||
      EH.SEH->Scopes.empty() ||
      (EH.Encoding != ExceptionEncoding::X64UnwindV1 &&
       EH.Encoding != ExceptionEncoding::X64UnwindV2))
    return false;

  struct Region {
    const SEHScopeRecord *Scope = nullptr;
    llvm::BasicBlock *Handler = nullptr;
    llvm::Function *Callback = nullptr;
    llvm::BasicBlock *UnwindDest = nullptr;
    int Parent = -1;
    std::set<llvm::BasicBlock *> Blocks;
  };

  auto FunctionAt = [&](va_t Address) -> llvm::Function * {
    auto NameIt = FuncNames.find(Address);
    return NameIt == FuncNames.end() ? nullptr
                                     : Mod->getFunction(NameIt->second);
  };
  auto BlockAt = [&](va_t Address) -> llvm::BasicBlock * {
    for (const MedBlock &Block : Func.Blocks) {
      if (Block.StartAddr != Address)
        continue;
      auto It = OriginalBlockMap.find(Block.Id);
      return It == OriginalBlockMap.end() ? nullptr : It->second;
    }
    return nullptr;
  };

  std::vector<Region> Regions;
  Regions.reserve(EH.SEH->Scopes.size());
  for (const SEHScopeRecord &Scope : EH.SEH->Scopes) {
    if (Scope.ParseStatus != ExceptionParseStatus::Complete ||
        !Scope.GuardedRange.isValid() ||
        !EH.CodeRange.contains(Scope.GuardedRange))
      return false;

    Region R;
    R.Scope = &Scope;
    if (Scope.Kind == SEHScopeKind::Finally) {
      R.Callback = FunctionAt(Scope.FilterOrFinallyVA);
      if (!R.Callback || R.Callback == &LLVMFunc)
        return false;
    } else {
      R.Handler = BlockAt(Scope.HandlerVA);
      if (!R.Handler || Scope.GuardedRange.contains(Scope.HandlerVA))
        return false;
      if (Scope.Kind == SEHScopeKind::Filter) {
        R.Callback = FunctionAt(Scope.FilterOrFinallyVA);
        if (!R.Callback || R.Callback == &LLVMFunc)
          return false;
      }
    }

    bool HasBegin = false;
    bool HasEnd = false;
    for (const MedBlock &Block : Func.Blocks) {
      ExceptionAddressRange BlockRange{Block.StartAddr, Block.EndAddr};
      if (!BlockRange.isValid())
        continue;
      if (Scope.GuardedRange.overlaps(BlockRange) &&
          !Scope.GuardedRange.contains(BlockRange))
        return false;
      if (!Scope.GuardedRange.contains(BlockRange))
        continue;
      auto It = OriginalBlockMap.find(Block.Id);
      if (It == OriginalBlockMap.end())
        return false;
      R.Blocks.insert(It->second);
      HasBegin |= Block.StartAddr == Scope.GuardedRange.Begin;
      HasEnd |= Block.EndAddr == Scope.GuardedRange.End;
    }
    if (R.Blocks.empty() || !HasBegin || !HasEnd)
      return false;
    Regions.push_back(std::move(R));
  }

  // Native WinEH can express nested or disjoint intervals.  Crossing and
  // duplicate intervals have no unambiguous unwind-parent relation, so leave
  // those functions in lossless-metadata-only form.
  for (size_t I = 0; I < Regions.size(); ++I) {
    for (size_t J = I + 1; J < Regions.size(); ++J) {
      const ExceptionAddressRange &A = Regions[I].Scope->GuardedRange;
      const ExceptionAddressRange &B = Regions[J].Scope->GuardedRange;
      if (!A.overlaps(B))
        continue;
      if ((A.Begin == B.Begin && A.End == B.End) ||
          (!A.contains(B) && !B.contains(A)))
        return false;
    }
  }
  std::stable_sort(
      Regions.begin(), Regions.end(), [](const Region &A, const Region &B) {
        if (A.Scope->GuardedRange.size() != B.Scope->GuardedRange.size())
          return A.Scope->GuardedRange.size() > B.Scope->GuardedRange.size();
        return A.Scope->GuardedRange.Begin < B.Scope->GuardedRange.Begin;
      });
  for (size_t I = 0; I < Regions.size(); ++I) {
    uint64_t ParentSize = std::numeric_limits<uint64_t>::max();
    for (size_t J = 0; J < I; ++J) {
      if (!Regions[J].Scope->GuardedRange.contains(
              Regions[I].Scope->GuardedRange))
        continue;
      if (Regions[J].Scope->GuardedRange.size() < ParentSize) {
        Regions[I].Parent = static_cast<int>(J);
        ParentSize = Regions[J].Scope->GuardedRange.size();
      }
    }
  }

  auto *I32Ty = llvm::Type::getInt32Ty(*Ctx);
  auto *PersonalityTy = llvm::FunctionType::get(I32Ty, {}, true);
  llvm::FunctionCallee Personality =
      Mod->getOrInsertFunction("__C_specific_handler", PersonalityTy);
  LLVMFunc.setPersonalityFn(
      llvm::cast<llvm::Constant>(Personality.getCallee()));

  auto *TokenNone = llvm::ConstantTokenNone::get(*Ctx);
  auto *PtrTy = llvm::PointerType::get(*Ctx, 0);
  for (size_t I = 0; I < Regions.size(); ++I) {
    Region &R = Regions[I];
    llvm::BasicBlock *ParentDest =
        R.Parent >= 0 ? Regions[static_cast<size_t>(R.Parent)].UnwindDest
                      : nullptr;
    std::string Suffix = std::to_string(I);
    if (R.Scope->Kind == SEHScopeKind::Finally) {
      auto *PadBB = llvm::BasicBlock::Create(
          *Ctx, "seh.finally.dispatch." + Suffix, &LLVMFunc);
      llvm::IRBuilder<> B(PadBB);
      auto *Pad = B.CreateCleanupPad(TokenNone, {}, "seh.finally.pad");
      llvm::Function *LocalAddress = llvm::Intrinsic::getOrInsertDeclaration(
          Mod, llvm::Intrinsic::localaddress);
      llvm::Value *Frame = B.CreateCall(LocalAddress);
      auto *CallbackTy =
          llvm::FunctionType::get(llvm::Type::getVoidTy(*Ctx),
                                  {llvm::Type::getInt8Ty(*Ctx), PtrTy}, false);
      llvm::SmallVector<llvm::Value *, 2> Args{
          llvm::ConstantInt::get(llvm::Type::getInt8Ty(*Ctx), 1), Frame};
      llvm::SmallVector<llvm::Value *, 1> BundleInputs{Pad};
      llvm::OperandBundleDef Funclet("funclet", BundleInputs);
      B.CreateCall(CallbackTy, R.Callback, Args, {Funclet});
      B.CreateCleanupRet(Pad, ParentDest);
      R.UnwindDest = PadBB;
      continue;
    }

    auto *Dispatch = llvm::BasicBlock::Create(
        *Ctx, "seh.catch.dispatch." + Suffix, &LLVMFunc);
    llvm::IRBuilder<> DB(Dispatch);
    auto *Switch =
        DB.CreateCatchSwitch(TokenNone, ParentDest, 1, "seh.catch.switch");
    auto *PadBB =
        llvm::BasicBlock::Create(*Ctx, "seh.catch.pad." + Suffix, &LLVMFunc);
    Switch->addHandler(PadBB);
    llvm::IRBuilder<> PB(PadBB);
    llvm::Value *Filter =
        R.Scope->Kind == SEHScopeKind::CatchAll
            ? static_cast<llvm::Value *>(llvm::ConstantPointerNull::get(
                  llvm::cast<llvm::PointerType>(PtrTy)))
            : static_cast<llvm::Value *>(R.Callback);
    auto *Pad = PB.CreateCatchPad(Switch, {Filter}, "seh.catch.pad.token");
    PB.CreateCatchRet(Pad, R.Handler);
    R.UnwindDest = Dispatch;
  }

  // Replace may-unwind calls in each protected machine block with invokes to
  // the innermost active region.  Non-call hardware faults are covered by the
  // asynchronous try markers installed below.
  for (const MedBlock &MedBB : Func.Blocks) {
    auto BBIt = OriginalBlockMap.find(MedBB.Id);
    if (BBIt == OriginalBlockMap.end())
      continue;
    llvm::BasicBlock *InitialBB = BBIt->second;
    int Innermost = -1;
    uint64_t InnermostSize = std::numeric_limits<uint64_t>::max();
    for (size_t I = 0; I < Regions.size(); ++I) {
      if (!Regions[I].Blocks.count(InitialBB) ||
          Regions[I].Scope->GuardedRange.size() >= InnermostSize)
        continue;
      Innermost = static_cast<int>(I);
      InnermostSize = Regions[I].Scope->GuardedRange.size();
    }
    if (Innermost < 0)
      continue;

    llvm::SmallVector<llvm::CallInst *, 8> Calls;
    for (llvm::Instruction &Inst : *InitialBB)
      if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Inst))
        if (!Call->doesNotThrow() && !Call->isMustTailCall() &&
            !llvm::isa<llvm::IntrinsicInst>(Call))
          Calls.push_back(Call);

    for (llvm::CallInst *Call : Calls) {
      llvm::BasicBlock *CallBB = Call->getParent();
      llvm::Instruction *Next = Call->getNextNode();
      if (!Next)
        return false;
      llvm::BasicBlock *Cont =
          CallBB->splitBasicBlock(Next, CallBB->getName() + ".seh.cont");
      auto *OldBranch = CallBB->getTerminator();
      llvm::SmallVector<llvm::Value *, 8> Args;
      for (llvm::Use &Arg : Call->args())
        Args.push_back(Arg.get());
      llvm::SmallVector<llvm::OperandBundleDef, 2> Bundles;
      Call->getOperandBundlesAsDefs(Bundles);
      auto *Invoke = llvm::InvokeInst::Create(
          Call->getFunctionType(), Call->getCalledOperand(), Cont,
          Regions[static_cast<size_t>(Innermost)].UnwindDest, Args, Bundles,
          Call->getName(), OldBranch->getIterator());
      Invoke->setCallingConv(Call->getCallingConv());
      Invoke->setAttributes(Call->getAttributes());
      Invoke->setDebugLoc(Call->getDebugLoc());
      Invoke->copyMetadata(*Call);
      Call->replaceAllUsesWith(Invoke);
      Call->eraseFromParent();
      OldBranch->eraseFromParent();
      for (Region &R : Regions)
        if (R.Blocks.count(CallBB))
          R.Blocks.insert(Cont);
    }
  }

  llvm::Function *TryBegin = llvm::Intrinsic::getOrInsertDeclaration(
      Mod, llvm::Intrinsic::seh_try_begin);
  llvm::Function *TryEnd = llvm::Intrinsic::getOrInsertDeclaration(
      Mod, llvm::Intrinsic::seh_try_end);
  auto IsNormalSuccessor = [](llvm::Instruction *Term, unsigned Index) {
    if (auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(Term))
      return Index == 0 && Invoke->getNormalDest();
    if (llvm::isa<llvm::CatchSwitchInst, llvm::CleanupReturnInst>(Term))
      return false;
    return true;
  };

  // Outer-first placement produces the required nesting order on a shared
  // boundary: outer.begin -> inner.begin -> body -> inner.end -> outer.end.
  for (size_t RegionIndex = 0; RegionIndex < Regions.size(); ++RegionIndex) {
    Region &R = Regions[RegionIndex];
    struct EntryEdge {
      llvm::BasicBlock *Pred = nullptr;
      llvm::BasicBlock *Target = nullptr;
      unsigned SuccessorIndex = 0;
    };
    llvm::SmallVector<EntryEdge, 8> Entries;
    bool EntryWithoutPred = false;
    llvm::BasicBlock *FunctionEntry = &LLVMFunc.getEntryBlock();
    for (llvm::BasicBlock *Target : R.Blocks) {
      bool HasNormalPred = false;
      for (llvm::BasicBlock *Pred : llvm::predecessors(Target)) {
        llvm::Instruction *Term = Pred->getTerminator();
        for (unsigned I = 0; I < Term->getNumSuccessors(); ++I) {
          if (Term->getSuccessor(I) != Target || !IsNormalSuccessor(Term, I))
            continue;
          HasNormalPred = true;
          if (!R.Blocks.count(Pred))
            Entries.push_back({Pred, Target, I});
        }
      }
      EntryWithoutPred |= Target == FunctionEntry && !HasNormalPred;
    }

    unsigned Marker = 0;
    for (const EntryEdge &Edge : Entries) {
      auto *BeginBB = llvm::BasicBlock::Create(
          *Ctx,
          "seh.try.begin." + std::to_string(RegionIndex) + "." +
              std::to_string(Marker++),
          &LLVMFunc, Edge.Target);
      Edge.Pred->getTerminator()->setSuccessor(Edge.SuccessorIndex, BeginBB);
      llvm::IRBuilder<> B(BeginBB);
      B.CreateInvoke(TryBegin, Edge.Target, R.UnwindDest);
      for (int Parent = R.Parent; Parent >= 0;
           Parent = Regions[static_cast<size_t>(Parent)].Parent)
        Regions[static_cast<size_t>(Parent)].Blocks.insert(BeginBB);
    }
    if (EntryWithoutPred) {
      auto *OldEntry = &LLVMFunc.getEntryBlock();
      auto *BeginBB = llvm::BasicBlock::Create(
          *Ctx, "seh.try.begin." + std::to_string(RegionIndex) + ".entry",
          &LLVMFunc, OldEntry);
      llvm::IRBuilder<> B(BeginBB);
      B.CreateInvoke(TryBegin, OldEntry, R.UnwindDest);
      for (int Parent = R.Parent; Parent >= 0;
           Parent = Regions[static_cast<size_t>(Parent)].Parent)
        Regions[static_cast<size_t>(Parent)].Blocks.insert(BeginBB);
    }

    struct ExitEdge {
      llvm::Instruction *Term = nullptr;
      llvm::BasicBlock *Target = nullptr;
      unsigned SuccessorIndex = 0;
    };
    llvm::SmallVector<ExitEdge, 8> Exits;
    llvm::SmallVector<llvm::ReturnInst *, 4> Returns;
    for (llvm::BasicBlock *Source : R.Blocks) {
      llvm::Instruction *Term = Source->getTerminator();
      if (auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(Term)) {
        Returns.push_back(Ret);
        continue;
      }
      for (unsigned I = 0; I < Term->getNumSuccessors(); ++I) {
        llvm::BasicBlock *Target = Term->getSuccessor(I);
        if (IsNormalSuccessor(Term, I) && !R.Blocks.count(Target))
          Exits.push_back({Term, Target, I});
      }
    }

    Marker = 0;
    for (const ExitEdge &Edge : Exits) {
      auto *EndBB = llvm::BasicBlock::Create(*Ctx,
                                             "seh.try.end." +
                                                 std::to_string(RegionIndex) +
                                                 "." + std::to_string(Marker++),
                                             &LLVMFunc, Edge.Target);
      Edge.Term->setSuccessor(Edge.SuccessorIndex, EndBB);
      llvm::IRBuilder<> B(EndBB);
      B.CreateInvoke(TryEnd, Edge.Target, R.UnwindDest);
      for (int Parent = R.Parent; Parent >= 0;
           Parent = Regions[static_cast<size_t>(Parent)].Parent)
        Regions[static_cast<size_t>(Parent)].Blocks.insert(EndBB);
    }
    for (llvm::ReturnInst *Ret : Returns) {
      llvm::BasicBlock *Source = Ret->getParent();
      auto *EndBB = llvm::BasicBlock::Create(
          *Ctx,
          "seh.try.end." + std::to_string(RegionIndex) + ".ret." +
              std::to_string(Marker++),
          &LLVMFunc);
      auto *RetBB = llvm::BasicBlock::Create(
          *Ctx, "seh.try.ret." + std::to_string(RegionIndex), &LLVMFunc);
      llvm::Value *ReturnValue = Ret->getReturnValue();
      Ret->eraseFromParent();
      llvm::IRBuilder<> SourceBuilder(Source);
      SourceBuilder.CreateBr(EndBB);
      llvm::IRBuilder<> EndBuilder(EndBB);
      EndBuilder.CreateInvoke(TryEnd, RetBB, R.UnwindDest);
      llvm::IRBuilder<> RetBuilder(RetBB);
      if (ReturnValue)
        RetBuilder.CreateRet(ReturnValue);
      else
        RetBuilder.CreateRetVoid();
      for (int Parent = R.Parent; Parent >= 0;
           Parent = Regions[static_cast<size_t>(Parent)].Parent)
        Regions[static_cast<size_t>(Parent)].Blocks.insert(EndBB);
    }
  }

  if (!Mod->getModuleFlag("eh-asynch"))
    Mod->addModuleFlag(llvm::Module::Warning, "eh-asynch", 1);
  LLVMFunc.setMetadata(
      windows_eh_md::NativeAttachment,
      llvm::MDNode::get(*Ctx, {mdUInt(*Ctx, 1, 1),
                               llvm::MDString::get(*Ctx, "seh-x64-native")}));
  return true;
}

bool MedLLVMEmitter::emitNativeCxxEH(
    const MedFunc &Func, llvm::Function &LLVMFunc,
    const std::map<int, llvm::BasicBlock *> &OriginalBlockMap) {
  if (TargetArch != Arch::X64 || TargetFormat != BinaryFormat::COFF ||
      !Func.ExceptionMetadata)
    return false;
  const ExceptionFunction &EH = *Func.ExceptionMetadata;
  if (EH.ParseStatus != ExceptionParseStatus::Complete ||
      EH.Personality != ExceptionPersonality::CxxFrameHandler3 || !EH.Cxx ||
      (EH.Encoding != ExceptionEncoding::X64UnwindV1 &&
       EH.Encoding != ExceptionEncoding::X64UnwindV2))
    return false;
  const CxxExceptionInfo &Cxx = *EH.Cxx;

  // This native closure is intentionally exact and narrow.  Destructors,
  // catch-object frame homes, noexcept, asynchronous /EHa, and out-of-line
  // catch funclets all need parent-frame rewriting; metadata-only IR is safer
  // until that proof is available.  Typed catches without a catch object are
  // representable because the RTTI address remains an external absolute data
  // symbol in the original image.
  //
  // A dynamic exception specification is excluded for a different reason: it
  // is not dispatch at all.  Escaping a `throw(A)` calls `unexpected` rather
  // than selecting a handler, and nothing in the LLVM WinEH model spells that,
  // so regenerating from this IR would silently drop the contract.  Only a
  // record whose magic declares `EHFlags` can be trusted about /EHs either, and
  // an older one leaves `IsSynchronous` unset, which the check above rejects.
  if (!Cxx.hasValidStateGraph() || Cxx.TryBlocks.empty() || Cxx.IPMap.empty() ||
      Cxx.IsCatchFunclet || Cxx.IsSeparated || !Cxx.IsSynchronous ||
      Cxx.IsNoExcept || Cxx.hasExceptionSpecification() ||
      (Cxx.Flags & ~uint32_t(1)) != 0)
    return false;
  for (const CxxUnwindAction &Action : Cxx.UnwindMap)
    if (Action.ActionVA != 0 ||
        Action.Kind != CxxUnwindAction::ActionKind::None)
      return false;

  struct Handler {
    const CxxCatchHandler *Catch = nullptr;
    llvm::BasicBlock *Target = nullptr;
  };
  struct Region {
    const CxxTryBlock *Try = nullptr;
    std::set<llvm::BasicBlock *> Blocks;
    std::vector<Handler> Handlers;
    llvm::BasicBlock *UnwindDest = nullptr;
    int Parent = -1;
  };

  auto StateAt = [&](va_t Address) {
    int32_t State = -1;
    for (const CxxIPState &Entry : Cxx.IPMap) {
      if (Entry.IP > Address)
        break;
      State = Entry.State;
    }
    return State;
  };
  auto BlockAt = [&](va_t Address) -> llvm::BasicBlock * {
    for (const MedBlock &Block : Func.Blocks) {
      if (Block.StartAddr != Address)
        continue;
      auto It = OriginalBlockMap.find(Block.Id);
      return It == OriginalBlockMap.end() ? nullptr : It->second;
    }
    return nullptr;
  };
  auto IsMayUnwindCall = [](const llvm::CallInst &Call) {
    return !Call.doesNotThrow() && !Call.isMustTailCall() &&
           !llvm::isa<llvm::IntrinsicInst>(Call);
  };

  // Every IP-state boundary must already be a machine-block boundary.  This
  // avoids assigning one generated call site to two native states.
  for (const MedBlock &Block : Func.Blocks) {
    if (Block.StartAddr >= Block.EndAddr)
      return false;
    for (const CxxIPState &Entry : Cxx.IPMap)
      if (Entry.IP > Block.StartAddr && Entry.IP < Block.EndAddr)
        return false;
  }

  std::vector<Region> Regions;
  Regions.reserve(Cxx.TryBlocks.size());
  for (const CxxTryBlock &Try : Cxx.TryBlocks) {
    if (Try.Handlers.empty())
      return false;
    Region R;
    R.Try = &Try;
    for (const MedBlock &Block : Func.Blocks) {
      int32_t State = StateAt(Block.StartAddr);
      if (State < Try.TryLow || State > Try.TryHigh)
        continue;
      auto It = OriginalBlockMap.find(Block.Id);
      if (It == OriginalBlockMap.end())
        return false;
      R.Blocks.insert(It->second);
    }
    if (R.Blocks.empty())
      return false;
    for (const CxxCatchHandler &Catch : Try.Handlers) {
      if (Catch.CatchObjectOffset != 0 || Catch.ParentFrameOffset != 0 ||
          Catch.HandlerVA == 0)
        return false;
      llvm::BasicBlock *Target = BlockAt(Catch.HandlerVA);
      if (!Target || Target == &LLVMFunc.getEntryBlock() ||
          R.Blocks.count(Target) || !llvm::pred_empty(Target))
        return false;
      R.Handlers.push_back({&Catch, Target});
    }
    Regions.push_back(std::move(R));
  }

  auto IsSubset = [](const std::set<llvm::BasicBlock *> &A,
                     const std::set<llvm::BasicBlock *> &B) {
    return std::includes(B.begin(), B.end(), A.begin(), A.end());
  };
  auto Overlaps = [](const std::set<llvm::BasicBlock *> &A,
                     const std::set<llvm::BasicBlock *> &B) {
    for (llvm::BasicBlock *Block : A)
      if (B.count(Block))
        return true;
    return false;
  };
  for (size_t I = 0; I < Regions.size(); ++I) {
    for (size_t J = I + 1; J < Regions.size(); ++J) {
      if (!Overlaps(Regions[I].Blocks, Regions[J].Blocks))
        continue;
      if (Regions[I].Blocks == Regions[J].Blocks ||
          (!IsSubset(Regions[I].Blocks, Regions[J].Blocks) &&
           !IsSubset(Regions[J].Blocks, Regions[I].Blocks)))
        return false;
    }
  }
  std::stable_sort(Regions.begin(), Regions.end(),
                   [](const Region &A, const Region &B) {
                     return A.Blocks.size() > B.Blocks.size();
                   });
  for (size_t I = 0; I < Regions.size(); ++I) {
    size_t ParentSize = std::numeric_limits<size_t>::max();
    for (size_t J = 0; J < I; ++J) {
      if (!IsSubset(Regions[I].Blocks, Regions[J].Blocks) ||
          Regions[J].Blocks.size() >= ParentSize)
        continue;
      Regions[I].Parent = static_cast<int>(J);
      ParentSize = Regions[J].Blocks.size();
    }
  }

  // Catch bodies in this closure execute after catchret.  They must therefore
  // be ordinary, call-free continuation blocks and cannot themselves be in a
  // protected region.  This is equivalent for simple catch bodies and avoids
  // pretending an out-of-line native funclet has the regenerated frame ABI.
  for (const Region &R : Regions)
    for (const Handler &H : R.Handlers) {
      for (const Region &Protected : Regions)
        if (Protected.Blocks.count(H.Target))
          return false;
      for (const llvm::Instruction &Inst : *H.Target)
        if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Inst);
            Call && IsMayUnwindCall(*Call))
          return false;
    }

  std::set<const llvm::CallInst *> MayUnwindCallSet;
  for (const Region &R : Regions)
    for (llvm::BasicBlock *Block : R.Blocks)
      for (const llvm::Instruction &Inst : *Block)
        if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Inst);
            Call && IsMayUnwindCall(*Call))
          MayUnwindCallSet.insert(Call);
  size_t MayUnwindCalls = MayUnwindCallSet.size();
  if (MayUnwindCalls == 0)
    return false;

  auto *I8Ty = llvm::Type::getInt8Ty(*Ctx);
  auto *I32Ty = llvm::Type::getInt32Ty(*Ctx);
  auto *PtrTy = llvm::PointerType::get(*Ctx, 0);
  auto *PersonalityTy = llvm::FunctionType::get(I32Ty, {}, true);
  llvm::FunctionCallee Personality =
      Mod->getOrInsertFunction("__CxxFrameHandler3", PersonalityTy);
  LLVMFunc.setPersonalityFn(
      llvm::cast<llvm::Constant>(Personality.getCallee()));

  auto TypeDescriptor = [&](va_t Address) -> llvm::Constant * {
    if (Address == 0)
      return llvm::ConstantPointerNull::get(PtrTy);
    std::string Name = makeNdDataSymbol(Address);
    llvm::GlobalVariable *GV = Mod->getNamedGlobal(Name);
    if (!GV)
      GV = new llvm::GlobalVariable(*Mod, I8Ty, /*isConstant=*/true,
                                    llvm::GlobalValue::ExternalLinkage, nullptr,
                                    Name);
    return GV;
  };

  auto *TokenNone = llvm::ConstantTokenNone::get(*Ctx);
  for (size_t I = 0; I < Regions.size(); ++I) {
    Region &R = Regions[I];
    llvm::BasicBlock *ParentDest =
        R.Parent >= 0 ? Regions[static_cast<size_t>(R.Parent)].UnwindDest
                      : nullptr;
    auto *Dispatch = llvm::BasicBlock::Create(
        *Ctx, "cxx.catch.dispatch." + std::to_string(I), &LLVMFunc);
    llvm::IRBuilder<> DB(Dispatch);
    auto *Switch = DB.CreateCatchSwitch(TokenNone, ParentDest,
                                        R.Handlers.size(), "cxx.catch.switch");
    for (size_t J = 0; J < R.Handlers.size(); ++J) {
      const Handler &H = R.Handlers[J];
      auto *PadBB = llvm::BasicBlock::Create(
          *Ctx, "cxx.catch.pad." + std::to_string(I) + "." + std::to_string(J),
          &LLVMFunc);
      Switch->addHandler(PadBB);
      llvm::IRBuilder<> PB(PadBB);
      llvm::SmallVector<llvm::Value *, 3> Args{
          TypeDescriptor(H.Catch->TypeDescriptorVA),
          llvm::ConstantInt::get(I32Ty, H.Catch->Adjectives),
          llvm::ConstantPointerNull::get(PtrTy)};
      auto *Pad = PB.CreateCatchPad(Switch, Args, "cxx.catch.pad.token");
      PB.CreateCatchRet(Pad, H.Target);
    }
    R.UnwindDest = Dispatch;
  }

  size_t ConvertedCalls = 0;
  for (const MedBlock &MedBB : Func.Blocks) {
    auto BBIt = OriginalBlockMap.find(MedBB.Id);
    if (BBIt == OriginalBlockMap.end())
      continue;
    llvm::BasicBlock *InitialBB = BBIt->second;
    int Innermost = -1;
    size_t InnermostSize = std::numeric_limits<size_t>::max();
    for (size_t I = 0; I < Regions.size(); ++I) {
      if (!Regions[I].Blocks.count(InitialBB) ||
          Regions[I].Blocks.size() >= InnermostSize)
        continue;
      Innermost = static_cast<int>(I);
      InnermostSize = Regions[I].Blocks.size();
    }
    if (Innermost < 0)
      continue;

    llvm::SmallVector<llvm::CallInst *, 8> Calls;
    for (llvm::Instruction &Inst : *InitialBB)
      if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Inst);
          Call && IsMayUnwindCall(*Call))
        Calls.push_back(Call);
    for (llvm::CallInst *Call : Calls) {
      llvm::BasicBlock *CallBB = Call->getParent();
      llvm::Instruction *Next = Call->getNextNode();
      if (!Next)
        return false;
      llvm::BasicBlock *Cont =
          CallBB->splitBasicBlock(Next, CallBB->getName() + ".cxx.cont");
      llvm::Instruction *OldBranch = CallBB->getTerminator();
      llvm::SmallVector<llvm::Value *, 8> Args;
      for (llvm::Use &Arg : Call->args())
        Args.push_back(Arg.get());
      llvm::SmallVector<llvm::OperandBundleDef, 2> Bundles;
      Call->getOperandBundlesAsDefs(Bundles);
      auto *Invoke = llvm::InvokeInst::Create(
          Call->getFunctionType(), Call->getCalledOperand(), Cont,
          Regions[static_cast<size_t>(Innermost)].UnwindDest, Args, Bundles,
          Call->getName(), OldBranch->getIterator());
      Invoke->setCallingConv(Call->getCallingConv());
      Invoke->setAttributes(Call->getAttributes());
      Invoke->setDebugLoc(Call->getDebugLoc());
      Invoke->copyMetadata(*Call);
      Call->replaceAllUsesWith(Invoke);
      Call->eraseFromParent();
      OldBranch->eraseFromParent();
      for (Region &Protected : Regions)
        if (Protected.Blocks.count(InitialBB))
          Protected.Blocks.insert(Cont);
      ++ConvertedCalls;
    }
  }
  if (ConvertedCalls != MayUnwindCalls)
    return false;

  LLVMFunc.setMetadata(
      windows_eh_md::NativeAttachment,
      llvm::MDNode::get(*Ctx, {mdUInt(*Ctx, 1, 1),
                               llvm::MDString::get(*Ctx, "cxx-fh3-native")}));
  return true;
}

namespace {

/// The canonical symbol for each Itanium personality NeverD can lower.  The
/// SJLJ personality is deliberately absent: its call-site table indexes call
/// sites rather than addresses, so nothing in it names code an `invoke` could
/// protect, and `IsCallSiteAddressForm` already reports that.
const char *itaniumPersonalitySymbol(ExceptionPersonality P) {
  switch (P) {
  case ExceptionPersonality::GxxPersonalityV0:
    return "__gxx_personality_v0";
  case ExceptionPersonality::GxxPersonalitySEH0:
    return "__gxx_personality_seh0";
  case ExceptionPersonality::GccPersonalityV0:
    return "__gcc_personality_v0";
  case ExceptionPersonality::ObjCPersonalityV0:
    return "__objc_personality_v0";
  case ExceptionPersonality::RustEhPersonality:
    return "rust_eh_personality";
  default:
    return nullptr;
  }
}

} // namespace

bool MedLLVMEmitter::emitNativeItaniumEH(
    const MedFunc &Func, llvm::Function &LLVMFunc,
    const std::map<int, llvm::BasicBlock *> &OriginalBlockMap) {
  if (!Func.ExceptionMetadata)
    return false;
  const ExceptionFunction &EH = *Func.ExceptionMetadata;
  const char *PersonalitySymbol = itaniumPersonalitySymbol(EH.Personality);
  if (!PersonalitySymbol || !EH.Itanium ||
      !EH.Itanium->IsCallSiteAddressForm ||
      EH.ParseStatus != ExceptionParseStatus::Complete)
    return false;
  const ItaniumEHInfo &Itanium = *EH.Itanium;

  auto *PtrTy = llvm::PointerType::get(*Ctx, 0);
  auto *I8Ty = llvm::Type::getInt8Ty(*Ctx);
  auto *I32Ty = llvm::Type::getInt32Ty(*Ctx);

  // A type-table slot resolves to the RTTI object the catch matches.  The
  // mangled name is used when the decoder could read it, because that is the
  // identity a rebuilt object would link against; an address-derived symbol is
  // the fallback, and a null pointer is what the ABI itself spells for a
  // catch-all.
  auto TypeInfoConstant = [&](uint64_t Index) -> llvm::Constant * {
    const ItaniumTypeEntry *Entry = nullptr;
    for (const ItaniumTypeEntry &Candidate : Itanium.TypeTable)
      if (Candidate.Index == Index) {
        Entry = &Candidate;
        break;
      }
    if (!Entry)
      return nullptr;
    if (Entry->IsCatchAll || Entry->TypeInfoVA == 0)
      return llvm::ConstantPointerNull::get(PtrTy);
    std::string Name = Entry->TypeName.empty()
                           ? makeNdDataSymbol(Entry->TypeInfoVA)
                           : Entry->TypeName;
    llvm::GlobalVariable *GV = Mod->getNamedGlobal(Name);
    if (!GV)
      GV = new llvm::GlobalVariable(*Mod, I8Ty, /*isConstant=*/true,
                                    llvm::GlobalValue::ExternalLinkage, nullptr,
                                    Name);
    return GV;
  };

  struct Pad {
    llvm::BasicBlock *Block = nullptr;
    bool Cleanup = false;
    std::vector<llvm::Constant *> Clauses;
    bool Usable = false;
  };
  std::map<va_t, Pad> Pads;

  auto BlockAt = [&](va_t Address) -> llvm::BasicBlock * {
    for (const MedBlock &Block : Func.Blocks) {
      if (Block.StartAddr != Address)
        continue;
      auto It = OriginalBlockMap.find(Block.Id);
      return It == OriginalBlockMap.end() ? nullptr : It->second;
    }
    return nullptr;
  };
  auto FindAction = [&](uint64_t Offset) -> const ItaniumAction * {
    for (const ItaniumAction &Action : Itanium.Actions)
      if (Action.TableOffset == Offset)
        return &Action;
    return nullptr;
  };
  auto AddClause = [](Pad &P, llvm::Constant *Clause) {
    if (std::find(P.Clauses.begin(), P.Clauses.end(), Clause) ==
        P.Clauses.end())
      P.Clauses.push_back(Clause);
  };

  // Collect what each pad has to select on.  Several call sites can share a
  // pad, and the clauses they name accumulate on the one `landingpad` the pad
  // block gets.
  bool TableFullyRead = true;
  for (const ItaniumCallSite &Site : Itanium.CallSites) {
    if (Site.LandingPadVA == 0)
      continue;
    Pad &P = Pads[Site.LandingPadVA];
    if (!Site.FirstActionOffset) {
      // The ABI defines a pad with no action record as an unconditional
      // cleanup, which is the shape of every destructor-only frame and of
      // every Rust drop-glue pad.
      P.Cleanup = true;
      continue;
    }
    std::optional<uint64_t> Offset = Site.FirstActionOffset;
    // A step budget of the action count terminates a cycle without being able
    // to cut a well-formed chain short.
    for (size_t Step = 0; Offset && Step <= Itanium.Actions.size(); ++Step) {
      const ItaniumAction *Action = FindAction(*Offset);
      if (!Action) {
        TableFullyRead = false;
        break;
      }
      if (Action->isCleanup()) {
        P.Cleanup = true;
      } else if (Action->isCatch()) {
        llvm::Constant *Info =
            TypeInfoConstant(static_cast<uint64_t>(Action->TypeFilter));
        if (!Info)
          TableFullyRead = false;
        else
          AddClause(P, Info);
      } else {
        const ItaniumExceptionSpec *Spec = nullptr;
        for (const ItaniumExceptionSpec &Candidate : Itanium.ExceptionSpecs)
          if (Candidate.Index == static_cast<uint64_t>(-Action->TypeFilter)) {
            Spec = &Candidate;
            break;
          }
        if (!Spec) {
          TableFullyRead = false;
        } else {
          // A filter clause is an array of the types the specification
          // permits; the empty array is `noexcept`, which permits none.
          std::vector<llvm::Constant *> Permitted;
          for (uint64_t Index : Spec->TypeIndices) {
            llvm::Constant *Info = TypeInfoConstant(Index);
            if (!Info) {
              TableFullyRead = false;
              break;
            }
            Permitted.push_back(Info);
          }
          if (Permitted.size() == Spec->TypeIndices.size())
            AddClause(P, llvm::ConstantArray::get(
                             llvm::ArrayType::get(PtrTy, Permitted.size()),
                             Permitted));
        }
      }
      Offset = Action->NextActionOffset;
    }
  }
  // An action this decoder could not resolve leaves the pad's clause list
  // short, and a short clause list does not merely describe less — it says the
  // pad selects on fewer types than it does.  Nothing here is emitted from a
  // table that could not be read through.
  if (Pads.empty() || !TableFullyRead)
    return false;

  // A pad is lowerable only where LLVM's landing-pad model holds: the block
  // must start exactly at the pad, must not be the entry, and must be reached
  // by nothing but an unwind edge.  A pad that fails any of these keeps its
  // calls as plain calls rather than producing IR the verifier would reject.
  size_t UsablePads = 0;
  for (auto &[PadVA, P] : Pads) {
    P.Block = BlockAt(PadVA);
    if (!P.Block || P.Block == &LLVMFunc.getEntryBlock() ||
        !llvm::pred_empty(P.Block) || P.Block->isLandingPad())
      continue;
    // Every pad the ABI can enter runs at least cleanup; a pad that named no
    // clause and no cleanup would be a `landingpad` LLVM rejects.
    if (P.Clauses.empty())
      P.Cleanup = true;
    P.Usable = true;
    ++UsablePads;
  }
  if (UsablePads == 0)
    return false;

  // Match each emitted call to the innermost call-site range that covers the
  // address it came from.  The ranges a compiler emits do not overlap, so the
  // innermost test only ever disambiguates a table this decoder read loosely.
  auto PadForCall = [&](va_t Address) -> Pad * {
    Pad *Best = nullptr;
    uint64_t BestSize = std::numeric_limits<uint64_t>::max();
    for (const ItaniumCallSite &Site : Itanium.CallSites) {
      if (Site.LandingPadVA == 0 || !Site.GuardedRange.contains(Address) ||
          Site.GuardedRange.size() >= BestSize)
        continue;
      auto It = Pads.find(Site.LandingPadVA);
      if (It == Pads.end() || !It->second.Usable)
        continue;
      Best = &It->second;
      BestSize = Site.GuardedRange.size();
    }
    return Best;
  };

  size_t LoweredCalls = 0;
  for (const MedBlock &MedBB : Func.Blocks) {
    auto BBIt = OriginalBlockMap.find(MedBB.Id);
    if (BBIt == OriginalBlockMap.end())
      continue;
    llvm::SmallVector<llvm::CallInst *, 8> Calls;
    for (llvm::Instruction &Inst : *BBIt->second)
      if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Inst))
        if (!Call->doesNotThrow() && !Call->isMustTailCall() &&
            !llvm::isa<llvm::IntrinsicInst>(Call))
          Calls.push_back(Call);

    for (llvm::CallInst *Call : Calls) {
      auto AddrIt = CallSiteAddrs.find(Call);
      if (AddrIt == CallSiteAddrs.end())
        continue;
      Pad *Target = PadForCall(AddrIt->second);
      if (!Target)
        continue;
      llvm::Instruction *Next = Call->getNextNode();
      if (!Next)
        continue;
      llvm::BasicBlock *CallBB = Call->getParent();
      llvm::BasicBlock *Cont =
          CallBB->splitBasicBlock(Next, CallBB->getName() + ".eh.cont");
      llvm::Instruction *OldBranch = CallBB->getTerminator();
      llvm::SmallVector<llvm::Value *, 8> Args;
      for (llvm::Use &Arg : Call->args())
        Args.push_back(Arg.get());
      llvm::SmallVector<llvm::OperandBundleDef, 2> Bundles;
      Call->getOperandBundlesAsDefs(Bundles);
      auto *Invoke = llvm::InvokeInst::Create(
          Call->getFunctionType(), Call->getCalledOperand(), Cont,
          Target->Block, Args, Bundles, Call->getName(),
          OldBranch->getIterator());
      Invoke->setCallingConv(Call->getCallingConv());
      Invoke->setAttributes(Call->getAttributes());
      Invoke->setDebugLoc(Call->getDebugLoc());
      Invoke->copyMetadata(*Call);
      Call->replaceAllUsesWith(Invoke);
      CallSiteAddrs.erase(Call);
      Call->eraseFromParent();
      OldBranch->eraseFromParent();
      ++LoweredCalls;
    }
  }
  if (LoweredCalls == 0)
    return false;

  auto *PersonalityTy = llvm::FunctionType::get(I32Ty, {}, true);
  llvm::FunctionCallee Personality =
      Mod->getOrInsertFunction(PersonalitySymbol, PersonalityTy);
  LLVMFunc.setPersonalityFn(
      llvm::cast<llvm::Constant>(Personality.getCallee()));

  // The pad's `landingpad` has to precede the recovered body, because LLVM
  // requires it to be the block's first non-PHI instruction and because the
  // unwinder has already run by the time that body executes.
  auto *ResultTy = llvm::StructType::get(PtrTy, I32Ty);
  size_t LoweredPads = 0;
  for (auto &[PadVA, P] : Pads) {
    if (!P.Usable || llvm::pred_empty(P.Block))
      continue;
    llvm::IRBuilder<> B(P.Block, P.Block->begin());
    auto *LP = B.CreateLandingPad(ResultTy, P.Clauses.size(), "eh.lpad");
    LP->setCleanup(P.Cleanup);
    for (llvm::Constant *Clause : P.Clauses)
      LP->addClause(Clause);
    ++LoweredPads;
  }
  if (LoweredPads == 0) {
    // No invoke reached a pad after all, which leaves a personality on a
    // function that has no landing pad.  Undo it rather than describe a
    // dispatch that is not there.
    LLVMFunc.setPersonalityFn(nullptr);
    return false;
  }

  LLVMFunc.setMetadata(
      language_eh_md::ItaniumAttachment,
      llvm::MDNode::get(*Ctx,
                        {llvm::MDString::get(*Ctx, PersonalitySymbol),
                         mdUInt(*Ctx, 32, LoweredPads),
                         mdUInt(*Ctx, 32, Pads.size() - LoweredPads),
                         mdUInt(*Ctx, 32, LoweredCalls)}));
  return true;
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
    for (auto &Blk : Func.Blocks) {
      for (auto &Phi : Blk.Phis) {
        for (auto &[PredId, Var] : Phi.Args) {
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
