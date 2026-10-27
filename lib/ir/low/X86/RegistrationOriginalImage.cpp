//===- RegistrationOriginalImage.cpp - Scoped PE32 values ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep original-image arithmetic and immutable values in an explicit scope.
//===----------------------------------------------------------------------===//

#include "RegistrationOriginalImage.h"

#include "../../../loader/ReadOnlyBytesIndex.h"

#include "neverd/Limits.h"
#include "neverd/decode/InstructionRelocations.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include <bit>

namespace neverd::registration_state {

bool OriginalImageFrameProof::hasMappedIdentity(uint32_t Address,
                                                size_t &Work) {
  if (!registration_abi::chargeCalleeWork(Work, Mapped.size() + 1))
    return false;
  const auto [It, New] = Mapped.try_emplace(Address, false);
  if (New)
    It->second = Address >= Image.Base &&
                 registration_abi::hasSEHCallbackImageMemory(Image, Address, 1,
                                                             false, Work);
  return It->second;
}

const registration_abi::ImmutableRegistrationScalar *
OriginalImageFrameProof::read(va_t Address, uint16_t Width, size_t &Work) {
  if (!registration_abi::chargeCalleeWork(Work, Reads.size() + 1))
    return nullptr;
  const auto [It, New] = Reads.try_emplace({Address, Width});
  if (New) {
    // A runtime cookie is an opaque protocol value even when its source
    // section is marked read-only. Ordinary/narrow and nested helper reads
    // must not turn its file bytes into a constant. Root protocol loads use
    // recordCookieRead instead and retain the state owner's live identity.
    const auto RVA = Image.DynInfo.SecurityCookieRVA;
    if (RVA && Image.Base <= UINT32_MAX - 3 &&
        RVA <= UINT32_MAX - 3 - Image.Base) {
      const va_t Cookie = Image.Base + RVA;
      if (Width &&
          (Address <= Cookie ? Cookie - Address < Width : Address - Cookie < 4))
        return nullptr;
    }
    It->second = registration_abi::readImmutableRegistrationScalar(
        Image, Address, Width, Work);
  }
  return It->second ? &*It->second : nullptr;
}

bool OriginalImageFrameProof::permitsRead(va_t Address, uint16_t Width,
                                          size_t &Work) {
  return read(Address, Width, Work) != nullptr;
}

bool OriginalImageFrameProof::recordAddress(uint32_t Address, size_t &Work) {
  if (!hasMappedIdentity(Address, Work) ||
      !registration_abi::chargeCalleeWork(Work, Addresses.size() + 1))
    return false;
  Addresses.insert(Address);
  return true;
}

bool OriginalImageFrameProof::recordReadAddress(const FrameValue &Address,
                                                uint16_t Width, size_t &Work) {
  const auto VA = Address.preferredAddress();
  if (!VA || !Width ||
      !registration_abi::chargeCalleeWork(Work,
                                          ReadAddressIdentities.size() + 1))
    return false;
  const bool Exact = Address.ImageAddress &&
                     Address == FrameValue::imageAddress(*Address.ImageAddress);
  const auto [It, New] =
      ReadAddressIdentities.emplace(std::make_pair(*VA, Width), Exact);
  if (!New)
    It->second &= Exact;
  return true;
}

bool OriginalImageFrameProof::recordCookieRead(const LowOp &Op,
                                               const FrameTransfer &Transfer,
                                               size_t &Work) {
  if (Op.Opcode != NdOp::LOAD || Op.NumInputs != 1)
    return true;
  const auto Address = Transfer.read(Op.Inputs[0]);
  const auto VA = Address.preferredAddress();
  if (!VA)
    return Address.Offset.has_value();
  if (Op.Output.Size != 4 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      !registration_abi::chargeCalleeWork(
          Work, CookieReadAddressIdentities.size() + 1))
    return false;
  const bool Exact = Address.ImageAddress &&
                     Address == FrameValue::imageAddress(*Address.ImageAddress);
  const auto [It, New] = CookieReadAddressIdentities.emplace(*VA, Exact);
  if (!New)
    It->second &= Exact;
  return true;
}

bool OriginalImageFrameProof::recordProtocol(
    const ExceptionAddressRange &Range, const std::map<va_t, va_t> &Operands,
    size_t &Work) {
  if (!Range.isValid() || Range.End > uint64_t(UINT32_MAX) + 1 ||
      !registration_abi::chargeCalleeWork(
          Work, Range.size() + ProtocolCode.size() + Operands.size() + 1))
    return false;
  const auto *Bytes = Image.readVA(Range.Begin, Range.size());
  if (!Bytes)
    return false;
  for (const auto &[Begin, Code] : ProtocolCode)
    if (Begin != Range.Begin && Range.overlaps({Begin, Begin + Code.size()}))
      return false;
  for (const auto &[Address, Target] : Operands) {
    if (!Range.contains(Address))
      continue;
    if (Range.End - Address < 4 ||
        readLE<uint32_t>(Bytes + (Address - Range.Begin)) != Target ||
        !registration_abi::chargeCalleeWork(Work, ProtocolOperands.size() + 1))
      return false;
    const auto [It, New] = ProtocolOperands.emplace(Address, Target);
    if (!New && It->second != Target)
      return false;
  }
  const std::vector<uint8_t> Copy(Bytes, Bytes + Range.size());
  const auto [It, New] = ProtocolCode.emplace(Range.Begin, Copy);
  return New || It->second == Copy;
}

bool OriginalImageFrameProof::recordCode(const LowFunc &Function,
                                         size_t &Work) {
  std::vector<ExceptionAddressRange> Ranges;
  if (!registration_abi::collectCalleeCodeRanges(Function, Image, Work, Ranges))
    return false;
  Decoder Dec;
  if (!Dec.init(Image))
    return false;
  for (const auto &Block : Function.Blocks)
    for (const auto &Boundary : Block.InstructionBoundaries) {
      if (!registration_abi::chargeCalleeWork(
              Work,
              Boundary.Size + Image.Segments.size() +
                  2 * std::bit_width(Image.DataAddressRelocOperands.size()) +
                  2 * std::bit_width(Image.CodeAddressRelocOperands.size()) +
                  1))
        return false;
      const auto *Bytes = Image.readVA(Boundary.Address, Boundary.Size);
      DecodedInsn Insn;
      if (!Bytes ||
          !Dec.decodeOne(Bytes, Boundary.Size, Boundary.Address, Insn) ||
          Insn.Size != Boundary.Size || !Insn.Raw || !Insn.Raw->detail)
        return false;
      const auto Record = [&](va_t Field, va_t Target) {
        if (Field < Boundary.Address ||
            Field - Boundary.Address > Boundary.Size ||
            Boundary.Size - (Field - Boundary.Address) < 4 ||
            readLE<uint32_t>(Bytes + Field - Boundary.Address) != Target ||
            !registration_abi::chargeCalleeWork(Work, CodeOperands.size() + 1))
          return false;
        const auto [It, New] = CodeOperands.emplace(Field, Target);
        return New || It->second == Target;
      };
      for (const auto &Operand : instructionRelocations(Image, Insn).Addresses)
        if (!Operand.PCRelativeFromInstructionEnd &&
            (Operand.Width != 4 ||
             !isExactAddressProvenance(Operand.Provenance) ||
             !Record(Operand.FieldVA, Operand.TargetVA)))
          return false;
      // A plain absolute memory operand has LowIR address provenance even
      // without a loader fixup. Retention must authenticate the encoded field
      // as well; image identity alone cannot establish ASLR behavior.
      const auto &X86 = Insn.Raw->detail->x86;
      for (unsigned I = 0; I != X86.op_count; ++I) {
        const auto &Operand = X86.operands[I];
        if (Operand.type == X86_OP_MEM && Operand.mem.base == X86_REG_INVALID &&
            Operand.mem.segment != X86_REG_FS &&
            Operand.mem.segment != X86_REG_GS && X86.encoding.disp_size == 4 &&
            Image.getSegmentFor(uint32_t(Operand.mem.disp)) &&
            !Record(Boundary.Address + X86.encoding.disp_offset,
                    uint32_t(Operand.mem.disp)))
          return false;
      }
    }
  for (const auto &Range : Ranges) {
    if (!Range.isValid() ||
        !registration_abi::chargeCalleeWork(Work, Range.size() + Code.size()))
      return false;
    const auto *Bytes = Image.readVA(Range.Begin, Range.size());
    if (!Bytes)
      return false;
    const std::vector<uint8_t> Copy(Bytes, Bytes + Range.size());
    const auto [It, New] = Code.emplace(Range.Begin, Copy);
    if (!New && It->second != Copy)
      return false;
  }
  return !Ranges.empty();
}

std::optional<FrameValue> OriginalImageFrameProof::readScalar(va_t Address,
                                                              uint16_t Width,
                                                              size_t &Work) {
  const auto *Read = read(Address, Width, Work);
  if (Read && Read->Value)
    return FrameValue::constant(*Read->Value);
  return std::nullopt;
}

namespace {
bool checkRetainedCode(const BinaryImage &Image,
                       const OriginalImageSEHInvocation &Proof, size_t &Work) {
  // Shared caller instructions also occur in the root CFG. Unite overlapping
  // bytes and operands so one bounded loader query checks each physical range.
  std::map<va_t, uint8_t> Bytes;
  std::map<va_t, va_t> Operands;
  for (const auto *Code : {&Proof.Code, &Proof.ProtocolCode})
    for (const auto &[Begin, Expected] : *Code)
      for (size_t I = 0; I != Expected.size(); ++I) {
        if (!registration_abi::chargeCalleeWork(
                Work, 2 * std::bit_width(Bytes.size()) + 1))
          return false;
        const auto [It, New] = Bytes.emplace(Begin + I, Expected[I]);
        if (!New && It->second != Expected[I])
          return false;
      }
  for (const auto *Fields : {&Proof.CodeOperands, &Proof.ProtocolOperands})
    for (const auto &[Address, Target] : *Fields) {
      if (!registration_abi::chargeCalleeWork(Work, Operands.size() + 1))
        return false;
      const auto [It, New] = Operands.emplace(Address, Target);
      if (!New && It->second != Target)
        return false;
    }
  size_t Covered = 0;
  for (auto It = Bytes.begin(); It != Bytes.end();) {
    const va_t Begin = It->first;
    std::vector<uint8_t> Expected;
    do {
      Expected.push_back(It->second);
      ++It;
    } while (It != Bytes.end() && It->first == Begin + Expected.size());
    if (!registration_abi::chargeCalleeWork(Work, Expected.size() +
                                                      Operands.size() + 1))
      return false;
    std::vector<va_t> Fields;
    for (const auto &[Address, Target] : Operands)
      if (Address >= Begin && Address - Begin < Expected.size())
        Fields.push_back(Address);
    const auto Current =
        readRelocatedPE32CodeBytes(Image, Begin, Expected.size(), Fields, Work,
                                   limits::kMaxRegistrationEHStateWork);
    if (!Current || *Current != Expected)
      return false;
    Covered += Fields.size();
  }
  return !Bytes.empty() && Covered == Operands.size();
}
} // namespace

std::optional<FrameValue>
OriginalImageFrameProof::evaluate(const LowOp &Op,
                                  const FrameTransfer &Transfer, size_t &Work) {
  if (!registration_abi::chargeCalleeWork(Work, 1) ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.MemoryOrdering != NdMemoryOrdering::None || !Op.Output.Size ||
      (!Op.Output.isReg() && !Op.Output.isTemp()))
    return std::nullopt;
  if (Op.Opcode == NdOp::INT_SUB && Op.NumInputs == 2 && Op.Output.Size == 4 &&
      Op.Inputs[0].Size == 4 && Op.Inputs[1].Size == 4) {
    const auto Left = Transfer.read(Op.Inputs[0]);
    const auto Right = Transfer.read(Op.Inputs[1]);
    if (!Left.ImageAddress || !Right.ImageAddress ||
        Left != FrameValue::imageAddress(*Left.ImageAddress) ||
        Right != FrameValue::imageAddress(*Right.ImageAddress) ||
        !hasMappedIdentity(*Left.ImageAddress, Work) ||
        !hasMappedIdentity(*Right.ImageAddress, Work) ||
        !registration_abi::chargeCalleeWork(Work, Addresses.size() + 2))
      return std::nullopt;
    Addresses.insert(*Left.ImageAddress);
    Addresses.insert(*Right.ImageAddress);
    return FrameValue::constant(*Left.ImageAddress - *Right.ImageAddress);
  }
  // The x86 lifter zero-extends a PE32 address into its canonical 64-bit
  // memory temporary. Its exact image identity still comes from the 32-bit
  // producer; an arbitrary wide register supplies no such address shape.
  if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1 &&
      (Op.Inputs[0].Size == 4 ||
       (Op.Inputs[0].isTemp() && Op.Inputs[0].Size == 8))) {
    const auto Address = Transfer.read(Op.Inputs[0]);
    if (Address.preferredAddress() &&
        !recordReadAddress(Address, Op.Output.Size, Work))
      return std::nullopt;
    if (Address.ImageAddress &&
        Address == FrameValue::imageAddress(*Address.ImageAddress))
      return readScalar(*Address.ImageAddress, Op.Output.Size, Work);
  }
  return std::nullopt;
}

std::optional<RetainedOriginalImageSEHReplay>
replayRetainedOriginalImageSEHCallee(const BinaryImage &Original,
                                     const BinaryImage &Candidate,
                                     const LowOp &Op, const FrameState &Parent,
                                     size_t &Work,
                                     const std::set<int32_t> *Initialized) {
  auto Replay = replayBoundOriginalImageSEHCallee(Original, Candidate, Op,
                                                  Parent, Work, Initialized);
  if (!Replay)
    return std::nullopt;
  // Removing both a fixup and its normalized immediate cannot shrink the
  // candidate's obligations, even if the weaker source query keeps the same
  // scalar result. The original instruction fields own the retained roles.
  if (!registration_abi::chargeCalleeWork(
          Work, Replay->Original.CodeOperands.size() +
                    Replay->Candidate.CodeOperands.size() + 1) ||
      Replay->Original.CodeOperands != Replay->Candidate.CodeOperands)
    return std::nullopt;
  for (const auto Pair : {std::make_pair(&Original, &Replay->Original),
                          std::make_pair(&Candidate, &Replay->Candidate)}) {
    const auto &[Image, Proof] = Pair;
    if (!registration_abi::chargeCalleeWork(
            Work, Proof->Reads.size() + Proof->ReadAddressIdentities.size() +
                      Proof->CookieReadAddressIdentities.size()))
      return std::nullopt;
    for (const auto &[Key, Read] : Proof->Reads) {
      const auto Identity = Proof->ReadAddressIdentities.find(Key);
      if (Identity == Proof->ReadAddressIdentities.end() || !Identity->second)
        return std::nullopt;
    }
    for (const auto &[Address, Exact] : Proof->CookieReadAddressIdentities)
      if (!Exact)
        return std::nullopt;
    if (!checkRetainedCode(*Image, *Proof, Work))
      return std::nullopt;
  }
  const auto *Before =
      Original.ExceptionMetadata.findFunction(Op.Inputs[0].Offset);
  const auto *After =
      Candidate.ExceptionMetadata.findFunction(Op.Inputs[0].Offset);
  if (!Before || !After)
    return std::nullopt;
  const auto BeforeRange =
      coff_loader::getX86RegistrationSEHScopeTableRange(*Before);
  const auto AfterRange =
      coff_loader::getX86RegistrationSEHScopeTableRange(*After);
  if (!BeforeRange || !AfterRange || BeforeRange->Begin != AfterRange->Begin ||
      BeforeRange->End != AfterRange->End)
    return std::nullopt;
  auto Scope =
      coff_loader::getCheckedX86SEHScopeTableBytes(Original, *Before, Work);
  const auto CandidateScope =
      coff_loader::getCheckedX86SEHScopeTableBytes(Candidate, *After, Work);
  if (!Scope || !CandidateScope ||
      !registration_abi::chargeCalleeWork(Work, Scope->size() + 1) ||
      *Scope != *CandidateScope)
    return std::nullopt;
  auto Runtime =
      coff_loader::getCheckedX86SEHRuntimeCode(Original, *Before, Work);
  const auto CandidateRuntime =
      coff_loader::getCheckedX86SEHRuntimeCode(Candidate, *After, Work);
  if (!Runtime || !CandidateRuntime ||
      !registration_abi::chargeCalleeWork(Work,
                                          Runtime->AbsoluteOperands.size() +
                                              Runtime->ImportModule.size() +
                                              Runtime->ImportName.size() + 1))
    return std::nullopt;
  for (const auto &[Begin, Bytes] : Runtime->Code)
    if (!registration_abi::chargeCalleeWork(Work, Bytes.size() + 1))
      return std::nullopt;
  if (*Runtime != *CandidateRuntime)
    return std::nullopt;
  if (Runtime->SecurityCookieVA &&
      (!registration_abi::hasSEHCallbackImageMemory(
           Original, Runtime->SecurityCookieVA, 4, false, Work) ||
       !registration_abi::hasSEHCallbackImageMemory(
           Candidate, Runtime->SecurityCookieVA, 4, false, Work)))
    return std::nullopt;
  return RetainedOriginalImageSEHReplay{std::move(*Replay), std::move(*Runtime),
                                        *BeforeRange, std::move(*Scope)};
}

} // namespace neverd::registration_state
