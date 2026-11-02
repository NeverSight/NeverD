//===- ComparisonSource.cpp - Snapshot-bound source pages -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keeps source projection separate from native correspondence and proofs.
//===----------------------------------------------------------------------===//

#include "ComparisonSource.h"

#include "SessionImpl.h"

#include "neverd/sdk/Limits.h"
#include "neverd/sdk/NeverDCAPIDisasm.h"

#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>

namespace neverd::sdk {
ComparisonSourcePage
readComparisonSource(Session &S, const diff::Program &P, uint32_t Index,
                     const ComparisonSourceOptions &Options,
                     const diff::ComparisonControl &Control) {
  ComparisonSourcePage Result;
  const auto Stopped = [&] {
    if (auto State = Control.stop()) {
      Result.State = *State;
      Result.Availability = "unavailable";
      Result.Reason = "source inspection stopped";
      Result.Document.clear();
      return true;
    }
    return false;
  };
  const auto &F = P.Functions.at(Index);
  if (Stopped())
    return Result;
  if (!F.hasBody()) {
    Result.Reason = "no native body is present in this comparison snapshot";
    return Result;
  }
  if (Options.MaxNativeInstructions &&
      P.instructionCount(F) > Options.MaxNativeInstructions) {
    Result.State = diff::ComparisonState::ResourceLimited;
    Result.Reason =
        "function exceeds the requested source instruction allowance";
    return Result;
  }
  try {
    // Select exactly the requested entry, including when an earlier SSA or
    // source request prepared another function. This changes only retained
    // analysis caches; the native comparison snapshot stays immutable.
    if (Options.Refresh || S.OnlyFunctionEntries.size() != 1 ||
        !S.OnlyFunctionEntries.contains(F.Address))
      S.selectFunction(F.Address);
    if (Control.Progress)
      Control.Progress("analysis", 0, 2);
    if (!neverd_prepare_function_view(&S, F.Address,
                                      Options.Representation.c_str()))
      throw std::runtime_error(S.LastError.empty() ? "function analysis failed"
                                                   : S.LastError);
    if (Stopped())
      return Result;
    if (Control.Progress)
      Control.Progress("render", 1, 2);
    using Text = std::unique_ptr<const char, decltype(&neverd_free_string)>;
    Text Raw(neverd_ir_view_json(&S, F.Address, Options.Representation.c_str(),
                                 Options.Offset, Options.Limit),
             neverd_free_string);
    if (!Raw)
      throw std::runtime_error(S.LastError.empty() ? "source emission failed"
                                                   : S.LastError);
    if (Stopped())
      return Result;
    auto Parsed = llvm::json::parse(Raw.get());
    if (!Parsed)
      throw std::runtime_error(llvm::toString(Parsed.takeError()));
    auto *Page = Parsed->getAsObject();
    if (!Page)
      throw std::runtime_error("source emitter returned no page object");
    if (!Page->getString("text")) {
      Result.Reason = Page->getString("reason")
                          .value_or(Page->getString("mapping_status")
                                        .value_or("source unavailable"))
                          .str();
      return Result;
    }
    const auto Total = Page->getInteger("total_lines");
    if (!Total || *Total < 0 || Options.Offset > uint64_t(*Total))
      throw std::runtime_error("source offset exceeds the rendered line count");

    std::map<va_t, uint32_t> Native;
    for (const auto &Block : P.blocks(F))
      for (uint32_t I = 0; I < Block.InstructionCount; ++I) {
        const auto Instruction = Block.FirstInstruction + I;
        Native.emplace(P.Instructions[Instruction].Address, Instruction);
      }
    auto *Rows = Page->getArray("rows");
    if (!Rows)
      throw std::runtime_error("source emitter returned no line records");
    for (auto &Value : *Rows) {
      if (Stopped())
        return Result;
      auto *Row = Value.getAsObject();
      if (!Row)
        throw std::runtime_error("source emitter returned an invalid line");
      llvm::json::Array Anchors;
      if (const auto *Addresses = Row->getArray("addresses"))
        for (const auto &Address : *Addresses) {
          const auto Name = Address.getAsString();
          va_t VA = 0;
          if (!Name || Name->getAsInteger(0, VA))
            throw std::runtime_error(
                "source emitter returned an invalid anchor");
          if (const auto It = Native.find(VA); It != Native.end())
            Anchors.push_back(llvm::json::Object{
                // The row object can relocate its inline array/string storage
                // when this new field is inserted or the evidence is retained.
                // An anchor owns its spelling independently of that storage.
                {"address", Name->str()},
                {"instruction_index", It->second}});
        }
      // Keep the authoritative emitter's addresses, including its explicit
      // synthetic/unmapped rows. Only these separately checked anchors may
      // navigate within the captured comparison.
      (*Row)["comparison_anchors"] = std::move(Anchors);
    }
    Result.Document = std::move(*Page);
    Result.Availability = "available";
    if (Control.Progress)
      Control.Progress("ready", 2, 2);
    (void)Stopped();
  } catch (const std::exception &Error) {
    Result.Availability = "failed";
    Result.Reason = Error.what();
    Result.Document.clear();
    (void)Stopped();
  }
  Result.Reason.resize(std::min<size_t>(Result.Reason.size(),
                                        limits::diff::kMaxSourceReasonBytes));
  return Result;
}

namespace {
struct SourceInterrupted {
  diff::ComparisonState State;
  std::string Availability, Reason;
};
} // namespace

ComparisonSourceDifference
compareSources(Session &AS, const diff::Program &A, uint32_t AI, Session &BS,
               const diff::Program &B, uint32_t BI,
               const ComparisonSourceDifferenceOptions &Options,
               const diff::ComparisonControl &Control) {
  ComparisonSourceDifference Result;
  const auto Poll = [&] {
    if (auto State = Control.stop())
      throw SourceInterrupted{*State, "unavailable",
                              "source comparison stopped"};
  };
  const auto Charge = [&](uint64_t Bytes) {
    Poll();
    if (Bytes > UINT64_MAX - Result.SourceBytes ||
        (Options.MaxSourceBytes &&
         Bytes > Options.MaxSourceBytes - Result.SourceBytes))
      throw SourceInterrupted{
          diff::ComparisonState::ResourceLimited, "unavailable",
          "source pair exceeds the requested evidence byte allowance"};
    Result.SourceBytes += Bytes;
  };
  const auto Collect = [&](Session &S, const diff::Program &P, uint32_t Index,
                           ComparisonSourceDocument &Document) {
    auto PageOptions = Options.Source;
    PageOptions.Offset = 0;
    PageOptions.Limit = kNeverDCAPIDecompileNeverdIrViewJsonLimit;
    bool First = true;
    while (true) {
      Poll();
      auto Page = readComparisonSource(S, P, Index, PageOptions, Control);
      PageOptions.Refresh = false;
      if (Page.Availability != "available")
        throw SourceInterrupted{Page.State, std::move(Page.Availability),
                                std::move(Page.Reason)};
      auto &D = Page.Document;
      const auto Total = D.getInteger("total_lines");
      const auto Offset = D.getInteger("offset");
      auto *Rows = D.getArray("rows");
      const auto Text = D.getString("text");
      if (!Total || *Total < 0 || !Offset || *Offset < 0 || !Rows || !Text ||
          uint64_t(*Offset) != PageOptions.Offset)
        throw std::runtime_error("source page lost its line coordinates");
      if (First) {
        First = false;
        Document.TotalLines = *Total;
        Document.Dialect = D.getString("dialect").value_or("c").str();
        if (!Options.IncludePrelude)
          if (const auto *Prelude = D.getObject("prelude")) {
            const auto Lines = Prelude->getInteger("lines");
            if (!Lines || *Lines < 0 || *Lines > *Total)
              throw std::runtime_error(
                  "source prelude has invalid line coordinates");
            Document.FirstLine = *Lines;
          }
      } else if (uint64_t(*Total) != Document.TotalLines ||
                 D.getString("dialect").value_or("c") != Document.Dialect) {
        throw std::runtime_error("source rendering changed during collection");
      }
      if (Document.FirstLine >= PageOptions.Offset + Rows->size() &&
          Document.FirstLine > PageOptions.Offset) {
        PageOptions.Offset = Document.FirstLine;
        continue;
      }
      size_t Byte = 0;
      for (auto &Value : *Rows) {
        Poll();
        auto *Row = Value.getAsObject();
        const auto Line = Row ? Row->getInteger("line") : std::nullopt;
        if (!Line || *Line < 0 || Byte >= Text->size())
          throw std::runtime_error("source text and line records disagree");
        const auto Newline = Text->find('\n', Byte);
        const auto End =
            Newline == llvm::StringRef::npos ? Text->size() : Newline + 1;
        if (uint64_t(*Line) >= Document.FirstLine) {
          if (uint64_t(*Line) != Document.FirstLine + Document.Lines.size())
            throw std::runtime_error("source page omitted or repeated a line");
          std::string Evidence;
          llvm::raw_string_ostream(Evidence) << Value;
          Charge(End - Byte);
          Charge(Evidence.size());
          Document.Lines.push_back(
              {Text->slice(Byte, End).str(), std::move(*Row)});
        }
        Byte = End;
      }
      if (Byte != Text->size())
        throw std::runtime_error(
            "source text has no corresponding line records");
      const auto Next = D.getInteger("next_offset");
      if (!Next)
        break;
      if (*Next < 0 || uint64_t(*Next) <= PageOptions.Offset || *Next > *Total)
        throw std::runtime_error("source page did not advance");
      PageOptions.Offset = *Next;
    }
    if (Document.Lines.size() != Document.TotalLines - Document.FirstLine)
      throw std::runtime_error("source rendering ended before its final line");
    if (Document.Lines.empty())
      throw SourceInterrupted{diff::ComparisonState::Complete, "unavailable",
                              "no function source lines were emitted"};
  };
  try {
    Collect(AS, A, AI, Result.Documents[0]);
    Collect(BS, B, BI, Result.Documents[1]);
    const auto Lines = [](const ComparisonSourceDocument &D) {
      std::vector<llvm::StringRef> Text;
      Text.reserve(D.Lines.size());
      for (const auto &Line : D.Lines)
        Text.emplace_back(Line.Text);
      return Text;
    };
    const auto Primary = Lines(Result.Documents[0]);
    const auto Secondary = Lines(Result.Documents[1]);
    Result.Alignment =
        diff::alignSourceLines(Primary, Secondary, Options.Alignment, Control);
    Poll();
    if (Result.Alignment.ComputationComplete)
      Result.Availability = "available";
    else
      Result.Reason = "source line alignment stopped before completion";
  } catch (const SourceInterrupted &Stop) {
    Result.Alignment.State = Stop.State;
    Result.Availability = Stop.Availability;
    Result.Reason = Stop.Reason;
  } catch (const std::exception &Error) {
    Result.Availability = "failed";
    Result.Reason = Error.what();
  }
  if (Result.Availability != "available") {
    Result.Alignment.ComputationComplete = false;
    Result.Alignment.Rows.clear();
    Result.Documents = {};
  }
  Result.Reason.resize(std::min<size_t>(Result.Reason.size(),
                                        limits::diff::kMaxSourceReasonBytes));
  return Result;
}
} // namespace neverd::sdk
