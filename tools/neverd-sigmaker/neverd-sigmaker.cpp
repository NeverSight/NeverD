//===- neverd-sigmaker.cpp - Generate .pat signatures from libraries ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Tool to generate .pat signature files from static libraries (.a / .lib)
/// and object files. The line format, the wildcard rules for relocations,
/// and the naming rule live in neverd::sigs::PatternGenerator; this tool
/// walks archives and reports what each input contributed.
///
/// Usage:
///   neverd-sigmaker /path/to/libfoo.a -o foo.pat
///   neverd-sigmaker libgcc_eh.a -o eh.pat --tail 65535
///   neverd-sigmaker libcmt.lib libcpmt.lib -o vs.pat --machine arm64
///   neverd-sigmaker --verify vs.pat winsdk.pat
///
/// The --tail 65535 form states every byte of every function, so a match is
/// agreement over the whole routine rather than over its opening run.  A
/// consumer that acts on the name it gets -- naming an exception personality,
/// say -- asks for that; see SignatureMatcher::isFullyVerified.
///
/// --machine keeps only the objects built for one architecture, so a
/// library directory that also carries another target's objects cannot file
/// them under the wrong signature directory.
///
/// --verify reads .pat files back with the parser the signature loader
/// uses. The loader rejects a whole directory for one bad line, so a
/// generated file is checked this way before it is published.
///
//===----------------------------------------------------------------------===//

#include "neverd/sigs/PatternGenerator.h"
#include "neverd/sigs/PatternParser.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>

using namespace llvm;
using namespace llvm::object;
using namespace neverd::sigs;

static cl::list<std::string> Inputs(cl::Positional,
                                    cl::desc("<library|object|pattern>..."),
                                    cl::OneOrMore);
static cl::opt<std::string> OutputFile("o", cl::desc("Output .pat file"));
static cl::opt<std::string> LibName("name", cl::desc("Library name tag"),
                                    cl::init(""));
static cl::opt<unsigned>
    LeadingLen("leading", cl::desc("Leading pattern bytes"),
               cl::init(PatternGeneratorOptions().LeadingLen));
static cl::opt<unsigned>
    MinFuncSize("min-size", cl::desc("Minimum function size"),
                cl::init(PatternGeneratorOptions().MinFuncSize));
static cl::opt<unsigned>
    TailLen("tail",
            cl::desc("Trailing pattern bytes to emit after the CRC span; "
                     "0 emits none, a value at least as large as the function "
                     "covers it to its end"),
            cl::init(PatternGeneratorOptions().TailLen));
/// The names --machine takes, as its help and its diagnostic list them.
static const std::string MachineNames = join(targetMachineNames(), ", ");
static const std::string MachineHelp =
    "Keep only COFF, ELF and Mach-O objects for this architecture (" +
    MachineNames + "); others are skipped and counted";
static cl::opt<std::string> Machine("machine", cl::desc(MachineHelp),
                                    cl::init(""));
static cl::opt<bool> References(
    "references",
    cl::desc("State direct branch targets and canonical COFF import pointers "
             "as ^offset name references, which matching checks "
             "against the image; loaders older than the references reject "
             "such lines"),
    cl::init(false));
static cl::opt<bool>
    Contextual("contextual",
               cl::desc("Require verified references for referenced functions, "
                        "including short functions; emits whole-function "
                        "NeverD !refs patterns and implies --references"),
               cl::init(false));
static cl::opt<bool> CallerContext(
    "caller-context",
    cl::desc("Emit whole-function patterns with !caller; only a verified "
             "caller may request them by name; implies --references"),
    cl::init(false));
static cl::opt<bool> CollisionEvidence(
    "collision-evidence",
    cl::desc("Emit complete offline collision evidence, including short and "
             "unreferenced functions; !evidence records cannot be loaded as "
             "signatures and imply --references"),
    cl::init(false));
static cl::opt<bool> CodeLayoutEvidence(
    "code-layout-evidence",
    cl::desc("Emit offline complete non-COMDAT COFF x64 code contributions"));
static cl::opt<bool> VTableEvidence(
    "vtable-evidence",
    cl::desc(
        "Emit offline JSONL primary x64 COFF table declarations, including "
        "incomplete counterclaims; this is not a signature pack"),
    cl::init(false));
static cl::opt<bool>
    RuntimePointerEvidence("runtime-pointer-evidence",
                           cl::desc("Emit offline JSONL load-configuration and "
                                    "selected source definitions"),
                           cl::init(false));
static cl::list<std::string>
    EvidenceSymbols("evidence-symbol",
                    cl::desc("Exact pointer or function definition to audit"));
static cl::opt<bool> DataReferences(
    "data-references",
    cl::desc("Include typed external-data operands in contextual or collision "
             "evidence output; requires a complete contextual output mode"),
    cl::init(false));
static cl::list<std::string> LocalDataAnchors(
    "local-data-anchor",
    cl::desc("Typed function anchoring same-object static data operands; "
             "repeat as needed; requires --data-references"));
static cl::opt<bool> LocalDataLayout(
    "local-data-layout",
    cl::desc("Include checked address deltas within one non-COMDAT data "
             "section; requires --local-data-anchor"),
    cl::init(false));
static cl::opt<bool>
    Verify("verify",
           cl::desc("Parse the positional .pat files with the signature "
                    "loader's parser instead of generating"),
           cl::init(false));

namespace {

struct InputStats {
  unsigned Objects = 0;
  unsigned OtherMachine = 0;
  unsigned NotObjects = 0;
  unsigned Unreadable = 0;
  PatternGeneratorStats Patterns;
};

/// Calls \p Visit with each object \p Path holds: the file itself, or every
/// member of an archive.  Members that are no object, or that LLVM cannot
/// read, are counted in \p Stats.
bool forEachObject(StringRef Path, InputStats &Stats,
                   function_ref<void(const ObjectFile &)> Visit) {
  auto BufOrErr = MemoryBuffer::getFile(Path);
  if (!BufOrErr) {
    WithColor::error() << "cannot open: " << Path << "\n";
    return false;
  }
  MemoryBufferRef MemRef = (*BufOrErr)->getMemBufferRef();

  if (identify_magic(MemRef.getBuffer()) != file_magic::archive) {
    auto ObjOrErr = ObjectFile::createObjectFile(MemRef);
    if (!ObjOrErr) {
      WithColor::error() << "not a valid object/archive: " << Path << ": "
                         << toString(ObjOrErr.takeError()) << "\n";
      return false;
    }
    Visit(**ObjOrErr);
    return true;
  }

  auto ArchOrErr = Archive::create(MemRef);
  if (!ArchOrErr) {
    WithColor::error() << "invalid archive: " << Path << ": "
                       << toString(ArchOrErr.takeError()) << "\n";
    return false;
  }

  Error Err = Error::success();
  for (auto &Child : (*ArchOrErr)->children(Err)) {
    auto BinOrErr = Child.getAsBinary();
    if (!BinOrErr) {
      // LLVM cannot read every member kind; an MSVC /GL object carries
      // compiler IR rather than machine code and is one of them.
      consumeError(BinOrErr.takeError());
      ++Stats.Unreadable;
      continue;
    }
    if (auto *Obj = dyn_cast<ObjectFile>(BinOrErr->get()))
      Visit(*Obj);
    else
      ++Stats.NotObjects; // import-library members and the like: no code
  }
  if (Err) {
    WithColor::error() << "invalid archive: " << Path << ": "
                       << toString(std::move(Err)) << "\n";
    return false;
  }
  return true;
}

int verifyPatterns() {
  int Status = 0;
  for (const std::string &Path : Inputs) {
    auto ModulesOrErr = PatternParser::parseFile(Path);
    if (!ModulesOrErr) {
      WithColor::error() << Path << ": " << toString(ModulesOrErr.takeError())
                         << "\n";
      Status = 1;
      continue;
    }
    outs() << Path << ": " << ModulesOrErr->size() << " modules\n";
  }
  return Status;
}

} // anonymous namespace

int main(int Argc, char *Argv[]) {
  InitLLVM X(Argc, Argv);
  cl::ParseCommandLineOptions(Argc, Argv,
                              "NeverD Signature Maker\n\n"
                              "  Generate .pat files from static libraries.\n");

  if (unsigned(Contextual) + unsigned(CallerContext) +
          unsigned(CollisionEvidence) + unsigned(VTableEvidence) +
          unsigned(RuntimePointerEvidence) + unsigned(CodeLayoutEvidence) >
      1) {
    WithColor::error()
        << "--contextual, --caller-context, --collision-evidence and "
           "--vtable-evidence, --runtime-pointer-evidence and "
           "--code-layout-evidence "
           "are separate outputs\n";
    return 1;
  }
  if ((VTableEvidence || RuntimePointerEvidence || CodeLayoutEvidence) &&
      (References || DataReferences || Verify)) {
    WithColor::error()
        << "metadata evidence is a separate offline output mode\n";
    return 1;
  }
  if (!EvidenceSymbols.empty() && !RuntimePointerEvidence) {
    WithColor::error()
        << "--evidence-symbol requires --runtime-pointer-evidence\n";
    return 1;
  }
  if (Verify)
    return verifyPatterns();
  if (!LocalDataAnchors.empty() && !DataReferences) {
    WithColor::error() << "--local-data-anchor requires --data-references\n";
    return 1;
  }
  if (LocalDataLayout && LocalDataAnchors.empty()) {
    WithColor::error() << "--local-data-layout requires --local-data-anchor\n";
    return 1;
  }
  if (DataReferences && !Contextual && !CallerContext && !CollisionEvidence) {
    WithColor::error() << "--data-references requires --contextual, "
                          "--caller-context or --collision-evidence\n";
    return 1;
  }

  if (OutputFile.empty()) {
    WithColor::error() << "no output file; pass -o <file.pat>\n";
    return 1;
  }

  std::optional<TargetMachine> Required;
  if (!Machine.empty()) {
    Required = parseTargetMachine(Machine);
    if (!Required) {
      WithColor::error() << "unknown --machine '" << Machine
                         << "'; expected one of " << MachineNames << "\n";
      return 1;
    }
  }

  std::error_code EC;
  raw_fd_ostream OS(OutputFile, EC);
  if (EC) {
    WithColor::error() << "cannot open output: " << EC.message() << "\n";
    return 1;
  }

  PatternGeneratorOptions Opts;
  Opts.LeadingLen = LeadingLen;
  Opts.MinFuncSize = MinFuncSize;
  Opts.TailLen = TailLen;
  Opts.EmitReferences =
      References || Contextual || CallerContext || CollisionEvidence;
  Opts.RequireReferences = Contextual;
  Opts.RequireCaller = CallerContext;
  Opts.CollisionEvidence = CollisionEvidence;
  Opts.EmitDataReferences = DataReferences;
  Opts.LocalDataAnchors.assign(LocalDataAnchors.begin(),
                               LocalDataAnchors.end());
  Opts.EmitLocalDataLayout = LocalDataLayout;

  auto ForMachine = [&](const ObjectFile &Obj) {
    return !Required || isObjectForMachine(Obj, *Required);
  };
  if (Opts.EmitReferences) {
    // A COFF link resolves a symbol no object defines to the alternate name
    // an `/alternatename` directive of any object it includes gives it, so
    // the directives of every input count before any reference is stated.
    InputStats Uncounted;
    for (const std::string &Path : Inputs)
      if (!forEachObject(Path, Uncounted, [&](const ObjectFile &Obj) {
            if (ForMachine(Obj))
              collectAlternateNames(Obj, Opts.AlternateNames);
          }))
        return 1;
  }

  InputStats Stats;
  VTableEvidenceStats Tables;
  RuntimePointerEvidenceStats Pointers;
  CodeLayoutEvidenceStats Layouts;
  bool EvidenceFailed = false;
  for (const std::string &Path : Inputs)
    if (!forEachObject(Path, Stats, [&](const ObjectFile &Obj) {
          if (!ForMachine(Obj)) {
            ++Stats.OtherMachine;
            return;
          }
          ++Stats.Objects;
          if (CodeLayoutEvidence) {
            auto Result = generateCodeLayoutEvidence(Obj, OS);
            if (!Result) {
              WithColor::error() << toString(Result.takeError()) << "\n";
              EvidenceFailed = true;
            } else {
              Layouts.Complete += Result->Complete;
              Layouts.Incomplete += Result->Incomplete;
            }
          } else if (RuntimePointerEvidence) {
            auto Result =
                generateRuntimePointerEvidence(Obj, EvidenceSymbols, OS);
            if (!Result) {
              WithColor::error() << toString(Result.takeError()) << "\n";
              EvidenceFailed = true;
            } else {
              Pointers.Complete += Result->Complete;
              Pointers.Incomplete += Result->Incomplete;
            }
          } else if (VTableEvidence) {
            auto Result = generateVTableEvidence(Obj, OS);
            if (!Result) {
              WithColor::error() << toString(Result.takeError()) << "\n";
              EvidenceFailed = true;
            } else {
              Tables.Complete += Result->Complete;
              Tables.Incomplete += Result->Incomplete;
            }
          } else {
            Stats.Patterns += generatePatterns(Obj, Opts, OS);
          }
        }))
      return 1;

  if (EvidenceFailed)
    return 1;

  for (const auto &[Mach, Type] : Stats.Patterns.UnsupportedCOFFRelocations)
    WithColor::warning() << "functions with COFF relocation type "
                         << format_hex(Type, 6) << " (machine "
                         << format_hex(Mach, 6)
                         << ") were left out: its width is unknown\n";
  for (const auto &[Mach, Type] : Stats.Patterns.UnsupportedELFRelocations)
    WithColor::warning() << "functions with ELF relocation type " << Type
                         << " (machine " << Mach
                         << ") were left out: the bytes it may change are "
                            "unknown\n";
  for (const auto &[CPU, Type] : Stats.Patterns.UnsupportedMachORelocations)
    WithColor::warning() << "functions with Mach-O relocation type " << Type
                         << " (CPU type " << format_hex(CPU, 10)
                         << ") were left out: the bytes it may change are "
                            "unknown\n";
  for (const auto &[CPU, Kind] : Stats.Patterns.UnsupportedMachOHints)
    WithColor::warning() << "functions with Mach-O linker optimization hint "
                            "kind "
                         << Kind << " (CPU type " << format_hex(CPU, 10)
                         << ") were left out: the bytes it lets the linker "
                            "change are unknown\n";
  if (Stats.Patterns.UnreadableHints)
    WithColor::warning() << "the functions of "
                         << Stats.Patterns.UnreadableHints
                         << " Mach-O objects were left out: their linker "
                            "optimization hints cannot be read\n";

  if (CodeLayoutEvidence)
    outs() << "Generated " << Layouts.Complete << " complete and "
           << Layouts.Incomplete << " incomplete code layouts → ";
  else if (RuntimePointerEvidence)
    outs() << "Generated " << Pointers.Complete << " complete and "
           << Pointers.Incomplete << " incomplete runtime declarations → ";
  else if (VTableEvidence)
    outs() << "Generated " << Tables.Complete << " complete and "
           << Tables.Incomplete << " incomplete table declarations → ";
  else
    outs() << "Generated " << Stats.Patterns.Functions << " signatures → ";
  outs() << OutputFile << " (" << Stats.Objects << " objects";
  if (Stats.OtherMachine)
    outs() << ", " << Stats.OtherMachine << " for another machine skipped";
  if (Stats.NotObjects)
    outs() << ", " << Stats.NotObjects << " non-object members";
  if (Stats.Unreadable)
    outs() << ", " << Stats.Unreadable << " unreadable members";
  if (Stats.Patterns.TooSmall)
    outs() << ", " << Stats.Patterns.TooSmall << " functions below --min-size";
  if (Stats.Patterns.TooWeak)
    outs() << ", " << Stats.Patterns.TooWeak
           << " functions stating too few exact bytes";
  if (Stats.Patterns.UnsupportedRelocation)
    outs() << ", " << Stats.Patterns.UnsupportedRelocation
           << " functions with unsupported relocations";
  if (Stats.Patterns.UnsupportedHint)
    outs() << ", " << Stats.Patterns.UnsupportedHint
           << " functions with unsupported linker optimization hints";
  if (Stats.Patterns.Synthesized)
    outs() << ", " << Stats.Patterns.Synthesized
           << " compiler-synthesized routines left unnamed";
  outs() << ")\n";
  return 0;
}
