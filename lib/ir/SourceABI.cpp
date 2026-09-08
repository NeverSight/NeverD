#include "neverd/ir/SourceABI.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/lift/AArch64Regs.h"

#include <algorithm>
#include <set>
#include <utility>

namespace neverd {
namespace {
bool scalarType(const TypeRef &Type) {
  if (!Type)
    return false;
  if (Type->Kind == NdTypeKind::Ptr)
    return Type->Size == 8 && Type->Pointee;
  if (Type->Kind == NdTypeKind::Float)
    return Type->Size == 4 || Type->Size == 8;
  return Type->Kind == NdTypeKind::Int && (Type->Size == 1 || Type->Size == 2 ||
                                           Type->Size == 4 || Type->Size == 8);
}

bool fail(std::string &Diagnostic, const char *Message) {
  Diagnostic = Message;
  return false;
}
} // namespace

bool validateSourceABI(const SourceFunctionTypeHint &Hint,
                       std::string &Diagnostic) {
  Diagnostic.clear();
  if (!Hint.HasExplicitABI ||
      (Hint.Architecture != Arch::AArch64 && Hint.Architecture != Arch::X64) ||
      Hint.Parameters.size() > 64 || !Hint.ReturnType)
    return fail(Diagnostic,
                "Source ABI requires an explicit arm64/x86_64 layout");
  const auto &TRI = getTargetRegInfo(Hint.Architecture);
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
  if (Hint.ReturnType->Kind == NdTypeKind::Void) {
    if (Hint.ReturnLocation.Kind != SourceABICarrierKind::None ||
        Hint.ReturnLocation.ValueBytes != 0)
      return fail(Diagnostic,
                  "Void source result has a physical value carrier");
  } else if (!ValidLocation(Hint.ReturnType, Hint.ReturnLocation, true)) {
    return fail(Diagnostic, "Unsupported source return carrier");
  }
  std::set<std::pair<SourceABICarrierKind, uint64_t>> Registers;
  std::vector<std::pair<int64_t, int64_t>> StackRanges;
  for (const auto &Parameter : Hint.Parameters) {
    if (!ValidLocation(Parameter.Type, Parameter.Location, false))
      return fail(Diagnostic, "Unsupported source parameter carrier");
    if (Parameter.Location.Kind == SourceABICarrierKind::Stack) {
      const int64_t Begin = Parameter.Location.EntryStackOffset;
      const int64_t End = Begin + Parameter.Location.ValueBytes;
      for (const auto &[OtherBegin, OtherEnd] : StackRanges)
        if (Begin < OtherEnd && OtherBegin < End)
          return fail(Diagnostic,
                      "Overlapping source stack parameter locations");
      StackRanges.emplace_back(Begin, End);
    } else if (!Registers
                    .emplace(Parameter.Location.Kind,
                             Parameter.Location.RegisterOffset)
                    .second) {
      return fail(Diagnostic, "Source parameters share one physical register");
    }
  }
  return true;
}

bool assignDarwinScalarSourceABI(SourceFunctionTypeHint &Hint,
                                 Arch Architecture, std::string &Diagnostic) {
  Diagnostic.clear();
  if ((Architecture != Arch::AArch64 && Architecture != Arch::X64) ||
      !Hint.ReturnType || Hint.Parameters.size() > 64)
    return fail(Diagnostic, "Unsupported Darwin scalar source ABI");
  const auto &TRI = getTargetRegInfo(Architecture);
  size_t IntegerIndex = 0;
  size_t FloatIndex = 0;
  int64_t StackOffset = Architecture == Arch::X64 ? 8 : 0;
  for (auto &Parameter : Hint.Parameters) {
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
  if (Hint.ReturnType->Kind != NdTypeKind::Void) {
    if (!scalarType(Hint.ReturnType))
      return fail(Diagnostic, "Unsupported Darwin scalar return value");
    const bool Floating = Hint.ReturnType->Kind == NdTypeKind::Float;
    Hint.ReturnLocation.Kind = Floating ? SourceABICarrierKind::FloatingRegister
                                        : SourceABICarrierKind::IntegerRegister;
    Hint.ReturnLocation.RegisterOffset =
        Floating ? TRI.FPReturnReg : TRI.IntReturnReg;
    Hint.ReturnLocation.ValueBytes = Hint.ReturnType->Size;
  }
  Hint.Architecture = Architecture;
  Hint.HasExplicitABI = true;
  return validateSourceABI(Hint, Diagnostic);
}

bool assignDarwinObjCSourceABI(SourceFunctionTypeHint &Hint, Arch Architecture,
                               std::string &Diagnostic) {
  if (Hint.Origin != SourceFunctionTypeHint::OriginKind::ObjCRuntime ||
      Hint.Parameters.size() < 2)
    return fail(Diagnostic, "Unsupported Objective-C source ABI");
  return assignDarwinScalarSourceABI(Hint, Architecture, Diagnostic);
}

} // namespace neverd
