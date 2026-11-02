//===- PipelineLibraryRecognition.cpp - Shared presentation evidence -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedLibraryRecognition.h"
#include "neverd/pipeline/Limits.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/sigs/SignatureDB.h"

#include "llvm/ADT/STLExtras.h"

#include <cctype>

namespace neverd {
namespace {

std::string typeSpelling(llvm::StringRef Name) {
  // Only whitespace around punctuation is immaterial. Keep word boundaries:
  // the built-in 'unsigned int' must not equal a user type 'unsignedint'.
  std::string Result;
  bool Space = false;
  auto Word = [](unsigned char C) { return std::isalnum(C) || C == '_'; };
  for (unsigned char C : Name) {
    if (std::isspace(C)) {
      Space = true;
      continue;
    }
    if (Space && !Result.empty() && Word(Result.back()) && Word(C))
      Result += ' ';
    Space = false;
    Result += C;
  }
  return Result;
}

bool sameTypeSpelling(llvm::StringRef Actual, llvm::StringRef Expected) {
  if (Actual == Expected)
    return true;
  // CodeView spells the fixed boolean template argument numerically.
  // Only an explicitly false argument in the admitted receiver can use this
  // spelling; ordinary integral arguments and other configurations stay
  // distinct.
  std::string Numeric = Expected.str();
  size_t At = 0;
  while ((At = Numeric.find(",false>", At)) != std::string::npos)
    Numeric.replace(At, 7, ",0>");
  return Actual == Numeric;
}

std::optional<uint64_t> returnedEntryRegister(const MedFunc &Function) {
  using Key = std::tuple<int, int, int>;
  auto key = [](const MedVar &V) -> Key { return {V.Kind, V.Id, V.SSAVer}; };
  std::map<Key, const MedOp *> Defs;
  size_t Count = 0;
  for (const auto &B : Function.Blocks) {
    for (const auto &P : B.Phis)
      Defs[key(P.Output)] = nullptr;
    for (const auto &O : B.Ops) {
      if (++Count >
              ::neverd::limits::pipeline::
                  kPipelineLibraryRecognitionReturnedEntryRegisterCountLimit ||
          O.NumInputs > O.Inputs.size())
        return std::nullopt;
      if (O.Dead || !O.Output.Size)
        continue;
      if (O.Opcode == NdOp::COPY && O.NumInputs == 1 &&
          O.Output == O.Inputs[0] && O.Output.Size == O.Inputs[0].Size)
        continue;
      if (!Defs.emplace(key(O.Output), &O).second)
        return std::nullopt;
    }
  }
  for (const auto &C : Function.CallClobbers)
    Defs.try_emplace(key(C.Value), nullptr);
  std::optional<uint64_t> Register;
  for (const auto &B : Function.Blocks)
    for (const auto &O : B.Ops) {
      if (O.Dead || O.Opcode != NdOp::RETURN)
        continue;
      if (O.NumInputs != 1 || O.Inputs[0].Size != 8)
        return std::nullopt;
      MedVar V = O.Inputs[0];
      unsigned Depth = 0;
      for (;;) {
        const auto At = Defs.find(key(V));
        if (At == Defs.end())
          break;
        const auto *D = At->second;
        if (++Depth >
                ::neverd::limits::pipeline::
                    kPipelineLibraryRecognitionReturnedEntryRegisterDepthLimit ||
            !D || D->Opcode != NdOp::COPY || D->NumInputs != 1 ||
            D->Inputs[0].Size != 8)
          return std::nullopt;
        V = D->Inputs[0];
      }
      if (V.Kind != MedVar::Reg || V.SSAVer != 0 || V.Size != 8 ||
          (Register && *Register != V.RegOff))
        return std::nullopt;
      Register = V.RegOff;
    }
  return Register;
}

std::vector<MedLibraryReceiver>
receivers(const BinaryImage &Image, const MedFunc &Function, DebugContext *Dbg,
          const std::map<std::string, sigs::LibraryFeaturePack> &Packs) {
  std::vector<MedLibraryReceiver> Result;
  if (!Dbg)
    return Result;
  auto Parameters = Dbg->resolveAuthenticatedRecordParameters(Function.Entry);
  bool ReturnAlias = false;
  if (Image.Arch == Arch::X64 && Image.Format == BinaryFormat::COFF)
    if (auto Return = Dbg->resolveAuthenticatedRecordReturn(Function.Entry);
        Return && Return->Register == getTargetRegInfo(Image.Arch).IntReturnReg)
      if (auto Register = returnedEntryRegister(Function)) {
        Return->Register = *Register;
        Parameters.push_back(std::move(*Return));
        ReturnAlias = true;
      }
  for (size_t I = 0; I < Parameters.size(); ++I) {
    const auto &Parameter = Parameters[I];
    const std::string Spelling = typeSpelling(Parameter.QualifiedType);
    std::string Identity = Parameter.QualifiedType;
    for (const auto &[ID, Pack] : Packs) {
      (void)ID;
      if (!Pack.accepts(Image.Arch, Image.abiFormat(), Image.Bits))
        continue;
      for (const auto &[LayoutID, Layout] : Pack.Layouts) {
        (void)LayoutID;
        if (sameTypeSpelling(Spelling, typeSpelling(Layout.ReceiverType)))
          Identity = Layout.ReceiverType;
        for (const auto &[Derived, Offset] : Layout.DerivedReceivers)
          if (Offset == 0 && sameTypeSpelling(Spelling, typeSpelling(Derived)))
            Identity = Derived;
      }
    }
    Result.push_back({Parameter.Register, Identity, Parameter.ObjectBytes,
                      ReturnAlias && I + 1 == Parameters.size()
                          ? "authenticated-debug-return-alias"
                          : "authenticated-debug-parameter"});
  }
  return Result;
}

} // namespace

void Pipeline::recognizeLibraries(const BinaryImage &Img,
                                  const PipelineOptions &Opts,
                                  PipelineResult &Result, DebugContext *Dbg) {
  if (!Opts.LibraryFeatures || Opts.LibraryFeatures->empty() ||
      (Img.Arch != Arch::X64 && Img.Arch != Arch::AArch64) ||
      (Opts.usesMachineMedIR() && !Opts.SourceProjection))
    return;
  MedLibraryIdentityIndex Identities(Img);
  // The database owns the pack snapshot. Byte recognition uses the same
  // immutable feature view as structural recognition without granting names
  // to ABI, exception, or source-prototype recovery.
  std::vector<uint64_t> Entries;
  std::map<va_t, std::vector<MedLibraryReceiver>> Receivers;
  for (const auto &F : Result.MedFuncs) {
    Entries.push_back(F.Entry);
    Receivers.emplace(F.Entry, receivers(Img, F, Dbg, *Opts.LibraryFeatures));
  }
  auto ByteMatches = sigs::recognizeLibraryFeatureBytes(
      Img, *Opts.LibraryFeatures, Entries,
      [&](const auto &Pack, const sigs::LibraryFeatureRule &Rule,
          va_t Entry) -> std::string {
        if (Rule.Identity == sigs::LibraryFeatureIdentity::ByteSignature)
          return "whole-byte-pattern";
        const auto Layout = Pack.Layouts.find(Rule.Layout);
        if (Layout == Pack.Layouts.end())
          return "";
        const auto Params =
            getTargetRegInfo(Img.Arch).integerParamRegs(Img.abiFormat());
        if (Params.empty())
          return "";
        for (const auto &R : Receivers.at(Entry)) {
          if (R.Register != Params.front() || R.Evidence.empty())
            continue;
          const auto Derived = Layout->second.DerivedReceivers.find(R.Type);
          if (R.ObjectBytes != Layout->second.ObjectBytes ||
              (R.Type != Rule.ReceiverType &&
               (Derived == Layout->second.DerivedReceivers.end() ||
                Derived->second != 0)))
            return "";
        }
        const auto Names = Identities.names(Img, Entry);
        return llvm::is_contained(Names, Rule.LinkageName)
                   ? "stated-member-symbol+byte-pattern"
                   : "";
      });
  std::map<va_t, std::vector<sigs::LibraryRecognition>> BytesByEntry;
  for (auto &Byte : ByteMatches)
    BytesByEntry[Byte.Function].push_back(std::move(Byte));
  for (const MedFunc &Function : Result.MedFuncs) {
    Result.LibraryRecognitionsByFunction.try_emplace(Function.Entry);
    if (Img.functionHasBytePatch(Function.Entry) ||
        llvm::any_of(Function.Blocks, [&](const auto &Block) {
          return Block.EndAddr > Block.StartAddr &&
                 Img.hasBytePatchIn(Block.StartAddr,
                                    Block.EndAddr - Block.StartAddr);
        }))
      continue;
    auto Evidence = recognizeMedLibraryOperations(
        Function, Img, *Opts.LibraryFeatures, Receivers.at(Function.Entry),
        250000, &Identities);
    if (Evidence.BudgetExhausted)
      Result.LibraryRecognitionBudgetExhausted.insert(Function.Entry);
    for (auto &Byte : BytesByEntry[Function.Entry]) {
      if (Byte.Function != Function.Entry || !Byte.ByteLength ||
          Function.Entry > InvalidVA - Byte.ByteLength)
        continue;
      const va_t End = Function.Entry + Byte.ByteLength;
      const bool EntireBody =
          !llvm::any_of(Function.Blocks, [&](const auto &B) {
            return B.StartAddr < Function.Entry || B.EndAddr > End;
          });
      for (const auto &Block : Function.Blocks)
        for (const auto &Op : Block.Ops)
          if (!Op.Dead && Op.Addr >= Function.Entry && Op.Addr < End &&
              Op.OriginSeq >= 0)
            Byte.Occurrences.push_back({Op.Addr, Op.OriginSeq});
      std::sort(Byte.Occurrences.begin(), Byte.Occurrences.end());
      Byte.Occurrences.erase(
          std::unique(Byte.Occurrences.begin(), Byte.Occurrences.end()),
          Byte.Occurrences.end());
      // A tail-shared CFG can include another function's instructions. The
      // checked byte identity still holds at this entry, but its expanded
      // source body must remain visible until all its extents are proved.
      Byte.Isolated = EntireBody && !Byte.Occurrences.empty();
      Evidence.Matches.push_back(std::move(Byte));
    }
    size_t ConflictWork =
        ::neverd::limits::pipeline::kPipelineLibraryRecognitionConflictWork;
    if (!sigs::finalizeLibraryRecognitions(Evidence.Matches, ConflictWork))
      Result.LibraryRecognitionBudgetExhausted.insert(Function.Entry);
    llvm::append_range(Result.LibraryRecognitions, std::move(Evidence.Matches));
  }
  // Only a previously verified whole function may annotate its original
  // direct calls. No inferred callee, extra function or modified operand is
  // introduced for an inline region.
  std::map<va_t, size_t> Whole;
  std::set<va_t> Ambiguous;
  std::map<va_t, std::vector<size_t>> BodyMatches;
  for (size_t I = 0; I < Result.LibraryRecognitions.size(); ++I) {
    const auto &R = Result.LibraryRecognitions[I];
    BodyMatches[R.Function].push_back(I);
    if (R.Scope == sigs::LibraryFeatureScope::WholeFunction &&
        !Whole.emplace(R.Function, I).second)
      Ambiguous.insert(R.Function);
  }
  for (va_t Entry : Ambiguous)
    Whole.erase(Entry);
  for (const auto &F : Result.MedFuncs) {
    std::vector<sigs::LibraryRecognition> Calls;
    size_t Work = ::neverd::limits::pipeline::
        kPipelineLibraryRecognitionPipelineRecognizeLibrariesWork;
    bool Exhausted = false;
    for (const auto &B : F.Blocks)
      for (const auto &O : B.Ops) {
        if (!Work ||
            Calls.size() >=
                ::neverd::limits::pipeline::
                    kPipelineLibraryRecognitionPipelineRecognizeLibrariesCallsSizeLimit) {
          Exhausted = true;
          break;
        }
        --Work;
        if (O.Dead || O.Opcode != NdOp::CALL || !O.NumInputs ||
            !O.Inputs[0].isConst() || O.Addr == InvalidVA || O.OriginSeq < 0)
          continue;
        const auto Target = Whole.find(O.Inputs[0].ConstVal);
        if (Target == Whole.end())
          continue;
        auto Call = Result.LibraryRecognitions[Target->second];
        Call.Callee = Call.Function;
        Call.Function = F.Entry;
        Call.Scope = sigs::LibraryFeatureScope::CallSite;
        Call.Occurrences = {{O.Addr, O.OriginSeq}};
        Call.ResultOccurrence = Call.Occurrences.front();
        Call.ByteLength = 0;
        Call.IdentityEvidence += "+original-direct-call";
        Call.Isolated = !Whole.contains(F.Entry);
        // A larger region already owns this effect; retain the call identity
        // as an annotation without creating overlapping folds.
        for (size_t I : BodyMatches[F.Entry]) {
          const auto &R = Result.LibraryRecognitions[I];
          if (R.Isolated &&
              std::binary_search(R.Occurrences.begin(), R.Occurrences.end(),
                                 Call.Occurrences.front()))
            Call.Isolated = false;
        }
        Calls.push_back(std::move(Call));
      }
    if (Exhausted)
      Result.LibraryRecognitionBudgetExhausted.insert(F.Entry);
    else
      llvm::append_range(Result.LibraryRecognitions, std::move(Calls));
  }
  for (size_t I = 0; I < Result.LibraryRecognitions.size(); ++I)
    Result.LibraryRecognitionsByFunction[Result.LibraryRecognitions[I].Function]
        .push_back(I);
}

} // namespace neverd
