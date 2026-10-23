//===- MedLLVMCall.cpp - CALL/INDIR_CALL lowering ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Direct/indirect CALL lowering: libc/libm signature reconstruction, argument
/// coercion, and small-struct-return flattening.
///
//===----------------------------------------------------------------------===//

#include "neverd/Common.h"
#include "neverd/backend/LLVMValueProvenance.h"
#include "neverd/backend/Limits.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/LLVMCallContract.h"
#include "neverd/backend/llvm/LLVMFloatingCall.h"
#include "neverd/backend/llvm/LLVMName.h"
#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/backend/llvm/LLVMSourceTypeHints.h"
#include "neverd/backend/llvm/LanguageEHMetadata.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/PEImportShadow.h"
#include "neverd/backend/llvm/SafetyCallsiteMetadata.h"
#include "neverd/backend/llvm/X86X87ImportCall.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/low/X87ImportCall.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/libc/LibCFloatingArguments.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/libc/LibCVariadicArguments.h"
#include "neverd/loader/SymbolDecoration.h"
#include "neverd/object/SectionNames.h"
#include "neverd/safety/CountedWriteSemantics.h"
#include "neverd/support/Diagnostic.h"

#define DEBUG_TYPE "neverd-med-llvm-call"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/Loads.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace neverd {

namespace {
constexpr auto ArgumentBanks = LLVMArgumentBanksAttribute;

/// A fixed header signature whose floating operands already have complete
/// register summaries. SourceParameterPlacement owns the positions; this only
/// verifies that ABI recovery retained those exact operands and scalar lanes.
bool floatingHeaderCall(const FunctionSym &Function, Arch Architecture,
                        BinaryFormat Format, const MedOp &Op,
                        const MedCallInfo &Call) {
  const auto Signature =
      libc::floatingCallArguments(Function, Architecture, Format);
  const auto &Registers = getTargetRegInfo(Architecture);
  if (Signature && (Architecture == Arch::X86 || !Signature->Scalars.empty() ||
                    Signature->ReturnElements)) {
    const auto Recovered =
        recoveredFloatingCallArguments(Op, Call, Architecture, Format);
    return Recovered && *Recovered == *Signature;
  }
  if (!Signature || Signature->ParameterBytes.size() != Call.Args.size() ||
      Op.NumInputs != Call.Args.size() + 1 || Op.Output.Kind != MedVar::Reg ||
      Op.Output.RegOff != Registers.FPReturnReg ||
      Op.Output.Size < Signature->ReturnBytes)
    return false;
  for (size_t I = 0; I < Signature->ParameterBytes.size(); ++I)
    if (Op.vectorArgumentWidth(I + 1) != Signature->ParameterBytes[I] ||
        !(Call.Args[I] == Op.Inputs[I + 1]))
      return false;
  return true;
}

std::optional<std::pair<unsigned, unsigned>>
argumentBanks(const llvm::Function &Function) {
  const auto Attribute = Function.getFnAttribute(ArgumentBanks);
  if (!Attribute.isStringAttribute())
    return std::nullopt;
  auto Parts = Attribute.getValueAsString().split(',');
  unsigned Integers, Floating;
  if (Parts.first.getAsInteger(10, Integers) ||
      Parts.second.getAsInteger(10, Floating) ||
      Integers > Function.arg_size() ||
      Floating > Function.arg_size() - Integers)
    return std::nullopt;
  return std::pair{Integers, Floating};
}

std::optional<std::pair<unsigned, unsigned>>
argumentBanks(const llvm::CallBase &Call) {
  const auto *Node = Call.getMetadata(ArgumentBanks);
  if (!Node || Node->getNumOperands() < 2)
    return std::nullopt;
  const auto *Integers =
      llvm::mdconst::dyn_extract<llvm::ConstantInt>(Node->getOperand(0));
  const auto *Floating =
      llvm::mdconst::dyn_extract<llvm::ConstantInt>(Node->getOperand(1));
  if (!Integers || !Floating || Integers->getBitWidth() != 32 ||
      Floating->getBitWidth() != 32 ||
      Integers->getZExtValue() > Call.arg_size() ||
      Floating->getZExtValue() > Call.arg_size() - Integers->getZExtValue())
    return std::nullopt;
  return std::pair{static_cast<unsigned>(Integers->getZExtValue()),
                   static_cast<unsigned>(Floating->getZExtValue())};
}

/// The slot size and stack value offsets a call records after its banks.
struct ArgumentStackLayout {
  std::vector<int64_t> Offsets;
  unsigned SlotBytes = 0;
};

ArgumentStackLayout argumentStackLayout(const llvm::CallBase &Call) {
  const auto *Node = Call.getMetadata(ArgumentBanks);
  if (!Node || Node->getNumOperands() < 3)
    return {};
  const auto *Slot =
      llvm::mdconst::dyn_extract<llvm::ConstantInt>(Node->getOperand(2));
  if (!Slot || Slot->getBitWidth() != 32)
    return {};
  ArgumentStackLayout Result;
  for (unsigned I = 3; I < Node->getNumOperands(); ++I) {
    const auto *Offset =
        llvm::mdconst::dyn_extract<llvm::ConstantInt>(Node->getOperand(I));
    if (!Offset || Offset->getBitWidth() != 64)
      return {};
    Result.Offsets.push_back(Offset->getSExtValue());
  }
  Result.SlotBytes = static_cast<unsigned>(Slot->getZExtValue());
  return Result;
}

llvm::GlobalValue *constantCallIdentity(llvm::Value *Value,
                                        const llvm::DataLayout &Layout,
                                        unsigned PointerBits) {
  // A source indirect call can already name one rebuilt function, directly
  // or through immutable relocation storage. Reuse that function's recovered
  // signature before emitting the call. Otherwise the generic register
  // signature can survive as an opaque-pointer call with extra parameters
  // even after LLVM folds the pointer to the function itself.
  for (unsigned Depth = 0;
       Value &&
       Depth != ::neverd::limits::backend::kMaxConstantCallIdentityDepth;
       ++Depth) {
    if (auto *Global = llvm::dyn_cast<llvm::GlobalValue>(Value))
      return Global;
    if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(Value)) {
      if (Load->isAtomic())
        return nullptr;
      if (auto *Pointer =
              llvm::dyn_cast<llvm::Constant>(Load->getPointerOperand())) {
        // The emitter marks image loads volatile to retain their observation.
        // An immutable private mirror still fixes their value. Keep the load
        // itself; only its call target gains the already-proved identity.
        Value = llvm::ConstantFoldLoadFromConstPtr(Pointer, Load->getType(),
                                                   Layout);
      } else if (Load->isSimple() &&
                 llvm::isa<llvm::AllocaInst>(Load->getPointerOperand())) {
        // Before promotion, MedIR SSA values use local allocas. Use LLVM's
        // bounded store-to-load proof, not an arbitrary defining store.
        auto From = Load->getIterator();
        Value =
            llvm::FindAvailableLoadedValue(Load, Load->getParent(), From, 32);
      } else
        return nullptr;
      continue;
    }
    const auto *Cast = llvm::dyn_cast<llvm::Operator>(Value);
    if (!Cast || (Cast->getOpcode() != llvm::Instruction::PtrToInt &&
                  Cast->getOpcode() != llvm::Instruction::IntToPtr &&
                  Cast->getOpcode() != llvm::Instruction::BitCast))
      return nullptr;
    llvm::Type *Integer = Cast->getOpcode() == llvm::Instruction::PtrToInt
                              ? Cast->getType()
                              : Cast->getOperand(0)->getType();
    if (Cast->getOpcode() != llvm::Instruction::BitCast &&
        !Integer->isIntegerTy(PointerBits))
      return nullptr;
    Value = Cast->getOperand(0);
  }
  return nullptr;
}
} // namespace

void setLLVMArgumentBanks(llvm::Function &Function, unsigned Integers,
                          unsigned Floating) {
  Function.addFnAttr(ArgumentBanks,
                     std::to_string(Integers) + "," + std::to_string(Floating));
}

void setLLVMArgumentBanks(llvm::CallBase &Call, unsigned Integers,
                          unsigned Floating,
                          llvm::ArrayRef<int64_t> StackOffsets,
                          unsigned SlotBytes) {
  llvm::IRBuilder<> Builder(Call.getContext());
  llvm::SmallVector<llvm::Metadata *> Operands = {
      llvm::ConstantAsMetadata::get(Builder.getInt32(Integers)),
      llvm::ConstantAsMetadata::get(Builder.getInt32(Floating))};
  if (!StackOffsets.empty() && SlotBytes > 0) {
    Operands.push_back(
        llvm::ConstantAsMetadata::get(Builder.getInt32(SlotBytes)));
    for (int64_t Offset : StackOffsets)
      Operands.push_back(
          llvm::ConstantAsMetadata::get(Builder.getInt64(Offset)));
  }
  Call.setMetadata(ArgumentBanks,
                   llvm::MDNode::get(Call.getContext(), Operands));
}

std::optional<LLVMArgumentBinding> bindResolvedLLVMArguments(
    const llvm::Function &Target, llvm::ArrayRef<llvm::Value *> Values,
    unsigned Integers, unsigned Floating, llvm::IRBuilderBase &Builder,
    llvm::ArrayRef<int64_t> StackOffsets, unsigned SlotBytes) {
  const auto Banks = argumentBanks(Target);
  const unsigned Count = Values.size();
  const unsigned Fixed = Target.arg_size();
  if (!Banks || Target.isVarArg() || Integers > Count ||
      Floating > Count - Integers)
    return std::nullopt;
  const auto [WantedIntegers, WantedFloating] = *Banks;
  if (WantedIntegers > Integers || WantedFloating > Floating)
    return std::nullopt;
  LLVMArgumentBinding Binding;
  auto Append = [&](unsigned I) {
    Binding.Values.push_back(Values[I]);
    Binding.SourceIndices.push_back(I);
  };
  for (unsigned I = 0; I < WantedIntegers; ++I)
    Append(I);
  for (unsigned I = 0; I < WantedFloating; ++I)
    Append(Integers + I);

  // Stack recovery records where each value was stored. Its stores need not
  // have the callee's load widths: Darwin packs two overflow floats into one
  // eight-byte incoming slot, and an int stored into an eight-byte System V
  // slot leaves the rest of the slot as padding. Bind bytes, not argument
  // counts, and never fill an unproved missing byte with zero.
  const auto &Layout = Target.getParent()->getDataLayout();
  auto Bits = [&](llvm::Type *Type) -> unsigned {
    if (!Type->isSingleValueType() || !Type->isSized())
      return 0;
    const auto Size = Layout.getTypeSizeInBits(Type);
    return Size.isScalable() ? 0 : Size.getFixedValue();
  };
  // Each stack value's bits from the first stack argument: its own, then the
  // padding it owns to the end of its last slot or to the next value. Without
  // recorded offsets the values are one contiguous run.
  struct Placed {
    unsigned Index;
    uint64_t Begin, DataEnd, End;
  };
  std::vector<Placed> Stack;
  const bool Located = !StackOffsets.empty() && SlotBytes > 0;
  for (unsigned I = Integers + Floating, Next = 0; I < Count; ++I) {
    const unsigned Size = Bits(Values[I]->getType());
    if (!Located) {
      // The run ends at a value without a size.
      if (!Size)
        break;
      Stack.push_back({I, Next, Next + Size, Next + Size});
      Next += Size;
      continue;
    }
    const unsigned K = I - Integers - Floating;
    if (K >= StackOffsets.size() || StackOffsets[K] < 0 || !Size)
      continue;
    const uint64_t Begin = static_cast<uint64_t>(StackOffsets[K]) * 8;
    const uint64_t Slot = uint64_t{SlotBytes} * 8;
    Stack.push_back(
        {I, Begin, Begin + Size, (Begin + Size + Slot - 1) / Slot * Slot});
  }
  llvm::stable_sort(Stack, [](const Placed &A, const Placed &B) {
    return A.Begin < B.Begin;
  });
  for (size_t I = 0; I + 1 < Stack.size(); ++I) {
    if (Stack[I].DataEnd > Stack[I + 1].Begin)
      return std::nullopt;
    Stack[I].End = std::min(Stack[I].End, Stack[I + 1].Begin);
  }
  struct Slice {
    unsigned Index, Offset, Bits, Shift;
  };
  std::vector<std::vector<Slice>> Slots;
  uint64_t Param = 0;
  for (unsigned I = WantedIntegers + WantedFloating; I < Fixed; ++I) {
    const unsigned Size = Bits(Target.getFunctionType()->getParamType(I));
    if (!Size)
      return std::nullopt;
    const uint64_t End = Param + Size;
    std::vector<Slice> Slices;
    uint64_t Cursor = Param;
    for (const Placed &Value : Stack) {
      if (Cursor == End)
        break;
      if (Value.End <= Cursor)
        continue;
      if (Value.Begin > Cursor)
        break;
      const uint64_t Stop = std::min(End, Value.End);
      if (const uint64_t Data = std::min(Stop, Value.DataEnd); Cursor < Data)
        Slices.push_back({Value.Index,
                          static_cast<unsigned>(Cursor - Value.Begin),
                          static_cast<unsigned>(Data - Cursor),
                          static_cast<unsigned>(Cursor - Param)});
      Cursor = Stop;
    }
    if (Cursor != End)
      return std::nullopt;
    Slots.push_back(std::move(Slices));
    Param = End;
  }
  if (!Slots.empty() && !Layout.isLittleEndian())
    return std::nullopt;
  llvm::IRBuilder<> Pack(Builder.getContext());
  Pack.restoreIP(Builder.saveIP());
  for (const auto &Slices : Slots) {
    auto *Wanted =
        Target.getFunctionType()->getParamType(Binding.Values.size());
    const bool Whole =
        Slices.size() == 1 && Slices[0].Offset == 0 && Slices[0].Shift == 0 &&
        Slices[0].Bits == Bits(Values[Slices[0].Index]->getType()) &&
        Slices[0].Bits == Bits(Wanted);
    if (Whole && Values[Slices[0].Index]->getType() == Wanted) {
      Append(Slices[0].Index);
      continue;
    }
    // Padding bits pass as zero: the callee's slot holds no value there.
    auto *Integer = Pack.getIntNTy(Bits(Wanted));
    llvm::Value *Packed = llvm::ConstantInt::get(Integer, 0);
    for (const auto &Slice : Slices) {
      auto *Value = Values[Slice.Index];
      auto *SourceInteger = Pack.getIntNTy(Bits(Value->getType()));
      if (Value->getType()->isPointerTy())
        Value = Pack.CreatePtrToInt(Value, SourceInteger);
      else if (!Value->getType()->isIntegerTy())
        Value = Pack.CreateBitCast(Value, SourceInteger);
      if (Slice.Offset)
        Value = Pack.CreateLShr(Value, Slice.Offset);
      Value = Pack.CreateZExtOrTrunc(Value, Pack.getIntNTy(Slice.Bits));
      Value = Pack.CreateZExtOrTrunc(Value, Integer);
      if (Slice.Shift)
        Value = Pack.CreateShl(Value, Slice.Shift);
      Packed = Pack.CreateOr(Packed, Value);
    }
    if (Wanted->isPointerTy())
      Packed = Pack.CreateIntToPtr(Packed, Wanted);
    else if (Packed->getType() != Wanted)
      Packed = Pack.CreateBitCast(Packed, Wanted);
    Binding.Values.push_back(Packed);
    Binding.SourceIndices.push_back(Whole ? int(Slices[0].Index) : -1);
  }
  return Binding;
}

bool normalizeResolvedLLVMCalls(llvm::Module &Module, LLVMSourceMap *Sources,
                                bool AnalystSourceProjection) {
  const auto &Layout = Module.getDataLayout();
  const unsigned PointerBits = Layout.getPointerSizeInBits();
  auto Compatible = [&](llvm::Type *From, llvm::Type *To) {
    if (From == To)
      return true;
    if (From->isIntegerTy() && To->isIntegerTy())
      return true;
    auto PointerInteger = [&](llvm::Type *P, llvm::Type *I) {
      return P->isPointerTy() && P->getPointerAddressSpace() == 0 &&
             I->isIntegerTy(PointerBits);
    };
    return PointerInteger(From, To) || PointerInteger(To, From);
  };
  auto Convert = [](llvm::IRBuilder<> &B, llvm::Value *V, llvm::Type *T) {
    if (V->getType() == T)
      return V;
    if (V->getType()->isPointerTy())
      return B.CreatePtrToInt(V, T);
    if (T->isPointerTy())
      return B.CreateIntToPtr(V, T);
    return B.CreateZExtOrTrunc(V, T);
  };
  std::vector<llvm::CallBase *> Calls;
  for (auto &F : Module)
    for (auto &B : F)
      for (auto &I : B)
        if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&I))
          Calls.push_back(Call);
  for (auto *Call : Calls) {
    if (AnalystSourceProjection &&
        Call->getMetadata("neverd.source.analyst-call"))
      continue;
    auto *Target = llvm::dyn_cast_or_null<llvm::Function>(
        constantCallIdentity(Call->getCalledOperand(), Layout, PointerBits));
    if (!Target || (Call->getFunctionType() == Target->getFunctionType() &&
                    Call->getCallingConv() == Target->getCallingConv()))
      continue;
    auto Refuse = [&](llvm::StringRef Reason) {
      syncError() << "med_llvm_emitter: resolved call in "
                  << Call->getFunction()->getName() << " to "
                  << Target->getName() << " " << Reason << "\n";
      return false;
    };
    auto *Type = Target->getFunctionType();
    const unsigned Fixed = Type->getNumParams();
    auto *Ordinary = llvm::dyn_cast<llvm::CallInst>(Call);
    auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(Call);
    if ((!Ordinary && !Invoke) || Call->isMustTailCall() ||
        Call->getCallingConv() != Target->getCallingConv())
      return Refuse("has an incompatible calling convention or control flow");
    llvm::IRBuilder<> Builder(Call);
    std::vector<int> Indices;
    std::vector<llvm::Value *> Values;
    for (unsigned I = 0; I < Call->arg_size(); ++I) {
      Indices.push_back(I);
      Values.push_back(Call->getArgOperand(I));
    }
    const auto CallerBanks = argumentBanks(*Call);
    const auto TargetBanks = argumentBanks(*Target);
    if (CallerBanks && TargetBanks && !Target->isVarArg()) {
      const ArgumentStackLayout Stack = argumentStackLayout(*Call);
      auto Bound = bindResolvedLLVMArguments(
          *Target, Values, CallerBanks->first, CallerBanks->second, Builder,
          Stack.Offsets, Stack.SlotBytes);
      if (!Bound)
        return Refuse("lacks recovered argument bank or stack values");
      Indices = std::move(Bound->SourceIndices);
      Values = std::move(Bound->Values);
    }
    if (Values.size() < Fixed)
      return Refuse("lacks recovered arguments");
    for (unsigned I = 0; I < Fixed; ++I)
      if (!Compatible(Values[I]->getType(), Type->getParamType(I)))
        return Refuse("has incompatible argument carriers");
    if (!Call->use_empty() &&
        (!Compatible(Type->getReturnType(), Call->getType()) ||
         (Invoke && Type->getReturnType() != Call->getType())))
      return Refuse("has an incompatible observed result");

    std::vector<llvm::Value *> Arguments;
    llvm::SmallVector<llvm::AttributeSet> ParameterAttrs;
    const unsigned Count = Type->isVarArg() ? Call->arg_size() : Fixed;
    for (unsigned I = 0; I < Count; ++I) {
      const int SourceIndex = Indices[I];
      auto *Value = Values[I];
      auto *Wanted = I < Fixed ? Type->getParamType(I) : Value->getType();
      ParameterAttrs.push_back(
          SourceIndex >= 0 && Value->getType() == Wanted &&
                  Value == Call->getArgOperand(SourceIndex)
              ? Call->getAttributes().getParamAttrs(SourceIndex)
              : llvm::AttributeSet{});
      Arguments.push_back(Convert(Builder, Value, Wanted));
    }
    llvm::SmallVector<llvm::OperandBundleDef> Bundles;
    Call->getOperandBundlesAsDefs(Bundles);
    llvm::CallBase *Replacement;
    if (Invoke)
      Replacement =
          Builder.CreateInvoke(Type, Target, Invoke->getNormalDest(),
                               Invoke->getUnwindDest(), Arguments, Bundles);
    else
      Replacement = Builder.CreateCall(Type, Target, Arguments, Bundles);
    Replacement->setCallingConv(Target->getCallingConv());
    auto FnAttrs = Call->getAttributes().getFnAttrs();
    if (Count != Call->arg_size())
      FnAttrs = FnAttrs.removeAttribute(Module.getContext(),
                                        llvm::Attribute::AllocSize);
    Replacement->setAttributes(
        llvm::AttributeList::get(Module.getContext(), FnAttrs,
                                 Type->getReturnType() == Call->getType()
                                     ? Call->getAttributes().getRetAttrs()
                                     : llvm::AttributeSet{},
                                 ParameterAttrs));
    Replacement->copyMetadata(*Call);
    if (CallerBanks && TargetBanks && !Target->isVarArg())
      setLLVMArgumentBanks(*Replacement, TargetBanks->first,
                           TargetBanks->second);
    if (Replacement->getType() != Call->getType())
      for (unsigned Kind :
           {llvm::LLVMContext::MD_range, llvm::LLVMContext::MD_nonnull,
            llvm::LLVMContext::MD_align, llvm::LLVMContext::MD_dereferenceable,
            llvm::LLVMContext::MD_dereferenceable_or_null})
        Replacement->setMetadata(Kind, nullptr);
    Replacement->setDebugLoc(Call->getDebugLoc());
    if (!Replacement->getType()->isVoidTy())
      Replacement->takeName(Call);
    if (Ordinary && Replacement->getType() == Call->getType())
      llvm::cast<llvm::CallInst>(Replacement)
          ->setTailCallKind(Ordinary->getTailCallKind());
    if (Sources)
      for (auto &Observation : Sources->Observations)
        if (Observation.Value == Call)
          Observation.Value = Replacement;
    if (!Call->use_empty())
      Call->replaceAllUsesWith(Convert(Builder, Replacement, Call->getType()));
    Call->eraseFromParent();
  }
  return true;
}

bool validateResolvedLLVMCallSignatures(llvm::Module &Module,
                                        bool AnalystSourceProjection) {
  const auto &Layout = Module.getDataLayout();
  for (auto &Function : Module)
    for (auto &Block : Function)
      for (auto &Instruction : Block)
        if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction))
          if (auto *Target = llvm::dyn_cast_or_null<llvm::Function>(
                  constantCallIdentity(Call->getCalledOperand(), Layout,
                                       Layout.getPointerSizeInBits()));
              Target && (Call->getFunctionType() != Target->getFunctionType() ||
                         Call->getCallingConv() != Target->getCallingConv())) {
            if (AnalystSourceProjection &&
                Call->getMetadata("neverd.source.analyst-call"))
              continue;
            syncError() << "med_llvm_emitter: resolved call in "
                        << Function.getName() << " disagrees with the ABI of "
                        << Target->getName() << "\n";
            return false;
          }
  return true;
}

//===----------------------------------------------------------------------===//
// CALL / INDIR_CALL -- direct and indirect call lowering
//===----------------------------------------------------------------------===//

void MedLLVMEmitter::defineCallClobbers(const MedOp &Op,
                                        llvm::IRBuilder<> &Builder) {
  if (!CurMedFunc || Op.CallSiteId == 0 || Op.PreservesCallerSaved)
    return;

  for (const MedCallClobber &Clobber : CurMedFunc->CallClobbers) {
    if (Clobber.CallSiteId != Op.CallSiteId || Clobber.Value.Size == 0)
      continue;
    auto *ClobberTy =
        llvm::cast<llvm::IntegerType>(sizeToType(Clobber.Value.Size));
    auto *Unknown = llvm::UndefValue::get(ClobberTy);
    llvm::Value *Frozen = Builder.CreateFreeze(
        Unknown, Clobber.Value.display() + "_call_clobber_unknown");
    if (Clobber.PreservedPrefixSize > 0) {
      llvm::Value *Preserved = getVar(Clobber.PreservedInput, Builder);
      if (Preserved->getType()->isPointerTy())
        Preserved = Builder.CreatePtrToInt(Preserved, ClobberTy);
      else if (Preserved->getType() != ClobberTy)
        Preserved = Builder.CreateZExtOrTrunc(Preserved, ClobberTy);

      unsigned Width = ClobberTy->getBitWidth();
      unsigned PrefixWidth = Clobber.PreservedPrefixSize * 8u;
      llvm::APInt LowMask = llvm::APInt::getLowBitsSet(Width, PrefixWidth);
      auto *LowMaskValue = llvm::ConstantInt::get(ClobberTy, LowMask);
      auto *HighMaskValue = llvm::ConstantInt::get(ClobberTy, ~LowMask);
      llvm::Value *KnownLow = Builder.CreateAnd(Preserved, LowMaskValue);
      llvm::Value *UnknownHigh = Builder.CreateAnd(Frozen, HighMaskValue);
      Frozen = Builder.CreateOr(UnknownHigh, KnownLow,
                                Clobber.Value.display() + "_call_clobber");
    }
    setVar(Clobber.Value, Frozen, Builder);
  }
}

void MedLLVMEmitter::emitCallOp(const MedOp &Op, llvm::IRBuilder<> &Builder,
                                int BlockId, int OpIdx) {
  auto GetInput = [&](uint8_t Idx) -> llvm::Value * {
    if (Idx >= Op.NumInputs)
      return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*Ctx), 0);
    return getVar(Op.Inputs[Idx], Builder);
  };
  llvm::Value *Result = nullptr;
  // Elide a Darwin stack-probe call (____chkstk_darwin).  It is invoked
  // GOT-indirect with its allocation size in a fixed register (x9) the lifter
  // does not model, so re-emitting it with the recovered argument registers
  // makes the probe walk off the stack and fault.  The probe is pure (it only
  // touches guard pages) and the dynamic allocation it guards is
  // independently lowered to a real alloca, so dropping it preserves
  // semantics.  chkstk leaves the result register untouched and clang never
  // reads it, so a defined-but-unused zero keeps any (dead) downstream use
  // well-typed.
  if (isStackProbeCall(Op)) {
    if (Op.Output.Size > 0)
      setVar(Op.Output,
             llvm::ConstantInt::get(
                 llvm::Type::getIntNTy(*Ctx, Op.Output.Size * 8u), 0),
             Builder);
    return;
  }

  const MedCallInfo *CI =
      CurMedFunc ? CurMedFunc->findCall(BlockId, OpIdx) : nullptr;
  if (Op.ScalarCallSignature &&
      (!CI ||
       !recoveredFloatingCallArguments(Op, *CI, TargetArch, TargetFormat))) {
    syncError() << "med_llvm_emitter: header call lost its checked scalar "
                   "or HFA carriers\n";
    FatalCodePointerResolution = true;
    return;
  }
  if (Op.ScalarCallSignature && Dbg && Img) {
    const std::string Name = importCalleeName(*Img, CI->TargetAddr);
    if (!Name.empty())
      if (const auto Header = Dbg->resolvePrototype(Name);
          Header &&
          !floatingHeaderCall(*Header, TargetArch, TargetFormat, Op, *CI)) {
        syncError() << "med_llvm_emitter: selected header disagrees with the "
                       "bound call carriers\n";
        FatalCodePointerResolution = true;
        return;
      }
  }
  // Address fragments are legal only while an address-forming expression is
  // being completed. Audit every call escape before ABI classification: an
  // indirect target, integer-class argument, FP/vector argument, or variadic
  // tail can otherwise bypass tryResolvePointerArg and leak a stale page VA.
  if (Op.Opcode == NdOp::INDIR_CALL && Op.NumInputs > 0)
    rejectEscapingAddressFragment(Op.Inputs[0], "an indirect call target");
  if (CI)
    for (const MedVar &Arg : CI->Args)
      rejectEscapingAddressFragment(Arg, "a call argument");

  llvm::Value *Target = nullptr;
  if (Op.Opcode == NdOp::INDIR_CALL &&
      !getX86RegistrationSEHRuntimeTarget(Op)) {
    if (Op.NumInputs > 0)
      Target = tryResolveIndirectCallTarget(Op.Inputs[0], Builder);
    if (!Target)
      Target = GetInput(0);
  }
  uint64_t CallAddr = Op.Inputs[0].isConst() ? Op.Inputs[0].ConstVal : 0;
  const Section *TargetSection =
      Img && CallAddr != 0 ? Img->getSectionFor(CallAddr) : nullptr;
  const bool IsObjCMessageStub =
      TargetSection && Img->isMachO() &&
      TargetSection->Name == section_names::macho::ObjCStubs;

  auto canonicalizeObjectCallee = [&](llvm::StringRef Name) -> std::string {
    if (!Img || TargetFormat != BinaryFormat::MachO || Name.empty())
      return Name.str();

    va_t TargetAddr = CallAddr;
    if (TargetAddr == 0 && CI)
      TargetAddr = CI->TargetAddr;
    bool IsObjectName = false;
    if (TargetAddr != 0) {
      if (const Import *Imp = Img->findImportStubAt(TargetAddr))
        IsObjectName = Imp->Name == Name;
      if (!IsObjectName)
        if (auto It = EffectiveImportStorageSlots.find(TargetAddr);
            It != EffectiveImportStorageSlots.end())
          IsObjectName = It->second.Name == Name;
    }
    // A relocatable object's first function can have address zero.  Its
    // symbol still proves the object-file spelling, whereas zero by itself
    // is only an absent-address sentinel for the import lookups above.
    if (!IsObjectName)
      for (const Symbol &Sym : Img->Symbols)
        if (Sym.Addr == TargetAddr && Sym.Name == Name) {
          IsObjectName = true;
          break;
        }
    if (!IsObjectName)
      for (const Export &Exp : Img->Exports)
        if (Exp.Addr == TargetAddr && Exp.Name == Name) {
          IsObjectName = true;
          break;
        }
    if (!IsObjectName)
      for (const RelocationEntry &Rel : Img->Relocations)
        if ((Rel.Address == Op.Addr || Rel.Address == Op.Addr + 1) &&
            Rel.SymbolName == Name) {
          IsObjectName = true;
          break;
        }

    return IsObjectName ? llvm_name::fromObjectSymbol(Name, TargetFormat).str()
                        : Name.str();
  };

  std::vector<llvm::Value *> Args;
  if (CI) {
    for (auto &Arg : CI->Args) {
      llvm::Value *Value =
          tryResolveCodeAddressValue(Arg, /*RequireCodeRole=*/false, Builder);
      Args.push_back(Value ? Value : getVar(Arg, Builder));
    }
  }
  // This is a typed binary80 argument even when the caller stores its bits
  // as i80, or emits a declaration because the callee body was not selected.
  if (Op.X87ArgumentInput) {
    const unsigned Count = Op.X87ImportArguments ? Op.X87ImportArguments : 1;
    if (Args.size() < Count ||
        (Op.X87ImportArguments && Args.size() != Count) ||
        llvm::any_of(llvm::ArrayRef(Args).take_back(
                         std::min<size_t>(Count, Args.size())),
                     [](llvm::Value *Value) {
                       return !Value->getType()->isIntegerTy(80) &&
                              !Value->getType()->isX86_FP80Ty();
                     })) {
      syncError() << "med_llvm_emitter: checked x87 call has no binary80 "
                     "argument in "
                  << (CurMedFunc ? CurMedFunc->Name : "<unknown>") << "\n";
      FatalCodePointerResolution = true;
      return;
    }
    for (size_t I = Args.size() - Count; I < Args.size(); ++I)
      if (Args[I]->getType()->isIntegerTy(80))
        Args[I] =
            Builder.CreateBitCast(Args[I], llvm::Type::getX86_FP80Ty(*Ctx));
  }

  auto *RetTy = llvm::Type::getInt64Ty(*Ctx);
  auto *I8Ty = llvm::Type::getInt8Ty(*Ctx);
  auto *I32Ty = llvm::Type::getInt32Ty(*Ctx);
  auto *I64Ty = llvm::Type::getInt64Ty(*Ctx);
  auto *PtrTy = llvm::PointerType::getUnqual(*Ctx);

  std::vector<llvm::Type *> ArgTypes;
  for (auto *A : Args)
    ArgTypes.push_back(A->getType());

  // A multi-register return is represented in MedIR as one flat temporary
  // followed by SUBBYTES extracts into its physical return registers.  Recover
  // the corresponding LLVM aggregate type once, for both direct imports and
  // indirect calls.  Direct unknown externs previously kept the default i64
  // declaration even after call-site modeling had proven an X0:X1 result.
  auto inferAggregateReturnType = [&]() -> llvm::Type * {
    if (Op.Output.Kind != MedVar::Temp || Op.Output.Size == 0 || !CurMedFunc)
      return nullptr;
    const auto &TRI = getTargetRegInfo(TargetArch);
    std::map<unsigned, llvm::Type *> FieldsByOffset;
    for (const auto &Blk2 : CurMedFunc->Blocks) {
      if (Blk2.Id != BlockId)
        continue;
      for (size_t J = static_cast<size_t>(OpIdx) + 1; J < Blk2.Ops.size();
           ++J) {
        const auto &Ex = Blk2.Ops[J];
        if (Ex.Opcode != NdOp::SUBBYTES || Ex.NumInputs < 2 ||
            Ex.Inputs[0].Kind != MedVar::Temp ||
            Ex.Inputs[0].Id != Op.Output.Id ||
            Ex.Inputs[0].SSAVer != Op.Output.SSAVer ||
            !Ex.Inputs[1].isConst() || Ex.Output.Kind != MedVar::Reg)
          continue;
        unsigned Offset = static_cast<unsigned>(Ex.Inputs[1].ConstVal);
        uint16_t Size = Ex.Output.Size ? Ex.Output.Size : 8;
        if (Offset + Size > Op.Output.Size)
          continue;
        llvm::Type *FieldTy = TRI.isVectorReg(Ex.Output.RegOff)
                                  ? (Size <= 4 ? llvm::Type::getFloatTy(*Ctx)
                                               : llvm::Type::getDoubleTy(*Ctx))
                                  : llvm::Type::getIntNTy(*Ctx, Size * 8);
        FieldsByOffset.emplace(Offset, FieldTy);
      }
      break;
    }
    if (FieldsByOffset.size() < 2)
      return nullptr;
    std::vector<llvm::Type *> FieldTys;
    unsigned ExpectedOffset = 0;
    for (const auto &[Offset, FieldTy] : FieldsByOffset) {
      if (Offset != ExpectedOffset)
        return nullptr;
      FieldTys.push_back(FieldTy);
      ExpectedOffset += FieldTy->getPrimitiveSizeInBits() / 8;
    }
    if (ExpectedOffset != Op.Output.Size)
      return nullptr;
    return llvm::StructType::get(*Ctx, FieldTys);
  };
  llvm::Type *AggregateRetTy = inferAggregateReturnType();
  llvm::Type *DefaultRetTy = AggregateRetTy ? AggregateRetTy : RetTy;
  const bool X87Result =
      Op.Output.Kind == MedVar::Reg && Op.Output.Size == 10 &&
      getTargetRegInfo(TargetArch).isX87StackReg(Op.Output.RegOff);
  if (X87Result)
    DefaultRetTy = llvm::Type::getX86_FP80Ty(*Ctx);
  else if (Op.X87ArgumentInput && !AggregateRetTy &&
           Op.Output.Kind == MedVar::Reg && Op.Output.Size &&
           Op.Output.RegOff == getTargetRegInfo(TargetArch).IntReturnReg)
    DefaultRetTy = llvm::Type::getIntNTy(*Ctx, Op.Output.Size * 8);

  llvm::GlobalValue *KnownTarget =
      Target
          ? constantCallIdentity(Target, Mod->getDataLayout(),
                                 getTargetRegInfo(TargetArch).PointerSize * 8)
          : nullptr;
  std::string IndirectImportName;
  bool PreserveHeaderTarget = false;
  std::optional<FunctionSym> VariadicHeader;
  if (Op.CalleeVariadicFrom >= 0 && Img && CI && Dbg) {
    const std::string Name = importCalleeName(*Img, CI->TargetAddr);
    if (!Name.empty())
      if (auto Header = Dbg->resolvePrototype(Name);
          Header &&
          libc::variadicCallArguments(*Header, TargetArch, TargetFormat) &&
          Header->Params.size() == static_cast<size_t>(Op.CalleeVariadicFrom) &&
          CI->VarArgFixedCount == Op.CalleeVariadicFrom &&
          Args.size() >= Header->Params.size()) {
        VariadicHeader = std::move(*Header);
        IndirectImportName = Name;
        PreserveHeaderTarget = Target && !KnownTarget;
      }
    if (!VariadicHeader) {
      syncError() << "med_llvm_emitter: variadic header call lost its checked "
                     "fixed prefix\n";
      FatalCodePointerResolution = true;
      return;
    }
  }
  if ((Op.CalleeFloatReturnBytes || Op.ScalarCallSignature) && Target && Img &&
      CI && Dbg) {
    const std::string Name = importCalleeName(*Img, CI->TargetAddr);
    if (!Name.empty())
      if (const auto Header = Dbg->resolvePrototype(Name);
          Header &&
          floatingHeaderCall(*Header, TargetArch, TargetFormat, Op, *CI)) {
        IndirectImportName = Name;
        // A declaration proves the ABI of this import slot. It does not
        // replace a writable slot's current contents with the initial symbol.
        PreserveHeaderTarget = !KnownTarget;
      }
  }
  if (Op.X87ImportArguments) {
    if (!Img || !CI ||
        x87CRTImportArguments(*Img, CI->TargetAddr) != Op.X87ImportArguments) {
      syncError() << "med_llvm_emitter: CRT x87 call lost its checked import "
                     "identity\n";
      FatalCodePointerResolution = true;
      return;
    }
    // The slot proves the special ABI, but its runtime contents remain the
    // call target. A writable IAT must not become a fixed-symbol call merely
    // because the imported routine's name is known.
    IndirectImportName = importCalleeName(*Img, CI->TargetAddr);
  }
  if (KnownTarget && !llvm::isa<llvm::Function>(KnownTarget)) {
    const auto It =
        ImportedSymbolPlaceholders.find(KnownTarget->getName().str());
    const auto Header = Dbg && CI
                            ? Dbg->resolvePrototype(KnownTarget->getName())
                            : std::nullopt;
    if (It != ImportedSymbolPlaceholders.end() && It->second == KnownTarget &&
        (libc::libcArityForSymbol(It->first, TargetLibC) ||
         (Header &&
          floatingHeaderCall(*Header, TargetArch, TargetFormat, Op, *CI)) ||
         (Op.X87ImportArguments && isX87CRTImportName(It->first)) ||
         libc::varArgFixedCount(stripLeadingUnderscores(It->first),
                                TargetLibC)))
      IndirectImportName = It->first;
  }

  // An exact immutable import identity uses the same ABI materializer as a
  // direct call. It must not wait for some later body to promote the import's
  // data placeholder into a Function: LLVM can fold that pointer after
  // emission.
  auto calleeSymbol = [&]() -> std::string {
    if (!IndirectImportName.empty())
      return IndirectImportName;
    if (CI && !CI->TargetName.empty() &&
        !CI->TargetName.starts_with(kAutoFuncPrefix))
      return canonicalizeObjectCallee(CI->TargetName);
    if (CallAddr != 0 ||
        (Op.Opcode == NdOp::CALL && Op.NumInputs && Op.Inputs[0].isConst())) {
      auto FnIt = FuncNames.find(CallAddr);
      if (FnIt != FuncNames.end())
        return canonicalizeObjectCallee(FnIt->second);
    }
    return {};
  };
  auto resolveCalleeName = [&]() -> std::string {
    std::string Name = calleeSymbol();
    if (IndirectImportName.empty() && Img && CallAddr > 1 && !Name.empty())
      if (auto Shadow =
              peImportShadowName(PEImportCNames, *Img, CallAddr, Name))
        return std::move(*Shadow);
    return Name;
  };

  const std::optional<counted_write::Semantics> SafetyShape =
      counted_write::classify(resolveCalleeName(), TargetFormat);
  auto AttachSafetyCallsite = [&](llvm::CallBase &Call) {
    if (!SafetyShape || !CurMedFunc || BlockId < 0 || OpIdx < 0 ||
        Op.OriginSeq < 0 || Op.CallSiteId == 0)
      return;
    safety_callsite_md::SafetyCallsiteRecord Record;
    Record.Occurrence.FuncEntry = CurMedFunc->Entry;
    Record.Occurrence.CallVA = Op.Addr;
    Record.Occurrence.BlockId = static_cast<uint32_t>(BlockId);
    Record.Occurrence.OpIdx = static_cast<uint32_t>(OpIdx);
    Record.Occurrence.OriginSeq = static_cast<uint32_t>(Op.OriginSeq);
    Record.Occurrence.CallSiteId = Op.CallSiteId;
    Record.Kind = SafetyShape->Kind;
    Record.DestinationOperandIndex = SafetyShape->DestinationOperandIndex;
    Record.LengthOperandIndex = SafetyShape->LengthOperandIndex;
    Record.ElementBytes = SafetyShape->ElementBytes;
    if (llvm::Error Error = safety_callsite_md::attach(Call, Record))
      llvm::consumeError(std::move(Error));
  };

  if (CI && CI->SourceCallHint &&
      CI->SourceCallHint->CallKind == SourceCallTypeHint::Kind::Analyst) {
    const auto &Hint = *CI->SourceCallHint;
    std::string Diagnostic;
    auto Refuse = [&](llvm::StringRef Reason) {
      syncError() << "source call contract: " << Reason << "\n";
      FatalCodePointerResolution = true;
    };
    if (!AnalystSourceProjection || !Hint.AnalystSite ||
        Hint.AnalystSite->Instruction != Op.Addr ||
        Hint.AnalystSite->Sequence != Op.OriginSeq ||
        Hint.Signature.Architecture != TargetArch ||
        !validateSourceABI(Hint.Signature, Diagnostic) ||
        Args.size() != Hint.Signature.Parameters.size()) {
      Refuse("invalid or non-source-only occurrence binding");
      return;
    }
    std::vector<llvm::Type *> Types;
    std::vector<llvm::Value *> Values;
    for (unsigned I = 0; I < Args.size(); ++I) {
      auto *Type = mapNdtype(Hint.Signature.Parameters[I].Type);
      auto *Value = Args[I];
      const auto Bits = [](llvm::Type *T) -> unsigned {
        return T->isPointerTy() ? 64
                                : T->getPrimitiveSizeInBits().getFixedValue();
      };
      if (!Type || Bits(Type) != Bits(Value->getType()) ||
          llvm::isa<llvm::UndefValue, llvm::PoisonValue>(Value)) {
        Refuse("a complete argument carrier is unavailable");
        return;
      }
      if (Type != Value->getType()) {
        if (Type->isPointerTy())
          Value = Builder.CreateIntToPtr(Value, Type);
        else if (Value->getType()->isPointerTy())
          Value = Builder.CreatePtrToInt(Value, Type);
        else
          Value = Builder.CreateBitCast(Value, Type);
      }
      Types.push_back(Type);
      Values.push_back(Value);
    }
    const unsigned Fixed =
        Hint.AnalystFixedCount < 0 ? Types.size() : Hint.AnalystFixedCount;
    if (Fixed > Types.size()) {
      Refuse("the variadic prefix exceeds the available operands");
      return;
    }
    std::vector<llvm::Type *> Prefix(Types.begin(), Types.begin() + Fixed);
    auto *FT = llvm::FunctionType::get(mapNdtype(Hint.Signature.ReturnType),
                                       Prefix, Hint.AnalystFixedCount >= 0);
    llvm::Value *Callable = Target;
    if (!Callable) {
      auto Name = resolveCalleeName();
      if (Name.empty() && CallAddr)
        Name = CI->TargetName.empty()
                   ? (kAutoFuncPrefix + llvm::utohexstr(CallAddr)).str()
                   : CI->TargetName;
      if (Name.empty()) {
        Refuse("the original target is unresolved");
        return;
      }
      Callable = Mod->getNamedValue(Name);
      if (!Callable) {
        auto *DeclarationType = llvm::FunctionType::get(I64Ty, true);
        unsigned Convention = llvm::CallingConv::C;
        if (AnalystTargets)
          if (const auto It = AnalystTargets->find(Name);
              It != AnalystTargets->end()) {
            DeclarationType = It->second.first;
            Convention = It->second.second;
          }
        auto *Declaration = llvm::Function::Create(
            DeclarationType, llvm::GlobalValue::ExternalLinkage, Name, Mod);
        Declaration->setCallingConv(Convention);
        Callable = Declaration;
      }
    }
    if (Callable->getType()->isIntegerTy())
      Callable = Builder.CreateIntToPtr(Callable, PtrTy);
    auto *Call = Builder.CreateCall(FT, Callable, Values);
    if (Hint.Signature.ReturnType->Kind == NdTypeKind::Int &&
        Hint.Signature.ReturnType->IsSigned)
      Call->addFnAttr(llvm::Attribute::get(*Ctx, LLVMSourceSignedResult));
    for (unsigned I = 0; I < Hint.Signature.Parameters.size(); ++I)
      if (const auto &Type = Hint.Signature.Parameters[I].Type;
          Type->Kind == NdTypeKind::Int && Type->IsSigned)
        Call->addParamAttr(
            I, llvm::Attribute::get(*Ctx, LLVMSourceSignedParameter));
    Call->setMetadata(
        "neverd.source.analyst-call",
        llvm::MDNode::get(*Ctx,
                          {llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(I64Ty, Op.Addr)),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(I32Ty, Op.OriginSeq))}));
    if (TargetFormat == BinaryFormat::COFF && TargetArch == Arch::X64)
      Call->setCallingConv(llvm::CallingConv::Win64);
    if (SourceMap)
      SourceMap->Observations.push_back(
          {CurMedFunc->Entry, {Op.Addr, Op.OriginSeq}, Call});
    Call->setMetadata(
        language_eh_md::InternalSourceCallAttachment,
        llvm::MDNode::get(*Ctx, llvm::ConstantAsMetadata::get(
                                    llvm::ConstantInt::get(I64Ty, Op.Addr))));
    CallSiteAddrs[Call] = Op.Addr;
    if (Op.DoesNotReturn) {
      Builder.CreateUnreachable();
      return;
    }
    if (Op.Output.Size && !Call->getType()->isVoidTy())
      setVar(Op.Output, Call, Builder);
    defineCallClobbers(Op, Builder);
    return;
  }

  if (Op.Opcode == NdOp::CALL && CI && Args.size() >= 3) {
    std::string LibNameStr = resolveCalleeName();
    llvm::StringRef LibName = stripLeadingUnderscores(LibNameStr);
    if (libc::isMemSetName(LibName)) {
      llvm::Value *Dest = Args[0];
      if (Dest->getType()->isIntegerTy()) {
        llvm::Value *Sym =
            (CI && !CI->Args.empty())
                ? tryResolvePointerArg(CI->Args[0],
                                       /*FailClosed=*/true, Builder)
                : nullptr;
        Dest = Sym ? Sym : Builder.CreateIntToPtr(Dest, PtrTy);
      }
      llvm::Value *Val = Args[1];
      if (!Val->getType()->isIntegerTy(8))
        Val = Builder.CreateTrunc(Val, I8Ty);
      llvm::Value *Len = Args[2];
      if (Len->getType()->getIntegerBitWidth() < 64)
        Len = Builder.CreateZExt(Len, I64Ty);
      llvm::CallInst *Memset =
          Builder.CreateMemSet(Dest, Val, Len, llvm::MaybeAlign(1));
      AttachSafetyCallsite(*Memset);
      if (Op.Output.Size > 0)
        setVar(Op.Output, Dest, Builder);
      defineCallClobbers(Op, Builder);
      return;
    }
  }

  llvm::Function *Callee = nullptr;
  if (Op.Opcode == NdOp::CALL || !IndirectImportName.empty()) {
    std::string CalleeName = resolveCalleeName();
    // An import whose prototype the user declares passes what call-ABI
    // recovery gave it, which the tables do not count.
    const bool PrototypeArguments =
        PrototypeArgumentCallees && CI &&
        PrototypeArgumentCallees->count(CI->TargetAddr);
    if (!IndirectImportName.empty() && !PrototypeArguments) {
      const auto Arity = libc::libcArityForSymbol(CalleeName, TargetLibC);
      const unsigned Required =
          Arity ? Arity->IntArgs + Arity->FpArgs
                : libc::varArgFixedCount(stripLeadingUnderscores(CalleeName),
                                         TargetLibC);
      if (Args.size() < Required) {
        syncError() << "med_llvm_emitter: resolved indirect call to "
                    << CalleeName << " lacks recovered arguments\n";
        FatalCodePointerResolution = true;
        return;
      }
    }
    if (!CalleeName.empty() && CallAddr == 0)
      CallAddr = 1; // force direct-call resolution below
    if (CallAddr != 0 ||
        (Op.Opcode == NdOp::CALL && Op.NumInputs && Op.Inputs[0].isConst())) {
      // A pointer-table bind can prove a symbol's external identity before a
      // call proves its ABI.  The mirror represents that unresolved identity
      // as an external i8 global.  Temporarily free the canonical name so the
      // existing call-signature recovery can create the correctly typed
      // Function; once it has, rewrite every table initializer use to the
      // Function and erase the placeholder.
      llvm::GlobalVariable *ImportedPlaceholder = nullptr;
      if (!CalleeName.empty()) {
        auto It = ImportedSymbolPlaceholders.find(CalleeName);
        if (It != ImportedSymbolPlaceholders.end() && It->second &&
            It->second->getParent() == Mod) {
          ImportedPlaceholder = It->second;
          ImportedPlaceholder->setName(CalleeName + ".import_data");
        }
      }

      if (VariadicHeader) {
        std::vector<llvm::Type *> Parameters;
        for (const auto &[Name, Type] : VariadicHeader->Params)
          Parameters.push_back(mapNdtype(Type));
        auto *FT = llvm::FunctionType::get(
            mapNdtype(VariadicHeader->ReturnType), Parameters, true);
        Callee = Mod->getFunction(CalleeName);
        if (Callee && Callee->getFunctionType() != FT) {
          syncError() << "med_llvm_emitter: conflicting variadic import ABI\n";
          FatalCodePointerResolution = true;
          return;
        }
        if (!Callee)
          Callee = llvm::Function::Create(
              FT, llvm::GlobalValue::ExternalLinkage, CalleeName, Mod);
        Callee->setCallingConv(TargetArch == Arch::X64
                                   ? llvm::CallingConv::Win64
                                   : llvm::CallingConv::C);
      }
      if (Op.X87ImportArguments) {
        Callee = Mod->getFunction(CalleeName);
        if (!Callee && !CalleeName.empty())
          Callee = llvm::Function::Create(
              llvm::FunctionType::get(llvm::Type::getX86_FP80Ty(*Ctx), false),
              llvm::GlobalValue::ExternalLinkage, CalleeName, Mod);
      }

      // On AArch64, variadic and non-variadic functions use different
      // calling conventions (variadic args go on the stack).  We detect
      // known variadic functions by pattern/whitelist, and for any
      // remaining unknown external function we conservatively declare
      // it as variadic (all actual args as fixed + "...") so the
      // backend never puts arguments in the wrong location.
      unsigned NumFixed = 0;
      unsigned NamedNumFixed = 0;
      if (CI && CI->VarArgFixedCount >= 0) {
        NumFixed = static_cast<unsigned>(CI->VarArgFixedCount);
        // A stripped Mach-O has no local `_objc_msgSend$selector` symbol, but
        // the preserved __objc_stubs entry still has a concrete address.
        // Encode that address in the backend's resolvable code-pointer symbol
        // instead of falling through to a non-variadic anonymous stub.
        if (CalleeName.empty())
          CalleeName =
              IsObjCMessageStub
                  ? (kNdCodePtrPrefix + llvm::utohexstr(CallAddr)).str()
                  : (kAutoFuncPrefix + llvm::utohexstr(CallAddr)).str();
      } else if (!CalleeName.empty()) {
        llvm::StringRef Bare = stripLeadingUnderscores(CalleeName);
        NamedNumFixed = libc::varArgFixedCount(Bare, TargetLibC);
        NumFixed = NamedNumFixed;
      }

      if (!CalleeName.empty() && NamedNumFixed == 0)
        NamedNumFixed = libc::varArgFixedCount(
            stripLeadingUnderscores(CalleeName), TargetLibC);

      bool IsKnownVarArg = (NumFixed > 0);
      const bool IsStructurallyRecoveredVarArg =
          IsKnownVarArg && CI && CI->VarArgFixedCount >= 0 &&
          NamedNumFixed == 0 && !IsObjCMessageStub;

      if (IsKnownVarArg && !CalleeName.empty()) {
        Callee = Mod->getFunction(CalleeName);
        if (!Callee) {
          std::vector<llvm::Type *> FixedTypes;
          for (unsigned I = 0; I < NumFixed && I < Args.size(); ++I) {
            const auto Kind =
                libc::varArgFixedParamKind(CalleeName, I, TargetLibC);
            if (IsStructurallyRecoveredVarArg ||
                Kind == libc::VarArgFixedParamKind::Unknown) {
              FixedTypes.push_back(ArgTypes[I]);
            } else if (Kind == libc::VarArgFixedParamKind::Pointer) {
              FixedTypes.push_back(PtrTy);
            } else {
              FixedTypes.push_back(ArgTypes[I]->isIntegerTy() ? ArgTypes[I]
                                                              : I64Ty);
            }
          }
          llvm::Type *VarRetTy =
              IsObjCMessageStub
                  ? RetTy
                  : (IsStructurallyRecoveredVarArg ? DefaultRetTy : I32Ty);
          auto *VarFT = llvm::FunctionType::get(VarRetTy, FixedTypes, true);
          Callee = llvm::Function::Create(
              VarFT, llvm::GlobalValue::ExternalLinkage, CalleeName, Mod);
          Callee->setCallingConv(llvm::CallingConv::C);
        }
        for (unsigned I = 0;
             !IsStructurallyRecoveredVarArg && I < NumFixed && I < Args.size();
             ++I) {
          if (libc::varArgFixedParamKind(CalleeName, I, TargetLibC) ==
                  libc::VarArgFixedParamKind::Pointer &&
              Args[I]->getType()->isIntegerTy()) {
            llvm::Value *Sym =
                (CI && I < CI->Args.size())
                    ? tryResolvePointerArg(CI->Args[I],
                                           /*FailClosed=*/true, Builder)
                    : nullptr;
            Args[I] = Sym ? Sym : Builder.CreateIntToPtr(Args[I], PtrTy);
          }
        }
      }

      if (!Callee && !CalleeName.empty())
        Callee = Mod->getFunction(CalleeName);

      // A selected header can describe imports absent from the curated libc
      // tables. Keep their scalar FP ABI instead of declaring their vector
      // bit carriers as integer arguments and an integer result.
      if (!Callee && Dbg && CI && Img && !CalleeName.empty() &&
          (!IndirectImportName.empty() || Img->findImportAt(CI->TargetAddr) ||
           Img->findImportStubAt(CI->TargetAddr)))
        if (const auto Function = Dbg->resolvePrototype(CalleeName);
            Function &&
            floatingHeaderCall(*Function, TargetArch, TargetFormat, Op, *CI)) {
          const auto Signature =
              libc::floatingCallArguments(*Function, TargetArch, TargetFormat);
          auto *FT = floatingCallType(*Ctx, *Signature);
          Callee = llvm::Function::Create(
              FT, llvm::GlobalValue::ExternalLinkage, CalleeName, Mod);
          Callee->setCallingConv(TargetArch == Arch::X64 &&
                                         TargetFormat == BinaryFormat::COFF
                                     ? llvm::CallingConv::Win64
                                     : llvm::CallingConv::C);
        }

      // A selected caller can omit the body that established this complete
      // scalar FP signature. Its declaration must keep that recovered ABI;
      // machine vector operands do not make the callee return an integer.
      if (!Callee && CI && !CI->IsIndirect) {
        if (const auto Signature = recoveredFloatingCallArguments(
                Op, *CI, TargetArch, TargetFormat)) {
          const std::string Name =
              CalleeName.empty()
                  ? (kAutoFuncPrefix + llvm::utohexstr(CallAddr)).str()
                  : CalleeName;
          Callee = Mod->getFunction(Name);
          if (!Callee) {
            Callee = llvm::Function::Create(floatingCallType(*Ctx, *Signature),
                                            llvm::GlobalValue::ExternalLinkage,
                                            Name, Mod);
            Callee->setCallingConv(TargetArch == Arch::X64 &&
                                           TargetFormat == BinaryFormat::COFF
                                       ? llvm::CallingConv::Win64
                                       : llvm::CallingConv::C);
          }
        }
      }

      // A known non-variadic libc function with floating-point arguments (the
      // math.h family sqrt/pow/sin/...) must be declared with FP
      // `double`/`float` parameters and return so its arguments travel in the
      // FP registers (d0-d7 / s0-s7) and the result is read from d0/s0.  The
      // recovered arguments are the FP register bit pattern tracked as an
      // integer/vector (i128); the per-argument coercion below bridges them
      // to the FP type, and setVar bitcasts the FP result back to the
      // integer-tracked return register.  Declared only for the all-FP shape
      // (IntArgs==0 && FpArgs>0) we model; mixed int/FP forms fall to the
      // conservative variadic fallback. Without this an external `pow`/`sin`
      // is declared `(i128,...)->i64`, so the double args go in
      // integer/vector registers (pow reads garbage d0/d1) and the double
      // result is read from x0 -- patched math returns wrong values.
      if (!Callee && !CalleeName.empty()) {
        // The user's prototype of an import is declared from its calls.
        std::optional<libc::LibCArity> Arity;
        if (!PrototypeArguments)
          Arity = libc::libcArityForSymbol(CalleeName, TargetLibC);
        // A known libc function whose RESULT is floating-point.  Three shapes
        // are modelled: (a) all-FP-args (sqrt/pow), (b) FP-first mixed int/FP
        // (ldexp/frexp), and (c) integer/pointer args with an FP return
        // (atof/strtod/difftime/nan -- FpRet).  long double is only ABI-equal
        // to double on Apple AArch64, so FpRetLongDouble (strtold) is
        // honoured only there; elsewhere it falls to the conservative
        // fallback.  (d) a
        // `_Complex` result (csqrt/cexp/cpow -- FpRetComplex): a 2-element FP
        // aggregate returned in two FP registers, declared as a `{fp,fp}`
        // struct return so the struct-return flatten below packs both result
        // registers.
        bool LongDoubleAsDouble = Arity && Arity->FpRetLongDouble && Img &&
                                  Img->isMachO() && TargetArch == Arch::AArch64;
        bool ReturnsFp = Arity && (Arity->FpArgs > 0 || Arity->FpRet ||
                                   LongDoubleAsDouble || Arity->FpRetComplex);
        bool ArgsModelled = Arity && (Arity->FpArgs == 0 ||
                                      Arity->IntArgs == 0 || Arity->FpFirst);
        bool IsPlainInteger =
            Arity && Arity->IntArgs >= 0 && Arity->FpArgs == 0 &&
            !Arity->FpRet && !Arity->FpRetLongDouble && !Arity->FpRetComplex &&
            !libc::isVaListConsumer(stripLeadingUnderscores(CalleeName));
        if (IsPlainInteger && Args.size() >= size_t(Arity->IntArgs)) {
          // Use one carrier width for this target, regardless of which call
          // site or LLVM shard first observes a narrow register alias.
          const std::vector<llvm::Type *> Parameters(
              Arity->IntArgs,
              sizeToType(getTargetRegInfo(TargetArch).PointerSize));
          auto *FT = llvm::FunctionType::get(DefaultRetTy, Parameters, false);
          Callee = llvm::Function::Create(
              FT, llvm::GlobalValue::ExternalLinkage, CalleeName, Mod);
          Callee->setCallingConv(llvm::CallingConv::C);
        } else if (Arity && ReturnsFp && ArgsModelled) {
          auto *FpTy = Arity->FpIsFloat ? llvm::Type::getFloatTy(*Ctx)
                                        : llvm::Type::getDoubleTy(*Ctx);
          const int NFp = Arity->FpArgs, NInt = Arity->IntArgs;
          // A `_Complex` return is two FP registers.  When the lifter
          // recovered the call as a multi-register aggregate
          // (modelCallStructReturn turned the output into a flat temp with
          // two SUBBYTES extracts), declare a
          // `{fp,fp}` struct return so the backend's HFA lowering writes both
          // result registers and the struct-return flatten below repacks
          // them. The struct-return flatten packs the two elements
          // CONTIGUOUSLY, so the element width must equal the lifter's
          // per-field slot (Output.Size/2) for the second element to land at
          // its return register's byte offset. On AArch64 a `str d0,d1` of a
          // `_Complex float` result reads full D registers, so the lifter
          // lays the fields out 8 bytes apart even though the values are
          // 32-bit floats — declaring `{double,double}` there keeps the
          // layout aligned and the float values occupy the low 32 bits, read
          // back correctly via the S0/S1 sub-register views.  If only the
          // real part was used (output stayed a single register), a scalar FP
          // return (real part in d0) is exactly right, so fall through.
          llvm::Type *RetTy2 = FpTy;
          if (Arity->FpRetComplex && Op.Output.Kind == MedVar::Temp &&
              Op.Output.Size >= 2) {
            unsigned Half = static_cast<unsigned>(Op.Output.Size) / 2;
            llvm::Type *ElemTy = Half >= 8 ? llvm::Type::getDoubleTy(*Ctx)
                                           : llvm::Type::getFloatTy(*Ctx);
            RetTy2 = llvm::StructType::get(*Ctx, {ElemTy, ElemTy});
          }
          // Mixed FP-first libm (ldexp/frexp/modf/scalbn): the recovered
          // arguments are in the int-then-FP model order, but the real
          // signature is the FP arg(s) first.  Reorder by count (the leading
          // NInt are the int/pointer args, the next NFp are the FP args) so
          // they line up with the declared FP-first signature.
          if (Arity->FpFirst && NInt > 0 &&
              static_cast<int>(Args.size()) >= NInt + NFp) {
            std::vector<llvm::Value *> Reordered;
            for (int I = 0; I < NFp; ++I)
              Reordered.push_back(Args[NInt + I]);
            for (int I = 0; I < NInt; ++I)
              Reordered.push_back(Args[I]);
            for (size_t I = static_cast<size_t>(NInt + NFp); I < Args.size();
                 ++I)
              Reordered.push_back(Args[I]);
            Args = std::move(Reordered);
          }
          Callee = Mod->getFunction(CalleeName);
          if (!Callee) {
            // FP params first, then int/pointer params (modelled as i64; the
            // callee reads the low 32 bits for an `int` arg, and an i64
            // pointer value occupies the same x-register).
            std::vector<llvm::Type *> ParamTys;
            for (int I = 0; I < NFp; ++I)
              ParamTys.push_back(FpTy);
            for (int I = 0; I < NInt; ++I)
              ParamTys.push_back(I64Ty);
            auto *FT = llvm::FunctionType::get(RetTy2, ParamTys, false);
            Callee = llvm::Function::Create(
                FT, llvm::GlobalValue::ExternalLinkage, CalleeName, Mod);
            Callee->setCallingConv(llvm::CallingConv::C);
          }
        } else if (Arity && libc::isVaListConsumer(
                                stripLeadingUnderscores(CalleeName))) {
          std::vector<llvm::Type *> ParamTys(
              static_cast<size_t>(Arity->IntArgs), PtrTy);
          auto *FT = llvm::FunctionType::get(DefaultRetTy, ParamTys, false);
          Callee = llvm::Function::Create(
              FT, llvm::GlobalValue::ExternalLinkage, CalleeName, Mod);
          Callee->setCallingConv(llvm::CallingConv::C);
        }
      }

      // A callee the module neither defines nor describes is declared from
      // this call's arguments.  Where the module's calls pass it different
      // numbers of arguments, those past the ones every call passes are
      // variadic: a declaration from this call's would cut a longer call's.
      auto undescribedCalleeType = [&](bool VarArg) {
        std::vector<llvm::Type *> Fixed = ArgTypes;
        if (!VarArg && AggregateRetTy == nullptr && CI && !CI->IsIndirect)
          if (const auto It = DirectCallArgumentCounts.find(CI->TargetAddr);
              It != DirectCallArgumentCounts.end() &&
              It->second.first != It->second.second) {
            VarArg = true;
            Fixed.resize(std::min(Fixed.size(), It->second.first));
          }
        return llvm::FunctionType::get(DefaultRetTy, Fixed, VarArg);
      };
      if (!Callee && !CalleeName.empty()) {
        bool UseVarArg = getTargetRegInfo(TargetArch).UnknownExternIsVarArg &&
                         AggregateRetTy == nullptr;
        auto *FT = undescribedCalleeType(UseVarArg);
        Callee = llvm::Function::Create(FT, llvm::GlobalValue::ExternalLinkage,
                                        CalleeName, Mod);
        Callee->setCallingConv(llvm::CallingConv::C);
      }

      if (!Callee) {
        auto StubName = (kAutoFuncPrefix + llvm::utohexstr(CallAddr)).str();
        Callee = Mod->getFunction(StubName);
        if (!Callee) {
          auto *StubTy = undescribedCalleeType(false);
          Callee = llvm::Function::Create(
              StubTy, llvm::GlobalValue::ExternalLinkage, StubName, Mod);
          Callee->addFnAttr(llvm::Attribute::NullPointerIsValid);
        }
      }

      if (ImportedPlaceholder) {
        if (Callee) {
          if (Target == ImportedPlaceholder)
            Target = Callee;
          ImportedPlaceholder->replaceAllUsesWith(Callee);
          ImportedPlaceholder->eraseFromParent();
          ImportedSymbolPlaceholders.erase(CalleeName);
          if (!IndirectImportName.empty() && !PreserveHeaderTarget)
            KnownTarget = Callee;
        } else {
          // Defensive only: every direct-call path above creates a callee, but
          // leave a valid canonical declaration if a future path declines.
          ImportedPlaceholder->setName(CalleeName);
        }
      }
    }
  }

  if (!Callee && Target)
    Callee = llvm::dyn_cast_or_null<llvm::Function>(KnownTarget);
  if (auto *Runtime = prepareX86RegistrationSEHRuntimeCall(Op, Builder, Args)) {
    Callee = Runtime;
    KnownTarget = Runtime;
    Target = nullptr;
  }
  if (auto *Runtime =
          prepareX86RegistrationLocalUnwindCall(Op, Builder, Args)) {
    Callee = Runtime;
    KnownTarget = Runtime;
    Target = nullptr;
  }
  if (Op.X87ImportArguments) {
    if (!Callee) {
      syncError() << "med_llvm_emitter: CRT x87 call has no import identity\n";
      FatalCodePointerResolution = true;
      return;
    }
    if (CallAddr > 1 && Callee->isDeclaration()) {
      auto Identity = rewrite_source::getOriginalVA(*Callee);
      if (!Identity) {
        llvm::consumeError(Identity.takeError());
        FatalCodePointerResolution = true;
        return;
      }
      if (!*Identity)
        rewrite_source::setOriginalVA(*Callee, CallAddr);
    }
    auto Adapter = createX87ImportCallAdapter(*Callee, Op.X87ImportArguments);
    if (!Adapter) {
      syncError() << "med_llvm_emitter: " << llvm::toString(Adapter.takeError())
                  << "\n";
      FatalCodePointerResolution = true;
      return;
    }
    llvm::Value *RuntimeTarget = Target ? Target : Callee;
    if (RuntimeTarget->getType()->isIntegerTy())
      RuntimeTarget = Builder.CreateIntToPtr(RuntimeTarget, PtrTy);
    if (!RuntimeTarget->getType()->isPointerTy()) {
      syncError() << "med_llvm_emitter: CRT x87 call has no native pointer\n";
      FatalCodePointerResolution = true;
      return;
    }
    Args.push_back(RuntimeTarget);
    Callee = *Adapter;
    KnownTarget = Callee;
    Target = nullptr;
  }
  std::vector<int> ArgumentIndices;
  for (unsigned I = 0; I < Args.size(); ++I)
    ArgumentIndices.push_back(I);
  bool BoundArgumentBanks = false;
  if (Target && Callee && CI && CI->IntegerRegisterArgCount >= 0 &&
      CI->FloatingRegisterArgCount >= 0 && argumentBanks(*Callee) &&
      !Callee->isVarArg()) {
    // Recovery ends the register arguments at the register that holds the
    // call's own target (MedCallInfo::TargetRegisterArgIdx): no argument is
    // set there.  A routine whose parameters continue on the stack still
    // names that register before them (AAPCS skips r3 for a 64-bit argument
    // that does not fit), so it receives the target, as the machine passes
    // it.
    unsigned Integers = CI->IntegerRegisterArgCount;
    std::vector<llvm::Value *> Values = Args;
    std::optional<unsigned> HeldTarget;
    if (CI->TargetRegisterArgIdx >= 0 &&
        static_cast<unsigned>(CI->TargetRegisterArgIdx) == Integers &&
        argumentBanks(*Callee)->first > Integers && Integers <= Values.size()) {
      HeldTarget = Integers;
      Values.insert(Values.begin() + Integers, GetInput(0));
      ++Integers;
    }
    auto Bound = bindResolvedLLVMArguments(
        *Callee, Values, Integers, CI->FloatingRegisterArgCount, Builder,
        CI->StackArgumentOffsets, CI->StackArgumentSlotBytes);
    if (!Bound) {
      syncError() << "med_llvm_emitter: resolved indirect call to "
                  << Callee->getName()
                  << " lacks recovered argument bank or stack values\n";
      FatalCodePointerResolution = true;
      return;
    }
    ArgumentIndices = std::move(Bound->SourceIndices);
    // Source indices name CI->Args, which holds no value for the target.
    if (HeldTarget) {
      for (int &Index : ArgumentIndices) {
        if (Index == static_cast<int>(*HeldTarget))
          Index = -1;
        else if (Index > static_cast<int>(*HeldTarget))
          --Index;
      }
    }
    Args = std::move(Bound->Values);
    BoundArgumentBanks = true;
  }
  if (Op.ScalarCallSignature &&
      !Op.ScalarCallSignature->ParameterElements.empty()) {
    const auto &Signature = *Op.ScalarCallSignature;
    if (!Callee ||
        Callee->getFunctionType() != floatingCallType(*Ctx, Signature) ||
        Args.size() != Signature.Scalars.size()) {
      syncError()
          << "med_llvm_emitter: HFA call lost its complete source groups\n";
      FatalCodePointerResolution = true;
      return;
    }
    std::vector<llvm::Value *> Grouped;
    std::vector<int> Indices;
    size_t At = 0;
    for (unsigned Count : Signature.ParameterElements) {
      if (!Count) {
        Grouped.push_back(Args[At]);
        Indices.push_back(ArgumentIndices[At++]);
        continue;
      }
      auto *Element =
          floatingCallElementType(*Ctx, Signature.Scalars[At].Bytes);
      auto *Array = llvm::ArrayType::get(Element, Count);
      llvm::Value *Value = llvm::PoisonValue::get(Array);
      for (unsigned I = 0; I < Count; ++I, ++At) {
        llvm::Value *Lane = Args[At];
        const unsigned Bits = Lane->getType()->getPrimitiveSizeInBits();
        if (!Bits || Lane->getType()->isPointerTy()) {
          syncError()
              << "med_llvm_emitter: HFA member has no scalar bit carrier\n";
          FatalCodePointerResolution = true;
          return;
        }
        if (!Lane->getType()->isIntegerTy())
          Lane = Builder.CreateBitCast(Lane, llvm::Type::getIntNTy(*Ctx, Bits));
        Lane = Builder.CreateZExtOrTrunc(
            Lane, llvm::Type::getIntNTy(*Ctx, Signature.Scalars[At].Bytes * 8));
        Lane = Builder.CreateBitCast(Lane, Element);
        Value = Builder.CreateInsertValue(Value, Lane, {I});
      }
      Grouped.push_back(Value);
      Indices.push_back(-1);
    }
    Args = std::move(Grouped);
    ArgumentIndices = std::move(Indices);
  }
  if (Target && Callee && Args.size() < Callee->arg_size()) {
    syncError() << "med_llvm_emitter: resolved indirect call to "
                << Callee->getName() << " lacks recovered arguments\n";
    FatalCodePointerResolution = true;
    return;
  }

  if (Op.Opcode == NdOp::CALL && Callee && CallAddr > 1 &&
      Callee->isDeclaration()) {
    auto VAOr = rewrite_source::getOriginalVA(*Callee);
    if (VAOr && !*VAOr)
      rewrite_source::setOriginalVA(*Callee, CallAddr);
  }

  if (Callee) {
    auto *CalleeTy = Callee->getFunctionType();
    std::vector<llvm::Value *> CoercedArgs;
    for (size_t AI = 0; AI < Args.size(); ++AI) {
      llvm::Value *AV = Args[AI];
      if (AI < CalleeTy->getNumParams()) {
        auto *Want = CalleeTy->getParamType(AI);
        const auto FixedKind =
            CalleeTy->isVarArg() ? libc::varArgFixedParamKind(
                                       Callee->getName().str(), AI, TargetLibC)
                                 : libc::VarArgFixedParamKind::Unknown;
        // Symbolize a pointer-width integer arg resolving to a global address
        // when the callee param is itself an integer.  A callee recovered
        // with integer params (e.g. a variadic call whose args were recovered
        // as fixed integer params) never reaches the pointer-param path
        // below, so a computed `&global[i]` would be passed as a stale
        // absolute VA the callee dereferences into unmapped memory.
        // tryResolvePointerArg only fires for a provable data/rodata address,
        // but fixed libc scalars such as snprintf's size can legitimately
        // overlap a low-address relocatable data segment.  The libc signature
        // table keeps those integers scalar.  The width guard keeps other
        // narrow integers untouched; converting back to the integer param keeps
        // a structurally recovered signature valid. Runs before the
        // type-mismatch block so a same-width (i64==i64) pointer arg is
        // symbolized too.
        const int SourceIndex = ArgumentIndices[AI];
        if (unsigned PtrSz = getTargetRegInfo(TargetArch).PointerSize;
            Want->isIntegerTy() && AV->getType()->isIntegerTy() && CI &&
            SourceIndex >= 0 &&
            static_cast<size_t>(SourceIndex) < CI->Args.size() && PtrSz &&
            FixedKind != libc::VarArgFixedParamKind::Integer &&
            Want->getIntegerBitWidth() == PtrSz * 8)
          if (llvm::Value *Sym = tryResolvePointerArg(
                  CI->Args[SourceIndex], /*FailClosed=*/false, Builder))
            AV = Builder.CreatePtrToInt(Sym, Want);
        if (AV->getType() != Want) {
          llvm::Type *AT = AV->getType();
          if (AT->isIntegerTy() && Want->isIntegerTy()) {
            if (AT->getIntegerBitWidth() > Want->getIntegerBitWidth())
              AV = Builder.CreateTrunc(AV, Want);
            else
              AV = Builder.CreateZExt(AV, Want);
          } else if (AT->isIntegerTy() && Want->isPointerTy()) {
            // Symbolize a computed `&global[index]` so the callee
            // dereferences the recompiled global rather than a stale absolute
            // table VA.
            llvm::Value *Sym = nullptr;
            if (CI && SourceIndex >= 0 &&
                static_cast<size_t>(SourceIndex) < CI->Args.size())
              Sym = tryResolvePointerArg(CI->Args[SourceIndex],
                                         /*FailClosed=*/true, Builder);
            AV = Sym ? Sym : Builder.CreateIntToPtr(AV, Want);
          } else if (AT->isPointerTy() && Want->isIntegerTy()) {
            AV = Builder.CreatePtrToInt(AV, Want);
          } else if (!AT->isPointerTy() && !Want->isPointerTy()) {
            // A floating-point/vector argument is held in its register as an
            // integer (XMM/V tracked as i128, a scalar in the low lane as
            // i64). Bridge through an integer of each side's width to reach
            // the vector ABI type <2 x i64>: a same-width reinterpret, or a
            // scalar in the low lane widened (zero high lane) to the full
            // vector.
            unsigned ABits = AT->getPrimitiveSizeInBits();
            unsigned WBits = Want->getPrimitiveSizeInBits();
            if (ABits && WBits) {
              llvm::Value *AsInt =
                  AT->isIntegerTy()
                      ? AV
                      : Builder.CreateBitCast(
                            AV, llvm::Type::getIntNTy(*Ctx, ABits));
              auto *WInt = llvm::Type::getIntNTy(*Ctx, WBits);
              if (ABits > WBits)
                AsInt = Builder.CreateTrunc(AsInt, WInt);
              else if (ABits < WBits)
                AsInt = Builder.CreateZExt(AsInt, WInt);
              AV = Want->isIntegerTy() ? AsInt
                                       : Builder.CreateBitCast(AsInt, Want);
            }
          }
        }
      }
      CoercedArgs.push_back(AV);
    }
    if (!CalleeTy->isVarArg() && CoercedArgs.size() > CalleeTy->getNumParams())
      CoercedArgs.resize(CalleeTy->getNumParams());
    // A reused variadic declaration still has a mandatory fixed prefix.  An
    // unknown external can be recovered with fewer arguments at a later call
    // site; supply neutral values rather than constructing an invalid call and
    // aborting inside LLVM before the module verifier can report anything.
    while (CoercedArgs.size() < CalleeTy->getNumParams()) {
      auto *PadTy = CalleeTy->getParamType(CoercedArgs.size());
      CoercedArgs.push_back(llvm::Constant::getNullValue(PadTy));
    }
    llvm::Value *Callable = Callee;
    if (PreserveHeaderTarget) {
      Callable = Target;
      if (Callable->getType()->isIntegerTy())
        Callable = Builder.CreateIntToPtr(Callable, PtrTy);
    }
    auto *Call =
        Builder.CreateCall(CalleeTy, Callable, CoercedArgs,
                           CalleeTy->getReturnType()->isVoidTy() ? "" : "call");
    Call->setCallingConv(Callee->getCallingConv());
    Result = Call;
  } else {
    if (!Target) {
      syncError() << "med_llvm_emitter: call has no resolved target\n";
      FatalCodePointerResolution = true;
      return;
    }
    // Indirect call: the callee is unknown, so its LLVM signature is rebuilt
    // from the recovered register classes.  A target with a separate FP
    // register file (AArch64 v0-v7, x86-64 xmm0-7) passes a floating-point
    // argument in a vector register and returns an FP result in one, so an
    // argument whose recovered source is an FP/vector register must take the
    // vector ABI type (<2 x i64>) — typing it as an integer would route it
    // through the GP registers and the callee would read d0 garbage — and an
    // FP-return-register output makes the call return that vector type so the
    // result is read from d0/v0 instead of x0.  A small struct returned
    // across multiple registers (modelCallStructReturn rewrote the output to
    // a flat aggregate temp) is declared as the matching aggregate so every
    // field register (x0:x1 / v0:v1) is read.  Non-FP, non-aggregate indirect
    // calls keep the original integer signature unchanged.
    const auto &ITRI = getTargetRegInfo(TargetArch);
    auto *Vec2 = llvm::FixedVectorType::get(I64Ty, 2);

    // Floating-point arguments past the FP-argument registers (v0-v7 /
    // xmm0-7) are passed on the stack as their natural 8-byte (double) or
    // 4-byte (float) scalar -- not the 16-byte vector ABI type used for the
    // in-register ones.  A <2 x i64> stack argument would occupy two 8-byte
    // slots and shift every slot the callee reads past it (the 10th double of
    // a `double(*)(double x10)` call then lands in the 9th's high half).  The
    // overflow ones share the FP register file with the in-register ones, so
    // the leading FP arguments (assembled FP-register-first, then stack) fill
    // the registers and only the surplus spills.
    const int NumFPRegs =
        static_cast<int>(ITRI.floatingParamRegs(TargetFormat).size());
    int FPSeen = 0;
    // A variadic indirect call (Darwin AArch64, marked by VarArgFixedCount):
    // every variadic argument -- FP included -- is passed on the stack, none
    // in a vector register.  A variadic FP argument therefore takes its
    // natural 8-byte double type, NOT the 16-byte <2 x i64> register ABI type
    // used for the fixed FP prefix (a <2 x i64> would occupy two stack slots
    // and shift every slot the callee reads off its va_list, so
    // `dsum(n, 1.5, 2.5, ...)` would read 1.5, 0, 2.5, 0, ...).
    const bool IsVariadicCall = CI && CI->VarArgFixedCount >= 0;
    std::vector<llvm::Type *> ParamTys;
    for (size_t AI = 0; AI < Args.size(); ++AI) {
      bool IsFP = CI && AI < CI->Args.size() &&
                  CI->Args[AI].Kind == MedVar::Reg &&
                  ITRI.isFPArgReg(CI->Args[AI].RegOff);
      bool IsVariadicTail =
          IsVariadicCall && AI >= static_cast<size_t>(CI->VarArgFixedCount);
      if (IsFP) {
        if (FPSeen < NumFPRegs && !IsVariadicTail) {
          ParamTys.push_back(Vec2);
        } else {
          uint16_t Sz = CI->Args[AI].Size;
          ParamTys.push_back(Sz && Sz <= 4 ? llvm::Type::getFloatTy(*Ctx)
                                           : llvm::Type::getDoubleTy(*Ctx));
        }
        ++FPSeen;
      } else {
        ParamTys.push_back(Args[AI]->getType());
      }
    }

    llvm::Type *IRetTy = RetTy; // default i64
    if (Op.Output.Kind == MedVar::Reg && Op.Output.Size > 0 &&
        ITRI.isVectorReg(Op.Output.RegOff)) {
      IRetTy = Vec2; // scalar FP result returned in v0/d0
    } else if (Op.Output.Kind == MedVar::Reg && Op.Output.Size > 0 &&
               ITRI.isX87StackReg(Op.Output.RegOff)) {
      // Read the complete physical x87 result. Inferring double here would
      // round an unknown long-double result and selects XMM0 on x86-64.
      IRetTy = llvm::Type::getX86_FP80Ty(*Ctx);
    } else if (AggregateRetTy) {
      IRetTy = AggregateRetTy;
    }

    // Coerce each argument to its reconstructed ABI parameter type.  An
    // integer argument that resolves to a recompiled global address is
    // symbolized (the direct path's tryResolvePointerArg) so a computed
    // `&global[i]` passed to a function pointer dereferences the recompiled
    // global rather than a stale absolute VA; FP/vector arguments are bridged
    // through an integer of each side's width to reach <2 x i64>.
    std::vector<llvm::Value *> CoercedArgs;
    for (size_t AI = 0; AI < Args.size(); ++AI) {
      llvm::Value *AV = Args[AI];
      llvm::Type *Want = ParamTys[AI];
      if (Want->isIntegerTy() && AV->getType()->isIntegerTy() && CI &&
          AI < CI->Args.size())
        if (llvm::Value *Sym = tryResolvePointerArg(
                CI->Args[AI], /*FailClosed=*/false, Builder))
          AV = Builder.CreatePtrToInt(Sym, AV->getType());
      if (AV->getType() != Want) {
        llvm::Type *AT = AV->getType();
        if (AT->isIntegerTy() && Want->isIntegerTy()) {
          AV = AT->getIntegerBitWidth() > Want->getIntegerBitWidth()
                   ? Builder.CreateTrunc(AV, Want)
                   : Builder.CreateZExt(AV, Want);
        } else if (AT->isPointerTy() && Want->isIntegerTy()) {
          AV = Builder.CreatePtrToInt(AV, Want);
        } else if (AT->isIntegerTy() && Want->isPointerTy()) {
          AV = Builder.CreateIntToPtr(AV, Want);
        } else if (!AT->isPointerTy() && !Want->isPointerTy()) {
          unsigned ABits = AT->getPrimitiveSizeInBits();
          unsigned WBits = Want->getPrimitiveSizeInBits();
          if (ABits && WBits) {
            llvm::Value *AsInt =
                AT->isIntegerTy() ? AV
                                  : Builder.CreateBitCast(
                                        AV, llvm::Type::getIntNTy(*Ctx, ABits));
            auto *WInt = llvm::Type::getIntNTy(*Ctx, WBits);
            if (ABits > WBits)
              AsInt = Builder.CreateTrunc(AsInt, WInt);
            else if (ABits < WBits)
              AsInt = Builder.CreateZExt(AsInt, WInt);
            AV = Want->isIntegerTy() ? AsInt
                                     : Builder.CreateBitCast(AsInt, Want);
          }
        }
      }
      CoercedArgs.push_back(AV);
    }

    auto *IndirectFT = llvm::FunctionType::get(IRetTy, ParamTys, false);
    if (CI && CI->VarArgFixedCount >= 0 &&
        static_cast<size_t>(CI->VarArgFixedCount) <= ParamTys.size()) {
      std::vector<llvm::Type *> FixedTys(
          ParamTys.begin(), ParamTys.begin() + CI->VarArgFixedCount);
      IndirectFT = llvm::FunctionType::get(IRetTy, FixedTys, true);
    }
    auto *FPtr =
        Builder.CreateIntToPtr(Target, llvm::PointerType::get(*Ctx, 0));
    Result = Builder.CreateCall(IndirectFT, FPtr, CoercedArgs,
                                CoercedArgs.empty() ? "icall" : "call");
  }

  // Itanium call-site ranges are tight around individual calls rather than
  // aligned to machine blocks, so the address this call came from has to
  // survive lowering for the LSDA to be able to name it.
  if (SourceMap && llvm::isa_and_nonnull<llvm::CallBase>(Result) &&
      Op.Addr != InvalidVA && Op.OriginSeq >= 0)
    SourceMap->Observations.push_back(
        {CurMedFunc->Entry, {Op.Addr, Op.OriginSeq}, Result});
  if (auto *Emitted = llvm::dyn_cast_or_null<llvm::CallBase>(Result)) {
    AttachSafetyCallsite(*Emitted);
    if (BoundArgumentBanks) {
      const auto Banks = argumentBanks(*Callee);
      setLLVMArgumentBanks(*Emitted, Banks->first, Banks->second);
    } else if (CI && CI->IntegerRegisterArgCount >= 0 &&
               CI->FloatingRegisterArgCount >= 0 &&
               static_cast<unsigned>(CI->IntegerRegisterArgCount) <=
                   Emitted->arg_size() &&
               static_cast<unsigned>(CI->FloatingRegisterArgCount) <=
                   Emitted->arg_size() - CI->IntegerRegisterArgCount) {
      setLLVMArgumentBanks(
          *Emitted, CI->IntegerRegisterArgCount, CI->FloatingRegisterArgCount,
          CI->StackArgumentOffsets, CI->StackArgumentSlotBytes);
    }
  }
  if (auto *Emitted = llvm::dyn_cast_or_null<llvm::CallInst>(Result)) {
    llvm::Metadata *Address = llvm::ConstantAsMetadata::get(
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(*Ctx), Op.Addr));
    Emitted->setMetadata(language_eh_md::InternalSourceCallAttachment,
                         llvm::MDNode::get(*Ctx, {Address}));
    CallSiteAddrs[Emitted] = Op.Addr;
    // A routine whose C declaration returns a value returns one: its result
    // is not what a void routine leaves in the return register.  The routine
    // is the one a direct call's symbol names, or the import whose slot is
    // called through.
    std::string Routine;
    if (Op.Opcode == NdOp::CALL) {
      Routine = calleeSymbol();
      if (Routine.empty() && Img && CallAddr > 1)
        Routine = Img->getFunctionNameAt(CallAddr);
    } else if (Op.Inputs[0].isConst()) {
      const va_t Slot = Op.Inputs[0].ConstVal;
      if (auto It = EffectiveImportStorageSlots.find(Slot);
          It != EffectiveImportStorageSlots.end() && It->second.Addend == 0)
        Routine = It->second.Name;
      else if (const Import *Imp = Img ? Img->findImportAt(Slot) : nullptr;
               Imp && Imp->IATAddr == Slot)
        Routine = Imp->Name;
    }
    if (Routine.empty() && KnownTarget) {
      if (Callee && Callee->isDeclaration())
        Routine = Callee->getName().str();
      else if (auto It = ImportedSymbolPlaceholders.find(
                   KnownTarget->getName().str());
               It != ImportedSymbolPlaceholders.end() &&
               It->second == KnownTarget)
        Routine = It->first;
    }
    // The same runtime identity owns direct, IAT and register-carried calls.
    // An imported symbol may still be an opaque global rather than a Function;
    // attaching only declaration attributes silently loses returns-twice on
    // those calls, allowing live values to be stranded in volatile registers.
    std::optional<llvm::Attribute::AttrKind> ControlAttribute;
    if (libc::isReturnsTwiceFunction(Routine))
      ControlAttribute = llvm::Attribute::ReturnsTwice;
    else if (libc::isNoReturnFunction(Routine))
      ControlAttribute = llvm::Attribute::NoReturn;
    if (ControlAttribute) {
      Emitted->addFnAttr(*ControlAttribute);
      if (Callee)
        Callee->addFnAttr(*ControlAttribute);
    }
    const auto ReturnsValue = [&](llvm::StringRef Name) {
      return libc::libcReturnsValue(Name, TargetLibC).value_or(false);
    };
    if (!Routine.empty() &&
        (ReturnsValue(Routine) ||
         ReturnsValue(cNameOfSymbol(Routine, TargetFormat, TargetArch))))
      llvm_value_provenance::markSemanticProducer(*Emitted);
  }

  if (Op.DoesNotReturn) {
    if (auto *Emitted = llvm::dyn_cast_or_null<llvm::CallBase>(Result))
      Emitted->addFnAttr(llvm::Attribute::NoReturn);
    Builder.CreateUnreachable();
    return;
  }

  // A direct call returning a small struct by value across multiple registers
  // (modelCallStructReturn rewrote it to a flat aggregate temp): the callee's
  // LLVM type is the matching struct, so flatten the returned aggregate into
  // a packed wide integer that the SUBBYTES extract ops slice into each field
  // return register (the inverse of the callee-side aggregate assembly).
  if (Result && Result->getType()->isStructTy() &&
      Op.Output.Kind == MedVar::Temp && Op.Output.Size > 0) {
    auto *ST = llvm::cast<llvm::StructType>(Result->getType());
    unsigned TotalBits = static_cast<unsigned>(Op.Output.Size) * 8;
    auto *WideTy = llvm::Type::getIntNTy(*Ctx, TotalBits);
    llvm::Value *Acc = llvm::ConstantInt::get(WideTy, 0);
    unsigned CumBits = 0;
    for (unsigned I = 0; I < ST->getNumElements(); ++I) {
      llvm::Value *F = Builder.CreateExtractValue(Result, I);
      auto *FT = ST->getElementType(I);
      unsigned FBits = FT->getPrimitiveSizeInBits();
      llvm::Value *Fi =
          FT->isIntegerTy()
              ? F
              : Builder.CreateBitCast(F, llvm::Type::getIntNTy(*Ctx, FBits));
      Fi = Builder.CreateZExt(Fi, WideTy);
      if (CumBits)
        Fi = Builder.CreateShl(Fi, llvm::ConstantInt::get(WideTy, CumBits));
      Acc = Builder.CreateOr(Acc, Fi);
      CumBits += FBits;
    }
    setVar(Op.Output, Acc, Builder);
    defineCallClobbers(Op, Builder);
    return;
  }

  if (Op.Output.Size > 0) {
    // A void source callee does not define its machine return register.
    // Keep a consumed register value explicitly unknown, never a void SSA
    // operand or an invented zero.
    if (Result && Result->getType()->isVoidTy())
      Result = Builder.CreateFreeze(
          llvm::UndefValue::get(sizeToType(Op.Output.Size)),
          "void_call_result_unknown");
    // A scalar float/double returned through the x87 stack (i386 st0) is held
    // in the 80-bit ST register as the widened FP value, not raw bits: fp-
    // extend it to x86_fp80 so the caller's `fstp` conversion (FLOAT2FLOAT
    // back to float/double) reads the right number instead of a denormal
    // formed from zero-extended narrow-FP bits.
    if (Result && Result->getType()->isFloatingPointTy() &&
        Result->getType()->getPrimitiveSizeInBits() < 80 &&
        getTargetRegInfo(TargetArch).isX87StackReg(Op.Output.RegOff))
      Result = Builder.CreateFPExt(Result, llvm::Type::getX86_FP80Ty(*Ctx));
    setVar(Op.Output, Result, Builder);
  }
  defineCallClobbers(Op, Builder);
  return;
}

} // namespace neverd
