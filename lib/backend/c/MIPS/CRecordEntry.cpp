//===- CRecordEntry.cpp - MIPS by-value C entry points -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "CRecordEntry.h"

#include "../CRecordCall.h"

#include "neverd/debug/UserPrototype.h"
#include "neverd/ir/MIPS/SourceRecordABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

using namespace neverd;

namespace {
#define MIPS_C_RECORD_ENTRY_TEXT(Name, Text) constexpr char Name[] = Text;
#include "CRecordEntry.def"

bool record(const TypeRef &T) {
  return T && T->Kind == NdTypeKind::Struct && !T->IsEnum;
}

bool word(uint16_t Bytes) {
  return Bytes == 1 || Bytes == 2 || Bytes == 4 || Bytes == 8;
}

std::string sourceType(const TypeRef &T, const CEmitterOptions &Opts) {
  return record(T) ? c_detail::recordCallType(T, Opts) : typeToC(T);
}

std::string bytes(llvm::StringRef Object, unsigned Offset, bool Read) {
  return (Read ? "(const unsigned char *)&" : "(unsigned char *)&") +
         Object.str() + (Offset ? " + " + std::to_string(Offset) : "");
}

unsigned memoryOffset(unsigned Width, unsigned Size, unsigned Bit,
                      bool Little) {
  return Little ? Bit / 8 : Width - Size - Bit / 8;
}
} // namespace

bool mips::hasRecordEntryDeclaration(const FunctionSym &Function) {
  return record(Function.ReturnType) ||
         llvm::any_of(Function.Params,
                      [](const auto &P) { return record(P.second); });
}

std::optional<mips::CRecordEntry>
mips::cRecordEntry(const CEmitterOptions &Options, const DebugContext *Debug,
                   const FunctionSym &Function, va_t Entry,
                   llvm::StringRef Name, llvm::ArrayRef<CEntryCarrier> Inputs,
                   const CEntryCarrier &Result) {
  if (!Debug || !Options.Image || !Options.Image->MIPS ||
      Options.Format != BinaryFormat::ELF || !Function.PlatformConvention ||
      Function.Addr != Entry || Function.IsVariadic || Function.NoReturn ||
      !Options.RecordDefinitions || !Function.ReturnType ||
      !hasRecordEntryDeclaration(Function) ||
      (!Debug->hasAuthenticatedFunctionSignatures() &&
       !isUserFunctionDeclaration(Function)) ||
      Options.Image->instructionModeAt(Entry) == InstructionMode::MIPS16)
    return std::nullopt;

  std::vector<TypeRef> Types;
  for (const auto &[Name, T] : Function.Params) {
    if (!T)
      return std::nullopt;
    Debug->completeType(T);
    Types.push_back(T);
  }
  Debug->completeType(Function.ReturnType);
  const auto Layout = placeRecordCall(Options.TheArch, Options.Format,
                                      Function.ReturnType, Types);
  if (!Layout)
    return std::nullopt;

  const auto &TRI = getTargetRegInfo(Options.TheArch);
  const bool Little = archLittleEndian(Options.TheArch);
  const auto &Hidden = Layout->Arguments.ResultPointer;
  const bool HardFloat =
      Options.Image->MIPS->hasScalarFP64ABI(Options.TheArch) ||
      Options.Image->MIPS->hasScalarFP32ABI(Options.TheArch);
  std::vector<unsigned> ResultOffsets;
  if (!Hidden && record(Function.ReturnType)) {
    const bool Tuple = Result.Kind == NdTypeKind::Struct;
    if (Tuple ? Layout->Result.size() != Result.Fields.size()
              : Layout->Result.size() != 1)
      return std::nullopt;
    unsigned End = 0;
    for (unsigned I = 0; I < Layout->Result.size(); ++I) {
      const auto &P = Layout->Result[I];
      const auto Field =
          Tuple ? Result.Fields[I]
                : CEntryCarrier::Field{Result.Kind, 0, Result.Site.Bytes};
      const bool FP = P.Location.Kind == SourceABICarrierKind::FloatingRegister;
      if (!word(Field.Bytes) || Field.Offset < End ||
          Field.Offset + Field.Bytes > Result.Site.Bytes ||
          (FP && (!HardFloat || Field.Kind != NdTypeKind::Float ||
                  I >= TRI.FPReturnRegs.size() ||
                  P.Location.RegisterOffset != TRI.FPReturnRegs[I])) ||
          (!FP &&
           (Field.Kind != NdTypeKind::Int || I >= TRI.IntReturnRegs.size() ||
            P.Location.RegisterOffset != TRI.IntReturnRegs[I])) ||
          P.RegisterBitOffset + 8 * P.Location.ValueBytes > 8 * Field.Bytes)
        return std::nullopt;
      ResultOffsets.push_back(Field.Offset +
                              memoryOffset(Field.Bytes, P.Location.ValueBytes,
                                           P.RegisterBitOffset, Little));
      End = Field.Offset + Field.Bytes;
    }
  } else if (!Hidden && Function.ReturnType->Kind != Result.Kind) {
    return std::nullopt;
  }

  std::vector<RecoveredParamSite> Sites;
  for (const auto &Input : Inputs)
    Sites.push_back(Input.Site);
  CRecordEntry Output;
  Output.BodyName = BodyPrefix + llvm::utohexstr(Entry);
  std::string Parameters, Body;
  llvm::raw_string_ostream OS(Body);
  const std::string ResultType = sourceType(Function.ReturnType, Options);
  if (record(Function.ReturnType))
    OS << "    " << ResultType << " " << ResultObject << ";\n";

  for (size_t I = 0; I < Types.size(); ++I) {
    if (I)
      Parameters += ", ";
    Parameters += sourceType(Types[I], Options) + " " + ArgumentPrefix +
                  std::to_string(I);
  }
  Output.Declaration = ResultType + " " + Name.str() + "(" +
                       (Parameters.empty() ? "void" : Parameters) + ")";
  std::vector<std::string> Arguments;
  bool BoundResult = false;
  for (size_t I = 0; I < Inputs.size(); ++I) {
    const auto &Input = Inputs[I];
    const auto &Site = Input.Site;
    if (!word(Site.Bytes) || Input.Spelling.empty() ||
        (Input.Kind != NdTypeKind::Int && Input.Kind != NdTypeKind::Ptr &&
         Input.Kind != NdTypeKind::Float))
      return std::nullopt;
    if (Hidden && Site.RegOff == Hidden->RegisterOffset) {
      if (BoundResult || Input.Kind == NdTypeKind::Float ||
          Site.Bytes < TRI.PointerSize)
        return std::nullopt;
      Arguments.push_back("(" + Input.Spelling + ")(uintptr_t)&" +
                          ResultObject);
      BoundResult = true;
      continue;
    }
    const auto Stack =
        stackParamOffset(Options.TheArch, Options.Format, Sites, I);
    const SourceParameterPiece *Found = nullptr;
    size_t Owner = 0;
    for (size_t J = 0; J < Types.size(); ++J) {
      for (const auto &P : Layout->Arguments.Parameters[J]) {
        if (P.Location.Kind == SourceABICarrierKind::Stack
                ? !Stack || P.Location.EntryStackOffset != *Stack
                : P.Location.RegisterOffset != Site.RegOff)
          continue;
        if (Found || P.Indirect)
          return std::nullopt;
        Found = &P;
        Owner = J;
      }
    }
    if (!Found) {
      // Alignment can skip a register or stack slot on o32. A double in an
      // n32/n64 FPR leaves the same numbered GPR unused. These placeholders
      // keep the physical ordering of later parameters.
      if (!Input.Unused || Input.Kind != NdTypeKind::Int ||
          (!Stack && TRI.integerArgumentLayout(Options.Format)
                             .registerIndex(Site.RegOff) < 0))
        return std::nullopt;
      Arguments.emplace_back("0");
      continue;
    }
    const auto &P = *Found;
    const bool FP = P.Location.Kind == SourceABICarrierKind::FloatingRegister;
    if ((FP && !HardFloat) || P.RegisterBitOffset % 8 ||
        P.RegisterBitOffset + 8 * P.Location.ValueBytes > 8 * Site.Bytes)
      return std::nullopt;
    const auto &T = Types[Owner];
    const auto Size = P.Location.ValueBytes;
    const std::string Arg = ArgumentPrefix + std::to_string(Owner);
    if (!record(T) && !P.Offset && !P.RegisterBitOffset && Size == T->Size &&
        (Input.Kind == T->Kind ||
         (T->Kind == NdTypeKind::Ptr && Input.Kind == NdTypeKind::Int))) {
      Arguments.push_back("(" + Input.Spelling + ")" +
                          (T->Kind == NdTypeKind::Ptr ? "(uintptr_t)" : "") +
                          Arg);
      continue;
    }
    const unsigned Source =
        record(T) || Little ? P.Offset : T->Size - P.Offset - Size;
    if (Source + Size > T->Size)
      return std::nullopt;
    const auto Word = WordPrefix + std::to_string(I);
    // Bytes outside the declared object do not belong to the C argument.
    // The C-to-physical call may choose their value, just as a native caller
    // does.
    OS << "    " << Input.Spelling << " " << Word << " = 0;\n"
       << "    __builtin_memcpy("
       << bytes(Word,
                memoryOffset(Site.Bytes, Size, P.RegisterBitOffset, Little),
                false)
       << ", " << bytes(Arg, Source, true) << ", " << Size << ");\n";
    Arguments.push_back(Word);
  }
  const std::string Call =
      Output.BodyName + "(" + llvm::join(Arguments, ", ") + ")";
  if (Hidden) {
    if (!BoundResult)
      return std::nullopt;
    Output.BodyReturnsVoid = !Layout->ReturnsResultPointer;
    OS << "    " << Call << ";\n    return " << ResultObject << ";\n";
  } else if (record(Function.ReturnType)) {
    OS << "    " << Result.Spelling << " " << ResultWord << " = " << Call
       << ";\n";
    for (unsigned I = 0; I < Layout->Result.size(); ++I) {
      const auto &P = Layout->Result[I];
      OS << "    __builtin_memcpy(" << bytes(ResultObject, P.Offset, false)
         << ", " << bytes(ResultWord, ResultOffsets[I], true) << ", "
         << P.Location.ValueBytes << ");\n";
    }
    OS << "    return " << ResultObject << ";\n";
  } else {
    OS << "    " << (Result.Kind == NdTypeKind::Void ? "" : "return ") << Call
       << ";\n";
  }
  Output.Definition = Output.Declaration + " {\n" + Body + "}\n";
  return Output;
}
