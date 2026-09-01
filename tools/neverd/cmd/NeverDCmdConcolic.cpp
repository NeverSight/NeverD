//===- NeverDCmdConcolic.cpp - LowIR concolic CLI ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "neverd/lift/AArch64Regs.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace neverd::cli {

namespace {

struct SeedSpec {
  std::string Location;
  uint32_t Bytes = 0;
  uint64_t Value = 0;
};

struct ImageABI {
  std::string Architecture;
  std::string Format;
};

struct PositionalPool {
  std::vector<std::string> Values;
  std::vector<unsigned> Positions;
  std::vector<bool> Consumed;

  PositionalPool() {
    Values.reserve(ConcolicInputFiles.size());
    Positions.reserve(ConcolicInputFiles.size());
    for (size_t I = 0; I < ConcolicInputFiles.size(); ++I) {
      Values.push_back(ConcolicInputFiles[I]);
      Positions.push_back(ConcolicInputFiles.getPosition(I));
    }
    Consumed.resize(Values.size());
  }

  std::vector<std::string> remaining() const {
    std::vector<std::string> Result;
    for (size_t I = 0; I < Values.size(); ++I)
      if (!Consumed[I])
        Result.push_back(Values[I]);
    return Result;
  }
};

bool isMissingOptionalValue(StringRef Value) {
  return Value.size() == 1 && Value.front() == '\0';
}

std::vector<std::string> recoverOptionalValues(const ConcolicStringList &Values,
                                               PositionalPool &Positionals) {
  std::vector<std::string> Result;
  Result.reserve(Values.size());
  for (size_t I = 0; I < Values.size(); ++I) {
    if (!isMissingOptionalValue(Values[I])) {
      Result.push_back(Values[I]);
      continue;
    }

    // ValueOptional keeps LLVM from terminating before the JSON handler.  To
    // retain the conventional `--flag value` spelling, claim an immediately
    // following positional token; a genuinely missing value becomes empty
    // and is rejected below with a versioned invalid_arguments report.
    const unsigned ExpectedPosition = Values.getPosition(I) + 1;
    bool Recovered = false;
    for (size_t J = 0; J < Positionals.Values.size(); ++J) {
      if (!Positionals.Consumed[J] &&
          Positionals.Positions[J] == ExpectedPosition) {
        Result.push_back(Positionals.Values[J]);
        Positionals.Consumed[J] = true;
        Recovered = true;
        break;
      }
    }
    if (!Recovered)
      Result.emplace_back();
  }
  return Result;
}

std::string renderJSON(json::Object Object) {
  std::string Text;
  raw_string_ostream OS(Text);
  OS << json::Value(std::move(Object));
  OS.flush();
  return Text;
}

std::string errorReport(StringRef Code, StringRef Message) {
  return renderJSON(json::Object{{"schema_version", 1},
                                 {"adapter", "lowir-concolic-v1"},
                                 {"mode", "concolic"},
                                 {"ok", false},
                                 {"exhaustive", false},
                                 {"error_code", Code},
                                 {"error", Message}});
}

bool writeText(StringRef Text, StringRef OutputPath, std::string &Error) {
  if (OutputPath.empty()) {
    outs() << Text;
    if (!Text.ends_with("\n"))
      outs() << '\n';
    return true;
  }

  std::error_code EC;
  raw_fd_ostream OS(OutputPath, EC);
  if (EC) {
    Error =
        "cannot open output file '" + OutputPath.str() + "': " + EC.message();
    return false;
  }
  OS << Text;
  if (!Text.ends_with("\n"))
    OS << '\n';
  OS.flush();
  if (OS.has_error()) {
    Error = "cannot write output file '" + OutputPath.str() + "'";
    return false;
  }
  return true;
}

int emit(StringRef Report, StringRef OutputPath, bool Ok) {
  std::string Error;
  if (writeText(Report, OutputPath, Error))
    return Ok ? 0 : 1;

  // The requested destination itself failed, so stdout becomes the only
  // reliable channel.  It still carries one versioned JSON value and no
  // banner or prose.
  outs() << errorReport("output_error", Error) << '\n';
  return 1;
}

int fail(StringRef Code, StringRef Message, StringRef OutputPath = {}) {
  const std::string Report = errorReport(Code, Message);
  return emit(Report, OutputPath, false);
}

bool parseDecimalOrHex(StringRef Text, uint64_t &Value) {
  if (Text.empty() || Text.front() == '-' || Text.front() == '+')
    return false;
  unsigned Radix = 10;
  if (Text.consume_front("0x") || Text.consume_front("0X")) {
    if (Text.empty())
      return false;
    Radix = 16;
  }
  return !Text.getAsInteger(Radix, Value);
}

bool readOne(ArrayRef<std::string> Values, StringRef Flag, uint64_t Maximum,
             uint64_t &Value, std::string &Error) {
  if (Values.size() > 1) {
    Error = "--" + Flag.str() + " may be specified only once";
    return false;
  }
  if (Values.empty()) {
    Value = 0;
    return true;
  }
  uint64_t Parsed = 0;
  if (!parseDecimalOrHex(Values.front(), Parsed)) {
    Error = "--" + Flag.str() + " requires a decimal or 0x-prefixed integer";
    return false;
  }
  if (Parsed > Maximum) {
    Error = "--" + Flag.str() + " is out of range";
    return false;
  }
  Value = Parsed;
  return true;
}

bool parseSeed(StringRef Text, SeedSpec &Result, std::string &Error) {
  const size_t Colon = Text.find(':');
  const size_t Equal = Text.find('=');
  if (Colon == StringRef::npos || Equal == StringRef::npos || Colon == 0 ||
      Equal <= Colon + 1 || Equal + 1 >= Text.size() ||
      Text.find(':', Colon + 1) != StringRef::npos ||
      Text.find('=', Equal + 1) != StringRef::npos) {
    Error = "--seed requires location:bytes=value";
    return false;
  }

  Result.Location = Text.take_front(Colon).str();
  uint64_t Bytes = 0;
  if (!parseDecimalOrHex(Text.slice(Colon + 1, Equal), Bytes) || Bytes == 0 ||
      Bytes > 8) {
    Error = "--seed byte width must be an integer from 1 through 8";
    return false;
  }
  Result.Bytes = static_cast<uint32_t>(Bytes);
  if (!parseDecimalOrHex(Text.drop_front(Equal + 1), Result.Value)) {
    Error = "--seed value requires a decimal or 0x-prefixed integer";
    return false;
  }
  if (Result.Bytes < 8 && Result.Value >= (uint64_t{1} << (Result.Bytes * 8))) {
    Error = "--seed value does not fit its byte width";
    return false;
  }
  return true;
}

std::optional<uint64_t> resolveSeedLocation(StringRef Location,
                                            const ImageABI &ABI,
                                            std::string &Error) {
  uint64_t Numeric = 0;
  if (parseDecimalOrHex(Location, Numeric))
    return Numeric;

  std::optional<uint64_t> FirstArgument;
  StringRef RegisterAlias;
  if (ABI.Architecture == "x86_64") {
    if (ABI.Format == "PE") {
      FirstArgument = x86reg::RCX;
      RegisterAlias = "rcx";
    } else if (ABI.Format == "ELF" || ABI.Format == "Mach-O") {
      FirstArgument = x86reg::RDI;
      RegisterAlias = "rdi";
    }
  } else if (ABI.Architecture == "aarch64" &&
             (ABI.Format == "ELF" || ABI.Format == "Mach-O" ||
              ABI.Format == "PE")) {
    FirstArgument = a64reg::X0;
    RegisterAlias = "x0";
  }

  if (!FirstArgument) {
    Error = "register aliases are unavailable for this image ABI";
    return std::nullopt;
  }
  if (Location == "arg0" || Location == RegisterAlias)
    return FirstArgument;

  Error = "'" + Location.str() +
          "' is not the full first-argument register alias for this image "
          "ABI";
  return std::nullopt;
}

bool readImageABI(neverd_session_t Sess, ImageABI &Result) {
  const char *Arch = neverd_session_arch_name(Sess);
  const char *Format = neverd_session_format_name(Sess);
  if (!Arch || !Format) {
    neverd_free_string(Arch);
    neverd_free_string(Format);
    return false;
  }
  Result.Architecture = Arch;
  Result.Format = Format;
  neverd_free_string(Arch);
  neverd_free_string(Format);
  return true;
}

void addNamedAddress(StringRef Candidate, uint64_t Address, StringRef Wanted,
                     const ImageABI &ABI, std::set<uint64_t> &Exact,
                     std::set<uint64_t> &Normalized) {
  if (Candidate == Wanted)
    Exact.insert(Address);
  // Mach-O encodes exactly one ABI decoration underscore.  Removing more (or
  // applying this to ELF/PE) would conflate real `_foo`/`__foo` identifiers.
  if (ABI.Format == "Mach-O" && Candidate.consume_front("_") &&
      Candidate == Wanted)
    Normalized.insert(Address);
}

bool collectNamedAddresses(const char *OwnedJSON, StringRef Wanted,
                           const ImageABI &ABI, std::set<uint64_t> &Exact,
                           std::set<uint64_t> &Normalized, std::string &Error) {
  if (!OwnedJSON) {
    Error = "could not query image symbols";
    return false;
  }
  std::string Text(OwnedJSON);
  neverd_free_string(OwnedJSON);
  Expected<json::Value> Parsed = json::parse(Text);
  if (!Parsed) {
    consumeError(Parsed.takeError());
    Error = "image symbol query returned malformed JSON";
    return false;
  }
  const json::Array *Entries = Parsed->getAsArray();
  if (!Entries) {
    Error = "image symbol query did not return an array";
    return false;
  }
  for (const json::Value &Entry : *Entries) {
    const json::Object *Object = Entry.getAsObject();
    if (!Object)
      continue;
    const std::optional<StringRef> Name = Object->getString("name");
    const std::optional<StringRef> AddressText = Object->getString("addr");
    uint64_t Address = 0;
    if (Name && AddressText && parseDecimalOrHex(*AddressText, Address))
      addNamedAddress(*Name, Address, Wanted, ABI, Exact, Normalized);
  }
  return true;
}

std::optional<uint64_t> resolveFunctionEntry(neverd_session_t Sess,
                                             StringRef Function,
                                             const ImageABI &ABI,
                                             std::string &ErrorCode,
                                             std::string &Error) {
  std::set<uint64_t> Exact;
  std::set<uint64_t> Normalized;
  const int FunctionCount = neverd_func_count(Sess);
  for (int I = 0; I < FunctionCount; ++I) {
    const char *OwnedName = neverd_func_name(Sess, I);
    if (!OwnedName)
      continue;
    const std::string Name(OwnedName);
    neverd_free_string(OwnedName);
    addNamedAddress(Name, neverd_func_entry(Sess, I), Function, ABI, Exact,
                    Normalized);
  }

  // Some native fixtures expose their sole function only through the image
  // export/symbol tables (notably PE), while Mach-O carries the ABI underscore
  // in those tables.  Resolve both views and require one unique address.
  if (!collectNamedAddresses(neverd_exports_json(Sess), Function, ABI, Exact,
                             Normalized, Error) ||
      !collectNamedAddresses(neverd_symbols_json(Sess), Function, ABI, Exact,
                             Normalized, Error)) {
    ErrorCode = "pipeline_error";
    return std::nullopt;
  }

  const std::set<uint64_t> &Matches = Exact.empty() ? Normalized : Exact;
  if (Matches.size() == 1)
    return *Matches.begin();
  if (Matches.size() > 1) {
    ErrorCode = "ambiguous_function";
    Error = "function name '" + Function.str() +
            "' resolves to more than one address";
    return std::nullopt;
  }

  // Prefer a real symbol/export even when its name contains only hexadecimal
  // digits (for example `add` or `face`).  With no name match, preserve
  // neverd's optional-0x hexadecimal address convention and let the versioned
  // API perform the authoritative native-LowIR membership check.
  if (std::optional<uint64_t> Address = parseAddrArg(Function))
    return Address;

  ErrorCode = "function_not_found";
  Error = "function '" + Function.str() + "' was not found";
  return std::nullopt;
}

} // namespace

int runConcolic() {
  PositionalPool Positionals;
  const std::vector<std::string> OutputFiles =
      recoverOptionalValues(ConcolicOutputFiles, Positionals);
  const std::vector<std::string> FunctionArguments =
      recoverOptionalValues(ConcolicFunctions, Positionals);
  const std::vector<std::string> SeedArguments =
      recoverOptionalValues(ConcolicSeeds, Positionals);
  const std::vector<std::string> MaxSteps =
      recoverOptionalValues(ConcolicMaxSteps, Positionals);
  const std::vector<std::string> MaxBlockVisits =
      recoverOptionalValues(ConcolicMaxBlockVisits, Positionals);
  const std::vector<std::string> MaxLoopIterations =
      recoverOptionalValues(ConcolicMaxLoopIterations, Positionals);
  const std::vector<std::string> MaxFlipAttempts =
      recoverOptionalValues(ConcolicMaxFlipAttempts, Positionals);
  const std::vector<std::string> MaxCandidates =
      recoverOptionalValues(ConcolicMaxCandidates, Positionals);
  const std::vector<std::string> SolverConflicts =
      recoverOptionalValues(ConcolicSolverConflicts, Positionals);
  const std::vector<std::string> SolverPropagations =
      recoverOptionalValues(ConcolicSolverPropagations, Positionals);
  const std::vector<std::string> SolverWatchVisits =
      recoverOptionalValues(ConcolicSolverWatchVisits, Positionals);
  const std::vector<std::string> SolverGates =
      recoverOptionalValues(ConcolicSolverGates, Positionals);
  const std::vector<std::string> InputFiles = Positionals.remaining();

  StringRef OutputPath;
  if (OutputFiles.size() > 1)
    return fail("invalid_arguments", "-o may be specified only once");
  if (!OutputFiles.empty()) {
    if (OutputFiles.front().empty())
      return fail("invalid_arguments", "-o requires a non-empty path");
    OutputPath = OutputFiles.front();
  }

  if (InputFiles.size() != 1)
    return fail("invalid_arguments", "concolic requires exactly one binary",
                OutputPath);
  if (FunctionArguments.size() != 1 || FunctionArguments.front().empty())
    return fail("invalid_arguments",
                "concolic requires exactly one --func name or hex address",
                OutputPath);
  if (SeedArguments.size() > NEVERD_LOWIR_CONCOLIC_MAX_REGISTER_SEEDS_V1)
    return fail("invalid_arguments", "too many --seed values", OutputPath);

  neverd_lowir_concolic_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  std::string Error;
  uint64_t Value = 0;
#define READ_UNSIGNED(Values, Flag, Field)                                     \
  do {                                                                         \
    if (!readOne(Values, Flag, std::numeric_limits<unsigned>::max(), Value,    \
                 Error))                                                       \
      return fail("invalid_arguments", Error, OutputPath);                     \
    Options.Field = static_cast<unsigned>(Value);                              \
  } while (false)

  READ_UNSIGNED(MaxSteps, "max-steps", max_steps);
  READ_UNSIGNED(MaxBlockVisits, "max-block-visits", max_block_visits);
  READ_UNSIGNED(MaxLoopIterations, "max-loop-iterations", max_loop_iterations);
  READ_UNSIGNED(MaxFlipAttempts, "max-flip-attempts", max_flip_attempts);
  READ_UNSIGNED(MaxCandidates, "max-candidates", max_candidates);
#undef READ_UNSIGNED

#define READ_U64(Values, Flag, Field)                                          \
  do {                                                                         \
    if (!readOne(Values, Flag, std::numeric_limits<uint64_t>::max(), Value,    \
                 Error))                                                       \
      return fail("invalid_arguments", Error, OutputPath);                     \
    Options.Field = Value;                                                     \
  } while (false)

  READ_U64(SolverConflicts, "solver-conflicts", solver_max_conflicts);
  READ_U64(SolverPropagations, "solver-propagations", solver_max_propagations);
  READ_U64(SolverWatchVisits, "solver-watch-visits", solver_max_watch_visits);
  READ_U64(SolverGates, "solver-gates", solver_max_gates);
#undef READ_U64

  std::vector<SeedSpec> SeedSpecs;
  SeedSpecs.reserve(SeedArguments.size());
  for (const std::string &Text : SeedArguments) {
    SeedSpec Seed;
    if (!parseSeed(Text, Seed, Error))
      return fail("invalid_arguments", Error, OutputPath);
    SeedSpecs.push_back(std::move(Seed));
  }

  const std::filesystem::path Input(InputFiles.front());
  std::error_code FileError;
  if (!std::filesystem::is_regular_file(Input, FileError)) {
    const std::string Message =
        FileError ? "cannot inspect binary '" + Input.string() +
                        "': " + FileError.message()
                  : "binary not found: '" + Input.string() + "'";
    return fail("load_error", Message, OutputPath);
  }

  neverd_session_t Sess = neverd_session_create();
  if (!Sess)
    return fail("internal_error", "could not create a NeverD session",
                OutputPath);
  SessionGuard Guard(Sess);
  if (!neverd_session_load(Sess, Input.string().c_str())) {
    Error = takeLastError(Sess);
    if (Error.empty())
      Error = "failed to load binary";
    return fail("load_error", Error, OutputPath);
  }
  if (!configureAnalysisSession(Sess)) {
    Error = takeLastError(Sess);
    if (Error.empty())
      Error = "failed to configure the analysis pipeline";
    return fail("pipeline_error", Error, OutputPath);
  }

  ImageABI ABI;
  if (!readImageABI(Sess, ABI))
    return fail("internal_error", "could not identify the image ABI",
                OutputPath);

  std::vector<neverd_lowir_concolic_register_seed_v1> Seeds;
  Seeds.reserve(SeedSpecs.size());
  for (const SeedSpec &Spec : SeedSpecs) {
    std::optional<uint64_t> Offset =
        resolveSeedLocation(Spec.Location, ABI, Error);
    if (!Offset)
      return fail("invalid_arguments", Error, OutputPath);
    if (*Offset > std::numeric_limits<uint64_t>::max() - (Spec.Bytes - 1))
      return fail("invalid_arguments", "--seed register range overflows",
                  OutputPath);
    Seeds.push_back({*Offset, Spec.Value, Spec.Bytes, 0});
  }
  std::sort(Seeds.begin(), Seeds.end(),
            [](const auto &Left, const auto &Right) {
              if (Left.offset != Right.offset)
                return Left.offset < Right.offset;
              if (Left.bytes != Right.bytes)
                return Left.bytes < Right.bytes;
              return Left.value < Right.value;
            });
  for (size_t I = 1; I < Seeds.size(); ++I) {
    const uint64_t PreviousEnd = Seeds[I - 1].offset + Seeds[I - 1].bytes - 1;
    if (Seeds[I].offset <= PreviousEnd)
      return fail("invalid_arguments",
                  "--seed register ranges overlap or are duplicated",
                  OutputPath);
  }
  Options.register_seeds = Seeds.empty() ? nullptr : Seeds.data();
  Options.register_seed_count = Seeds.size();

  std::string ErrorCode;
  std::optional<uint64_t> FunctionEntry = resolveFunctionEntry(
      Sess, FunctionArguments.front(), ABI, ErrorCode, Error);
  if (!FunctionEntry)
    return fail(ErrorCode, Error, OutputPath);

  const char *OwnedReport =
      neverd_lowir_concolic_json_v1(Sess, *FunctionEntry, &Options);
  if (!OwnedReport) {
    Error = takeLastError(Sess);
    if (Error.empty())
      Error = "concolic analysis produced no report";
    return fail("internal_error", Error, OutputPath);
  }

  std::string Report(OwnedReport);
  neverd_free_string(OwnedReport);
  Expected<json::Value> Parsed = json::parse(Report);
  if (!Parsed) {
    consumeError(Parsed.takeError());
    return fail("internal_error", "concolic API returned malformed JSON",
                OutputPath);
  }
  const json::Object *Root = Parsed->getAsObject();
  const std::optional<bool> Ok = Root ? Root->getBoolean("ok") : std::nullopt;
  if (!Ok)
    return fail("internal_error", "concolic API report has no boolean ok field",
                OutputPath);
  return emit(Report, OutputPath, *Ok);
}

} // namespace neverd::cli
