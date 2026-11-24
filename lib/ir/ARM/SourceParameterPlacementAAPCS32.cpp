//===- SourceParameterPlacementAAPCS32.cpp - ARM ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The ARM procedure call standard's (AAPCS) scalar parameter passing.
/// Integers and pointers take r0-r3 and then 4-byte stack slots.  A 64-bit
/// integer takes an even-odd register pair or an 8-byte aligned stack slot,
/// never one register and the stack, and once an argument goes on the stack
/// no later one takes a core register (rules C.3-C.8).  The VFP variant,
/// which Windows on ARM uses, passes a float or a double in the lowest free
/// s0-s15 or d0-d7, a float back-filling the single register a double
/// skipped, and once one goes on the stack no later one takes a VFP register
/// (C.1, C.2).  An ELF image's header says which variant it uses and Apple's
/// armv7 calls with the base one, so neither places a floating value here.
/// Records are not placed.
///
//===----------------------------------------------------------------------===//

#include "../SourceParameterPlacementDetail.h"

#include "neverd/ArchSupport.h"
#include "neverd/lift/ARM/ARMRegs.h"

#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace neverd;
using namespace neverd::source_placement;

namespace {
/// The single-precision registers s0-s15 the VFP variant passes in.
constexpr unsigned kVFPArgumentSingles = 16;
/// The bytes of one core register and of one stack slot.
constexpr unsigned kWordBytes = 4;
} // namespace

std::optional<SourceParameterPlacement>
source_placement::placeAAPCS32(const TargetRegInfo &TRI, BinaryFormat F,
                               const TypeRef &ReturnType,
                               llvm::ArrayRef<TypeRef> ParamTypes) {
  const auto Layout = TRI.integerArgumentLayout(F);
  // A record result in memory takes r0 for its address, which these rules
  // do not place.
  if (!ReturnType || isRecord(ReturnType) || Layout.Registers.empty() ||
      Layout.SlotBytes != kWordBytes)
    return std::nullopt;
  const bool VFP = F == BinaryFormat::COFF;
  const bool Little = archLittleEndian(TRI.TheArch);
  const unsigned CoreRegisters = static_cast<unsigned>(Layout.Registers.size());
  SourceParameterPlacement Placement;
  unsigned NextCore = 0;
  // Bytes past the first stack argument, which stays 8-byte aligned.
  uint64_t NextStack = 0;
  uint32_t FreeSingles = (uint32_t{1} << kVFPArgumentSingles) - 1;
  for (const TypeRef &Type : ParamTypes) {
    // A pointer of another width than the target's is no ARM value.
    if (!Type || isRecord(Type) ||
        (Type->Size != 1 && Type->Size != 2 && Type->Size != 4 &&
         Type->Size != 8) ||
        (Type->Kind == NdTypeKind::Ptr && Type->Size != TRI.PointerSize))
      break;
    std::vector<SourceParameterPiece> Pieces;
    if (isFloatingScalar(Type)) {
      if (!VFP || (Type->Size != 4 && Type->Size != 8))
        break;
      // C.1: the lowest run of free single registers that holds the value,
      // a double's starting at an even one.
      const unsigned Count = Type->Size / kWordBytes;
      const uint32_t Run = (uint32_t{1} << Count) - 1;
      std::optional<unsigned> First;
      for (unsigned S = 0; S + Count <= kVFPArgumentSingles; S += Count)
        if (((FreeSingles >> S) & Run) == Run) {
          First = S;
          break;
        }
      if (First) {
        FreeSingles &= ~(Run << *First);
        Pieces.push_back(registerPiece(SourceABICarrierKind::FloatingRegister,
                                       armreg::D0 + *First * kWordBytes, 0,
                                       Type->Size));
      } else {
        // C.2: the stack, aligned for the value, and no VFP register after.
        FreeSingles = 0;
        NextStack = llvm::alignTo(NextStack, Type->Size);
        Pieces.push_back(
            stackPiece(Layout.EntryStackBase + static_cast<int64_t>(NextStack),
                       0, Type->Size));
        NextStack += Type->Size;
      }
    } else if (isIntegerScalar(Type)) {
      const unsigned Words = Type->Size > kWordBytes ? 2 : 1;
      // C.3: a doubleword starts at an even register.
      if (Words == 2)
        NextCore = llvm::alignTo(NextCore, 2);
      if (NextCore + Words <= CoreRegisters) {
        // C.4: core registers, holding what LDM would load into them.
        for (unsigned I = 0; I < Words; ++I)
          Pieces.push_back(registerPiece(
              SourceABICarrierKind::IntegerRegister,
              Layout.Registers[NextCore + I],
              static_cast<uint16_t>((Little ? I : Words - I - 1) * kWordBytes),
              static_cast<uint16_t>(
                  std::min<unsigned>(Type->Size, kWordBytes))));
        NextCore += Words;
      } else {
        // C.6-C.8: the stack, aligned for a doubleword, and no core register
        // after.  A narrow value sits where a word load finds its bits.
        NextCore = CoreRegisters;
        NextStack = llvm::alignTo(NextStack, Words * kWordBytes);
        const uint16_t Pad = static_cast<uint16_t>(
            Little || Words == 2 ? 0 : kWordBytes - Type->Size);
        Pieces.push_back(stackPiece(Layout.EntryStackBase +
                                        static_cast<int64_t>(NextStack) + Pad,
                                    0, Type->Size));
        NextStack += Words * kWordBytes;
      }
    } else {
      break;
    }
    Placement.Parameters.push_back(std::move(Pieces));
  }
  return Placement;
}
