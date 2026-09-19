#include "neverd/ir/SourceABI.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/lift/X86Regs.h"

#include <algorithm>
#include <set>
#include <utility>

namespace neverd {
namespace {
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
  if (!Remaining || !Left || !Right || Depth > 16 ||
      Left->Kind != Right->Kind || Left->Size != Right->Size ||
      Left->IsSigned != Right->IsSigned)
    return false;
  --Remaining;
  switch (Left->Kind) {
  case NdTypeKind::Void:
    return Left->Size == 0;
  case NdTypeKind::Int:
    return Left->Size == 1 || Left->Size == 2 || Left->Size == 4 ||
           Left->Size == 8 || Left->Size == 16;
  case NdTypeKind::Float:
    return Left->Size == 4 || Left->Size == 8;
  case NdTypeKind::Ptr:
    return Left->Size == 8 &&
           equalTypes(Left->Pointee, Right->Pointee, Depth + 1, Remaining);
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
    if (Left->Size != 0 || !Left->RetType || !Right->RetType ||
        Left->RetType->Kind == NdTypeKind::Func ||
        Left->ParamTypes.size() > 64 ||
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
  if (Type->Kind == NdTypeKind::Ptr)
    return Type->Size == 8 && equalSourceTypes(Type, Type);
  if (Type->Kind == NdTypeKind::Float)
    return Type->Size == 4 || Type->Size == 8;
  return Type->Kind == NdTypeKind::Int && (Type->Size == 1 || Type->Size == 2 ||
                                           Type->Size == 4 || Type->Size == 8);
}

bool fail(std::string &Diagnostic, const char *Message) {
  Diagnostic = Message;
  return false;
}

bool swiftFixedShape(const SourceFunctionTypeHint &Hint, Arch Architecture) {
  if (Architecture != Arch::AArch64 && Architecture != Arch::X64)
    return false;
  const auto Word = [](const TypeRef &T) {
    return scalarType(T) && T->Size == 8 && T->Kind != NdTypeKind::Float;
  };
  const auto Scalar = [](const TypeRef &T) {
    return scalarType(T) && T->Kind != NdTypeKind::Float;
  };
  if (!Hint.ReturnType || Hint.Parameters.size() > 64 ||
      !std::all_of(Hint.Parameters.begin(), Hint.Parameters.end(),
                   [&](const auto &P) { return Scalar(P.Type); }))
    return false;
  unsigned IndirectResults = 0, Contexts = 0;
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
  const auto Members = sourceAggregateMembers(Hint.ReturnType);
  return !Members.empty() && Members.size() <= 2 &&
         std::all_of(Members.begin(), Members.end(),
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
  unsigned Remaining = 4096;
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
  const bool Floating = Result.front().Type->Kind == NdTypeKind::Float;
  if (Floating) {
    if (Result.size() > 4 ||
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
  const bool FullWords =
      Result.size() <= 2 &&
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

std::vector<SourceABIParameter>
sourceABIParameters(const SourceFunctionTypeHint &Hint) {
  std::string Error;
  if (!validateSourceABI(Hint, Error))
    return {};
  std::vector<SourceABIParameter> Result;
  for (size_t I = 0; I < Hint.Parameters.size(); ++I) {
    const auto &P = Hint.Parameters[I];
    if (P.Components.empty())
      Result.push_back({I, 0, P.Name, P.Type, P.Location});
    else {
      const auto Members = sourceAggregateMembers(P.Type);
      for (size_t J = 0; J < Members.size(); ++J)
        Result.push_back({I, Members[J].ByteOffset, P.Name, Members[J].Type,
                          P.Components[J]});
    }
  }
  return Result;
}

bool validateSourceABI(const SourceFunctionTypeHint &Hint,
                       std::string &Diagnostic) {
  Diagnostic.clear();
  if ((Hint.Convention != SourceFunctionTypeHint::ConventionKind::C &&
       Hint.Convention != SourceFunctionTypeHint::ConventionKind::Swift) ||
      !Hint.HasExplicitABI ||
      (Hint.Architecture != Arch::AArch64 && Hint.Architecture != Arch::X64) ||
      Hint.Parameters.size() > 64 || !Hint.ReturnType)
    return fail(Diagnostic,
                "Source ABI requires an explicit arm64/x86_64 layout");
  const auto &TRI = getTargetRegInfo(Hint.Architecture);
  if (Hint.Convention == SourceFunctionTypeHint::ConventionKind::Swift) {
    if (!swiftFixedShape(Hint, Hint.Architecture))
      return fail(Diagnostic, "Unsupported fixed Swift source ABI shape");
    size_t IntegerIndex = 0;
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
    if (!scalarType(Type) || Location.ValueBytes != Type->Size)
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
             Location.EntryStackOffset <= 4096 - Type->Size &&
             Location.EntryStackOffset % Type->Size == 0;
    if (Location.EntryStackOffset != 0)
      return false;
    if (Location.Kind == SourceABICarrierKind::FloatingRegister)
      return Type->Kind == NdTypeKind::Float &&
             TRI.isVectorReg(Location.RegisterOffset) &&
             (!IsReturn || Location.RegisterOffset == TRI.FPReturnReg);
    if (Location.Kind == SourceABICarrierKind::IntegerRegister)
      return Type->Kind != NdTypeKind::Float &&
             IntegerRegister(Location.RegisterOffset) &&
             (!IsReturn || Location.RegisterOffset == TRI.IntReturnReg);
    return false;
  };
  const auto EmptyLocation = [](const SourceABIValueLocation &Location) {
    return Location.Kind == SourceABICarrierKind::None &&
           Location.RegisterOffset == 0 && Location.EntryStackOffset == 0 &&
           Location.ValueBytes == 0 && !Location.ExtendTo32Bits;
  };
  auto AggregateLocations = [&](const TypeRef &Type,
                                const std::vector<SourceABIValueLocation>
                                    &Parts,
                                bool IsReturn) {
    const auto Members = sourceAggregateMembers(Type);
    if (Members.empty() || Parts.size() != Members.size())
      return false;
    const bool Floating = Members.front().Type->Kind == NdTypeKind::Float;
    if (Floating && Hint.Architecture != Arch::AArch64)
      return false;
    const auto Bank = Floating   ? TRI.FPParamRegs
                      : IsReturn ? TRI.IntReturnRegs
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
    if (!EmptyLocation(Hint.ReturnLocation) ||
        !AggregateLocations(Hint.ReturnType, Hint.ReturnComponents, true))
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
  std::vector<std::pair<int64_t, int64_t>> StackRanges;
  size_t PhysicalCount = 0;
  for (const auto &Parameter : Hint.Parameters) {
    if (Parameter.TheRole != SourceParameterTypeHint::Role::Ordinary &&
        Hint.Convention != SourceFunctionTypeHint::ConventionKind::Swift)
      return fail(Diagnostic,
                  "Swift parameter role requires the Swift convention");
    if (!Parameter.Components.empty()) {
      if (!EmptyLocation(Parameter.Location) ||
          !AggregateLocations(Parameter.Type, Parameter.Components, false))
        return fail(Diagnostic, "Unsupported source record parameter carriers");
    } else if (!ValidLocation(Parameter.Type, Parameter.Location, false)) {
      return fail(Diagnostic, "Unsupported source parameter carrier");
    }
    const auto Locations = Parameter.Components.empty()
                               ? std::vector{Parameter.Location}
                               : Parameter.Components;
    PhysicalCount += Locations.size();
    if (PhysicalCount > static_cast<size_t>(limits::kMaxBoundSourceCallArgs))
      return fail(Diagnostic, "Source parameter carrier budget exceeded");
    for (const auto &Location : Locations) {
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
  return true;
}

namespace {
bool assignDarwinSourceABI(SourceFunctionTypeHint &Hint, Arch Architecture,
                           std::string &Diagnostic, bool Records,
                           SourceFunctionTypeHint::ConventionKind Convention) {
  Diagnostic.clear();
  if ((Architecture != Arch::AArch64 && Architecture != Arch::X64) ||
      !Hint.ReturnType || Hint.Parameters.size() > 64)
    return fail(Diagnostic, "Unsupported Darwin fixed source ABI");
  const auto &TRI = getTargetRegInfo(Architecture);
  size_t IntegerIndex = 0;
  size_t FloatIndex = 0;
  int64_t StackOffset = Architecture == Arch::X64 ? 8 : 0;
  for (auto &Parameter : Hint.Parameters) {
    Parameter.Components.clear();
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
      default:
        return fail(Diagnostic, "Unsupported Darwin special parameter ABI");
      }
      continue;
    }
    if (Records && Parameter.Type &&
        Parameter.Type->Kind == NdTypeKind::Struct) {
      const auto Members = sourceAggregateMembers(Parameter.Type);
      if (Members.empty())
        return fail(Diagnostic, "Unsupported Darwin record parameter ABI");
      const bool Floating = Members.front().Type->Kind == NdTypeKind::Float;
      if (Floating && Architecture != Arch::AArch64)
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
    auto &Location = Parameter.Location;
    Location = {};
    Location.ValueBytes = Parameter.Type->Size;
    const bool Floating = Parameter.Type->Kind == NdTypeKind::Float;
    auto &Index = Floating ? FloatIndex : IntegerIndex;
    const auto Bank = Floating ? TRI.FPParamRegs : TRI.IntParamRegs;
    if (Index < Bank.size()) {
      Location.Kind = Floating ? SourceABICarrierKind::FloatingRegister
                               : SourceABICarrierKind::IntegerRegister;
      Location.RegisterOffset = Bank[Index++];
      // Both Darwin ABIs require callers to extend narrow integer register
      // arguments to 32 bits. Stack arguments retain their own storage width.
      Location.ExtendTo32Bits =
          Parameter.Type->Kind == NdTypeKind::Int && Parameter.Type->Size < 4;
    } else {
      // Apple arm64 packs fixed stack scalars at natural alignment. x86_64
      // Darwin uses eight-byte argument slots, above the pushed return address.
      const int64_t Alignment =
          Architecture == Arch::AArch64 ? Parameter.Type->Size : 8;
      StackOffset = (StackOffset + Alignment - 1) & -Alignment;
      Location.Kind = SourceABICarrierKind::Stack;
      Location.EntryStackOffset = StackOffset;
      StackOffset += Architecture == Arch::AArch64 ? Parameter.Type->Size : 8;
    }
  }
  Hint.ReturnLocation = {};
  Hint.ReturnComponents.clear();
  if (Records && Hint.ReturnType->Kind == NdTypeKind::Struct) {
    const auto Members = sourceAggregateMembers(Hint.ReturnType);
    if (Members.empty())
      return fail(Diagnostic, "Unsupported Darwin record return ABI");
    const bool Floating = Members.front().Type->Kind == NdTypeKind::Float;
    const auto Bank = Floating ? TRI.FPParamRegs : TRI.IntReturnRegs;
    if ((Floating && Architecture != Arch::AArch64) ||
        Members.size() > Bank.size())
      return fail(Diagnostic, "Unsupported Darwin record return ABI");
    for (size_t I = 0; I < Members.size(); ++I)
      Hint.ReturnComponents.push_back(
          {Floating ? SourceABICarrierKind::FloatingRegister
                    : SourceABICarrierKind::IntegerRegister,
           Bank[I], 0, Members[I].Type->Size});
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
  if (!Hint.ValueWitness)
    return false;
  const auto Expected =
      swiftValueWitnessSourceCallHint(Architecture, *Hint.ValueWitness);
  return Expected &&
         Hint.CallKind == SourceCallTypeHint::Kind::SwiftValueWitness &&
         Hint.TargetAddress == 0 && Hint.TargetName == Expected->TargetName &&
         Hint.Selector.empty() && Hint.OwnerClass.empty() &&
         Hint.SelectorReferenceAddress == 0 && !Hint.DoesNotReturn &&
         !Hint.ReturnedArgument && !Hint.RuntimeObjCResultType &&
         Hint.BorrowedByteInputs.empty() && Hint.SwiftStringInputs.empty() &&
         !Hint.Format && !Hint.Receiver && !Hint.SelectorResultUse &&
         !Hint.SelectorResultTypeUse &&
         !Hint.SelectorArgumentTypeUse && !Hint.SelectorArgumentStorageUse &&
         Hint.ByteCount == 0 && Hint.ImmutablePointerSlot == 0 &&
         equalSourceABIs(Hint.Signature, Expected->Signature);
}

bool assignDarwinObjCSourceABI(SourceFunctionTypeHint &Hint, Arch Architecture,
                               std::string &Diagnostic) {
  if ((Hint.Origin != SourceFunctionTypeHint::OriginKind::ObjCRuntime &&
       Hint.Origin != SourceFunctionTypeHint::OriginKind::ObjCSDK) ||
      Hint.Parameters.size() < 2)
    return fail(Diagnostic, "Unsupported Objective-C source ABI");
  return assignDarwinFixedSourceABI(Hint, Architecture, Diagnostic);
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

} // namespace neverd
