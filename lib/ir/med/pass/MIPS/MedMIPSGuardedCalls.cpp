//===- MedMIPSGuardedCalls.cpp - Equality-guarded native arguments --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MedMIPSGuardedCalls.h"

#include "../../../../safety/StackSlotFlow.h"
#include "../../abi/MedABIPassDetail.h"

#include "neverd/ArchSupport.h"
#include "neverd/ir/Limits.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <array>

namespace neverd::mips {

const MedVar *copiedWord(const MedOp &Op) {
  const MedVar *Input = nullptr;
  if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
    Input = &Op.Inputs[0];
  else if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_OR ||
            Op.Opcode == NdOp::INT_XOR) &&
           Op.NumInputs == 2) {
    for (unsigned I : {0u, 1u})
      if (Op.Inputs[I].isConst() && Op.Inputs[I].ConstVal == 0) {
        Input = &Op.Inputs[1 - I];
        break;
      }
  }
  return Input && Input->Size == Op.Output.Size ? Input : nullptr;
}

namespace {
enum class Evidence { None, Independent, Equal };
// Identity 0 is unknown. Reserve one identity for the all-zero pattern
// so a shift and an OR can reconstruct the two exact 32-bit halves.
constexpr unsigned ZeroId = 1;
// The bounded analysis uses the low bits for local class numbers. Preserve
// literal identities across edges and distinguish entry bits from opaque data.
constexpr unsigned EntryBit = 1u << 30;
constexpr unsigned LiteralBit = 1u << 31;
bool literal(unsigned Id) { return Id == ZeroId || (Id & LiteralBit); }
bool opaque(unsigned Id) {
  return Id > ZeroId && !(Id & (EntryBit | LiteralBit));
}
using BitPair = std::pair<unsigned, unsigned>;
using KnownWords = llvm::SmallDenseSet<BitPair, 8>;
constexpr int64_t WordBytes = mipsreg::SlotBytes;
constexpr int64_t HalfBytes = WordBytes / 2;
constexpr unsigned HalfByteMask = (1u << HalfBytes) - 1;

bool highMemoryHalf(int64_t Displacement, bool Little) {
  return Displacement == (Little ? HalfBytes : 0);
}

struct Half {
  unsigned Id = 0;
  // Partners in a whole-word guard, indexed by the position of these bits.
  // A later check of a projection can replace one partner while the other
  // half still records the original pair, including across a CFG join.
  unsigned CheckedHigh = 0, CheckedLow = 0;
  // The equality class on this path. The original identity survives a call.
  unsigned Compared = 0;
  unsigned number() const { return Compared ? Compared : Id; }
  bool operator==(const Half &) const = default;
};

struct Word {
  Half Low, High;
  Evidence Proof = Evidence::None;
  bool operator==(const Word &) const = default;

  bool complete() const { return Low.Id && High.Id; }
  BitPair numbers() const { return {Low.number(), High.number()}; }
  void qualify(const KnownWords &Known) {
    if (Proof != Evidence::None || !complete())
      return;
    const auto [L, H] = numbers();
    if ((literal(L) && literal(H)) || Known.contains({L, H}))
      Proof = Evidence::Equal;
  }
  Evidence evidence() const {
    if (Proof == Evidence::None &&
        ((Low.CheckedHigh && Low.CheckedHigh == High.Id) ||
         (High.CheckedLow && High.CheckedLow == Low.Id)))
      return Evidence::Equal;
    return Proof;
  }
  void check(const Word &W) {
    for (auto *H : {&Low, &High}) {
      if (H->Id == W.Low.Id)
        H->CheckedHigh = W.High.Id;
      if (H->Id == W.High.Id)
        H->CheckedLow = W.Low.Id;
    }
    if (Low.Id == W.Low.Id && High.Id == W.High.Id &&
        Proof != Evidence::Independent)
      Proof = Evidence::Equal;
  }
  void afterCall() {
    if (Proof == Evidence::Equal)
      Proof = Evidence::None;
    for (auto *H : {&Low, &High})
      H->CheckedHigh = H->CheckedLow = H->Compared = 0;
  }
};

/// Only transfer exact 32-bit patterns. Sign extension leaves the upper
/// pattern unknown; a subsequent left shift by 32 discards it.
Word transferredHalves(const MedOp &Op,
                       llvm::function_ref<Word(const MedVar &)> Read) {
  if (Op.NumInputs == 1 && Op.Inputs[0].Size == HalfBytes &&
      Op.Output.Size == WordBytes &&
      (Op.Opcode == NdOp::INT_ZEXT || Op.Opcode == NdOp::INT_SEXT))
    return {Read(Op.Inputs[0]).Low, {Op.Opcode == NdOp::INT_ZEXT ? ZeroId : 0}};
  if (Op.NumInputs != 2)
    return {};
  const auto &A = Op.Inputs[0], &B = Op.Inputs[1];
  const bool Shift = Op.Opcode == NdOp::INT_LEFT ||
                     Op.Opcode == NdOp::INT_RIGHT ||
                     Op.Opcode == NdOp::INT_ASHR;
  if (Shift && B.isConst() && B.ConstVal == 0 && A.Size == B.Size &&
      A.Size == Op.Output.Size)
    return Read(A);
  if (Op.Opcode == NdOp::SUBBYTES && A.Size == WordBytes && B.isConst() &&
      Op.Output.Size == HalfBytes &&
      (B.ConstVal == 0 || B.ConstVal == HalfBytes)) {
    const auto W = Read(A);
    return {B.ConstVal == 0 ? W.Low : W.High, {}};
  }
  if (Op.Output.Size != WordBytes)
    return {};
  if (Op.Opcode == NdOp::CONCAT && A.Size == HalfBytes && B.Size == HalfBytes)
    return {Read(B).Low, Read(A).Low};
  if (A.Size != WordBytes || B.Size != WordBytes)
    return {};
  if (Shift && B.isConst() && B.ConstVal == 32) {
    if (Op.Opcode == NdOp::INT_LEFT)
      return {{ZeroId}, Read(A).Low};
    return {Read(A).High, {Op.Opcode == NdOp::INT_RIGHT ? ZeroId : 0}};
  }
  if (Op.Opcode == NdOp::INT_AND && (A.isConst() || B.isConst())) {
    const auto Mask = A.isConst() ? A.ConstVal : B.ConstVal;
    const auto W = Read(A.isConst() ? B : A);
    const auto Apply = [](Half H, uint32_t Bits) -> Half {
      if (Bits == UINT32_MAX)
        return H;
      return {Bits == 0 ? ZeroId : 0};
    };
    return {Apply(W.Low, uint32_t(Mask)), Apply(W.High, uint32_t(Mask >> 32))};
  }
  if (Op.Opcode == NdOp::INT_OR || Op.Opcode == NdOp::INT_XOR ||
      Op.Opcode == NdOp::INT_ADD) {
    const auto L = Read(A), R = Read(B);
    // A carry out of overlapping low bits may change the entire high half.
    if (Op.Opcode == NdOp::INT_ADD && L.Low.Id != ZeroId && R.Low.Id != ZeroId)
      return {};
    const auto Combine = [](Half L, Half R) -> Half {
      if (L.Id == ZeroId)
        return R;
      return R.Id == ZeroId ? L : Half{};
    };
    return {Combine(L.Low, R.Low), Combine(L.High, R.High)};
  }
  return {};
}

struct RegisterWord {
  const MedVar *Name = nullptr;
  Word Value;
  bool operator==(const RegisterWord &) const = default;
};
using Registers = std::array<RegisterWord, 32>;

bool fullGPR(const MedVar &V) {
  return V.Kind == MedVar::Reg && V.RegOff < mipsreg::r(32) &&
         V.RegOff % mipsreg::SlotBytes == 0 && V.Size == mipsreg::SlotBytes;
}

RegisterWord bind(const MedVar &Name, Word Value) {
  return Value == Word{} ? RegisterWord{} : RegisterWord{&Name, Value};
}

void writeRegister(Registers &Regs, const MedVar &V, Word Value) {
  if (V.Kind != MedVar::Reg || !V.Size || V.RegOff >= mipsreg::r(32))
    return;
  const unsigned First = V.RegOff / mipsreg::SlotBytes;
  if (fullGPR(V)) {
    Regs[First] = bind(V, Value);
    return;
  }
  for (unsigned I = First; I < Regs.size() && mipsreg::r(I) < V.RegOff + V.Size;
       ++I)
    Regs[I] = {};
}

struct Spill {
  Word Value;
  // Bit i is the byte at slot address + i. A checked word can still contain
  // opaque bytes that matter if a later write changes its value.
  unsigned OpaqueBytes = 0;
  bool Unchecked = false;
  bool operator==(const Spill &) const = default;

  void forget() {
    Value = {};
    Unchecked = OpaqueBytes != 0;
  }

  bool store(int64_t Offset, uint16_t Bytes, Word W, bool Little,
             unsigned &NextIdentity) {
    if (!Offset && Bytes == WordBytes) {
      Value = W;
      // A join may keep a whole-word guard but lose both individual
      // identities (one arm supplied an independent value).
      const auto Proof = W.evidence();
      const auto OpaqueHalf = [&](Half H) {
        return opaque(H.Id) || (!H.Id && Proof == Evidence::Equal);
      };
      const unsigned Low = OpaqueHalf(W.Low) ? HalfByteMask : 0;
      const unsigned High = OpaqueHalf(W.High) ? HalfByteMask : 0;
      OpaqueBytes =
          Little ? Low | (High << HalfBytes) : (Low << HalfBytes) | High;
      Unchecked = OpaqueBytes && Proof == Evidence::None;
      return (W.complete() && OpaqueBytes) || Proof == Evidence::Equal;
    }
    bool Tracked = false;
    // Replacing part of a checked word can expose a different pointer.
    // Clear only the bytes covered by this store, in address order.
    const bool HadOpaque = OpaqueBytes != 0;
    const auto Begin = std::max<int64_t>(0, Offset);
    const auto End = std::min<int64_t>(WordBytes, Offset + Bytes);
    OpaqueBytes &= ~(llvm::maskTrailingOnes<unsigned>(End - Begin) << Begin);
    Value.Proof = Evidence::None;
    for (int64_t At : {int64_t{0}, HalfBytes}) {
      if (!safety::detail::stackRangesOverlap(At, HalfBytes, Offset, Bytes))
        continue;
      const bool High = highMemoryHalf(At, Little);
      auto &Part = High ? Value.High : Value.Low;
      Half H;
      if (At == Offset && Bytes == HalfBytes) {
        H = W.Low;
        Tracked = opaque(H.Id);
      }
      if (opaque(H.Id))
        OpaqueBytes |= HalfByteMask << At;
      // A sample after a narrow write has fresh bits. Earlier samples
      // keep their identities, including the half that was untouched.
      if (!H.Id && Part.Id && HadOpaque && Bytes <= HalfBytes)
        H.Id = NextIdentity++;
      Part = H;
    }
    Unchecked = OpaqueBytes && Value.evidence() != Evidence::Equal;
    return Tracked;
  }

  bool exposedAtCall(bool Private) {
    if (Private) {
      Value.afterCall();
      return false;
    }
    const bool Exposed = Unchecked;
    *this = {};
    return Exposed;
  }
};
struct ZeroTest {
  Word Left, Right;
  bool BothZero;
  bool operator==(const ZeroTest &) const = default;
};
// (lo, hi) names a whole arithmetic sample. (bits, 0) names a single
// 32-bit pattern anywhere a move, projection or stack reload carries it.
using ZeroTests = llvm::DenseMap<BitPair, ZeroTest>;

struct State {
  Registers Regs{};
  llvm::SmallVector<Spill, 0> Slots;
  // Entry words that are independently non-frame. Keep their ordered halves
  // even if the machine later overwrites the register holding the peer.
  // Only the predecessor-free entry block appends to this list, so all
  // incoming edges carry the same peers in the same order.
  llvm::SmallVector<Word, 0> Peers;
  ZeroTests Tests;
  unsigned NextId = ZeroId + 1;

  KnownWords knownWords() const {
    KnownWords Known;
    for (const Word &Peer : Peers)
      Known.insert(Peer.numbers());
    return Known;
  }
  bool operator==(const State &) const = default;
};

/// A true machine equality identifies corresponding 32-bit patterns. Apply
/// both substitutions to every live copy and to the independent peers. This
/// keeps earlier equalities when another comparison shares only one half.
void assumeEqual(State &Out, Word L, Word R) {
  const Word *Whole = nullptr;
  if (L.complete() && R.Proof == Evidence::Independent)
    Whole = &L;
  else if (R.complete() && L.Proof == Evidence::Independent)
    Whole = &R;
  llvm::SmallVector<BitPair, 2> Substitutions;
  const auto Number = [&](unsigned Id) {
    for (auto [From, To] : Substitutions)
      if (Id == From)
        Id = To;
    return Id;
  };
  for (auto [A, B] : {BitPair{L.Low.number(), R.Low.number()},
                      BitPair{L.High.number(), R.High.number()}}) {
    A = Number(A);
    B = Number(B);
    if (!A || !B || A == B)
      continue;
    if (literal(A) && literal(B))
      return;
    if (literal(A))
      std::swap(A, B);
    Substitutions.emplace_back(A, B);
  }
  if (!Whole && Substitutions.empty())
    return;
  const auto Rename = [&](Word &W) {
    for (auto *H : {&W.Low, &W.High}) {
      const unsigned Id = Number(H->number());
      H->Compared = Id == H->Id ? 0 : Id;
    }
  };
  for (Word &Peer : Out.Peers)
    Rename(Peer);
  const auto Known = Out.knownWords();
  const auto Check = [&](Word &W) {
    if (Whole)
      W.check(*Whole);
    Rename(W);
    W.qualify(Known);
  };
  for (auto &Reg : Out.Regs)
    Check(Reg.Value);
  for (auto &S : Out.Slots) {
    Check(S.Value);
    if (S.Value.evidence() != Evidence::None)
      S.Unchecked = false;
  }
}

// Sample the operands before any register reuse or branch delay slot. A
// modular subtraction can borrow into the high half; only its low half
// and the complete difference independently imply an equality.
void captureZeroTest(ZeroTests &Tests, NdOp Op, Word &Result, Word L, Word R,
                     unsigned &NextId) {
  if (Result.complete() || L == Word{} || R == Word{})
    return;
  const bool BothZero = Op == NdOp::INT_OR;
  // Equality is symmetric, including a zero modular subtraction. Keep an
  // independent peer on the same side when two paths reverse the operands.
  if (L.Proof == Evidence::Independent && R.Proof != Evidence::Independent)
    std::swap(L, R);
  for (auto Part : {&Word::Low, &Word::High}) {
    auto &H = Result.*Part;
    if (H.Id)
      continue;
    H.Id = NextId++;
    if (Op != NdOp::INT_SUB || Part == &Word::Low)
      Tests.try_emplace({H.Id, 0},
                        ZeroTest{{L.*Part, {}}, {R.*Part, {}}, BothZero});
  }
  Tests.try_emplace({Result.Low.Id, Result.High.Id}, ZeroTest{L, R, BothZero});
}

bool assumeZeroTest(State &Out, Word Tested, size_t Locations,
                    size_t &Remaining) {
  llvm::SmallVector<const ZeroTest *, 8> Work;
  llvm::SmallPtrSet<const ZeroTest *, 8> Seen;
  const auto Add = [&](BitPair Bits) {
    const auto I = Out.Tests.find(Bits);
    if (I == Out.Tests.end())
      return false;
    if (Seen.insert(&I->second).second)
      Work.push_back(&I->second);
    return true;
  };
  const auto Visit = [&](Word W) {
    if (!(W.complete() && Add({W.Low.Id, W.High.Id}))) {
      Add({W.Low.Id, 0});
      Add({W.High.Id, 0});
    }
  };
  Visit(Tested);
  const Word Zero{{ZeroId}, {ZeroId}, Evidence::Independent};
  while (!Work.empty()) {
    const auto &T = *Work.pop_back_val();
    const auto Cost = Locations * (T.BothZero ? 2 : 1);
    if (Cost > Remaining)
      return false;
    Remaining -= Cost;
    if (T.BothZero) {
      for (const Word &Part : {T.Left, T.Right}) {
        assumeEqual(Out, Part, Zero);
        Visit(Part);
      }
    } else {
      assumeEqual(Out, T.Left, T.Right);
    }
  }
  return true;
}

/// An identity names equal bits within one execution state. At a join two
/// locations remain equal exactly when their incoming identities agree on
/// each edge. Renumbering by location order gives loops a stable fixed point
/// without identifying successive executions of the same call instruction.
bool intersect(State &A, const State &B, size_t &Remaining) {
  llvm::SmallDenseMap<std::pair<unsigned, unsigned>, unsigned, 8> Classes;
  const auto MeetBits = [&](unsigned L, unsigned R) -> unsigned {
    if (!L || !R)
      return 0;
    if (L == R && literal(L))
      return L;
    const unsigned Kind = !opaque(L) && !opaque(R) ? EntryBit : 0;
    return Classes.try_emplace({L, R}, Kind | (Classes.size() + ZeroId + 1))
        .first->second;
  };
  const auto MeetHalf = [&](Half L, Half R) -> Half {
    Half H{MeetBits(L.Id, R.Id), MeetBits(L.CheckedHigh, R.CheckedHigh),
           MeetBits(L.CheckedLow, R.CheckedLow)};
    const auto Id = MeetBits(L.number(), R.number());
    H.Compared = Id == H.Id ? 0 : Id;
    return H;
  };
  const auto Meet = [&](Word L, Word R) {
    Word V{MeetHalf(L.Low, R.Low), MeetHalf(L.High, R.High)};
    if (L.evidence() == R.evidence())
      V.Proof = L.evidence();
    else if (L.evidence() != Evidence::None && R.evidence() != Evidence::None)
      V.Proof = Evidence::Equal;
    return V;
  };
  // Keep only implications reachable from a surviving register or slot.
  // Each merged implication must hold on both edges. Its sampled operands
  // use the same identity mapping as the live words, even after a PHI.
  struct PendingTest {
    BitPair Bits;
    const ZeroTest *Left, *Right;
  };
  ZeroTests Joined;
  llvm::SmallVector<PendingTest, 8> Work;
  const auto Match = [&](BitPair Bits, BitPair L, BitPair R) {
    const auto I = A.Tests.find(L);
    const auto J = B.Tests.find(R);
    if (I != A.Tests.end() && J != B.Tests.end() &&
        I->second.BothZero == J->second.BothZero &&
        Joined.try_emplace(Bits).second)
      Work.push_back({Bits, &I->second, &J->second});
  };
  const auto MatchWord = [&](Word V, Word L, Word R) {
    if (V.complete())
      Match({V.Low.Id, V.High.Id}, {L.Low.Id, L.High.Id},
            {R.Low.Id, R.High.Id});
    if (V.Low.Id)
      Match({V.Low.Id, 0}, {L.Low.Id, 0}, {R.Low.Id, 0});
    if (V.High.Id)
      Match({V.High.Id, 0}, {L.High.Id, 0}, {R.High.Id, 0});
  };
  const auto MeetLive = [&](Word L, Word R) {
    Word V = Meet(L, R);
    if (!A.Tests.empty() && !B.Tests.empty())
      MatchWord(V, L, R);
    return V;
  };
  for (unsigned I = 0; I < A.Regs.size(); ++I) {
    auto &L = A.Regs[I];
    const auto &R = B.Regs[I];
    L = L.Name && L.Name == R.Name ? bind(*L.Name, MeetLive(L.Value, R.Value))
                                   : RegisterWord{};
  }
  for (size_t I = 0; I < A.Slots.size(); ++I) {
    A.Slots[I].Value = MeetLive(A.Slots[I].Value, B.Slots[I].Value);
    A.Slots[I].OpaqueBytes |= B.Slots[I].OpaqueBytes;
    A.Slots[I].Unchecked |= B.Slots[I].Unchecked;
  }
  for (size_t I = 0; I < A.Peers.size(); ++I)
    A.Peers[I] = Meet(A.Peers[I], B.Peers[I]);
  while (!Work.empty()) {
    if (!Remaining)
      return false;
    --Remaining;
    const auto [Bits, L, R] = Work.pop_back_val();
    const Word Left = Meet(L->Left, R->Left), Right = Meet(L->Right, R->Right);
    Joined.at(Bits) = {Left, Right, L->BothZero};
    if (L->BothZero) {
      MatchWord(Left, L->Left, R->Left);
      MatchWord(Right, L->Right, R->Right);
    }
  }
  A.Tests = std::move(Joined);
  A.NextId = Classes.size() + ZeroId + 1;
  return true;
}

bool comparesWordToZero(const MedOp &Op) {
  const auto IsZero = [](const MedVar &V) {
    return V.isConst() && !V.ConstVal;
  };
  return (Op.Opcode == NdOp::INT_EQUAL || Op.Opcode == NdOp::INT_NOTEQUAL) &&
         Op.NumInputs == 2 && Op.Inputs[0].Size == WordBytes &&
         Op.Inputs[1].Size == WordBytes &&
         (IsZero(Op.Inputs[0]) || IsZero(Op.Inputs[1]));
}

/// Select the successor on which the comparison was equal. The instruction
/// in the delay slot may change a register after the condition was captured.
struct EqualityEdge {
  const MedOp *Compare = nullptr;
  const MedBlock *Successor = nullptr;
};
EqualityEdge equalityEdge(const MedBlock &Block,
                          const safety::detail::ExactPhiGraph &Graph,
                          MedDefSiteIndex &Definitions) {
  if (Block.Role != MedBlockRole::Machine || Block.Ops.empty() ||
      Block.Succs.size() != 2)
    return {};
  const auto &Branch = Block.Ops.back();
  if (Branch.Opcode != NdOp::COND_BR || Branch.NumInputs != 2 ||
      !Branch.Inputs[0].isConst() || Branch.Inputs[1].Size != 1)
    return {};
  const auto D = Definitions.find(Branch.Inputs[1]);
  if (D.Ambiguous || D.Block != &Block || !D.Op || D.Op->NumInputs != 2 ||
      D.Op->Output.Size != 1 || D.Op->Output != Branch.Inputs[1] ||
      (D.Op->Opcode != NdOp::INT_EQUAL && D.Op->Opcode != NdOp::INT_NOTEQUAL))
    return {};
  const auto *A = Graph.blockById(Block.Succs[0]);
  const auto *B = Graph.blockById(Block.Succs[1]);
  const auto Target = Branch.Inputs[0].ConstVal;
  if (!A || !B || A->Role != MedBlockRole::Machine ||
      B->Role != MedBlockRole::Machine ||
      (A->StartAddr == Target) == (B->StartAddr == Target))
    return {};
  return {D.Op, (A->StartAddr == Target) == (D.Op->Opcode == NdOp::INT_EQUAL)
                    ? A
                    : B};
}

/// Rename only a complete incoming physical value. A PHI is not a write
/// that can erase a partial register update or an unchecked incoming path.
void enterPhi(Registers &Out, const PhiNode &Phi, int Pred) {
  if (!fullGPR(Phi.Output)) {
    writeRegister(Out, Phi.Output, {});
    return;
  }
  auto &R = Out[Phi.Output.RegOff / mipsreg::SlotBytes];
  if (!R.Name)
    return;
  for (const auto &[Id, V] : Phi.Args)
    if (Id == Pred && safety::detail::sameRegisterOccurrence(*R.Name, V)) {
      R.Name = &Phi.Output;
      return;
    }
  R = {};
}

} // namespace

GuardedCalls guardedResultArguments(
    const MedFunc &F, const NativeCallSites &Native, const TargetRegInfo &TRI,
    BinaryFormat Format, const safety::detail::ExactPhiGraph &Graph,
    MedDefSiteIndex &Definitions, GuardedStackLayout Stack, size_t &Remaining,
    llvm::function_ref<bool(const MedVar &)> IsNonFrame) {
  if (F.Blocks.empty() || !F.Blocks.front().isFunctionEntry(F.Entry))
    return GuardedCalls{};
  GuardedCalls Proven;
  const auto Arguments = TRI.integerArgumentLayout(Format);
  // Index aligned frame cells used for a 64- or 32-bit transfer. Even a store
  // with no later load may escape as a stack argument.
  const bool Little = archLittleEndian(TRI.TheArch);
  llvm::SmallVector<int64_t, 8> Offsets;
  bool HasZeroComparison = false;
  for (const auto &B : F.Blocks) {
    if (B.Ops.size() > Remaining)
      return {};
    Remaining -= B.Ops.size();
    for (const auto &Op : B.Ops) {
      HasZeroComparison = HasZeroComparison || comparesWordToZero(Op);
      if (safety::detail::isStackMemoryWrite(Op.Opcode) &&
          (Op.Opcode != NdOp::STORE || Op.NumInputs != 2 || !Op.Inputs[1].Size))
        return {};
      if (Op.Opcode != NdOp::STORE &&
          (Op.Opcode != NdOp::LOAD || Op.NumInputs != 1))
        continue;
      const auto Bytes =
          Op.Opcode == NdOp::STORE ? Op.Inputs[1].Size : Op.Output.Size;
      if (Bytes == WordBytes || Bytes == HalfBytes)
        if (const auto At = Stack.Offset(Op.Inputs[0]);
            At && *At >= -limits::ir::kSourceFrameAnalysisMaxFrame &&
            *At <= -Bytes && *At % Bytes == 0)
          Offsets.push_back(*At & -WordBytes);
    }
  }
  llvm::sort(Offsets);
  Offsets.erase(std::unique(Offsets.begin(), Offsets.end()), Offsets.end());
  State Entry;
  Entry.Slots.resize(Offsets.size());
  // Every 32-bit pattern is a valid literal, including DenseMap's reserved
  // integer keys. Widen the key before interning the pattern.
  llvm::SmallDenseMap<uint64_t, unsigned, 8> Literals;
  const auto LiteralId = [&](uint32_t Bits) -> unsigned {
    if (!Bits)
      return ZeroId;
    return Literals.try_emplace(Bits, LiteralBit | Literals.size())
        .first->second;
  };
  struct BlockInput {
    State Words;
    llvm::SmallVector<std::pair<int, State>, 0> Edges;
    bool Visited = false;
  };
  llvm::DenseMap<const MedBlock *, BlockInput> Incoming;
  llvm::SmallSetVector<const MedBlock *, 8> Work;
  Incoming[&F.Blocks.front()].Words = Entry;
  llvm::SmallPtrSet<const MedOp *, 4> Exposed;
  Work.insert(&F.Blocks.front());
  while (!Work.empty()) {
    const auto *B = Work.pop_back_val();
    const size_t Cost = 1 + B->Ops.size() + B->Preds.size() + B->Succs.size();
    if (Cost > Remaining)
      return GuardedCalls{};
    Remaining -= Cost;
    auto &Input = Incoming.at(B);
    if (!Input.Visited) {
      if (!Graph.hasCompleteNormalPredecessors(*B))
        return GuardedCalls{};
      for (const auto &Phi : B->Phis) {
        if (Phi.Args.size() + 1 > Remaining)
          return GuardedCalls{};
        Remaining -= Phi.Args.size() + 1;
        if (fullGPR(Phi.Output) &&
            !Graph.hasCompleteIncoming(B - F.Blocks.data(), Phi))
          return GuardedCalls{};
      }
      Input.Visited = true;
    }
    if (Input.Words.Tests.size() > Remaining)
      return {};
    Remaining -= Input.Words.Tests.size();
    auto Current = Input.Words;
    auto &Regs = Current.Regs;
    auto SP = Stack.BlockEntrySP(B->Id);
    if (Current.Peers.size() > Remaining)
      return {};
    Remaining -= Current.Peers.size();
    auto Known = Current.knownWords();
    auto &NextIdentity = Current.NextId;
    llvm::SmallDenseMap<std::pair<int, int>, Word, 8> Temps;
    auto read = [&](const MedVar &V) -> Word {
      if (V.Size != WordBytes && V.Size != HalfBytes)
        return {};
      if (V.isConst())
        return {
            {LiteralId(uint32_t(V.ConstVal))},
            {V.Size == WordBytes ? LiteralId(uint32_t(V.ConstVal >> 32)) : 0},
            V.Size == WordBytes ? Evidence::Independent : Evidence::None};
      if (V.Kind == MedVar::Temp)
        return Temps.lookup({V.Id, V.SSAVer});
      if (fullGPR(V)) {
        const auto &R = Regs[V.RegOff / mipsreg::SlotBytes];
        if (R.Name && safety::detail::sameRegisterOccurrence(*R.Name, V))
          return R.Value;
      }
      return {};
    };
    auto write = [&](const MedVar &V, Word Value) {
      if (V.Kind == MedVar::Reg && V.RegOff == TRI.StackPointer) {
        SP = Stack.Offset(V);
        // An allocation cannot reacquire the identity of a released word.
        const auto End = SP ? llvm::lower_bound(Offsets, *SP) : Offsets.end();
        const size_t Count = End - Offsets.begin();
        if (Count > Remaining)
          return false;
        Remaining -= Count;
        for (auto &S : llvm::MutableArrayRef(Current.Slots).take_front(Count))
          S.forget();
      }
      if (V.Kind == MedVar::Temp) {
        if ((V.Size == WordBytes || V.Size == HalfBytes) && Value != Word{})
          Temps[{V.Id, V.SSAVer}] = Value;
        else if (V.Size)
          Temps.erase({V.Id, V.SSAVer});
      } else {
        writeRegister(Regs, V, Value);
      }
      return true;
    };
    const auto Edge = equalityEdge(*B, Graph, Definitions);
    const bool ChecksZero = Edge.Compare && comparesWordToZero(*Edge.Compare);
    Word Left, Right;
    llvm::ArrayRef<size_t> Sites;
    if (const auto It = Native.find(B); It != Native.end())
      Sites = It->second;
    bool AtEntry = B->Preds.empty() && B->isFunctionEntry(F.Entry);
    for (const MedOp &Op : B->Ops) {
      if (&Op == Edge.Compare && Op.Inputs[0].Size == WordBytes &&
          Op.Inputs[1].Size == WordBytes) {
        Left = read(Op.Inputs[0]);
        Right = read(Op.Inputs[1]);
      }
      Word Value;
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        const bool Untyped = !Sites.empty() && &Op == &B->Ops[Sites.front()];
        if (Untyped) {
          Sites = Sites.drop_front();
          GuardedCall Fact;
          for (unsigned I = 0; I < Arguments.Registers.size(); ++I)
            if (Regs[Arguments.Registers[I] / mipsreg::SlotBytes]
                    .Value.evidence() == Evidence::Equal)
              Fact.Registers |= uint32_t{1} << I;
          for (unsigned I = 1; I < Op.NumInputs; ++I)
            if (read(Op.Inputs[I]).evidence() == Evidence::Equal)
              Fact.Inputs.push_back(I);
          if (Fact.Registers || !Fact.Inputs.empty())
            Proven.Arguments[&Op] = std::move(Fact);
          else
            Proven.Arguments.erase(&Op);
        }
        // Raw SSA precedes ABI clobber modeling. Only preserved registers
        // keep their identities; rechecking one after the call is required.
        for (unsigned I = 0; I < Regs.size(); ++I) {
          auto &R = Regs[I];
          if (!R.Name)
            continue;
          if (!TRI.isCallPreserved(mipsreg::r(I), TRI.FullRegWidth))
            R = {};
          else {
            R.Value.afterCall();
            R = bind(*R.Name, R.Value);
          }
        }
        if (Offsets.size() + Current.Peers.size() > Remaining)
          return {};
        Remaining -= Offsets.size() + Current.Peers.size();
        for (auto &Peer : Current.Peers)
          Peer.afterCall();
        Known = Current.knownWords();
        const auto Private = Stack.ClosedPrivateStart(Op);
        bool Unsafe = false;
        for (size_t I = 0; I < Offsets.size(); ++I)
          Unsafe |=
              Current.Slots[I].exposedAtCall(Private && Offsets[I] >= *Private);
        if (Unsafe)
          Exposed.insert(&Op);
        else
          Exposed.erase(&Op);
        Left = Right = {};
        Current.Tests.clear();
        Temps.clear();
        AtEntry = false;
        if (Untyped && fullGPR(Op.Output) &&
            Op.Output.RegOff == TRI.IntReturnReg)
          Value = {{NextIdentity++}, {NextIdentity++}};
      } else {
        if (const auto *Source = copiedWord(Op)) {
          Value = read(*Source);
        } else if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1 &&
                   (Op.Output.Size == WordBytes ||
                    Op.Output.Size == HalfBytes)) {
          const auto At = Stack.Offset(Op.Inputs[0]);
          if (At && SP && *At >= *SP && *At % Op.Output.Size == 0) {
            const auto Base = *At & -WordBytes;
            const auto It = llvm::lower_bound(Offsets, Base);
            if (It != Offsets.end() && *It == Base) {
              const auto &W = Current.Slots[It - Offsets.begin()].Value;
              Value = Op.Output.Size == WordBytes
                          ? W
                          : Word{highMemoryHalf(*At - Base, Little) ? W.High
                                                                    : W.Low,
                                 {}};
            }
          }
        } else if (Op.Opcode == NdOp::STORE && Op.NumInputs == 2) {
          Proven.Spills.erase(&Op);
          const auto At = Stack.Offset(Op.Inputs[0]);
          const auto &Stored = Op.Inputs[1];
          if (At) {
            auto It = llvm::lower_bound(Offsets, *At & -WordBytes);
            for (; It != Offsets.end() && safety::detail::stackRangesOverlap(
                                              *It, WordBytes, *At, Stored.Size);
                 ++It) {
              if (!Remaining)
                return {};
              --Remaining;
              auto &S = Current.Slots[It - Offsets.begin()];
              if (!SP || *It < *SP) {
                S.forget();
                continue;
              }
              if (S.store(*At - *It, Stored.Size, read(Stored), Little,
                          NextIdentity))
                Proven.Spills.insert(&Op);
              S.Value.qualify(Known);
              if (S.Value.evidence() != Evidence::None)
                S.Unchecked = false;
            }
          } else if (!Offsets.empty() && !IsNonFrame(Op.Inputs[0])) {
            if (Offsets.size() > Remaining)
              return {};
            Remaining -= Offsets.size();
            for (auto &S : Current.Slots)
              S.forget();
          }
        } else if (Op.Output.Size) {
          Value = transferredHalves(Op, read);
          if (HasZeroComparison && Op.NumInputs == 2 &&
              Op.Output.Size == WordBytes && Op.Inputs[0].Size == WordBytes &&
              Op.Inputs[1].Size == WordBytes &&
              (Op.Opcode == NdOp::INT_XOR || Op.Opcode == NdOp::INT_SUB ||
               Op.Opcode == NdOp::INT_OR)) {
            captureZeroTest(Current.Tests, Op.Opcode, Value, read(Op.Inputs[0]),
                            read(Op.Inputs[1]), NextIdentity);
          }
        }
        if (AtEntry && fullGPR(Op.Output) &&
            (Value.Proof == Evidence::Independent || IsNonFrame(Op.Output))) {
          Value.Proof = Evidence::Independent;
          for (auto *H : {&Value.Low, &Value.High})
            if (!H->Id)
              H->Id = EntryBit | NextIdentity++;
          if (!(literal(Value.Low.Id) && literal(Value.High.Id)) &&
              Known.insert(Value.numbers()).second)
            Current.Peers.push_back(Value);
        } else {
          Value.qualify(Known);
        }
      }
      if (!write(Op.Output, Value))
        return {};
      for (const auto &Output : Op.IntrinsicOutputs)
        if (!write(Output, {}))
          return {};
    }
    const size_t Locations =
        Regs.size() + Offsets.size() + Current.Peers.size();
    for (int Id : B->Succs) {
      const auto *Succ = Graph.blockById(Id);
      const auto EdgeCost = Locations + Current.Tests.size();
      if (!Succ || Remaining < EdgeCost)
        return {};
      Remaining -= EdgeCost;
      auto Out = Current;
      if (Succ == Edge.Successor) {
        assumeEqual(Out, Left, Right);
        if (ChecksZero &&
            !assumeZeroTest(Out,
                            Edge.Compare->Inputs[0].isConst() ? Right : Left,
                            Locations, Remaining))
          return {};
      }
      for (const auto &Phi : Succ->Phis) {
        if (Phi.Args.size() + 1 > Remaining)
          return GuardedCalls{};
        Remaining -= Phi.Args.size() + 1;
        enterPhi(Out.Regs, Phi, B->Id);
      }
      // Also normalize a single edge so fresh call identities never collide
      // with incoming ones when this block is visited again.
      if (!intersect(Out, Out, Remaining))
        return {};
      auto [It, Inserted] = Incoming.try_emplace(Succ);
      auto &Next = It->second;
      if (Succ->Preds.size() > 1 || Succ == &F.Blocks.front()) {
        // A predecessor may discover a new PHI identity on a later visit.
        // Replace its old contribution before intersecting the current edges.
        auto &Edges = Next.Edges;
        auto Pos = llvm::lower_bound(
            Edges, B->Id, [](const auto &E, int Id) { return E.first < Id; });
        if (Pos != Edges.end() && Pos->first == B->Id) {
          if (Pos->second == Out)
            continue;
          Pos->second = Out;
        } else {
          Edges.insert(Pos, {B->Id, Out});
        }
        if (Edges.size() > Remaining / Locations)
          return {};
        Remaining -= Edges.size() * Locations;
        const auto &First = Edges.front().second;
        if (First.Tests.size() > Remaining)
          return {};
        Remaining -= First.Tests.size();
        Out = First;
        for (const auto &E : llvm::drop_begin(Edges))
          if (!intersect(Out, E.second, Remaining))
            return {};
        if (Succ == &F.Blocks.front() && !intersect(Out, Entry, Remaining))
          return {};
      }
      if (Inserted || Next.Words != Out) {
        Next.Words = std::move(Out);
        Work.insert(Succ);
      }
    }
  }
  // A disconnected predecessor cannot contribute evidence to a merge.
  if (Incoming.size() != F.Blocks.size() || !Exposed.empty())
    return GuardedCalls{};
  return Proven;
}

} // namespace neverd::mips
