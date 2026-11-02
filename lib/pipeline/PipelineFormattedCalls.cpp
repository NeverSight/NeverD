//===- PipelineFormattedCalls.cpp - Calls a constant format describes -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A printf-family call reads its fixed parameters and the further arguments
/// its format's conversions name.  Where the format is a constant string the
/// image holds, those are known at the call: the call's summary reads exactly
/// them, and the call passes them in the source's order.
///
//===----------------------------------------------------------------------===//

#include "PipelineLowIRDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/SourceParameterPlacement.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CallRegisterBody.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/low/ImportCallee.h"
#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/ObjC/ObjCFormattedCalls.h"
#include "neverd/pipeline/Limits.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>

namespace neverd::pipeline_detail {
namespace {

/// Integer-register constants used to form a format address. The call-effect
/// family numbering is x64-only; this local state also follows A64 X registers.
std::optional<unsigned> formatRegister(Arch A, uint64_t Register) {
  if (A == Arch::AArch64 && Register < a64reg::SP && Register % 8 == 0)
    return static_cast<unsigned>(Register / 8);
  return gprFamilyOf(A, Register);
}

/// The C string at \p Address, when the program cannot change it: in memory
/// it reads but neither writes nor executes.
std::optional<std::vector<uint16_t>> constantFormat(const BinaryImage &Img,
                                                    va_t Address) {
  const Segment *Seg = Img.getSegmentFor(Address);
  const Section *Sec = Img.getSectionFor(Address);
  if (!Address || !Seg || !Seg->isReadable() || Seg->isWritable() ||
      (Sec ? Sec->isWritable() || Sec->isExecutable() : Seg->isExecutable()))
    return std::nullopt;
  std::vector<uint16_t> Units;
  for (uint32_t I = 0;
       I < ::neverd::limits::pipeline::kPipelineFormattedCallsMaxUnits &&
       Address <= InvalidVA - I;
       ++I) {
    const uint8_t *Byte = Img.readVA(Address + I, 1);
    if (!Byte)
      return std::nullopt;
    if (!*Byte)
      return Units;
    Units.push_back(*Byte);
  }
  return std::nullopt;
}

/// The source type of a fixed parameter or return of C type \p Type: what
/// the carrier assignment needs of it.
TypeRef fixedType(std::string_view Type) {
  if (Type == "void")
    return NdType::makeVoid();
  if (Type == "int")
    return NdType::makeInt(sizeof(int32_t), true);
  if (Type == "size_t")
    return NdType::makeInt(sizeof(uint64_t), false);
  if (libc::isPointerType(Type))
    return NdType::makePtr(NdType::makeVoid());
  return nullptr;
}

/// A printf-family routine a call reaches, and the integer register family
/// its format arrives in: the last fixed parameter, an integer register
/// argument since every fixed parameter here is one.
struct PrintfCallee {
  const libc::LibCPrintfFormat *Routine = nullptr;
  unsigned FormatFamily = 0;
};

std::optional<PrintfCallee> printfCallee(const BinaryImage &Img, va_t Target) {
  const std::string Name = importCalleeName(Img, Target);
  const libc::LibCPrintfFormat *Routine =
      Name.empty() ? nullptr
                   : libc::libcPrintfFormat(
                         cNameOfSymbol(Name, Img.Format, Img.Arch).str());
  if (!Routine || !Routine->ParamCount)
    return std::nullopt;
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  const unsigned FormatParameter = Routine->ParamCount - 1;
  if (FormatParameter >= TRI.IntParamRegs.size())
    return std::nullopt;
  const std::optional<unsigned> Family =
      formatRegister(Img.Arch, TRI.IntParamRegs[FormatParameter]);
  if (!Family)
    return std::nullopt;
  return PrintfCallee{Routine, *Family};
}

/// The arguments of a call to \p Routine with the format at \p Format, given
/// the count of vector arguments AL holds, if known.
std::optional<FormattedCall>
formattedCall(const BinaryImage &Img, const libc::LibCPrintfFormat &Routine,
              uint64_t Format, std::optional<uint8_t> VectorCount) {
  SourceFunctionTypeHint Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Signature.Architecture = Img.Arch;
  Signature.ReturnType = fixedType(Routine.Return);
  if (!Signature.ReturnType)
    return std::nullopt;
  for (unsigned I = 0; I < Routine.ParamCount; ++I) {
    TypeRef Type = fixedType(Routine.Params[I]);
    if (!Type || Type->Kind == NdTypeKind::Void)
      return std::nullopt;
    Signature.Parameters.push_back({"", std::move(Type)});
  }
  // Each conversion names one promoted argument; a format the strict parser
  // cannot read whole (%n, long double, a wide string) binds nothing.
  const std::optional<std::vector<uint16_t>> Units =
      constantFormat(Img, Format);
  const std::optional<std::vector<TypeRef>> Named =
      Units ? objcFormatArgumentTypes(*Units,
                                      SourceCallTypeHint::FormatSyntax::Printf)
            : std::nullopt;
  if (!Named || Signature.Parameters.size() + Named->size() >
                    static_cast<size_t>(limits::kMaxBoundSourceCallArgs))
    return std::nullopt;
  for (const TypeRef &Type : *Named)
    Signature.Parameters.push_back({"", Type});
  if (Img.Arch == Arch::X64) {
    std::string Diagnostic;
    if (!assignSysVX64VariadicSourceABI(Signature, Routine.ParamCount,
                                        Diagnostic))
      return std::nullopt;
  } else {
    // AAPCS64's promoted scalar variadic values use the same independent
    // integer/vector banks as fixed scalars. Darwin and Windows variadics
    // have different rules and do not enable this convention policy.
    std::vector<TypeRef> Types;
    for (const auto &Parameter : Signature.Parameters)
      Types.push_back(Parameter.Type);
    const auto Placement = placeSourceParameters(Img.Arch, Img.abiFormat(),
                                                 Signature.ReturnType, Types);
    if (!Placement || Placement->Parameters.size() != Types.size())
      return std::nullopt;
    for (size_t I = 0; I < Types.size(); ++I) {
      const auto &Pieces = Placement->Parameters[I];
      if (Pieces.size() != 1 || Pieces.front().Indirect ||
          Pieces.front().Offset != 0)
        return std::nullopt;
      Signature.Parameters[I].Location = Pieces.front().Location;
    }
  }
  FormattedCall Result;
  unsigned Vectors = 0;
  for (const SourceParameterTypeHint &Parameter : Signature.Parameters) {
    const SourceABIValueLocation &Location = Parameter.Location;
    const bool Floating =
        Location.Kind == SourceABICarrierKind::FloatingRegister;
    // An argument the registers do not hold is on the stack, which this
    // binding does not read: such a call keeps the ordinary argument scan.
    if (!Floating && Location.Kind != SourceABICarrierKind::IntegerRegister)
      return std::nullopt;
    Result.Arguments.push_back(
        {Location.RegisterOffset, Location.ValueBytes, Floating,
         Parameter.Type && Parameter.Type->Kind == NdTypeKind::Ptr});
    Vectors += Floating;
  }
  // The caller counts the vector arguments in AL; a format that disagrees
  // with it is not the one the call passes.
  if (VectorCount && *VectorCount != Vectors)
    return std::nullopt;
  return Result;
}

/// Whether \p A and \p B pass their arguments in the same registers, each
/// of the same width and class.
bool sameCarriers(const FormattedCall &A, const FormattedCall &B) {
  return A.Arguments.size() == B.Arguments.size() &&
         std::equal(A.Arguments.begin(), A.Arguments.end(), B.Arguments.begin(),
                    [](const FormattedCallArgument &X,
                       const FormattedCallArgument &Y) {
                      return X.Register == Y.Register && X.Bytes == Y.Bytes &&
                             X.Floating == Y.Floating;
                    });
}

/// The constants each integer register family may hold: one where a block
/// sets it, a few where paths that set it differently join.  A family it
/// lacks may hold anything.
using FamilyConstants = std::map<unsigned, std::set<uint64_t>>;

/// Gets each call to a printf-family routine: the call, the routine, the
/// constants its format may be (null for unknown), and the count AL holds,
/// if known.
using PrintfCallVisitor = llvm::function_ref<void(
    const LowOp &, const PrintfCallee &, const std::set<uint64_t> *,
    std::optional<uint8_t>)>;

/// The constants \p Block leaves each family when it begins with
/// \p Constants, visiting its printf-family calls with \p OnCall, if given.
FamilyConstants scanBlock(const BinaryImage &Img, const LowBlock &Block,
                          FamilyConstants Constants,
                          PrintfCallVisitor OnCall = nullptr) {
  const std::optional<unsigned> VectorCountFamily =
      Img.Arch == Arch::X64 ? gprFamilyOf(Img.Arch, x86reg::RAX) : std::nullopt;
  // The constant each temporary holds here, and the count AL holds.
  std::map<uint64_t, uint64_t> Temporaries;
  std::optional<uint8_t> VectorCount;
  if (VectorCountFamily)
    if (const auto It = Constants.find(*VectorCountFamily);
        It != Constants.end())
      for (uint64_t Value : It->second) {
        const uint8_t Count = Value & 0xFF;
        if (VectorCount && *VectorCount != Count) {
          // No one argument layout agrees with different known AL counts.
          VectorCount = 0xFF;
          break;
        }
        VectorCount = Count;
      }
  auto Truncated = [](uint64_t Value, uint16_t Size) {
    return Size >= 8 ? Value : Value & ((uint64_t(1) << (8 * Size)) - 1);
  };
  auto ValueOf = [&](const NdVar &V) -> std::optional<uint64_t> {
    if (V.isConst())
      return Truncated(V.Offset, V.Size);
    if (V.isTemp())
      if (const auto It = Temporaries.find(V.Offset); It != Temporaries.end())
        return Truncated(It->second, V.Size);
    if (V.isReg() && V.Offset % 8 == 0)
      if (const auto Family = formatRegister(Img.Arch, V.Offset))
        if (const auto It = Constants.find(*Family);
            It != Constants.end() && It->second.size() == 1)
          return Truncated(*It->second.begin(), V.Size);
    return std::nullopt;
  };
  for (const LowOp &Op : Block.Ops) {
    if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
      if (OnCall && Op.NumInputs > 0 && Op.Inputs[0].isConst())
        if (const auto Callee = printfCallee(Img, Op.Inputs[0].Offset)) {
          const auto Format = Constants.find(Callee->FormatFamily);
          OnCall(Op, *Callee,
                 Format == Constants.end() ? nullptr : &Format->second,
                 VectorCount);
        }
      Constants.clear();
      Temporaries.clear();
      VectorCount.reset();
      continue;
    }
    // `lea rdx, [rip + d]` lifts as the next instruction's address plus d.
    std::optional<uint64_t> Value;
    if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT) &&
        Op.NumInputs == 1) {
      Value = ValueOf(Op.Inputs[0]);
    } else if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
               Op.NumInputs == 2) {
      const auto Left = ValueOf(Op.Inputs[0]);
      const auto Right = ValueOf(Op.Inputs[1]);
      if (Left && Right)
        Value = Op.Opcode == NdOp::INT_ADD ? *Left + *Right : *Left - *Right;
    } else if (Op.Opcode == NdOp::INT_XOR && Op.NumInputs == 2 &&
               Op.Inputs[0] == Op.Inputs[1] && !Op.Inputs[0].isConst()) {
      Value = 0;
    }
    if (Value)
      Value = Truncated(*Value, Op.Output.Size);
    if (Op.Output.isTemp()) {
      if (Value)
        Temporaries[Op.Output.Offset] = *Value;
      else
        Temporaries.erase(Op.Output.Offset);
      continue;
    }
    if (!Op.Output.isReg())
      continue;
    const std::optional<unsigned> Family =
        formatRegister(Img.Arch, Op.Output.Offset);
    if (!Family)
      continue;
    const bool LowByte = Op.Output.Offset % 8 == 0;
    if (Family == VectorCountFamily)
      VectorCount = Value && LowByte ? std::optional<uint8_t>(*Value & 0xFF)
                                     : std::nullopt;
    // A write of four or eight bytes defines the whole register; a
    // narrower one keeps the rest of an unknown old value.
    if (Value && LowByte && Op.Output.Size >= 4)
      Constants[*Family] = {*Value};
    else
      Constants.erase(*Family);
  }
  return Constants;
}

} // namespace

void collectFormattedCalls(const BinaryImage &Img, const LowFunc &F,
                           std::map<va_t, FormattedCall> &Calls) {
  collectFormattedCalls(Img, F.Blocks, Calls,
                        CallRegisterBodyView(F).HasIndependentRoots);
}

void collectFormattedCalls(const BinaryImage &Img,
                           llvm::ArrayRef<LowBlock> Blocks,
                           std::map<va_t, FormattedCall> &Calls,
                           bool HasIndependentRoots) {
  const CallArgumentConvention *Convention =
      callArgumentConvention(Img.Arch, Img.abiFormat());
  if (!Convention || !Convention->FormattedCallArguments || Blocks.empty())
    return;
  // A call that paths setting different formats share passes the one its
  // path set: each must name the arguments in the same registers, and an
  // argument carries an address only where every format says so.
  const auto Bind = [&](const LowOp &Call, const PrintfCallee &Callee,
                        const std::set<uint64_t> &Formats,
                        std::optional<uint8_t> VectorCount) {
    std::optional<FormattedCall> Bound;
    for (const uint64_t Format : Formats) {
      std::optional<FormattedCall> Each =
          formattedCall(Img, *Callee.Routine, Format, VectorCount);
      if (!Each || (Bound && !sameCarriers(*Bound, *Each)))
        return;
      if (!Bound) {
        Bound = std::move(Each);
        continue;
      }
      for (size_t I = 0; I < Bound->Arguments.size(); ++I)
        Bound->Arguments[I].Pointer &= Each->Arguments[I].Pointer;
    }
    if (Bound)
      Calls.emplace(Call.Addr, std::move(*Bound));
  };
  // A format the call's block sets binds there.
  bool Unknown = false;
  for (const LowBlock &Block : Blocks)
    scanBlock(Img, Block, {},
              [&](const LowOp &Call, const PrintfCallee &Callee,
                  const std::set<uint64_t> *Formats,
                  std::optional<uint8_t> VectorCount) {
                if (Formats)
                  Bind(Call, Callee, *Formats, VectorCount);
                Unknown |= !Formats;
              });
  // Block views retain only the presence of extra roots, not each root's
  // position. Keep those bodies block-local until every entry can be seeded.
  if (!Unknown || HasIndependentRoots)
    return;
  // A format the block begins with is one its predecessors all leave, a few
  // at most where they leave different ones.  The function's entry and an
  // exception's landing pad begin with nothing known.
  std::map<int, size_t> IndexOf;
  for (size_t I = 0; I < Blocks.size(); ++I)
    IndexOf.emplace(Blocks[I].Id, I);
  std::vector<std::optional<FamilyConstants>> Out(Blocks.size());
  const auto In = [&](size_t Index) {
    const LowBlock &Block = Blocks[Index];
    FamilyConstants Joined;
    if (Index == 0 || Block.Preds.empty() || !Block.ExceptionalPreds.empty())
      return Joined;
    bool First = true;
    for (const int Pred : Block.Preds) {
      const auto It = IndexOf.find(Pred);
      if (It == IndexOf.end())
        return FamilyConstants{};
      // A predecessor not yet scanned joins in a later round.
      if (!Out[It->second])
        continue;
      const FamilyConstants &Leaves = *Out[It->second];
      if (First) {
        Joined = Leaves;
        First = false;
        continue;
      }
      for (auto J = Joined.begin(); J != Joined.end();) {
        const auto L = Leaves.find(J->first);
        if (L != Leaves.end())
          J->second.insert(L->second.begin(), L->second.end());
        if (L == Leaves.end() ||
            J->second.size() >
                ::neverd::limits::pipeline::
                    kPipelineFormattedCallsMaxJoinedFormatConstants)
          J = Joined.erase(J);
        else
          ++J;
      }
    }
    return Joined;
  };
  bool Stable = false;
  for (size_t Round = 0;
       Round < ::neverd::limits::pipeline::
                   kPipelineFormattedCallsMaxFormatJoinRounds &&
       !Stable;
       ++Round) {
    Stable = true;
    for (size_t I = 0; I < Blocks.size(); ++I) {
      FamilyConstants Leaves = scanBlock(Img, Blocks[I], In(I));
      if (!Out[I] || *Out[I] != Leaves) {
        Out[I] = std::move(Leaves);
        Stable = false;
      }
    }
  }
  if (!Stable)
    return;
  for (size_t I = 0; I < Blocks.size(); ++I)
    scanBlock(Img, Blocks[I], In(I),
              [&](const LowOp &Call, const PrintfCallee &Callee,
                  const std::set<uint64_t> *Formats,
                  std::optional<uint8_t> VectorCount) {
                if (Formats && !Calls.count(Call.Addr))
                  Bind(Call, Callee, *Formats, VectorCount);
              });
}

} // namespace neverd::pipeline_detail
