//===- SourceABI.cpp - Checked source value and carrier contracts --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validates complete logical source types and their physical value carriers.

#include "neverd/ir/SourceABI.h"

#include "neverd/Limits.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceParameterPlacement.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/lift/X86/X86Regs.h"

#include <algorithm>
#include <set>
#include <utility>

namespace neverd {
bool isSourceBooleanType(const TypeRef &Type) {
  return Type && Type->Kind == NdTypeKind::Int && Type->Size == 1 &&
         !Type->IsSigned && Type->SourceName == kSourceBooleanCType;
}

namespace {
#define SOURCE_ABI_CARRIER_VALUE(Type, Name, Value) constexpr Type Name = Value;
#define SOURCE_ABI_CARRIER_TEXT(Name, Text) constexpr char Name[] = Text;
#include "SourceABICarriers.def"

bool sameLocation(const SourceABIValueLocation &Left,
                  const SourceABIValueLocation &Right) {
  return Left.Kind == Right.Kind &&
         Left.RegisterOffset == Right.RegisterOffset &&
         Left.EntryStackOffset == Right.EntryStackOffset &&
         Left.ValueBytes == Right.ValueBytes &&
         Left.ExtendTo32Bits == Right.ExtendTo32Bits;
}

bool equalTypes(const TypeRef &Left, const TypeRef &Right, unsigned Depth,
                unsigned &Remaining) {
  if (!Remaining || !Left || !Right ||
      Depth > ::neverd::limits::ir::kSourceABIEqualTypesDepthLimit ||
      Left->Kind != Right->Kind || Left->Size != Right->Size ||
      Left->IsSigned != Right->IsSigned)
    return false;
  --Remaining;
  switch (Left->Kind) {
  case NdTypeKind::Void:
    return Left->Size == 0;
  case NdTypeKind::Int:
    if (Left->SourceName == kSourceBooleanCType ||
        Right->SourceName == kSourceBooleanCType)
      return isSourceBooleanType(Left) && isSourceBooleanType(Right);
    if (Left->Size == 16 &&
        (Left->SourceName == kSourceAArch64Vector128CType ||
         Right->SourceName == kSourceAArch64Vector128CType) &&
        Left->SourceName != Right->SourceName)
      return false;
    return Left->Size == 1 || Left->Size == 2 || Left->Size == 4 ||
           Left->Size == 8 || Left->Size == 16;
  case NdTypeKind::Float:
    return Left->Size == 4 || Left->Size == 8;
  case NdTypeKind::Ptr:
    if (Left->Size != 8 || !Left->Pointee || !Right->Pointee)
      return false;
    // A named opaque pointee supplies identity, not a by-value layout.
    if (Left->Pointee->Kind == NdTypeKind::Struct &&
        Right->Pointee->Kind == NdTypeKind::Struct && !Left->Pointee->IsEnum &&
        !Right->Pointee->IsEnum && Left->Pointee->Fields.empty() &&
        Right->Pointee->Fields.empty() && !Left->Pointee->SourceName.empty() &&
        !Right->Pointee->SourceName.empty())
      return Left->Pointee->SourceName == Right->Pointee->SourceName &&
             Left->Pointee->Size == Right->Pointee->Size;
    return equalTypes(Left->Pointee, Right->Pointee, Depth + 1, Remaining);
  case NdTypeKind::Struct: {
    const auto Layout = NdType::makeStruct(Left->Fields);
    if (!Layout || Left->Size != Layout->Size || Right->Size != Layout->Size ||
        Left->Alignment != Layout->Alignment ||
        Right->Alignment != Layout->Alignment ||
        Left->FieldOffsets != Layout->FieldOffsets ||
        Right->FieldOffsets != Layout->FieldOffsets ||
        Left->Fields.size() != Right->Fields.size())
      return false;
    for (size_t I = 0; I < Left->Fields.size(); ++I)
      if (!equalTypes(Left->Fields[I], Right->Fields[I], Depth + 1, Remaining))
        return false;
    return true;
  }
  case NdTypeKind::Func:
    // These projections currently support the ordinary platform C convention.
    // A displayable stdcall/vectorcall callback is not evidence for that ABI.
    if (Left->FunctionConvention != types::CallingConvention::Unspecified ||
        Right->FunctionConvention != types::CallingConvention::Unspecified ||
        Left->Size != 0 || !Left->RetType || !Right->RetType ||
        Left->RetType->Kind == NdTypeKind::Func ||
        Left->ParamTypes.size() >
            ::neverd::limits::ir::kSourceABIEqualTypesLeftParamTypesSizeLimit ||
        Left->ParamTypes.size() != Right->ParamTypes.size() ||
        !equalTypes(Left->RetType, Right->RetType, Depth + 1, Remaining))
      return false;
    for (size_t I = 0; I < Left->ParamTypes.size(); ++I)
      if (!Left->ParamTypes[I] ||
          Left->ParamTypes[I]->Kind == NdTypeKind::Void ||
          Left->ParamTypes[I]->Kind == NdTypeKind::Func ||
          !equalTypes(Left->ParamTypes[I], Right->ParamTypes[I], Depth + 1,
                      Remaining))
        return false;
    return true;
  default:
    return false;
  }
}

bool scalarType(const TypeRef &Type) {
  if (!Type)
    return false;
  if (Type->Kind == NdTypeKind::Ptr) {
    // A named display record has an independently owned layout, and may be
    // incomplete or recursive. Passing its address only transports pointer
    // bits; it does not pass a record or authenticate that pointee's layout.
    TypeRef Pointee = Type;
    for (unsigned Depth = 0;
         Depth < ::neverd::limits::ir::kSourceABIScalarTypeDepthLimit &&
         Pointee && Pointee->Kind == NdTypeKind::Ptr;
         ++Depth) {
      if (Pointee->Size != 8)
        return false;
      Pointee = Pointee->Pointee;
      if (Pointee && Pointee->Kind == NdTypeKind::Struct &&
          !Pointee->SourceName.empty() && Pointee->Fields.empty())
        return true;
    }
    return Type->Size == 8 && equalSourceTypes(Type, Type);
  }
  if (Type->Kind == NdTypeKind::Float)
    return Type->Size == 4 || Type->Size == 8;
  return Type->Kind == NdTypeKind::Int && (Type->Size == 1 || Type->Size == 2 ||
                                           Type->Size == 4 || Type->Size == 8);
}

bool fail(std::string &Diagnostic, const char *Message) {
  Diagnostic = Message;
  return false;
}

// The compiler-backed mixed Swift result is a flat record of three doubles
// followed by one opaque pointer. Its logical layout is independent of the
// three FP return lanes and the first integer return lane. Other mixtures,
// nesting, typed pointees and by-value parameters need separate contracts.
bool mixedSwiftResult(const TypeRef &Type,
                      const std::vector<SourceAggregateMember> &Members) {
  if (!Type || Type->Kind != NdTypeKind::Struct || Type->Size != 32 ||
      Type->Fields.size() != 4 || Members.size() != 4)
    return false;
  for (size_t I = 0; I != 4; ++I) {
    const auto &M = Members[I];
    if (M.ByteOffset != I * 8 || !M.Type || M.Type->Size != 8 ||
        (I < 3 ? M.Type->Kind != NdTypeKind::Float
               : M.Type->Kind != NdTypeKind::Ptr || !M.Type->Pointee ||
                     M.Type->Pointee->Kind != NdTypeKind::Void))
      return false;
  }
  return true;
}

std::vector<SourceABIValueLocation>
mixedSwiftResultLocations(Arch Architecture) {
  const auto &TRI = getTargetRegInfo(Architecture);
  return {{SourceABICarrierKind::FloatingRegister, TRI.FPParamRegs[0], 0, 8},
          {SourceABICarrierKind::FloatingRegister, TRI.FPParamRegs[1], 0, 8},
          {SourceABICarrierKind::FloatingRegister, TRI.FPParamRegs[2], 0, 8},
          {SourceABICarrierKind::IntegerRegister, TRI.IntReturnReg, 0, 8}};
}

// Swift's UTF-8 repair result is {i64, ptr, i1}. The Boolean has one byte of
// logical storage but only one defined physical bit. Do not admit a byte,
// nested String layout, typed pointee, or ordinary C indirect result here.
bool swiftBooleanTupleResult(
    const TypeRef &Type, const std::vector<SourceAggregateMember> &Members) {
  return Type && Type->Kind == NdTypeKind::Struct && Type->Size == 24 &&
         Type->Fields.size() == 3 && Members.size() == 3 &&
         Members[0].ByteOffset == 0 &&
         Members[0].Type->Kind == NdTypeKind::Int &&
         Members[0].Type->Size == 8 && !Members[0].Type->IsSigned &&
         Members[1].ByteOffset == 8 &&
         Members[1].Type->Kind == NdTypeKind::Ptr &&
         Members[1].Type->Size == 8 && Members[1].Type->Pointee &&
         Members[1].Type->Pointee->Kind == NdTypeKind::Void &&
         Members[2].ByteOffset == 16 && isSourceBooleanType(Members[2].Type);
}

std::vector<SourceABIValueLocation>
swiftBooleanTupleResultLocations(Arch Architecture) {
  const auto &TRI = getTargetRegInfo(Architecture);
  return {{SourceABICarrierKind::IntegerRegister, TRI.IntReturnRegs[0], 0, 8},
          {SourceABICarrierKind::IntegerRegister, TRI.IntReturnRegs[1], 0, 8},
          {SourceABICarrierKind::BooleanRegister,
           Architecture == Arch::AArch64 ? a64reg::X2 : x86reg::RCX, 0, 1}};
}

// Swift 6.1.2 emits a CGPoint instance transform as
// {double,double}(double,double,ptr swiftself) on both Darwin architectures.
// Keep this complete input/result/context shape separate from general HFAs.
bool swiftPointTransform(const SourceFunctionTypeHint &Hint) {
  const auto Point = [](const TypeRef &Type) {
    const auto Members = sourceAggregateMembers(Type);
    return Type && Type->Kind == NdTypeKind::Struct && Type->Size == 16 &&
           Type->Alignment == 8 && Type->Fields.size() == 2 &&
           std::all_of(Type->Fields.begin(), Type->Fields.end(),
                       [](const auto &Field) {
                         return Field && Field->Kind == NdTypeKind::Float &&
                                Field->Size == 8;
                       }) &&
           Members.size() == 2 && Members[0].ByteOffset == 0 &&
           Members[1].ByteOffset == 8 &&
           std::all_of(Members.begin(), Members.end(), [](const auto &M) {
             return M.Type && M.Type->Kind == NdTypeKind::Float &&
                    M.Type->Size == 8;
           });
  };
  return Point(Hint.ReturnType) && Hint.Parameters.size() == 2 &&
         Point(Hint.Parameters[0].Type) &&
         Hint.Parameters[0].TheRole ==
             SourceParameterTypeHint::Role::Ordinary &&
         !Hint.Parameters[0].IndirectByValue &&
         Hint.Parameters[1].TheRole ==
             SourceParameterTypeHint::Role::SwiftContext &&
         equalSourceTypes(Hint.Parameters[1].Type,
                          NdType::makePtr(NdType::makeVoid()));
}

bool swiftFixedShape(const SourceFunctionTypeHint &Hint, Arch Architecture) {
  if (Architecture != Arch::AArch64 && Architecture != Arch::X64)
    return false;
  const auto Word = [](const TypeRef &T) {
    return scalarType(T) && T->Size == 8 && T->Kind != NdTypeKind::Float;
  };
  const auto Scalar = [](const TypeRef &T) {
    return scalarType(T) && T->Kind != NdTypeKind::Float &&
           T->SourceName != kSourceBooleanCType;
  };
  if (!Hint.ReturnType ||
      Hint.Parameters.size() >
          ::neverd::limits::ir::
              kSourceABISwiftFixedShapeHintParametersSizeLimit)
    return false;
  const auto ReturnMembers = sourceAggregateMembers(Hint.ReturnType);
  const bool MixedResult = mixedSwiftResult(Hint.ReturnType, ReturnMembers);
  const bool PointTransform = swiftPointTransform(Hint);
  // Swift 6.1.2 CoreGraphics point actions on arm64 and x86_64 have exactly
  // void(double, double, ptr swiftself). This does not admit arbitrary x86_64
  // floating signatures, aggregate arguments, or stack-passed FP values.
  const bool PointAction =
      Hint.ReturnType->Kind == NdTypeKind::Void &&
      Hint.Parameters.size() == 3 &&
      std::all_of(Hint.Parameters.begin(), Hint.Parameters.begin() + 2,
                  [](const auto &P) {
                    return P.Type && P.Type->Kind == NdTypeKind::Float &&
                           P.Type->Size == 8 &&
                           P.TheRole == SourceParameterTypeHint::Role::Ordinary;
                  }) &&
      Hint.Parameters[2].TheRole ==
          SourceParameterTypeHint::Role::SwiftContext &&
      equalSourceTypes(Hint.Parameters[2].Type,
                       NdType::makePtr(NdType::makeVoid()));
  size_t FloatingParameters = 0;
  for (const auto &Parameter : Hint.Parameters) {
    if (Parameter.Type && Parameter.Type->Kind == NdTypeKind::Float) {
      // arm64 scalar lanes are v0..v7. x86_64 admits only the observed mixed
      // result and point-action shapes, both within its FP register bank.
      if ((Architecture != Arch::AArch64 &&
           ((!MixedResult && !PointAction) || Parameter.Type->Size != 8 ||
            FloatingParameters == (PointAction ? 2U : 3U))) ||
          Parameter.TheRole != SourceParameterTypeHint::Role::Ordinary ||
          !scalarType(Parameter.Type) ||
          ++FloatingParameters >
              getTargetRegInfo(Architecture).FPParamRegs.size())
        return false;
    } else if (Parameter.Type && Parameter.Type->Kind == NdTypeKind::Struct) {
      // CGPoint transforms use two lanes on both architectures; CGRect uses
      // four lanes on arm64. Other records need their own compiler contract.
      const auto Members = sourceAggregateMembers(Parameter.Type);
      if ((!PointTransform && Architecture != Arch::AArch64) ||
          Parameter.TheRole != SourceParameterTypeHint::Role::Ordinary ||
          Members.size() != (PointTransform ? 2U : 4U) ||
          Parameter.Type->Size != (PointTransform ? 16U : 32U) ||
          !std::all_of(Members.begin(), Members.end(),
                       [](const auto &M) {
                         return M.Type && M.Type->Kind == NdTypeKind::Float &&
                                M.Type->Size == 8;
                       }) ||
          (FloatingParameters += Members.size()) >
              getTargetRegInfo(Architecture).FPParamRegs.size())
        return false;
    } else if (!Scalar(Parameter.Type)) {
      return false;
    }
  }
  unsigned IndirectResults = 0, Contexts = 0, ErrorResults = 0;
  for (size_t I = 0; I < Hint.Parameters.size(); ++I) {
    const auto &Parameter = Hint.Parameters[I];
    switch (Parameter.TheRole) {
    case SourceParameterTypeHint::Role::Ordinary:
      break;
    case SourceParameterTypeHint::Role::SwiftIndirectResult:
      if (I != 0 || Parameter.Type->Kind != NdTypeKind::Ptr ||
          Hint.ReturnType->Kind != NdTypeKind::Void || ++IndirectResults != 1)
        return false;
      break;
    case SourceParameterTypeHint::Role::SwiftContext:
      if (Parameter.Type->Kind != NdTypeKind::Ptr || ++Contexts != 1)
        return false;
      break;
    case SourceParameterTypeHint::Role::SwiftErrorResult:
      if (++ErrorResults != 1 || I + 1 != Hint.Parameters.size() || !I ||
          Hint.Parameters[I - 1].TheRole !=
              SourceParameterTypeHint::Role::SwiftContext ||
          !equalSourceTypes(Parameter.Type, NdType::makePtr(NdType::makePtr(
                                                NdType::makeVoid()))))
        return false;
      break;
    default:
      return false;
    }
  }
  if (Word(Hint.ReturnType) ||
      (Hint.ReturnType->Kind == NdTypeKind::Int &&
       (Hint.ReturnType->Size == 1 || Hint.ReturnType->Size == 4)) ||
      Hint.ReturnType->Kind == NdTypeKind::Void ||
      (Hint.ReturnType->Kind == NdTypeKind::Int && Hint.ReturnType->Size == 16))
    return true;
  if (Architecture == Arch::AArch64 &&
      Hint.ReturnType->Kind == NdTypeKind::Float && scalarType(Hint.ReturnType))
    return true;
  if (MixedResult || PointTransform ||
      swiftBooleanTupleResult(Hint.ReturnType, ReturnMembers))
    return true;
  return !ReturnMembers.empty() &&
         (ReturnMembers.size() <= 2 ||
          (Architecture == Arch::AArch64 && ReturnMembers.size() == 4)) &&
         std::all_of(ReturnMembers.begin(), ReturnMembers.end(),
                     [&](const auto &M) { return Word(M.Type); });
}
} // namespace

bool equalSourceABIs(const SourceFunctionTypeHint &Left,
                     const SourceFunctionTypeHint &Right) {
  if (Left.Origin != Right.Origin || Left.Convention != Right.Convention ||
      Left.Architecture != Right.Architecture ||
      Left.HasExplicitABI != Right.HasExplicitABI ||
      (Left.HasExplicitABI &&
       !sameLocation(Left.ReturnLocation, Right.ReturnLocation)) ||
      !equalSourceTypes(Left.ReturnType, Right.ReturnType) ||
      Left.ReturnComponents.size() != Right.ReturnComponents.size() ||
      Left.Parameters.size() != Right.Parameters.size())
    return false;
  for (size_t I = 0; I < Left.ReturnComponents.size(); ++I)
    if (!sameLocation(Left.ReturnComponents[I], Right.ReturnComponents[I]))
      return false;
  for (size_t I = 0; I < Left.Parameters.size(); ++I) {
    const auto &L = Left.Parameters[I];
    const auto &R = Right.Parameters[I];
    if (L.Name != R.Name || L.TheRole != R.TheRole ||
        L.IndirectByValue != R.IndirectByValue ||
        !equalSourceTypes(L.Type, R.Type) ||
        (Left.HasExplicitABI && !sameLocation(L.Location, R.Location)) ||
        L.Components.size() != R.Components.size())
      return false;
    for (size_t J = 0; J < L.Components.size(); ++J)
      if (!sameLocation(L.Components[J], R.Components[J]))
        return false;
  }
  return true;
}

bool equalSourceTypes(const TypeRef &Left, const TypeRef &Right) {
  unsigned Remaining =
      ::neverd::limits::ir::kSourceABIEqualSourceTypesRemaining;
  return equalTypes(Left, Right, 0, Remaining);
}

std::vector<SourceAggregateMember> sourceAggregateMembers(const TypeRef &Type) {
  if (!Type || Type->Kind != NdTypeKind::Struct ||
      !equalSourceTypes(Type, Type))
    return {};
  std::vector<SourceAggregateMember> Pending{{Type, 0}}, Result;
  while (!Pending.empty()) {
    auto Member = std::move(Pending.back());
    Pending.pop_back();
    if (Member.Type->Kind == NdTypeKind::Struct) {
      for (size_t I = Member.Type->Fields.size(); I-- > 0;)
        Pending.push_back(
            {Member.Type->Fields[I],
             uint16_t(Member.ByteOffset + Member.Type->FieldOffsets[I])});
    } else {
      if (!scalarType(Member.Type))
        return {};
      Result.push_back(std::move(Member));
    }
  }
  if (Result.empty())
    return {};
  // A record of return registers takes no source convention's restrictions:
  // each member is the scalar one register carries, naturally aligned.
  if (Type->RegisterReturnRecord)
    return Result.size() <= 4 &&
                   std::all_of(Result.begin(), Result.end(),
                               [](const auto &Member) {
                                 return Member.Type->Kind == NdTypeKind::Float
                                            ? Member.Type->Size == 4 ||
                                                  Member.Type->Size == 8
                                            : Member.Type->Kind ==
                                                  NdTypeKind::Int;
                               })
               ? Result
               : std::vector<SourceAggregateMember>();
  if (mixedSwiftResult(Type, Result) || swiftBooleanTupleResult(Type, Result))
    return Result;
  const bool Floating = Result.front().Type->Kind == NdTypeKind::Float;
  if (Floating) {
    // CGAffineTransform and CATransform3D contain six and sixteen doubles.
    // Darwin arm64 returns these non-HFAs through x8 and passes a
    // by-value parameter through an independent integer/stack pointer carrier.
    if ((Result.size() > 4 && ((Result.size() != 6 && Result.size() != 16) ||
                               Result.front().Type->Size != 8)) ||
        !std::all_of(Result.begin(), Result.end(),
                     [&](const auto &Member) {
                       return Member.Type->Kind == NdTypeKind::Float &&
                              Member.Type->Size == Result.front().Type->Size &&
                              Member.ByteOffset ==
                                  (&Member - Result.data()) * Member.Type->Size;
                     }) ||
        Result.size() * Result.front().Type->Size != Type->Size)
      return {};
    return Result;
  }
  // Bound indirect results to the three signed words used by Darwin's
  // NSOperatingSystemVersion. Other three-word leaf types need separate
  // compiler-backed physical ABI contracts.
  if (Result.size() == 3 &&
      !std::all_of(Result.begin(), Result.end(), [](const auto &Member) {
        return Member.Type->Kind == NdTypeKind::Int && Member.Type->IsSigned;
      }))
    return {};
  const bool FullWords =
      Result.size() <= 4 &&
      std::all_of(Result.begin(), Result.end(),
                  [&](const auto &Member) {
                    return Member.Type->Kind != NdTypeKind::Float &&
                           Member.Type->Size == 8 &&
                           Member.ByteOffset == (&Member - Result.data()) * 8;
                  }) &&
      Result.size() * 8 == Type->Size;
  // Both Darwin arm64 and x86_64 classify this natural 16-byte layout as two
  // INTEGER eightbytes. Only four bytes of the first carrier are meaningful;
  // the padding is not a source field and must never become a value. Keep the
  // exception exact until other mixed layouts have their own compiler-backed
  // classification and lowering tests.
  const bool NarrowLeadingWord =
      Result.size() == 2 && Type->Size == 16 &&
      Result[0].Type->Kind == NdTypeKind::Int && Result[0].Type->Size == 4 &&
      Result[0].ByteOffset == 0 && Result[1].Type->Kind == NdTypeKind::Int &&
      Result[1].Type->Size == 8 && Result[1].ByteOffset == 8;
  if (!FullWords && !NarrowLeadingWord)
    return {};
  return Result;
}

namespace {
bool indirectTransformRecord(const TypeRef &Type) {
  const auto Members = sourceAggregateMembers(Type);
  return (Members.size() == 6 || Members.size() == 16) &&
         std::all_of(Members.begin(), Members.end(), [](const auto &M) {
           return M.Type->Kind == NdTypeKind::Float && M.Type->Size == 8;
         });
}
} // namespace

bool hasIndirectSourceParameters(const SourceFunctionTypeHint &Hint) {
  return std::any_of(Hint.Parameters.begin(), Hint.Parameters.end(),
                     [](const auto &P) { return P.IndirectByValue; });
}

bool hasSwiftErrorResult(const SourceFunctionTypeHint &Hint) {
  return std::any_of(
      Hint.Parameters.begin(), Hint.Parameters.end(), [](const auto &P) {
        return P.TheRole == SourceParameterTypeHint::Role::SwiftErrorResult;
      });
}

std::vector<SourceABIParameter>
sourceABIParameters(const SourceFunctionTypeHint &Hint) {
  std::string Error;
  if (!validateSourceABI(Hint, Error))
    return {};
  std::vector<SourceABIParameter> Result;
  for (size_t I = 0; I < Hint.Parameters.size(); ++I) {
    const auto &P = Hint.Parameters[I];
    if (P.Components.empty())
      Result.push_back(
          {I, 0, P.Name,
           P.IndirectByValue ? NdType::makePtr(P.Type)
           : P.TheRole == SourceParameterTypeHint::Role::SwiftErrorResult
               ? P.Type->Pointee
               : P.Type,
           P.Location});
    else {
      const auto Members = sourceABIValueMembers(P.Type);
      for (size_t J = 0; J < Members.size(); ++J)
        Result.push_back({I, Members[J].ByteOffset, P.Name, Members[J].Type,
                          P.Components[J]});
    }
  }
  return Result;
}

std::vector<SourceAggregateMember> sourceABIValueMembers(const TypeRef &Type) {
  if (Type && Type->Kind == NdTypeKind::Int && Type->Size == kDarwinWideBytes &&
      Type->SourceName != kSourceAArch64Vector128CType)
    return {{NdType::makeInt(kDarwinWordBytes, false), 0},
            {NdType::makeInt(kDarwinWordBytes, false), kDarwinWordBytes}};
  return sourceAggregateMembers(Type);
}

std::optional<SourceABIParameter>
sourceABIErrorResult(const SourceFunctionTypeHint &Hint) {
  if (!hasSwiftErrorResult(Hint))
    return std::nullopt;
  for (const auto &Parameter : sourceABIParameters(Hint))
    if (Hint.Parameters[Parameter.ParameterIndex].TheRole ==
        SourceParameterTypeHint::Role::SwiftErrorResult)
      return Parameter;
  return std::nullopt;
}

TypeRef sourceABICallResultType(const SourceFunctionTypeHint &Hint) {
  std::string Error;
  if (!validateSourceABI(Hint, Error))
    return nullptr;
  if (Hint.ReturnType->Kind == NdTypeKind::Void ||
      Hint.ReturnType->Kind == NdTypeKind::Struct)
    return Hint.ReturnType;
  return NdType::makeInt(Hint.ReturnType->Size, false);
}

bool validateSourceABI(const SourceFunctionTypeHint &Hint,
                       std::string &Diagnostic) {
  Diagnostic.clear();
  if ((Hint.Convention != SourceFunctionTypeHint::ConventionKind::C &&
       Hint.Convention != SourceFunctionTypeHint::ConventionKind::Swift) ||
      !Hint.HasExplicitABI ||
      (Hint.Architecture != Arch::AArch64 && Hint.Architecture != Arch::X64) ||
      Hint.Parameters.size() >
          ::neverd::limits::ir::
              kSourceABIValidateSourceABIHintParametersSizeLimit ||
      !Hint.ReturnType)
    return fail(Diagnostic,
                "Source ABI requires an explicit arm64/x86_64 layout");
  const auto &TRI = getTargetRegInfo(Hint.Architecture);
  const auto EmptyLocation = [](const SourceABIValueLocation &Location) {
    return Location.Kind == SourceABICarrierKind::None &&
           Location.RegisterOffset == 0 && Location.EntryStackOffset == 0 &&
           Location.ValueBytes == 0 && !Location.ExtendTo32Bits;
  };
  if (Hint.Convention == SourceFunctionTypeHint::ConventionKind::Swift) {
    if (!swiftFixedShape(Hint, Hint.Architecture))
      return fail(Diagnostic, "Unsupported fixed Swift source ABI shape");
    size_t IntegerIndex = 0;
    size_t FloatingIndex = 0;
    int64_t StackOffset = Hint.Architecture == Arch::X64 ? 8 : 0;
    for (size_t I = 0; I < Hint.Parameters.size(); ++I) {
      const auto &P = Hint.Parameters[I];
      SourceABIValueLocation Expected;
      Expected.ValueBytes = P.Type->Size;
      if (P.TheRole == SourceParameterTypeHint::Role::SwiftIndirectResult) {
        Expected.Kind = SourceABICarrierKind::IntegerRegister;
        Expected.RegisterOffset = Hint.Architecture == Arch::AArch64
                                      ? TRI.indirectResultReg()
                                      : TRI.IntReturnReg;
      } else if (P.TheRole == SourceParameterTypeHint::Role::SwiftContext) {
        Expected.Kind = SourceABICarrierKind::IntegerRegister;
        Expected.RegisterOffset =
            Hint.Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13;
      } else if (P.TheRole == SourceParameterTypeHint::Role::SwiftErrorResult) {
        Expected.Kind = SourceABICarrierKind::IntegerRegister;
        Expected.RegisterOffset =
            Hint.Architecture == Arch::AArch64 ? a64reg::X21 : x86reg::R12;
      } else if (P.Type->Kind == NdTypeKind::Float) {
        Expected.Kind = SourceABICarrierKind::FloatingRegister;
        Expected.RegisterOffset = TRI.FPParamRegs[FloatingIndex++];
      } else if (P.Type->Kind == NdTypeKind::Struct) {
        const auto Members = sourceAggregateMembers(P.Type);
        if (!EmptyLocation(P.Location) || P.Components.size() != Members.size())
          return fail(Diagnostic, "Unsupported fixed Swift record carrier");
        for (size_t J = 0; J < Members.size(); ++J) {
          const SourceABIValueLocation Member = {
              SourceABICarrierKind::FloatingRegister,
              TRI.FPParamRegs[FloatingIndex++], 0, Members[J].Type->Size};
          if (!sameLocation(P.Components[J], Member))
            return fail(Diagnostic, "Unsupported fixed Swift record carrier");
        }
        continue;
      } else if (IntegerIndex < TRI.IntParamRegs.size()) {
        Expected.Kind = SourceABICarrierKind::IntegerRegister;
        Expected.RegisterOffset = TRI.IntParamRegs[IntegerIndex++];
        Expected.ExtendTo32Bits =
            P.Type->Kind == NdTypeKind::Int && P.Type->Size < 4;
      } else {
        const int64_t Alignment =
            Hint.Architecture == Arch::AArch64 ? P.Type->Size : 8;
        StackOffset = (StackOffset + Alignment - 1) & -Alignment;
        Expected.Kind = SourceABICarrierKind::Stack;
        Expected.EntryStackOffset = StackOffset;
        StackOffset += Hint.Architecture == Arch::AArch64 ? P.Type->Size : 8;
      }
      if (!P.Components.empty() || !sameLocation(P.Location, Expected))
        return fail(Diagnostic, "Unsupported fixed Swift argument carrier");
    }
  }
  auto IntegerRegister = [&](uint64_t Offset) {
    if (TRI.isFrameOrLinkReg(Offset))
      return false;
    // AArch64's target table describes its contiguous scalar bank through
    // subregister records; GeneralRegs is currently populated only on x86.
    if (Hint.Architecture == Arch::AArch64)
      return Offset >= a64reg::X0 && Offset <= a64reg::X28 &&
             (Offset - a64reg::X0) % 8 == 0;
    return TRI.isGeneralReg(Offset);
  };
  auto ValidLocation = [&](const TypeRef &Type,
                           const SourceABIValueLocation &Location,
                           bool IsReturn) {
    const bool Vector128 =
        Hint.Origin == SourceFunctionTypeHint::OriginKind::NativeAnalysis &&
        Hint.Architecture == Arch::AArch64 && !IsReturn && Type &&
        Type->Kind == NdTypeKind::Int && Type->Size == 16 &&
        Type->SourceName == kSourceAArch64Vector128CType &&
        Location.Kind == SourceABICarrierKind::FloatingRegister;
    if ((!scalarType(Type) && !Vector128) ||
        Location.ValueBytes != Type->Size ||
        Type->SourceName == kSourceBooleanCType)
      return false;
    if (Location.ExtendTo32Bits &&
        ((IsReturn &&
          (Hint.Architecture != Arch::AArch64 ||
           Hint.Convention != SourceFunctionTypeHint::ConventionKind::C)) ||
         Location.Kind != SourceABICarrierKind::IntegerRegister ||
         Type->Kind != NdTypeKind::Int || Type->Size >= 4))
      return false;
    if (Location.Kind == SourceABICarrierKind::Stack)
      return !IsReturn && Location.RegisterOffset == 0 &&
             Location.EntryStackOffset >=
                 (Hint.Architecture == Arch::X64 ? 8 : 0) &&
             Location.EntryStackOffset <=
                 ::neverd::limits::ir::kMaxSourceStackArgumentBytes -
                     Type->Size &&
             Location.EntryStackOffset % Type->Size == 0;
    if (Location.EntryStackOffset != 0)
      return false;
    if (Location.Kind == SourceABICarrierKind::FloatingRegister)
      return (Type->Kind == NdTypeKind::Float || Vector128) &&
             TRI.isVectorReg(Location.RegisterOffset) &&
             (!IsReturn || Location.RegisterOffset == TRI.FPReturnReg);
    if (Location.Kind == SourceABICarrierKind::IntegerRegister)
      // Windows ARM64 variadic arguments carry floating bits in X registers.
      // Explicit source declarations retain that logical floating type; the
      // source boundary reinterprets the complete bits rather than converting
      // their integer value. Inferred signatures and result carriers keep
      // their existing restrictions.
      return (Type->Kind != NdTypeKind::Float ||
              (!IsReturn && Hint.Architecture == Arch::AArch64 &&
               Hint.Origin ==
                   SourceFunctionTypeHint::OriginKind::ExplicitSource &&
               Hint.Convention == SourceFunctionTypeHint::ConventionKind::C)) &&
             IntegerRegister(Location.RegisterOffset) &&
             (!IsReturn || Location.RegisterOffset == TRI.IntReturnReg);
    return false;
  };
  auto AggregateLocations = [&](const TypeRef &Type,
                                const std::vector<SourceABIValueLocation>
                                    &Parts,
                                bool IsReturn) {
    const auto Members = sourceAggregateMembers(Type);
    if (Members.empty() || Parts.size() != Members.size())
      return false;
    if (swiftBooleanTupleResult(Type, Members)) {
      if (!IsReturn ||
          Hint.Convention != SourceFunctionTypeHint::ConventionKind::Swift)
        return false;
      const auto Expected = swiftBooleanTupleResultLocations(Hint.Architecture);
      return std::equal(Parts.begin(), Parts.end(), Expected.begin(),
                        sameLocation);
    }
    if (mixedSwiftResult(Type, Members)) {
      if (!IsReturn ||
          Hint.Convention != SourceFunctionTypeHint::ConventionKind::Swift)
        return false;
      const auto Expected = mixedSwiftResultLocations(Hint.Architecture);
      return std::equal(Parts.begin(), Parts.end(), Expected.begin(),
                        sameLocation);
    }
    const bool Floating = Members.front().Type->Kind == NdTypeKind::Float;
    if (Floating && Members.size() > 4)
      return false;
    const bool SwiftFourWordReturn =
        IsReturn && !Floating &&
        Hint.Convention == SourceFunctionTypeHint::ConventionKind::Swift &&
        Hint.Architecture == Arch::AArch64 && Members.size() == 4;
    if (!Floating && Members.size() > 2 && !SwiftFourWordReturn)
      return false;
    if (Floating && Hint.Architecture != Arch::AArch64 &&
        !(Hint.Convention == SourceFunctionTypeHint::ConventionKind::Swift &&
          swiftPointTransform(Hint)))
      return false;
    const auto Bank = Floating              ? TRI.FPParamRegs
                      : SwiftFourWordReturn ? TRI.IntParamRegs
                      : IsReturn            ? TRI.IntReturnRegs
                                            : TRI.IntParamRegs;
    const auto Start =
        std::find(Bank.begin(), Bank.end(), Parts.front().RegisterOffset);
    const bool Stack = Parts.front().Kind == SourceABICarrierKind::Stack;
    if (Stack) {
      if (IsReturn || Parts.front().EntryStackOffset % Type->Alignment)
        return false;
    } else if (Start == Bank.end() ||
               size_t(Bank.end() - Start) < Parts.size() ||
               (IsReturn && Start != Bank.begin())) {
      return false;
    }
    for (size_t I = 0; I < Parts.size(); ++I) {
      const auto &Part = Parts[I];
      if (!ValidLocation(Members[I].Type, Part, false) || Part.ExtendTo32Bits ||
          (Stack ? Part.Kind != SourceABICarrierKind::Stack ||
                       Part.EntryStackOffset != Parts.front().EntryStackOffset +
                                                    Members[I].ByteOffset
                 : Part.Kind != (Floating
                                     ? SourceABICarrierKind::FloatingRegister
                                     : SourceABICarrierKind::IntegerRegister) ||
                       Part.RegisterOffset != Start[I]))
        return false;
    }
    return true;
  };
  if (Hint.ReturnType->Kind == NdTypeKind::Struct) {
    const auto &Return = Hint.ReturnLocation;
    if (Return.Kind == SourceABICarrierKind::IndirectResultPointer) {
      const auto Members = sourceAggregateMembers(Hint.ReturnType);
      const bool ThreeSignedWords =
          Members.size() == 3 && Hint.ReturnType->Size == 24 &&
          std::all_of(Members.begin(), Members.end(), [](const auto &M) {
            return M.Type->Kind == NdTypeKind::Int && M.Type->Size == 8 &&
                   M.Type->IsSigned;
          });
      const bool IndirectDoubles =
          (Members.size() == 6 || Members.size() == 16) &&
          Hint.ReturnType->Size == Members.size() * 8 &&
          Members.front().Type->Kind == NdTypeKind::Float &&
          Members.front().Type->Size == 8;
      if (Hint.Architecture != Arch::AArch64 ||
          Hint.Convention != SourceFunctionTypeHint::ConventionKind::C ||
          (!ThreeSignedWords && !IndirectDoubles) ||
          Return.RegisterOffset != TRI.indirectResultReg() ||
          Return.EntryStackOffset != 0 || Return.ValueBytes != 8 ||
          Return.ExtendTo32Bits || !Hint.ReturnComponents.empty())
        return fail(Diagnostic, "Unsupported source indirect record result");
    } else if (!EmptyLocation(Return) ||
               !AggregateLocations(Hint.ReturnType, Hint.ReturnComponents,
                                   true))
      return fail(Diagnostic, "Unsupported source record return carriers");
  } else if (!Hint.ReturnComponents.empty()) {
    if (Hint.ReturnType->Kind != NdTypeKind::Int ||
        Hint.ReturnType->Size != 16 || Hint.ReturnComponents.size() != 2 ||
        !EmptyLocation(Hint.ReturnLocation) || TRI.IntReturnRegs.size() < 2)
      return fail(Diagnostic, "Unsupported source return components");
    for (size_t I = 0; I != 2; ++I) {
      const auto &Component = Hint.ReturnComponents[I];
      if (Component.Kind != SourceABICarrierKind::IntegerRegister ||
          Component.RegisterOffset != TRI.IntReturnRegs[I] ||
          Component.EntryStackOffset != 0 || Component.ValueBytes != 8 ||
          Component.ExtendTo32Bits)
        return fail(Diagnostic, "Invalid source integer-pair return carrier");
    }
  } else if (Hint.ReturnType->Kind == NdTypeKind::Void) {
    if (!EmptyLocation(Hint.ReturnLocation))
      return fail(Diagnostic,
                  "Void source result has a physical value carrier");
  } else if (!ValidLocation(Hint.ReturnType, Hint.ReturnLocation, true)) {
    return fail(Diagnostic, "Unsupported source return carrier");
  }
  std::set<std::pair<SourceABICarrierKind, uint64_t>> Registers;
  if (Hint.ReturnLocation.Kind == SourceABICarrierKind::IndirectResultPointer)
    Registers.emplace(SourceABICarrierKind::IntegerRegister,
                      Hint.ReturnLocation.RegisterOffset);
  std::vector<std::pair<int64_t, int64_t>> StackRanges;
  size_t PhysicalCount = 0;
  std::vector<uint64_t> FloatingRegisters;
  bool HasNativeVector = false;
  for (const auto &Parameter : Hint.Parameters) {
    if (Parameter.TheRole != SourceParameterTypeHint::Role::Ordinary &&
        Hint.Convention != SourceFunctionTypeHint::ConventionKind::Swift)
      return fail(Diagnostic,
                  "Swift parameter role requires the Swift convention");
    if (Parameter.IndirectByValue) {
      const auto &L = Parameter.Location;
      if (Hint.Architecture != Arch::AArch64 ||
          Hint.Convention != SourceFunctionTypeHint::ConventionKind::C ||
          Parameter.TheRole != SourceParameterTypeHint::Role::Ordinary ||
          !Parameter.Components.empty() ||
          !indirectTransformRecord(Parameter.Type) ||
          !ValidLocation(NdType::makePtr(Parameter.Type), L, false) ||
          (L.Kind == SourceABICarrierKind::IntegerRegister &&
           std::find(TRI.IntParamRegs.begin(), TRI.IntParamRegs.end(),
                     L.RegisterOffset) == TRI.IntParamRegs.end()))
        return fail(Diagnostic, "Unsupported source indirect record parameter");
    } else if (!Parameter.Components.empty()) {
      if (Parameter.Type && Parameter.Type->Kind == NdTypeKind::Int &&
          Parameter.Type->Size == kDarwinWideBytes) {
        const auto &Parts = Parameter.Components;
        const auto Members = sourceABIValueMembers(Parameter.Type);
        if (Hint.Architecture != Arch::AArch64 ||
            Hint.Convention != SourceFunctionTypeHint::ConventionKind::C ||
            !EmptyLocation(Parameter.Location) ||
            Parts.size() != kDarwinPairWords || Members.empty())
          return fail(Diagnostic, kInvalidWideParameter);
        const bool Stack = Parts.front().Kind == SourceABICarrierKind::Stack;
        const auto First =
            std::find(TRI.IntParamRegs.begin(), TRI.IntParamRegs.end(),
                      Parts.front().RegisterOffset);
        if (Stack ? Parts.front().EntryStackOffset % kDarwinWideBytes != 0
                  : First == TRI.IntParamRegs.end() ||
                        size_t(TRI.IntParamRegs.end() - First) < Parts.size())
          return fail(Diagnostic, kInvalidWideParameter);
        for (size_t I = 0; I < Parts.size(); ++I)
          if (!ValidLocation(Members[I].Type, Parts[I], false) ||
              Parts[I].ExtendTo32Bits ||
              (Stack ? Parts[I].Kind != SourceABICarrierKind::Stack ||
                           Parts[I].EntryStackOffset !=
                               Parts.front().EntryStackOffset +
                                   Members[I].ByteOffset
                     : Parts[I].Kind != SourceABICarrierKind::IntegerRegister ||
                           Parts[I].RegisterOffset != First[I]))
            return fail(Diagnostic, kInvalidWideParameter);
      } else if (!EmptyLocation(Parameter.Location) ||
                 !AggregateLocations(Parameter.Type, Parameter.Components,
                                     false))
        return fail(Diagnostic, "Unsupported source record parameter carriers");
    } else if (!ValidLocation(Parameter.Type, Parameter.Location, false)) {
      return fail(Diagnostic, "Unsupported source parameter carrier");
    }
    const auto Locations = Parameter.Components.empty()
                               ? std::vector{Parameter.Location}
                               : Parameter.Components;
    HasNativeVector |=
        Hint.Origin == SourceFunctionTypeHint::OriginKind::NativeAnalysis &&
        Hint.Architecture == Arch::AArch64 && Parameter.Type &&
        Parameter.Type->Kind == NdTypeKind::Int && Parameter.Type->Size == 16 &&
        Parameter.Type->SourceName == kSourceAArch64Vector128CType;
    PhysicalCount += Locations.size();
    if (PhysicalCount > static_cast<size_t>(limits::kMaxBoundSourceCallArgs))
      return fail(Diagnostic, "Source parameter carrier budget exceeded");
    for (const auto &Location : Locations) {
      if (Location.Kind == SourceABICarrierKind::FloatingRegister)
        FloatingRegisters.push_back(Location.RegisterOffset);
      if (Location.Kind == SourceABICarrierKind::Stack) {
        const int64_t Begin = Location.EntryStackOffset;
        const int64_t End = Begin + Location.ValueBytes;
        for (const auto &[OtherBegin, OtherEnd] : StackRanges)
          if (Begin < OtherEnd && OtherBegin < End)
            return fail(Diagnostic,
                        "Overlapping source stack parameter locations");
        StackRanges.emplace_back(Begin, End);
      } else if (!Registers.emplace(Location.Kind, Location.RegisterOffset)
                      .second) {
        return fail(Diagnostic,
                    "Source parameters share one physical register");
      }
    }
  }
  if (HasNativeVector) {
    if (FloatingRegisters.size() > TRI.FPParamRegs.size())
      return fail(Diagnostic,
                  "Native vector parameters exceed the floating register bank");
    for (size_t Index = 0; Index < FloatingRegisters.size(); ++Index)
      if (FloatingRegisters[Index] != TRI.FPParamRegs[Index])
        return fail(Diagnostic,
                    "Native vector parameters require a contiguous floating "
                    "register prefix");
  }
  return true;
}

namespace {
bool assignDarwinSourceABI(SourceFunctionTypeHint &Hint, Arch Architecture,
                           std::string &Diagnostic, bool Records,
                           SourceFunctionTypeHint::ConventionKind Convention) {
  Diagnostic.clear();
  if ((Architecture != Arch::AArch64 && Architecture != Arch::X64) ||
      !Hint.ReturnType ||
      Hint.Parameters.size() >
          ::neverd::limits::ir::
              kSourceABIAssignDarwinSourceABIHintParametersSizeLimit)
    return fail(Diagnostic, "Unsupported Darwin fixed source ABI");
  const auto &TRI = getTargetRegInfo(Architecture);
  const bool PointTransform =
      Convention == SourceFunctionTypeHint::ConventionKind::Swift &&
      swiftPointTransform(Hint);
  size_t IntegerIndex = 0;
  size_t FloatIndex = 0;
  int64_t StackOffset = Architecture == Arch::X64 ? 8 : 0;
  const auto AssignScalarLocation = [&](const TypeRef &Type,
                                        SourceABIValueLocation &Location) {
    Location = {};
    Location.ValueBytes = Type->Size;
    const bool Floating = Type->Kind == NdTypeKind::Float;
    auto &Index = Floating ? FloatIndex : IntegerIndex;
    const auto Bank = Floating ? TRI.FPParamRegs : TRI.IntParamRegs;
    if (Index < Bank.size()) {
      Location.Kind = Floating ? SourceABICarrierKind::FloatingRegister
                               : SourceABICarrierKind::IntegerRegister;
      Location.RegisterOffset = Bank[Index++];
      // Both Darwin ABIs require callers to extend narrow integer register
      // arguments to 32 bits. Stack arguments retain their own storage width.
      Location.ExtendTo32Bits = Type->Kind == NdTypeKind::Int && Type->Size < 4;
    } else {
      // Apple arm64 packs fixed stack scalars at natural alignment. x86_64
      // Darwin uses eight-byte argument slots, above the pushed return address.
      const int64_t Alignment = Architecture == Arch::AArch64 ? Type->Size : 8;
      StackOffset = (StackOffset + Alignment - 1) & -Alignment;
      Location.Kind = SourceABICarrierKind::Stack;
      Location.EntryStackOffset = StackOffset;
      StackOffset += Architecture == Arch::AArch64 ? Type->Size : 8;
    }
  };
  std::optional<SourceParameterPlacement> Placement;
  if (Architecture == Arch::AArch64 &&
      Convention == SourceFunctionTypeHint::ConventionKind::C) {
    std::vector<TypeRef> Types;
    Types.reserve(Hint.Parameters.size());
    for (const auto &Parameter : Hint.Parameters) {
      auto Type = Parameter.Type;
      if (Parameter.TheRole != SourceParameterTypeHint::Role::Ordinary ||
          (!scalarType(Type) &&
           !(Type && Type->Kind == NdTypeKind::Int &&
             Type->Size == kDarwinWideBytes &&
             Type->SourceName != kSourceAArch64Vector128CType) &&
           !(Records && !sourceAggregateMembers(Type).empty())))
        return fail(Diagnostic, kUnsupportedDarwinValue);
      if (Type->Kind == NdTypeKind::Struct) {
        Type = std::make_shared<NdType>(*Type);
        Type->Passing = NdRecordPassing::ByValue;
      }
      Types.push_back(std::move(Type));
    }
    Placement = placeSourceParameters(Architecture, BinaryFormat::MachO,
                                      Hint.ReturnType, Types);
    if (!Placement || Placement->Parameters.size() != Types.size())
      return fail(Diagnostic, kIncompleteDarwinPlacement);
  }
  for (size_t ParameterIndex = 0; ParameterIndex < Hint.Parameters.size();
       ++ParameterIndex) {
    auto &Parameter = Hint.Parameters[ParameterIndex];
    Parameter.Components.clear();
    Parameter.IndirectByValue = false;
    if (Placement) {
      const auto &Pieces = Placement->Parameters[ParameterIndex];
      const auto Members = sourceABIValueMembers(Parameter.Type);
      Parameter.Location = {};
      if (Pieces.front().Indirect) {
        Parameter.IndirectByValue = true;
        Parameter.Location = Pieces.front().Location;
      } else if (Members.empty()) {
        Parameter.Location = Pieces.front().Location;
        Parameter.Location.ExtendTo32Bits =
            Parameter.Location.Kind == SourceABICarrierKind::IntegerRegister &&
            Parameter.Type->Kind == NdTypeKind::Int &&
            Parameter.Type->Size < kDarwinPromotedIntegerBytes;
      } else {
        for (const auto &Member : Members) {
          const auto Piece = std::find_if(
              Pieces.begin(), Pieces.end(), [&](const auto &Piece) {
                return Piece.Offset <= Member.ByteOffset &&
                       Member.ByteOffset + Member.Type->Size <=
                           Piece.Offset + Piece.Location.ValueBytes;
              });
          if (Piece == Pieces.end())
            return fail(Diagnostic, kIncompleteDarwinPlacement);
          auto Location = Piece->Location;
          if (Location.Kind == SourceABICarrierKind::Stack)
            Location.EntryStackOffset += Member.ByteOffset - Piece->Offset;
          Location.ValueBytes = Member.Type->Size;
          Parameter.Components.push_back(Location);
        }
      }
      continue;
    }
    if (Parameter.TheRole != SourceParameterTypeHint::Role::Ordinary) {
      if (Convention != SourceFunctionTypeHint::ConventionKind::Swift ||
          !Parameter.Type || Parameter.Type->Kind != NdTypeKind::Ptr)
        return fail(Diagnostic, "Unsupported Darwin special parameter ABI");
      Parameter.Location = {};
      Parameter.Location.Kind = SourceABICarrierKind::IntegerRegister;
      Parameter.Location.ValueBytes = Parameter.Type->Size;
      switch (Parameter.TheRole) {
      case SourceParameterTypeHint::Role::SwiftIndirectResult:
        Parameter.Location.RegisterOffset = Architecture == Arch::AArch64
                                                ? TRI.indirectResultReg()
                                                : TRI.IntReturnReg;
        break;
      case SourceParameterTypeHint::Role::SwiftContext:
        Parameter.Location.RegisterOffset =
            Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13;
        break;
      case SourceParameterTypeHint::Role::SwiftErrorResult:
        Parameter.Location.RegisterOffset =
            Architecture == Arch::AArch64 ? a64reg::X21 : x86reg::R12;
        break;
      default:
        return fail(Diagnostic, "Unsupported Darwin special parameter ABI");
      }
      continue;
    }
    if (Records && Parameter.Type &&
        Parameter.Type->Kind == NdTypeKind::Struct) {
      const auto Members = sourceAggregateMembers(Parameter.Type);
      if (Members.empty() || mixedSwiftResult(Parameter.Type, Members))
        return fail(Diagnostic, "Unsupported Darwin record parameter ABI");
      const bool Floating = Members.front().Type->Kind == NdTypeKind::Float;
      if (Floating && Members.size() > 4) {
        if (Architecture != Arch::AArch64 ||
            Convention != SourceFunctionTypeHint::ConventionKind::C ||
            !indirectTransformRecord(Parameter.Type))
          return fail(Diagnostic,
                      "Unsupported Darwin indirect record parameter");
        Parameter.IndirectByValue = true;
        AssignScalarLocation(NdType::makePtr(Parameter.Type),
                             Parameter.Location);
        continue;
      }
      if ((Floating && Architecture != Arch::AArch64 && !PointTransform) ||
          (!Floating && Members.size() > 2))
        return fail(Diagnostic, "Unsupported Darwin record parameter ABI");
      Parameter.Location = {};
      auto &Index = Floating ? FloatIndex : IntegerIndex;
      const auto Bank = Floating ? TRI.FPParamRegs : TRI.IntParamRegs;
      const bool Stack = Index + Members.size() > Bank.size();
      if (Stack) {
        // AAPCS64 exhausts the bank when the whole record cannot fit. SysV
        // rolls back this argument's allocation, leaving registers for later
        // scalars or smaller records.
        if (Architecture == Arch::AArch64)
          Index = Bank.size();
        const int64_t Alignment = Parameter.Type->Alignment;
        StackOffset = (StackOffset + Alignment - 1) & -Alignment;
      }
      for (const auto &Member : Members) {
        Parameter.Components.push_back(
            Stack ? SourceABIValueLocation{SourceABICarrierKind::Stack, 0,
                                           StackOffset + Member.ByteOffset,
                                           Member.Type->Size}
                  : SourceABIValueLocation{
                        Floating ? SourceABICarrierKind::FloatingRegister
                                 : SourceABICarrierKind::IntegerRegister,
                        Bank[Index++], 0, Member.Type->Size});
      }
      if (Stack)
        StackOffset += Parameter.Type->Size;
      continue;
    }
    if (!scalarType(Parameter.Type))
      return fail(Diagnostic,
                  "Darwin source ABI currently supports scalar values");
    AssignScalarLocation(Parameter.Type, Parameter.Location);
  }
  Hint.ReturnLocation = {};
  Hint.ReturnComponents.clear();
  if (Records && Hint.ReturnType->Kind == NdTypeKind::Struct) {
    const auto Members = sourceAggregateMembers(Hint.ReturnType);
    if (Members.empty())
      return fail(Diagnostic, "Unsupported Darwin record return ABI");
    if (swiftBooleanTupleResult(Hint.ReturnType, Members)) {
      if (Convention != SourceFunctionTypeHint::ConventionKind::Swift)
        return fail(Diagnostic, "Boolean tuple result requires the Swift ABI");
      Hint.ReturnComponents = swiftBooleanTupleResultLocations(Architecture);
    } else if (mixedSwiftResult(Hint.ReturnType, Members)) {
      if (Convention != SourceFunctionTypeHint::ConventionKind::Swift)
        return fail(Diagnostic, "Mixed record result requires the Swift ABI");
      Hint.ReturnComponents = mixedSwiftResultLocations(Architecture);
    } else {
      const bool Floating = Members.front().Type->Kind == NdTypeKind::Float;
      const bool IndirectDoubles =
          Floating && (Members.size() == 6 || Members.size() == 16) &&
          Hint.ReturnType->Size == Members.size() * 8 &&
          Members.front().Type->Size == 8 && Architecture == Arch::AArch64 &&
          Convention == SourceFunctionTypeHint::ConventionKind::C;
      const bool SwiftFourWordReturn =
          !Floating &&
          Convention == SourceFunctionTypeHint::ConventionKind::Swift &&
          Architecture == Arch::AArch64 && Members.size() == 4;
      const auto Bank = Floating              ? TRI.FPParamRegs
                        : SwiftFourWordReturn ? TRI.IntParamRegs
                                              : TRI.IntReturnRegs;
      const bool Indirect =
          IndirectDoubles ||
          (!Floating && Members.size() == 3 && Architecture == Arch::AArch64 &&
           Convention == SourceFunctionTypeHint::ConventionKind::C);
      if (Indirect)
        Hint.ReturnLocation = {SourceABICarrierKind::IndirectResultPointer,
                               TRI.indirectResultReg(), 0, 8};
      else if ((Floating && Architecture != Arch::AArch64 && !PointTransform) ||
               Members.size() > Bank.size())
        return fail(Diagnostic, "Unsupported Darwin record return ABI");
      for (size_t I = 0; !Indirect && I < Members.size(); ++I)
        Hint.ReturnComponents.push_back(
            {Floating ? SourceABICarrierKind::FloatingRegister
                      : SourceABICarrierKind::IntegerRegister,
             Bank[I], 0, Members[I].Type->Size});
    }
  } else if (Hint.ReturnType->Kind == NdTypeKind::Int &&
             Hint.ReturnType->Size == 16 && TRI.IntReturnRegs.size() >= 2) {
    for (size_t I = 0; I != 2; ++I)
      Hint.ReturnComponents.push_back(
          {SourceABICarrierKind::IntegerRegister, TRI.IntReturnRegs[I], 0, 8});
  } else if (Hint.ReturnType->Kind != NdTypeKind::Void) {
    if (!scalarType(Hint.ReturnType))
      return fail(Diagnostic, "Unsupported Darwin scalar return value");
    const bool Floating = Hint.ReturnType->Kind == NdTypeKind::Float;
    Hint.ReturnLocation.Kind = Floating ? SourceABICarrierKind::FloatingRegister
                                        : SourceABICarrierKind::IntegerRegister;
    Hint.ReturnLocation.RegisterOffset =
        Floating ? TRI.FPReturnReg : TRI.IntReturnReg;
    Hint.ReturnLocation.ValueBytes = Hint.ReturnType->Size;
    // Clang's DarwinPCS classifies these results with ABIArgInfo::getExtend;
    // AArch64's return convention promotes them to W0. Keep the source value
    // width separate so a byte result is still emitted with its byte type.
    Hint.ReturnLocation.ExtendTo32Bits =
        Architecture == Arch::AArch64 &&
        Convention == SourceFunctionTypeHint::ConventionKind::C &&
        Hint.ReturnType->Kind == NdTypeKind::Int && Hint.ReturnType->Size < 4;
  }
  Hint.Architecture = Architecture;
  Hint.Convention = Convention;
  Hint.HasExplicitABI = true;
  return validateSourceABI(Hint, Diagnostic);
}

} // namespace

bool assignDarwinScalarSourceABI(SourceFunctionTypeHint &Hint,
                                 Arch Architecture, std::string &Diagnostic) {
  return assignDarwinSourceABI(Hint, Architecture, Diagnostic, false,
                               SourceFunctionTypeHint::ConventionKind::C);
}

bool assignDarwinFixedSourceABI(SourceFunctionTypeHint &Hint, Arch Architecture,
                                std::string &Diagnostic) {
  return assignDarwinSourceABI(Hint, Architecture, Diagnostic, true,
                               SourceFunctionTypeHint::ConventionKind::C);
}

bool assignDarwinSwiftSourceABI(SourceFunctionTypeHint &Hint, Arch Architecture,
                                std::string &Diagnostic) {
  if (!swiftFixedShape(Hint, Architecture))
    return fail(Diagnostic, "Unsupported fixed Swift source ABI shape");
  return assignDarwinSourceABI(Hint, Architecture, Diagnostic, true,
                               SourceFunctionTypeHint::ConventionKind::Swift);
}

std::optional<SourceCallTypeHint> swiftValueWitnessSourceCallHint(
    Arch Architecture, SourceCallTypeHint::SwiftValueWitnessKind Operation) {
  SourceCallTypeHint Result;
  Result.CallKind = SourceCallTypeHint::Kind::SwiftValueWitness;
  Result.ValueWitness = Operation;
  auto &Signature = Result.Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftRuntime;
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  switch (Operation) {
  case SourceCallTypeHint::SwiftValueWitnessKind::Destroy:
    Result.TargetName = "destroy";
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"value", Pointer}, {"metadata", Pointer}};
    break;
  case SourceCallTypeHint::SwiftValueWitnessKind::InitializeWithCopy:
    Result.TargetName = "initializeWithCopy";
    break;
  case SourceCallTypeHint::SwiftValueWitnessKind::
      InitializeBufferWithCopyOfBuffer:
    Result.TargetName = "initializeBufferWithCopyOfBuffer";
    break;
  case SourceCallTypeHint::SwiftValueWitnessKind::AssignWithCopy:
    Result.TargetName = "assignWithCopy";
    break;
  case SourceCallTypeHint::SwiftValueWitnessKind::InitializeWithTake:
    Result.TargetName = "initializeWithTake";
    break;
  case SourceCallTypeHint::SwiftValueWitnessKind::AssignWithTake:
    Result.TargetName = "assignWithTake";
    break;
  case SourceCallTypeHint::SwiftValueWitnessKind::GetEnumTagSinglePayload:
    Result.TargetName = "getEnumTagSinglePayload";
    Signature.ReturnType = NdType::makeInt(4, false);
    Signature.Parameters = {{"value", Pointer},
                            {"emptyCases", NdType::makeInt(4, false)},
                            {"metadata", Pointer}};
    break;
  case SourceCallTypeHint::SwiftValueWitnessKind::StoreEnumTagSinglePayload:
    Result.TargetName = "storeEnumTagSinglePayload";
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"value", Pointer},
                            {"whichCase", NdType::makeInt(4, false)},
                            {"emptyCases", NdType::makeInt(4, false)},
                            {"metadata", Pointer}};
    break;
  default:
    return std::nullopt;
  }
  if (!Signature.ReturnType) {
    Signature.ReturnType = Pointer;
    Signature.Parameters = {
        {"destination", Pointer}, {"source", Pointer}, {"metadata", Pointer}};
  }
  std::string Diagnostic;
  if (!assignDarwinSwiftSourceABI(Signature, Architecture, Diagnostic))
    return std::nullopt;
  return Result;
}

std::optional<unsigned>
swiftValueWitnessSlot(SourceCallTypeHint::SwiftValueWitnessKind Operation) {
  // Required function entries precede the layout words in Swift ABI
  // include/swift/ABI/ValueWitness.def. Optional enum witnesses are excluded.
  switch (Operation) {
  case SourceCallTypeHint::SwiftValueWitnessKind::
      InitializeBufferWithCopyOfBuffer:
    return 0;
  case SourceCallTypeHint::SwiftValueWitnessKind::Destroy:
    return 1;
  case SourceCallTypeHint::SwiftValueWitnessKind::InitializeWithCopy:
    return 2;
  case SourceCallTypeHint::SwiftValueWitnessKind::AssignWithCopy:
    return 3;
  case SourceCallTypeHint::SwiftValueWitnessKind::InitializeWithTake:
    return 4;
  case SourceCallTypeHint::SwiftValueWitnessKind::AssignWithTake:
    return 5;
  case SourceCallTypeHint::SwiftValueWitnessKind::GetEnumTagSinglePayload:
    return 6;
  case SourceCallTypeHint::SwiftValueWitnessKind::StoreEnumTagSinglePayload:
    return 7;
  }
  return std::nullopt;
}

bool isSwiftValueWitnessSourceCallHint(const SourceCallTypeHint &Hint,
                                       Arch Architecture) {
  if (!Hint.ValueWitness || Hint.BooleanResult || Hint.WeakImport ||
      Hint.ImmutableNativeCall || Hint.Virtual || Hint.FunctionParameterCall ||
      Hint.AddressedFunctionABI || Hint.SwiftWitnessUndefDescriptor ||
      !Hint.CanonicalBooleanInputs.empty())
    return false;
  if (Hint.SwiftWitnessFrame) {
    const auto &Proof = *Hint.SwiftWitnessFrame;
    if (Architecture != Arch::AArch64 || !Proof.FunctionEntry ||
        Proof.FunctionEntry % 4 || !Proof.Site.Instruction ||
        Proof.Site.Instruction % 4 || Proof.Site.Sequence < 0 ||
        Proof.Site.Opcode != NdOp::INDIR_CALL || Proof.Site.StaticTarget)
      return false;
  }
  const auto Expected =
      swiftValueWitnessSourceCallHint(Architecture, *Hint.ValueWitness);
  return Expected &&
         Hint.CallKind == SourceCallTypeHint::Kind::SwiftValueWitness &&
         Hint.TargetAddress == 0 && Hint.TargetName == Expected->TargetName &&
         Hint.Selector.empty() && Hint.OwnerClass.empty() &&
         Hint.SelectorReferenceAddress == 0 && !Hint.DoesNotReturn &&
         !Hint.ReturnedArgument && !Hint.RuntimeObjCResultType &&
         Hint.BorrowedByteInputs.empty() &&
         Hint.SwiftStaticStringInputs.empty() &&
         Hint.SwiftStringInputs.empty() && !Hint.Format &&
         !Hint.NilTerminated && !Hint.Receiver && !Hint.SelectorResultUse &&
         !Hint.SelectorResultTypeUse && !Hint.SelectorArgumentTypeUse &&
         !Hint.SelectorForwardingUse && !Hint.SelectorArgumentStorageUse &&
         !Hint.ObjCIndirectResultStorage && Hint.ByteCount == 0 &&
         Hint.ImmutablePointerSlot == 0 &&
         equalSourceABIs(Hint.Signature, Expected->Signature);
}

bool assignDarwinObjCSourceABI(SourceFunctionTypeHint &Hint, Arch Architecture,
                               std::string &Diagnostic) {
  if ((Hint.Origin != SourceFunctionTypeHint::OriginKind::ObjCRuntime &&
       Hint.Origin != SourceFunctionTypeHint::OriginKind::ObjCSDK) ||
      Hint.Parameters.size() < 2)
    return fail(Diagnostic, "Unsupported Objective-C source ABI");
  if (!assignDarwinFixedSourceABI(Hint, Architecture, Diagnostic))
    return false;
  // Unlike an ordinary C callee, objc_msgSend with a nil receiver leaves an
  // indirect result buffer untouched. The logical-record call projection
  // cannot supply that storage behavior until it models nil dispatch.
  if (Hint.ReturnLocation.Kind == SourceABICarrierKind::IndirectResultPointer) {
    Hint.HasExplicitABI = false;
    return fail(Diagnostic,
                "Objective-C indirect results require nil storage modeling");
  }
  return true;
}

bool assignDarwinVariadicSourceABI(SourceFunctionTypeHint &Hint,
                                   unsigned FixedCount, Arch Architecture,
                                   std::string &Diagnostic) {
  if (!FixedCount || FixedCount > Hint.Parameters.size())
    return fail(Diagnostic, "Invalid variadic source prefix");
  for (size_t I = FixedCount; I < Hint.Parameters.size(); ++I) {
    const auto &T = Hint.Parameters[I].Type;
    if (!scalarType(T) || (T->Kind == NdTypeKind::Int && T->Size < 4) ||
        (T->Kind == NdTypeKind::Float && T->Size != 8))
      return fail(Diagnostic, "Variadic source arguments must be promoted");
  }
  if (!assignDarwinScalarSourceABI(Hint, Architecture, Diagnostic))
    return false;
  if (Architecture == Arch::AArch64) {
    int64_t StackOffset = 0;
    for (size_t I = 0; I < FixedCount; ++I) {
      const auto &P = Hint.Parameters[I];
      if (P.Location.Kind == SourceABICarrierKind::Stack)
        StackOffset =
            std::max(StackOffset, P.Location.EntryStackOffset + P.Type->Size);
    }
    StackOffset = (StackOffset + 7) & ~int64_t(7);
    for (size_t I = FixedCount; I < Hint.Parameters.size(); ++I) {
      auto &P = Hint.Parameters[I];
      P.Location = {SourceABICarrierKind::Stack, 0, StackOffset, P.Type->Size};
      StackOffset += 8;
    }
  }
  return validateSourceABI(Hint, Diagnostic);
}

bool assignSysVX64VariadicSourceABI(SourceFunctionTypeHint &Hint,
                                    unsigned FixedCount,
                                    std::string &Diagnostic) {
  return assignDarwinVariadicSourceABI(Hint, FixedCount, Arch::X64, Diagnostic);
}

} // namespace neverd
