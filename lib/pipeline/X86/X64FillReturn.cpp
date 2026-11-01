//===- X64FillReturn.cpp - Checked fill-loop register results ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A bounded source projection for an exact scalar/SSE fill implementation.
/// A library name or a normal C prototype does not describe scratch results.
//===----------------------------------------------------------------------===//

#include "X64FillReturn.h"

#include "neverd/ir/med/MedIR.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {
namespace {
// Complete encoding, including every branch destination and return. No name,
// image address, loaded data or mutable dispatch table participates in
// admission. Padding is checked too: accepting a changed implementation
// requires a new proof, not a looser signature. The routine has no calls or
// memory reads.
constexpr uint8_t FillBody[] = {
    0x48, 0x8b, 0xc1, 0x49, 0x83, 0xf8, 0x08, 0x72, 0x47, 0x0f, 0xb6, 0xd2,
    0x49, 0xb9, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x49, 0x0f,
    0xaf, 0xd1, 0x49, 0x83, 0xf8, 0x4f, 0x73, 0x50, 0x4d, 0x8b, 0xc8, 0x49,
    0x83, 0xe1, 0xf8, 0x49, 0x03, 0xc9, 0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00,
    0x4a, 0x89, 0x54, 0x08, 0xf8, 0x49, 0x83, 0xe9, 0x08, 0x75, 0xf5, 0x49,
    0x83, 0xe0, 0x07, 0x74, 0x05, 0x4a, 0x89, 0x54, 0x01, 0xf8, 0xc3, 0x66,
    0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x49, 0x83, 0xe0, 0x07,
    0x74, 0x0a, 0x42, 0x88, 0x54, 0x00, 0xff, 0x49, 0xff, 0xc8, 0x75, 0xf6,
    0xc3, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x0f, 0x1f, 0x84, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x66, 0x48, 0x0f, 0x6e, 0xc2, 0x0f, 0x16, 0xc0,
    0x0f, 0x11, 0x01, 0x4c, 0x03, 0xc1, 0x48, 0x83, 0xc1, 0x10, 0x48, 0x83,
    0xe1, 0xf0, 0x4c, 0x2b, 0xc1, 0x4d, 0x8b, 0xc8, 0x49, 0xc1, 0xe9, 0x07,
    0x74, 0x2f, 0x0f, 0x29, 0x01, 0x0f, 0x29, 0x41, 0x10, 0x48, 0x81, 0xc1,
    0x80, 0x00, 0x00, 0x00, 0x0f, 0x29, 0x41, 0xa0, 0x0f, 0x29, 0x41, 0xb0,
    0x49, 0xff, 0xc9, 0x0f, 0x29, 0x41, 0xc0, 0x0f, 0x29, 0x41, 0xd0, 0x0f,
    0x29, 0x41, 0xe0, 0x0f, 0x29, 0x41, 0xf0, 0x75, 0xd5, 0x49, 0x83, 0xe0,
    0x7f, 0x4d, 0x8b, 0xc8, 0x49, 0xc1, 0xe9, 0x04, 0x74, 0x12, 0x66, 0x0f,
    0x1f, 0x44, 0x00, 0x00, 0x0f, 0x11, 0x01, 0x48, 0x83, 0xc1, 0x10, 0x49,
    0xff, 0xc9, 0x75, 0xf4, 0x49, 0x83, 0xe0, 0x0f, 0x74, 0x06, 0x42, 0x0f,
    0x11, 0x44, 0x01, 0xf0, 0xc3};

bool matches(const BinaryImage &Image, va_t Target) {
  if (Target > std::numeric_limits<va_t>::max() - sizeof(FillBody) + 1)
    return false;
  const auto *Segment = Image.getSegmentFor(Target);
  if (!Segment || !Segment->isExecutable() || Segment->isWritable() ||
      !Segment->contains(Target + sizeof(FillBody) - 1))
    return false;
  const auto *Bytes = Image.readVA(Target, sizeof(FillBody));
  return Bytes && std::equal(std::begin(FillBody), std::end(FillBody), Bytes);
}

using Key = std::tuple<MedVar::VarKind, int, int>;
Key key(const MedVar &V) { return {V.Kind, V.Id, V.SSAVer}; }
} // namespace

void recoverX64FillReturns(MedFunc &Function, const BinaryImage &Image) {
  if (Image.Arch != Arch::X64 || Image.abiFormat() != BinaryFormat::COFF ||
      Function.SkippedSSA || Function.SourceParametersBound ||
      Function.AnalystStackFrame || Function.CallClobbers.empty())
    return;
  std::map<uint32_t, std::vector<MedVar>> Outputs;
  for (const auto &Clobber : Function.CallClobbers)
    if (Clobber.CallSiteId && !Clobber.PreservedPrefixSize &&
        Clobber.Value.Kind == MedVar::Reg &&
        Clobber.Value.RegOff == x86reg::RCX && Clobber.Value.Size == 8)
      Outputs[Clobber.CallSiteId].push_back(Clobber.Value);
  if (Outputs.empty())
    return;
  std::map<va_t, bool> Known;
  std::map<std::pair<int, int>, const MedCallInfo *> Bound;
  for (const auto &Call : Function.CallInfos)
    Bound.emplace(std::pair{Call.BlockId, Call.OpIdx}, &Call);
  std::map<const MedOp *, std::pair<MedVar, MedVar>> Calls;
  for (const auto &Block : Function.Blocks)
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const auto &Call = Block.Ops[I];
      if (!Outputs.count(Call.CallSiteId) || Call.Opcode != NdOp::CALL ||
          Call.NumInputs != 4 || !Call.Inputs[0].isConst() ||
          Call.SourceCallHint || Call.ScalarCallSignature ||
          Call.IntegerCallSignature || Call.IsTailCall || Call.DoesNotReturn ||
          Call.Inputs[1].Size != 8 || Call.Inputs[3].Size != 8)
        continue;
      auto Destination = Call.Inputs[1], Count = Call.Inputs[3];
      if (const auto At = Bound.find({Block.Id, I}); At != Bound.end()) {
        const auto &Source = *At->second;
        if (Source.TargetAddr != Call.Inputs[0].ConstVal || Source.IsIndirect ||
            Source.SourceCallHint || Source.ArgumentsInCalleeRegisterOrder ||
            Source.Args.size() != 3 || Source.Args[0].Size != 8 ||
            Source.Args[2].Size != 8)
          continue;
        // Recovered source operands own the emitted call. A propagated or
        // edited operand must not leave the scratch result bound to old SSA.
        Destination = Source.Args[0];
        Count = Source.Args[2];
      }
      auto [Body, New] = Known.try_emplace(Call.Inputs[0].ConstVal);
      if (New)
        Body->second = matches(Image, Body->first);
      if (Body->second)
        Calls.emplace(&Call, std::pair{Destination, Count});
    }
  if (Calls.empty())
    return;
  std::set<Key> Candidates, Used, Counts;
  for (const auto &[Call, Inputs] : Calls) {
    if (!Inputs.second.isConst())
      Counts.insert(key(Inputs.second));
    for (const auto &V : Outputs.at(Call->CallSiteId))
      Candidates.insert(key(V));
  }
  std::map<Key, const MedOp *> CountDefinitions;
  const auto DefineCount = [&](const MedVar &V, const MedOp *Op) {
    if (Counts.count(key(V))) {
      auto [At, New] = CountDefinitions.emplace(key(V), Op);
      if (!New)
        At->second = nullptr;
    }
  };
  const auto ScalarConstant = [&](const MedVar &V) {
    return V.isConst() && !isAddressProvenance(V.Provenance) &&
           !(V.Provenance == ConstantAddressProvenance::Unknown &&
             Image.isPotentiallyRelocatableAddress(V.ConstVal));
  };
  // The result expression needs fresh SSA temporaries. Index once;
  // the output remains the exact existing call-clobber value.
  int NextId = 0;
  const auto Note = [&](const MedVar &V) {
    NextId = std::max(NextId, V.Id + 1);
  };
  const auto Read = [&](const MedVar &V) {
    Note(V);
    if (Candidates.count(key(V)))
      Used.insert(key(V));
  };
  for (const auto &Block : Function.Blocks) {
    for (const auto &Phi : Block.Phis) {
      Note(Phi.Output);
      DefineCount(Phi.Output, nullptr);
      for (const auto &[Pred, V] : Phi.Args)
        Read(V);
    }
    for (const auto &Op : Block.Ops) {
      Note(Op.Output);
      DefineCount(Op.Output, &Op);
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        Read(Op.Inputs[I]);
      for (const auto &V : Op.IntrinsicOutputs) {
        Note(V);
        DefineCount(V, nullptr);
      }
    }
  }
  for (const auto &Clobber : Function.CallClobbers) {
    Note(Clobber.Value);
    DefineCount(Clobber.Value, nullptr);
    Read(Clobber.PreservedInput);
  }
  for (const auto &Call : Function.CallInfos)
    for (const auto &V : Call.Args)
      Read(V);
  for (const auto &V : Function.Params)
    Note(V);
  for (const auto &V : Function.Locals)
    Note(V);
  if (Used.empty())
    return;
  for (auto &[Call, Inputs] : Calls) {
    auto &Count = Inputs.second;
    const auto At = CountDefinitions.find(key(Count));
    if (Count.isConst() || At == CountDefinitions.end() || !At->second)
      continue;
    const auto &Op = *At->second;
    if (Op.NumInputs != 1 || Op.Output.Size != Count.Size ||
        !Op.Inputs[0].Size || Op.Inputs[0].Size > Count.Size ||
        !ScalarConstant(Op.Inputs[0]) ||
        !((Op.Opcode == NdOp::COPY && Op.Inputs[0].Size == Count.Size) ||
          Op.Opcode == NdOp::INT_ZEXT))
      continue;
    // A named register can retain a literal COPY/ZEXT after source binding.
    // Index only the requested counts during the existing value-use walk.
    const auto &Input = Op.Inputs[0];
    const uint64_t Mask = Input.Size == sizeof(uint64_t)
                              ? ~uint64_t{0}
                              : (uint64_t{1} << (Input.Size * 8)) - 1;
    Count = MedVar::makeConst(Input.ConstVal & Mask, sizeof(uint64_t),
                              ConstantAddressProvenance::Scalar);
  }
  for (auto &[Site, Values] : Outputs)
    llvm::erase_if(Values,
                   [&](const MedVar &V) { return !Used.count(key(V)); });
  std::set<Key> Recovered;
  std::map<std::pair<int, int>, int> Positions;
  for (auto &Block : Function.Blocks) {
    if (llvm::none_of(Block.Ops, [&](const MedOp &Op) {
          return Calls.count(&Op) && !Outputs.at(Op.CallSiteId).empty();
        }))
      continue;
    std::vector<MedOp> Ops;
    Ops.reserve(Block.Ops.size());
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const auto &Call = Block.Ops[I];
      if (Call.Opcode == NdOp::CALL || Call.Opcode == NdOp::INDIR_CALL)
        Positions[{Block.Id, I}] = Ops.size();
      Ops.push_back(Call);
      if (!Calls.count(&Call))
        continue;
      const auto At = Outputs.find(Call.CallSiteId);
      if (At->second.empty())
        continue;
      // These are instruction constants in the checked body, not heuristic
      // limits. Small fills leave RCX unchanged; scalar loops advance by full
      // words. The SSE loops end at floor((destination + count) / 16) * 16.
      // This fact describes normal returns only. The original call still
      // performs all writes, and any fault prevents these operations running.
      const auto Constant = [](uint64_t V) {
        return MedVar::makeConst(V, 8, ConstantAddressProvenance::Scalar);
      };
      const auto Append = [&](NdOp Opcode, std::initializer_list<MedVar> Inputs,
                              uint16_t Size = 8) {
        MedOp Op;
        Op.Opcode = Opcode;
        Op.Output.Kind = MedVar::Temp;
        Op.Output.Id = NextId++;
        Op.Output.Size = Size;
        Op.Output.TheArch = Arch::X64;
        Op.NumInputs = Inputs.size();
        Op.Inputs.assign(Inputs.begin(), Inputs.end());
        Op.Addr = Call.Addr;
        Ops.push_back(Op);
        return Op.Output;
      };
      const auto [Destination, Count] = Calls.at(&Call);
      MedVar Value = Destination;
      if (ScalarConstant(Count)) {
        // Source call binding has already propagated common fixed-size fills.
        // Do not manufacture dead branches for later whole-function passes.
        if (Count.ConstVal >= 0x4f) {
          const auto End = Append(NdOp::INT_ADD, {Destination, Count});
          Value = Append(NdOp::INT_AND, {End, Constant(~uint64_t{15})});
        } else if (Count.ConstVal >= 8) {
          Value =
              Append(NdOp::INT_ADD,
                     {Destination, Constant(Count.ConstVal & ~uint64_t{7})});
        }
      } else {
        const auto End = Append(NdOp::INT_ADD, {Destination, Count});
        const auto Vector =
            Append(NdOp::INT_AND, {End, Constant(~uint64_t{15})});
        const auto Words =
            Append(NdOp::INT_AND, {Count, Constant(~uint64_t{7})});
        const auto Scalar = Append(NdOp::INT_ADD, {Destination, Words});
        const auto Small = Append(NdOp::INT_LESS, {Count, Constant(8)}, 1);
        const auto ScalarCount =
            Append(NdOp::INT_LESS, {Count, Constant(0x4f)}, 1);
        const auto Short = Append(NdOp::SELECT, {Small, Destination, Scalar});
        Value = Append(NdOp::SELECT, {ScalarCount, Short, Vector});
      }
      for (const auto &Output : At->second) {
        MedOp Result;
        Result.Opcode = NdOp::COPY;
        Result.Output = Output;
        Result.NumInputs = 1;
        Result.Inputs[0] = Value;
        Result.Addr = Call.Addr;
        Ops.push_back(Result);
        Recovered.insert(key(Output));
      }
    }
    Block.Ops = std::move(Ops);
  }
  for (auto &Call : Function.CallInfos)
    if (const auto At = Positions.find({Call.BlockId, Call.OpIdx});
        At != Positions.end())
      Call.OpIdx = At->second;
  llvm::erase_if(Function.CallClobbers, [&](const MedCallClobber &Clobber) {
    return Recovered.count(key(Clobber.Value));
  });
}
} // namespace neverd
