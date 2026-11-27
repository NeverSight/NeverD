//===- MachOSigningAddress.cpp - Darwin signing PC use analysis -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Track signing-PC values through Darwin register bodies before publication.
//===----------------------------------------------------------------------===//

#include "MachOSigningAddress.h"

#include "MachOPageAddress.h"
#include "MachOSigningJoins.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/FunctionBody.h"
#include "neverd/support/AArch64Literal.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <utility>

namespace neverd {
namespace {
enum class SigningAddressUse { Local, Observable, Unknown };

constexpr aarch64_reg SigningScratchRegisters[] = {
#define MACHO_SIGNING_SCRATCH(Reg) AARCH64_REG_##Reg,
#include "MachORegisterResults.def"
};

std::optional<aarch64_reg> copiedAddress(const DecodedInsn &I,
                                         aarch64_reg From) {
  const auto &D = I.Raw->detail->aarch64;
  if ((I.Id == AARCH64_INS_MOV || I.Raw->alias_id == AARCH64_INS_ALIAS_MOV) &&
      D.op_count == 2 && D.operands[0].type == AARCH64_OP_REG &&
      D.operands[0].reg >= AARCH64_REG_X0 &&
      D.operands[0].reg <= AARCH64_REG_X28 &&
      D.operands[1].type == AARCH64_OP_REG && D.operands[1].reg == From)
    return D.operands[0].reg;
  return std::nullopt;
}

bool storesAddress(const DecodedInsn &I, aarch64_reg Reg) {
  const auto &D = I.Raw->detail->aarch64;
  const bool Pair = I.Id == AARCH64_INS_STP;
  if ((!Pair && I.Id != AARCH64_INS_STR && I.Id != AARCH64_INS_STUR) ||
      D.op_count != (Pair ? 3 : 2))
    return false;
  const auto Values = llvm::ArrayRef(D.operands, D.op_count).drop_back();
  if (D.operands[D.op_count - 1].type != AARCH64_OP_MEM ||
      !llvm::any_of(Values, [Reg](const auto &Op) {
        return Op.type == AARCH64_OP_REG && Op.reg == Reg;
      }))
    return false;
  const auto &Mem = D.operands[D.op_count - 1].mem;
  const auto Offset = mapCapstoneReg(Reg).Offset;
  return (Mem.base == AARCH64_REG_INVALID ||
          mapCapstoneReg(Mem.base).Offset != Offset) &&
         (Mem.index == AARCH64_REG_INVALID ||
          mapCapstoneReg(Mem.index).Offset != Offset);
}

// A copied address can have several live aliases. Follow each until its W/X
// register is overwritten, checking both successors of a conditional branch.
SigningAddressUse signingAddressUse(Decoder &Decode,
                                    const MachOLiftedSource &Source, va_t Start,
                                    va_t After,
                                    llvm::ArrayRef<aarch64_reg> Registers,
                                    bool Returns, bool Observed = false) {
  const auto Scratch = [](aarch64_reg R) {
    return llvm::is_contained(SigningScratchRegisters, R);
  };
  if (Returns)
    return !Observed && llvm::all_of(Registers, Scratch)
               ? SigningAddressUse::Local
               : SigningAddressUse::Observable;
  constexpr auto Word = aarch64::kInsnSize;
  using Use = std::pair<va_t, unsigned>;
  llvm::SmallVector<Use> Work;
  for (auto Reg : Registers)
    Work.emplace_back(After, Reg);
  llvm::SmallDenseSet<Use, 8> Visited;
  while (!Work.empty()) {
    const auto [PC, Number] = Work.pop_back_val();
    if (!Visited.insert({PC, Number}).second)
      continue;
    const auto Reg = static_cast<aarch64_reg>(Number);
    if (PC < Start || !rangeInBounds(PC - Start, Word, Source.Bytes.size()) ||
        !machODataInCodeRanges(Source.Embedded, PC, Word).empty())
      return SigningAddressUse::Unknown;
    DecodedInsn I{};
    if (Decode.decodeOne(Source.Bytes.data() + (PC - Start), Word, PC, I) !=
            int(Word) ||
        !I.Raw || !I.Raw->detail)
      return SigningAddressUse::Unknown;
    cs_regs Reads, Writes;
    uint8_t NR = 0, NW = 0;
    const auto Handle = Decode.getHandle();
    if (cs_regs_access(Handle, I.Raw, Reads, &NR, Writes, &NW) != CS_ERR_OK)
      return SigningAddressUse::Unknown;
    const auto Offset = mapCapstoneReg(Reg).Offset;
    const auto Matches = [&](uint16_t Other) {
      const auto R = mapCapstoneReg(static_cast<aarch64_reg>(Other));
      return R.Size && R.Offset == Offset;
    };
    const auto &D = I.Raw->detail->aarch64;
    if (llvm::any_of(llvm::ArrayRef(Reads, NR), Matches)) {
      if (auto Copy = copiedAddress(I, Reg)) {
        Work.emplace_back(PC + Word, *Copy);
      } else if (storesAddress(I, Reg)) {
        Observed = true;
      } else {
        return SigningAddressUse::Unknown;
      }
    }
    if (llvm::any_of(llvm::ArrayRef(Writes, NW), Matches) ||
        I.Id == AARCH64_INS_BRK)
      continue;
    if (cs_insn_group(Handle, I.Raw, CS_GRP_RET) ||
        isAArch64PAuthLRReturnWord(readLE<uint32_t>(I.Raw->bytes))) {
      Observed |= !Scratch(Reg);
      continue;
    }
    if (cs_insn_group(Handle, I.Raw, CS_GRP_CALL)) {
      if (!Scratch(Reg))
        return SigningAddressUse::Unknown;
    } else if (cs_insn_group(Handle, I.Raw, CS_GRP_JUMP)) {
      if (!D.op_count || D.operands[D.op_count - 1].type != AARCH64_OP_IMM)
        return SigningAddressUse::Unknown;
      const va_t Target = D.operands[D.op_count - 1].imm;
      if (Target >= Start && Target - Start < Source.Bytes.size())
        Work.emplace_back(Target, Reg);
      else if (!Scratch(Reg))
        return SigningAddressUse::Unknown;
      if (I.Id == AARCH64_INS_B &&
          (D.cc == AArch64CC_Invalid || D.cc == AArch64CC_AL))
        continue;
    }
    Work.emplace_back(PC + Word, Reg);
  }
  return Observed ? SigningAddressUse::Observable : SigningAddressUse::Local;
}

} // namespace

llvm::ArrayRef<aarch64_reg> machOSigningScratchRegisters() {
  return SigningScratchRegisters;
}

bool machODeadSigningScratch(Decoder &Decode, const MachOLiftedSource &Source,
                             va_t Start, va_t At, aarch64_reg Reg) {
  return signingAddressUse(Decode, Source, Start, At, Reg, false) ==
         SigningAddressUse::Local;
}

// Use the ISA's PC-relative encoding of the same operation. It uses the same
// key, LR and SP and leaves the caller's numeric register untouched.
std::optional<uint32_t> machOPCRelativeAuthentication(Intrinsic Op, va_t PC,
                                                      va_t Sign) {
  if (Sign > PC || (PC - Sign) % kAArch64PAuthLRScale)
    return std::nullopt;
  const auto Distance = (PC - Sign) / kAArch64PAuthLRScale;
  if (Distance > kAArch64PAuthLRImmMask)
    return std::nullopt;
  enum class Modifier { X15, PC, PCRel, Rn, Rm };
#define NEVERD_A64_PAUTH_LR_RETURN(Mask, Encoding, Id, Mod2)                   \
  if (Modifier::Mod2 == Modifier::PCRel && Op == Intrinsic::Id)                \
    return Encoding | (uint32_t(Distance) << kAArch64PAuthLRImmShift);
#define NEVERD_A64_PAUTH_LR_INSTRUCTION(Mask, Encoding, Id, Pointer, Mod,      \
                                        Mod2)                                  \
  NEVERD_A64_PAUTH_LR_RETURN(Mask, Encoding, Id, Mod2)
#include "neverd/ir/intrinsics/AArch64/AArch64PAuthLR.def"
  return std::nullopt;
}

bool machORestoresSigningAddress(Decoder &Decode, llvm::ArrayRef<uint8_t> Bytes,
                                 va_t PC, unsigned Register, va_t Target) {
  const auto Pair = decodeMachOPageAddress(Decode, Bytes, PC);
  const auto R = AARCH64_REG_X0 + Register;
  return Pair && Pair->PageRegister == R && Pair->ResultRegister == R &&
         Pair->Target == Target;
}

MachOSigningAddress machOSigningAddress(Decoder &Decode,
                                        const MachOLiftedSource &Source,
                                        va_t Start, va_t PC, va_t Target,
                                        aarch64_reg Reg,
                                        llvm::ArrayRef<va_t> Consumers) {
  constexpr auto Word = aarch64::kInsnSize;
  if (!rangeInBounds(PC - Start, 2 * Word, Source.Bytes.size()) ||
      !machODataInCodeRanges(Source.Embedded, PC + Word, Word).empty())
    return {};
  const auto Bytes = [&](va_t VA) {
    return Source.Bytes.data() + (VA - Start);
  };
  const auto At = [&](va_t VA) { return readLE<uint32_t>(Bytes(VA)); };
  const bool HasSigner =
      Target >= Start && !(Target % Word) &&
      rangeInBounds(Target - Start, Word, Source.Bytes.size()) &&
      machODataInCodeRanges(Source.Embedded, Target, Word).empty();
  if (At(PC + Word) != kAArch64PACMEncoding) {
    llvm::SmallVector<aarch64_reg, 2> Aliases{Reg};
    bool Stored = false;
    DecodedInsn I{};
    va_t AuthPC = PC + Word;
    std::optional<std::pair<aarch64_reg, va_t>> SeparateModifier;
    // Ordinary stores and copies may observe the address before AUT. Walk
    // only their straight-line sequence and preserve every live GPR alias.
    for (;; AuthPC += Word) {
      if (!rangeInBounds(AuthPC - Start, Word, Source.Bytes.size()) ||
          !machODataInCodeRanges(Source.Embedded, AuthPC, Word).empty() ||
          Decode.decodeOne(Bytes(AuthPC), Word, AuthPC, I) != int(Word) ||
          !I.Raw || !I.Raw->detail)
        return {};
      const auto &D = I.Raw->detail->aarch64;
      if (SeparateModifier || D.op_count < 2 || I.Raw->detail->writeback)
        break;
      // A previous publication can set a separate scratch register for a
      // forward or distant signer. Its own ADR remains local on the next lift.
      if (I.Id == AARCH64_INS_ADR && D.operands[0].type == AARCH64_OP_REG &&
          D.operands[1].type == AARCH64_OP_IMM) {
        SeparateModifier =
            std::pair(D.operands[0].reg, va_t(D.operands[1].imm));
        continue;
      }
      if (D.operands[1].type == AARCH64_OP_REG &&
          llvm::is_contained(Aliases, D.operands[1].reg)) {
        if (const auto Copy = copiedAddress(I, D.operands[1].reg)) {
          if (!llvm::is_contained(Aliases, *Copy))
            Aliases.push_back(*Copy);
          continue;
        }
      }
      if (llvm::any_of(Aliases, [&](auto R) { return storesAddress(I, R); })) {
        Stored = true;
        continue;
      }
      break;
    }
    const auto Auth = machOPCSigningInstruction(I.Id);
    const auto &D = I.Raw->detail->aarch64;
    if (!Auth || !Auth->Authenticate || D.op_count != 1 ||
        (!Consumers.empty() && !llvm::binary_search(Consumers, AuthPC)))
      return {};
    const bool Immediate = D.operands[0].type == AARCH64_OP_IMM;
    const auto AuthRegister =
        Immediate ? AARCH64_REG_INVALID : D.operands[0].reg;
    const bool Independent = Immediate || SeparateModifier.has_value();
    va_t SignedAt = Target;
    if (SeparateModifier) {
      if (D.operands[0].type != AARCH64_OP_REG ||
          D.operands[0].reg != SeparateModifier->first)
        return {};
      SignedAt = SeparateModifier->second;
      if (SignedAt < Start || SignedAt % Word ||
          !rangeInBounds(SignedAt - Start, Word, Source.Bytes.size()) ||
          !machODataInCodeRanges(Source.Embedded, SignedAt, Word).empty())
        return {};
    } else if (!Immediate && (D.operands[0].type != AARCH64_OP_REG ||
                              !llvm::is_contained(Aliases, D.operands[0].reg) ||
                              !HasSigner)) {
      return {};
    }
    if (Immediate) {
      const auto Distance =
          ((At(AuthPC) >> kAArch64PAuthLRImmShift) & kAArch64PAuthLRImmMask) *
          kAArch64PAuthLRScale;
      if (Distance > AuthPC - Start)
        return {};
      SignedAt = AuthPC - Distance;
      if (!machODataInCodeRanges(Source.Embedded, SignedAt, Word).empty())
        return {};
    }
    if (Decode.decodeOne(Bytes(SignedAt), Word, SignedAt, I) != int(Word))
      return {};
    const auto Sign = machOPCSigningInstruction(I.Id);
    if (!Sign || Sign->Authenticate || Auth->Key != Sign->Key)
      return {};
    if (SeparateModifier &&
        llvm::is_contained(Aliases, SeparateModifier->first)) {
      const auto Restore = AuthPC + Word;
      if (!Auth->Return && AuthRegister >= AARCH64_REG_X0 &&
          AuthRegister <= AARCH64_REG_X28 &&
          rangeInBounds(Restore - Start, 2 * Word, Source.Bytes.size()) &&
          machODataInCodeRanges(Source.Embedded, Restore, 2 * Word).empty() &&
          machORestoresSigningAddress(
              Decode, Source.Bytes.drop_front(Restore - Start), Restore,
              AuthRegister - AARCH64_REG_X0, Target))
        return {.Use = MachOSigningAddress::Observable,
                .InstalledSigner = At(SignedAt),
                .RestorePC = Restore};
      // The independent ADR overwrote this copy. Only the surviving aliases
      // or an earlier store can still expose the original address.
      llvm::erase(Aliases, AuthRegister);
    }
    const auto Use = signingAddressUse(Decode, Source, Start, AuthPC + Word,
                                       Aliases, Auth->Return, Stored);
    if (Use == SigningAddressUse::Local && !Independent)
      return {.Use = MachOSigningAddress::Local};
    if (Use == SigningAddressUse::Observable) {
      if (Independent)
        return {.Use = MachOSigningAddress::Observable,
                .InstalledSigner = At(SignedAt)};
      if (auto Relative =
              machOPCRelativeAuthentication(Auth->Id, AuthPC, Target))
        return {.Use = MachOSigningAddress::Observable,
                .Authentication = MachOAuthenticationReplacement{
                    {AuthPC, std::nullopt}, *Relative, Target}};
      // The immediate counts backwards. Use a proven-dead Darwin scratch
      // register when the signer follows AUT or lies beyond its unsigned
      // immediate.
      for (auto Scratch : SigningScratchRegisters) {
        const unsigned Number = Scratch - AARCH64_REG_X0;
        if (!aarch64::encodePCRelativeAddress(aarch64::AddressInstruction::ADR,
                                              Number, AuthPC, Target) ||
            signingAddressUse(Decode, Source, Start, AuthPC + Word, Scratch,
                              Auth->Return) != SigningAddressUse::Local)
          continue;
        const unsigned Shift = Auth->Return ? 0 : kAArch64PAuthLRImmShift;
        const uint32_t Encoding =
            (At(AuthPC) & ~(kAArch64PAuthLRRegMask << Shift)) |
            (Number << Shift);
        return {.Use = MachOSigningAddress::Observable,
                .Authentication = MachOAuthenticationReplacement{
                    {AuthPC, Number}, Encoding, Target}};
      }
      // The input value has an exact source relocation. AUT falls through
      // without changing this GPR, so it can be reconstructed immediately
      // afterwards without borrowing another register or changing SP.
      if (!Auth->Return && AuthRegister >= AARCH64_REG_X0 &&
          AuthRegister <= AARCH64_REG_X28 &&
          aarch64::encodePCRelativeAddress(aarch64::AddressInstruction::ADR,
                                           AuthRegister - AARCH64_REG_X0,
                                           AuthPC, Target))
        return {.Use = MachOSigningAddress::Observable,
                .Authentication = MachOAuthenticationReplacement{
                    {AuthPC, unsigned(AuthRegister - AARCH64_REG_X0), true},
                    At(AuthPC),
                    Target}};
    }
    return {};
  }
  if (!HasSigner || Reg != AARCH64_REG_X16 || Target == Start ||
      Source.Bytes.size() - (PC - Start) < 3 * Word ||
      !machODataInCodeRanges(Source.Embedded, Target - Word, Word).empty() ||
      !machODataInCodeRanges(Source.Embedded, PC + 2 * Word, Word).empty() ||
      At(Target - Word) != kAArch64PACMEncoding)
    return {};
  enum class Modifier { PC, X15, X16 };
  const auto Operation = [](uint32_t Bits, Modifier Expected) {
    switch (Bits) {
#define NEVERD_A64_PAUTH_M_CONSUMER(Encoding, Id, Mod2)                        \
  case Encoding:                                                               \
    if (Expected == Modifier::Mod2)                                            \
      return aarch64PAuthMOperation(Intrinsic::Id);                            \
    break;
#include "neverd/ir/intrinsics/AArch64/AArch64PAuthM.def"
    }
    return std::optional<AArch64PAuthLROperation>{};
  };
  const auto Sign = Operation(At(Target), Modifier::PC);
  const auto Auth = Operation(At(PC + 2 * Word), Modifier::X16);
  if (Sign && Auth && Sign->Key == Auth->Key &&
      signingAddressUse(Decode, Source, Start, PC + 3 * Word, Reg,
                        Auth->Return) == SigningAddressUse::Local)
    return {.Use = MachOSigningAddress::Local};
  return {};
}

bool needsMachONativeRegisterBody(const MedFunc &Function,
                                  const BinaryImage &Image,
                                  const LowFunc *CFG) {
  constexpr auto Word = aarch64::kInsnSize;
  llvm::SmallVector<va_t, 4> Consumers;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      if (aarch64AddressMemoryMedShapeIsValid(Op)) {
        const auto I = *aarch64AddressMemoryInstruction(
            Op.Inputs[kAddressMemoryWordOperand + 1].ConstVal);
        if (I.stackBase() && I.hasRegisterIndex())
          return true;
      }
      if (Op.Addr > Function.Entry && aarch64PAuthMedShapeIsValid(Op)) {
        const auto LR = aarch64PAuthLROperation(
            static_cast<Intrinsic>(Op.Inputs.front().ConstVal));
        if (LR && LR->Authenticate)
          Consumers.push_back(Op.Addr);
      }
    }
  if (Consumers.empty())
    return false;
  llvm::sort(Consumers);
  const auto *Frame = Image.ExceptionMetadata.findFunction(Function.Entry);
  if (!Frame)
    return false;
  const auto Source =
      machOLiftedSource(Image, Function.Entry, Frame->CodeRange.End);
  Decoder Decode;
  if (!Source || Source->ForwardTarget || !Decode.init(Image))
    return false;
  // Compact rows can span adjacent functions. Stop scanning at this
  // function's last authentication and verify the matched consumer below.
  const auto End = std::min<uint64_t>(Source->Bytes.size(),
                                      Consumers.back() - Function.Entry);
  for (size_t Offset = 0; Offset < End; Offset += Word) {
    const auto Bytes = Source->Bytes.drop_front(Offset);
    const auto Encoding = readLE<uint32_t>(Bytes.data());
    va_t PC = Function.Entry + Offset;
    auto Target = aarch64::decodePCRelativeAddress(Encoding, PC);
    if (!Target && aarch64::isPointerLiteralLoad(Encoding))
      Target = machORebasedLiteralAddress(Image, Encoding, PC);
    if (!Target || !machODataInCodeRanges(Source->Embedded, PC, Word).empty())
      continue;
    aarch64_reg Reg;
    if ((Encoding & aarch64::kADRP_Mask) == aarch64::kADRP_Op) {
      const auto Pair = decodeMachOPageAddress(Decode, Bytes, PC);
      if (!Pair)
        continue;
      PC += Word;
      Reg = Pair->ResultRegister;
      Target = Pair->Target;
    } else {
      DecodedInsn Producer{};
      if (Decode.decodeOne(Bytes.data(), Word, PC, Producer) != int(Word) ||
          !Producer.Raw || !Producer.Raw->detail)
        continue;
      Reg = Producer.Raw->detail->aarch64.operands[0].reg;
    }
    if (machOSigningAddress(Decode, *Source, Function.Entry, PC, *Target, Reg,
                            Consumers)
            .Use == MachOSigningAddress::Observable)
      return true;
  }
  if (CFG) {
    const auto Joins = machOSigningJoins(Image, Decode, *Source, *CFG);
    return Joins.Unresolved || !Joins.Addresses.empty() ||
           !Joins.RetainedSigningWords.empty();
  }
  return false;
}

} // namespace neverd
