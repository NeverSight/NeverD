//===- NeverDCLIOptions.cpp - neverd command-line option table -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The single definition point for every neverd subcommand and option.  The
/// handlers in NeverDCmd*.cpp only reference these through NeverDCLI.h, so
/// there is one authoritative place to see (and edit) the tool's CLI surface.
/// SubCommands are defined before the options that reference them via
/// cl::sub(), matching cl's construction-order requirement within a TU.
///
//===----------------------------------------------------------------------===//

#include "NeverDCLI.h"

using namespace llvm;

namespace neverd::cli {

std::optional<uint64_t> parseAddrArg(StringRef Ref) {
  if (Ref.empty() || Ref.front() == '-')
    return std::nullopt;
  if (Ref.consume_front("0x") || Ref.consume_front("0X")) {
    if (Ref.empty())
      return std::nullopt;
  }
  uint64_t Addr = 0;
  if (Ref.getAsInteger(16, Addr))
    return std::nullopt;
  return Addr;
}

//===----------------------------------------------------------------------===//
// Subcommands
//===----------------------------------------------------------------------===//

cl::SubCommand LiftCmd("lift", "Lift binary to LLVM IR");
cl::SubCommand DecompileCmd("decompile",
                            "Decompile binary to C, Rust, or Solidity");
cl::SubCommand PatchCmd("patch", "Patch binary with modified IR");
cl::SubCommand InfoCmd("info", "Show binary metadata summary");
cl::SubCommand StringsCmd("strings", "Scan binary for strings");
cl::SubCommand XrefsCmd("xrefs", "Show cross-references for address");
cl::SubCommand FuncsCmd("funcs", "List discovered functions");
cl::SubCommand DisasmCmd("disasm", "Disassemble a function");
cl::SubCommand CfgCmd("cfg", "Show control flow graph");
cl::SubCommand HexCmd("hex", "Hex dump bytes at address");
cl::SubCommand ImportsCmd("imports", "List imported symbols");
cl::SubCommand ExportsCmd("exports", "List exported symbols");
cl::SubCommand SegmentsCmd("segments", "List binary segments");
cl::SubCommand PluginsCmd("plugins", "List or run loaded plugins");
cl::SubCommand ExportCmd("export", "Export analysis results to file");
cl::SubCommand BookmarksCmd("bookmarks", "Manage address bookmarks");
cl::SubCommand AnnotateCmd("annotate", "Manage per-address annotations");
cl::SubCommand DiffCmd("diff", "Compare two binaries");
cl::SubCommand CallGraphCmd("callgraph", "Show function call graph");
cl::SubCommand RenameCmd("rename", "Rename a function symbol");
cl::SubCommand SearchCmd("search", "Search bytes/strings in binary");
cl::SubCommand SectionsCmd("sections", "List binary sections");
cl::SubCommand SymbolsCmd("symbols", "List all symbols");
cl::SubCommand RelocsCmd("relocs", "List relocations");
cl::SubCommand HeadersCmd("headers", "Show comprehensive binary headers");
cl::SubCommand EntryPointsCmd("entrypoints", "List binary entry points");
cl::SubCommand DashboardCmd("dashboard", "Show binary overview dashboard");
cl::SubCommand SigsCmd("sigs", "Apply FLIRT signatures to binary");
// Takes text rather than a binary, so it is not among the subcommands that
// register the positional input file below.
cl::SubCommand SimplifyCmd("simplify", "Simplify a bitvector expression");

//===----------------------------------------------------------------------===//
// Common options (registered with all subcommands)
//===----------------------------------------------------------------------===//

cl::opt<std::string>
    InputFile(cl::Positional, cl::desc("<binary>"), cl::Required,
              cl::sub(LiftCmd), cl::sub(DecompileCmd), cl::sub(PatchCmd),
              cl::sub(InfoCmd), cl::sub(StringsCmd), cl::sub(XrefsCmd),
              cl::sub(FuncsCmd), cl::sub(DisasmCmd), cl::sub(CfgCmd),
              cl::sub(HexCmd), cl::sub(ImportsCmd), cl::sub(ExportsCmd),
              cl::sub(SegmentsCmd), cl::sub(ExportCmd), cl::sub(BookmarksCmd),
              cl::sub(AnnotateCmd), cl::sub(CallGraphCmd), cl::sub(RenameCmd),
              cl::sub(SearchCmd), cl::sub(SectionsCmd), cl::sub(SymbolsCmd),
              cl::sub(RelocsCmd), cl::sub(HeadersCmd), cl::sub(EntryPointsCmd),
              cl::sub(DashboardCmd), cl::sub(SigsCmd));

cl::opt<std::string> OutputFile("o", cl::desc("Output file"), cl::init(""),
                                cl::sub(LiftCmd), cl::sub(DecompileCmd),
                                cl::sub(PatchCmd));

cl::opt<bool>
    Verbose("v", cl::desc("Verbose output"), cl::sub(LiftCmd),
            cl::sub(DecompileCmd), cl::sub(PatchCmd), cl::sub(InfoCmd),
            cl::sub(StringsCmd), cl::sub(FuncsCmd), cl::sub(DisasmCmd),
            cl::sub(CfgCmd), cl::sub(HexCmd), cl::sub(ImportsCmd),
            cl::sub(ExportsCmd), cl::sub(SegmentsCmd), cl::sub(ExportCmd),
            cl::sub(BookmarksCmd), cl::sub(AnnotateCmd), cl::sub(CallGraphCmd),
            cl::sub(RenameCmd), cl::sub(SearchCmd), cl::sub(SectionsCmd),
            cl::sub(SymbolsCmd), cl::sub(RelocsCmd), cl::sub(HeadersCmd),
            cl::sub(EntryPointsCmd), cl::sub(DashboardCmd), cl::sub(SigsCmd));

cl::opt<bool> InjectHello("hello",
                          cl::desc("Inject hello_world() test function"),
                          cl::sub(LiftCmd), cl::sub(DecompileCmd),
                          cl::sub(PatchCmd));

cl::opt<bool> RunNop("nop", cl::desc("Run NOP MIR pass (test pass)"),
                     cl::sub(PatchCmd));

cl::opt<bool>
    InstSubst("subst",
              cl::desc("Replace integer arithmetic with equivalent instruction "
                       "sequences (instruction substitution pass)"),
              cl::sub(PatchCmd));

cl::opt<unsigned> InstSubstRounds("subst-rounds",
                                  cl::desc("Instruction substitution rounds"),
                                  cl::init(1), cl::sub(PatchCmd));

cl::opt<bool>
    ConstEnc("const-enc",
             cl::desc("Encrypt integer constants, decrypting them at run time "
                      "(constant encryption pass)"),
             cl::sub(PatchCmd));

cl::opt<bool>
    OpaquePred("opaque",
               cl::desc("Guard basic blocks behind always-true predicates "
                        "(opaque predicate pass)"),
               cl::sub(PatchCmd));

cl::opt<bool> Flatten("flatten",
                      cl::desc("Flatten control flow into a dispatcher loop "
                               "(control-flow flattening pass)"),
                      cl::sub(PatchCmd));

cl::opt<bool> BogusCF(
    "bogus",
    cl::desc("Add dead, opaque-guarded fake control-flow around each block "
             "(bogus control flow pass)"),
    cl::sub(PatchCmd));

cl::opt<bool> IndirectBr(
    "indirect",
    cl::desc("Rewrite conditional branches into position-independent indirect "
             "branches (indirect branch pass)"),
    cl::sub(PatchCmd));

cl::opt<bool> IndirectCall(
    "ind-call",
    cl::desc("Rewrite direct calls to defined functions into position-"
             "independent indirect calls (indirect call pass)"),
    cl::sub(PatchCmd));

cl::opt<bool> Mba(
    "mba",
    cl::desc("Inject provably-zero mixed-boolean-arithmetic terms into integer "
             "operator results (MBA pass)"),
    cl::sub(PatchCmd));

cl::opt<bool> IndirectGv(
    "ind-gv",
    cl::desc("Rewrite direct references to defined globals into position-"
             "independent indirect addresses (indirect global pass)"),
    cl::sub(PatchCmd));

cl::opt<bool> ValueLaunder(
    "launder",
    cl::desc(
        "Route integer (scalar / integer-vector) values through a volatile "
        "stack slot (value-laundering pass)"),
    cl::sub(PatchCmd));

cl::opt<bool> ConstPool(
    "const-pool",
    cl::desc(
        "Move integer constants into a read-only global pool, fetching "
        "them at run time through an opaque index (constant-pooling pass)"),
    cl::sub(PatchCmd));

cl::opt<bool> BitMask(
    "bit-mask",
    cl::desc(
        "Replace integer (scalar / integer-vector) values with the bitwise "
        "identity (x & m) | (x & ~m) using opaque masks (bit-masking pass)"),
    cl::sub(PatchCmd));

cl::opt<bool> NoDebug("no-debug", cl::desc("Skip debug info loading"),
                      cl::sub(LiftCmd), cl::sub(DecompileCmd),
                      cl::sub(PatchCmd), cl::sub(CfgCmd));

cl::opt<bool> NoOpt("no-opt", cl::desc("Skip LLVM optimization passes"),
                    cl::sub(LiftCmd), cl::sub(DecompileCmd), cl::sub(PatchCmd),
                    cl::sub(CfgCmd));

cl::opt<size_t> MaxFunc("max-func", cl::desc("Limit to first N functions"),
                        cl::init(0), cl::sub(LiftCmd), cl::sub(DecompileCmd),
                        cl::sub(PatchCmd));

//===----------------------------------------------------------------------===//
// Lift-specific options
//===----------------------------------------------------------------------===//

cl::opt<bool> DumpLow("dump-low", cl::desc("Dump LowIR"), cl::sub(LiftCmd));
cl::opt<bool> DumpMed("dump-med", cl::desc("Dump MedIR"), cl::sub(LiftCmd));
cl::opt<bool> DumpHigh("dump-high", cl::desc("Dump HighIR"), cl::sub(LiftCmd));

//===----------------------------------------------------------------------===//
// Decompile-specific options
//===----------------------------------------------------------------------===//

cl::opt<bool>
    LlvmRoute("llvm",
              cl::desc("Route through LLVM IR + opt passes (goto-style C)"),
              cl::sub(DecompileCmd));

cl::opt<neverd_output_language_t>
    OutputLanguage("language", cl::desc("Output source language"),
                   cl::ValuesClass({
#define NEVERD_OUTPUT_LANGUAGE(NAME, VALUE, SPELLING, DISPLAY_NAME)            \
  clEnumValN(NEVERD_OUTPUT_##NAME, SPELLING, DISPLAY_NAME " source"),
#include "neverd/OutputLanguages.def"
                   }),
                   cl::init(NEVERD_OUTPUT_C), cl::sub(DecompileCmd));

cl::opt<evm::Hardfork> EVMHardfork("evm-hardfork", cl::desc("EVM hardfork"),
                                   cl::ValuesClass({
#define EVM_HARDFORK(NAME, SPELLING)                                           \
  clEnumValN(evm::Hardfork::NAME, SPELLING, SPELLING),
#define EVM_HARDFORK_ALIAS(SPELLING, NAME)                                     \
  clEnumValN(evm::Hardfork::NAME, SPELLING, SPELLING),
#define EVM_HARDFORK_LATEST(NAME, SPELLING)                                    \
  clEnumValN(evm::Hardfork::NAME, SPELLING, SPELLING),
#include "neverd/evm/EVMHardforks.def"
                                   }),
                                   cl::init(evm::Hardfork::Latest),
                                   cl::sub(LiftCmd), cl::sub(DecompileCmd),
                                   cl::sub(DisasmCmd), cl::sub(CfgCmd));

cl::opt<bool> EVMRelaxed(
    "evm-relaxed",
    cl::desc("Keep unknown/inactive EVM opcodes as explicit fault nodes"),
    cl::sub(LiftCmd), cl::sub(DecompileCmd), cl::sub(DisasmCmd),
    cl::sub(CfgCmd));

cl::opt<sbf::Version> SBFVersion(
    "sbf-version", cl::desc("Solana SBF version"), cl::ValuesClass({
#define SBF_VERSION_AUTO(NAME, SPELLING, DISPLAY_NAME)                         \
  clEnumValN(sbf::Version::NAME, SPELLING, DISPLAY_NAME),
#define SBF_VERSION(NAME, ELF_FLAGS, SPELLING, DISPLAY_NAME, FEATURES, STATUS) \
  clEnumValN(sbf::Version::NAME, SPELLING, DISPLAY_NAME),
#include "neverd/sbf/SBFVersions.def"
    }),
    cl::init(sbf::Version::Auto), cl::sub(LiftCmd), cl::sub(DecompileCmd));

cl::opt<bool> SBFRelaxed(
    "sbf-relaxed",
    cl::desc("Keep invalid or version-inactive SBF instructions as fault nodes"),
    cl::sub(LiftCmd), cl::sub(DecompileCmd));

cl::opt<std::string> SBFIdl(
    "sbf-idl",
    cl::desc("Anchor IDL JSON file naming this program's instructions"),
    cl::value_desc("path"), cl::sub(LiftCmd), cl::sub(DecompileCmd));

// Which chain, when, under which loader, and for what. None of these are in
// the program file, and each of them changes the answer: a gate that is on for
// one cluster is off for another, a loader decides the shape of the input
// buffer, and a syscall the runtime keeps honouring can be one no new program
// may use.
cl::opt<sbf::Cluster> SBFCluster("sbf-cluster",
                                 cl::desc("Solana cluster to describe against"),
                                 cl::ValuesClass({
#define SBF_CLUSTER(ID, NAME, ACTIVATES_EVERYTHING, SUMMARY)                   \
  clEnumValN(sbf::Cluster::ID, NAME, SUMMARY),
#include "neverd/sbf/SBFRuntimeFeatures.def"
                                 }),
                                 cl::init(sbf::Cluster::MainnetBeta),
                                 cl::sub(LiftCmd), cl::sub(DecompileCmd));

cl::opt<uint64_t> SBFSlot(
    "sbf-slot",
    cl::desc("Slot to describe against; gates activated after it count as off"),
    cl::value_desc("slot"), cl::init(sbf::kCurrentSlot), cl::sub(LiftCmd),
    cl::sub(DecompileCmd));

cl::opt<sbf::Loader> SBFLoader("sbf-loader",
                               cl::desc("Loader that owns the program"),
                               cl::ValuesClass({
#define SBF_LOADER(ID, NAME, KNOWN_ADDRESS, ACCOUNT_ABI, DEPLOYS, EXECUTES,    \
                   SUMMARY)                                                    \
  clEnumValN(sbf::Loader::ID, NAME, SUMMARY),
#include "neverd/sbf/SBFLoaders.def"
                               }),
                               cl::init(sbf::Loader::V3), cl::sub(LiftCmd),
                               cl::sub(DecompileCmd));

cl::opt<sbf::RuntimePurpose> SBFPurpose(
    "sbf-purpose", cl::desc("Whether to answer for running or for deploying"),
    cl::ValuesClass({
#define SBF_RUNTIME_PURPOSE(ID, NAME, SUMMARY)                                 \
  clEnumValN(sbf::RuntimePurpose::ID, NAME, SUMMARY),
#include "neverd/sbf/SBFRuntimeFeatures.def"
    }),
    cl::init(sbf::RuntimePurpose::Execution), cl::sub(LiftCmd),
    cl::sub(DecompileCmd));

//===----------------------------------------------------------------------===//
// Strings-specific options
//===----------------------------------------------------------------------===//

cl::opt<unsigned> MinStrLen("min-len", cl::desc("Minimum string length"),
                            cl::init(4), cl::sub(StringsCmd));

//===----------------------------------------------------------------------===//
// Xrefs-specific options
//===----------------------------------------------------------------------===//

cl::opt<std::string> XrefAddr("addr", cl::desc("Target address (hex)"),
                              cl::Required, cl::sub(XrefsCmd));

//===----------------------------------------------------------------------===//
// Funcs/Disasm/Cfg/Hex options
//===----------------------------------------------------------------------===//

cl::opt<std::string> DisasmFunc("func",
                                cl::desc("Function name or address (hex)"),
                                cl::sub(DisasmCmd), cl::sub(CfgCmd));

cl::opt<bool>
    DisasmAnnotate("annotate",
                   cl::desc("Show resolved address references as comments"),
                   cl::sub(DisasmCmd));

cl::opt<std::string> HexAddr("addr", cl::desc("Start address (hex)"),
                             cl::init(""), cl::sub(HexCmd));

cl::opt<unsigned> HexSize("size", cl::desc("Number of bytes"), cl::init(256),
                          cl::sub(HexCmd));

cl::opt<bool> CfgDot("dot", cl::desc("Output in DOT format"), cl::sub(CfgCmd));

cl::opt<std::string> CfgSvg("svg", cl::desc("Output CFG as SVG file"),
                            cl::init(""), cl::sub(CfgCmd));

//===----------------------------------------------------------------------===//
// JSON output option (shared)
//===----------------------------------------------------------------------===//

cl::opt<bool>
    JsonOutput("json", cl::desc("Output as JSON"), cl::sub(InfoCmd),
               cl::sub(FuncsCmd), cl::sub(DisasmCmd), cl::sub(CfgCmd),
               cl::sub(HexCmd), cl::sub(StringsCmd), cl::sub(XrefsCmd),
               cl::sub(ImportsCmd), cl::sub(ExportsCmd), cl::sub(SegmentsCmd),
               cl::sub(PluginsCmd), cl::sub(BookmarksCmd), cl::sub(AnnotateCmd),
               cl::sub(CallGraphCmd), cl::sub(RenameCmd), cl::sub(SearchCmd),
               cl::sub(SectionsCmd), cl::sub(SymbolsCmd), cl::sub(RelocsCmd),
               cl::sub(HeadersCmd), cl::sub(EntryPointsCmd),
               cl::sub(DashboardCmd), cl::sub(SigsCmd));

//===----------------------------------------------------------------------===//
// Plugins-specific options
//===----------------------------------------------------------------------===//

cl::opt<bool> PluginList("list", cl::desc("List loaded plugins"),
                         cl::sub(PluginsCmd));

cl::opt<std::string> PluginRun("run", cl::desc("Run a plugin by name"),
                               cl::init(""), cl::sub(PluginsCmd));

cl::opt<std::string> PluginBinary("binary",
                                  cl::desc("Binary file to load (for --run)"),
                                  cl::init(""), cl::sub(PluginsCmd));

cl::opt<std::string> PluginDir("plugin-dir",
                               cl::desc("Additional plugin directory to scan"),
                               cl::init(""), cl::sub(PluginsCmd));

//===----------------------------------------------------------------------===//
// Bookmarks-specific options
//===----------------------------------------------------------------------===//

cl::opt<bool> BookmarkList("list", cl::desc("List all bookmarks"),
                           cl::sub(BookmarksCmd));

cl::opt<std::string> BookmarkAdd("add",
                                 cl::desc("Add bookmark at address (hex)"),
                                 cl::init(""), cl::sub(BookmarksCmd));

cl::opt<std::string> BookmarkName("name", cl::desc("Bookmark name (for --add)"),
                                  cl::init(""), cl::sub(BookmarksCmd));

cl::opt<std::string>
    BookmarkRemove("remove", cl::desc("Remove bookmark at address (hex)"),
                   cl::init(""), cl::sub(BookmarksCmd));

//===----------------------------------------------------------------------===//
// Diff-specific options
//===----------------------------------------------------------------------===//

cl::opt<std::string> DiffFileA("a", cl::desc("First binary"), cl::Required,
                               cl::sub(DiffCmd));

cl::opt<std::string> DiffFileB("b", cl::desc("Second binary"), cl::Required,
                               cl::sub(DiffCmd));

cl::opt<std::string> DiffFunc("func",
                              cl::desc("Compare specific function (by name)"),
                              cl::init(""), cl::sub(DiffCmd));

cl::opt<bool> DiffJson("json", cl::desc("Output as JSON"), cl::sub(DiffCmd));

//===----------------------------------------------------------------------===//
// Annotate-specific options
//===----------------------------------------------------------------------===//

cl::opt<bool> AnnotateList("list", cl::desc("List all annotations"),
                           cl::sub(AnnotateCmd));

cl::opt<std::string> AnnotateAdd("add",
                                 cl::desc("Add annotation at address (hex)"),
                                 cl::init(""), cl::sub(AnnotateCmd));

cl::opt<std::string> AnnotateText("text",
                                  cl::desc("Annotation text (for --add)"),
                                  cl::init(""), cl::sub(AnnotateCmd));

cl::opt<std::string>
    AnnotateRemove("remove", cl::desc("Remove annotation at address (hex)"),
                   cl::init(""), cl::sub(AnnotateCmd));

//===----------------------------------------------------------------------===//
// Export-specific options
//===----------------------------------------------------------------------===//

cl::opt<ExportFormat> ExportFmt(
    "format", cl::desc("What to export"), cl::Required,
    cl::values(clEnumValN(FmtDecompile, "decompile", "Decompiled C code"),
               clEnumValN(FmtIR, "ir", "LLVM IR"),
               clEnumValN(FmtFuncs, "funcs", "Function list (JSON)"),
               clEnumValN(FmtImports, "imports", "Import table (JSON)"),
               clEnumValN(FmtExports, "exports", "Export table (JSON)"),
               clEnumValN(FmtStrings, "strings", "String table (JSON)")),
    cl::sub(ExportCmd));

cl::opt<std::string> ExportOutput("o", cl::desc("Output file path"),
                                  cl::Required, cl::sub(ExportCmd));

cl::opt<std::string>
    ExportFunc("func", cl::desc("Function name or address (for decompile/ir)"),
               cl::init(""), cl::sub(ExportCmd));

//===----------------------------------------------------------------------===//
// Rename-specific options
//===----------------------------------------------------------------------===//

cl::opt<std::string> RenameFrom("func", cl::desc("Function name to rename"),
                                cl::init(""), cl::sub(RenameCmd));

cl::opt<std::string> RenameTo("to", cl::desc("New function name"), cl::init(""),
                              cl::sub(RenameCmd));

cl::opt<bool> RenameList("list", cl::desc("List all renames"),
                         cl::sub(RenameCmd));

//===----------------------------------------------------------------------===//
// Search-specific options
//===----------------------------------------------------------------------===//

cl::opt<std::string> SearchText("text", cl::desc("Search for text string"),
                                cl::init(""), cl::sub(SearchCmd));

cl::opt<std::string>
    SearchHex("hex", cl::desc("Search for hex byte pattern (e.g. 48 8b 05)"),
              cl::init(""), cl::sub(SearchCmd));

cl::opt<bool> SearchCaseSensitive("case-sensitive",
                                  cl::desc("Case-sensitive text search"),
                                  cl::sub(SearchCmd));

cl::opt<unsigned> SearchMaxResults("max-results", cl::desc("Maximum results"),
                                   cl::init(256), cl::sub(SearchCmd));

//===----------------------------------------------------------------------===//
// CallGraph-specific options
//===----------------------------------------------------------------------===//

cl::opt<bool> CgDot("dot", cl::desc("Output in DOT format"),
                    cl::sub(CallGraphCmd));

cl::opt<std::string> CgSvg("svg", cl::desc("Output call graph as SVG file"),
                           cl::init(""), cl::sub(CallGraphCmd));

//===----------------------------------------------------------------------===//
// Patch from external file options
//===----------------------------------------------------------------------===//

cl::opt<std::string>
    PatchFromIR("from-ir", cl::desc("Patch from external LLVM IR file (.ll)"),
                cl::init(""), cl::sub(PatchCmd));

cl::opt<std::string>
    PatchFromC("from-c", cl::desc("Patch from C source file (requires clang)"),
               cl::init(""), cl::sub(PatchCmd));

cl::opt<std::string>
    PatchFuncAddr("func", cl::desc("Function address for --from-c patch (hex)"),
                  cl::init(""), cl::sub(PatchCmd));

//===----------------------------------------------------------------------===//
// Patch-specific options
//===----------------------------------------------------------------------===//

cl::opt<PatchStrategy> PatchStrat(
    "mode", cl::desc("Patch mode"), cl::init(SectionMode),
    cl::values(clEnumValN(SectionMode, "section",
                          "Add new section with trampolines"),
               clEnumValN(InplaceMode, "inplace", "In-place binary rewriting")),
    cl::sub(PatchCmd));

cl::opt<std::string> TextSection(
    "text-section",
    cl::desc("Name of the original code section to patch when it is not the "
             "canonical .text/__text — e.g. a binary processed by a "
             "packer/protector that renamed it (.vmp0, UPX1, .themida). "
             "Default: format-specific .text/__text."),
    cl::init(""), cl::sub(PatchCmd));

//===----------------------------------------------------------------------===//
// Sigs-specific options
//===----------------------------------------------------------------------===//

cl::opt<std::string> SigDir("sig-dir",
                            cl::desc("Directory containing .sig/.pat files"),
                            cl::init(""), cl::sub(SigsCmd));

cl::opt<std::string> SigFile("sig-file",
                             cl::desc("Single .sig or .pat file to load"),
                             cl::init(""), cl::sub(SigsCmd));

cl::opt<bool>
    SigAuto("auto", cl::desc("Auto-detect arch/format and load matching sigs"),
            cl::sub(SigsCmd));

//===----------------------------------------------------------------------===//
// Simplify-specific options
//===----------------------------------------------------------------------===//

cl::opt<std::string> SimplifyExpr(cl::Positional,
                                  cl::desc("<expression>"), cl::init(""),
                                  cl::sub(SimplifyCmd));

cl::opt<std::string> SimplifyFile(
    "f",
    cl::desc("Read one expression per line from a file, or '-' for stdin"),
    cl::init(""), cl::sub(SimplifyCmd));

cl::opt<unsigned> SimplifyWidth(
    "width",
    cl::desc("Bit width of every leaf without an explicit '#bits' suffix "
             "(default 32)"),
    cl::init(32), cl::sub(SimplifyCmd));

cl::opt<bool> SimplifyShallow(
    "shallow",
    cl::desc("Measure the expression as one region instead of walking into "
             "the subterms a single measurement has to treat as opaque"),
    cl::sub(SimplifyCmd));

cl::opt<bool> SimplifyJson("json", cl::desc("Output as JSON"),
                           cl::sub(SimplifyCmd));

} // namespace neverd::cli
