//===- SwiftRuntimeSource.cpp - Bounded native compiler-entry effects
//------===//
#include "neverd/loader/Swift/SwiftRuntimeSource.h"

#include "../ObjC/ObjCRuntimeData.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Swift/SwiftMetadata.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace neverd {
namespace {
struct Unproven : std::runtime_error {
  using std::runtime_error::runtime_error;
};
using Kind = SwiftRuntimeSourceKind;
using Identity = SwiftRuntimeSourceIdentity;
using Key = std::tuple<VnodeSpace, uint64_t, uint16_t>;

std::string normalized(std::string Name) {
  if (Name.starts_with("_"))
    Name.erase(0, 1);
  return Name;
}
bool identifier(const std::string &Name) {
  return !Name.empty() && Name.size() <= 255 &&
         (std::isalpha(static_cast<unsigned char>(Name[0])) ||
          Name[0] == '_') &&
         std::all_of(Name.begin(), Name.end(), [](unsigned char C) {
           return C < 128 && (std::isalnum(C) || C == '_');
         });
}
std::string contextPrefix(const SwiftSourceSignature &S) {
  if (!identifier(S.Module) || !identifier(S.ContextName) ||
      (S.ContextKind != "class" && S.ContextKind != "struct"))
    throw Unproven(
        "runtime source projection requires one named nongeneric context");
  return "$s" + std::to_string(S.Module.size()) + S.Module +
         std::to_string(S.ContextName.size()) + S.ContextName +
         (S.ContextKind == "class" ? "C" : "V");
}

// Values retain origin and width. An unknown scalar may be discarded, but can
// never reach a modeled memory write, runtime call, or observable return.
struct Value {
  enum Tag {
    Unknown,
    Constant,
    Incoming,
    Parameter,
    Stack,
    Self,
    Metatype,
    Allocation,
    MetadataResult
  } T = Unknown;
  uint64_t Identity = 0;
  int64_t Offset = 0;
  unsigned Bytes = 8;
  bool operator==(const Value &) const = default;
};
Value constant(uint64_t Bits, unsigned Bytes = 8) {
  if (!Bytes || Bytes > 8)
    throw Unproven("runtime scalar width is unsupported");
  if (Bytes < 8)
    Bits &= (uint64_t(1) << (Bytes * 8)) - 1;
  return {Value::Constant, Bits, 0, Bytes};
}
bool isConstant(Value V, uint64_t Bits) {
  return V.T == Value::Constant && V.Identity == Bits;
}
struct Write {
  int64_t Offset;
  unsigned Bytes;
  Value Stored;
  bool operator==(const Write &) const = default;
};
struct Effects {
  Value First, Second;
  std::vector<Write> Writes;
  unsigned Calls = 0;
};

class NativeFlow {
  const BinaryImage &Image;
  const TargetRegInfo &Regs;
  const SwiftRecoveredType &Type;
  std::optional<Kind> Model;
  std::map<Key, Value> Values;
  std::map<std::pair<int64_t, unsigned>, Value> Frame;
  std::map<uint64_t, Value> Preserved;
  std::vector<Write> Writes;
  int64_t LowestSP = 0, LowestWrite = 0;
  std::optional<std::pair<va_t, int64_t>> PendingAllocation;
  unsigned Calls = 0;
  bool Returned = false;

  Value get(const NdVar &V) const {
    if (V.isConst())
      return constant(V.Offset, V.Size);
    if ((!V.isReg() && !V.isTemp()) || !V.Size || V.Size > 8)
      throw Unproven("runtime proof has an unsupported native value");
    if (auto It = Values.find({V.Space, V.Offset, V.Size}); It != Values.end())
      return It->second;
    for (const auto &[K, Entry] : Values) {
      auto [Space, Offset, Size] = K;
      if (Space != V.Space || V.Offset < Offset || V.Offset - Offset >= Size ||
          V.Size > Size - (V.Offset - Offset))
        continue;
      if (Entry.T == Value::Constant)
        return constant(Entry.Identity >> ((V.Offset - Offset) * 8), V.Size);
      // Partial pointers and partial identities cannot establish an exact ABI
      // input. The supported constructor model deliberately uses full words.
      return {};
    }
    return {};
  }
  void put(const NdVar &V, Value X) {
    if ((!V.isReg() && !V.isTemp()) || !V.Size || V.Size > 8 ||
        V.Offset > UINT64_MAX - V.Size)
      throw Unproven("runtime proof has an invalid native destination");
    for (auto It = Values.begin(); It != Values.end();) {
      auto [Space, Offset, Size] = It->first;
      if (Space == V.Space && Offset < V.Offset + V.Size &&
          V.Offset < Offset + Size)
        It = Values.erase(It);
      else
        ++It;
    }
    Values[{V.Space, V.Offset, V.Size}] = X;
    if (V.isReg() && V.Offset == Regs.StackPointer) {
      if (X.T != Value::Stack || X.Bytes != 8 || X.Offset < -4096 ||
          X.Offset > 0)
        throw Unproven("runtime proof has an unexplained stack transition");
      LowestSP = std::min(LowestSP, X.Offset);
      for (auto It = Frame.begin(); It != Frame.end();) {
        if (It->first.first < X.Offset)
          It = Frame.erase(It);
        else
          ++It;
      }
    }
  }
  Value resized(Value V, unsigned InputBytes, unsigned OutputBytes,
                bool ZeroExtend) {
    if (V.T == Value::Constant)
      return constant(V.Identity, OutputBytes);
    if (InputBytes != OutputBytes || (ZeroExtend && V.Bytes != OutputBytes))
      return {};
    return V;
  }
  Value arithmetic(const LowOp &Op) {
    auto A = get(Op.Inputs[0]), B = get(Op.Inputs[1]);
    if (Op.Opcode == NdOp::INT_ADD && A.T == Value::Constant)
      std::swap(A, B);
    if (A.T == Value::Constant && B.T == Value::Constant)
      return constant(Op.Opcode == NdOp::INT_SUB ? A.Identity - B.Identity
                                                 : A.Identity + B.Identity,
                      Op.Output.Size);
    if ((A.T == Value::Stack || A.T == Value::Self ||
         A.T == Value::Allocation) &&
        B.T == Value::Constant && Op.Output.Size == 8) {
      // A narrow integer payload is not evidence of sign extension. Negative
      // address displacements must already carry their full machine width.
      int64_t Delta = static_cast<int64_t>(B.Identity);
      if (B.Bytes < 8 && (B.Identity & (uint64_t(1) << (B.Bytes * 8 - 1))))
        throw Unproven(
            "runtime address displacement has an ambiguous narrow sign");
      if (Delta < -1048576 || Delta > 1048576 || A.Offset < -1048576 ||
          A.Offset > 1048576)
        throw Unproven("runtime address arithmetic exceeds its bound");
      A.Offset += Op.Opcode == NdOp::INT_SUB ? -Delta : Delta;
      return A;
    }
    return {};
  }
  Value load(Value Address, unsigned Bytes) const {
    if (Address.T != Value::Stack)
      throw Unproven(
          "canonical runtime projection has an unmodeled memory read");
    const auto SP = get(NdVar::reg(Regs.StackPointer, 8));
    if (SP.T != Value::Stack || Address.Offset < SP.Offset)
      throw Unproven(
          "runtime projection reads outside the currently allocated frame");
    auto It = Frame.find({Address.Offset, Bytes});
    if (It == Frame.end())
      throw Unproven("runtime projection reads an undefined stack slot");
    return It->second;
  }
  void store(Value Address, unsigned Bytes, Value Stored, va_t Instruction) {
    if (Address.T == Value::Stack) {
      if (Address.Offset < -4096 || Address.Offset >= 0 ||
          Bytes > uint64_t(-Address.Offset))
        throw Unproven(
            "runtime projection writes outside a bounded private frame");
      LowestWrite = std::min(LowestWrite, Address.Offset);
      const auto SP = get(NdVar::reg(Regs.StackPointer, 8));
      if (SP.T != Value::Stack)
        throw Unproven("runtime frame store has no established stack pointer");
      if (Address.Offset < SP.Offset) {
        if (Image.Arch != Arch::AArch64)
          throw Unproven("runtime frame store precedes its stack allocation");
        if (!PendingAllocation)
          PendingAllocation = {Instruction, Address.Offset};
        else
          PendingAllocation->second =
              std::min(PendingAllocation->second, Address.Offset);
      }
      for (auto It = Frame.begin(); It != Frame.end();) {
        if (It->first.first < Address.Offset + Bytes &&
            Address.Offset < It->first.first + It->first.second)
          It = Frame.erase(It);
        else
          ++It;
      }
      Frame[{Address.Offset, Bytes}] = Stored;
      return;
    }
    const auto Expected =
        Model == Kind::AllocatingInitializer ? Value::Allocation : Value::Self;
    if ((Model && Model != Kind::AllocatingInitializer) ||
        Address.T != Expected || Stored.T == Value::Unknown ||
        (Stored.T != Value::Parameter && Stored.T != Value::Constant) ||
        Stored.Bytes != Bytes)
      throw Unproven("runtime projection has an unmodeled object write");
    auto It = std::find_if(Type.Fields.begin(), Type.Fields.end(),
                           [&](const SwiftStorageField &F) {
                             return Address.Offset >= 0 &&
                                    F.Offset == uint64_t(Address.Offset) &&
                                    F.Type.TheKind ==
                                        SwiftSourceType::Kind::Integer &&
                                    F.Type.Bits == Bytes * 8;
                           });
    if (It == Type.Fields.end())
      throw Unproven("initializer store does not match a native integer field");
    Writes.push_back({Address.Offset, Bytes, Stored});
  }
  void finishAllocation() {
    if (!PendingAllocation)
      return;
    const auto SP = get(NdVar::reg(Regs.StackPointer, 8));
    if (SP.T != Value::Stack || SP.Offset > PendingAllocation->second)
      throw Unproven("pre-index frame stores lack an SP update in the same "
                     "native instruction");
    PendingAllocation.reset();
  }
  void frameIsRestored() const {
    auto SP = get(NdVar::reg(Regs.StackPointer, 8));
    if (SP.T != Value::Stack || SP.Offset || LowestWrite < LowestSP)
      throw Unproven(
          "runtime projection has an unbalanced or unallocated private frame");
    for (const auto &[Register, Expected] : Preserved)
      if (get(NdVar::reg(Register, 8)) != Expected)
        throw Unproven(
            "runtime projection does not restore a callee-saved register");
  }
  void boundary(const LowBlock &B, size_t Index, bool Tail, va_t Target) const {
    const auto &Op = B.Ops[Index];
    const LowInstructionBoundary *Found = nullptr;
    for (const auto &Insn : B.InstructionBoundaries) {
      if (Insn.FirstOp > Index || Index - Insn.FirstOp >= Insn.OpCount)
        continue;
      if (Found)
        throw Unproven("runtime call has overlapping instruction boundaries");
      Found = &Insn;
    }
    const auto Flags = Tail ? LowInstructionControlFlag::Call |
                                  LowInstructionControlFlag::Return
                            : LowInstructionControlFlag::Call;
    if (!Found || Found->Address != Op.Addr || !Found->Size ||
        Found->Control != (Tail ? LowInstructionControl::TailCall
                                : LowInstructionControl::Call) ||
        Found->ControlFlags != Flags ||
        Found->Mode != InstructionMode::Default ||
        Found->TargetMode != LowInstructionTargetMode::Preserve ||
        !Found->Immediate || *Found->Immediate != Target ||
        Found->FirstOp > B.Ops.size() ||
        Found->OpCount > B.Ops.size() - Found->FirstOp)
      throw Unproven("runtime call lacks an exact native instruction target");
    if (Tail &&
        (Index + 2 != B.Ops.size() || Found->FirstOp != Index ||
         Found->OpCount != 2 || B.Ops[Index + 1].Opcode != NdOp::RETURN ||
         B.Ops[Index + 1].Addr != Op.Addr || B.Ops[Index + 1].NumInputs != 1 ||
         B.Ops[Index + 1].Inputs[0] != Op.Output))
      throw Unproven(
          "runtime deallocation is not an exact terminal tail transfer");
  }
  void call(const LowBlock &B, size_t Index) {
    const auto &Op = B.Ops[Index];
    if (!Model || ++Calls != 1 || Op.NumInputs != 1 ||
        !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 8 ||
        Op.Output != NdVar::reg(Regs.IntReturnReg, 8))
      throw Unproven("runtime projection has an unexpected native call");
    const auto Target = Op.Inputs[0].Offset;
    const auto *Import = Image.findImportStubAt(Target);
    if (!Image.isCodeAddress(Target) || !Import)
      throw Unproven(
          "runtime call target lacks an exact loader-owned executable import");
    const auto Name = normalized(Import->Name);
    const auto ExpectedModule = Name == "objc_opt_self"
                                    ? "/usr/lib/libobjc.A.dylib"
                                    : "/usr/lib/swift/libswiftCore.dylib";
    if (Import->Module != ExpectedModule)
      throw Unproven(
          "runtime import is not bound to the modeled Darwin runtime library");
    const bool Tail = *Model == Kind::DeallocatingDestructor;
    boundary(B, Index, Tail, Target);
    const auto SP = get(NdVar::reg(Regs.StackPointer, 8));
    if (SP.T != Value::Stack ||
        (!Tail && (static_cast<uint64_t>(SP.Offset) & 15) !=
                      (Image.Arch == Arch::AArch64 ? 0u : 8u)))
      throw Unproven(
          "runtime call does not preserve its native stack alignment");
    auto Arg = [&](unsigned N) {
      return get(NdVar::reg(Regs.IntParamRegs[N], 8));
    };
    Value Result;
    if (*Model == Kind::AllocatingInitializer || Tail) {
      const auto Expected = Tail ? Value::Self : Value::Metatype;
      if (Name != (Tail ? "swift_deallocClassInstance" : "swift_allocObject") ||
          Arg(0).T != Expected || Arg(0).Offset ||
          !isConstant(Arg(1), Type.Size) || !Type.Alignment ||
          !isConstant(Arg(2), Type.Alignment - 1))
        throw Unproven("runtime allocation/deallocation arguments disagree "
                       "with native type layout");
      if (Tail)
        frameIsRestored();
      Result = {Value::Allocation, 0, 0, 8};
    } else if (*Model == Kind::TypeMetadataAccessor && Type.Kind == "class") {
      if (Name != "objc_opt_self" || !isConstant(Arg(0), Type.Metadata))
        throw Unproven("class metadata accessor does not preserve its exact "
                       "runtime lookup");
      // Preserve the runtime result. objc_opt_self is not an unconditional
      // identity operation and must not be replaced with the input address.
      Result = {Value::MetadataResult, Type.Metadata, 0, 8};
    } else
      throw Unproven("runtime projection has an unmodeled call effect");
    for (auto It = Values.begin(); It != Values.end();) {
      auto [Space, Offset, Size] = It->first;
      if (Space == VnodeSpace::REG && Offset != Regs.StackPointer &&
          !Preserved.count(Offset))
        It = Values.erase(It);
      else
        ++It;
    }
    put(Op.Output, Result);
    if (!Tail && Image.Arch == Arch::AArch64)
      put(NdVar::reg(Regs.LinkRegister, 8), {});
  }
  void nativeReturn(const LowBlock &B, size_t Index) const {
    const auto &Op = B.Ops[Index];
    if (Model == Kind::DeallocatingDestructor && Index &&
        B.Ops[Index - 1].Opcode == NdOp::CALL &&
        B.Ops[Index - 1].Addr == Op.Addr)
      return; // Already proven exact tail pair.
    if (Op.NumInputs != 1 ||
        Op.Inputs[0] != NdVar::reg(Image.Arch == Arch::AArch64
                                       ? Regs.LinkRegister
                                       : Regs.IntReturnReg,
                                   8))
      throw Unproven("runtime entry has an unexplained native return carrier");
    const LowInstructionBoundary *Found = nullptr;
    for (const auto &Insn : B.InstructionBoundaries) {
      if (Insn.FirstOp > Index || Index - Insn.FirstOp >= Insn.OpCount)
        continue;
      if (Found)
        throw Unproven("runtime return has overlapping instruction boundaries");
      Found = &Insn;
    }
    if (!Found || Found->Address != Op.Addr || !Found->Size ||
        Found->Control != LowInstructionControl::Return ||
        Found->ControlFlags != LowInstructionControlFlag::Return ||
        Found->Mode != InstructionMode::Default ||
        (Found->Immediate && *Found->Immediate != 0) || Found->OpCount != 1 ||
        Found->FirstOp != Index)
      throw Unproven(
          "runtime entry lacks an exact ordinary native return boundary");
  }

public:
  NativeFlow(const BinaryImage &I, const SwiftRecoveredType &T,
             std::optional<Kind> K,
             const SwiftSourceSignature *Initializer = nullptr)
      : Image(I), Regs(getTargetRegInfo(I.Arch)), Type(T), Model(K) {
    for (const auto &Range : Regs.callPreservedRanges(I.Format)) {
      if (Range.Offset == Regs.StackPointer)
        continue;
      if (Range.Bytes != 8)
        throw Unproven(
            "runtime proof has an unsupported preserved register width");
      Value V{Value::Incoming, Range.Offset, 0, 8};
      put(NdVar::reg(Range.Offset, 8), V);
      Preserved[Range.Offset] = V;
    }
    put(NdVar::reg(Regs.StackPointer, 8), {Value::Stack, 0, 0, 8});
    const auto SelfReg = I.Arch == Arch::AArch64 ? a64reg::X20 : reg::R13;
    Value Self{K == Kind::AllocatingInitializer ? Value::Metatype : Value::Self,
               0, 0, 8};
    put(NdVar::reg(SelfReg, 8), Self);
    Preserved[SelfReg] = Self;
    if (I.Arch == Arch::AArch64) {
      Value LR{Value::Incoming, Regs.LinkRegister, 0, 8};
      put(NdVar::reg(Regs.LinkRegister, 8), LR);
      Preserved[Regs.LinkRegister] = LR;
    }
    if (Initializer) {
      if (Initializer->Parameters.size() > Regs.IntParamRegs.size())
        throw Unproven("canonical allocator currently requires register-only "
                       "word arguments");
      for (size_t N = 0; N < Initializer->Parameters.size(); ++N) {
        const auto &P = Initializer->Parameters[N];
        if (P.Type.TheKind != SwiftSourceType::Kind::Integer ||
            P.Type.Bits != 64)
          throw Unproven("canonical allocator currently requires 64-bit "
                         "integer arguments");
        put(NdVar::reg(Regs.IntParamRegs[N], 8), {Value::Parameter, N, 0, 8});
      }
    }
  }
  Effects run(const LowFunc &F) {
    if (F.Blocks.size() != 1 || F.Blocks.front().StartAddr != F.Entry ||
        !F.Blocks.front().Succs.empty() ||
        !F.Blocks.front().ExceptionalSuccs.empty() ||
        !F.Blocks.front().ExceptionalPreds.empty() ||
        F.Blocks.front().Ops.size() > 4096)
      throw Unproven("runtime projection requires one bounded nonexceptional "
                     "native block");
    const auto &B = F.Blocks.front();
    for (size_t Index = 0; Index < B.Ops.size(); ++Index) {
      const auto &Op = B.Ops[Index];
      if (PendingAllocation && PendingAllocation->first != Op.Addr)
        finishAllocation();
      if (Returned || Op.MemoryOrdering != NdMemoryOrdering::None ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        throw Unproven("runtime projection has an effect after return or "
                       "nonordinary memory ordering");
      auto arity = [&](unsigned N) {
        if (Op.NumInputs != N)
          throw Unproven("malformed runtime value operation");
      };
      switch (Op.Opcode) {
      case NdOp::NOP:
        arity(0);
        break;
      case NdOp::COPY:
      case NdOp::INT_ZEXT:
        arity(1);
        put(Op.Output, resized(get(Op.Inputs[0]), Op.Inputs[0].Size,
                               Op.Output.Size, Op.Opcode == NdOp::INT_ZEXT));
        break;
      case NdOp::INT_ADD:
      case NdOp::INT_SUB:
        arity(2);
        put(Op.Output, arithmetic(Op));
        break;
      case NdOp::LOAD:
        arity(1);
        put(Op.Output, load(get(Op.Inputs[0]), Op.Output.Size));
        break;
      case NdOp::STORE:
        arity(2);
        store(get(Op.Inputs[0]), Op.Inputs[1].Size, get(Op.Inputs[1]), Op.Addr);
        break;
      case NdOp::CALL:
        finishAllocation();
        call(B, Index);
        break;
      case NdOp::RETURN:
        finishAllocation();
        nativeReturn(B, Index);
        frameIsRestored();
        Returned = true;
        break;
      // These operations only produce scalar values/flags. Their exact bits
      // are unnecessary unless consumed, in which case Unknown rejects proof.
      case NdOp::INT_EQUAL:
      case NdOp::INT_NOTEQUAL:
      case NdOp::INT_SLESS:
      case NdOp::INT_LESS:
      case NdOp::INT_CARRY:
      case NdOp::INT_SOVF:
      case NdOp::INT_SBOR:
      case NdOp::INT_AND:
      case NdOp::INT_OR:
      case NdOp::INT_XOR:
      case NdOp::SUBBYTES:
        arity(2);
        (void)get(Op.Inputs[0]);
        (void)get(Op.Inputs[1]);
        put(Op.Output, {});
        break;
      case NdOp::POPCOUNT:
        arity(1);
        (void)get(Op.Inputs[0]);
        put(Op.Output, {});
        break;
      default:
        throw Unproven("runtime projection has an unmodeled native operation");
      }
    }
    if (!Returned)
      throw Unproven("runtime projection has no terminal native return");
    return {get(NdVar::reg(Regs.IntReturnReg, 8)),
            get(NdVar::reg(Regs.IntReturnRegs[1], 8)), Writes, Calls};
  }
};

class Proof {
  const SwiftRuntimeSourceRequest &R;
  const BinaryImage &Image;
  const std::vector<LowFunc> &Functions;
  SwiftRecoveredType Type;
  SwiftRuntimeSourceProof Result;
  std::string Prefix;

  const LowFunc &body(const Identity &ID) const {
    if (!ID.Entry || !Image.isCodeAddress(ID.Entry) ||
        Image.findImportAt(ID.Entry))
      throw Unproven("runtime entry is not native executable code");
    bool Matched = false;
    for (const auto &Sym : Image.Symbols) {
      if (normalized(Sym.Name) != normalized(ID.MangledSymbol))
        continue;
      if (!Sym.IsFunc || Sym.Addr != ID.Entry)
        throw Unproven("runtime symbol identity is ambiguous");
      Matched = true;
    }
    if (!Matched)
      throw Unproven("runtime entry has no matching native symbol");
    const LowFunc *F = nullptr;
    for (const auto &Candidate : Functions) {
      if (Candidate.Entry != ID.Entry)
        continue;
      if (F)
        throw Unproven("runtime entry has ambiguous native bodies");
      F = &Candidate;
    }
    if (!F)
      throw Unproven("runtime entry has no inspected native body");
    return *F;
  }
  Identity identity() const {
    return {R.Signature.Entry, R.Signature.MangledSymbol};
  }
  void dependency(std::string K, std::string Name = {}, Identity ID = {}) {
    Result.Dependencies.push_back({std::move(K), Type.Module, Type.Kind,
                                   Type.Name, std::move(Name), std::move(ID)});
  }
  void exactRole(const Identity &ID, const std::string &Name) const {
    if (normalized(ID.MangledSymbol) != Name)
      throw Unproven(
          "runtime identity disagrees with its context and compiler role");
  }
  const Identity &related() const {
    if (!R.RelatedEntry)
      throw Unproven("runtime projection is missing its related native entry");
    return *R.RelatedEntry;
  }
  void noEffects(const Effects &E) const {
    if (E.Calls || !E.Writes.empty())
      throw Unproven("trivial runtime entry has observable effects");
  }
  void destructor(const Identity &ID) const {
    exactRole(ID, Prefix + "fd");
    auto E = NativeFlow(Image, Type, Kind::TrivialDestructor).run(body(ID));
    noEffects(E);
    if (E.First != Value{Value::Self, 0, 0, 8})
      throw Unproven(
          "trivial destructor does not return its original instance");
  }
  void modifier(const Identity &Modify, const Identity &Resume) {
    if (Type.Kind != "struct" || !identifier(R.Signature.Name))
      throw Unproven("modify projection requires a native struct property");
    auto Field = std::find_if(
        Type.Fields.begin(), Type.Fields.end(),
        [&](const SwiftStorageField &F) { return F.Name == R.Signature.Name; });
    if (Field == Type.Fields.end() || !Field->IsMutable ||
        Field->Type.TheKind != SwiftSourceType::Kind::Integer ||
        Field->Type.Bits != 64)
      throw Unproven(
          "modify projection requires one exact mutable 64-bit integer field");
    const auto PropertyName =
        Prefix + std::to_string(Field->Name.size()) + Field->Name + "s" +
        std::to_string(Field->Type.Name.size()) + Field->Type.Name + "VvM";
    exactRole(Modify, PropertyName);
    exactRole(Resume, PropertyName + ".resume.0");
    auto E = NativeFlow(Image, Type, Kind::ModifyAccessor).run(body(Modify));
    noEffects(E);
    if (!isConstant(E.First, Resume.Entry) ||
        E.Second !=
            Value{Value::Self, 0, static_cast<int64_t>(Field->Offset), 8})
      throw Unproven("modify return does not bind the exact continuation and "
                     "mutable field address");
    noEffects(NativeFlow(Image, Type, Kind::ModifyResume).run(body(Resume)));
    Result.FieldOffset = Field->Offset;
    Result.Continuation = Resume;
    dependency("property", Field->Name);
    dependency("compiler_entry", Field->Name,
               R.Kind == Kind::ModifyAccessor ? Resume : Modify);
    Result.Evidence.push_back(
        "The modifier returns the exact related continuation and "
        "metadata-confirmed mutable field address; the continuation has no "
        "observable effects.");
  }

public:
  Proof(const SwiftRuntimeSourceRequest &Request, const BinaryImage &I,
        const std::vector<LowFunc> &F)
      : R(Request), Image(I), Functions(F) {}
  SwiftRuntimeSourceProof run() {
    if (!Image.isMachO() || Image.Bits != Bitness::Bits64 ||
        Image.IsRelocatable ||
        (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64))
      throw Unproven("runtime source projection requires linked 64-bit Darwin "
                     "native code");
    Prefix = contextPrefix(R.Signature);
    bool Found = false;
    for (const auto &Candidate : recoverSwiftTypes(Image)) {
      if (Candidate.Module != R.Signature.Module ||
          Candidate.Kind != R.Signature.ContextKind ||
          Candidate.Name != R.Signature.ContextName)
        continue;
      if (Found || Candidate.Status != "recovered")
        throw Unproven("runtime source context has ambiguous or unsupported "
                       "native metadata");
      Found = true;
      Type = Candidate;
    }
    if (!Found)
      throw Unproven(
          "runtime source context lacks native fixed-layout metadata");
    if (!Type.Alignment || (Type.Alignment & (Type.Alignment - 1)) ||
        (Type.Kind == "class" && Type.Alignment != 8))
      throw Unproven(
          "runtime source context requires an unsupported storage alignment");
    const auto &F = body(identity());
    Result.Descriptor = Type.Descriptor;
    Result.Metadata = Type.Metadata;
    dependency("context");
    switch (R.Kind) {
    case Kind::EmptyValueInitializer: {
      const auto &S = R.Signature;
      if (Type.Kind != "struct" || Type.Size != 0 || Type.Alignment != 1 ||
          !Type.Fields.empty() || S.DeclarationKind != "initializer" ||
          S.Name != "init" || S.IsStatic || S.IsMutating ||
          !S.IsMutatingKnown || !S.Parameters.empty() || !S.Labels.empty() ||
          S.ReturnType.TheKind != SwiftSourceType::Kind::Void || R.Initializer)
        throw Unproven("empty value initializer requires exact zero-size "
                       "native storage and no arguments");
      exactRole(identity(), Prefix + "ACycfC");
      const auto Accessor = related();
      exactRole(Accessor, Prefix + "Ma");
      objc::RuntimeData Data(Image);
      if (Type.Descriptor > InvalidVA - 12)
        throw Unproven("empty value initializer descriptor address overflows");
      const auto Slot = Type.Descriptor + 12;
      auto Offset = Data.u32(Slot);
      const int64_t Delta = Offset ? static_cast<int32_t>(*Offset) : 0;
      if (!Offset || !*Offset ||
          (Delta < 0 && Slot < uint64_t(-Delta)) ||
          (Delta > 0 && Slot > InvalidVA - Delta) ||
          (Delta < 0 ? Slot - uint64_t(-Delta) : Slot + Delta) !=
              Accessor.Entry)
        throw Unproven("empty value initializer has no descriptor-owned "
                       "metadata accessor dependency");
      auto E = NativeFlow(Image, Type, R.Kind).run(F);
      noEffects(E);
      dependency("compiler_entry", "typeMetadata", Accessor);
      Result.ProjectionKind = "empty_value_initializer";
      Result.Evidence.push_back(
          "The exact zero-size value initializer returns normally with no "
          "observable memory or call effects and restores its native frame; "
          "its zero-size result has no scalar return storage.");
      break;
    }
    case Kind::AllocatingInitializer: {
      if (Type.Kind != "class" || !R.Initializer ||
          R.Initializer->DeclarationKind != "initializer" ||
          R.Initializer->IsStatic || R.Initializer->Module != Type.Module ||
          R.Initializer->ContextKind != Type.Kind ||
          R.Initializer->ContextName != Type.Name)
        throw Unproven("allocator requires the actual initializing-constructor "
                       "source dependency");
      const auto &S = *R.Initializer;
      const auto Name = normalized(R.Signature.MangledSymbol);
      if (!Name.starts_with(Prefix) || !Name.ends_with("cfC"))
        throw Unproven("allocating-constructor identity is unsupported");
      const Identity Init{S.Entry, S.MangledSymbol};
      exactRole(Init, Name.substr(0, Name.size() - 1) + "c");
      if (S.Parameters.size() != R.Signature.Parameters.size() ||
          S.Labels != R.Signature.Labels)
        throw Unproven(
            "allocating and initializing constructor arguments disagree");
      for (size_t N = 0; N < S.Parameters.size(); ++N)
        if (S.Parameters[N].Type.Name != R.Signature.Parameters[N].Type.Name ||
            S.Parameters[N].Type.TheKind !=
                R.Signature.Parameters[N].Type.TheKind ||
            S.Parameters[N].Type.Bits != R.Signature.Parameters[N].Type.Bits ||
            S.Parameters[N].Type.IsSigned !=
                R.Signature.Parameters[N].Type.IsSigned)
          throw Unproven(
              "allocating and initializing constructor scalar types disagree");
      auto Alloc = NativeFlow(Image, Type, R.Kind, &S).run(F);
      auto InitEffects =
          NativeFlow(Image, Type, std::nullopt, &S).run(body(Init));
      if (Alloc.Calls != 1 || InitEffects.Calls ||
          Alloc.First != Value{Value::Allocation, 0, 0, 8} ||
          InitEffects.First != Value{Value::Self, 0, 0, 8} ||
          Alloc.Writes != InitEffects.Writes ||
          Alloc.Writes.size() != Type.Fields.size())
        throw Unproven("allocator and initializing constructor native effect "
                       "graphs disagree");
      std::set<int64_t> Written;
      for (const auto &W : Alloc.Writes)
        if (!Written.insert(W.Offset).second)
          throw Unproven("canonical initializer does not initialize every "
                         "field exactly once");
      dependency("method", "init", Init);
      Result.ProjectionKind = "allocating_initializer";
      Result.Evidence.push_back(
          "The exact runtime allocation size/alignment and allocated return "
          "are proven; every ordered field write equals the actual "
          "initializing-constructor native effect graph.");
      break;
    }
    case Kind::TrivialDestructor:
      if (Type.Kind != "class")
        throw Unproven("destructor context is not a class");
      destructor(identity());
      Result.ProjectionKind = "trivial_destructor";
      Result.Evidence.push_back(
          "The complete destructor returns its original instance, restores the "
          "frame, and has no object memory or runtime call effects.");
      break;
    case Kind::DeallocatingDestructor: {
      if (Type.Kind != "class")
        throw Unproven("deallocator context is not a class");
      exactRole(identity(), Prefix + "fD");
      destructor(related());
      // Heap metadata's destroy entry establishes the class-to-deallocator
      // relationship independently of a submitted compiler-role spelling.
      objc::RuntimeData Data(Image);
      auto Destroy = Type.Metadata >= 16 ? Data.pointer(Type.Metadata - 16)
                                         : std::optional<uint64_t>{};
      if (!Destroy || *Destroy != F.Entry)
        throw Unproven(
            "native heap metadata does not own this class deallocator");
      auto E = NativeFlow(Image, Type, R.Kind).run(F);
      if (E.Calls != 1 || !E.Writes.empty())
        throw Unproven("deallocator has unmodeled effects");
      dependency("compiler_entry", "deinit", related());
      Result.ProjectionKind = "deallocating_destructor";
      Result.Evidence.push_back(
          "The native terminal tail transfer calls the exact class "
          "deallocation runtime with original self and native size/alignment; "
          "the related destructor is independently trivial.");
      break;
    }
    case Kind::TypeMetadataAccessor: {
      exactRole(identity(), Prefix + "Ma");
      objc::RuntimeData Data(Image);
      auto Offset = Data.u32(Type.Descriptor + 12);
      if (!Offset || !*Offset)
        throw Unproven("native descriptor has no metadata accessor reference");
      const int64_t Delta = static_cast<int32_t>(*Offset);
      const auto Slot = Type.Descriptor + 12;
      if ((Delta < 0 && Slot < uint64_t(-Delta)) ||
          (Delta > 0 && Slot > InvalidVA - Delta) ||
          (Delta < 0 ? Slot - uint64_t(-Delta) : Slot + Delta) != F.Entry)
        throw Unproven(
            "native type descriptor does not own this metadata accessor");
      auto E = NativeFlow(Image, Type, R.Kind).run(F);
      const Value Expected = Type.Kind == "class" ? Value{Value::MetadataResult,
                                                          Type.Metadata, 0, 8}
                                                  : constant(Type.Metadata);
      if (!E.Writes.empty() || E.First != Expected ||
          !isConstant(E.Second, 0) ||
          E.Calls != (Type.Kind == "class" ? 1u : 0u))
        throw Unproven("metadata accessor does not return its exact context "
                       "metadata and complete state");
      Result.ProjectionKind = "type_metadata_accessor";
      Result.Evidence.push_back(
          Type.Kind == "class"
              ? "The descriptor-owned accessor preserves objc_opt_self on its "
                "exact class metadata and returns that runtime result with "
                "Complete state zero."
              : "The descriptor-owned accessor returns its exact fixed context "
                "metadata and Complete state zero without additional effects.");
      break;
    }
    case Kind::ModifyAccessor:
      modifier(identity(), related());
      Result.ProjectionKind = "modify_accessor";
      break;
    case Kind::ModifyResume:
      modifier(related(), identity());
      Result.ProjectionKind = "modify_resume";
      break;
    }
    Result.Evidence.push_back(
        "This proves a compiler source projection only; every emitted context, "
        "method, property, and related-entry dependency must still be "
        "satisfied.");
    Result.Proven = true;
    return Result;
  }
};
} // namespace

SwiftRuntimeSourceProof
recoverSwiftRuntimeSource(const SwiftRuntimeSourceRequest &Request,
                          const BinaryImage &Image,
                          const std::vector<LowFunc> &NativeFunctions) {
  try {
    return Proof(Request, Image, NativeFunctions).run();
  } catch (const Unproven &Error) {
    SwiftRuntimeSourceProof Result;
    Result.Reason = Error.what();
    return Result;
  }
}
} // namespace neverd
