#include "neverd/loader/Swift/SwiftABI.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/loader/Swift/SwiftMetadata.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace neverd {
namespace {
struct Unproven : std::runtime_error {
  using std::runtime_error::runtime_error;
};
struct Value {
  enum Kind { Scalar, Constant, Stack, Self } TheKind = Scalar;
  int64_t Offset = 0;
  bool Unknown = true;
  uint64_t DirectCoefficient = 0;
  bool ExactDirect = true;
};
using Key = std::tuple<VnodeSpace, uint64_t, uint16_t>;
TypeRef nativeField(const SwiftSourceType &T) {
  switch (T.TheKind) {
  case SwiftSourceType::Kind::Integer:
    return NdType::makeInt(T.Bits / 8, T.IsSigned);
  case SwiftSourceType::Kind::Boolean:
    return NdType::makeInt(1, false);
  case SwiftSourceType::Kind::Floating:
    return NdType::makeFloat(T.Bits / 8);
  case SwiftSourceType::Kind::Pointer:
    return NdType::makePtr(NdType::makeVoid());
  default:
    throw Unproven("Swift stored property is not a fixed scalar");
  }
}
bool sameFields(const std::vector<SwiftStorageField> &A,
                const std::vector<SwiftStorageField> &B) {
  if (A.size() != B.size())
    return false;
  for (size_t I = 0; I < A.size(); ++I)
    if (A[I].Name != B[I].Name || A[I].Offset != B[I].Offset ||
        A[I].IsMutable != B[I].IsMutable || A[I].Type.Name != B[I].Type.Name ||
        A[I].Type.TheKind != B[I].Type.TheKind ||
        A[I].Type.Bits != B[I].Type.Bits ||
        A[I].Type.IsSigned != B[I].Type.IsSigned)
      return false;
  return true;
}

class Proof {
  const SwiftSourceSignature &S;
  const BinaryImage &Image;
  const LowFunc &F;
  const SourceFunctionTypeHint &Explicit;
  const std::vector<LowFunc> &NativeFunctions;
  std::set<va_t> Entered;
  unsigned InspectedOps = 0, TailCalls = 0;
  const TargetRegInfo &Regs;
  std::map<Key, Value> Values;
  std::map<std::pair<int64_t, unsigned>, Value> StackValues;
  unsigned SelfReads = 0, SelfWrites = 0;
  bool DirectReturned = false;
  bool Returned = false;
  SourceParameterTypeHint DirectParameter;

  Value get(const NdVar &V) const {
    if (V.isConst())
      return {Value::Constant, static_cast<int64_t>(V.Offset), false, false};
    if (!V.isReg() && !V.isTemp())
      throw Unproven("Swift self proof encountered an unsupported value space");
    const auto Exact = Values.find({V.Space, V.Offset, V.Size});
    if (Exact != Values.end())
      return Exact->second;
    for (const auto &[K, Entry] : Values) {
      const auto [Space, Offset, Bytes] = K;
      if (Space == V.Space && V.Offset >= Offset &&
          V.Offset - Offset <= Bytes && V.Size <= Bytes - (V.Offset - Offset)) {
        if (Entry.TheKind != Value::Scalar && Entry.TheKind != Value::Constant)
          throw Unproven(
              "Swift context address is consumed through a partial register");
        return {Value::Scalar, 0, Entry.Unknown, Entry.DirectCoefficient,
                !Entry.DirectCoefficient && Entry.ExactDirect};
      }
    }
    return {};
  }
  void put(const NdVar &V, Value Result) {
    if (!V.isReg() && !V.isTemp())
      throw Unproven("Swift self proof encountered an invalid destination");
    for (auto It = Values.begin(); It != Values.end();) {
      const auto [Space, Offset, Bytes] = It->first;
      if (Space == V.Space && Offset < V.Offset + V.Size &&
          V.Offset < Offset + Bytes)
        It = Values.erase(It);
      else
        ++It;
    }
    Values[{V.Space, V.Offset, V.Size}] = Result;
  }
  const SwiftStorageField &field(Value Address, unsigned Bytes) const {
    for (const auto &Field : S.ContextFields)
      if (Address.Offset >= 0 &&
          Field.Offset == static_cast<uint64_t>(Address.Offset) &&
          nativeField(Field.Type)->Size == Bytes)
        return Field;
    throw Unproven(
        "Swift context memory access does not match an exact stored field");
  }
  Value load(Value Address, unsigned Bytes) {
    if (Address.TheKind == Value::Self) {
      (void)field(Address, Bytes);
      ++SelfReads;
      return {Value::Scalar, 0, false, false};
    }
    if (Address.TheKind != Value::Stack)
      throw Unproven("Swift self proof encountered an unrelated memory access");
    const auto It = StackValues.find({Address.Offset, Bytes});
    if (It != StackValues.end())
      return It->second;
    for (const auto &P : Explicit.Parameters)
      if (P.Location.Kind == SourceABICarrierKind::Stack &&
          P.Location.EntryStackOffset == Address.Offset &&
          P.Location.ValueBytes == Bytes)
        return {Value::Scalar, 0, false, false};
    throw Unproven("Swift self proof reads an unbound entry stack location");
  }
  void store(Value Address, unsigned Bytes, Value Stored) {
    if (Address.TheKind == Value::Self) {
      if (!field(Address, Bytes).IsMutable || Stored.Unknown ||
          Stored.TheKind == Value::Self || Stored.TheKind == Value::Stack ||
          Stored.DirectCoefficient || !Stored.ExactDirect)
        throw Unproven(
            "Swift self write lacks a mutable field and fully bound value");
      ++SelfWrites;
      return;
    }
    if (Address.TheKind != Value::Stack || Address.Offset < -4096 ||
        Address.Offset >= 0 || static_cast<int64_t>(Bytes) > -Address.Offset)
      throw Unproven("Swift self proof encountered a nonlocal memory write");
    const auto SP = get(NdVar::reg(Regs.StackPointer, 8));
    if (SP.TheKind != Value::Stack || Address.Offset < SP.Offset)
      throw Unproven("Swift self proof writes outside its current local frame");
    for (auto It = StackValues.begin(); It != StackValues.end();) {
      if (It->first.first < Address.Offset + Bytes &&
          Address.Offset < It->first.first + It->first.second)
        It = StackValues.erase(It);
      else
        ++It;
    }
    StackValues[{Address.Offset, Bytes}] = Stored;
  }
  Value scalar(const LowOp &Op) const {
    Value Result{Value::Scalar, 0, false, false};
    for (unsigned I = 0; I < Op.NumInputs; ++I) {
      const auto Input = get(Op.Inputs[I]);
      if (Input.TheKind == Value::Self || Input.TheKind == Value::Stack)
        throw Unproven(
            "Swift self address escapes an exact field or frame computation");
      Result.Unknown |= Input.Unknown;
      if (Input.DirectCoefficient || !Input.ExactDirect) {
        Result.DirectCoefficient = 1;
        Result.ExactDirect = false;
      }
    }
    // Only exact modular affine flow establishes a direct component's use.
    // Mere register reads (including xor-to-zero idioms or discarded flag
    // computations) cannot prove that the native return consumes self.
    if (Op.Output.Size == 8 && Op.NumInputs == 2) {
      auto A = get(Op.Inputs[0]), B = get(Op.Inputs[1]);
      if (A.ExactDirect && B.ExactDirect) {
        if (Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) {
          Result.DirectCoefficient =
              Op.Opcode == NdOp::INT_ADD
                  ? A.DirectCoefficient + B.DirectCoefficient
                  : A.DirectCoefficient - B.DirectCoefficient;
          Result.ExactDirect = true;
        } else if (Op.Opcode == NdOp::INT_MULT &&
                   (A.TheKind == Value::Constant ||
                    B.TheKind == Value::Constant)) {
          Result.DirectCoefficient =
              A.TheKind == Value::Constant
                  ? static_cast<uint64_t>(A.Offset) * B.DirectCoefficient
                  : static_cast<uint64_t>(B.Offset) * A.DirectCoefficient;
          Result.ExactDirect = true;
        } else if (Op.Opcode == NdOp::INT_LEFT &&
                   B.TheKind == Value::Constant && B.Offset >= 0 &&
                   B.Offset < 64) {
          Result.DirectCoefficient = A.DirectCoefficient << B.Offset;
          Result.ExactDirect = true;
        } else if (Op.Opcode == NdOp::INT_XOR && Op.Inputs[0] == Op.Inputs[1]) {
          Result.DirectCoefficient = 0;
          Result.ExactDirect = true;
          Result.Unknown = false;
        }
      }
    }
    return Result;
  }

  void inspect(const LowFunc &Function, unsigned Depth = 0) {
    if (Depth > 8 || !Entered.insert(Function.Entry).second)
      throw Unproven(
          "Swift self tail flow is recursive or exceeds its depth limit");
    if (!Image.isCodeAddress(Function.Entry) || Function.Blocks.size() != 1 ||
        Function.Blocks.front().StartAddr != Function.Entry ||
        !Function.Blocks.front().ExceptionalSuccs.empty() ||
        !Function.Blocks.front().ExceptionalPreds.empty() ||
        !Function.Blocks.front().Succs.empty() ||
        Function.Blocks.front().Ops.size() > 4096)
      throw Unproven("Swift self entry-flow proof currently requires one "
                     "bounded nonexceptional block");
    const auto &Block = Function.Blocks.front();
    InspectedOps += Block.Ops.size();
    if (InspectedOps > 16384)
      throw Unproven("Swift self native flow exceeds its operation limit");
    for (size_t Index = 0; Index < Block.Ops.size(); ++Index) {
      const auto &Op = Block.Ops[Index];
      if (Returned && Op.Opcode != NdOp::NOP)
        throw Unproven("Swift self proof has instructions after its return");
      if (Op.MemoryOrdering != NdMemoryOrdering::None ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        throw Unproven(
            "Swift self proof encountered nonordinary memory ordering");
      switch (Op.Opcode) {
      case NdOp::NOP:
        break;
      case NdOp::COPY:
        if (Op.NumInputs != 1)
          throw Unproven("malformed Swift copy");
        {
          auto V = get(Op.Inputs[0]);
          if (V.DirectCoefficient &&
              (Op.Output.Size != 8 || Op.Inputs[0].Size != 8))
            V.ExactDirect = false;
          put(Op.Output, V);
        }
        break;
      case NdOp::LOAD:
        if (Op.NumInputs != 1)
          throw Unproven("malformed Swift load");
        put(Op.Output, load(get(Op.Inputs[0]), Op.Output.Size));
        break;
      case NdOp::STORE:
        if (Op.NumInputs != 2)
          throw Unproven("malformed Swift store");
        store(get(Op.Inputs[0]), Op.Inputs[1].Size, get(Op.Inputs[1]));
        break;
      case NdOp::INT_ADD:
      case NdOp::INT_SUB: {
        if (Op.NumInputs != 2)
          throw Unproven("malformed Swift address arithmetic");
        auto A = get(Op.Inputs[0]), B = get(Op.Inputs[1]);
        if (Op.Opcode == NdOp::INT_ADD && A.TheKind == Value::Constant)
          std::swap(A, B);
        if ((A.TheKind == Value::Self || A.TheKind == Value::Stack) &&
            B.TheKind == Value::Constant && B.Offset >= -1048576 &&
            B.Offset <= 1048576 && A.Offset >= -1048576 &&
            A.Offset <= 1048576 && Op.Output.Size == 8) {
          A.Offset += Op.Opcode == NdOp::INT_SUB ? -B.Offset : B.Offset;
          put(Op.Output, A);
        } else
          put(Op.Output, scalar(Op));
        break;
      }
      case NdOp::RETURN: {
        if (Explicit.ReturnType &&
            Explicit.ReturnType->Kind != NdTypeKind::Void) {
          auto Value = get(NdVar::reg(Explicit.ReturnLocation.RegisterOffset,
                                      Explicit.ReturnLocation.ValueBytes));
          if (Value.Unknown || Value.TheKind == Value::Self ||
              Value.TheKind == Value::Stack)
            throw Unproven("Swift self proof has an unbound return value");
          DirectReturned = Value.ExactDirect && Value.DirectCoefficient != 0;
        }
        const auto SP = get(NdVar::reg(Regs.StackPointer, 8));
        if (SP.TheKind != Value::Stack || SP.Offset != 0)
          throw Unproven("Swift self proof has an unbalanced local frame");
        Returned = true;
        break;
      }
      case NdOp::CALL: {
        // A normal call has a distinct return/frame transition and unknown
        // effects. Only the CFG owner's exact CALL + RETURN tail-transfer
        // boundary permits continuing the same machine entry-value flow.
        if (Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
            Op.Inputs[0].Size != 8 || Index + 2 != Block.Ops.size() ||
            Block.Ops[Index + 1].Opcode != NdOp::RETURN ||
            Block.Ops[Index + 1].NumInputs != 1 ||
            Block.Ops[Index + 1].Inputs[0] != Op.Output ||
            Block.Ops[Index + 1].MemoryOrdering != NdMemoryOrdering::None ||
            Block.Ops[Index + 1].MemoryAddressSpace !=
                NdMemoryAddressSpace::Default ||
            Op.Output != NdVar::reg(Regs.IntReturnReg, 8))
          throw Unproven(
              "Swift self proof requires an exact native tail transfer");
        const auto Target = Op.Inputs[0].Offset;
        unsigned Boundaries = 0;
        for (const auto &Boundary : Block.InstructionBoundaries) {
          if (!Boundary.OpCount || Boundary.FirstOp > Index + 1 ||
              (Boundary.FirstOp <= Index &&
               Index - Boundary.FirstOp >= Boundary.OpCount))
            continue;
          if (++Boundaries != 1 || Boundary.FirstOp != Index ||
              Boundary.OpCount != 2 ||
              Boundary.Control != LowInstructionControl::TailCall ||
              Boundary.ControlFlags != (LowInstructionControlFlag::Call |
                                        LowInstructionControlFlag::Return) ||
              Boundary.Mode != InstructionMode::Default ||
              Boundary.TargetMode != LowInstructionTargetMode::Preserve ||
              !Boundary.Immediate || *Boundary.Immediate != Target ||
              !Boundary.Size || Boundary.Address != Op.Addr ||
              Block.Ops[Index + 1].Addr != Op.Addr)
            throw Unproven("Swift self tail transfer lacks exact "
                           "instruction-boundary evidence");
        }
        if (Boundaries != 1 || !Image.isCodeAddress(Target) ||
            Image.findImportAt(Target))
          throw Unproven(
              "Swift self tail target is not a proven native code entry");
        const LowFunc *Callee = nullptr;
        for (const auto &Candidate : NativeFunctions) {
          if (Candidate.Entry != Target)
            continue;
          if (Callee)
            throw Unproven(
                "Swift self tail target has ambiguous native bodies");
          Callee = &Candidate;
        }
        if (!Callee)
          throw Unproven("Swift self tail target has no inspected native body");
        const auto SP = get(NdVar::reg(Regs.StackPointer, 8));
        if (SP.TheKind != Value::Stack || SP.Offset != 0)
          throw Unproven(
              "Swift self tail transfer has an unbalanced local frame");
        // LowIR temporary identifiers and private stack slots are local to a
        // function; a callee must never inherit an unrelated initialized local.
        for (auto It = Values.begin(); It != Values.end();) {
          if (std::get<0>(It->first) == VnodeSpace::TEMP)
            It = Values.erase(It);
          else
            ++It;
        }
        StackValues.clear();
        ++TailCalls;
        inspect(*Callee, Depth + 1);
        return;
      }
      case NdOp::INDIR_CALL:
      case NdOp::BRANCH:
      case NdOp::COND_BR:
      case NdOp::INDIR_BR:
      case NdOp::INTRINSIC:
      case NdOp::ATOMIC_ADD:
      case NdOp::ATOMIC_XCHG:
      case NdOp::ATOMIC_CMPXCHG:
        throw Unproven(
            "Swift self proof has a call, branch, or unmodeled native effect");
      default:
        if (!Op.Output.Size || !Op.NumInputs || Op.NumInputs > 6)
          throw Unproven("Swift self proof has an unmodeled value operation");
        put(Op.Output, scalar(Op));
        break;
      }
    }
  }

public:
  Proof(const SwiftSourceSignature &S, const BinaryImage &Image,
        const LowFunc &F, const SourceFunctionTypeHint &Explicit,
        const std::vector<LowFunc> &NativeFunctions)
      : S(S), Image(Image), F(F), Explicit(Explicit),
        NativeFunctions(NativeFunctions), Regs(getTargetRegInfo(Image.Arch)) {}
  SwiftSelfABIProof run() {
    if (!Image.isMachO() ||
        (Image.Arch != Arch::X64 && Image.Arch != Arch::AArch64) ||
        !Image.isCodeAddress(S.Entry) || F.Entry != S.Entry)
      throw Unproven(
          "Swift self proof requires the matching native Mach-O entry");
    if (std::none_of(Image.Symbols.begin(), Image.Symbols.end(),
                     [&](const Symbol &Sym) {
                       return Sym.IsFunc && Sym.Addr == S.Entry &&
                              (Sym.Name == S.MangledSymbol ||
                               "_" + Sym.Name == S.MangledSymbol ||
                               Sym.Name == "_" + S.MangledSymbol);
                     }))
      throw Unproven(
          "Swift self declaration has no exact native symbol identity");
    const bool Getter = S.DeclarationKind == "getter";
    const bool Setter = S.DeclarationKind == "setter";
    if (S.ContextKind != "struct" || S.IsStatic ||
        (S.DeclarationKind != "function" && !Getter && !Setter) ||
        !S.ContextLayoutKnown || S.ContextFields.empty())
      throw Unproven("Swift self proof requires a fixed-layout struct instance "
                     "function or accessor");
    if ((Getter && (!S.Parameters.empty() ||
                    S.ReturnType.TheKind == SwiftSourceType::Kind::Void ||
                    !Explicit.ReturnType ||
                    Explicit.ReturnType->Kind == NdTypeKind::Void)) ||
        (Setter && (S.Parameters.size() != 1 ||
                    S.ReturnType.TheKind != SwiftSourceType::Kind::Void ||
                    !Explicit.ReturnType ||
                    Explicit.ReturnType->Kind != NdTypeKind::Void)))
      throw Unproven(
          "Swift accessor argument and return shape is inconsistent");
    bool Matched = false;
    for (const auto &Type : recoverSwiftTypes(Image)) {
      if (Type.Module != S.Module || Type.Name != S.ContextName ||
          Type.Kind != "struct")
        continue;
      if (Matched || Type.Status != "recovered" ||
          !sameFields(Type.Fields, S.ContextFields))
        throw Unproven(
            "Swift self fields disagree with native storage metadata");
      Matched = true;
    }
    if (!Matched)
      throw Unproven("Swift self has no native fixed-layout type metadata");
    std::string Diagnostic;
    if (Explicit.Architecture != Image.Arch ||
        Explicit.Parameters.size() != S.Parameters.size() ||
        !validateSourceABI(Explicit, Diagnostic))
      throw Unproven("Swift explicit argument ABI is not validated: " +
                     Diagnostic);
    put(NdVar::reg(Regs.StackPointer, 8), {Value::Stack, 0, false, false});
    put(NdVar::reg(Image.Arch == Arch::AArch64 ? a64reg::X20 : reg::R13, 8),
        {Value::Self, 0, false, false});
    size_t IntegerCount = 0;
    for (size_t I = 0; I < Explicit.Parameters.size(); ++I) {
      const auto &P = Explicit.Parameters[I];
      if (P.Name != S.Parameters[I].Name || !P.Type)
        throw Unproven("Swift explicit parameter identity is inconsistent");
      if (P.Type->Kind != NdTypeKind::Float)
        ++IntegerCount;
      if (P.Location.Kind == SourceABICarrierKind::IntegerRegister ||
          P.Location.Kind == SourceABICarrierKind::FloatingRegister)
        put(NdVar::reg(P.Location.RegisterOffset, P.Location.ValueBytes),
            {Value::Scalar, 0, false, false});
    }
    // One fixed, word-sized integer field is a single ordinary integer ABI
    // component. Larger aggregates and packed scalar mixtures need separate
    // ABI expansion evidence and are deliberately not inferred here.
    const auto &First = S.ContextFields.front();
    if (S.ContextFields.size() == 1 && First.Offset == 0 &&
        First.Type.TheKind == SwiftSourceType::Kind::Integer &&
        First.Type.Bits == 64 && IntegerCount < Regs.IntParamRegs.size()) {
      DirectParameter = {"swift_self_0",
                         nativeField(First.Type),
                         {SourceABICarrierKind::IntegerRegister,
                          Regs.IntParamRegs[IntegerCount], 0, 8}};
      put(NdVar::reg(DirectParameter.Location.RegisterOffset, 8),
          {Value::Scalar, 0, false, true});
    }
    inspect(F);
    if (!Returned)
      throw Unproven("Swift self proof has no native return");
    SwiftSelfABIProof Result;
    if (SelfWrites && !DirectReturned) {
      Result.Proven = true;
      Result.IsMutating = true;
      Result.SelfConvention = "indirect-mutating";
      Result.SelfParameters = {
          {"swift_self",
           NdType::makePtr(NdType::makeVoid()),
           {SourceABICarrierKind::IntegerRegister,
            Image.Arch == Arch::AArch64 ? a64reg::X20 : reg::R13, 0, 8}}};
      Result.Evidence = {
          "The fixed Swift context register flows to exact mutable "
          "stored-field writes with bound scalar values.",
          "All inspected context address uses remain inside the "
          "metadata-confirmed field layout."};
    } else if (DirectReturned && !SelfReads && !SelfWrites &&
               DirectParameter.Type) {
      Result.Proven = true;
      Result.IsMutating = false;
      Result.SelfConvention = "direct-fields";
      Result.SelfParameters = {DirectParameter};
      Result.Evidence = {"Native metadata establishes a single 64-bit integer "
                         "field at offset zero.",
                         "Its fixed Swift ABI component, after the explicit "
                         "integer arguments, contributes to the actual return "
                         "value without context-register memory effects."};
    } else
      throw Unproven("Swift native entry flow does not distinguish a supported "
                     "self convention");
    if (Setter &&
        (!Result.IsMutating || Result.SelfConvention != "indirect-mutating"))
      throw Unproven(
          "Swift setter self ABI requires actual mutable stored-field writes");
    if (TailCalls)
      Result.Evidence.push_back(
          "Direct native tail transfers preserve the entry frame; inspected "
          "target bodies carry the proven field component to the actual "
          "return.");
    return Result;
  }
};
} // namespace

SwiftSelfABIProof
recoverSwiftSelfABI(const SwiftSourceSignature &S, const BinaryImage &Image,
                    const LowFunc &F, const SourceFunctionTypeHint &Explicit,
                    const std::vector<LowFunc> &NativeFunctions) {
  try {
    return Proof(S, Image, F, Explicit, NativeFunctions).run();
  } catch (const Unproven &Error) {
    SwiftSelfABIProof Result;
    Result.Reason = Error.what();
    return Result;
  }
}
} // namespace neverd
