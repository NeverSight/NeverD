//===- BunProjectSource.cpp - Verified Bun source reference relocation -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verified Bun source reference relocation.
///
//===----------------------------------------------------------------------===//

#include "BunProjectSource.h"

#include "BunProjectRequire.h"
#include "Internal.h"
#include "RecoveryParser.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/ConvertUTF.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace neverd::web {
namespace {
constexpr uint32_t None = std::numeric_limits<uint32_t>::max();
constexpr uint64_t MaxSteps = 256ULL * 1024 * 1024;

// Recovery models deliberately remain inadmissible to semantic consumers.
// This private syntax-only check protects the relocation walk without routing
// the larger recovery profile through validateSourceModel/sourceLimits.
uint64_t validateRecoveryTree(const SourceAnalysis &Source, uint64_t Bytes) {
  if (Source.Mode != SourceAnalysisMode::Recovery ||
      Source.ParseStatus != "parsed" || Source.Nodes.empty() ||
      Source.Nodes.size() > MaxRecoveryNodes ||
      Bytes > MaxRecoverySourceBytes ||
      Source.Nodes.front().Kind != "Program" ||
      !Source.Nodes.front().ParentID.empty())
    throw Error("bun_project_invalid_source_tree");
  std::vector<bool> HasParent(Source.Nodes.size());
  uint64_t Steps = 0;
  for (uint32_t I = 0; I < Source.Nodes.size(); ++I) {
    ++Steps;
    const auto &Node = Source.Nodes[I];
    if (Node.Start > Node.End || Node.End > Bytes || Node.ID.empty() ||
        (I && !HasParent[I]))
      throw Error("bun_project_invalid_source_tree");
    for (const auto &Child : Node.Children) {
      ++Steps;
      if (Child.Index <= I || Child.Index >= Source.Nodes.size() ||
          HasParent[Child.Index])
        throw Error("bun_project_invalid_source_tree");
      HasParent[Child.Index] = true;
      const auto &N = Source.Nodes[Child.Index];
      if (N.ParentID != Node.ID || N.Start < Node.Start || N.End > Node.End)
        throw Error("bun_project_invalid_source_tree");
    }
  }
  return Steps;
}

bool virtualName(std::u16string_view Text) {
  return Text.starts_with(u"/$bunfs/") || Text.starts_with(u"B:/~BUN/");
}

std::u16string utf16(std::string_view Text) {
  llvm::SmallVector<llvm::UTF16, 64> Units;
  if (!llvm::convertUTF8ToUTF16String(llvm::StringRef(Text), Units))
    throw Error("bun_project_invalid_virtual_name");
  return std::u16string(Units.begin(), Units.end());
}

// Escape code units, not Unicode scalar values: this also faithfully encodes
// any lone surrogate if a later mapping policy admits one.
std::string quote(std::u16string_view Text) {
  constexpr char Hex[] = "0123456789abcdef";
  std::string Result = "\"";
  for (auto Unit : Text) {
    if (Unit >= 32 && Unit < 127 && Unit != u'"' && Unit != u'\\') {
      Result += char(Unit);
    } else {
      Result += "\\u";
      for (int Shift = 12; Shift >= 0; Shift -= 4)
        Result += Hex[(Unit >> Shift) & 15];
    }
  }
  Result += '"';
  return Result;
}

struct PlannedEdit {
  uint32_t Node;
  BunProjectSourceEdit Edit;
};

struct Planner {
  const SourceAnalysis &Source;
  BunProjectSource &Result;
  const std::map<std::u16string, std::u16string> &Targets;
  std::vector<PlannedEdit> Planned;
  std::vector<bool> Handled;
  std::map<uint32_t, uint32_t> ProvenRequireArguments;
  uint64_t Steps = 0;

  void step(uint64_t Count = 1) {
    if (Count > MaxSteps - Steps)
      throw Error("bun_project_source_budget_exceeded");
    Steps += Count;
  }

  uint32_t child(uint32_t Node, std::string_view Field) {
    if (Node == None)
      return None;
    if (Node >= Source.Nodes.size())
      throw Error("bun_project_invalid_source_tree");
    for (const auto &Child : Source.Nodes[Node].Children) {
      step();
      if (Child.Field == Field && Child.Ordinal == 0) {
        if (Child.Index >= Source.Nodes.size())
          throw Error("bun_project_invalid_source_tree");
        return Child.Index;
      }
    }
    return None;
  }

  bool is(uint32_t Node, std::string_view Kind) const {
    return Node != None && Source.Nodes[Node].Kind == Kind;
  }

  bool identifier(uint32_t Node, std::u16string_view Name) const {
    const auto *Text =
        is(Node, "Identifier") ? Source.Nodes[Node].text("name") : nullptr;
    return Text && *Text == Name;
  }

  bool metaRequire(uint32_t Node) {
    if (Source.SourceType != "module" || !is(Node, "MemberExpression") ||
        Source.Nodes[Node].flag("computed") ||
        !identifier(child(Node, "property"), u"require"))
      return false;
    const auto Object = child(Node, "object");
    return is(Object, "MetaProperty") &&
           identifier(child(Object, "meta"), u"import") &&
           identifier(child(Object, "property"), u"meta");
  }

  void diagnostic(std::string_view Code, uint64_t Offset) {
    if (Result.Diagnostics.size() >= MaxBunProjectSourceReferences)
      throw Error("bun_project_reference_budget_exceeded");
    Result.Diagnostics.push_back({std::string(Code), int64_t(Offset)});
  }

  void unresolved(uint32_t Node, std::u16string_view Value) {
    if (Result.UnresolvedRuntimeReferences.size() >=
        MaxBunProjectSourceReferences)
      throw Error("bun_project_reference_budget_exceeded");
    const auto &N = Source.Nodes[Node];
    Result.UnresolvedRuntimeReferences.push_back(
        {N.Start, N.End, std::u16string(Value)});
  }

  void reference(uint32_t Node, std::string_view Kind, uint64_t Offset) {
    if (!is(Node, "StringLiteral")) {
      diagnostic("bun_project_nonliteral_module_reference", Offset);
      return;
    }
    const auto &N = Source.Nodes[Node];
    const auto *Value = N.text("value");
    if (!Value)
      throw Error("bun_project_invalid_literal_model");
    Handled[Node] = true;
    step(Value->size());
    if (!virtualName(*Value)) {
      if (Result.ExternalModuleReferences.size() >=
          MaxBunProjectSourceReferences)
        throw Error("bun_project_reference_budget_exceeded");
      Result.ExternalModuleReferences.push_back({N.Start, N.End, *Value});
      return;
    }
    const auto Target = Targets.find(*Value);
    if (Target == Targets.end()) {
      diagnostic("bun_project_unknown_virtual_target", N.Start);
      unresolved(Node, *Value);
      return;
    }
    if (Planned.size() >= MaxBunProjectSourceReferences)
      throw Error("bun_project_reference_budget_exceeded");
    Planned.push_back(
        {Node, {N.Start, N.End, *Value, Target->second, std::string(Kind)}});
  }

  void admitRequireCalls(std::string_view ArtifactID, std::string_view Text,
                         std::span<const BunProjectRequireCall> Calls) {
    if (Calls.empty())
      return;
    if (Calls.size() > MaxBunProjectRequireRecords)
      throw Error("bun_project_reference_budget_exceeded");
    if (Source.SourceType != "module")
      throw Error("bun_project_invalid_require_proof");
    step(Text.size());
    const auto Hash = sha256(Text);
    using Span = std::pair<uint64_t, uint64_t>;
    std::map<Span, const BunProjectRequireCall *> BySpan;
    for (const auto &Call : Calls) {
      step();
      if (!validBunProjectRequireCall(Call))
        throw Error("bun_project_invalid_require_proof");
      const auto &Proof = Call.data();
      if (Proof.SourceArtifactID != ArtifactID || Proof.SourceHash != Hash ||
          !BySpan.emplace(Span{Proof.CallStart, Proof.CallEnd}, &Call).second)
        throw Error("bun_project_invalid_require_proof");
    }
    for (uint32_t I = 0; I < Source.Nodes.size(); ++I) {
      step();
      const auto &N = Source.Nodes[I];
      if (N.Kind != "CallExpression")
        continue;
      const auto Found = BySpan.find({N.Start, N.End});
      if (Found == BySpan.end())
        continue;
      const auto &Proof = Found->second->data();
      const auto Callee = child(I, "callee");
      if (!is(Callee, "Identifier") ||
          Source.Nodes[Callee].Start != Proof.CalleeStart ||
          Source.Nodes[Callee].End != Proof.CalleeEnd)
        throw Error("bun_project_invalid_require_proof");
      uint32_t Argument = None;
      for (const auto &C : N.Children) {
        step();
        if (C.Field == "arguments") {
          if (Argument != None)
            throw Error("bun_project_invalid_require_proof");
          Argument = C.Index;
        }
      }
      if (!is(Argument, "StringLiteral"))
        throw Error("bun_project_invalid_require_proof");
      const auto &Literal = Source.Nodes[Argument];
      const auto *Value = Literal.text("value");
      step(Proof.Target.size());
      if (Literal.Start != Proof.OriginalStart ||
          Literal.End != Proof.OriginalEnd || !Value || *Value != Proof.Target)
        throw Error("bun_project_invalid_require_proof");
      if (!ProvenRequireArguments.emplace(I, Argument).second)
        throw Error("bun_project_invalid_require_proof");
      BySpan.erase(Found);
    }
    if (!BySpan.empty())
      throw Error("bun_project_invalid_require_proof");
  }

  void collect() {
    Handled.assign(Source.Nodes.size(), false);
    for (uint32_t I = 0; I < Source.Nodes.size(); ++I) {
      step();
      const auto &N = Source.Nodes[I];
      if (N.Kind == "ImportDeclaration") {
        reference(child(I, "source"), "import", N.Start);
      } else if (N.Kind == "ExportNamedDeclaration" ||
                 N.Kind == "ExportAllDeclaration") {
        const auto Target = child(I, "source");
        if (Target != None)
          reference(Target,
                    N.Kind == "ExportNamedDeclaration" ? "export_named"
                                                       : "export_all",
                    N.Start);
      } else if (N.Kind == "ImportExpression") {
        reference(child(I, "source"), "import_expression", N.Start);
      } else if (N.Kind == "CallExpression" &&
                 metaRequire(child(I, "callee"))) {
        uint32_t Argument = None;
        uint64_t Arguments = 0;
        for (const auto &C : N.Children) {
          step();
          if (C.Field == "arguments") {
            Argument = C.Index;
            ++Arguments;
          }
        }
        if (Arguments != 1)
          diagnostic("bun_project_import_meta_require_arity", N.Start);
        else
          reference(Argument, "import_meta_require", N.Start);
      } else if (const auto Proof = ProvenRequireArguments.find(I);
                 Proof != ProvenRequireArguments.end()) {
        reference(Proof->second, "imported_import_meta_require", N.Start);
      }
    }
    for (uint32_t I = 0; I < Source.Nodes.size(); ++I) {
      step();
      if (Handled[I] || !is(I, "StringLiteral"))
        continue;
      const auto *Value = Source.Nodes[I].text("value");
      if (!Value)
        throw Error("bun_project_invalid_literal_model");
      step(Value->size());
      if (virtualName(*Value)) {
        diagnostic("bun_project_unresolved_runtime_reference",
                   Source.Nodes[I].Start);
        unresolved(I, *Value);
      }
    }
    std::sort(Planned.begin(), Planned.end(), [](const auto &A, const auto &B) {
      return A.Edit.Start < B.Edit.Start;
    });
    std::sort(Result.UnresolvedRuntimeReferences.begin(),
              Result.UnresolvedRuntimeReferences.end(),
              [](const auto &A, const auto &B) { return A.Start < B.Start; });
    std::stable_sort(Result.Diagnostics.begin(), Result.Diagnostics.end(),
                     [](const auto &A, const auto &B) {
                       return A.ByteOffset < B.ByteOffset;
                     });
  }

  bool sameTree(const SourceAnalysis &Candidate) {
    if (Source.SourceType != Candidate.SourceType ||
        Source.Nodes.size() != Candidate.Nodes.size())
      return false;
    std::vector<uint32_t> Expected(Source.Nodes.size(), None);
    for (uint32_t I = 0; I < Planned.size(); ++I)
      Expected[Planned[I].Node] = I;
    // Validated models have a unique preorder tree. Equal child indices,
    // fields and ordinals compare the full recursive topology without relying
    // on content-derived IDs or byte positions, which necessarily change.
    for (size_t I = 0; I < Source.Nodes.size(); ++I) {
      step();
      const auto &A = Source.Nodes[I], &B = Candidate.Nodes[I];
      if (A.Kind != B.Kind || A.Strict != B.Strict ||
          A.Children.size() != B.Children.size() ||
          A.Attributes.size() != B.Attributes.size())
        return false;
      for (size_t J = 0; J < A.Children.size(); ++J) {
        step();
        const auto &X = A.Children[J], &Y = B.Children[J];
        if (X.Field != Y.Field || X.Index != Y.Index || X.Ordinal != Y.Ordinal)
          return false;
      }
      bool MatchedEdit = false;
      for (size_t J = 0; J < A.Attributes.size(); ++J) {
        step();
        const auto &X = A.Attributes[J], &Y = B.Attributes[J];
        if (X.Field != Y.Field)
          return false;
        if (const auto *Text = std::get_if<std::u16string>(&X.Value))
          step(Text->size());
        if (Expected[I] != None && X.Field == "value") {
          const auto &Edit = Planned[Expected[I]].Edit;
          const auto *Old = std::get_if<std::u16string>(&X.Value);
          const auto *New = std::get_if<std::u16string>(&Y.Value);
          if (A.Kind != "StringLiteral" || !Old || !New ||
              *Old != Edit.OldTarget || *New != Edit.NewTarget)
            return false;
          MatchedEdit = true;
        } else if (X.Value != Y.Value) {
          return false;
        }
      }
      if (Expected[I] != None && !MatchedEdit)
        return false;
    }
    return true;
  }
};
} // namespace

BunProjectSource
planBunProjectSource(std::string_view ArtifactID, std::string_view Text,
                     std::string_view SourceType,
                     std::span<const BunProjectSourceTarget> Targets,
                     std::span<const BunProjectRequireCall> RequireCalls) {
  BunProjectSource Result;
  try {
    if (Targets.size() > MaxBunProjectSourceTargets)
      throw Error("bun_project_target_budget_exceeded");
    std::map<std::u16string, std::u16string> Mapping;
    std::set<std::string> OutputNames;
    uint64_t TargetBytes = 0;
    for (const auto &Target : Targets) {
      for (const auto &Text : {std::string_view(Target.VirtualName),
                               std::string_view(Target.OutputFile)}) {
        if (Text.size() > MaxBunProjectTargetBytes - TargetBytes)
          throw Error("bun_project_target_budget_exceeded");
        TargetBytes += Text.size();
      }
      const auto Name = utf16(Target.VirtualName);
      if (!virtualName(Name) || Name.find(u'\0') != Name.npos)
        throw Error("bun_project_invalid_virtual_name");
      if (Target.OutputFile.empty() ||
          std::any_of(Target.OutputFile.begin(), Target.OutputFile.end(),
                      [](unsigned char C) {
                        return !((C >= 'a' && C <= 'z') ||
                                 (C >= 'A' && C <= 'Z') ||
                                 (C >= '0' && C <= '9') || C == '.' ||
                                 C == '_' || C == '-');
                      }))
        throw Error("bun_project_invalid_output_file");
      try {
        validateMemberName(Target.OutputFile);
      } catch (const Error &) {
        throw Error("bun_project_invalid_output_file");
      }
      if (!OutputNames.insert(nameKey(Target.OutputFile)).second)
        throw Error("bun_project_duplicate_output_file");
      const auto Output = utf16("./" + Target.OutputFile);
      if (!Mapping.emplace(Name, Output).second)
        throw Error("bun_project_duplicate_virtual_name");
    }
    auto Source = inspectRecoveryJavaScript(ArtifactID, Text, SourceType);
    Result.ParseStatus = Source.ParseStatus;
    Result.Diagnostics = Source.Diagnostics;
    if (Source.ParseStatus != "parsed" || Source.LexemeStatus != "ok") {
      Result.Status = Source.ParseStatus == "parsed" ? Source.LexemeStatus
                                                     : Source.ParseStatus;
      return Result;
    }
    Planner Plan{Source, Result, Mapping, {}, {}, {}, 0};
    Plan.step(Text.size() + TargetBytes);
    Plan.step(validateRecoveryTree(Source, Text.size()));
    Plan.admitRequireCalls(ArtifactID, Text, RequireCalls);
    Plan.collect();
    std::string Candidate;
    auto Append = [&](std::string_view Part) {
      if (Part.size() > MaxRecoverySourceBytes - Candidate.size())
        throw Error("bun_project_source_budget_exceeded");
      Plan.step(Part.size());
      Candidate += Part;
    };
    uint64_t At = 0;
    for (const auto &P : Plan.Planned) {
      const auto &Edit = P.Edit;
      if (Edit.Start < At || Edit.Start >= Edit.End || Edit.End > Text.size())
        throw Error("bun_project_invalid_edit_span");
      Append(Text.substr(At, Edit.Start - At));
      Append(quote(Edit.NewTarget));
      At = Edit.End;
    }
    Append(Text.substr(At));
    auto Reparsed =
        inspectRecoveryJavaScript(ArtifactID, Candidate, SourceType);
    if (Reparsed.ParseStatus != "parsed" || Reparsed.LexemeStatus != "ok")
      throw Error("bun_project_reparse_mismatch");
    Plan.step(validateRecoveryTree(Reparsed, Candidate.size()));
    if (!Plan.sameTree(Reparsed))
      throw Error("bun_project_reparse_mismatch");
    for (auto &P : Plan.Planned)
      Result.Edits.push_back(std::move(P.Edit));
    Result.Text = std::move(Candidate);
    Result.Status = "verified_literal_relocations";
  } catch (const Error &E) {
    Result.Status = E.what();
    Result.Text.clear();
    Result.Edits.clear();
    Result.Diagnostics.push_back({Result.Status, -1});
  }
  return Result;
}
} // namespace neverd::web
