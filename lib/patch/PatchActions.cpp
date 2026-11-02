//===- PatchActions.cpp - Checked instruction patch planning --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit NOP, branch, call and immediate edits of current instructions.
//===----------------------------------------------------------------------===//

#include "neverd/patch/PatchActions.h"

#include "neverd/decode/Decoder.h"
#include "neverd/decode/NativeOperands.h"
#include "neverd/patch/Limits.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

#include <bit>

using namespace neverd;
using namespace neverd::patch;
namespace patchLimits = neverd::limits::patch;

static llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::make_error<llvm::StringError>(Message,
                                             llvm::inconvertibleErrorCode());
}

static llvm::Expected<std::vector<uint8_t>> nops(Arch A, InstructionMode Mode,
                                                 uint64_t Size) {
  std::vector<uint8_t> Unit;
  switch (A) {
  case Arch::X86:
  case Arch::X64:
    Unit = {0x90};
    break;
  case Arch::AArch64:
    Unit = {0x1f, 0x20, 0x03, 0xd5};
    break;
  case Arch::ARM:
    Unit = Mode == InstructionMode::Thumb
               ? std::vector<uint8_t>{0x00, 0xbf}
               : std::vector<uint8_t>{0x00, 0xf0, 0x20, 0xe3};
    break;
  default:
    return invalid("unsupported architecture for NOP patches");
  }
  if (Size % Unit.size())
    return invalid("range does not fit whole architecture NOP instructions");
  std::vector<uint8_t> Result;
  Result.reserve(Size);
  while (Result.size() < Size)
    Result.insert(Result.end(), Unit.begin(), Unit.end());
  return Result;
}

// Thumb IT predicates up to four following instructions. Changing instruction
// count within that window can predicate bytes outside the reviewed patch.
static bool mayBeInThumbIT(const BinaryImage &Img, const Segment &Seg,
                           va_t Address) {
  const auto Offset = Address - Seg.VA;
  Decoder Probe;
  if (!Probe.init(Img))
    return true;
  for (uint64_t Back = 2; Back <= std::min<uint64_t>(Offset, 2 + 4 * 4);
       Back += 2) {
    const uint16_t Word =
        llvm::support::endian::read16le(Seg.Data.data() + Offset - Back);
    if ((Word & 0xff00) != 0xbf00 || !(Word & 15))
      continue;
    unsigned Slots = 4 - std::countr_zero(unsigned(Word & 15));
    uint64_t At = Offset - Back + 2;
    while (Slots && At < Offset) {
      DecodedInsn Insn{};
      if (!Probe.selectMode(Img, Seg.VA + At) ||
          Probe.currentMode() != InstructionMode::Thumb ||
          !Probe.decodeOne(Seg.Data.data() + At, Seg.Data.size() - At,
                           Seg.VA + At, Insn))
        return true;
      At += Insn.Size;
      --Slots;
    }
    if (Slots && At == Offset)
      return true;
  }
  return false;
}

static llvm::json::Object instructionJSON(const DecodedInsn &I) {
  return llvm::json::Object{
      {"address", "0x" + llvm::utohexstr(I.Addr, true)},
      {"size", I.Size},
      {"text", std::string(I.Raw->mnemonic) + " " + I.Raw->op_str}};
}

llvm::json::Object PatchPlan::json() const {
  return llvm::json::Object{
      {"address", "0x" + llvm::utohexstr(Address, true)},
      {"file_off", "0x" + llvm::utohexstr(FileOffset, true)},
      {"size", After.size()},
      {"kind", patchKindName(Kind)},
      {"original", llvm::toHex(Before, true)},
      {"current", llvm::toHex(After, true)},
      {"hex", llvm::toHex(After, true)},
      {"covers", llvm::json::Array(Covers)},
      {"decoded", llvm::json::Array(Decoded)},
      {"covers_truncated", CoversTruncated},
      {"decoded_truncated", DecodedTruncated},
      {"warnings", llvm::json::Array(Warnings)}};
}

llvm::Expected<PatchPlan> patch::planPatch(const BinaryImage &Img,
                                           const InputFile &Input,
                                           const PatchOverlay &Overlay,
                                           const PatchRequest &R) {
  const auto *Seg = Img.getSegmentFor(R.Address);
  if (!Seg || R.Address - Seg->VA >= Seg->Data.size())
    return invalid("patch address has no image bytes");
  const auto Data =
      llvm::ArrayRef<uint8_t>(Seg->Data).drop_front(R.Address - Seg->VA);
  PatchPlan P;
  P.Address = R.Address;
  P.Kind = R.Kind;
  Decoder Dec;
  DecodedInsn I{};
  if (R.Kind == PatchKind::Bytes) {
    P.After = R.Bytes;
  } else {
    if (!Dec.init(Img) || !Dec.selectMode(Img, R.Address) ||
        !Dec.decodeOne(Data.data(), Data.size(), R.Address, I))
      return invalid("address does not decode in the image's instruction mode");
    const auto Mode = Dec.currentMode();
    if (Img.Arch == Arch::ARM && Mode == InstructionMode::Thumb &&
        mayBeInThumbIT(Img, *Seg, R.Address))
      return invalid(
          "structured edits inside a Thumb IT block require predication proof");
    const unsigned Alignment =
        Img.Arch == Arch::AArch64 ||
                (Img.Arch == Arch::ARM && Mode == InstructionMode::ARM)
            ? 4
        : Img.Arch == Arch::ARM ? 2
                                : 1;
    if (R.Address % Alignment)
      return invalid("instruction address is not aligned for its architecture");
    const size_t OriginalSize = I.Size;
    const auto Raw = Data.take_front(I.Size);
    const bool X86 = Img.Arch == Arch::X64 || Img.Arch == Arch::X86;
    const bool Call = [&] {
      for (unsigned G = 0; G < I.Raw->detail->groups_count; ++G)
        if (I.Raw->detail->groups[G] == CS_GRP_CALL)
          return true;
      return false;
    }();
    bool Branch = false;
    unsigned ConditionByte = 0, ConditionMask = 0;
    if (X86) {
      Branch = (Raw.size() == 2 && (Raw[0] & 0xf0) == 0x70) ||
               (Raw.size() == 6 && Raw[0] == 0x0f && (Raw[1] & 0xf0) == 0x80);
      ConditionByte = Raw.size() == 6 ? 1 : 0;
      ConditionMask = 1;
    } else if (Img.Arch == Arch::AArch64 && Raw.size() == 4) {
      const uint32_t W = llvm::support::endian::read32le(Raw.data());
      if ((W & 0xff000010) == 0x54000000 && (W & 15) < 14) {
        Branch = true;
        ConditionMask = 1;
      } else if ((W & 0x7e000000) == 0x34000000 ||
                 (W & 0x7e000000) == 0x36000000) {
        Branch = true;
        ConditionByte = 3;
        ConditionMask = 1;
      }
    } else if (Img.Arch == Arch::ARM && Mode == InstructionMode::ARM &&
               Raw.size() == 4) {
      Branch = (Raw[3] & 0x0f) == 0x0a && (Raw[3] >> 4) < 14;
      ConditionByte = 3;
      ConditionMask = 0x10;
    } else if (Img.Arch == Arch::ARM && Mode == InstructionMode::Thumb &&
               Raw.size() == 2) {
      Branch = (Raw[1] & 0xf0) == 0xd0 && (Raw[1] & 15) < 14;
      ConditionByte = 1;
      ConditionMask = 1;
    }
    switch (R.Kind) {
    case PatchKind::Assemble: {
      AssemblyRequest A;
      A.Address = R.Address;
      A.Mode = Mode;
      A.ATT = R.ATT;
      A.Text = R.Text;
      A.Symbols = R.Symbols;
      auto Bytes = assemble(Img.Arch, A);
      if (!Bytes)
        return Bytes.takeError();
      P.After = std::move(*Bytes);
      break;
    }
    case PatchKind::Nop:
    case PatchKind::FillNops:
    case PatchKind::SkipCall:
    case PatchKind::BranchNever: {
      if (R.Kind == PatchKind::SkipCall && !Call)
        return invalid("instruction is not a call");
      if (R.Kind == PatchKind::BranchNever && !Branch)
        return invalid("unsupported conditional branch encoding");
      const uint64_t Count = R.Kind == PatchKind::FillNops ? R.Size : I.Size;
      if (!Count || Count > patchLimits::kMaxApplyBytes)
        return invalid("invalid NOP fill size");
      auto Bytes = nops(Img.Arch, Mode, Count);
      if (!Bytes)
        return Bytes.takeError();
      P.After = std::move(*Bytes);
      break;
    }
    case PatchKind::BranchInvert:
    case PatchKind::BranchAlways: {
      if (!Branch)
        return invalid("unsupported conditional branch encoding");
      P.After.assign(Raw.begin(), Raw.end());
      if (R.Kind == PatchKind::BranchInvert) {
        P.After[ConditionByte] ^= ConditionMask;
      } else if (X86) {
        if (Raw.size() == 2)
          P.After[0] = 0xeb;
        else {
          // The new JMP ends one byte earlier; preserve the old absolute
          // target.
          const int64_t D =
              int64_t(
                  int32_t(llvm::support::endian::read32le(Raw.data() + 2))) +
              1;
          if (D > INT32_MAX)
            return invalid("unconditional target is out of range");
          P.After[0] = 0xe9;
          llvm::support::endian::write32le(P.After.data() + 1, uint32_t(D));
          P.After[5] = 0x90;
        }
      } else if (Img.Arch == Arch::AArch64) {
        const uint32_t W = llvm::support::endian::read32le(Raw.data());
        const bool TestBit = (W & 0x7e000000) == 0x36000000;
        const unsigned Bits = TestBit ? 14 : 19;
        int32_t D = int32_t((W >> 5) & ((1u << Bits) - 1));
        D = (D ^ (1 << (Bits - 1))) - (1 << (Bits - 1));
        llvm::support::endian::write32le(
            P.After.data(), 0x14000000 | (uint32_t(D) & 0x03ffffff));
      } else if (Mode == InstructionMode::ARM) {
        P.After[3] = 0xea;
      } else {
        const int32_t D = int8_t(Raw[0]);
        llvm::support::endian::write16le(P.After.data(),
                                         0xe000 | (uint32_t(D) & 0x7ff));
      }
      break;
    }
    case PatchKind::SkipCallValue: {
      if (!Call)
        return invalid("instruction is not a call");
      if (X86 && I.Size >= 5 && R.Value <= UINT32_MAX) {
        P.After = {0xb8, 0, 0, 0, 0};
        llvm::support::endian::write32le(P.After.data() + 1, R.Value);
      } else if (Img.Arch == Arch::AArch64 && R.Value <= UINT16_MAX) {
        P.After.resize(4);
        llvm::support::endian::write32le(P.After.data(),
                                         0xd2800000 | (uint32_t(R.Value) << 5));
      } else if (Img.Arch == Arch::ARM && Mode == InstructionMode::ARM &&
                 R.Value <= UINT8_MAX) {
        // Preserve predicated calls. BLX-immediate occupies condition 0xf
        // but is unconditional; its replacement must use AL, not MOV NV.
        const uint32_t Condition = Raw[3] >> 4;
        P.After.resize(4);
        llvm::support::endian::write32le(
            P.After.data(), ((Condition == 15 ? 14 : Condition) << 28) |
                                0x03a00000 | uint32_t(R.Value));
      } else if (Img.Arch == Arch::ARM && Mode == InstructionMode::Thumb &&
                 I.Size == 4 && R.Value <= UINT16_MAX) {
        AssemblyRequest A;
        A.Address = R.Address;
        A.Mode = Mode;
        A.Text = "movw r0, #" + std::to_string(R.Value);
        auto Bytes = assemble(Img.Arch, A);
        if (!Bytes)
          return Bytes.takeError();
        P.After = std::move(*Bytes);
      } else
        return invalid(
            "return value does not fit this call's instruction length");
      break;
    }
    case PatchKind::Constant: {
      const auto Operands = describeNativeOperands(Dec, I, Img.Arch, false);
      const auto &Detail = *I.Raw->detail;
      bool Control = false;
      for (unsigned G = 0; G < Detail.groups_count; ++G)
        Control |=
            Detail.groups[G] == CS_GRP_JUMP ||
            Detail.groups[G] == CS_GRP_CALL || Detail.groups[G] == CS_GRP_RET ||
            Detail.groups[G] == CS_GRP_INT || Detail.groups[G] == CS_GRP_IRET;
      if (!X86 || Call || Branch || Control || Operands.Immediates.size() != 1)
        return invalid("instruction has no supported unique immediate field");
      const auto &Encoding = I.Raw->detail->x86.encoding;
      const unsigned N = Encoding.imm_size, At = Encoding.imm_offset;
      if (!N || N > sizeof(uint64_t) || At + N > Raw.size())
        return invalid("constant does not fit the encoded immediate field");
      P.After.assign(Raw.begin(), Raw.end());
      for (unsigned B = 0; B < N; ++B)
        P.After[At + B] = R.Value >> (B * 8);
      DecodedInsn Check{};
      if (!Dec.decodeOne(P.After.data(), P.After.size(), R.Address, Check))
        return invalid("edited immediate does not decode");
      const auto Actual = describeNativeOperands(Dec, Check, Img.Arch, false);
      if (Actual.Immediates.size() != 1 ||
          Actual.Immediates[0].Value != R.Value)
        return invalid("requested value is changed by the immediate encoding's "
                       "sign extension");
      break;
    }
    default:
      return invalid("unsupported patch action");
    }
    if (P.After.size() < OriginalSize && R.FillNops) {
      auto Pad = nops(Img.Arch, Mode, OriginalSize - P.After.size());
      if (!Pad)
        return Pad.takeError();
      P.After.insert(P.After.end(), Pad->begin(), Pad->end());
    }
    if (P.After.size() > OriginalSize && !R.AllowOverlap &&
        R.Kind != PatchKind::FillNops)
      return invalid("encoding overlaps following instructions; enable "
                     "allow_overlap after reviewing the range");
    if (P.After.size() < OriginalSize)
      return invalid("shorter instruction requires fill_nops to preserve the "
                     "original boundary");
    uint64_t End = 0;
    unsigned Count = 0;
    unsigned Predicated = 0;
    while (End < P.After.size()) {
      if (++Count > patchLimits::kMaxFillInstructions || End >= Data.size() ||
          !Dec.selectMode(Img, R.Address + End) || Dec.currentMode() != Mode ||
          !Dec.decodeOne(Data.data() + End, Data.size() - End, R.Address + End,
                         I))
        return invalid(
            "patch range is not a bounded sequence of decoded instructions");
      if (Img.Arch == Arch::ARM && Mode == InstructionMode::Thumb) {
        if (I.Id == ARM_INS_IT) {
          if (Predicated)
            return invalid("patch range contains nested Thumb IT blocks");
          const auto Mask = Data[End] & 15;
          if (!Mask)
            return invalid("invalid Thumb IT predicate mask");
          Predicated = 4 - std::countr_zero(unsigned(Mask));
        } else if (Predicated) {
          --Predicated;
        }
      }
      if (P.Covers.size() < patchLimits::kMaxPreviewInstructions)
        P.Covers.push_back(instructionJSON(I));
      else
        P.CoversTruncated = true;
      End += I.Size;
    }
    if (Predicated)
      return invalid("patch would change Thumb predication outside its range; "
                     "include the complete IT block");
    if (End != P.After.size()) {
      if (R.Kind != PatchKind::Assemble || !R.FillNops)
        return invalid(
            "patch would leave a partial instruction at the end of the range");
      auto Pad = nops(Img.Arch, Mode, End - P.After.size());
      if (!Pad)
        return Pad.takeError();
      P.After.insert(P.After.end(), Pad->begin(), Pad->end());
    }
    if (End > OriginalSize)
      P.Warnings.push_back("This patch replaces additional instructions.");
  }
  if (P.After.empty() || P.After.size() > patchLimits::kMaxApplyBytes)
    return invalid("patch byte count is outside the apply limit");
  auto Target = Overlay.admit(Img, Input, R.Address, P.After.size());
  if (!Target)
    return Target.takeError();
  P.FileOffset = Target->FileOffset;
  P.Before.assign(Data.begin(), Data.begin() + P.After.size());
  if (Dec.init(Img) && Dec.selectMode(Img, R.Address)) {
    for (size_t At = 0; At < P.After.size();) {
      if (!Dec.selectMode(Img, R.Address + At) ||
          !Dec.decodeOne(P.After.data() + At, P.After.size() - At,
                         R.Address + At, I)) {
        if (R.Kind != PatchKind::Bytes)
          return invalid("patch bytes do not decode completely");
        P.Warnings.push_back(
            "Patched bytes do not form a complete instruction sequence.");
        break;
      }
      if (R.Kind != PatchKind::Bytes && Img.Arch == Arch::ARM &&
          Dec.currentMode() == InstructionMode::Thumb && I.Id == ARM_INS_IT)
        return invalid("assembling Thumb IT requires proof of the following "
                       "predicated instructions");
      if (P.Decoded.size() < patchLimits::kMaxPreviewInstructions)
        P.Decoded.push_back(instructionJSON(I));
      else
        P.DecodedTruncated = true;
      At += I.Size;
    }
  }
  if (P.CoversTruncated || P.DecodedTruncated)
    P.Warnings.push_back("Instruction preview is truncated; review the "
                         "complete byte range.");
  return P;
}
