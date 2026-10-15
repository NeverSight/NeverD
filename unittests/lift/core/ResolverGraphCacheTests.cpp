//===- ResolverGraphCacheTests.cpp - Frozen proof graph reuse -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

using namespace neverd;

namespace neverd::detail {

// Exercise the production graph/value query, including its resource account.
// The access shim changes graph facts directly so equal-size mutations cannot
// accidentally pass a test that only adds/removes instructions.
struct ResolverGraphCacheTestAccess {
  using Query = CFGBuilder::JumpTableValueQuery;
  using Relation = CFGBuilder::JumpTableValueRelation;
  using Occurrence = CFGBuilder::JumpTableValueOccurrence;
  using Scope = CFGBuilder::ResolverQueryScope;

  struct Answer {
    std::vector<bool> Values;
    std::vector<bool> QueryComplete;
    std::vector<uint64_t> Masks;
    bool Complete = false;
    size_t Remaining = 0;
    bool operator==(const Answer &) const = default;
  };

  static void seed(CFGBuilder &B, const BinaryImage &Image,
                   unsigned Length = 3) {
    B.CurrentImg = &Image;
    B.CurrentFuncEntry = 0x100;
    B.JumpTableProofContextComplete = true;
    B.PersistentCFGRoots = {0x100};
    for (unsigned I = 0; I <= Length; ++I) {
      const va_t Address = 0x100 + I;
      CFGBuilder::InsnRecord R{};
      R.Addr = Address;
      R.Size = 1;
      LowOp Op;
      Op.Addr = Address;
      if (I == Length) {
        R.IsRet = true;
        Op.Opcode = NdOp::RETURN;
        Op.addInput(NdVar::tmp((I - 1) * 8, 8));
      } else {
        Op.Opcode = NdOp::COPY;
        Op.Output = NdVar::tmp(I * 8, 8);
        Op.addInput(I ? NdVar::tmp((I - 1) * 8, 8) : NdVar::cst(7, 8));
      }
      R.Ops.push_back(Op);
      B.Insns.emplace(Address, std::move(R));
      B.BlockStarts.insert(Address);
    }
  }

  // Two nested loops share H and A. A late definition at D reaches the exit
  // along entry -> H -> A -> D -> A -> H -> exit. Unknown condition loads
  // preserve both branch arms. Querying H first must not freeze the exit's
  // intermediate {2} prefix before D contributes 4.
  static std::vector<Query> seedPendingCycle(CFGBuilder &B,
                                             const BinaryImage &Image,
                                             bool Memory,
                                             bool Arithmetic = false) {
    B.CurrentImg = &Image;
    B.CurrentFuncEntry = 0x100;
    B.JumpTableProofContextComplete = true;
    B.PersistentCFGRoots = {0x100};
    const auto Width = static_cast<uint16_t>(Image.getPointerSize());
    const auto SP =
        NdVar::reg(getTargetRegInfo(Image.Arch).StackPointer, Width);
    const auto Lane = NdVar::reg(0, 4);
    auto Emit = [&](va_t Address, NdOp Code, NdVar Output,
                    std::initializer_list<NdVar> Inputs) -> LowOp & {
      auto &R = B.Insns[Address];
      R.Addr = Address;
      R.Size = 0x10;
      LowOp Op;
      Op.Addr = Address;
      Op.Seq = static_cast<int>(R.Ops.size());
      Op.Opcode = Code;
      Op.Output = Output;
      for (const auto &Input : Inputs)
        Op.addInput(Input);
      R.Ops.push_back(Op);
      return R.Ops.back();
    };
    std::vector<Query> Queries;
    for (unsigned I = 0; I < 5; ++I) {
      const va_t Address = 0x100 + I * 0x10;
      B.BlockStarts.insert(Address);
      const auto Slot = NdVar::tmp(0x9000 + I * 16, Width);
      const auto Read = NdVar::tmp(0xa000 + I * 16, 4);
      if (Memory)
        Emit(Address, NdOp::INT_ADD, Slot,
             {SP, NdVar::scalar(uint64_t{0} - 4, Width)});
      if (I == 0 || I == 3) {
        NdVar Value = NdVar::scalar(I == 0 ? 2 : 4, 4);
        if (Arithmetic && I == 3) {
          if (Memory)
            Emit(Address, NdOp::LOAD, Read, {Slot});
          const auto Updated = Memory ? Read : Lane;
          Emit(Address, NdOp::INT_ADD, Updated, {Updated, NdVar::scalar(1, 4)});
          Value = Updated;
        }
        if (Memory)
          Emit(Address, NdOp::STORE, {}, {Slot, Value});
        else if (!Arithmetic || I != 3)
          Emit(Address, NdOp::COPY, Lane, {Value});
      }
      if (Memory && (I == 1 || I == 4))
        Emit(Address, NdOp::LOAD, Read, {Slot});
      const auto Candidate = Memory ? Read : Lane;
      int UseSeq;
      if (I == 1 || I == 2) {
        const auto Condition = NdVar::tmp(0x8000 + I * 16, 1);
        Emit(Address, NdOp::LOAD, Condition, {NdVar::reg(8, Width)});
        const va_t Target = I == 1 ? 0x140 : 0x110;
        UseSeq = Emit(Address, NdOp::COND_BR, {},
                      {NdVar::cst(Target, Width), Condition})
                     .Seq;
        auto &R = B.Insns.at(Address);
        R.IsBranch = R.IsCond = true;
        R.BranchTarget = Target;
      } else if (I == 4) {
        UseSeq = Emit(Address, NdOp::RETURN, {}, {Candidate}).Seq;
        B.Insns.at(Address).IsRet = true;
      } else {
        const va_t Target = I == 0 ? 0x110 : 0x120;
        UseSeq =
            Emit(Address, NdOp::BRANCH, {}, {NdVar::cst(Target, Width)}).Seq;
        auto &R = B.Insns.at(Address);
        R.IsBranch = true;
        R.BranchTarget = Target;
      }
      if (I == 1 || I == 4) {
        Query Q;
        Q.Candidate = Candidate;
        Q.UseAddr = Address;
        Q.UseSeq = UseSeq;
        Q.Relation = Relation::UnsignedFeasibleSet;
        Q.UnsignedUpperBound = 8;
        Queries.push_back(Q);
      }
    }
    return Queries;
  }

  static Query seedPredecessorLane(CFGBuilder &B, const BinaryImage &Image,
                                   unsigned Length) {
    B.CurrentImg = &Image;
    B.CurrentFuncEntry = 0x100;
    B.JumpTableProofContextComplete = true;
    B.PersistentCFGRoots = {0x100};
    const auto Lane = NdVar::reg(0, 8);
    for (unsigned I = 0; I <= Length; ++I) {
      const va_t Address = 0x100 + I;
      CFGBuilder::InsnRecord R{};
      R.Addr = Address;
      R.Size = 1;
      LowOp Op;
      Op.Addr = Address;
      if (I == 0) {
        Op.Opcode = NdOp::COPY;
        Op.Output = Lane;
        Op.addInput(NdVar::cst(7, 8));
      } else if (I == Length) {
        R.IsRet = true;
        Op.Opcode = NdOp::RETURN;
        Op.addInput(Lane);
      } else {
        R.IsBranch = true;
        R.BranchTarget = Address + 1;
        Op.Opcode = NdOp::BRANCH;
        Op.addInput(NdVar::cst(Address + 1, 8));
      }
      R.Ops.push_back(Op);
      B.Insns.emplace(Address, std::move(R));
      B.BlockStarts.insert(Address);
    }
    Query Q;
    Q.Candidate = Lane;
    Q.UseAddr = 0x100 + Length;
    Q.UseSeq = 0;
    Q.Alternatives = {{NdVar::cst(7, 8), InvalidVA, -1, false}};
    return Q;
  }

  static Query seedUnusedPredicate(CFGBuilder &B, const BinaryImage &Image,
                                   NdOp Opcode, unsigned Variant = 0) {
    B.CurrentImg = &Image;
    B.CurrentFuncEntry = 0x100;
    B.JumpTableProofContextComplete = true;
    B.PersistentCFGRoots = {0x100};
    B.BlockStarts = {0x100, 0x191, 0x200};
    auto Emit = [&](va_t Address, NdOp Op, NdVar Output,
                    std::initializer_list<NdVar> Inputs) {
      auto &R = B.Insns[Address];
      R.Addr = Address;
      R.Size = 1;
      LowOp Instruction;
      Instruction.Addr = Address;
      Instruction.Seq = R.Ops.size();
      Instruction.Opcode = Op;
      Instruction.Output = Output;
      for (const NdVar &Input : Inputs)
        Instruction.addInput(Input);
      R.Ops.push_back(Instruction);
    };
    const NdVar Lane = NdVar::reg(0, 4);
    Emit(0x100, NdOp::COPY, Lane, {NdVar::scalar(2, 4)});
    NdVar Input = NdVar::reg(8, 1);
    // The known candidate is independent of this condition's deep operand.
    // Reconstructing the operand would exceed the ordinary expression depth.
    for (unsigned I = 0; I < 96; ++I) {
      const NdVar Next = NdVar::tmp(0x1000 + I * 8, 1);
      Emit(0x101 + I, NdOp::COPY, Next, {Input});
      Input = Next;
    }
    NdVar Condition = NdVar::tmp(0x9000, 1);
    NdVar Output = Condition;
    if (Variant == 5)
      Output.Size = 2; // A partial view does not establish an exact producer.
    Emit(0x180, Opcode, Output, {Input, NdVar::scalar(1, 1)});
    if (Variant == 1 || Variant == 2) {
      const NdVar Alias = NdVar::tmp(0x9008, 1);
      Emit(0x181, Variant == 1 ? NdOp::COPY : NdOp::BOOL_NOT, Alias,
           {Condition});
      Condition = Alias;
    } else if (Variant == 3) {
      B.Insns.at(0x180).IsInstructionGuard = true;
    } else if (Variant == 4) {
      B.BlockStarts.insert(
          0x190); // Keep cross-block producers on the full path.
      Emit(0x18f, NdOp::BRANCH, {},
           {NdVar::cst(0x190, Image.getPointerSize())});
      B.Insns.at(0x18f).IsBranch = true;
      B.Insns.at(0x18f).BranchTarget = 0x190;
    }
    Emit(0x190, NdOp::COND_BR, {},
         {NdVar::cst(0x200, Image.getPointerSize()), Condition});
    auto &Branch = B.Insns.at(0x190);
    Branch.IsBranch = Branch.IsCond = true;
    Branch.BranchTarget = 0x200;
    for (va_t Address : {va_t{0x191}, va_t{0x200}}) {
      Emit(Address, NdOp::RETURN, {}, {Lane});
      B.Insns.at(Address).IsRet = true;
    }
    Query Q;
    Q.Candidate = Lane;
    Q.UseAddr = 0x200;
    Q.UseSeq = 0;
    Q.Relation = Relation::UnsignedLessThan;
    Q.UnsignedUpperBound = 4;
    return Q;
  }

  // The first and recurrent values have separate guards. Neither comparison
  // alone names the complete table-index merge after the backedge is known.
  static Query seedJoinedGuard(CFGBuilder &B, const BinaryImage &Image,
                               unsigned Variant = 0) {
    B.CurrentImg = &Image;
    B.CurrentFuncEntry = 0x100;
    B.JumpTableProofContextComplete = true;
    B.PersistentCFGRoots = {0x100};
    B.BlockStarts = {0x100, 0x101, 0x200, 0x201, 0x300};
    if (Variant == 5)
      B.PersistentCFGRoots.insert(0x101);
    const uint16_t PointerSize = Image.getPointerSize();
    const NdVar Index = NdVar::reg(0, 4);
    const NdVar WideIndex = NdVar::reg(0, PointerSize);
    auto Emit = [&](va_t Address, NdOp Opcode, NdVar Output,
                    std::initializer_list<NdVar> Inputs) {
      auto &R = B.Insns[Address];
      R.Addr = Address;
      R.Size = 1;
      LowOp Op;
      Op.Addr = Address;
      Op.Seq = static_cast<int>(R.Ops.size());
      Op.Opcode = Opcode;
      Op.Output = Output;
      for (NdVar Input : Inputs)
        Op.addInput(Input);
      R.Ops.push_back(Op);
    };
    auto Guard = [&](va_t Address, uint64_t InputReg, bool Above) {
      const uint16_t InputWidth = Variant == 6 ? PointerSize : 4;
      Emit(Address, NdOp::COPY, NdVar::reg(0, InputWidth),
           {NdVar::reg(InputReg, InputWidth)});
      if (PointerSize > 4 && Variant != 6)
        Emit(Address, NdOp::INT_ZEXT, WideIndex, {Index});
      if (!Above && Variant == 1) {
        Emit(Address, NdOp::BRANCH, {}, {NdVar::cst(0x101, PointerSize)});
        auto &R = B.Insns.at(Address);
        R.IsBranch = true;
        R.BranchTarget = 0x101;
        return;
      }
      const NdVar Bound = NdVar::cst(!Above && Variant == 4 ? 5 : 2, 4);
      const NdVar Less = NdVar::tmp(0x100, 1);
      const NdVar Difference = NdVar::tmp(0x108, 4);
      const NdVar Equal = NdVar::tmp(0x110, 1);
      const NdVar NotLess = NdVar::tmp(0x118, 1);
      const NdVar NotEqual = NdVar::tmp(0x120, 1);
      const NdVar Condition = NdVar::tmp(0x128, 1);
      Emit(Address, NdOp::INT_LESS, Less, {Index, Bound});
      Emit(Address, NdOp::INT_SUB, Difference,
           {!Above && Variant == 3 ? NdVar::reg(24, 4) : Index, Bound});
      Emit(Address, NdOp::INT_EQUAL, Equal, {Difference, NdVar::cst(0, 4)});
      if (Above) {
        Emit(Address, NdOp::BOOL_NOT, NotLess, {Less});
        Emit(Address, NdOp::BOOL_NOT, NotEqual, {Equal});
        Emit(Address, NdOp::BOOL_AND, Condition, {NotLess, NotEqual});
      } else {
        Emit(Address, NdOp::BOOL_OR, Condition, {Less, Equal});
      }
      const va_t Target = Above ? 0x300 : 0x101;
      Emit(Address, NdOp::COND_BR, {},
           {NdVar::cst(Target, PointerSize), Condition});
      auto &R = B.Insns.at(Address);
      R.IsBranch = true;
      R.IsCond = true;
      R.BranchTarget = Target;
    };
    Guard(0x100, 8, true);
    Guard(0x200, 16, false);
    if (Variant == 2)
      Emit(0x101, NdOp::COPY, NdVar::reg(0, 1), {NdVar::cst(255, 1)});
    const int UseSeq = static_cast<int>(B.Insns[0x101].Ops.size());
    Emit(0x101, NdOp::INDIR_BR, {}, {WideIndex});
    auto &Dispatch = B.Insns.at(0x101);
    Dispatch.IsBranch = true;
    Dispatch.IsIndirect = true;
    Dispatch.JumpTableTargets = {0x200, 0x300};
    for (va_t Address : {va_t{0x201}, va_t{0x300}}) {
      Emit(Address, NdOp::RETURN, {}, {NdVar::cst(99, 4)});
      B.Insns.at(Address).IsRet = true;
    }
    if (Variant == 7) {
      Dispatch.JumpTableTargets = {0x300};
      B.Insns.erase(0x200);
      B.Insns.erase(0x201);
      B.BlockStarts.erase(0x200);
      B.BlockStarts.erase(0x201);
    }
    Query Q;
    Q.Candidate = WideIndex;
    Q.UseAddr = 0x101;
    Q.UseSeq = UseSeq;
    Q.Relation = Relation::JoinedUnsignedFeasibleSet;
    Q.UnsignedUpperBound = 4;
    return Q;
  }

  static Query seedFrameModeChain(CFGBuilder &B, const BinaryImage &Image,
                                  unsigned Length) {
    Query Q = seedPredecessorLane(B, Image, Length);
    Q.Candidate = NdVar::reg(getTargetRegInfo(Image.Arch).StackPointer,
                             Image.getPointerSize());
    Q.Alternatives = {{Q.Candidate, 0x100, 0, false}};
    Q.Relation = Relation::SameCanonicalFrameAddress;
    return Q;
  }

  static void clobberFrameLane(CFGBuilder &B, const BinaryImage &Image,
                               va_t Address) {
    auto &Ops = B.Insns.at(Address).Ops;
    for (auto &Op : Ops)
      ++Op.Seq;
    LowOp Write;
    Write.Addr = Address;
    Write.Seq = 0;
    Write.Opcode = NdOp::COPY;
    Write.Output = NdVar::reg(getTargetRegInfo(Image.Arch).StackPointer,
                              Image.getPointerSize() / 2);
    Write.addInput(NdVar::cst(0, Write.Output.Size));
    Ops.insert(Ops.begin(), Write);
  }

  using RoleInfo = CFGBuilder::JumpTableInfo;
  static RoleInfo seedLocalLoadRole(CFGBuilder &B, const BinaryImage &Image,
                                    unsigned Noise, unsigned Variant = 0) {
    B.CurrentImg = &Image;
    B.CurrentFuncEntry = 0x100;
    B.JumpTableProofContextComplete = true;
    B.PersistentCFGRoots = {0x100};
    B.BlockStarts = {0x100};
    const NdVar Index = NdVar::reg(0, 8);
    auto Emit = [&](va_t Address, NdOp Opcode, const NdVar &Output,
                    std::initializer_list<NdVar> Inputs) -> LowOp & {
      auto &Rec = B.Insns[Address];
      Rec.Addr = Address;
      Rec.Size = 1;
      LowOp Op;
      Op.Addr = Address;
      Op.Seq = static_cast<int>(Rec.Ops.size());
      Op.Opcode = Opcode;
      Op.Output = Output;
      for (const NdVar &Input : Inputs)
        Op.addInput(Input);
      Rec.Ops.push_back(Op);
      return Rec.Ops.back();
    };
    Emit(0x100, NdOp::COPY, Index, {NdVar::scalar(7, 8)});
    for (unsigned I = 0; I < Noise; ++I)
      Emit(0x101 + I, NdOp::INT_ADD, NdVar::tmp(0x1000 + I * 8, 8),
           {NdVar::reg(8, 8), Index});
    const va_t ProducerAddr = 0x101 + Noise;
    const NdVar Address = NdVar::tmp(0x4000, 8);
    const LowOp Producer = Emit(ProducerAddr, NdOp::INT_ADD, Address,
                                {NdVar::address(0x4000, 8), Index});
    va_t LoadAddr = ProducerAddr;
    NdVar LoadAddress = Address;
    if (Variant == 1 || Variant == 2) {
      // Cross-instruction aliases use the full discovery path. Variant 2
      // has a local ADD, but its operands cannot authenticate the table base.
      ++LoadAddr;
      LoadAddress = NdVar::tmp(0x5000, 8);
      if (Variant == 1)
        Emit(LoadAddr, NdOp::COPY, LoadAddress, {Address});
      else
        Emit(LoadAddr, NdOp::INT_ADD, LoadAddress,
             {Address, NdVar::scalar(0, 8)});
    } else if (Variant == 3) {
      Emit(LoadAddr, NdOp::COPY, Address, {NdVar::reg(8, 8)});
    }
    const LowOp Load =
        Emit(LoadAddr, NdOp::LOAD, NdVar::tmp(0x6000, 8), {LoadAddress});
    Emit(LoadAddr, NdOp::RETURN, {}, {Load.Output});
    B.Insns.at(LoadAddr).IsRet = true;
    RoleInfo Info;
    Info.setBaseAddr(0x4000);
    Info.EntrySize = 8;
    Info.TableLoadAddr = Load.Addr;
    Info.TableLoadSeq = Load.Seq;
    CFGBuilder::JumpTableLoadRole Role;
    Role.Load = {Load.Output, Load.Addr, Load.Seq, true};
    Role.LoadWidth = 8;
    Role.AllowedBases = {0x4000};
    Role.Indices = {{Index, Producer.Addr, Producer.Seq, false}};
    Role.AddressIndex = Role.Indices.front();
    Role.AddressScale = 1;
    Info.LoadRoles = {Role};
    return Info;
  }
  static bool proveLoadRole(CFGBuilder &B, RoleInfo &Info, size_t &Budget,
                            bool &Complete) {
    return B.tableLoadAddressesMatchRole(Info, &Budget, &Complete);
  }

  static Query seedCountLane(CFGBuilder &B, const BinaryImage &Image,
                             NdOp Opcode, uint16_t InputSize,
                             uint16_t OutputSize, unsigned SliceOffset = 0,
                             unsigned CountInputs = 1) {
    seed(B, Image, 1);
    auto &Ops = B.Insns.at(0x100).Ops;
    Ops.clear();
    LowOp Count;
    Count.Addr = 0x100;
    Count.Seq = 0;
    Count.Opcode = Opcode;
    Count.Output = NdVar::tmp(0, OutputSize);
    for (unsigned I = 0; I < CountInputs; ++I)
      Count.addInput(NdVar::reg(I * 32, InputSize));
    Ops.push_back(Count);
    LowOp Slice;
    Slice.Addr = 0x100;
    Slice.Seq = 1;
    Slice.Opcode = NdOp::SUBBYTES;
    Slice.Output = NdVar::tmp(32, 1);
    Slice.addInput(Count.Output);
    Slice.addInput(NdVar::cst(SliceOffset, 4));
    Ops.push_back(Slice);
    B.Insns.at(0x101).Ops.front().Inputs[0] = Count.Output;

    Query Q;
    Q.Candidate = Count.Output;
    Q.UseAddr = 0x101;
    Q.UseSeq = 0;
    Q.Alternatives = {{Slice.Output, 0x100, Slice.Seq, true}};
    Q.AllowZeroExtension = true;
    return Q;
  }

  static Answer query(CFGBuilder &B, size_t Budget = 1000000,
                      unsigned Length = 3, uint32_t Depth = 0,
                      const std::vector<va_t> *Targets = nullptr) {
    CFGBuilder::JumpTableValueQuery Q;
    Q.Candidate = NdVar::tmp((Length - 1) * 8, 8);
    Q.UseAddr = 0x100 + Length;
    Q.UseSeq = 0;
    Q.Alternatives.push_back({NdVar::cst(7, 8), Q.UseAddr, 0, false});
    Answer A;
    A.Remaining = Budget;
    A.Values = B.tableValuesMatchAtUses({Q}, &A.Complete, &A.QueryComplete,
                                        0x100, Targets, &A.Remaining, 0,
                                        nullptr, nullptr, Depth);
    return A;
  }

  static std::shared_ptr<const void> token(const CFGBuilder &B) {
    return B.CachedResolverGraph;
  }

  static void clear(CFGBuilder &B) { B.CachedResolverGraph.reset(); }

  static std::array<size_t, 3> stats(const CFGBuilder &B) {
    return B.resolverValueQueryCacheStatsForTesting();
  }

  static Query valueQuery(unsigned Length = 3) {
    Query Q;
    Q.Candidate = NdVar::tmp((Length - 1) * 8, 8);
    Q.UseAddr = 0x100 + Length;
    Q.UseSeq = 0;
    Q.Alternatives.push_back({NdVar::cst(7, 8), Q.UseAddr, 0, false});
    return Q;
  }

  static Answer batch(CFGBuilder &B, const std::vector<Query> &Queries,
                      size_t Budget = 1000000, uint32_t Depth = 0,
                      size_t MatchLimit = 0, bool WantComplete = true,
                      bool WantQueryComplete = true, bool WantMasks = false) {
    Answer A;
    A.Remaining = Budget;
    A.Values = B.tableValuesMatchAtUses(
        Queries, WantComplete ? &A.Complete : nullptr,
        WantQueryComplete ? &A.QueryComplete : nullptr, InvalidVA, nullptr,
        &A.Remaining, MatchLimit, nullptr, WantMasks ? &A.Masks : nullptr,
        Depth);
    return A;
  }

  static Answer
  scopedBatch(CFGBuilder &B, Scope *ReadOnlyScope,
              const std::vector<Query> &Queries, size_t Budget = 1000000,
              uint32_t Depth = 0, const std::vector<va_t> *Targets = nullptr,
              const std::set<va_t> *SharedBranches = nullptr,
              const std::map<va_t, std::vector<va_t>> *Edges = nullptr,
              va_t Branch = 0x100, bool Budgeted = true) {
    Answer A;
    A.Remaining = Budget;
    A.Values = B.tableValuesMatchAtUses(
        Queries, &A.Complete, &A.QueryComplete, Branch, Targets,
        Budgeted ? &A.Remaining : nullptr, 0, SharedBranches, &A.Masks, Depth,
        Edges, true, ReadOnlyScope);
    return A;
  }

  static void seedOccurrences(CFGBuilder &B) {
    RelocatedInstructionAddressOccurrence Address;
    Address.InstructionAddr = 0x100;
    Address.OpSeq = 0;
    Address.ArithmeticProof.resize(1);
    B.RelocatedInstructionAddressOccurrences = {Address};
    RelocatedInstructionScalarModelOccurrence Scalar;
    Scalar.InstructionAddr = 0x100;
    Scalar.OpSeq = 0;
    B.RelocatedInstructionScalarModelOccurrences = {Scalar};
  }

  static void changeAddressOccurrence(CFGBuilder &B) {
    B.RelocatedInstructionAddressOccurrences.front().TargetOwnerVA = 0x200;
  }

  static void changeNestedAddressOccurrence(CFGBuilder &B) {
    B.RelocatedInstructionAddressOccurrences.front()
        .ArithmeticProof.front()
        .OpSeq = 1;
  }

  static void changeScalarOccurrence(CFGBuilder &B) {
    B.RelocatedInstructionScalarModelOccurrences.front().SeedOpSeq = 1;
  }

  static void changeEntry(CFGBuilder &B) { B.CurrentFuncEntry = 0x101; }

  static void changeConsumerAudit(CFGBuilder &B) {
    B.ActiveJumpTableConsumerAudit = true;
  }

  static void changeSymbolBudget(CFGBuilder &B) {
    B.FiniteSetSymbolEvidenceBudgetForTesting = 0;
  }

  static void changeImage(CFGBuilder &B, const BinaryImage &Image) {
    B.CurrentImg = &Image;
  }

  static void changeValue(CFGBuilder &B) {
    B.Insns.at(0x100).Ops.front().Inputs[0] = NdVar::cst(8, 8);
  }

  static void malformedScalar(CFGBuilder &B, unsigned Variant) {
    auto &Op = B.Insns.at(0x100).Ops.front();
    if (Variant == 0) {
      Op.Opcode = NdOp::INT_ADD;
      // Missing second input must not become an independent scalar root.
    } else if (Variant == 1) {
      Op.Opcode = NdOp::SELECT;
      Op.addInput(NdVar::scalar(7, 8));
    } else if (Variant == 2)
      Op.NumInputs = std::size(Op.Inputs) + 1;
    else if (Variant == 3)
      Op.Inputs[0].Size = 0;
    else if (Variant == 4)
      Op.Output.Size = 0;
    else
      Op.Inputs[0].Space = VnodeSpace::RAM;
  }

  static std::vector<Query> seedEntryMemory(CFGBuilder &B,
                                            const BinaryImage &Image,
                                            bool Initialize, bool Loop) {
    B.CurrentImg = &Image;
    B.CurrentFuncEntry = 0x100;
    B.JumpTableProofContextComplete = true;
    B.PersistentCFGRoots = {0x100};
    B.BlockStarts = {0x100, 0x103};
    const auto &TRI = getTargetRegInfo(Image.Arch);
    const auto Width = static_cast<uint16_t>(Image.getPointerSize());
    const auto SP = NdVar::reg(TRI.StackPointer, Width);
    const auto Address = NdVar::tmp(0x8000, Width);
    const auto Read = NdVar::reg(TRI.IntReturnReg, Width);
    auto Emit = [&](va_t Addr, NdOp Code, NdVar Output,
                    std::initializer_list<NdVar> Inputs) -> LowOp & {
      auto &Rec = B.Insns[Addr];
      Rec.Addr = Addr;
      Rec.Size = 1;
      LowOp Op;
      Op.Addr = Addr;
      Op.Seq = static_cast<int>(Rec.Ops.size());
      Op.Opcode = Code;
      Op.Output = Output;
      for (const auto &Input : Inputs)
        Op.addInput(Input);
      Rec.Ops.push_back(Op);
      return Rec.Ops.back();
    };
    auto SlotAddress = [&](va_t Addr) {
      Emit(Addr, NdOp::INT_ADD, Address,
           {SP, NdVar::scalar(uint64_t{0} - Width, Width)});
    };
    SlotAddress(0x100);
    if (Initialize)
      Emit(0x100, NdOp::STORE, {}, {Address, NdVar::scalar(7, Width)});
    const int LoadSeq = Emit(0x100, NdOp::LOAD, Read, {Address}).Seq;
    Emit(0x100, NdOp::INT_AND, Read, {Read, NdVar::scalar(3, Width)});
    const int UseSeq = Emit(0x100, NdOp::NOP, {}, {}).Seq;
    SlotAddress(0x101);
    const int StoreSeq =
        Emit(0x101, NdOp::STORE, {}, {Address, NdVar::scalar(7, Width)}).Seq;
    if (Loop) {
      Emit(0x102, NdOp::COND_BR, {},
           {NdVar::address(0x100, Width), NdVar::reg(TRI.FlagZF, 1)});
      B.Insns.at(0x102).IsBranch = B.Insns.at(0x102).IsCond = true;
      B.Insns.at(0x102).BranchTarget = 0x100;
    } else
      Emit(0x102, NdOp::NOP, {}, {});
    Emit(0x103, NdOp::RETURN, {}, {});
    B.Insns.at(0x103).IsRet = true;

    Query Memory;
    Memory.Candidate = SP;
    Memory.UseAddr = 0x100;
    Memory.UseSeq = LoadSeq;
    Memory.FrameByteAddend = -Width;
    Memory.FrameMemorySize = Width;
    Memory.Relation = Relation::FrameMemoryMatches;
    Memory.Alternatives = {{NdVar::scalar(7, Width), InvalidVA, -1, false}};
    Memory.AlternativeFrameValueOffsets = {0};
    Query Writer = Memory;
    Writer.Relation = Relation::AuthenticatedFrameMemory;
    Writer.Alternatives.clear();
    Writer.AlternativeFrameValueOffsets.clear();
    Writer.AuthenticatedFrameStoreWriters = {
        {NdVar::scalar(7, Width), 0x101, StoreSeq, false}};
    if (Initialize)
      Writer.AuthenticatedFrameStoreWriters.push_back(
          {NdVar::scalar(7, Width), 0x100, LoadSeq - 1, false});
    Query Numeric;
    Numeric.Candidate = Read;
    Numeric.UseAddr = 0x100;
    Numeric.UseSeq = UseSeq;
    Numeric.Relation = Relation::UnsignedLessThan;
    Numeric.UnsignedUpperBound = 4;
    return {Memory, Writer, Numeric};
  }

  static void addBackedge(CFGBuilder &B) {
    auto &R = B.Insns.at(0x103);
    R.IsRet = false;
    R.IsBranch = true;
    R.BranchTarget = 0x100;
    R.Ops.front().Opcode = NdOp::BRANCH;
    R.Ops.front().Inputs[0] = NdVar::cst(0x100, 8);
  }

  static void changeRoot(CFGBuilder &B) {
    B.ActiveJumpTableProofRoots = std::set<va_t>{0x101};
  }

  static void resetRoot(CFGBuilder &B) { B.ActiveJumpTableProofRoots.reset(); }

  static void changeConditionalRoot(CFGBuilder &B) {
    B.DiscoveredCodeRefSources[0x102] = {0x100};
  }

  static void addOwner(CFGBuilder &B) {
    B.Insns.at(0x100).JumpTableTargets = {0x101};
    auto &Info = B.ResolvedTableInfo[0x100];
    Info.setBaseAddr(0x102);
    Info.EntrySize = 1;
    Info.MaxEntries = 1;
    B.DiscoveredCodeRefSources[0x102] = {0x100};
  }

  static void changeOwnerRange(CFGBuilder &B) {
    B.ResolvedTableInfo.at(0x100).BaseAddr = 0x103;
  }

  static void changeOwnerTargets(CFGBuilder &B) {
    B.Insns.at(0x100).JumpTableTargets.clear();
  }

  static void explicitOwnerRange(CFGBuilder &B) {
    B.ResolvedTableInfo.at(0x100).StorageRanges = {{0x102, 1, 1, 1}};
  }

  static void rollbackValue(CFGBuilder &B) {
    B.Insns.at(0x100).Ops.front().Inputs[0] = NdVar::cst(7, 8);
  }

  struct ConditionAnswer {
    std::vector<std::optional<bool>> Values;
    bool Complete = false;
    size_t Remaining = 0;
    bool operator==(const ConditionAnswer &) const = default;
  };

  static void seedCondition(CFGBuilder &B, const BinaryImage &Image) {
    seed(B, Image);
    auto &Guard = B.Insns.at(0x100);
    Guard.IsBranch = Guard.IsCond = true;
    Guard.BranchTarget = 0x102;
    Guard.Ops.clear();
    LowOp Branch;
    Branch.Addr = Guard.Addr;
    Branch.Opcode = NdOp::COND_BR;
    Branch.addInput(NdVar::cst(0x102, 8));
    Branch.addInput(NdVar::reg(0, 1));
    Guard.Ops.push_back(Branch);
    auto &Exit = B.Insns.at(0x101);
    Exit.IsRet = true;
    Exit.Ops.front().Opcode = NdOp::RETURN;
  }

  static ConditionAnswer conditions(
      CFGBuilder &B, size_t Budget = 1000000, va_t Load = 0x102,
      std::vector<va_t> Branches = {0x100},
      const std::map<va_t, std::vector<va_t>> *CertifiedEdges = nullptr) {
    ConditionAnswer A;
    A.Remaining = Budget;
    CFGBuilder::JumpTableInfo Info;
    Info.TableLoadAddr = Load;
    A.Values = B.tableLoadConditionValues(Branches, Info, &A.Complete,
                                          &A.Remaining, CertifiedEdges);
    return A;
  }

  static void reverseCondition(CFGBuilder &B) {
    auto &Guard = B.Insns.at(0x100);
    Guard.BranchTarget = 0x101;
    Guard.Ops.front().Inputs[0] = NdVar::cst(0x101, 8);
  }

  static va_t nodeAddress(size_t Node) { return 0x100 + 2 * Node; }

  struct RoleReplayFixture {
    RoleInfo Info;
    std::map<va_t, std::vector<va_t>> Hypothesis{{0x101, {0x103, 0x200}}};
    std::map<va_t, std::vector<va_t>> Restricted{{0x101, {0x103}}};
  };
  static RoleReplayFixture seedRoleReplay(CFGBuilder &B,
                                          const BinaryImage &Image) {
    B.CurrentImg = &Image;
    B.CurrentFuncEntry = 0x100;
    B.JumpTableProofContextComplete = true;
    B.PersistentCFGRoots = {0x100};
    B.ActiveJumpTableProofRoots = B.PersistentCFGRoots;
    B.BlockStarts = {0x100, 0x101, 0x103, 0x200, 0x201};
    const auto Index = NdVar::reg(0, 8);
    const auto Address = NdVar::reg(0x40, 8);
    const auto Target = NdVar::reg(0x48, 8);
    auto Emit = [&](va_t Addr, NdOp Code, NdVar Output,
                    std::initializer_list<NdVar> Inputs) -> LowOp & {
      auto &Rec = B.Insns[Addr];
      Rec.Addr = Addr;
      Rec.Size = 1;
      LowOp Op;
      Op.Addr = Addr;
      Op.Seq = static_cast<int>(Rec.Ops.size());
      Op.Opcode = Code;
      Op.Output = Output;
      for (const auto &Input : Inputs)
        Op.addInput(Input);
      Rec.Ops.push_back(Op);
      return Rec.Ops.back();
    };
    Emit(0x100, NdOp::COPY, Index, {NdVar::scalar(7, 8)});
    Emit(0x100, NdOp::INT_ADD, Address, {NdVar::address(0x4000, 8), Index});
    Emit(0x101, NdOp::LOAD, Target, {Address});
    Emit(0x101, NdOp::INDIR_BR, {}, {NdVar::reg(0x50, 8)});
    B.Insns.at(0x101).IsBranch = B.Insns.at(0x101).IsIndirect = true;
    Emit(0x103, NdOp::INDIR_BR, {}, {Target});
    B.Insns.at(0x103).IsBranch = B.Insns.at(0x103).IsIndirect = true;
    // The extra H edge may clobber the LOAD address before another iteration
    // and may reach the final branch without the authenticated target value.
    Emit(0x200, NdOp::COPY, Address, {NdVar::scalar(0x5000, 8)});
    Emit(0x200, NdOp::COPY, Target, {NdVar::scalar(0, 8)});
    Emit(0x200, NdOp::COND_BR, {},
         {NdVar::address(0x101, 8),
          NdVar::reg(getTargetRegInfo(Image.Arch).FlagZF, 1)});
    Emit(0x201, NdOp::BRANCH, {}, {NdVar::address(0x103, 8)});
    B.Insns.at(0x201).IsBranch = true;
    B.Insns.at(0x201).BranchTarget = 0x103;
    B.Insns.at(0x200).IsBranch = B.Insns.at(0x200).IsCond = true;
    B.Insns.at(0x200).BranchTarget = 0x101;
    RoleReplayFixture Fixture;
    auto &Info = Fixture.Info;
    Info.setBaseAddr(0x4000);
    Info.EntrySize = 8;
    Info.RelocAbsolute = true;
    Info.IndexValueAtUse = Index;
    Info.IndexUseAddr = 0x100;
    Info.IndexUseSeq = 1;
    Info.TableLoadAddr = 0x101;
    Info.TableLoadSeq = 0;
    CFGBuilder::JumpTableLoadRole Role;
    Role.Load = {Target, 0x101, 0, true};
    Role.LoadWidth = 8;
    Role.AllowedBases = {0x4000};
    Role.Indices = {{Index, 0x100, 1, false}};
    Role.AddressScale = 1;
    Info.LoadRoles = {Role};
    Info.TargetLoads = {Role.Load};
    CFGBuilder::GuardedJumpTableGroupProofContext Context;
    Context.Roots = B.PersistentCFGRoots;
    Context.Edges = Fixture.Hypothesis;
    B.GuardedGroupProofContext = Context;
    return Fixture;
  }
  static Answer replayRoles(CFGBuilder &B, RoleInfo Info,
                            const std::map<va_t, std::vector<va_t>> &Edges,
                            bool UseGroupContext) {
    Answer Result;
    Result.Remaining = 1000000;
    bool AddressComplete = false;
    bool TargetComplete = false;
    const bool Address =
        B.tableLoadAddressesMatchRole(Info, &Result.Remaining, &AddressComplete,
                                      true, &Edges, nullptr, UseGroupContext);
    const bool Target = B.branchTargetDependsOnTableLoad(
        B.Insns.at(0x103), Info, &Result.Remaining, &TargetComplete, true,
        &Edges, UseGroupContext);
    Result.Values = {Address, Target};
    Result.QueryComplete = {AddressComplete, TargetComplete};
    Result.Complete = AddressComplete && TargetComplete;
    return Result;
  }

  struct FiniteControlFixture {
    std::map<va_t, std::vector<va_t>> Members{{0x163, {0x180, 0x190}},
                                              {0x173, {0x180, 0x190}}};
    std::set<va_t> Roots{0x100};
    std::vector<Query> Selectors;
  };

  static FiniteControlFixture seedFiniteControl(CFGBuilder &B,
                                                const BinaryImage &Image) {
    B.CurrentImg = &Image;
    B.CurrentFuncEntry = 0x100;
    B.JumpTableProofContextComplete = true;
    B.PersistentCFGRoots = {0x100};
    B.BlockStarts = {0x100, 0x120, 0x140, 0x160, 0x170, 0x180, 0x190};
    const auto EAX = NdVar::reg(0, 4);
    const auto ECX = NdVar::reg(8, 4);
    const auto EBX = NdVar::reg(24, 4);
    const auto ESP = NdVar::reg(32, 4);
    const auto EDI = NdVar::reg(56, 4);
    auto Emit = [&](va_t Addr, uint16_t Size, NdOp Code, NdVar Output,
                    std::initializer_list<NdVar> Inputs) -> LowOp & {
      auto &Rec = B.Insns[Addr];
      Rec.Addr = Addr;
      Rec.Size = Size;
      LowOp Op;
      Op.Addr = Addr;
      Op.Seq = static_cast<int>(Rec.Ops.size());
      Op.Opcode = Code;
      Op.Output = Output;
      for (const auto &Input : Inputs)
        Op.addInput(Input);
      Rec.Ops.push_back(Op);
      return Rec.Ops.back();
    };
    auto Direct = [&](va_t Addr, va_t Target) {
      Emit(Addr, 2, NdOp::BRANCH, {}, {NdVar::address(Target, 4)});
      B.Insns.at(Addr).IsBranch = true;
      B.Insns.at(Addr).BranchTarget = Target;
    };
    Emit(0x100, 3, NdOp::INT_SUB, ESP, {ESP, NdVar::scalar(4, 4)});
    Emit(0x103, 7, NdOp::STORE, {}, {ESP, NdVar::scalar(0, 4)});
    Emit(0x10a, 2, NdOp::INDIR_BR, {}, {ECX});
    B.Insns.at(0x10a).IsBranch = B.Insns.at(0x10a).IsIndirect = true;
    B.Insns.at(0x10a).JumpTableTargets = {0x120};
    Emit(0x120, 2, NdOp::COPY, EAX, {EDI});
    Emit(0x122, 3, NdOp::INT_AND, EAX, {EAX, NdVar::scalar(1, 4)});
    Emit(0x125, 3, NdOp::STORE, {}, {ESP, EAX});
    Direct(0x128, 0x160);
    Emit(0x140, 7, NdOp::STORE, {}, {ESP, NdVar::scalar(2, 4)});
    Direct(0x147, 0x160);
    FiniteControlFixture Fixture;
    for (va_t Load : {0x160, 0x170}) {
      Emit(Load, 3, NdOp::LOAD, EAX, {ESP});
      const va_t Branch = Load + 3;
      const auto Offset = NdVar::tmp(0x8000, 4);
      const auto Address = NdVar::tmp(0x8004, 4);
      const auto Target = NdVar::tmp(0x8008, 4);
      Emit(Branch, 7, NdOp::INT_MULT, Offset, {EAX, NdVar::scalar(4, 4)});
      Emit(Branch, 7, NdOp::INT_ADD, Address, {EBX, Offset});
      Emit(Branch, 7, NdOp::LOAD, Target, {Address});
      Emit(Branch, 7, NdOp::INDIR_BR, {}, {Target});
      B.Insns.at(Branch).IsBranch = B.Insns.at(Branch).IsIndirect = true;
      Query Q;
      Q.Candidate = EAX;
      Q.UseAddr = Branch;
      Q.UseSeq = 0;
      Q.Relation = Relation::UnsignedFeasibleSet;
      Q.UnsignedUpperBound = 2;
      Fixture.Selectors.push_back(Q);
    }
    Direct(0x180, 0x170);
    Emit(0x190, 3, NdOp::INT_ADD, ESP, {ESP, NdVar::scalar(4, 4)});
    Emit(0x193, 1, NdOp::RETURN, {}, {});
    B.Insns.at(0x193).IsBranch = B.Insns.at(0x193).IsIndirect = true;
    B.Insns.at(0x193).IsRet = true;
    return Fixture;
  }

  static void makeFiniteControlDirect(CFGBuilder &B) {
    auto &Rec = B.Insns.at(0x10a);
    Rec.IsIndirect = false;
    Rec.BranchTarget = 0x120;
    Rec.JumpTableTargets.clear();
    Rec.Ops.front().Opcode = NdOp::BRANCH;
    Rec.Ops.front().Inputs[0] = NdVar::address(0x120, 4);
  }

  static void exposeFiniteControlStore(CFGBuilder &B) {
    B.Insns.at(0x10a).JumpTableTargets = {0x120, 0x140};
  }
  static void modelFiniteControlInteriorRoot(CFGBuilder &B) {
    B.BlockStarts.insert(0x103);
  }
  static void omitFiniteControlConditionalRoot(CFGBuilder &B) {
    B.Insns.erase(0x140);
    B.Insns.erase(0x147);
    B.DiscoveredCodeRefSources[0x140] = {0x120};
  }
  static void hideFiniteControlBranchBeforeLast(CFGBuilder &B,
                                                FiniteControlFixture &Fixture) {
    auto &Rec = B.Insns.at(0x103);
    Rec.IsBranch = Rec.IsIndirect = true;
    Fixture.Members.emplace(0x103, std::vector<va_t>{0x120});
  }

  struct ControlAnswer {
    std::set<va_t> Reachable;
    bool Complete = false;
    bool Closed = false;
    size_t Remaining = 0;
    bool operator==(const ControlAnswer &) const = default;
  };
  static ControlAnswer finiteControl(CFGBuilder &B,
                                     const FiniteControlFixture &Fixture,
                                     bool Strict = true,
                                     size_t Budget = 1000000) {
    ControlAnswer A;
    A.Remaining = Budget;
    // Also exercise clearing stale positive outputs on resource refusal.
    A.Complete = A.Closed = true;
    A.Reachable = B.candidateReachableInstructions(
        B.Insns.at(0x163), Fixture.Members.at(0x163), Fixture.Roots, {},
        &A.Remaining, &A.Complete, &Fixture.Members, &A.Closed,
        /*UseGroupContext=*/false, Strict);
    return A;
  }
  static Answer finiteSelectors(CFGBuilder &B,
                                const FiniteControlFixture &Fixture) {
    B.ActiveJumpTableProofRoots = Fixture.Roots;
    return scopedBatch(B, nullptr, Fixture.Selectors, 1000000, 0,
                       &Fixture.Members.at(0x163), nullptr, &Fixture.Members,
                       0x163);
  }
  static void malformedFiniteControlRoot(CFGBuilder &B,
                                         FiniteControlFixture &Fixture,
                                         unsigned Variant) {
    if (Variant == 0)
      Fixture.Roots.insert(0x999);
    else if (Variant == 1)
      Fixture.Roots.insert(0x101); // An interior instruction byte.
    else if (Variant == 2)
      Fixture.Roots.insert(0x103); // Real instruction, missing block boundary.
    else {
      Fixture.Roots.insert(0x120);
      if (Variant == 3)
        B.Insns.at(0x120).IsInstructionGuard = true;
      else if (Variant == 4)
        B.Insns.at(0x120).Size = 0;
      else
        ++B.Insns.at(0x120).Addr;
    }
  }

  using JointProof = CFGBuilder::FiniteJumpTableJointProof;

  static JointProof seedFiniteJointReuse(CFGBuilder &B,
                                         const BinaryImage &Image,
                                         RoleInfo &Info) {
    Info = seedLocalLoadRole(B, Image, 0);
    Info.EntrySize = 4;
    Info.RelocAbsolute = true;
    Info.PhysicalCapacity = 5;
    auto &Role = Info.LoadRoles.front();
    Role.LoadWidth = 4;
    Role.AddressScale = 4;
    Role.AddressIndex = {};
    for (auto &[Addr, Rec] : B.Insns) {
      (void)Addr;
      for (auto &Op : Rec.Ops) {
        if (Op.Output.Size)
          Op.Output.Size = 4;
        for (unsigned I = 0; I < Op.NumInputs; ++I)
          if (Op.Inputs[I].Size)
            Op.Inputs[I].Size = 4;
      }
    }
    Role.Load.Value.Size = 4;
    Role.Indices.front().Value.Size = 4;
    Info.IndexValueAtUse = Role.Indices.front().Value;
    Info.IndexUseAddr = Role.Indices.front().Addr;
    Info.IndexUseSeq = Role.Indices.front().Seq;
    Info.TargetLoads = {Role.Load};
    const auto Target = Role.Load.Value;
    auto &First = B.Insns.at(Role.Load.Addr);
    First.IsRet = false;
    First.IsBranch = First.IsIndirect = true;
    First.Ops.back().Opcode = NdOp::INDIR_BR;
    CFGBuilder::GuardedJumpTableGroupKey Key;
    Key.OwnerBegin = 0x4000;
    Key.OwnerEnd = 0x4028;
    Key.MemberCount = 4;
    Key.Kind = CFGBuilder::GuardedJumpTableGroupKind::FiniteAdjacentGOTOFF;
    CFGBuilder::GuardedJumpTableGroupProofContext Context;
    Context.Roots = B.PersistentCFGRoots;
    JointProof Proof;
    Proof.Roots = Context.Roots;
    for (unsigned I = 0; I < 4; ++I) {
      const va_t Branch = I ? 0x200 + I : Role.Load.Addr;
      if (I) {
        auto &Rec = B.Insns[Branch];
        Rec.Addr = Branch;
        Rec.Size = 1;
        Rec.IsBranch = Rec.IsIndirect = true;
        LowOp Op;
        Op.Addr = Branch;
        Op.Opcode = NdOp::INDIR_BR;
        Op.addInput(Target);
        Rec.Ops.push_back(Op);
      }
      Key.Members[I] = Branch;
      const auto &Op = B.Insns.at(Branch).Ops.back();
      Proof.Indices.emplace(Branch, Role.Indices.front());
      Proof.LoadRoles.emplace(Branch, Role);
      Proof.TargetUses.emplace(Branch,
                               Occurrence{Target, Op.Addr, Op.Seq, false});
      Proof.Domains.emplace(Branch, std::vector<uint32_t>{1, 4});
      Proof.PhysicalTargets.emplace(
          Branch, std::vector<va_t>{0x300, 0x301, 0x302, 0x303, 0x304});
      Proof.RestrictedEdges.emplace(Branch, std::vector<va_t>{0x301, 0x304});
      Context.Edges.emplace(Branch, Proof.PhysicalTargets.at(Branch));
      Context.EmptyEdges.emplace(Branch, std::vector<va_t>{});
    }
    Proof.HypothesisEdges = Context.Edges;
    B.GuardedGroupIdentity = Key;
    B.GuardedGroupProofContext = Context;
    size_t Remaining = 1000000;
    bool Incomplete = false;
    Proof.Source = B.freezeFiniteJumpTableSource(Remaining, Incomplete);
    EXPECT_FALSE(Incomplete);
    EXPECT_TRUE(Proof.Source);
    return Proof;
  }

  static bool reuseFiniteJoint(const CFGBuilder &B, const JointProof &Proof,
                               const RoleInfo &Info, size_t &Remaining,
                               bool &Incomplete) {
    const va_t Branch = B.GuardedGroupIdentity->Members.front();
    return B.finiteJumpTableJointProofMatches(Proof, B.Insns.at(Branch), Info,
                                              Proof.PhysicalTargets.at(Branch),
                                              Remaining, Incomplete);
  }
  static void mutateFiniteJointSource(CFGBuilder &B, unsigned Variant) {
    const va_t Branch = B.GuardedGroupIdentity->Members.front();
    switch (Variant) {
    case 0:
      ++B.Insns.begin()->second.Ops.front().Inputs[0].Offset;
      break;
    case 1:
      B.Insns.at(Branch).Ops.front().MemoryAddressSpace =
          NdMemoryAddressSpace::X86FS;
      break;
    case 2:
      ++B.Insns.at(Branch).Ops.back().Inputs[0].Offset;
      break;
    case 3:
      ++B.Insns.at(Branch).Ops.front().Seq;
      break;
    case 4:
      B.BlockStarts.erase(0x100);
      B.BlockStarts.insert(0x101);
      break;
    case 5:
      B.CurrentFuncEntry = 0x101;
      break;
    case 6:
      B.RelocatedInstructionScalarModelOccurrences.push_back({});
      break;
    case 7:
      B.RelocationCFGRootSources[0x300] = {0x4000};
      break;
    case 8:
      ++B.GuardedGroupIdentity->OwnerBegin;
      break;
    case 9:
      ++B.GuardedGroupIdentity->OwnerEnd;
      break;
    case 10:
      ++B.GuardedGroupIdentity->Members[1];
      break;
    case 11:
      std::swap(B.GuardedGroupIdentity->Members[1],
                B.GuardedGroupIdentity->Members[2]);
      break;
    }
  }

  static void seedControlGraph(CFGBuilder &B, const BinaryImage &Image,
                               const std::vector<std::array<int, 2>> &Edges,
                               const std::vector<int> &Roots) {
    B.CurrentImg = &Image;
    B.CurrentFuncEntry = nodeAddress(0);
    B.JumpTableProofContextComplete = true;
    for (int Root : Roots)
      B.PersistentCFGRoots.insert(nodeAddress(Root));
    for (size_t I = 0; I < Edges.size(); ++I) {
      CFGBuilder::InsnRecord R{};
      R.Addr = nodeAddress(I);
      R.Size = 2;
      LowOp Op;
      Op.Addr = R.Addr;
      if (I + 1 == Edges.size()) {
        R.IsRet = true;
        Op.Opcode = NdOp::RETURN;
        Op.addInput(NdVar::reg(0, 8));
        R.Ops.push_back(Op);
      } else {
        R.IsBranch = R.IsCond = true;
        R.BranchTarget = nodeAddress(Edges[I][0]);
        Op.Opcode = NdOp::COND_BR;
        Op.addInput(NdVar::cst(R.BranchTarget, 8));
        Op.addInput(NdVar::reg(0, 1));
        R.Ops.push_back(Op);
      }
      B.Insns.emplace(R.Addr, std::move(R));
      B.BlockStarts.insert(nodeAddress(I));
    }
  }
};

} // namespace neverd::detail

namespace {
using Access = detail::ResolverGraphCacheTestAccess;

BinaryImage image() {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  return Image;
}

void expectColdAnswer(CFGBuilder &B, const Access::Answer &Warm) {
  Access::clear(B);
  EXPECT_EQ(Access::query(B), Warm);
}

TEST(ResolverGuardPredicates, CompoundGuardsPreserveEveryJoinedInput) {
  for (Arch Target : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<unsigned>(Target));
    auto Image = image();
    Image.Arch = Target;
    Image.Bits = Target == Arch::X64 || Target == Arch::AArch64
                     ? Bitness::Bits64
                     : Bitness::Bits32;
    CFGBuilder B;
    const auto Q = Access::seedJoinedGuard(B, Image);
    const auto Answer = Access::batch(B, {Q}, 1000000, 0, 0, true, true, true);
    ASSERT_TRUE(Answer.Complete);
    EXPECT_EQ(Answer.Values, (std::vector<bool>{true}));
    EXPECT_EQ(Answer.Masks, (std::vector<uint64_t>{7}));
    EXPECT_EQ(Access::batch(B, {Q}, 1000000, 0, 0, true, true, true), Answer);
    auto Ordinary = Q;
    Ordinary.Relation = Access::Relation::UnsignedFeasibleSet;
    const std::vector<Access::Query> Mixed{Ordinary, Q, Ordinary, Q};
    const auto Modes = Access::batch(B, Mixed, 1000000, 0, 0, true, true, true);
    ASSERT_TRUE(Modes.Complete);
    EXPECT_EQ(Modes.Values, (std::vector<bool>{false, true, false, true}));
    EXPECT_EQ(Modes.Masks, (std::vector<uint64_t>{0, 7, 0, 7}));
    EXPECT_EQ(Access::batch(B, Mixed, 1000000, 0, 0, true, true, true), Modes);
  }
}

TEST(ResolverGuardPredicates, LogicalUnionCannotClaimPredecessorOwnership) {
  for (Arch Target : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<unsigned>(Target));
    auto Image = image();
    Image.Arch = Target;
    Image.Bits = Target == Arch::X64 || Target == Arch::AArch64
                     ? Bitness::Bits64
                     : Bitness::Bits32;
    CFGBuilder B;
    const auto Q = Access::seedJoinedGuard(B, Image, 7);
    const auto Answer = Access::batch(B, {Q}, 1000000, 0, 0, true, true, true);
    ASSERT_TRUE(Answer.Complete);
    EXPECT_EQ(Answer.Values, (std::vector<bool>{false}));
    EXPECT_EQ(Answer.Masks, (std::vector<uint64_t>{0}));
  }
}

TEST(ResolverGuardPredicates, UnsafePathsCannotBorrowAJoinedRange) {
  for (Arch Target : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    auto Image = image();
    Image.Arch = Target;
    Image.Bits = Target == Arch::X64 || Target == Arch::AArch64
                     ? Bitness::Bits64
                     : Bitness::Bits32;
    for (unsigned Variant : {1u, 2u, 3u, 4u, 5u, 6u}) {
      if (Variant == 6 && Image.getPointerSize() == 4)
        continue;
      SCOPED_TRACE(static_cast<unsigned>(Target));
      SCOPED_TRACE(Variant);
      CFGBuilder B;
      const auto Q = Access::seedJoinedGuard(B, Image, Variant);
      const auto Answer =
          Access::batch(B, {Q}, 1000000, 0, 0, true, true, true);
      EXPECT_EQ(Answer.Values, (std::vector<bool>{false}));
      EXPECT_EQ(Answer.Masks, (std::vector<uint64_t>{0}));
    }
  }
}

TEST(ResolverGuardPredicates, ResourceRefusalCannotPublishAPartialDomain) {
  auto Image = image();
  Image.Bits = Bitness::Bits64;
  CFGBuilder Warm;
  const auto Q = Access::seedJoinedGuard(Warm, Image);
  const auto Full = Access::batch(Warm, {Q}, 1000000, 0, 0, true, true, true);
  ASSERT_TRUE(Full.Complete);
  ASSERT_EQ(Full.Values, (std::vector<bool>{true}));
  const size_t Work = 1000000 - Full.Remaining;
  ASSERT_GT(Work, 0u);
  for (size_t Limit : {size_t{0}, size_t{1}, Work / 2, Work - 1, Work}) {
    SCOPED_TRACE(Limit);
    CFGBuilder Cold;
    Access::seedJoinedGuard(Cold, Image);
    const auto Answer = Access::batch(Cold, {Q}, Limit, 0, 0, true, true, true);
    EXPECT_EQ(Access::batch(Warm, {Q}, Limit, 0, 0, true, true, true), Answer);
    EXPECT_EQ(Answer.Complete, Limit == Work);
    if (Limit == Work) {
      EXPECT_EQ(Answer.Values, (std::vector<bool>{true}));
      EXPECT_EQ(Answer.Masks, (std::vector<uint64_t>{7}));
    } else {
      EXPECT_TRUE(std::none_of(Answer.Values.begin(), Answer.Values.end(),
                               [](bool Value) { return Value; }));
      EXPECT_TRUE(std::all_of(Answer.Masks.begin(), Answer.Masks.end(),
                              [](uint64_t Mask) { return Mask == 0; }));
    }
  }
  Access::changeSymbolBudget(Warm);
  const auto Refused =
      Access::batch(Warm, {Q}, 1000000, 0, 0, true, true, true);
  EXPECT_FALSE(Refused.Complete);
  EXPECT_EQ(Refused.Values, (std::vector<bool>{false}));
  EXPECT_EQ(Refused.Masks, (std::vector<uint64_t>{0}));
}

TEST(ResolverFrameModes, ValueModesDoNotRebuildIndependentFrameProofs) {
  auto Image = image();
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  CFGBuilder B;
  const auto Q = Access::seedFrameModeChain(B, Image, 40);
  std::vector<Access::Query> Queries(16, Q);
  constexpr size_t Budget = 1000000;
  const auto Uniform = Access::batch(B, Queries, Budget);
  ASSERT_TRUE(Uniform.Complete);
  ASSERT_EQ(Uniform.Values, std::vector<bool>(Queries.size(), true));
  const size_t UniformWork = Budget - Uniform.Remaining;
  for (size_t I = 0; I < Queries.size(); ++I) {
    Queries[I].UseDefinedAlternativesAsOccurrenceRoots = I & 1;
    Queries[I].AllowPrivateFrameMemoryAcrossCalls = I & 2;
    Queries[I].FoldScalarConstantOps = I & 4;
  }
  const auto Mixed = Access::batch(B, Queries, Budget);
  ASSERT_TRUE(Mixed.Complete);
  EXPECT_EQ(Mixed.Values, Uniform.Values);
  EXPECT_LT(Budget - Mixed.Remaining, UniformWork * 2);
  Access::clear(B);
  const auto Bounded = Access::batch(B, Queries, UniformWork * 2);
  EXPECT_TRUE(Bounded.Complete);
  EXPECT_EQ(Bounded.Values, Uniform.Values);
}

TEST(ResolverFrameModes, IncompleteFrameProofIsRetiredBeforeNextQuery) {
  auto Image = image();
  Image.Bits = Bitness::Bits64;
  CFGBuilder B;
  const auto Deep = Access::seedFrameModeChain(B, Image, 80);
  auto Entry = Deep;
  Entry.UseAddr = 0x100;
  Entry.UseDefinedAlternativesAsOccurrenceRoots = true;
  Entry.AllowPrivateFrameMemoryAcrossCalls = true;
  Entry.FoldScalarConstantOps = true;
  const auto Answer = Access::batch(B, {Deep, Entry}, 1000000, 16);
  EXPECT_FALSE(Answer.Complete);
  EXPECT_EQ(Answer.Values, (std::vector<bool>{false, true}));
  EXPECT_EQ(Answer.QueryComplete, (std::vector<bool>{false, true}));
}

TEST(ResolverFrameModes, ChangedStackLaneCannotBorrowAnEarlierFrameIdentity) {
  for (Arch Target : {Arch::X86, Arch::X64}) {
    auto Image = image();
    Image.Arch = Target;
    Image.Bits = Target == Arch::X86 ? Bitness::Bits32 : Bitness::Bits64;
    CFGBuilder B;
    const auto After = Access::seedFrameModeChain(B, Image, 40);
    Access::clobberFrameLane(B, Image, 0x114);
    auto Before = After;
    Before.UseAddr = 0x100;
    auto ChangedMode = After;
    ChangedMode.UseDefinedAlternativesAsOccurrenceRoots = true;
    ChangedMode.AllowPrivateFrameMemoryAcrossCalls = true;
    ChangedMode.FoldScalarConstantOps = true;
    const auto Answer = Access::batch(B, {Before, ChangedMode, Before});
    EXPECT_TRUE(Answer.Complete);
    EXPECT_EQ(Answer.Values, (std::vector<bool>{true, false, true}));
  }
}

TEST(ResolverFrameModes, UnsignedValueQueriesKeepTheirMasksAndFrameIdentity) {
  auto Image = image();
  Image.Bits = Bitness::Bits64;
  CFGBuilder B;
  const auto Frame = Access::seedFrameModeChain(B, Image, 40);
  auto Numeric = Frame;
  Numeric.Candidate = NdVar::reg(0, 8);
  Numeric.Relation = Access::Relation::UnsignedFeasibleSet;
  Numeric.UnsignedUpperBound = 8;
  Numeric.Alternatives.clear();
  const auto Answer = Access::batch(B, {Frame, Numeric, Frame, Numeric},
                                    1000000, 0, 0, true, true, true);
  EXPECT_TRUE(Answer.Complete);
  EXPECT_EQ(Answer.Values, (std::vector<bool>{true, true, true, true}));
  EXPECT_EQ(Answer.Masks, (std::vector<uint64_t>{0, 128, 0, 128}));
}

TEST(ResolverLocalAddress, UnrelatedArithmeticDoesNotMultiplyAddressQueries) {
  auto Image = image();
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  auto Work = [&](unsigned Noise) {
    CFGBuilder B;
    auto Info = Access::seedLocalLoadRole(B, Image, Noise);
    constexpr size_t Limit = 100000000;
    size_t Budget = Limit;
    bool Complete = false;
    EXPECT_TRUE(Access::proveLoadRole(B, Info, Budget, Complete));
    EXPECT_TRUE(Complete);
    EXPECT_NE(Info.LoadRoles.front().AddressIndex.Value.Size, 0u);
    return Limit - Budget;
  };
  const size_t Small = Work(64);
  const size_t Large = Work(256);
  EXPECT_LT(Large, Small * 5);
}

TEST(ResolverLocalAddress, FullDiscoverySurvivesMissingOrFailedLocalProof) {
  auto Image = image();
  Image.Bits = Bitness::Bits64;
  for (unsigned Variant : {1u, 2u}) {
    SCOPED_TRACE(Variant);
    CFGBuilder B;
    auto Info = Access::seedLocalLoadRole(B, Image, 4, Variant);
    size_t Budget = 1000000;
    bool Complete = false;
    EXPECT_TRUE(Access::proveLoadRole(B, Info, Budget, Complete));
    EXPECT_TRUE(Complete);
  }
}

TEST(ResolverLocalAddress, ClobberedProducerCannotAuthenticateTheLoad) {
  auto Image = image();
  Image.Bits = Bitness::Bits64;
  CFGBuilder B;
  auto Info = Access::seedLocalLoadRole(B, Image, 4, 3);
  const auto Original = Info.LoadRoles;
  size_t Budget = 1000000;
  bool Complete = false;
  EXPECT_FALSE(Access::proveLoadRole(B, Info, Budget, Complete));
  EXPECT_TRUE(Complete);
  EXPECT_EQ(Info.LoadRoles, Original);
}

TEST(ResolverLocalAddress, EveryBudgetBoundaryKeepsPublicationTransactional) {
  auto Image = image();
  Image.Bits = Bitness::Bits64;
  CFGBuilder B;
  auto Info = Access::seedLocalLoadRole(B, Image, 2);
  const auto OriginalInfo = Info;
  const auto Original = Info.LoadRoles;
  size_t Budget = 1000000;
  bool Complete = false;
  ASSERT_TRUE(Access::proveLoadRole(B, Info, Budget, Complete));
  ASSERT_TRUE(Complete);
  const size_t Work = 1000000 - Budget;
  ASSERT_LT(Work, 50000u);
  for (size_t Limit = 0; Limit <= Work; ++Limit) {
    CFGBuilder Cold;
    auto Candidate = Access::seedLocalLoadRole(Cold, Image, 2);
    size_t Remaining = Limit;
    bool Done = false;
    const bool Valid = Access::proveLoadRole(Cold, Candidate, Remaining, Done);
    auto WarmCandidate = OriginalInfo;
    size_t WarmRemaining = Limit;
    bool WarmDone = false;
    EXPECT_EQ(Access::proveLoadRole(B, WarmCandidate, WarmRemaining, WarmDone),
              Valid);
    EXPECT_EQ(WarmRemaining, Remaining);
    EXPECT_EQ(WarmDone, Done);
    EXPECT_EQ(WarmCandidate.LoadRoles, Candidate.LoadRoles);
    if (Limit == Work) {
      EXPECT_TRUE(Valid);
      EXPECT_TRUE(Done);
    } else {
      EXPECT_FALSE(Valid);
      EXPECT_FALSE(Done);
      EXPECT_EQ(Candidate.LoadRoles, Original);
    }
  }
}

TEST(ResolverConditionQueryCache, ReusesGraphAndPaysEveryColdBudgetBoundary) {
  const auto Image = image();
  CFGBuilder Warm;
  Access::seedCondition(Warm, Image);
  const auto First = Access::conditions(Warm);
  ASSERT_TRUE(First.Complete);
  ASSERT_EQ(First.Values, (std::vector<std::optional<bool>>{true}));
  const auto Token = Access::token(Warm);
  ASSERT_TRUE(Token);
  EXPECT_EQ(Access::conditions(Warm), First);
  EXPECT_EQ(Access::token(Warm), Token);

  const size_t Work = 1000000 - First.Remaining;
  ASSERT_LT(Work, 50000u);
  for (size_t Budget = 0; Budget <= Work + 1; ++Budget) {
    SCOPED_TRACE(Budget);
    CFGBuilder Cold;
    Access::seedCondition(Cold, Image);
    EXPECT_EQ(Access::conditions(Warm, Budget),
              Access::conditions(Cold, Budget));
  }
}

TEST(ResolverConditionQueryCache, BindsLoadAndOrderedQueries) {
  const auto Image = image();
  CFGBuilder B;
  Access::seedCondition(B, Image);
  const std::vector<va_t> Branches{0x100, InvalidVA, 0x102, 0x100};
  for (va_t Load : {va_t{0x102}, va_t{0x101}, va_t{0x100}, InvalidVA}) {
    const auto First = Access::conditions(B, 1000000, Load, Branches);
    EXPECT_EQ(First.Complete, Load != InvalidVA);
    EXPECT_EQ(Access::conditions(B, 1000000, Load, Branches), First);
    CFGBuilder Cold;
    Access::seedCondition(Cold, Image);
    EXPECT_EQ(Access::conditions(Cold, 1000000, Load, Branches), First);
    if (Load == 0x101 || Load == 0x102)
      EXPECT_EQ(First.Values,
                (std::vector<std::optional<bool>>{
                    Load == 0x102, std::nullopt, std::nullopt, Load == 0x102}));
    else
      EXPECT_EQ(First.Values,
                std::vector<std::optional<bool>>(Branches.size()));
  }
  const std::vector<va_t> Reversed{0x102, 0x100};
  EXPECT_EQ(Access::conditions(B, 1000000, 0x102, Reversed).Values,
            (std::vector<std::optional<bool>>{std::nullopt, true}));
}

TEST(ResolverConditionQueryCache, RevalidatesRootsBackedgesAndBranchOperands) {
  const auto Image = image();
  for (auto Mutate :
       {Access::addBackedge, Access::changeRoot, Access::changeConditionalRoot,
        Access::reverseCondition}) {
    CFGBuilder B;
    Access::seedCondition(B, Image);
    ASSERT_TRUE(Access::conditions(B).Complete);
    const auto Before = Access::token(B);
    Mutate(B);
    const auto Warm = Access::conditions(B);
    EXPECT_NE(Access::token(B), Before);
    Access::clear(B);
    EXPECT_EQ(Access::conditions(B), Warm);
    EXPECT_TRUE(Warm.Complete);
  }
}

TEST(ResolverConditionQueryCache, OwnsCertifiedEdgesAndRevalidatesStorage) {
  const auto Image = image();
  CFGBuilder B;
  Access::seedCondition(B, Image);
  std::map<va_t, std::vector<va_t>> Edges{{0x100, {0x102}}};
  const auto First = Access::conditions(B, 1000000, 0x102, {0x100}, &Edges);
  ASSERT_TRUE(First.Complete);
  const auto Original = Access::token(B);
  ASSERT_TRUE(Original);
  Edges.at(0x100).front() = 0x103;
  const auto Changed = Access::conditions(B, 1000000, 0x102, {0x100}, &Edges);
  EXPECT_NE(Access::token(B), Original);
  Access::clear(B);
  EXPECT_EQ(Access::conditions(B, 1000000, 0x102, {0x100}, &Edges), Changed);
  Edges.at(0x100).front() = 0x102;
  EXPECT_EQ(Access::conditions(B, 1000000, 0x102, {0x100}, &Edges), First);
  Edges.clear();
  for (auto Mutate : {Access::changeOwnerRange, Access::changeOwnerTargets,
                      Access::explicitOwnerRange}) {
    CFGBuilder Owner;
    Access::seedCondition(Owner, Image);
    Access::addOwner(Owner);
    ASSERT_TRUE(Access::conditions(Owner).Complete);
    const auto Before = Access::token(Owner);
    Mutate(Owner);
    const auto Warm = Access::conditions(Owner);
    EXPECT_NE(Access::token(Owner), Before);
    Access::clear(Owner);
    EXPECT_EQ(Access::conditions(Owner), Warm);
  }
}

TEST(ResolverConditionQueryCache, SharesExactGraphWithValueQueries) {
  const auto Image = image();
  CFGBuilder B;
  Access::seedCondition(B, Image);
  const auto Condition = Access::conditions(B);
  ASSERT_TRUE(Condition.Complete);
  const auto Graph = Access::token(B);
  ASSERT_TRUE(Graph);
  const auto Value = Access::query(B);
  EXPECT_EQ(Access::token(B), Graph);
  EXPECT_EQ(Access::conditions(B), Condition);
  EXPECT_EQ(Access::query(B), Value);
  Access::clear(B);
  EXPECT_EQ(Access::query(B), Value);
  EXPECT_EQ(Access::conditions(B), Condition);
}

TEST(ResolverConditionQueryCache,
     MatchesIndependentWalkOnCyclicMultiEntryGraphs) {
  const auto Image = image();
  // This oracle works on integer edges, independently of the production
  // snapshot/graph owner and cache. Include loops, irreducible regions,
  // disconnected nodes and exits outside the graph.
  uint32_t State = 0x918fa32b;
  for (unsigned Case = 0; Case < 128; ++Case) {
    SCOPED_TRACE(Case);
    std::vector<std::array<int, 2>> Edges(6);
    for (auto &Edge : Edges)
      for (int &Target : Edge) {
        State = State * 1664525u + 1013904223u;
        Target = (State >> 16) % 7;
      }
    // A decoded conditional branch's other edge is the next instruction.
    for (size_t I = 0; I < Edges.size(); ++I)
      Edges[I][1] = I + 1;
    std::vector<int> Roots{0};
    if (Case % 3 == 0)
      Roots.push_back(3);
    auto Reaches = [&](std::vector<int> Work, int Target, int Excluded) {
      std::set<int> Seen;
      while (!Work.empty()) {
        const int Node = Work.back();
        Work.pop_back();
        if (Node == Excluded || Node < 0 || Node >= int(Edges.size()) ||
            !Seen.insert(Node).second)
          continue;
        if (Node == Target)
          return true;
        if (Node + 1 != int(Edges.size()))
          Work.insert(Work.end(), Edges[Node].begin(), Edges[Node].end());
      }
      return false;
    };
    CFGBuilder B;
    Access::seedControlGraph(B, Image, Edges, Roots);
    std::vector<va_t> Branches;
    for (size_t I = 0; I < Edges.size(); ++I)
      Branches.push_back(Access::nodeAddress(I));
    for (int Load = 0; Load < int(Edges.size()); ++Load) {
      SCOPED_TRACE(Load);
      std::vector<std::optional<bool>> Expected(Edges.size());
      for (int Guard = 0; Guard + 1 < int(Edges.size()); ++Guard) {
        if (!Reaches(Roots, Load, -1) || Guard == Load ||
            Reaches(Roots, Load, Guard))
          continue;
        const bool True = Reaches({Edges[Guard][0]}, Load, Guard);
        const bool False = Reaches({Edges[Guard][1]}, Load, Guard);
        if (True != False)
          Expected[Guard] = True;
      }
      const auto Answer =
          Access::conditions(B, 1000000, Access::nodeAddress(Load), Branches);
      ASSERT_TRUE(Answer.Complete);
      EXPECT_EQ(Answer.Values, Expected);
      EXPECT_EQ(
          Access::conditions(B, 1000000, Access::nodeAddress(Load), Branches),
          Answer);
    }
  }
}

TEST(ResolverGraphCache, ReusesFrozenGraphAndPaysEveryColdBudgetBoundary) {
  const auto Image = image();
  CFGBuilder Warm;
  Access::seed(Warm, Image);
  const auto First = Access::query(Warm);
  ASSERT_EQ(First.Values, (std::vector<bool>{true}));
  ASSERT_TRUE(First.Complete);
  const auto Token = Access::token(Warm);
  ASSERT_TRUE(Token);
  EXPECT_EQ(Access::query(Warm), First);
  EXPECT_EQ(Access::token(Warm), Token);

  const size_t Work = 1000000 - First.Remaining;
  ASSERT_LT(Work, 50000u);
  for (size_t Budget = 0; Budget <= Work + 1; ++Budget) {
    SCOPED_TRACE(Budget);
    CFGBuilder Cold;
    Access::seed(Cold, Image);
    EXPECT_EQ(Access::query(Warm, Budget), Access::query(Cold, Budget));
  }
}

TEST(ResolverGraphCache, EqualCountsDoNotHideBackedgesRootsOrChangedOps) {
  const auto Image = image();
  for (auto Mutate : {Access::changeValue, Access::addBackedge,
                      Access::changeRoot, Access::changeConditionalRoot}) {
    CFGBuilder B;
    Access::seed(B, Image);
    ASSERT_TRUE(Access::query(B).Complete);
    const auto Before = Access::token(B);
    Mutate(B);
    const auto After = Access::query(B);
    EXPECT_NE(Access::token(B), Before);
    expectColdAnswer(B, After);
  }
}

TEST(ResolverGraphCache, StorageOwnershipAndCallbackChargesRemainCurrent) {
  const auto Image = image();
  for (auto Mutate : {Access::changeOwnerRange, Access::changeOwnerTargets,
                      Access::explicitOwnerRange}) {
    CFGBuilder B;
    Access::seed(B, Image);
    Access::addOwner(B);
    const auto First = Access::query(B);
    ASSERT_TRUE(First.Complete);
    const auto Before = Access::token(B);
    ASSERT_TRUE(Before);
    Mutate(B);
    const auto After = Access::query(B);
    EXPECT_NE(Access::token(B), Before);
    expectColdAnswer(B, After);
  }
}

TEST(ResolverGraphCache, OverridePayloadAndRolledBackPayloadAreOwned) {
  const auto Image = image();
  CFGBuilder B;
  Access::seed(B, Image);
  std::vector<va_t> Targets{0x101};
  const auto First = Access::query(B, 1000000, 3, 0, &Targets);
  const auto FirstToken = Access::token(B);
  Targets.front() = 0x102;
  const auto Changed = Access::query(B, 1000000, 3, 0, &Targets);
  EXPECT_NE(Access::token(B), FirstToken);
  Access::clear(B);
  EXPECT_EQ(Access::query(B, 1000000, 3, 0, &Targets), Changed);
  Targets.front() = 0x101;
  EXPECT_EQ(Access::query(B, 1000000, 3, 0, &Targets), First);

  Access::clear(B);
  const auto BeforeMutation = Access::query(B);
  const auto Frozen = Access::token(B);
  Access::changeValue(B);
  EXPECT_NE(Access::query(B).Values, BeforeMutation.Values);
  EXPECT_NE(Access::token(B), Frozen);
  Access::rollbackValue(B);
  EXPECT_EQ(Access::query(B), BeforeMutation);
}

TEST(ResolverGraphCache, ResourceFailureAndAnalysisIncompleteAreDistinct) {
  const auto Image = image();
  CFGBuilder B;
  Access::seed(B, Image, 16);
  const auto Exhausted = Access::query(B, 1, 16);
  EXPECT_FALSE(Exhausted.Complete);
  EXPECT_EQ(Exhausted.Remaining, 0u);
  EXPECT_FALSE(Access::token(B));

  const auto Incomplete = Access::query(B, 1000000, 16, 1);
  EXPECT_FALSE(Incomplete.Complete);
  EXPECT_GT(Incomplete.Remaining, 0u);
  const auto Graph = Access::token(B);
  ASSERT_TRUE(Graph);
  EXPECT_EQ(Access::query(B, 1000000, 16, 1), Incomplete);
  const auto Complete = Access::query(B, 1000000, 16);
  EXPECT_TRUE(Complete.Complete);
  EXPECT_EQ(Complete.Values, (std::vector<bool>{true}));
  EXPECT_EQ(Access::token(B), Graph);
}

TEST(ResolverQueryScope, ReusesValidationAndPreservesEveryBudgetBoundary) {
  const auto Image = image();
  CFGBuilder Warm;
  Access::seed(Warm, Image);
  Access::Scope Scope(Warm);
  const auto Good = Access::valueQuery();
  auto Bad = Good;
  Bad.Alternatives.front().Value = NdVar::cst(8, 8);
  const std::vector<Access::Query> Queries{Good, Bad};
  const auto First = Access::scopedBatch(Warm, &Scope, Queries);
  ASSERT_EQ(First.Values, (std::vector<bool>{true, false}));
  ASSERT_TRUE(First.Complete);
  const auto State = Scope.State;
  ASSERT_TRUE(State);
  EXPECT_EQ(Access::scopedBatch(Warm, &Scope, Queries), First);
  EXPECT_EQ(Scope.State, State);

  const size_t Work = 1000000 - First.Remaining;
  ASSERT_LT(Work, 50000u);
  for (size_t Budget = 0; Budget <= Work + 1; ++Budget) {
    SCOPED_TRACE(Budget);
    CFGBuilder Cold;
    Access::seed(Cold, Image);
    EXPECT_EQ(Access::scopedBatch(Warm, &Scope, Queries, Budget),
              Access::scopedBatch(Cold, nullptr, Queries, Budget));
  }
  EXPECT_EQ(Scope.State, State);
}

TEST(ResolverQueryScope, OwnsTemporaryOverridesAndRechecksTheirContents) {
  const auto Image = image();
  CFGBuilder B;
  Access::seed(B, Image);
  Access::Scope Scope(B);
  const std::vector<Access::Query> Queries{Access::valueQuery()};
  std::vector<va_t> Targets{0x101};
  std::set<va_t> Shared{0x100};
  std::map<va_t, std::vector<va_t>> Edges{{0x101, {0x102}}};
  const auto Check = [&](const std::vector<va_t> *T, const std::set<va_t> *S,
                         const std::map<va_t, std::vector<va_t>> *E,
                         va_t Branch = 0x100) {
    const auto Result =
        Access::scopedBatch(B, &Scope, Queries, 1000000, 0, T, S, E, Branch);
    Access::clear(B);
    EXPECT_EQ(Result, Access::scopedBatch(B, nullptr, Queries, 1000000, 0, T, S,
                                          E, Branch));
    const auto State = Scope.State;
    EXPECT_TRUE(State);
    EXPECT_EQ(Result, Access::scopedBatch(B, &Scope, Queries, 1000000, 0, T, S,
                                          E, Branch));
    EXPECT_EQ(Scope.State, State);
    return State;
  };
  auto Previous = Check(&Targets, &Shared, &Edges);
  Targets.front() = 0x102;
  EXPECT_NE(Check(&Targets, &Shared, &Edges), Previous);
  Previous = Scope.State;
  Targets.front() = 0x101;
  EXPECT_NE(Check(&Targets, &Shared, &Edges), Previous);
  Previous = Scope.State;
  Shared = {0x101};
  EXPECT_NE(Check(&Targets, &Shared, &Edges), Previous);
  Previous = Scope.State;
  Edges.begin()->second.front() = 0x103;
  EXPECT_NE(Check(&Targets, &Shared, &Edges), Previous);
  Previous = Scope.State;
  EXPECT_NE(Check(&Targets, &Shared, &Edges, 0x102), Previous);
  Previous = Scope.State;
  Edges.clear();
  EXPECT_NE(Check(&Targets, &Shared, &Edges), Previous);
  Previous = Scope.State;
  EXPECT_NE(Check(&Targets, &Shared, nullptr), Previous);

  {
    const std::vector<va_t> Temporary{0x101};
    Check(&Temporary, nullptr, nullptr);
  }
  const auto Owned = Scope.State;
  const std::vector<va_t> Equal{0x101};
  EXPECT_EQ(Check(&Equal, nullptr, nullptr), Owned);
}

TEST(ResolverQueryScope, RootsOwnersAndNewInvocationsCannotBorrowValidation) {
  const auto Image = image();
  CFGBuilder B;
  CFGBuilder Other;
  Access::seed(B, Image);
  Access::seed(Other, Image);
  Access::changeValue(Other);
  const std::vector<Access::Query> Queries{Access::valueQuery()};
  {
    Access::Scope Scope(B);
    ASSERT_TRUE(Access::scopedBatch(B, &Scope, Queries).Values.front());
    const auto Saved = Scope.State;
    const auto OtherResult = Access::scopedBatch(Other, &Scope, Queries);
    EXPECT_EQ(OtherResult.Values, (std::vector<bool>{false}));
    EXPECT_EQ(OtherResult, Access::scopedBatch(Other, nullptr, Queries));
    EXPECT_EQ(Scope.State, Saved);
    Access::changeRoot(B);
    const auto WithRoot = Access::scopedBatch(B, &Scope, Queries);
    EXPECT_NE(Scope.State, Saved);
    Access::clear(B);
    EXPECT_EQ(WithRoot, Access::scopedBatch(B, nullptr, Queries));
  }
  // Equal-sized instruction and storage mutations happen only between const
  // invocations; a new invocation must validate the builder's cached graph.
  Access::resetRoot(B);
  for (auto Mutate : {Access::changeValue, Access::rollbackValue,
                      Access::addBackedge, Access::changeConditionalRoot,
                      Access::addOwner, Access::changeOwnerRange,
                      Access::changeOwnerTargets, Access::explicitOwnerRange}) {
    Mutate(B);
    Access::Scope Scope(B);
    const auto Result = Access::scopedBatch(B, &Scope, Queries);
    Access::clear(B);
    EXPECT_EQ(Result, Access::scopedBatch(B, nullptr, Queries));
  }
}

TEST(ResolverQueryScope, NestedQueriesKeepTheirOwnGraphAlive) {
  const auto Image = image();
  CFGBuilder B;
  Access::seed(B, Image);
  const std::vector<Access::Query> Queries{Access::valueQuery()};
  Access::Scope Outer(B);
  const auto Original = Access::scopedBatch(B, &Outer, Queries);
  const auto Graph = Access::token(B);
  const auto State = Outer.State;
  {
    Access::Scope Inner(B);
    const std::vector<va_t> Targets{0x102};
    const auto Nested =
        Access::scopedBatch(B, &Inner, Queries, 1000000, 0, &Targets);
    EXPECT_NE(Access::token(B), Graph);
    EXPECT_EQ(Nested,
              Access::scopedBatch(B, nullptr, Queries, 1000000, 0, &Targets));
  }
  Access::clear(B);
  EXPECT_EQ(Access::scopedBatch(B, &Outer, Queries), Original);
  EXPECT_EQ(Outer.State, State);
  EXPECT_EQ(Access::token(B), Graph);
}

TEST(ResolverQueryScope, RefusalsAndOversizedKeysKeepTheOrdinaryPath) {
  const auto Image = image();
  CFGBuilder B;
  Access::seed(B, Image, 16);
  const std::vector<Access::Query> Queries{Access::valueQuery(16)};
  Access::Scope Scope(B);
  const auto Exhausted = Access::scopedBatch(B, &Scope, Queries, 1);
  EXPECT_FALSE(Exhausted.Complete);
  EXPECT_FALSE(Scope.State);
  const auto Refusal = Access::scopedBatch(B, &Scope, Queries, 1000000, 1);
  EXPECT_FALSE(Refusal.Complete);
  EXPECT_GT(Refusal.Remaining, 0u);
  const auto State = Scope.State;
  ASSERT_TRUE(State);
  EXPECT_EQ(Refusal, Access::scopedBatch(B, &Scope, Queries, 1000000, 1));
  const auto Complete = Access::scopedBatch(B, &Scope, Queries);
  EXPECT_TRUE(Complete.Complete);
  EXPECT_EQ(Scope.State, State);

  // An override for an absent instruction has no graph payload, but its key
  // still exceeds the scope's independent retention allowance.
  std::map<va_t, std::vector<va_t>> Large{{0xffff, std::vector<va_t>(9000)}};
  const auto Oversized = Access::scopedBatch(B, &Scope, Queries, 1000000, 0,
                                             nullptr, nullptr, &Large);
  EXPECT_FALSE(Scope.State);
  EXPECT_EQ(Oversized, Access::scopedBatch(B, nullptr, Queries, 1000000, 0,
                                           nullptr, nullptr, &Large));
  EXPECT_EQ(Access::scopedBatch(B, &Scope, Queries, 1000000, 0, nullptr,
                                nullptr, nullptr, 0x100, false),
            Access::scopedBatch(B, nullptr, Queries, 1000000, 0, nullptr,
                                nullptr, nullptr, 0x100, false));
  EXPECT_FALSE(Scope.State);
}

TEST(ResolverValueQueryCache, ExpandedValueDepthKeepsTheDefaultGuardCap) {
  const auto Image = image();
  for (unsigned Length : {80u, 160u}) {
    SCOPED_TRACE(Length);
    CFGBuilder Builder;
    const auto Query = Access::seedPredecessorLane(Builder, Image, Length);
    const auto Default = Access::batch(Builder, {Query});
    EXPECT_FALSE(Default.Complete);
    EXPECT_EQ(Default.QueryComplete, (std::vector<bool>{false}));
    EXPECT_EQ(Default.Values, (std::vector<bool>{false}));
    EXPECT_GT(Default.Remaining, 0u); // Depth, not aggregate work exhaustion.
    const auto Expanded = Access::batch(
        Builder, {Query}, 1000000, limits::kMaxJumpTableExpandedResolverDepth);
    EXPECT_EQ(Expanded.Complete, Length == 80);
    EXPECT_EQ(Expanded.QueryComplete, (std::vector<bool>{Length == 80}));
    EXPECT_EQ(Expanded.Values, (std::vector<bool>{Length == 80}));
    EXPECT_GT(Expanded.Remaining, 0u);
    if (Length == 80) {
      const auto Hits = Access::stats(Builder)[0];
      EXPECT_EQ(Access::batch(Builder, {Query}, 1000000,
                              limits::kMaxJumpTableExpandedResolverDepth),
                Expanded);
      EXPECT_EQ(Access::stats(Builder)[0], Hits + 1);
    }
    // A successful expanded query cannot turn a later default-depth request
    // into evidence, including when both requests use the same graph cache.
    EXPECT_EQ(Access::batch(Builder, {Query}), Default);
  }
}

TEST(ResolverCountLane, ScalarCountsEqualTheirZeroExtendedLowByte) {
  for (auto Architecture : {Arch::X86, Arch::X64, Arch::AArch64}) {
    auto Image = image();
    Image.Arch = Architecture;
    for (auto Opcode : {NdOp::POPCOUNT, NdOp::LZCOUNT}) {
      for (uint16_t InputSize : {1, 2, 4, 8}) {
        for (uint16_t OutputSize : {2, 4, 8}) {
          SCOPED_TRACE(static_cast<unsigned>(Architecture));
          SCOPED_TRACE(static_cast<unsigned>(Opcode));
          SCOPED_TRACE(InputSize);
          SCOPED_TRACE(OutputSize);
          CFGBuilder Builder;
          const auto Query = Access::seedCountLane(Builder, Image, Opcode,
                                                   InputSize, OutputSize);
          const auto Result = Access::batch(Builder, {Query});
          EXPECT_TRUE(Result.Complete);
          EXPECT_EQ(Result.QueryComplete, (std::vector<bool>{true}));
          EXPECT_EQ(Result.Values, (std::vector<bool>{true}));
        }
      }
    }
  }
}

TEST(ResolverCountLane, OtherWidthsLanesAndOperationsGrantNoProof) {
  const auto Image = image();
  for (auto Opcode : {NdOp::POPCOUNT, NdOp::LZCOUNT, NdOp::INT_NOT}) {
    for (const auto Shape : {std::array<unsigned, 4>{0, 4, 0, 1},
                             {16, 4, 0, 1},
                             {4, 16, 0, 1},
                             {4, 4, 1, 1},
                             {4, 4, 0, 0},
                             {4, 4, 0, 2}}) {
      SCOPED_TRACE(static_cast<unsigned>(Opcode));
      SCOPED_TRACE(::testing::PrintToString(Shape));
      CFGBuilder Builder;
      const auto Query = Access::seedCountLane(Builder, Image, Opcode, Shape[0],
                                               Shape[1], Shape[2], Shape[3]);
      EXPECT_EQ(Access::batch(Builder, {Query}).Values,
                (std::vector<bool>{false}));
    }
  }
  CFGBuilder Other;
  const auto Query = Access::seedCountLane(Other, Image, NdOp::INT_NOT, 4, 4);
  EXPECT_EQ(Access::batch(Other, {Query}).Values, (std::vector<bool>{false}));
}

TEST(ResolverCountLane, ExtensionsAndCachedProofsKeepTheirWorkContract) {
  const auto Image = image();
  CFGBuilder Warm;
  auto Query = Access::seedCountLane(Warm, Image, NdOp::POPCOUNT, 8, 8);
  const auto First = Access::batch(Warm, {Query});
  ASSERT_TRUE(First.Complete);
  ASSERT_EQ(First.Values, (std::vector<bool>{true}));
  const auto Hits = Access::stats(Warm)[0];
  EXPECT_EQ(Access::batch(Warm, {Query}), First);
  EXPECT_GT(Access::stats(Warm)[0], Hits);
  const size_t Work = 1000000 - First.Remaining;
  ASSERT_GT(Work, 1u);
  for (size_t Budget :
       {size_t{0}, size_t{1}, Work / 2, Work - 1, Work, Work + 1}) {
    SCOPED_TRACE(Budget);
    CFGBuilder Cold;
    Access::seedCountLane(Cold, Image, NdOp::POPCOUNT, 8, 8);
    EXPECT_EQ(Access::batch(Warm, {Query}, Budget),
              Access::batch(Cold, {Query}, Budget));
  }
  EXPECT_FALSE(Access::batch(Warm, {Query}, 0).Complete);
  EXPECT_TRUE(Access::batch(Warm, {Query}, 0).Values.empty());
  Query.AllowZeroExtension = false;
  const auto NoExtension = Access::batch(Warm, {Query});
  EXPECT_TRUE(NoExtension.Complete);
  EXPECT_EQ(NoExtension.Values, (std::vector<bool>{false}));
}

TEST(ResolverValueQueryCache, ReplaysOrderedResultsAndEveryBudgetBoundary) {
  const auto Image = image();
  CFGBuilder Warm;
  Access::seed(Warm, Image);
  auto True = Access::valueQuery();
  auto False = True;
  False.Alternatives.front().Value = NdVar::cst(8, 8);
  auto Missing = True;
  Missing.UseAddr = 0x9999;
  auto Feasible = True;
  Feasible.Relation = Access::Relation::UnsignedFeasibleSet;
  Feasible.UnsignedUpperBound = 8;
  Feasible.Alternatives.clear();
  const std::vector<Access::Query> Queries{True, False, Missing, Feasible};
  auto Run = [&](CFGBuilder &B, size_t Budget) {
    return Access::batch(B, Queries, Budget, 0, 0, true, true, true);
  };
  const auto First = Run(Warm, 1000000);
  ASSERT_TRUE(First.Complete);
  ASSERT_EQ(First.Values, (std::vector<bool>{true, false, false, true}));
  ASSERT_EQ(First.QueryComplete, (std::vector<bool>{true, true, true, true}));
  ASSERT_EQ(First.Masks, (std::vector<uint64_t>{0, 0, 0, 128}));
  ASSERT_GT(Access::stats(Warm)[1], 0u);
  const size_t Hits = Access::stats(Warm)[0];
  EXPECT_EQ(Run(Warm, 1000000), First);
  ASSERT_EQ(Access::stats(Warm)[0], Hits + 1);

  const size_t Work = 1000000 - First.Remaining;
  ASSERT_LT(Work, 50000u);
  for (size_t Budget = 0; Budget <= Work + 1; ++Budget) {
    SCOPED_TRACE(Budget);
    CFGBuilder Cold;
    Access::seed(Cold, Image);
    const size_t BeforeHits = Access::stats(Warm)[0];
    EXPECT_EQ(Run(Warm, Budget), Run(Cold, Budget));
    if (Budget < Work)
      EXPECT_EQ(Access::stats(Warm)[0], BeforeHits);
  }

  auto Reordered = Queries;
  std::swap(Reordered[0], Reordered[1]);
  const auto Changed =
      Access::batch(Warm, Reordered, 1000000, 0, 0, true, true, true);
  EXPECT_EQ(Changed.Values, (std::vector<bool>{false, true, false, true}));
  Access::clear(Warm);
  EXPECT_EQ(Access::batch(Warm, Reordered, 1000000, 0, 0, true, true, true),
            Changed);
}

TEST(ResolverValueQueryCache, EqualSizedQueryMutationsCannotReuseOldProofs) {
  using Query = Access::Query;
  using Mutation = std::pair<const char *, std::function<void(Query &)>>;
  const std::vector<Mutation> Mutations{
      {"candidate space",
       [](Query &Q) { Q.Candidate.Space = VnodeSpace::REG; }},
      {"candidate offset", [](Query &Q) { Q.Candidate.Offset = 8; }},
      {"candidate width", [](Query &Q) { Q.Candidate.Size = 4; }},
      {"candidate provenance",
       [](Query &Q) {
         Q.Candidate.Provenance = ConstantAddressProvenance::Scalar;
       }},
      {"candidate owner", [](Query &Q) { Q.Candidate.AddressOwnerVA = 0x200; }},
      {"use address", [](Query &Q) { Q.UseAddr = 0x102; }},
      {"use sequence", [](Query &Q) { Q.UseSeq = 1; }},
      {"alternative space",
       [](Query &Q) { Q.Alternatives.front().Value.Space = VnodeSpace::TEMP; }},
      {"alternative value",
       [](Query &Q) { Q.Alternatives.front().Value.Offset = 9; }},
      {"alternative width",
       [](Query &Q) { Q.Alternatives.front().Value.Size = 4; }},
      {"alternative provenance",
       [](Query &Q) {
         Q.Alternatives.front().Value.Provenance =
             ConstantAddressProvenance::DataAddress;
       }},
      {"alternative owner",
       [](Query &Q) { Q.Alternatives.front().Value.AddressOwnerVA = 0x200; }},
      {"alternative address",
       [](Query &Q) { Q.Alternatives.front().Addr = 0x101; }},
      {"alternative sequence",
       [](Query &Q) { Q.Alternatives.front().Seq = 1; }},
      {"alternative definition",
       [](Query &Q) { Q.Alternatives.front().DefinedAtPoint = true; }},
      {"alternative order",
       [](Query &Q) { std::swap(Q.Alternatives[0], Q.Alternatives[1]); }},
      {"zero extension", [](Query &Q) { Q.AllowZeroExtension = true; }},
      {"sign extension", [](Query &Q) { Q.AllowSignExtension = true; }},
      {"occurrence roots",
       [](Query &Q) { Q.UseDefinedAlternativesAsOccurrenceRoots = true; }},
      {"private frame calls",
       [](Query &Q) { Q.AllowPrivateFrameMemoryAcrossCalls = true; }},
      {"relation", [](Query &Q) { Q.Relation = Access::Relation::MayDepend; }},
      {"unsigned bound", [](Query &Q) { Q.UnsignedUpperBound = 8; }},
      {"exact owner", [](Query &Q) { Q.RequireExactAddressOwner = true; }},
      {"frame addend", [](Query &Q) { Q.FrameByteAddend = 1; }},
      {"alternative frame addend",
       [](Query &Q) { Q.AlternativeFrameByteAddend = 1; }},
      {"frame address", [](Query &Q) { Q.FrameAddressUseAddr = 0x101; }},
      {"frame sequence", [](Query &Q) { Q.FrameAddressUseSeq = 1; }},
      {"frame width", [](Query &Q) { Q.FrameMemorySize = 4; }},
      {"frame value offsets",
       [](Query &Q) { Q.AlternativeFrameValueOffsets.front() = 1; }},
      {"authenticated store",
       [](Query &Q) { Q.AuthenticatedFrameStoreWriters.front().Seq = 1; }},
      {"authenticated memcpy",
       [](Query &Q) { Q.AuthenticatedFrameMemcpyWriters.front().Seq = 1; }},
      {"scalar folding", [](Query &Q) { Q.FoldScalarConstantOps = true; }},
  };
  const auto Image = image();
  for (const auto &[Name, Mutate] : Mutations) {
    SCOPED_TRACE(Name);
    CFGBuilder B;
    Access::seed(B, Image);
    auto Q = Access::valueQuery();
    auto Other = Q.Alternatives.front();
    Other.Value = NdVar::cst(8, 8);
    Q.Alternatives.push_back(Other);
    Q.AlternativeFrameValueOffsets = {0, 0};
    Q.AuthenticatedFrameStoreWriters = {Q.Alternatives.front()};
    Q.AuthenticatedFrameMemcpyWriters = {Q.Alternatives.front()};
    ASSERT_TRUE(Access::batch(B, {Q}).Complete);
    ASSERT_GT(Access::stats(B)[1], 0u);
    const auto Graph = Access::token(B);
    const size_t Hits = Access::stats(B)[0];
    Mutate(Q);
    const auto Changed = Access::batch(B, {Q});
    EXPECT_EQ(Access::token(B), Graph);
    EXPECT_EQ(Access::stats(B)[0], Hits);
    Access::clear(B);
    EXPECT_EQ(Access::batch(B, {Q}), Changed);
  }
}

TEST(ResolverValueQueryCache,
     ModesLimitsAndOptionalOutputsFollowColdExecution) {
  const auto Image = image();
  CFGBuilder B;
  Access::seed(B, Image, 16);
  auto Q = Access::valueQuery(16);
  Q.Relation = Access::Relation::UnsignedFeasibleSet;
  Q.UnsignedUpperBound = 8;
  Q.Alternatives.clear();
  for (bool WantComplete : {false, true})
    for (bool WantQueryComplete : {false, true})
      for (bool WantMasks : {false, true}) {
        SCOPED_TRACE(WantComplete);
        SCOPED_TRACE(WantQueryComplete);
        SCOPED_TRACE(WantMasks);
        const auto Warm = Access::batch(B, {Q}, 1000000, 0, 0, WantComplete,
                                        WantQueryComplete, WantMasks);
        CFGBuilder Cold;
        Access::seed(Cold, Image, 16);
        EXPECT_EQ(Warm, Access::batch(Cold, {Q}, 1000000, 0, 0, WantComplete,
                                      WantQueryComplete, WantMasks));
        EXPECT_EQ(Warm, Access::batch(B, {Q}, 1000000, 0, 0, WantComplete,
                                      WantQueryComplete, WantMasks));
        if (WantMasks)
          EXPECT_EQ(Warm.Masks, (std::vector<uint64_t>{128}));
      }
  for (uint32_t Depth : {1u, 2u, 16u, 64u}) {
    SCOPED_TRACE(Depth);
    const auto Warm = Access::batch(B, {Q}, 1000000, Depth);
    CFGBuilder Cold;
    Access::seed(Cold, Image, 16);
    EXPECT_EQ(Warm, Access::batch(Cold, {Q}, 1000000, Depth));
  }
  Q = Access::valueQuery(16);
  for (size_t Limit : {1u, 2u, 4u, 16u, 64u, 128u}) {
    SCOPED_TRACE(Limit);
    const auto Warm = Access::batch(B, {Q}, 1000000, 0, Limit);
    CFGBuilder Cold;
    Access::seed(Cold, Image, 16);
    EXPECT_EQ(Warm, Access::batch(Cold, {Q}, 1000000, 0, Limit));
  }
}

TEST(ResolverValueQueryCache, GraphIdentityDoesNotReplaceProofContextIdentity) {
  const auto Image = image();
  for (auto Mutate :
       {Access::changeAddressOccurrence, Access::changeNestedAddressOccurrence,
        Access::changeScalarOccurrence, Access::changeEntry,
        Access::changeConsumerAudit, Access::changeSymbolBudget}) {
    CFGBuilder B;
    Access::seed(B, Image);
    Access::seedOccurrences(B);
    const auto Q = Access::valueQuery();
    const auto First = Access::batch(B, {Q});
    ASSERT_TRUE(First.Complete);
    EXPECT_EQ(Access::batch(B, {Q}), First);
    ASSERT_GT(Access::stats(B)[0], 0u);
    const auto Graph = Access::token(B);
    const size_t Hits = Access::stats(B)[0];
    Mutate(B);
    const auto Changed = Access::batch(B, {Q});
    EXPECT_EQ(Access::token(B), Graph);
    EXPECT_EQ(Access::stats(B)[0], Hits);
    Access::clear(B);
    EXPECT_EQ(Access::batch(B, {Q}), Changed);
  }

  CFGBuilder B;
  Access::seed(B, Image);
  const auto Q = Access::valueQuery();
  ASSERT_TRUE(Access::batch(B, {Q}).Complete);
  const auto Graph = Access::token(B);
  const auto OtherImage = image();
  Access::changeImage(B, OtherImage);
  const size_t Hits = Access::stats(B)[0];
  const auto Changed = Access::batch(B, {Q});
  EXPECT_EQ(Access::token(B), Graph);
  EXPECT_EQ(Access::stats(B)[0], Hits);
  Access::clear(B);
  EXPECT_EQ(Access::batch(B, {Q}), Changed);
}

TEST(ResolverValueQueryCache, DepthRefusalRemainsIncompleteAndRetriesNewLimit) {
  const auto Image = image();
  CFGBuilder B;
  Access::seed(B, Image, 16);
  const auto Q = Access::valueQuery(16);
  const auto Incomplete = Access::batch(B, {Q}, 1000000, 1);
  ASSERT_FALSE(Incomplete.Complete);
  ASSERT_GT(Incomplete.Remaining, 0u);
  ASSERT_TRUE(Access::token(B));
  EXPECT_EQ(Access::stats(B)[0], 0u);
  EXPECT_EQ(Access::stats(B)[1], 1u);
  EXPECT_EQ(Access::batch(B, {Q}, 1000000, 1), Incomplete);
  EXPECT_EQ(Access::stats(B)[0], 1u);
  EXPECT_EQ(Access::stats(B)[1], 1u);
  const auto Complete = Access::batch(B, {Q});
  ASSERT_TRUE(Complete.Complete);
  ASSERT_GT(Access::stats(B)[1], 0u);
  EXPECT_EQ(Access::batch(B, {Q}, 1000000, 1), Incomplete);
  EXPECT_EQ(Access::stats(B)[0], 2u);
  EXPECT_EQ(Access::batch(B, {Q}), Complete);
  EXPECT_EQ(Access::stats(B)[0], 3u);
}

TEST(ResolverValueQueryCache, DepthRefusalPreservesIndependentQueryOutputs) {
  const auto Image = image();
  const auto Deep = Access::valueQuery(16);
  auto Direct = Deep;
  Direct.Candidate = NdVar::cst(7, 8);
  auto Different = Direct;
  Different.Alternatives.front().Value = NdVar::cst(8, 8);
  auto Range = Direct;
  Range.Relation = Access::Relation::UnsignedFeasibleSet;
  Range.UnsignedUpperBound = 16;
  Range.Alternatives.clear();
  for (bool WantComplete : {false, true}) {
    for (bool WantQueryComplete : {false, true}) {
      for (bool WantMasks : {false, true}) {
        SCOPED_TRACE(std::to_string(WantComplete) + ":" +
                     std::to_string(WantQueryComplete) + ":" +
                     std::to_string(WantMasks));
        CFGBuilder B;
        Access::seed(B, Image, 16);
        auto Run = [&](const std::vector<Access::Query> &Queries) {
          return Access::batch(B, Queries, 1000000, 1, 0, WantComplete,
                               WantQueryComplete, WantMasks);
        };
        const std::vector<Access::Query> Queries{Range, Deep, Direct,
                                                 Different};
        const auto First = Run(Queries);
        EXPECT_FALSE(First.Complete);
        EXPECT_EQ(First.Values, (std::vector<bool>{true, false, true, false}));
        if (WantQueryComplete)
          EXPECT_EQ(First.QueryComplete,
                    (std::vector<bool>{true, false, true, true}));
        if (WantMasks)
          EXPECT_EQ(First.Masks, (std::vector<uint64_t>{128, 0, 0, 0}));
        EXPECT_EQ(Run(Queries), First);
        EXPECT_EQ(Access::stats(B)[0], 1u);
        const std::vector<Access::Query> Reordered{Deep, Range, Different,
                                                   Direct};
        const auto Changed = Run(Reordered);
        EXPECT_EQ(Access::stats(B)[0], 1u);
        Access::clear(B);
        EXPECT_EQ(Run(Reordered), Changed);
      }
    }
  }
}

TEST(ResolverValueQueryCache, DepthRefusalPaysColdWorkAndTracksContext) {
  const auto Image = image();
  const auto Deep = Access::valueQuery(16);
  auto Direct = Deep;
  Direct.Candidate = NdVar::cst(7, 8);
  CFGBuilder Warm;
  Access::seed(Warm, Image, 16);
  auto Run = [&](CFGBuilder &B, size_t Budget) {
    return Access::batch(B, {Deep, Direct}, Budget, 1, 0, true, true, true);
  };
  const auto First = Run(Warm, 1000000);
  ASSERT_FALSE(First.Complete);
  ASSERT_EQ(First.QueryComplete, (std::vector<bool>{false, true}));
  ASSERT_EQ(Access::stats(Warm)[1], 1u);
  const size_t Work = 1000000 - First.Remaining;
  for (size_t Budget = 0; Budget <= Work + 1; ++Budget) {
    SCOPED_TRACE(Budget);
    CFGBuilder Cold;
    Access::seed(Cold, Image, 16);
    EXPECT_EQ(Run(Warm, Budget), Run(Cold, Budget));
  }
  for (auto Mutate :
       {Access::changeValue, Access::changeEntry,
        Access::changeAddressOccurrence, Access::changeNestedAddressOccurrence,
        Access::changeScalarOccurrence}) {
    CFGBuilder B;
    Access::seed(B, Image, 16);
    Access::seedOccurrences(B);
    const auto Original = Run(B, 1000000);
    ASSERT_EQ(Run(B, 1000000), Original);
    const auto Graph = Access::token(B);
    const auto Hits = Access::stats(B)[0];
    Mutate(B);
    const auto Changed = Run(B, 1000000);
    EXPECT_EQ(Access::stats(B)[0], Access::token(B) == Graph ? Hits : 0u);
    Access::clear(B);
    EXPECT_EQ(Run(B, 1000000), Changed);
  }
}

TEST(ResolverValueQueryCache, SharedMatchFailureClearsEveryOutput) {
  const auto Image = image();
  CFGBuilder B;
  Access::seed(B, Image);
  auto Range = Access::valueQuery();
  Range.Relation = Access::Relation::UnsignedFeasibleSet;
  Range.UnsignedUpperBound = 16;
  Range.Alternatives.clear();
  const auto Match = Access::valueQuery();
  const auto Result =
      Access::batch(B, {Range, Match, Range}, 1000000, 0, 1, true, true, true);
  EXPECT_FALSE(Result.Complete);
  EXPECT_EQ(Result.Values, (std::vector<bool>{false, false, false}));
  EXPECT_EQ(Result.QueryComplete, (std::vector<bool>{false, false, false}));
  EXPECT_EQ(Result.Masks, (std::vector<uint64_t>{0, 0, 0}))
      << "a failed transaction must not expose a prefix or suffix mask";
}

TEST(ResolverValueQueryCache, MatchRefusalReplaysWithoutBecomingAProof) {
  const auto Image = image();
  const auto Q = Access::valueQuery();
  for (bool WantComplete : {false, true}) {
    for (bool WantQueryComplete : {false, true}) {
      for (bool WantMasks : {false, true}) {
        SCOPED_TRACE(std::to_string(WantComplete) + ":" +
                     std::to_string(WantQueryComplete) + ":" +
                     std::to_string(WantMasks));
        CFGBuilder B;
        Access::seed(B, Image);
        auto Run = [&](size_t MatchLimit) {
          return Access::batch(B, {Q}, 1000000, 0, MatchLimit, WantComplete,
                               WantQueryComplete, WantMasks);
        };
        const auto Refusal = Run(1);
        ASSERT_FALSE(Refusal.Complete);
        ASSERT_EQ(Refusal.Values, (std::vector<bool>{false}));
        ASSERT_GT(Refusal.Remaining, 0u);
        ASSERT_EQ(Access::stats(B)[1], 1u);
        EXPECT_EQ(Run(1), Refusal);
        EXPECT_EQ(Access::stats(B)[0], 1u);
        const auto Retried = Run(0);
        EXPECT_EQ(Retried.Complete, WantComplete);
        EXPECT_EQ(Retried.Values, (std::vector<bool>{true}));
        EXPECT_EQ(Access::stats(B)[0], 1u)
            << "a larger match allowance must reanalyze the query";
        EXPECT_EQ(Run(1), Refusal);
        EXPECT_EQ(Access::stats(B)[0], 2u);
      }
    }
  }
}

TEST(ResolverValueQueryCache, MatchRefusalPaysColdWorkAndTracksGraphChanges) {
  const auto Image = image();
  const auto Q = Access::valueQuery();
  CFGBuilder Warm;
  Access::seed(Warm, Image);
  auto Run = [&](CFGBuilder &B, size_t Budget) {
    return Access::batch(B, {Q}, Budget, 0, 1, true, true, true);
  };
  const auto Refusal = Run(Warm, 1000000);
  ASSERT_FALSE(Refusal.Complete);
  ASSERT_EQ(Access::stats(Warm)[1], 1u);
  const size_t Work = 1000000 - Refusal.Remaining;
  for (size_t Budget = 0; Budget <= Work + 1; ++Budget) {
    SCOPED_TRACE(Budget);
    CFGBuilder Cold;
    Access::seed(Cold, Image);
    EXPECT_EQ(Run(Warm, Budget), Run(Cold, Budget));
  }
  const auto OriginalGraph = Access::token(Warm);
  Access::changeValue(Warm);
  const auto Changed = Run(Warm, 1000000);
  EXPECT_NE(Access::token(Warm), OriginalGraph);
  EXPECT_EQ(Access::stats(Warm)[0], 0u);
  Access::clear(Warm);
  EXPECT_EQ(Run(Warm, 1000000), Changed);
  const auto Complete = Access::batch(Warm, {Q});
  EXPECT_TRUE(Complete.Complete);
  EXPECT_EQ(Complete.Values, (std::vector<bool>{false}));
}

TEST(ResolverValueQueryCache, EarlierUnknownPreventsMatchRefusalRetention) {
  const auto Image = image();
  CFGBuilder B;
  Access::seed(B, Image, 16);
  auto Unknown = Access::valueQuery(16);
  Unknown.Candidate = NdVar::cst(7, 8);
  Unknown.Relation = Access::Relation::UnsignedFeasibleSet;
  Unknown.UnsignedUpperBound = 0;
  auto Direct = Access::valueQuery(16);
  Direct.Candidate = NdVar::cst(7, 8);
  const auto First = Access::batch(B, {Unknown, Direct}, 1000000, 1, 1);
  ASSERT_FALSE(First.Complete);
  ASSERT_EQ(First.Values, (std::vector<bool>{false, false}));
  EXPECT_EQ(Access::stats(B)[1], 0u);
  EXPECT_EQ(Access::batch(B, {Unknown, Direct}, 1000000, 1, 1), First);
  EXPECT_EQ(Access::stats(B)[0], 0u);
  EXPECT_EQ(Access::stats(B)[1], 0u);
  Unknown.UnsignedUpperBound = 16;
  const auto Complete = Access::batch(B, {Unknown, Direct});
  EXPECT_TRUE(Complete.Complete);
  EXPECT_EQ(Complete.Values, (std::vector<bool>{true, true}));
}

TEST(ResolverValueQueryCache, RetainedCompletedBatchesAreBounded) {
  const auto Image = image();
  CFGBuilder B;
  Access::seed(B, Image);
  auto Q = Access::valueQuery();
  Q.Candidate = NdVar::cst(7, 8);
  for (unsigned I = 0; I != 66; ++I) {
    // Exact identities differ, while every query proves the same constant.
    Q.UseAddr = 0x1000 + I;
    const auto Answer = Access::batch(B, {Q});
    ASSERT_TRUE(Answer.Complete);
    ASSERT_EQ(Answer.Values, (std::vector<bool>{true}));
    EXPECT_LE(Access::stats(B)[1], 64u);
    EXPECT_LE(Access::stats(B)[2], 8u * 1024 * 1024);
  }
  EXPECT_GT(Access::stats(B)[1], 0u);
}

TEST(ResolverValueQueryCache, OversizedCompletedInputRunsWithoutRetention) {
  const auto Image = image();
  CFGBuilder B;
  Access::seed(B, Image);
  auto Q = Access::valueQuery();
  Q.Candidate = NdVar::cst(7, 8);
  constexpr size_t Capacity = 8 * 1024 * 1024;
  const auto Alternative = Q.Alternatives.front();
  Q.Alternatives.assign(Capacity / sizeof(Access::Occurrence) + 1, Alternative);
  const auto Answer = Access::batch(B, {Q}, 100000000);
  ASSERT_TRUE(Answer.Complete);
  EXPECT_EQ(Answer.Values, (std::vector<bool>{true}));
  EXPECT_EQ(Access::stats(B)[0], 0u);
  EXPECT_EQ(Access::stats(B)[1], 0u);
  EXPECT_LE(Access::stats(B)[2], Capacity);

  Q.Alternatives = {Alternative};
  const auto Small = Access::batch(B, {Q});
  ASSERT_TRUE(Small.Complete);
  ASSERT_GT(Access::stats(B)[1], 0u);
  EXPECT_EQ(Access::batch(B, {Q}), Small);
  EXPECT_EQ(Access::stats(B)[0], 1u);
}

TEST(ResolverValueQueryCache, MalformedScalarRecordsCannotPublishProofs) {
  for (const auto Architecture :
       {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
      auto Image = image();
      Image.Arch = Architecture;
      Image.Bits = Architecture == Arch::X86 || Architecture == Arch::ARM
                       ? Bitness::Bits32
                       : Bitness::Bits64;
      Image.Format = Format;
      SCOPED_TRACE(static_cast<unsigned>(Architecture));
      SCOPED_TRACE(static_cast<unsigned>(Format));
      for (unsigned Variant = 0; Variant != 6; ++Variant) {
        SCOPED_TRACE(Variant);
        CFGBuilder B;
        Access::seed(B, Image);
        const auto Valid = Access::batch(B, {Access::valueQuery()});
        ASSERT_TRUE(Valid.Complete);
        ASSERT_EQ(Valid.Values, (std::vector<bool>{true}));
        Access::malformedScalar(B, Variant);
        const auto Refused = Access::batch(B, {Access::valueQuery()});
        EXPECT_FALSE(Refused.Complete);
        EXPECT_EQ(Refused.Values, (std::vector<bool>{false}));
        EXPECT_EQ(Refused.QueryComplete, (std::vector<bool>{false}));
        EXPECT_EQ(Access::batch(B, {Access::valueQuery()}), Refused);
        Access::clear(B);
        EXPECT_EQ(Access::batch(B, {Access::valueQuery()}), Refused);
      }
    }
}

TEST(ResolverValueQueryCache,
     EntryMemoryCannotBorrowAStoreFromALaterIteration) {
  for (const auto Architecture :
       {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
      auto Image = image();
      Image.Arch = Architecture;
      Image.Bits = Architecture == Arch::X86 || Architecture == Arch::ARM
                       ? Bitness::Bits32
                       : Bitness::Bits64;
      Image.Format = Format;
      for (bool Initialize : {false, true})
        for (bool Loop : {false, true})
          for (bool Reverse : {false, true}) {
            SCOPED_TRACE(static_cast<unsigned>(Architecture));
            SCOPED_TRACE(static_cast<unsigned>(Format));
            SCOPED_TRACE(Initialize);
            SCOPED_TRACE(Loop);
            SCOPED_TRACE(Reverse);
            CFGBuilder B;
            auto Queries = Access::seedEntryMemory(B, Image, Initialize, Loop);
            if (Reverse)
              std::reverse(Queries.begin(), Queries.end());
            const auto Answer = Access::batch(B, Queries);
            EXPECT_TRUE(Answer.Complete);
            EXPECT_EQ(Answer.Values,
                      Reverse
                          ? (std::vector<bool>{true, Initialize, Initialize})
                          : (std::vector<bool>{Initialize, Initialize, true}));
            EXPECT_EQ(Access::batch(B, Queries), Answer);
            Access::clear(B);
            EXPECT_EQ(Access::batch(B, Queries), Answer);
            const size_t Work = 1000000 - Answer.Remaining;
            const auto Exact = Access::batch(B, Queries, Work);
            EXPECT_TRUE(Exact.Complete);
            EXPECT_EQ(Exact.Values, Answer.Values);
            const auto Short = Access::batch(B, Queries, Work - 1);
            EXPECT_FALSE(Short.Complete);
            EXPECT_EQ(Short.Values, (std::vector<bool>{false, false, false}));
            EXPECT_EQ(Short.QueryComplete,
                      (std::vector<bool>{false, false, false}));
            Access::clear(B);
            EXPECT_EQ(Access::batch(B, Queries, Work - 1), Short);
          }
    }
}

TEST(ResolverValueQueryCache, FiniteJointReuseRequiresCurrentSourceAndRoles) {
  // The fixture supplies a previously completed numerical result directly;
  // this exercises its production reuse gate, not construction of the theorem.
  // Real four/six-consumer GOTOFF fixtures exercise that construction
  // separately.
  auto Image = image();
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  for (unsigned Variant = 0; Variant < 23; ++Variant) {
    SCOPED_TRACE(Variant);
    CFGBuilder B;
    Access::RoleInfo Info;
    std::set<va_t> Protected{0x4000};
    std::set<va_t> Known{0x800};
    B.setProtectedJumpTableRelocationSlots(&Protected);
    B.setKnownFuncEntries(&Known);
    const auto Proof = Access::seedFiniteJointReuse(B, Image, Info);
    size_t Remaining = 1000000;
    bool Incomplete = false;
    ASSERT_TRUE(
        Access::reuseFiniteJoint(B, Proof, Info, Remaining, Incomplete));
    ASSERT_FALSE(Incomplete);
    const size_t Work = 1000000 - Remaining;
    Remaining = Work;
    EXPECT_TRUE(
        Access::reuseFiniteJoint(B, Proof, Info, Remaining, Incomplete));
    EXPECT_EQ(Remaining, 0u);
    Remaining = Work - 1;
    EXPECT_FALSE(
        Access::reuseFiniteJoint(B, Proof, Info, Remaining, Incomplete));
    EXPECT_TRUE(Incomplete);
    Incomplete = false;
    if (Variant < 8 || Variant >= 19)
      Access::mutateFiniteJointSource(B, Variant < 8 ? Variant : Variant - 11);
    else if (Variant == 8)
      ++Info.IndexUseSeq;
    else if (Variant == 9)
      Info.IndexValueDefinedAtUse = true;
    else if (Variant == 10)
      ++Info.TargetLoads.front().Seq;
    else if (Variant == 11)
      ++Info.LoadRoles.front().AddressIndex.Seq;
    else if (Variant == 12)
      ++Info.LoadRoles.front().AddressScale;
    else if (Variant == 13)
      ++Info.TableLoadSeq;
    else if (Variant == 14)
      ++Info.BaseAddr;
    else if (Variant == 15)
      Info.IndexValueAlternatives = {{Info.IndexValueAtUse, Info.IndexUseAddr,
                                      Info.IndexUseSeq + 1, false}};
    else if (Variant == 16)
      Protected = {0x4004};
    else if (Variant == 17)
      Known = {0x900};
    else
      ++Info.PhysicalCapacity;
    for (unsigned Repeat = 0; Repeat < 2; ++Repeat) {
      Remaining = 1000000;
      EXPECT_FALSE(
          Access::reuseFiniteJoint(B, Proof, Info, Remaining, Incomplete));
      EXPECT_FALSE(Incomplete);
    }
  }
}

TEST(ResolverLoadRoles, RestrictedReplayDoesNotBorrowTheGroupHypothesis) {
  auto Image = image();
  Image.Bits = Bitness::Bits64;
  CFGBuilder B;
  const auto Fixture = Access::seedRoleReplay(B, Image);
  const auto Hypothesis =
      Access::replayRoles(B, Fixture.Info, Fixture.Hypothesis, false);
  ASSERT_TRUE(Hypothesis.Complete);
  EXPECT_EQ(Hypothesis.Values, (std::vector<bool>{false, false}));
  const auto Restricted =
      Access::replayRoles(B, Fixture.Info, Fixture.Restricted, false);
  ASSERT_TRUE(Restricted.Complete);
  EXPECT_EQ(Restricted.Values, (std::vector<bool>{true, true}));
  EXPECT_EQ(Access::replayRoles(B, Fixture.Info, Fixture.Restricted, false),
            Restricted);
  // Ordinary group queries retain their preceding-phase H. A supplied map
  // does not silently opt them out of the enclosing group's proof context.
  const auto Group =
      Access::replayRoles(B, Fixture.Info, Fixture.Restricted, true);
  EXPECT_TRUE(Group.Complete);
  EXPECT_EQ(Group.Values, Hypothesis.Values);
  Access::clear(B);
  EXPECT_EQ(Access::replayRoles(B, Fixture.Info, Fixture.Restricted, false),
            Restricted);
}

TEST(ResolverFiniteControl, ProvisionalNonmemberCannotHideOutOfRangeStore) {
  auto Image = image();
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::ELF;
  CFGBuilder B;
  const auto Fixture = Access::seedFiniteControl(B, Image);
  const auto Apparent = Access::finiteSelectors(B, Fixture);
  ASSERT_TRUE(Apparent.Complete);
  EXPECT_EQ(Apparent.Values, (std::vector<bool>{true, true}));
  EXPECT_EQ(Apparent.Masks, (std::vector<uint64_t>{3, 3}));
  const auto Legacy = Access::finiteControl(B, Fixture, false);
  ASSERT_TRUE(Legacy.Complete);
  EXPECT_TRUE(Legacy.Closed);
  EXPECT_FALSE(Legacy.Reachable.count(0x140));
  const auto Strict = Access::finiteControl(B, Fixture);
  EXPECT_TRUE(Strict.Complete);
  EXPECT_FALSE(Strict.Closed);
  EXPECT_EQ(Strict.Reachable, Legacy.Reachable);
  EXPECT_EQ(Access::finiteControl(B, Fixture), Strict);
  Access::clear(B);
  EXPECT_EQ(Access::finiteControl(B, Fixture), Strict);
  // A second real target reaches STORE 2. Merely listing some targets cannot
  // provide completeness; exposing the omitted STORE also breaks the domain.
  Access::exposeFiniteControlStore(B);
  const auto Exposed = Access::finiteSelectors(B, Fixture);
  ASSERT_TRUE(Exposed.Complete);
  EXPECT_EQ(Exposed.Values, (std::vector<bool>{false, false}));
  EXPECT_EQ(Exposed.Masks, (std::vector<uint64_t>{0, 0}));
  const auto WithStore = Access::finiteControl(B, Fixture);
  EXPECT_TRUE(WithStore.Complete);
  EXPECT_FALSE(WithStore.Closed);
  EXPECT_TRUE(WithStore.Reachable.count(0x140));
  Access::clear(B);
  EXPECT_EQ(Access::finiteSelectors(B, Fixture), Exposed);
}

TEST(ResolverFiniteControl, CompleteDirectControlPreservesMembersAndBudgets) {
  for (auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
    auto Image = image();
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = Format;
    CFGBuilder B;
    const auto Fixture = Access::seedFiniteControl(B, Image);
    Access::makeFiniteControlDirect(B);
    const auto Numeric = Access::finiteSelectors(B, Fixture);
    ASSERT_TRUE(Numeric.Complete);
    ASSERT_EQ(Numeric.Values, (std::vector<bool>{true, true}));
    const auto Whole = Access::finiteControl(B, Fixture);
    ASSERT_TRUE(Whole.Complete);
    ASSERT_TRUE(Whole.Closed);
    ASSERT_TRUE(Whole.Reachable.count(0x193)); // Indirect RET remains an exit.
    ASSERT_FALSE(Whole.Reachable.count(0x140));
    const size_t Work = 1000000 - Whole.Remaining;
    ASSERT_GT(Work, 0u);
    const auto Exact = Access::finiteControl(B, Fixture, true, Work);
    EXPECT_TRUE(Exact.Complete);
    EXPECT_TRUE(Exact.Closed);
    EXPECT_EQ(Exact.Reachable, Whole.Reachable);
    EXPECT_EQ(Exact.Remaining, 0u);
    const auto Short = Access::finiteControl(B, Fixture, true, Work - 1);
    EXPECT_FALSE(Short.Complete);
    EXPECT_FALSE(Short.Closed);
    EXPECT_TRUE(Short.Reachable.empty());
    EXPECT_EQ(Short.Remaining, 0u);
    EXPECT_EQ(Access::finiteControl(B, Fixture), Whole);
    Access::clear(B);
    EXPECT_EQ(Access::finiteControl(B, Fixture), Whole);
  }
}

TEST(ResolverFiniteControl, EverySuppliedRootMustBeAnExactModeledEntry) {
  auto Image = image();
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  for (unsigned Variant = 0; Variant < 6; ++Variant) {
    SCOPED_TRACE(Variant);
    CFGBuilder B;
    auto Fixture = Access::seedFiniteControl(B, Image);
    Access::makeFiniteControlDirect(B);
    ASSERT_TRUE(Access::finiteControl(B, Fixture).Closed);
    Access::malformedFiniteControlRoot(B, Fixture, Variant);
    const auto Result = Access::finiteControl(B, Fixture);
    EXPECT_TRUE(Result.Complete);
    EXPECT_FALSE(Result.Closed);
    EXPECT_EQ(Access::finiteControl(B, Fixture), Result);
    Access::clear(B);
    EXPECT_EQ(Access::finiteControl(B, Fixture), Result);
    if (Variant == 2) {
      Access::modelFiniteControlInteriorRoot(B);
      const auto Modeled = Access::finiteControl(B, Fixture);
      EXPECT_TRUE(Modeled.Complete);
      EXPECT_TRUE(Modeled.Closed);
    }
  }
}

TEST(ResolverFiniteControl, MemberMapCannotHideAMalformedEarlierBranch) {
  auto Image = image();
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  CFGBuilder B;
  auto Fixture = Access::seedFiniteControl(B, Image);
  Access::makeFiniteControlDirect(B);
  ASSERT_TRUE(Access::finiteControl(B, Fixture).Closed);
  Access::hideFiniteControlBranchBeforeLast(B, Fixture);
  const auto Result = Access::finiteControl(B, Fixture);
  EXPECT_TRUE(Result.Complete);
  EXPECT_FALSE(Result.Closed);
}

TEST(ResolverFiniteControl, ConditionalRootsNeedTheirOwnAdmission) {
  auto Image = image();
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  CFGBuilder B;
  const auto Fixture = Access::seedFiniteControl(B, Image);
  Access::makeFiniteControlDirect(B);
  ASSERT_TRUE(Access::finiteControl(B, Fixture).Closed);
  // The ordinary graph may skip a conditional code root with no modeled
  // block. The narrow joint owner must not borrow that omission as closure.
  Access::omitFiniteControlConditionalRoot(B);
  const auto Legacy = Access::finiteControl(B, Fixture, false);
  ASSERT_TRUE(Legacy.Complete);
  EXPECT_TRUE(Legacy.Closed);
  const auto Strict = Access::finiteControl(B, Fixture);
  EXPECT_TRUE(Strict.Complete);
  EXPECT_FALSE(Strict.Closed);
  EXPECT_EQ(Strict.Reachable, Legacy.Reachable);
  EXPECT_FALSE(Strict.Reachable.count(0x140));
  Access::clear(B);
  EXPECT_EQ(Access::finiteControl(B, Fixture), Strict);
}

TEST(ResolverPendingCycles, BatchAndSingleQueriesIncludeTheLateLoopDefinition) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool Memory : {false, true})
        for (bool Reverse : {false, true}) {
          SCOPED_TRACE(static_cast<unsigned>(Architecture));
          SCOPED_TRACE(static_cast<unsigned>(Format));
          SCOPED_TRACE(Memory);
          SCOPED_TRACE(Reverse);
          auto Image = image();
          Image.Arch = Architecture;
          Image.Bits = Architecture == Arch::X86 || Architecture == Arch::ARM
                           ? Bitness::Bits32
                           : Bitness::Bits64;
          Image.Format = Format;
          CFGBuilder B;
          auto Queries = Access::seedPendingCycle(B, Image, Memory);
          if (Reverse)
            std::reverse(Queries.begin(), Queries.end());
          const auto Both =
              Access::batch(B, Queries, 10000000, 0, 0, true, true, true);
          ASSERT_TRUE(Both.Complete);
          EXPECT_EQ(Both.Values, (std::vector<bool>{true, true}));
          EXPECT_EQ(Both.Masks, (std::vector<uint64_t>{20, 20}));
          EXPECT_EQ(Both, Access::batch(B, Queries, 10000000, 0, 0, true, true,
                                        true));
          for (const auto &Q : Queries) {
            CFGBuilder Single;
            Access::seedPendingCycle(Single, Image, Memory);
            const auto One =
                Access::batch(Single, {Q}, 10000000, 0, 0, true, true, true);
            ASSERT_TRUE(One.Complete);
            EXPECT_EQ(One.Masks, (std::vector<uint64_t>{20}));
          }
          for (auto &Q : Queries) {
            Q.Relation = Access::Relation::UnsignedLessThan;
            Q.UnsignedUpperBound = 4;
          }
          const auto Bound = Access::batch(B, Queries);
          ASSERT_TRUE(Bound.Complete);
          EXPECT_EQ(Bound.Values, (std::vector<bool>{false, false}));
        }
}

TEST(ResolverPendingCycles, RealUpdatesCannotBecomeTransparentBackedges) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (bool Memory : {false, true}) {
      auto Image = image();
      Image.Arch = Architecture;
      Image.Bits = Architecture == Arch::X86 || Architecture == Arch::ARM
                       ? Bitness::Bits32
                       : Bitness::Bits64;
      Image.Format = BinaryFormat::ELF;
      SCOPED_TRACE(static_cast<unsigned>(Architecture));
      SCOPED_TRACE(Memory);
      CFGBuilder B;
      auto Queries = Access::seedPendingCycle(B, Image, Memory, true);
      for (auto &Q : Queries) {
        Q.Relation = Access::Relation::UnsignedLessThan;
        Q.UnsignedUpperBound = 4;
      }
      const auto Bound = Access::batch(B, Queries);
      EXPECT_EQ(Bound.Values, (std::vector<bool>{false, false}));
    }
}

TEST(ResolverPendingCycles, WarmAndColdLimitsKeepTheSameTransaction) {
  auto Image = image();
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  for (bool Memory : {false, true}) {
    CFGBuilder Warm;
    const auto Queries = Access::seedPendingCycle(Warm, Image, Memory);
    constexpr size_t Budget = 10000000;
    const auto Complete =
        Access::batch(Warm, Queries, Budget, 0, 0, true, true, true);
    ASSERT_TRUE(Complete.Complete);
    const size_t Work = Budget - Complete.Remaining;
    ASSERT_GT(Work, 0u);
    for (size_t Limit : {size_t{0}, Work - 1, Work, Work + 1, Budget})
      for (uint32_t Depth : {1u, 2u, 4u, 8u, 64u}) {
        CFGBuilder Cold;
        Access::seedPendingCycle(Cold, Image, Memory);
        EXPECT_EQ(
            Access::batch(Warm, Queries, Limit, Depth, 0, true, true, true),
            Access::batch(Cold, Queries, Limit, Depth, 0, true, true, true));
      }
  }
}

TEST(ResolverPredicateDemand, UnusedLocalShapesDoNotExpandTheirOperands) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (NdOp Opcode :
         {NdOp::BOOL_AND, NdOp::BOOL_OR, NdOp::BOOL_XOR, NdOp::INT_SLESS,
          NdOp::INT_SLESSEQUAL, NdOp::INT_LESS, NdOp::INT_LESSEQUAL})
      for (unsigned Variant : {0u, 1u, 2u}) {
        SCOPED_TRACE(static_cast<unsigned>(Architecture));
        SCOPED_TRACE(static_cast<unsigned>(Opcode));
        SCOPED_TRACE(Variant);
        auto Image = image();
        Image.Arch = Architecture;
        Image.Bits = Architecture == Arch::X86 || Architecture == Arch::ARM
                         ? Bitness::Bits32
                         : Bitness::Bits64;
        CFGBuilder B;
        const auto Q = Access::seedUnusedPredicate(B, Image, Opcode, Variant);
        const auto A = Access::batch(B, {Q});
        EXPECT_TRUE(A.Complete);
        EXPECT_EQ(A.Values, (std::vector<bool>{true}));
      }
}

TEST(ResolverPredicateDemand, NeededAndUncertainShapesKeepTheCompleteWalk) {
  auto Image = image();
  for (NdOp Opcode :
       {NdOp::BOOL_AND, NdOp::BOOL_OR, NdOp::INT_LESS, NdOp::INT_LESSEQUAL,
        NdOp::INT_EQUAL, NdOp::INT_NOTEQUAL}) {
    SCOPED_TRACE(static_cast<unsigned>(Opcode));
    CFGBuilder B;
    auto Q = Access::seedUnusedPredicate(B, Image, Opcode);
    Q.Relation = Opcode == NdOp::BOOL_AND || Opcode == NdOp::BOOL_OR
                     ? Access::Relation::JoinedUnsignedFeasibleSet
                     : Access::Relation::UnsignedFeasibleSet;
    const auto A = Access::batch(B, {Q}, 1000000, 0, 0, true, true, true);
    EXPECT_FALSE(A.Complete);
    EXPECT_EQ(A.QueryComplete, (std::vector<bool>{false}));
  }
  for (unsigned Variant : {3u, 4u, 5u}) {
    SCOPED_TRACE(Variant);
    CFGBuilder B;
    const auto Q =
        Access::seedUnusedPredicate(B, Image, NdOp::BOOL_AND, Variant);
    EXPECT_FALSE(Access::batch(B, {Q}).Complete);
  }
}

TEST(ResolverPredicateDemand, PolicyChangesAndBudgetsAgreeColdAndWarm) {
  auto Image = image();
  CFGBuilder Warm;
  auto Q = Access::seedUnusedPredicate(Warm, Image, NdOp::INT_LESS, 2);
  constexpr size_t Budget = 1000000;
  const auto A = Access::batch(Warm, {Q}, Budget);
  ASSERT_TRUE(A.Complete);
  const size_t Work = Budget - A.Remaining;
  ASSERT_GT(Work, 0u);
  for (auto Relation : {Access::Relation::UnsignedLessThan,
                        Access::Relation::UnsignedFeasibleSet,
                        Access::Relation::JoinedUnsignedFeasibleSet,
                        Access::Relation::UnsignedLessThan}) {
    Q.Relation = Relation;
    for (size_t Limit : {size_t{0}, Work - 1, Work, Work + 1, Budget})
      for (uint32_t Depth : {1u, 2u, 4u, 64u}) {
        CFGBuilder Cold;
        Access::seedUnusedPredicate(Cold, Image, NdOp::INT_LESS, 2);
        EXPECT_EQ(Access::batch(Warm, {Q}, Limit, Depth, 0, true, true, true),
                  Access::batch(Cold, {Q}, Limit, Depth, 0, true, true, true));
      }
  }
}

} // namespace
