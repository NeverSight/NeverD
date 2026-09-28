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
/// --machine keeps only COFF objects built for one architecture, so a
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

#include "llvm/ADT/StringSwitch.h"
#include "llvm/BinaryFormat/COFF.h"
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
    LeadingLen("leading", cl::desc("Leading pattern bytes"), cl::init(32));
static cl::opt<unsigned>
    MinFuncSize("min-size", cl::desc("Minimum function size"), cl::init(4));
static cl::opt<unsigned>
    TailLen("tail",
            cl::desc("Trailing pattern bytes to emit after the CRC span; "
                     "0 emits none, a value at least as large as the function "
                     "covers it to its end"),
            cl::init(0));
static cl::opt<std::string>
    Machine("machine",
            cl::desc("Keep only COFF objects for this architecture "
                     "(x86, x64, arm, arm64); others are skipped and counted"),
            cl::init(""));
static cl::opt<bool> References(
    "references",
    cl::desc("State the routines each COFF function branches to directly as "
             "^offset name references, which matching checks against the "
             "image; loaders older than the references reject such lines"),
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

std::optional<uint16_t> parseMachine(StringRef Name) {
  return StringSwitch<std::optional<uint16_t>>(Name)
      .Case("x86", COFF::IMAGE_FILE_MACHINE_I386)
      .Case("x64", COFF::IMAGE_FILE_MACHINE_AMD64)
      .Case("arm", COFF::IMAGE_FILE_MACHINE_ARMNT)
      .Case("arm64", COFF::IMAGE_FILE_MACHINE_ARM64)
      .Default(std::nullopt);
}

void processObject(const ObjectFile &Obj, std::optional<uint16_t> Required,
                   const PatternGeneratorOptions &Opts, raw_ostream &OS,
                   InputStats &Stats) {
  if (Required) {
    const auto *COFF = dyn_cast<COFFObjectFile>(&Obj);
    if (!COFF || COFF->getMachine() != *Required) {
      ++Stats.OtherMachine;
      return;
    }
  }
  ++Stats.Objects;
  Stats.Patterns += generatePatterns(Obj, Opts, OS);
}

bool processInput(StringRef Path, std::optional<uint16_t> Required,
                  const PatternGeneratorOptions &Opts, raw_ostream &OS,
                  InputStats &Stats) {
  auto BufOrErr = MemoryBuffer::getFile(Path);
  if (!BufOrErr) {
    WithColor::error() << "cannot open: " << Path << "\n";
    return false;
  }
  MemoryBufferRef MemRef = (*BufOrErr)->getMemBufferRef();

  StringRef Magic = MemRef.getBuffer().substr(0, 8);
  if (!Magic.starts_with("!<arch>") && !Magic.starts_with("!<thin>")) {
    auto ObjOrErr = ObjectFile::createObjectFile(MemRef);
    if (!ObjOrErr) {
      WithColor::error() << "not a valid object/archive: " << Path << ": "
                         << toString(ObjOrErr.takeError()) << "\n";
      return false;
    }
    processObject(**ObjOrErr, Required, Opts, OS, Stats);
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
      processObject(*Obj, Required, Opts, OS, Stats);
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

  if (Verify)
    return verifyPatterns();

  if (OutputFile.empty()) {
    WithColor::error() << "no output file; pass -o <file.pat>\n";
    return 1;
  }

  std::optional<uint16_t> Required;
  if (!Machine.empty()) {
    Required = parseMachine(Machine);
    if (!Required) {
      WithColor::error() << "unknown --machine '" << Machine
                         << "'; expected x86, x64, arm, or arm64\n";
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
  Opts.EmitReferences = References;

  InputStats Stats;
  for (const std::string &Path : Inputs)
    if (!processInput(Path, Required, Opts, OS, Stats))
      return 1;

  for (const auto &[Mach, Type] : Stats.Patterns.UnsupportedCOFFRelocations)
    WithColor::warning() << "functions with COFF relocation type "
                         << format_hex(Type, 6) << " (machine "
                         << format_hex(Mach, 6)
                         << ") were left out: its width is unknown\n";

  outs() << "Generated " << Stats.Patterns.Functions << " signatures → "
         << OutputFile << " (" << Stats.Objects << " objects";
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
  outs() << ")\n";
  return 0;
}
