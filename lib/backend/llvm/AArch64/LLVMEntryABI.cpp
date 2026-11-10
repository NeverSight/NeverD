//===- LLVMEntryABI.cpp - Darwin AArch64 entry reconstruction -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind logical source values to the physical carriers consumed by a body.
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/AArch64/LLVMEntryABI.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/LLVMSourceCallValue.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/MedSourceParameterUses.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace neverd {
namespace {
#define LLVM_ENTRY_ABI_TEXT(Name, Text) constexpr char Name[] = Text;
#include "neverd/backend/llvm/AArch64/LLVMEntryABI.def"
#define LLVM_SOURCE_VALUE(Type, Name, Value) constexpr Type Name = Value;
#include "neverd/backend/llvm/LLVMSourceCallValue.def"

struct Piece {
  unsigned Value = 0;
  unsigned SourceByte = 0;
  unsigned DestinationByte = 0;
  unsigned Bytes = 0;
  bool Extend = false;
  bool Signed = false;
};

unsigned valueBytes(const llvm::DataLayout &Layout, llvm::Type *Type) {
  if (Type->isPointerTy())
    return Layout.getPointerTypeSize(Type);
  if (Type->isIntegerTy() || Type->isFloatingPointTy() ||
      llvm::isa<llvm::FixedVectorType>(Type)) {
    const unsigned Bits = Type->getPrimitiveSizeInBits().getFixedValue();
    return Bits % kSourceValueByteBits == 0 ? Bits / kSourceValueByteBits : 0;
  }
  return 0;
}

llvm::Value *integerBits(llvm::IRBuilder<> &Builder, llvm::Value *Value) {
  const auto &Layout = Builder.GetInsertBlock()->getModule()->getDataLayout();
  auto *Type = llvm::IntegerType::get(Builder.getContext(),
                                      valueBytes(Layout, Value->getType()) *
                                          kSourceValueByteBits);
  if (Value->getType()->isPointerTy())
    return Builder.CreatePtrToInt(Value, Type);
  return Value->getType() == Type ? Value : Builder.CreateBitCast(Value, Type);
}

llvm::Value *fromBits(llvm::IRBuilder<> &Builder, llvm::Value *Value,
                      llvm::Type *Type) {
  if (Type->isPointerTy())
    return Builder.CreateIntToPtr(Value, Type);
  return Value->getType() == Type ? Value : Builder.CreateBitCast(Value, Type);
}

// Arrays and structs are the canonical ABI coercions, not the logical
// record's spelling. Their elements have no intervening padding.
llvm::Value *flatten(llvm::IRBuilder<> &Builder, llvm::Value *Value) {
  auto *Type = Value->getType();
  if (!Type->isAggregateType())
    return integerBits(Builder, Value);
  const auto &Layout = Builder.GetInsertBlock()->getModule()->getDataLayout();
  auto *Integer = llvm::IntegerType::get(Builder.getContext(),
                                         Layout.getTypeStoreSize(Type) *
                                             kSourceValueByteBits);
  llvm::Value *Bits = llvm::ConstantInt::get(Integer, 0);
  const unsigned Count = Type->isArrayTy() ? Type->getArrayNumElements()
                                           : Type->getStructNumElements();
  unsigned Offset = 0;
  for (unsigned I = 0; I < Count; ++I) {
    auto *Element = integerBits(Builder, Builder.CreateExtractValue(Value, I));
    auto *Part = Builder.CreateZExtOrTrunc(Element, Integer);
    if (Offset)
      Part = Builder.CreateShl(Part, Offset);
    Bits = Builder.CreateOr(Bits, Part);
    Offset += Element->getType()->getIntegerBitWidth();
  }
  return Bits;
}

llvm::Value *assemble(llvm::IRBuilder<> &Builder,
                      llvm::ArrayRef<llvm::Value *> Values,
                      llvm::ArrayRef<Piece> Pieces, unsigned Bytes) {
  auto *Integer = Builder.getIntNTy(Bytes * kSourceValueByteBits);
  llvm::Value *Bits = llvm::ConstantInt::get(Integer, 0);
  for (const Piece &P : Pieces) {
    auto *Value = Values[P.Value];
    if (P.SourceByte)
      Value = Builder.CreateLShr(Value, P.SourceByte * kSourceValueByteBits);
    Value = Builder.CreateZExtOrTrunc(
        Value, Builder.getIntNTy(P.Bytes * kSourceValueByteBits));
    Value = P.Extend && P.Signed ? Builder.CreateSExtOrTrunc(Value, Integer)
                                 : Builder.CreateZExtOrTrunc(Value, Integer);
    if (P.DestinationByte)
      Value =
          Builder.CreateShl(Value, P.DestinationByte * kSourceValueByteBits);
    Bits = Builder.CreateOr(Bits, Value);
  }
  return Bits;
}

bool sameRegister(const SourceABIValueLocation &A,
                  const SourceABIValueLocation &B) {
  return A.Kind != SourceABICarrierKind::Stack &&
         B.Kind != SourceABICarrierKind::Stack &&
         A.RegisterOffset == B.RegisterOffset;
}

} // namespace

llvm::Expected<llvm::Function *>
reconstructDarwinAArch64Entry(llvm::Function &Body, const MedFunc &Source) {
  auto Fail = [](const char *Message) {
    return llvm::createStringError(std::errc::invalid_argument, Message);
  };
  auto Identity = rewrite_source::getOriginalVA(Body);
  if (!Identity)
    return Identity.takeError();
  if (!*Identity || **Identity != Source.Entry)
    return Fail(kWrongSource);
  const auto &Triple = Body.getParent()->getTargetTriple();
  std::string Diagnostic;
  if (Triple.getArch() != llvm::Triple::aarch64 || !Triple.isOSDarwin() ||
      !Source.SourceTypeHint || Source.IsVariadic || Body.isVarArg() ||
      Body.isDeclaration() || Body.arg_size() != Source.Params.size() ||
      Body.getCallingConv() != llvm::CallingConv::C ||
      Source.SourceTypeHint->Architecture != Arch::AArch64 ||
      !Source.SourceTypeHint->HasExplicitABI ||
      Source.SourceTypeHint->Convention !=
          SourceFunctionTypeHint::ConventionKind::C ||
      Body.hasFnAttribute(llvm::Attribute::Naked) ||
      Body.hasFnAttribute(llvm::Attribute::ReturnsTwice) ||
      !validateSourceABI(*Source.SourceTypeHint, Diagnostic))
    return Fail(kInvalidSignature);
  const auto &Signature = *Source.SourceTypeHint;
  auto Canonical = Signature;
  if (!assignDarwinFixedSourceABI(Canonical, Arch::AArch64, Diagnostic) ||
      !equalSourceABIs(Canonical, Signature))
    return Fail(kInvalidSignature);
  const auto Bindings = sourceABIParameters(Signature);
  const auto &Layout = Body.getParent()->getDataLayout();
  const auto &Registers = getTargetRegInfo(Arch::AArch64);
  const auto Stack = Registers.integerArgumentLayout(BinaryFormat::MachO);
  const bool IndirectResult = Signature.ReturnLocation.Kind ==
                              SourceABICarrierKind::IndirectResultPointer;
  std::vector<llvm::Type *> Types;
  if (IndirectResult)
    Types.push_back(llvm::PointerType::getUnqual(Body.getContext()));
  for (const auto &Parameter : Signature.Parameters)
    Types.push_back(
        Parameter.IndirectByValue
            ? llvm::PointerType::getUnqual(Body.getContext())
            : llvmSourceCallValueType(Body.getContext(), Parameter.Type, true));
  auto *ResultType = IndirectResult
                         ? llvm::Type::getVoidTy(Body.getContext())
                         : llvmSourceCallValueType(Body.getContext(),
                                                   Signature.ReturnType, false);
  if (!ResultType || llvm::is_contained(Types, nullptr))
    return Fail(kInvalidSignature);
  auto *EntryType = llvm::FunctionType::get(ResultType, Types, false);

  std::vector<std::vector<Piece>> Arguments(Body.arg_size());
  std::vector<bool> OmittedArguments(Body.arg_size(), false);
  std::optional<std::map<uint64_t, uint64_t>> Observed;
  bool CheckedDemands = false;
  for (unsigned I = 0; I < Source.Params.size(); ++I) {
    const auto &Parameter = Source.Params[I];
    auto *Argument = Body.getArg(I);
    const unsigned Bytes = valueBytes(Layout, Argument->getType());
    if (!Bytes || Bytes > Registers.VecRegStride)
      return Fail(kMissingCarrier);
    // Synthetic unused bank padding has no incoming value. An attributed
    // parameter still has a contract (for example noundef or byval), so it
    // must receive its declared value even when the body never reads it.
    if (Argument->use_empty() &&
        !Body.getAttributes().getParamAttrs(I).hasAttributes()) {
      OmittedArguments[I] = true;
      continue;
    }
    const bool OnStack = Parameter.RegOff == kNoParamReg;
    if (OnStack && Parameter.Id < static_cast<int>(Stack.Registers.size()))
      return Fail(kMissingCarrier);
    const int64_t Offset =
        OnStack ? (Parameter.Id - Stack.Registers.size()) * Stack.SlotBytes +
                      Stack.EntryStackBase
                : 0;
    llvm::APInt Covered(Bytes * kSourceValueByteBits, 0);
    auto Add = [&](const SourceABIValueLocation &Location, unsigned Index,
                   unsigned SourceByte, bool Signed) {
      unsigned First = 0, Destination = 0, Count = Location.ValueBytes;
      if (OnStack) {
        if (Location.Kind != SourceABICarrierKind::Stack)
          return;
        const auto Start = std::max(Offset, Location.EntryStackOffset);
        const auto End = std::min(Offset + Bytes, Location.EntryStackOffset +
                                                      Location.ValueBytes);
        if (Start >= End)
          return;
        First = Start - Location.EntryStackOffset;
        Destination = Start - Offset;
        Count = End - Start;
      } else if (Location.Kind == SourceABICarrierKind::Stack ||
                 Location.RegisterOffset != Parameter.RegOff) {
        return;
      }
      const bool Extend = !OnStack && Location.ExtendTo32Bits;
      const unsigned Defined = Extend ? sizeof(uint32_t) : Count;
      const unsigned End = std::min(Bytes, Destination + Defined);
      if (Destination >= End)
        return;
      Covered.setBits(Destination * kSourceValueByteBits,
                      End * kSourceValueByteBits);
      Arguments[I].push_back({Index, SourceByte + First, Destination,
                              std::min(Count, Bytes), Extend, Signed});
    };
    if (IndirectResult)
      Add(Signature.ReturnLocation, 0, 0, false);
    for (const auto &Binding : Bindings)
      Add(Binding.Location, Binding.ParameterIndex + unsigned(IndirectResult),
          Binding.ByteOffset, Binding.Type->IsSigned);
    if (!Covered.isAllOnes()) {
      if (OnStack || Argument->use_empty())
        return Fail(kMissingCarrier);
      if (!CheckedDemands) {
        CheckedDemands = true;
        if (!Source.Blocks.empty())
          Observed = observedMedSourceEntryBytes(Source, Signature);
      }
      if (!Observed)
        return Fail(kMissingCarrier);
      auto Found = Observed->find(Parameter.RegOff);
      const uint64_t Demand = Found == Observed->end() ? 0 : Found->second;
      for (unsigned Byte = 0; Byte < Bytes; ++Byte)
        if ((Demand & (uint64_t(1) << Byte)) &&
            !Covered[Byte * kSourceValueByteBits])
          return Fail(kMissingCarrier);
    }
  }

  std::vector<SourceABIValueLocation> Returns;
  std::vector<llvm::Type *> BodyResults;
  if (auto *Record = llvm::dyn_cast<llvm::StructType>(Body.getReturnType())) {
    if (Source.MultiReturn.size() != Record->getNumElements())
      return Fail(kMissingResult);
    for (unsigned I = 0; I < Source.MultiReturn.size(); ++I) {
      const auto &R = Source.MultiReturn[I];
      BodyResults.push_back(Record->getElementType(I));
      Returns.push_back({R.IsFP ? SourceABICarrierKind::FloatingRegister
                                : SourceABICarrierKind::IntegerRegister,
                         R.RegOff, 0, R.Size});
    }
  } else if (!Body.getReturnType()->isVoidTy()) {
    const bool Floating =
        Source.ReturnsVectorRegister ||
        (Source.ReturnType && Source.ReturnType->Kind == NdTypeKind::Float);
    BodyResults.push_back(Body.getReturnType());
    Returns.push_back(
        {Floating ? SourceABICarrierKind::FloatingRegister
                  : SourceABICarrierKind::IntegerRegister,
         Floating ? Registers.fpReturnModelReg() : Registers.IntReturnReg, 0,
         static_cast<uint16_t>(valueBytes(Layout, Body.getReturnType()))});
  }
  for (auto *Type : BodyResults)
    if (!valueBytes(Layout, Type) ||
        valueBytes(Layout, Type) > Registers.VecRegStride)
      return Fail(kMissingResult);
  std::vector<Piece> Results;
  if (!ResultType->isVoidTy()) {
    auto Locations = Signature.ReturnComponents;
    auto Members = sourceABIValueMembers(Signature.ReturnType);
    if (Locations.empty()) {
      Locations.push_back(Signature.ReturnLocation);
      Members = {{Signature.ReturnType, 0}};
    }
    for (unsigned I = 0; I < Locations.size(); ++I) {
      const auto &Location = Locations[I];
      auto Found = llvm::find_if(Returns, [&](const auto &R) {
        return sameRegister(R, Location) && R.Kind == Location.Kind;
      });
      if (Found == Returns.end())
        return Fail(kMissingResult);
      const unsigned Index = Found - Returns.begin();
      if (Found->ValueBytes < Location.ValueBytes ||
          valueBytes(Layout, BodyResults[Index]) < Location.ValueBytes)
        return Fail(kMissingResult);
      Results.push_back({Index, 0, Members[I].ByteOffset, Location.ValueBytes});
    }
  }

  // A direct call already uses this physical-carrier interface. Preserve it
  // even in recursion and invokes; all other uses acquire the source entry.
  std::vector<llvm::CallBase *> DirectCalls;
  for (auto &Function : *Body.getParent())
    for (auto &Block : Function)
      for (auto &Instruction : Block)
        if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction))
          if (Call->getCalledOperand()->stripPointerCasts() == &Body) {
            if (Call->getMetadata(kAArch64SourceEntryCall)) {
              if (Call->getFunctionType() != EntryType ||
                  Call->getCallingConv() != llvm::CallingConv::C)
                return Fail(kUnsupportedUse);
              continue;
            }
            if (Call->getFunctionType() != Body.getFunctionType() ||
                Call->getCallingConv() != Body.getCallingConv())
              return Fail(kUnsupportedUse);
            DirectCalls.push_back(Call);
          }

  auto *Entry =
      llvm::Function::Create(EntryType, Body.getLinkage(),
                             Body.getAddressSpace(), {}, Body.getParent());
  Entry->setVisibility(Body.getVisibility());
  Entry->setDSOLocal(Body.isDSOLocal());
  Entry->setAttributes(llvm::AttributeList::get(
      Body.getContext(), Body.getAttributes().getFnAttrs(), {}, {}));
  // allocsize indexes the old physical argument list. The new logical list
  // has a different order and may combine several of those carriers.
  Entry->removeFnAttr(llvm::Attribute::AllocSize);
  if (IndirectResult)
    Entry->addParamAttr(
        0, llvm::Attribute::getWithStructRetType(
               Body.getContext(),
               llvmSourceCallValueType(Body.getContext(), Signature.ReturnType,
                                       false)));
  if (!IndirectResult && Signature.ReturnLocation.ExtendTo32Bits)
    Entry->addRetAttr(Signature.ReturnType->IsSigned ? llvm::Attribute::SExt
                                                     : llvm::Attribute::ZExt);
  for (unsigned I = 0; I < Signature.Parameters.size(); ++I)
    if (Signature.Parameters[I].Location.ExtendTo32Bits)
      Entry->addParamAttr(I + unsigned(IndirectResult),
                          Signature.Parameters[I].Type->IsSigned
                              ? llvm::Attribute::SExt
                              : llvm::Attribute::ZExt);

  Entry->takeName(&Body);
  Body.setName(Entry->getName() + kBodySuffix);
  // Interior labels still belong to the carrier body. Moving only the
  // function operand of a blockaddress would leave a foreign basic block.
  Body.replaceUsesWithIf(Entry, [](llvm::Use &Use) {
    return !llvm::isa<llvm::BlockAddress>(Use.getUser());
  });
  for (auto *Call : DirectCalls)
    Call->setCalledOperand(&Body);
  Body.setLinkage(llvm::GlobalValue::InternalLinkage);
  Body.setVisibility(llvm::GlobalValue::DefaultVisibility);
  Body.setMetadata(rewrite_source::FunctionAttachment, nullptr);
  rewrite_source::setOriginalVA(*Entry, Source.Entry);

  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Body.getContext(), kEntryBlock, Entry));
  std::vector<llvm::Value *> Values;
  for (auto &Argument : Entry->args())
    Values.push_back(flatten(Builder, &Argument));
  std::vector<llvm::Value *> Args;
  for (unsigned I = 0; I < Body.arg_size(); ++I) {
    auto *Type = Body.getArg(I)->getType();
    Args.push_back(OmittedArguments[I]
                       ? llvm::Constant::getNullValue(Type)
                       : fromBits(Builder,
                                  assemble(Builder, Values, Arguments[I],
                                           valueBytes(Layout, Type)),
                                  Type));
  }
  auto *Call = Builder.CreateCall(&Body, Args);
  Call->setAttributes(
      Body.getAttributes().removeFnAttributes(Body.getContext()));
  if (ResultType->isVoidTy()) {
    Builder.CreateRetVoid();
  } else {
    std::vector<llvm::Value *> Parts;
    for (unsigned I = 0; I < BodyResults.size(); ++I)
      Parts.push_back(
          integerBits(Builder, Body.getReturnType()->isStructTy()
                                   ? Builder.CreateExtractValue(Call, I)
                                   : Call));
    auto *Bits =
        assemble(Builder, Parts, Results, Layout.getTypeStoreSize(ResultType));
    if (auto *Record = llvm::dyn_cast<llvm::StructType>(ResultType)) {
      llvm::Value *Result = llvm::PoisonValue::get(Record);
      unsigned Offset = 0;
      for (unsigned I = 0; I < Record->getNumElements(); ++I) {
        auto *Type = Record->getElementType(I);
        const unsigned Width = valueBytes(Layout, Type) * kSourceValueByteBits;
        auto *Part = Builder.CreateZExtOrTrunc(Builder.CreateLShr(Bits, Offset),
                                               Builder.getIntNTy(Width));
        Result =
            Builder.CreateInsertValue(Result, fromBits(Builder, Part, Type), I);
        Offset += Width;
      }
      Builder.CreateRet(Result);
    } else {
      Builder.CreateRet(fromBits(Builder, Bits, ResultType));
    }
  }
  return Entry;
}

} // namespace neverd
