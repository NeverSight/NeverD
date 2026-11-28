//===- MachOSigningJoins.cpp - Joined Darwin signing addresses ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MachOSigningJoins.h"

#include "MachOPageAddress.h"

#include "neverd/analysis/AArch64UnwindRecovery.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/FunctionBody.h"
#include "neverd/support/AArch64Literal.h"

#include "llvm/ADT/STLExtras.h"

#include <iterator>
#include <optional>
#include <tuple>

namespace neverd {
namespace {
using namespace aarch64_unwind;
constexpr auto Word = aarch64::kInsnSize;
#define MACHO_SIGNING_ISLAND_LAYOUT(First, Return, Fallthrough)                \
  constexpr unsigned FirstIslandWord = First, ReturnIslandWords = Return,      \
                     FallthroughIslandWords = Fallthrough;
#include "MachORegisterResults.def"

std::optional<unsigned> gpr(aarch64_reg R) {
  if (R >= AARCH64_REG_X0 && R <= AARCH64_REG_X28)
    return R - AARCH64_REG_X0;
  if (R >= AARCH64_REG_W0 && R <= AARCH64_REG_W30)
    return R - AARCH64_REG_W0;
  if (R == AARCH64_REG_FP)
    return kFP;
  if (R == AARCH64_REG_LR)
    return kLR;
  return std::nullopt;
}

struct Instruction {
  uint32_t Reads = 0, Writes = 0;
  std::optional<unsigned> Definition, Copy;
  std::optional<va_t> Address;
  va_t Producer = InvalidVA;
  std::optional<AArch64PAuthLROperation> Signing;
  std::optional<unsigned> Modifier;
  std::optional<va_t> BranchTarget;
  bool Call = false;
};

using Origins = llvm::SmallVector<std::pair<va_t, va_t>, 2>;

struct Proof {
  Origins Definitions;
  llvm::SmallDenseMap<int, va_t, 2> IncomingSigners{};
  uint32_t WritesToConsumer = 0;
  bool CrossesEdge = false;
  bool Joined = false;
  bool Unknown = false;
  bool Mismatch = false;
};

// Walk the two reaching definitions together. A join with two different
// signers must not combine the LR from one predecessor with the GPR of another.
Proof reachingSigners(const LowFunc &CFG,
                      const llvm::DenseMap<va_t, Instruction> &Code,
                      const LowBlock &Consumer, va_t Auth, unsigned Modifier,
                      unsigned Key) {
  using Cursor = std::tuple<int, va_t, unsigned, va_t, va_t, va_t, int>;
  llvm::SmallVector<Cursor, 4> Work;
  llvm::SmallDenseSet<Cursor, 8> Seen;
  Work.emplace_back(Consumer.Id, Auth, Modifier, InvalidVA, InvalidVA,
                    InvalidVA, -1);
  Proof Result;
  size_t Steps = 0;
  while (!Work.empty()) {
    const auto State = Work.pop_back_val();
    if (!Seen.insert(State).second)
      continue;
    auto [Id, End, R, Origin, Target, Signer, Edge] = State;
    const auto &B = CFG.Blocks[Id];
    bool Done = false;
    for (va_t PC = End; PC > B.StartAddr;) {
      PC -= Word;
      const auto It = Code.find(PC);
      if (++Steps > kMaxTransitions || It == Code.end())
        return {.CrossesEdge = true, .Joined = true, .Unknown = true};
      const auto &I = It->second;
      if (Origin == InvalidVA) {
        Result.WritesToConsumer |= I.Writes;
        if (I.Definition == R && I.Address && I.Producer >= B.StartAddr) {
          Origin = I.Producer;
          Target = *I.Address;
          // A page pair can write a different register at its first word.
          Result.WritesToConsumer |= Code.at(Origin).Writes;
        } else if (I.Definition == R && I.Copy) {
          R = *I.Copy;
        } else if (I.Writes & (uint32_t(1) << R)) {
          Result.Unknown = true;
          Done = true;
          break;
        }
      }
      if (Signer == InvalidVA) {
        if (I.Signing && !I.Signing->Authenticate && I.Signing->Key == Key) {
          Signer = PC;
        } else if (I.Signing || I.Call || (I.Writes & (uint32_t(1) << kLR))) {
          Result.Unknown = true;
          Done = true;
          break;
        }
      }
      if (Origin != InvalidVA && Signer != InvalidVA) {
        if (Target != Signer)
          Result.Mismatch = true;
        else if (!llvm::is_contained(Result.Definitions,
                                     std::pair(Origin, Target)))
          Result.Definitions.emplace_back(Origin, Target);
        if (Edge >= 0) {
          auto [It, Inserted] =
              Result.IncomingSigners.try_emplace(Edge, Signer);
          if (!Inserted && It->second != Signer)
            It->second = InvalidVA;
        }
        Done = true;
        break;
      }
    }
    if (Done)
      continue;
    if (Origin == InvalidVA)
      Result.CrossesEdge = true;
    const bool Root = !B.ExceptionalPreds.empty() ||
                      CFG.ModuleAnalysisRoots.contains(B.StartAddr);
    Result.Joined |= B.Preds.size() > 1 || (Root && !B.Preds.empty());
    if (B.Preds.empty() || Root)
      Result.Unknown = true;
    for (auto Pred : B.Preds)
      Work.emplace_back(Pred, CFG.Blocks[Pred].EndAddr, R, Origin, Target,
                        Signer, Edge < 0 ? Pred : Edge);
  }
  return Result;
}

// A direct edge with one reaching signer can enter its own PC-relative AUT
// or authenticated return. No register, flags or stack slot carries the
// selection across the join.
std::optional<MachOSigningConsumer>
routeSigningEdges(const LowFunc &CFG,
                  const llvm::DenseMap<va_t, Instruction> &Code,
                  const Proof &Paths, const LowBlock &Join, va_t PC,
                  AArch64PAuthLROperation Operation) {
  if (Join.Preds.empty())
    return std::nullopt;
  MachOSigningConsumer Auth{PC, std::nullopt};
  Auth.Returns = Operation.Return;
  Auth.PrefixWords = (PC - Join.StartAddr) / Word;
  std::optional<va_t> Fallthrough;
  for (auto Id : Join.Preds) {
    const auto &Pred = CFG.Blocks[Id];
    auto Edge = Paths.IncomingSigners.find(Id);
    if (Edge == Paths.IncomingSigners.end() || Edge->second == InvalidVA)
      return std::nullopt;
    const va_t Signer = Edge->second;
    auto It = llvm::find_if(
        Auth.Routes, [Signer](const auto &R) { return R.Signer == Signer; });
    if (It == Auth.Routes.end()) {
      Auth.Routes.push_back({Signer, {}});
      It = std::prev(Auth.Routes.end());
    }
    const auto Last = Pred.EndAddr - Word;
    const auto Transfer = Code.find(Last);
    if (Transfer == Code.end())
      return std::nullopt;
    if (Transfer->second.BranchTarget == Join.StartAddr)
      It->Branches.push_back(Last);
    else if (Pred.EndAddr != Join.StartAddr)
      return std::nullopt;
    if (Pred.EndAddr == Join.StartAddr)
      Fallthrough = Signer;
  }
  llvm::sort(Auth.Routes, [&](const auto &L, const auto &R) {
    return std::pair(Fallthrough != L.Signer, L.Signer) <
           std::pair(Fallthrough != R.Signer, R.Signer);
  });
  return Auth;
}

} // namespace

MachOSigningJoins machOSigningJoins(const BinaryImage &Image, Decoder &Decode,
                                    const MachOLiftedSource &Source,
                                    const LowFunc &CFG) {
  MachOSigningJoins Result;
  const auto Count = Source.Bytes.size() / Word;
  if (CFG.Blocks.size() < 2 || !Count)
    return Result;
  llvm::SmallVector<std::pair<const LowBlock *, va_t>> Consumers;
  // Most bodies contain no explicit register authenticator. Inspect their
  // words before allocating register facts or decoding other instructions.
  for (const auto &B : CFG.Blocks)
    for (const auto &Boundary : B.InstructionBoundaries) {
      const auto PC = Boundary.Address;
      if (PC < CFG.Entry ||
          !rangeInBounds(PC - CFG.Entry, Word, Source.Bytes.size()))
        continue;
      const auto Bits =
          readLE<uint32_t>(Source.Bytes.data() + (PC - CFG.Entry));
      const auto Form = machOPCAuthenticationForm(Bits);
      if (Form == MachOAuthenticationForm::Register) {
        Consumers.emplace_back(&B, PC);
      } else if (Form == MachOAuthenticationForm::PCRelative) {
        // On a second lift the common authenticator already carries its PC.
        // An old numeric producer can precede the join on a different arm.
        const auto Distance =
            ((Bits >> kAArch64PAuthLRImmShift) & kAArch64PAuthLRImmMask) *
            kAArch64PAuthLRScale;
        if (Distance > PC - CFG.Entry)
          continue;
        const auto SignPC = PC - Distance;
        if (!machODataInCodeRanges(Source.Embedded, SignPC, Word).empty())
          continue;
        auto Operation = [&](va_t At) {
          DecodedInsn I{};
          return Decode.decodeOne(Source.Bytes.data() + (At - CFG.Entry), Word,
                                  At, I) == int(Word)
                     ? machOPCSigningInstruction(I.Id)
                     : std::nullopt;
        };
        const auto Sign = Operation(SignPC), Auth = Operation(PC);
        if (Sign && !Sign->Authenticate && Auth && Auth->Key == Sign->Key)
          Result.RetainedSigningWords.insert(
              readLE<uint32_t>(Source.Bytes.data() + (SignPC - CFG.Entry)));
      }
    }
  if (Consumers.empty())
    return Result;
  llvm::DenseMap<va_t, Instruction> Code;
  uint32_t Read = 0, ReadTwice = 0, Written = 0, Reserved = 0;
  for (const auto &B : CFG.Blocks)
    for (const auto &Boundary : B.InstructionBoundaries) {
      const auto PC = Boundary.Address;
      if (PC < CFG.Entry || (PC - CFG.Entry) / Word >= Count)
        continue;
      const auto Bytes = Source.Bytes.drop_front(PC - CFG.Entry);
      DecodedInsn I{};
      if (Decode.decodeOne(Bytes.data(), Word, PC, I) != int(Word) || !I.Raw ||
          !I.Raw->detail)
        return {.Unresolved = true};
      cs_regs Reads, Writes;
      uint8_t NR = 0, NW = 0;
      if (cs_regs_access(Decode.getHandle(), I.Raw, Reads, &NR, Writes, &NW) !=
          CS_ERR_OK)
        return {.Unresolved = true};
      auto &Fact = Code[PC];
      const auto Mask = [](llvm::ArrayRef<uint16_t> Regs) {
        uint32_t Bits = 0;
        for (auto Physical : Regs)
          if (auto R = gpr(static_cast<aarch64_reg>(Physical)))
            Bits |= uint32_t(1) << *R;
        return Bits;
      };
      Fact.Reads = Mask({Reads, NR});
      Fact.Writes = Mask({Writes, NW});
      Fact.Call = cs_insn_group(Decode.getHandle(), I.Raw, CS_GRP_CALL);
      if (Fact.Call)
        Fact.Writes |= (uint32_t(1) << kVolatileEnd) - 1;
      ReadTwice |= Read & Fact.Reads;
      Read |= Fact.Reads;
      Written |= Fact.Writes;
      Fact.Signing = machOPCSigningInstruction(I.Id);
      const auto &D = I.Raw->detail->aarch64;
      const auto Ops = llvm::ArrayRef(D.operands, D.op_count);
      if ((I.Id == AARCH64_INS_B || I.Id == AARCH64_INS_CBZ ||
           I.Id == AARCH64_INS_CBNZ || I.Id == AARCH64_INS_TBZ ||
           I.Id == AARCH64_INS_TBNZ) &&
          !Ops.empty() && Ops.back().type == AARCH64_OP_IMM)
        Fact.BranchTarget = va_t(Ops.back().imm);
      if (Ops.empty() || Ops.front().type != AARCH64_OP_REG)
        continue;
      const auto RD = Ops.front().reg;
      if (RD < AARCH64_REG_X0 || RD > AARCH64_REG_X28)
        continue;
      Fact.Definition = gpr(RD);
      if (Fact.Signing && Fact.Signing->Authenticate) {
        Fact.Modifier = Fact.Definition;
      } else if (I.Id == AARCH64_INS_ADR && Ops.size() == 2) {
        Fact.Address = va_t(Ops[1].imm);
        Fact.Producer = PC;
      } else if (aarch64::isPointerLiteralLoad(
                     readLE<uint32_t>(Bytes.data()))) {
        Fact.Address = machORebasedLiteralAddress(
            Image, readLE<uint32_t>(Bytes.data()), PC);
        Fact.Producer = PC;
      } else if (I.Id == AARCH64_INS_ADD && PC >= B.StartAddr + Word &&
                 Code.contains(PC - Word)) {
        const auto Pair = decodeMachOPageAddress(
            Decode, Source.Bytes.drop_front(PC - Word - CFG.Entry), PC - Word);
        if (Pair) {
          Fact.Address = Pair->Target;
          Fact.Producer = PC - Word;
        }
      } else if ((I.Id == AARCH64_INS_MOV ||
                  I.Raw->alias_id == AARCH64_INS_ALIAS_MOV) &&
                 Ops.size() == 2 && Ops[1].type == AARCH64_OP_REG &&
                 Ops[1].reg >= AARCH64_REG_X0 &&
                 Ops[1].reg <= AARCH64_REG_X28) {
        Fact.Copy = gpr(Ops[1].reg);
      }
    }

  for (const auto &[Block, PC] : Consumers) {
    const auto &I = Code.at(PC);
    if (!I.Signing || !I.Modifier)
      continue;
    const auto R = *I.Modifier;
    auto Paths = reachingSigners(CFG, Code, *Block, PC, R, I.Signing->Key);
    if (!Paths.Joined &&
        (!Paths.CrossesEdge || (!Paths.Mismatch && Paths.Definitions.empty())))
      continue;
    if (Paths.Unknown || Paths.Mismatch || Paths.Definitions.empty()) {
      Result.Unresolved = true;
      continue;
    }
    const uint32_t OtherReads = ReadTwice | (Read & ~I.Reads);
    const bool Local =
        llvm::is_contained(machOSigningScratchRegisters(),
                           AARCH64_REG_X0 + R) &&
        !(OtherReads & (uint32_t(1) << R)) &&
        llvm::all_of(Paths.Definitions, [&](const auto &P) {
          const auto &Producer = Code.at(P.first);
          return Producer.Definition == R && Producer.Address == P.second;
        });
    const bool Same = llvm::all_of(Paths.Definitions, [&](const auto &P) {
      return P.second == Paths.Definitions.front().second;
    });
    const auto Relative =
        Same ? machOPCRelativeAuthentication(I.Signing->Id, PC,
                                             Paths.Definitions.front().second)
             : std::nullopt;
    std::optional<MachOSigningConsumer> Dispatch;
    std::optional<unsigned> Scratch;
    if (!Local && !Relative) {
      for (auto Physical : machOSigningScratchRegisters()) {
        const unsigned N = Physical - AARCH64_REG_X0;
        const auto Mask = uint32_t(1) << N;
        if ((Reserved | Paths.WritesToConsumer) & Mask)
          continue;
        // It may be live elsewhere in the function. Its old value must be
        // dead on every exit from each site at which we install a modifier.
        if (!((Read | Written) & Mask) ||
            llvm::all_of(Paths.Definitions, [&](const auto &P) {
              return machODeadSigningScratch(Decode, Source, CFG.Entry, P.first,
                                             Physical);
            })) {
          Scratch = N;
          Reserved |= Mask;
          break;
        }
      }
      if (!Scratch) {
        Dispatch = routeSigningEdges(CFG, Code, Paths, *Block, PC, *I.Signing);
        if (!Dispatch) {
          Result.Unresolved = true;
          continue;
        }
        for (auto [Index, Route] : llvm::enumerate(Dispatch->Routes))
          for (va_t Branch : Route.Branches)
            if (Index)
              Result.BranchRoutes.try_emplace(Branch, PC, Index);
        Result.Dispatches.try_emplace(PC, *Dispatch);
      }
    }
    const auto Bits = readLE<uint32_t>(Source.Bytes.data() + (PC - CFG.Entry));
    uint32_t Replacement = Relative.value_or(Bits);
    if (Scratch) {
      const unsigned Shift = I.Signing->Return ? 0 : kAArch64PAuthLRImmShift;
      Replacement =
          (Bits & ~(kAArch64PAuthLRRegMask << Shift)) | (*Scratch << Shift);
    }
    bool First = true;
    for (const auto &[Origin, Sign] : Paths.Definitions) {
      MachOSigningAddress A;
      A.Use =
          Local ? MachOSigningAddress::Local : MachOSigningAddress::Observable;
      const auto SigningWord =
          readLE<uint32_t>(Source.Bytes.data() + (Sign - CFG.Entry));
      Result.SigningPCs.insert(Sign);
      Result.RetainedSigningWords.insert(SigningWord);
      if (!Local && (Scratch || First))
        A.Authentication = MachOAuthenticationReplacement{
            Dispatch.value_or(
                MachOSigningConsumer{PC, Scratch, false, Scratch.has_value()}),
            Replacement, Sign};
      if (!Result.Addresses.try_emplace(Origin, A).second)
        Result.Unresolved = true;
      First = false;
    }
    if (!Local)
      Result.Consumers.try_emplace(PC, Replacement);
  }
  // Numeric ADRs can widen at installation. Include the space available to
  // every local expansion before choosing a backwards immediate at its limit.
  // The linked-image verifier checks the exact displacement independently.
  if (Result.Dispatches.empty())
    return Result;
  const auto *Frame = Image.ExceptionMetadata.findFunction(CFG.Entry);
  size_t Growth = 0;
  for (size_t Offset = 0; Offset < Count * Word; Offset += Word) {
    const auto Bits = readLE<uint32_t>(Source.Bytes.data() + Offset);
    Growth += (Bits & aarch64::kADRP_Mask) == aarch64::kADR_Op ||
              aarch64::isPointerLiteralLoad(Bits);
  }
  for (const auto &[Block, PC] : Consumers) {
    if (auto It = Result.Dispatches.find(PC); It != Result.Dispatches.end()) {
      const auto &Auth = It->second;
      const auto BodyWords =
          Auth.Returns ? ReturnIslandWords : FallthroughIslandWords;
      Growth += Auth.insertedWords() + Auth.Routes.size() * (1 + BodyWords);
    } else {
      // A register setup and an ADRP/ADD restoring the caller's address.
      Growth += MachOSigningConsumer{PC, 0, true}.insertedWords();
    }
  }
  for (const auto &[Block, PC] : Consumers) {
    auto It = Result.Dispatches.find(PC);
    if (It == Result.Dispatches.end())
      continue;
    auto &Auth = It->second;
    const auto Op = Code.at(PC).Signing->Id;
    for (auto [Index, Route] : llvm::enumerate(Auth.Routes)) {
      const auto At = PC + Index * Auth.routeWords() * Word;
      if (Route.Signer <= At &&
          machOPCRelativeAuthentication(Op, At + Growth * Word, Route.Signer))
        continue;
      llvm::SmallVector<va_t, 4> PCs{Route.Signer, Route.Signer + Word, PC};
      if (!Auth.Returns)
        PCs.push_back(PC + Word);
      if (!Frame || !sameDarwinAArch64UnwindScope(Image, *Frame, PCs)) {
        if (machOPCRelativeAuthentication(Op, At, Route.Signer))
          continue;
        Result.Unresolved = true;
        return Result;
      }
      auto &Islands = Result.Islands[Route.Signer];
      // The original PAC falls through a single branch over these copies.
      unsigned Offset = FirstIslandWord;
      if (!Islands.empty()) {
        const auto &Last = Islands.back();
        const auto &Previous = Result.Dispatches.at(Last.Consumer);
        Offset =
            Previous.Routes[Last.Route].IslandWord +
            (Previous.Returns ? ReturnIslandWords : FallthroughIslandWords);
      }
      const auto Encoding = machOPCRelativeAuthentication(Op, Offset * Word, 0);
      if (!Encoding) {
        Result.Unresolved = true;
        return Result;
      }
      Route.IslandWord = Offset;
      Route.IslandWords =
          unsigned(Islands.empty()) +
          (Auth.Returns ? ReturnIslandWords : FallthroughIslandWords);
      Islands.push_back({PC, unsigned(Index), *Encoding});
    }
  }
  for (auto &[Origin, Address] : Result.Addresses)
    if (Address.Authentication &&
        !Address.Authentication->Consumer.Routes.empty())
      Address.Authentication->Consumer =
          Result.Dispatches.at(Address.Authentication->Consumer.Instruction);
  return Result;
}

} // namespace neverd
