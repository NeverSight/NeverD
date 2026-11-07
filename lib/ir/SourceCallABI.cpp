//===- SourceCallABI.cpp - Checked source call carriers -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Places source call operands and verifies their original machine carriers.

#include "neverd/ir/SourceCallABI.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/SourceParameterPlacement.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/LowScalarConstant.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <functional>
#include <map>
#include <set>
#include <tuple>
using namespace neverd;
namespace {
#define SOURCE_ABI_CARRIER_TEXT(Name, Text) constexpr char Name[] = Text;
#include "SourceABICarriers.def"

bool scalar(const TypeRef &T) {
  return T && (T->Size == 1 || T->Size == 2 || T->Size == 4 || T->Size == 8) &&
         (T->Kind == NdTypeKind::Int || T->Kind == NdTypeKind::Float ||
          (T->Kind == NdTypeKind::Ptr && T->Size == 8));
}
} // namespace
std::optional<int64_t>
neverd::sourceCallReturnAddressBias(const BinaryImage *Image, Arch Architecture,
                                    va_t Address) {
  if (Architecture == Arch::AArch64)
    return 0;
  if ((Architecture != Arch::X64 && Architecture != Arch::X86) || !Image)
    return {};
  const auto *P = Image->readVA(Address, 2);
  if (!P)
    return {};
  const auto Width = getTargetRegInfo(Architecture).PointerSize;
  if (P[0] == 0xe8)
    return Width;
  if (P[0] == 0xe9 || P[0] == 0xeb)
    return 0;
  if (Architecture == Arch::X64 && (P[0] & 0xf0) == 0x40) {
    P = Image->readVA(Address + 1, 2);
    if (!P)
      return {};
  }
  if (P[0] == 0xff && ((P[1] >> 3) & 7) == 2)
    return Width;
  if (P[0] == 0xff && ((P[1] >> 3) & 7) == 4)
    return 0;
  return {};
}
namespace {
bool refuse(std::string &Error, const char *Reason) {
  Error = Reason;
  return false;
}
} // namespace
bool neverd::assignAnalystCallABI(SourceCallTypeHint &Hint, Arch Architecture,
                                  BinaryFormat Format, std::string &Error) {
  auto &Signature = Hint.Signature;
  if (Hint.AnalystFixedCount < -1 || Hint.AnalystFixedCount == 0 ||
      (Hint.AnalystFixedCount > 0 &&
       static_cast<size_t>(Hint.AnalystFixedCount) >
           Signature.Parameters.size()))
    return refuse(Error, "A variadic call-site needs a nonempty fixed prefix "
                         "within its actual operands");
  if (Architecture == Arch::AArch64 && Format == BinaryFormat::MachO &&
      Hint.AnalystFixedCount < 0) {
    Signature.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
    if (!assignDarwinFixedSourceABI(Signature, Architecture, Error))
      return false;
    if (hasIndirectSourceParameters(Signature))
      return refuse(Error, kIndirectAnalystCopy);
    if (Signature.ReturnLocation.Kind ==
        SourceABICarrierKind::IndirectResultPointer)
      return refuse(Error, kIndirectAnalystResult);
    return true;
  }
  if ((Architecture != Arch::X64 && Architecture != Arch::AArch64) ||
      (Format != BinaryFormat::COFF && Format != BinaryFormat::ELF &&
       Format != BinaryFormat::MachO) ||
      !Signature.ReturnType ||
      (Signature.ReturnType->Kind != NdTypeKind::Void &&
       !scalar(Signature.ReturnType)) ||
      Signature.Parameters.size() >
          ::neverd::limits::ir::
              kSourceCallABINeverdAssignAnalystCallABISignatureParametersSizeLimit)
    return refuse(
        Error, "Call-site types support scalar/pointer x64 and AArch64 C ABIs; "
               "aggregate results need separate ABI support");
  std::vector<TypeRef> Types;
  for (size_t I = 0; I < Signature.Parameters.size(); ++I) {
    const auto &T = Signature.Parameters[I].Type;
    if (!scalar(T))
      return refuse(Error, "Every call-site argument must be a complete scalar "
                           "or pointer of at most eight bytes");
    if (Hint.AnalystFixedCount >= 0 &&
        I >= static_cast<unsigned>(Hint.AnalystFixedCount)) {
      if ((T->Kind == NdTypeKind::Int && T->Size < 4) ||
          (T->Kind == NdTypeKind::Float && T->Size != 8))
        return refuse(Error, "Variadic arguments need default promotions: "
                             "int/unsigned int or double");
    }
    if (Hint.AnalystFixedCount >= 0 && Format == BinaryFormat::COFF &&
        Architecture == Arch::X64 && I < 4 && T->Kind == NdTypeKind::Float)
      return refuse(Error,
                    "A Windows x64 variadic floating register argument needs a "
                    "verified duplicate integer-register carrier");
    Types.push_back(T);
  }
  Signature.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
  Signature.Architecture = Architecture;
  if (Format == BinaryFormat::MachO) {
    if (Hint.AnalystFixedCount >= 0)
      return assignDarwinVariadicSourceABI(Signature, Hint.AnalystFixedCount,
                                           Architecture, Error);
    // Fixed and variadic source calls share Darwin's scalar extension and
    // packed-stack contract with recovered native declarations.
    return assignDarwinScalarSourceABI(Signature, Architecture, Error);
  }
  if (Hint.AnalystFixedCount >= 0 && Architecture == Arch::X64 &&
      Format == BinaryFormat::ELF)
    return assignSysVX64VariadicSourceABI(Signature, Hint.AnalystFixedCount,
                                          Error);
  const auto Placement = placeSourceParameters(
      Architecture, Format, Signature.ReturnType, Types,
      Hint.AnalystFixedCount >= 0 && Architecture == Arch::AArch64 &&
          Format == BinaryFormat::COFF);
  if (!Placement || Placement->ResultPointer ||
      Placement->Parameters.size() != Types.size())
    return refuse(Error, "The selected platform cannot place every argument of "
                         "this call-site type");
  Signature.HasExplicitABI = true;
  Signature.ReturnLocation = {};
  const auto &TRI = getTargetRegInfo(Architecture);
  if (Signature.ReturnType->Kind != NdTypeKind::Void) {
    const bool FP = Signature.ReturnType->Kind == NdTypeKind::Float;
    Signature.ReturnLocation = {FP ? SourceABICarrierKind::FloatingRegister
                                   : SourceABICarrierKind::IntegerRegister,
                                FP ? TRI.FPReturnReg : TRI.IntReturnReg, 0,
                                Signature.ReturnType->Size};
  }
  for (unsigned I = 0; I < Types.size(); ++I) {
    if (Placement->Parameters[I].size() != 1 ||
        Placement->Parameters[I][0].Indirect)
      return refuse(
          Error,
          "A call-site argument requires unsupported aggregate carriers");
    Signature.Parameters[I].Location = Placement->Parameters[I][0].Location;
  }
  return validateSourceABI(Signature, Error);
}

bool neverd::sourceCallVariadicStateDefined(const LowFunc &Function,
                                            const SourceCallTypeHint &Hint,
                                            BinaryFormat Format,
                                            std::string &Error) {
  if (Hint.AnalystFixedCount < 0 || Hint.Signature.Architecture != Arch::X64 ||
      (Format != BinaryFormat::ELF && Format != BinaryFormat::MachO))
    return true;
  unsigned Floating = 0;
  for (const auto &P : Hint.Signature.Parameters)
    Floating += P.Location.Kind == SourceABICarrierKind::FloatingRegister;
  if (!Floating)
    return true;
  if (!Hint.AnalystSite)
    return refuse(Error, "The variadic call has no original occurrence");
  const auto Count = constantRegisterValueAtCall(Function, *Hint.AnalystSite,
                                                 NdVar::reg(x86reg::RAX, 1));
  if (Count.BudgetExceeded)
    return refuse(Error, "The variadic vector-count proof exceeded its budget");
  if (!Count.OccurrenceFound)
    return refuse(Error,
                  "The original variadic call occurrence is unavailable");
  return Count.Value && *Count.Value >= Floating && *Count.Value <= 8
             ? true
             : refuse(Error, "System V x64 variadic floating operands need a "
                             "defined AL vector-register count in this call's "
                             "basic block");
}

bool neverd::sourceCallStackArgumentsDefined(const LowFunc &Function,
                                             const SourceCallTypeHint &Hint,
                                             Arch Architecture, int64_t Bias,
                                             std::string &Error) {
  if (Hint.AnalystSite)
    return sourceCallStackArgumentsDefined(Function, Hint, *Hint.AnalystSite,
                                           Architecture, Bias, Error);
  if (!validateSourceABI(Hint.Signature, Error))
    return false;
  for (const auto &Parameter : sourceABIParameters(Hint.Signature))
    if (Parameter.Location.Kind == SourceABICarrierKind::Stack)
      return refuse(Error,
                    "The stack arguments have no original call occurrence");
  return true;
}

bool neverd::sourceCallStackArgumentsDefined(
    const LowFunc &Function, const SourceCallTypeHint &Hint,
    const SourceCallOccurrenceKey &OriginalSite, Arch Architecture,
    int64_t Bias, std::string &Error) {
  if (!validateSourceABI(Hint.Signature, Error))
    return false;
  const auto Parameters = sourceABIParameters(Hint.Signature);
  bool HasStack = false;
  for (const auto &P : Parameters)
    HasStack |= P.Location.Kind == SourceABICarrierKind::Stack;
  if (!HasStack)
    return true;
  using Key = std::tuple<VnodeSpace, uint64_t, uint16_t>;
  auto key = [](const NdVar &V) { return Key{V.Space, V.Offset, V.Size}; };
  const auto SP = NdVar::reg(getTargetRegInfo(Architecture).StackPointer, 8);
  size_t Work = 0;
  for (const auto &Block : Function.Blocks) {
    std::map<Key, int64_t> Addresses{{key(SP), 0}};
    std::set<int64_t> Written;
    va_t Instruction = InvalidVA;
    auto address = [&](const NdVar &V) -> std::optional<int64_t> {
      const auto It = Addresses.find(key(V));
      return It == Addresses.end() ? std::nullopt
                                   : std::optional<int64_t>(It->second);
    };
    for (const auto &Op : Block.Ops) {
      if (++Work >
          ::neverd::limits::ir::
              kSourceCallABINeverdSourceCallStackArgumentsDefinedWorkLimit)
        return refuse(Error,
                      "The outgoing stack argument proof exceeded its budget");
      if (Op.Addr != Instruction) {
        std::erase_if(Addresses, [](const auto &Entry) {
          return std::get<0>(Entry.first) == VnodeSpace::TEMP;
        });
        Instruction = Op.Addr;
      }
      if (const auto Site = sourceCallOccurrenceKey(Op);
          Site && *Site == OriginalSite) {
        const auto CurrentSP = address(SP);
        if (!CurrentSP)
          return refuse(Error, "The call's stack pointer is not a complete "
                               "constant-offset value");
        for (const auto &P : Parameters)
          if (P.Location.Kind == SourceABICarrierKind::Stack) {
            const int64_t Offset =
                *CurrentSP + P.Location.EntryStackOffset - Bias;
            for (unsigned I = 0; I < P.Location.ValueBytes; ++I)
              if (!Written.count(Offset + I))
                return refuse(Error, "An outgoing stack argument is not fully "
                                     "written in this call's basic block");
          }
        return true;
      }
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
          Op.Opcode == NdOp::INTRINSIC) {
        Written.clear();
        const auto CurrentSP = address(SP);
        Addresses.clear();
        if (CurrentSP)
          Addresses[key(SP)] = *CurrentSP;
      }
      if (Op.Opcode == NdOp::STORE) {
        const auto Offset =
            Op.NumInputs == 2 ? address(Op.Inputs[0]) : std::nullopt;
        if (!Offset || !Op.Inputs[1].Size ||
            Op.Inputs[1].Size > limits::ir::kSourceCallStackWriteBytes ||
            Op.MemoryOrdering != NdMemoryOrdering::None ||
            Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
          Written.clear();
        else
          for (unsigned I = 0; I < Op.Inputs[1].Size; ++I)
            Written.insert(*Offset + I);
      }
      std::optional<int64_t> Next;
      if (Op.Output.Size == 8 && Op.NumInputs == 1 && Op.Opcode == NdOp::COPY)
        Next = address(Op.Inputs[0]);
      if (Op.Output.Size == 8 && Op.NumInputs == 2 &&
          (Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
          Op.Inputs[1].isConst())
        if (const auto Base = address(Op.Inputs[0])) {
          int64_t Delta = static_cast<int64_t>(Op.Inputs[1].Offset);
          if (Op.Inputs[1].Size == 4)
            Delta = static_cast<int32_t>(Delta);
          if (Delta > -limits::ir::kSourceCallStackAddressMagnitude &&
              Delta < limits::ir::kSourceCallStackAddressMagnitude) {
            const int64_t Value =
                *Base + (Op.Opcode == NdOp::INT_ADD ? Delta : -Delta);
            if (Value > -limits::ir::kSourceCallStackAddressMagnitude &&
                Value < limits::ir::kSourceCallStackAddressMagnitude)
              Next = Value;
          }
        }
      if (Op.Output.Size) {
        for (auto It = Addresses.begin(); It != Addresses.end();) {
          const auto &[Space, Offset, Size] = It->first;
          if (Space == Op.Output.Space &&
              Offset < Op.Output.Offset + Op.Output.Size &&
              Op.Output.Offset < Offset + Size)
            It = Addresses.erase(It);
          else
            ++It;
        }
        if (Next)
          Addresses[key(Op.Output)] = *Next;
      }
    }
  }
  return refuse(Error, "The original call occurrence is unavailable for stack "
                       "argument validation");
}

bool neverd::sourceCallArgumentsDefined(const MedFunc &Function,
                                        const SourceCallOccurrenceKey &Site,
                                        std::string &Error) {
  using Key = std::tuple<unsigned, int, int, uint16_t>;
  auto key = [](const MedVar &V) {
    return Key{V.Kind, V.Id, V.SSAVer, V.Size};
  };
  std::map<Key, std::vector<MedVar>> Definitions;
  std::map<Key, const MedOp *> Operations;
  std::map<int, MedVar> EntryRegisters;
  std::set<Key> Unknown, Parameters;
  for (const auto &P : Function.Params) {
    Parameters.insert(key(P));
    // ABI recovery promotes an entry register marker to Param while retaining
    // its variable id. Some physical dependencies still name that exact SSA-0
    // marker; no later version or unrelated register is an entry value.
    if (P.Kind == MedVar::Param && P.Id >= 0 && P.RegOff != kNoParamReg) {
      Parameters.insert(Key{MedVar::Reg, P.Id, 0, P.Size});
      EntryRegisters.emplace(P.Id, P);
    }
  }
  for (const auto &C : Function.CallClobbers)
    if (C.Value.Size > C.PreservedPrefixSize)
      Unknown.insert(key(C.Value));
  const MedOp *Call = nullptr;
  size_t GraphWork = Function.Params.size() + Function.CallClobbers.size();
  for (const auto &Block : Function.Blocks) {
    for (const auto &Phi : Block.Phis) {
      GraphWork += 1 + Phi.Args.size();
      if (GraphWork >
          ::neverd::limits::ir::
              kSourceCallABINeverdSourceCallArgumentsDefinedGraphWorkLimit)
        return refuse(Error,
                      "The call-site carrier proof exceeded its graph budget");
      auto &Inputs = Definitions[key(Phi.Output)];
      for (const auto &[Predecessor, V] : Phi.Args)
        Inputs.push_back(V);
      if (Phi.ExceptionalEntry)
        Inputs.push_back(*Phi.ExceptionalEntry);
    }
    for (const auto &Op : Block.Ops) {
      GraphWork += 1 + Op.NumInputs;
      if (GraphWork >
          ::neverd::limits::ir::
              kSourceCallABINeverdSourceCallArgumentsDefinedGraphWorkLimit)
        return refuse(Error,
                      "The call-site carrier proof exceeded its graph budget");
      if (Op.SourceCallHint && Op.SourceCallHint->AnalystSite == Site) {
        if (Call)
          return refuse(Error, "The original call occurrence has multiple "
                               "surviving source operations");
        Call = &Op;
      }
      if (Op.Output.Size) {
        Operations.emplace(key(Op.Output), &Op);
        auto &Inputs = Definitions[key(Op.Output)];
        // Actual calls and memory reads produce runtime values. Their targets,
        // addresses and operands are separately retained by SSA and the stack
        // proof.
        if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL &&
            Op.Opcode != NdOp::LOAD)
          for (unsigned I = 0; I < Op.NumInputs; ++I)
            Inputs.push_back(Op.Inputs[I]);
      }
    }
  }
  if (!Call)
    return refuse(Error, "The call-site declaration could not bind to a "
                         "surviving original call");
  using Read = std::tuple<Key, unsigned, unsigned>;
  std::map<Read, unsigned> State;
  size_t Work = 0;
  std::string Missing;
  std::function<bool(const MedVar &, unsigned, unsigned)> Defined =
      [&](const MedVar &V, unsigned Offset, unsigned Bytes) {
        if (++Work >
                ::neverd::limits::ir::
                    kSourceCallABINeverdSourceCallArgumentsDefinedDefinedWorkLimit ||
            V.Kind == MedVar::Unspecified || Offset > V.Size ||
            Bytes > V.Size - Offset) {
          Missing = V.display();
          return false;
        }
        if (V.isConst() || Parameters.count(key(V)))
          return true;
        if (V.Kind == MedVar::Reg && V.SSAVer == 0)
          if (const auto Entry = EntryRegisters.find(V.Id);
              Entry != EntryRegisters.end() &&
              Entry->second.RegOff == V.RegOff &&
              Offset <= Entry->second.Size &&
              Bytes <= Entry->second.Size - Offset)
            return true;
        if (Unknown.count(key(V))) {
          Missing = V.display();
          return false;
        }
        const Read Query{key(V), Offset, Bytes};
        if (State[Query] == 2)
          return true;
        if (State[Query] == 1) {
          Missing = V.display();
          return false;
        }
        State[Query] = 1;
        const auto It = Definitions.find(key(V));
        if (It == Definitions.end()) {
          Missing = V.display();
          return false;
        }
        // Only bit-preserving projections narrow a dependency. A padded
        // record's int32 member may be copied through Xn before its low word is
        // extracted; those upper padding bits never become a source argument.
        // Arithmetic and all other operations still require their complete
        // physical inputs.
        if (const auto Found = Operations.find(key(V));
            Found != Operations.end()) {
          const auto &Op = *Found->second;
          if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
              Op.Inputs[0].Size == Op.Output.Size) {
            if (!Defined(Op.Inputs[0], Offset, Bytes))
              return false;
          } else if (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs == 2 &&
                     Op.Inputs[1].isConst() &&
                     Op.Inputs[1].ConstVal <= Op.Inputs[0].Size &&
                     Offset <= Op.Inputs[0].Size - Op.Inputs[1].ConstVal) {
            if (!Defined(Op.Inputs[0], Op.Inputs[1].ConstVal + Offset, Bytes))
              return false;
          } else {
            for (const auto &Input : It->second)
              if (!Defined(Input, 0, Input.Size))
                return false;
          }
        } else {
          for (const auto &Input : It->second)
            if (!Defined(Input, Offset, Bytes))
              return false;
        }
        State[Query] = 2;
        return true;
      };
  for (unsigned I = Call->Opcode == NdOp::INDIR_CALL ? 0 : 1;
       I < Call->NumInputs; ++I)
    if (!Defined(Call->Inputs[I], 0, Call->Inputs[I].Size)) {
      Error = (I == 0 ? "The indirect target" : "An argument") +
              std::string(
                  " depends on an undefined or clobbered physical carrier: ") +
              Missing;
      return false;
    }
  return true;
}
