//===- LanguageEHDwarf.h - DWARF call frame information -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Normalized DWARF call frame information: the canonicalized call-frame
/// instruction, the Common Information Entry that declares a frame's pointer
/// encodings, and the Frame Description Entry that names one function's
/// unwinding.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_LANGUAGEEHDWARF_H
#define NEVERD_LOADER_LANGUAGEEHDWARF_H

#include "neverd/Common.h"

#include <cstdint>
#include <string>
#include <vector>

namespace neverd {

/// A canonicalized call-frame instruction.  The primary and extended opcode
/// forms of one operation normalize to the same kind so consumers reason about
/// unwind semantics instead of DWARF encoding trivia; `Opaque` retains the
/// exact bytes of an operation this decoder does not model.
enum class CFIOpKind : uint8_t {
  Nop,
  SetLoc,
  AdvanceLoc,
  DefCFA,
  DefCFARegister,
  DefCFAOffset,
  DefCFAExpression,
  Offset,
  ValOffset,
  Register,
  Expression,
  ValExpression,
  Restore,
  Undefined,
  SameValue,
  RememberState,
  RestoreState,
  /// `DW_CFA_GNU_args_size`: bytes of stack arguments the unwinder must pop.
  GnuArgsSize,
  /// AArch64 pointer-authentication return-address signing state toggle.
  NegateRAState,
  /// AArch64 return-address signing with the PC as a diversifier.
  NegateRAStateWithPC,
  Opaque,
};

const char *getCFIOpKindName(CFIOpKind Kind);

/// One decoded call-frame instruction.  `CodeOffset` is the offset from the
/// owning FDE's initial location at which the rule takes effect, already
/// scaled by the CIE's code alignment factor.  `Offset` is already scaled by
/// the data alignment factor for the rules that are defined to use it, so a
/// consumer never has to re-apply a factor it did not read.
struct CFIInstruction {
  CFIOpKind Kind = CFIOpKind::Opaque;
  uint64_t CodeOffset = 0;
  uint64_t Register = 0;
  uint64_t Register2 = 0;
  int64_t Offset = 0;
  std::vector<uint8_t> Expression;
  std::vector<uint8_t> OperandBytes;
};

/// A decoded Common Information Entry.  The pointer encodings are retained
/// exactly because they are part of the contract a rewriter must reproduce.
struct DwarfCIE {
  /// Offset of this CIE from the start of its frame section.
  uint64_t SectionOffset = 0;
  uint8_t Version = 0;
  std::string Augmentation;
  uint64_t CodeAlignmentFactor = 0;
  int64_t DataAlignmentFactor = 0;
  uint64_t ReturnAddressRegister = 0;
  /// Address size and segment selector size, present from CIE version 4.
  uint8_t AddressSize = 0;
  uint8_t SegmentSelectorSize = 0;
  /// `DW_EH_PE_omit` when the augmentation did not declare the encoding.
  uint8_t FDEPointerEncoding = 0xFF;
  uint8_t LSDAPointerEncoding = 0xFF;
  uint8_t PersonalityEncoding = 0xFF;
  va_t PersonalityVA = 0;
  /// Address of the slot an indirect personality encoding loads through.
  /// Zero for a direct encoding.
  va_t PersonalitySlotVA = 0;
  /// True when the augmentation string begins with 'z' and therefore carries a
  /// self-describing length; a CIE without it cannot be skipped safely by a
  /// consumer that does not understand every augmentation character.
  bool HasAugmentationData = false;
  /// 'S': frames described by this CIE are signal frames, so the return
  /// address is the precise interrupted PC rather than a post-call address.
  bool IsSignalFrame = false;
  /// 'B'/'G': AArch64 pointer authentication and MTE tagged frames.
  bool HasPointerAuth = false;
  bool HasMTETaggedFrame = false;
  std::vector<CFIInstruction> InitialInstructions;
};

/// A decoded Frame Description Entry paired with its resolved CIE.
struct DwarfFDE {
  uint64_t SectionOffset = 0;
  uint64_t CIESectionOffset = 0;
  va_t InitialLocation = 0;
  /// Section offset of the `initial_location` field itself.  In a relocatable
  /// object that field is the target of a relocation and holds no address
  /// until one is applied, so a reader has to be able to ask whether it was.
  uint64_t InitialLocationOffset = 0;
  uint64_t AddressRange = 0;
  /// Zero when the CIE declared no LSDA encoding or the FDE omitted it.
  va_t LSDAVA = 0;
  std::vector<CFIInstruction> Instructions;
};

} // namespace neverd

#endif // NEVERD_LOADER_LANGUAGEEHDWARF_H
