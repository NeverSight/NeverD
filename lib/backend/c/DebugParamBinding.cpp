//===- DebugParamBinding.cpp - Debug parameters by location ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "DebugParamBinding.h"

#include "neverd/ir/SourceParameterPlacement.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedCallConvention.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>

using namespace neverd;

std::optional<int64_t>
neverd::stackParamOffset(Arch A, BinaryFormat F,
                         llvm::ArrayRef<RecoveredParamSite> Sites,
                         size_t Index) {
  const RecoveredParamSite &Site = Sites[Index];
  if (Site.RegOff != kNoParamReg || Site.MedIndex < 0)
    return std::nullopt;
  const auto Layout = getTargetRegInfo(A).integerArgumentLayout(F);
  if (!Layout.SlotBytes)
    return std::nullopt;
  // Stack arguments are numbered after every register position, or after the
  // registers the function uses where they follow those (i386 regparm): from
  // the lowest stack number, the first slot's.  A register parameter MedIR
  // promotes later, such as one a forwarder passes on, takes no number of
  // the stack's.
  int First = static_cast<int>(Layout.Registers.size());
  if (const CallArgumentConvention *Convention = callArgumentConvention(A, F);
      Convention && Convention->StackArgumentsFollowUsedRegisters) {
    First = Site.MedIndex;
    for (const RecoveredParamSite &Other : Sites)
      if (Other.RegOff == kNoParamReg && Other.MedIndex >= 0)
        First = std::min(First, Other.MedIndex);
  }
  if (Site.MedIndex < First)
    return std::nullopt;
  return Layout.EntryStackBase +
         static_cast<int64_t>(Site.MedIndex - First) * Layout.SlotBytes;
}

DebugParamBindings
neverd::bindDebugParams(Arch A, BinaryFormat F, const FunctionSym &Signature,
                        llvm::ArrayRef<RecoveredParamSite> Sites,
                        const DebugContext *Dbg, InstructionMode EntryMode) {
  DebugParamBindings Bindings;
  Bindings.Params.assign(Sites.size(), {});
  // A function of a language with conventions of its own, such as Go, has
  // no rules that say where its parameters arrive.
  if (!Signature.PlatformConvention)
    return Bindings;
  std::vector<TypeRef> Types;
  for (const auto &[Name, Type] : Signature.Params) {
    (void)Name;
    if (Type && Dbg)
      Dbg->completeType(Type);
    Types.push_back(Type);
  }
  if (Signature.ReturnType && Dbg)
    Dbg->completeType(Signature.ReturnType);
  // fastcall and thiscall pass leading values in registers that the
  // ordinary i386 rules do not describe, and a function built without
  // lowering does not say where its parameters arrive.
  const bool OrdinaryConvention =
      A != Arch::X86 || Signature.CallConv == DebugCallConv::Unknown ||
      Signature.CallConv == DebugCallConv::Cdecl ||
      Signature.CallConv == DebugCallConv::Stdcall;
  // A function whose code reads no parameter has none to locate: what its
  // declaration takes goes unread.
  const bool Located =
      Sites.empty() || llvm::any_of(Sites, [](const RecoveredParamSite &Site) {
        return Site.RegOff != kNoParamReg || Site.MedIndex >= 0;
      });
  // Where the debug information says the parameters enter in registers, as
  // an i386 local function's do, they are there, whatever the convention.
  const auto Placement =
      !Located ? std::nullopt
      : !Signature.ParamEntryLocations.empty()
          ? placeEnteredParameters(A, F, Signature.ReturnType, Types,
                                   Signature.ParamEntryLocations)
      : OrdinaryConvention
          ? placeSourceParameters(A, F, Signature.ReturnType, Types, false,
                                  std::nullopt, EntryMode)
          : std::nullopt;
  if (!Placement) {
    // A function that takes values in registers takes them in no order its
    // positions describe.
    if (!Signature.ParamEntryLocations.empty())
      return Bindings;
    // Without rules or locations, a debug parameter is the one at its
    // position only while each before it fills one integer slot, as in
    // every convention.
    const uint16_t Word = getTargetRegInfo(A).PointerSize;
    for (size_t PI = 0; PI < std::min(Sites.size(), Types.size()); ++PI) {
      const TypeRef &Type = Types[PI];
      if (!Type || Type->Size > Word ||
          !(Type->Kind == NdTypeKind::Int || Type->Kind == NdTypeKind::Ptr ||
            (Type->Kind == NdTypeKind::Struct && Type->IsEnum)))
        break;
      Bindings.Params[PI] = {static_cast<int>(PI), 0, true, false, false};
    }
    return Bindings;
  }
  Bindings.Placed = true;
  for (size_t PI = 0; PI < Sites.size(); ++PI) {
    const RecoveredParamSite &Site = Sites[PI];
    const std::optional<int64_t> Stack = stackParamOffset(A, F, Sites, PI);
    auto At = [&](const SourceABIValueLocation &Where, int64_t &Delta) {
      Delta = 0;
      if (Where.Kind == SourceABICarrierKind::Stack) {
        if (!Stack || *Stack < Where.EntryStackOffset ||
            *Stack >=
                Where.EntryStackOffset + std::max<int64_t>(Where.ValueBytes, 1))
          return false;
        Delta = *Stack - Where.EntryStackOffset;
        return true;
      }
      return Site.RegOff != kNoParamReg && Where.RegisterOffset == Site.RegOff;
    };
    int64_t Delta = 0;
    if (Placement->ResultPointer && At(*Placement->ResultPointer, Delta)) {
      Bindings.Params[PI].Result = true;
      continue;
    }
    for (size_t DI = 0; DI < Placement->Parameters.size(); ++DI)
      for (const SourceParameterPiece &Piece : Placement->Parameters[DI]) {
        if (!At(Piece.Location, Delta))
          continue;
        const TypeRef &Declared = Types[DI];
        const uint16_t Size = Declared ? Declared->Size : 0;
        // A record is the location's value only where the source says the
        // call passes its bytes: a C++ class can arrive as the address of a
        // copy at the same place.
        const bool Bytes = !Declared || Declared->Kind != NdTypeKind::Struct ||
                           Declared->IsEnum ||
                           Declared->Passing == NdRecordPassing::ByValue;
        DebugParamBinding &B = Bindings.Params[PI];
        B.Index = static_cast<int>(DI);
        B.Offset = static_cast<uint16_t>(Piece.Offset + Delta);
        B.Whole = Bytes && !Piece.Indirect && Piece.Offset == 0 && Delta == 0 &&
                  Piece.Location.ValueBytes >= Size &&
                  (Piece.Location.Kind != SourceABICarrierKind::Stack ||
                   Site.Bytes >= Size);
        B.Indirect = Piece.Indirect && Delta == 0;
      }
  }
  return Bindings;
}

std::string neverd::debugParamName(const FunctionSym &Signature,
                                   const DebugParamBinding &Binding) {
  if (Binding.Result)
    return "result";
  if (Binding.Index < 0 ||
      static_cast<size_t>(Binding.Index) >= Signature.Params.size())
    return {};
  // A later piece of a value, such as a record's second register, is named
  // after the value and where the piece starts in it.
  const std::string &Name = Signature.Params[Binding.Index].first;
  return Name.empty() || Binding.Whole || Binding.Offset == 0
             ? Name
             : Name + "_" + std::to_string(Binding.Offset);
}
