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

#include "neverd/emulation/DriverProfile.h"
#include "neverd/emulation/ExecutionBackend.h"
#include "neverd/emulation/ExecutionCLIStrings.h"
#include "neverd/emulation/ExecutionReportFields.h"
#include "neverd/emulation/ProcessCLIStrings.h"
#include "neverd/emulation/ProcessReportFields.h"
#include "neverd/loader/ARMModeCLIStrings.h"
#include "neverd/unpack/UnpackCLIStrings.h"
#include "neverd/unpack/UnpackStrings.h"

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
cl::SubCommand WebCmd("web", "Inspect offline web artifacts");
cl::list<std::string> WebArguments(
    cl::Positional,
    cl::desc(
        "capabilities | inspect <input> | bun|map <file> | "
        "bun-map <file> <module-index> | "
        "source|bindings|semantics|modules|bundles|view|navigate <file> "
        "<script|module|commonjs> | anchor <file> <source-type> "
        "<byte-offset> <byte-length> | bun-view|bun-navigate <file> "
        "<module-index> <source-type> | "
        "bun-anchor <file> <module-index> <source-type> <byte-offset> "
        "<byte-length> | native|native-analyze <file> | "
        "bun-native|bun-native-analyze <file> <module-index> | "
        "asar <file-or-root> [archive-index [unpacked-index|-]] | "
        "electron-manifest <file-or-root> [manifest-index] | "
        "electron-source <file> <source-type> | "
        "electron-ipc <root> <manifest-index> <source-index:source-type>... | "
        "asar-ipc <root> <archive-index> <unpacked-index|-> "
        "<manifest-member-index> <source-member-index:source-type>... | "
        "bun-electron <file> <module-index> <source-type> | "
        "asar-manifest <root> <archive-index> "
        "<unpacked-directory-index|-> <member-index> | "
        "asar-source|asar-view|asar-navigate|asar-electron <root> "
        "<archive-index> "
        "<unpacked-index|-> <member-index> <source-type> | "
        "asar-anchor <root> <archive-index> <unpacked-index|-> "
        "<member-index> <source-type> <byte-offset> <byte-length> | "
        "asar-map|asar-native|asar-native-analyze <root> <archive-index> "
        "<unpacked-index|-> <member-index>"),
    cl::ZeroOrMore, cl::sub(WebCmd));
cl::SubCommand
    MobileCmd("mobile",
              "Recover Android Java and iOS native sources and metadata");

cl::opt<std::string> MobilePlatform("platform",
                                    cl::desc("auto, android, or ios"),
                                    cl::init("auto"), cl::sub(MobileCmd));
cl::opt<std::string>
    MobileArch("arch", cl::desc("iOS slice: auto, arm64, arm, x86_64, or i386"),
               cl::init("auto"), cl::sub(MobileCmd));
cl::opt<std::string>
    MobileArtifact("artifact",
                   cl::desc("iOS executable path relative to the app bundle"),
                   cl::init(""), cl::sub(MobileCmd));
cl::opt<bool> MobileMetadataOnly(
    "metadata-only",
    cl::desc("Export iOS metadata without native C decompilation"),
    cl::sub(MobileCmd));
cl::opt<bool> MobileListClasses(
    "list-classes", cl::desc("List APK/DEX class descriptors without recovery"),
    cl::sub(MobileCmd));
cl::opt<std::string> MobileClassPrefix(
    "class-prefix",
    cl::desc("Class descriptor or dotted prefix for --list-classes"),
    cl::init(""), cl::sub(MobileCmd));
cl::opt<std::string> MobileFindRefs(
    "find-refs",
    cl::desc("Find APK/DEX code references: string, type, method, field"),
    cl::init(""), cl::sub(MobileCmd));
cl::opt<std::string>
    MobileReferenceQuery("query",
                         cl::desc("Literal target text for --find-refs"),
                         cl::init(""), cl::sub(MobileCmd));
cl::opt<bool> MobileReferenceExact(
    "exact", cl::desc("Match the complete target identity for --find-refs"),
    cl::sub(MobileCmd));
cl::opt<std::string> MobileReferenceOwner(
    "owner",
    cl::desc("Exact target owner descriptor for method/field references"),
    cl::init(""), cl::sub(MobileCmd));
cl::opt<bool> MobileInternalIOSWorker("internal-ios-worker", cl::Hidden,
                                      cl::init(false), cl::sub(MobileCmd));
cl::opt<unsigned> MobileTimeout(
    "timeout", cl::desc("Seconds for builtin analysis or each backend process"),
    cl::init(300), cl::sub(MobileCmd));
cl::opt<unsigned>
    MobileMaxFiles("max-files",
                   cl::desc("Maximum mobile input/output file count"),
                   cl::init(20000), cl::sub(MobileCmd));
cl::opt<uint64_t> MobileMaxBytes("max-bytes",
                                 cl::desc("Maximum mobile input/output bytes"),
                                 cl::init(2ULL * 1024 * 1024 * 1024),
                                 cl::sub(MobileCmd));
cl::SubCommand DecompileCmd("decompile",
                            "Decompile binary to C, Rust, or Solidity");
cl::SubCommand PatchCmd("patch", "Patch binary with modified IR");
cl::SubCommand InfoCmd("info", "Show binary metadata summary");
cl::SubCommand IdentifyCmd("identify",
                           "List the ways NeverD can load a file, as the load "
                           "dialog does");
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
cl::SubCommand FunctionEditsCmd("function-edits",
                                "Create or delete functions, or list those "
                                "edits");
cl::SubCommand ItemsCmd("items", "Define data items, or list them");
cl::SubCommand OperandsCmd("operands",
                           "Show instruction operands' numbers in a base, or "
                           "list those formats");
cl::SubCommand SearchCmd("search", "Search bytes/strings in binary");
cl::SubCommand SectionsCmd("sections", "List binary sections");
cl::SubCommand SymbolsCmd("symbols", "List all symbols");
cl::SubCommand RelocsCmd("relocs", "List relocations");
cl::SubCommand HeadersCmd("headers", "Show comprehensive binary headers");
cl::SubCommand EntryPointsCmd("entrypoints", "List binary entry points");
cl::SubCommand SwitchesCmd("switches",
                           "List the switch jump tables analysis recovers");
cl::SubCommand DashboardCmd("dashboard", "Show binary overview dashboard");
cl::SubCommand SigsCmd("sigs", "Apply FLIRT signatures to binary");
cl::SubCommand SymbolicCmd("sym-explore",
                           "Explore bounded symbolic paths through a function");
cl::SubCommand AuditCmd("audit",
                        "Audit heap object lifetimes and uninitialized local "
                        "stack reads");
cl::SubCommand HuntCmd("hunt",
                       "Hunt dangerous-copy overflows and report a witness");
cl::SubCommand
    ConcolicCmd("concolic",
                "Generate replay-verified LowIR conditional-branch seeds");
// Takes text rather than a binary, so it is not among the subcommands that
// register the positional input file below.
cl::SubCommand SimplifyCmd("simplify", "Simplify a bitvector expression");
cl::SubCommand OptimizeIRCmd("optimize-ir",
                             "Optimize textual LLVM IR transactionally");
cl::SubCommand
    EmulateDriverCmd("emulate-driver",
                     "Run bounded x64 WDM driver initialization and emit JSON");
cl::SubCommand CPUCapabilitiesCmd(execution_cli::CapabilitiesCommand,
                                  execution_cli::CapabilitiesHelp);
cl::SubCommand EmulateProcessCmd(process_cli::Command, process_cli::Help);
cl::SubCommand UnpackCmd(unpack_cli::Command, unpack_cli::Help);
cl::SubCommand TranslateObjectCmd(
    "translate-object",
    "Compile canonical legacy-prefix-free x86-64 v1 REX.W full-width GPR MOV, "
    "ADD/SUB, register/immediate AND/OR/XOR, full-width register-only CMP "
    "39/3B, "
    "register/immediate CMP 81/7, 83/7, and 3D, full-width register-only TEST "
    "85, and "
    "register/immediate TEST F7/0 and A9 forms (logical and TEST flags "
    "preserve AF in the NeverD state model), terminated by C3 RET, C2 iw "
    "RET-imm16, direct-relative EB "
    "cb/E9 cd JMP, or schema-9 legacy-prefix-free traditional Jcc: JO/JNO "
    "70/71 cb or "
    "0F 80/81 cd, JB/JAE 72/73 cb or 0F 82/83 cd, JE/JNE 74/75 cb or 0F "
    "84/85 cd, JBE/JA 76/77 cb or 0F 86/87 cd, JS/JNS 78/79 cb or 0F 88/89 "
    "cd, JP/JNP 7A/7B cb or 0F 8A/8B cd, JL/JGE 7C/7D cb or 0F 8C/8D cd, "
    "and JLE/JG 7E/7F cb or 0F 8E/8F cd. Reserved F7 /1, guest memory, "
    "partial registers, legacy prefixes, redundant REX bits, "
    "JRCXZ/JECXZ/JCXZ, and LOOP/LOOPE/LOOPNE remain unpublished and fail "
    "closed. Emits an audited "
    "AArch64 relocatable object");

//===----------------------------------------------------------------------===//
// Common options (registered with all subcommands)
//===----------------------------------------------------------------------===//

cl::opt<std::string> InputFile(
    cl::Positional, cl::desc("<binary>"), cl::Required, cl::sub(LiftCmd),
    cl::sub(DecompileCmd), cl::sub(PatchCmd), cl::sub(InfoCmd),
    cl::sub(IdentifyCmd), cl::sub(StringsCmd), cl::sub(XrefsCmd),
    cl::sub(FuncsCmd), cl::sub(DisasmCmd), cl::sub(CfgCmd), cl::sub(HexCmd),
    cl::sub(ImportsCmd), cl::sub(ExportsCmd), cl::sub(SegmentsCmd),
    cl::sub(ExportCmd), cl::sub(BookmarksCmd), cl::sub(AnnotateCmd),
    cl::sub(CallGraphCmd), cl::sub(RenameCmd), cl::sub(FunctionEditsCmd),
    cl::sub(ItemsCmd), cl::sub(OperandsCmd), cl::sub(SearchCmd),
    cl::sub(SectionsCmd), cl::sub(SymbolsCmd), cl::sub(RelocsCmd),
    cl::sub(HeadersCmd), cl::sub(EntryPointsCmd), cl::sub(SwitchesCmd),
    cl::sub(DashboardCmd), cl::sub(SigsCmd), cl::sub(SymbolicCmd),
    cl::sub(AuditCmd), cl::sub(HuntCmd), cl::sub(MobileCmd));

cl::opt<std::string> OutputFile("o", cl::desc("Output file"), cl::init(""),
                                cl::sub(LiftCmd), cl::sub(DecompileCmd),
                                cl::sub(PatchCmd), cl::sub(AuditCmd),
                                cl::sub(HuntCmd), cl::sub(MobileCmd));

cl::opt<bool>
    Verbose("v", cl::desc("Verbose output"), cl::sub(LiftCmd),
            cl::sub(DecompileCmd), cl::sub(PatchCmd), cl::sub(InfoCmd),
            cl::sub(StringsCmd), cl::sub(FuncsCmd), cl::sub(DisasmCmd),
            cl::sub(CfgCmd), cl::sub(HexCmd), cl::sub(ImportsCmd),
            cl::sub(ExportsCmd), cl::sub(SegmentsCmd), cl::sub(ExportCmd),
            cl::sub(BookmarksCmd), cl::sub(AnnotateCmd), cl::sub(CallGraphCmd),
            cl::sub(RenameCmd), cl::sub(FunctionEditsCmd), cl::sub(ItemsCmd),
            cl::sub(OperandsCmd), cl::sub(SearchCmd), cl::sub(SectionsCmd),
            cl::sub(SymbolsCmd), cl::sub(RelocsCmd), cl::sub(HeadersCmd),
            cl::sub(EntryPointsCmd), cl::sub(SwitchesCmd),
            cl::sub(DashboardCmd), cl::sub(SigsCmd), cl::sub(SymbolicCmd));

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

//===----------------------------------------------------------------------===//
// Debug-symbol options
//
// Registered with every subcommand that opens a binary: debug symbols name the
// functions each of them reports, not just the ones that decompile.
//===----------------------------------------------------------------------===//

cl::opt<bool> NoDebug(
    "no-debug",
    cl::desc("Ignore debug symbols and read the image alone.  Use when a "
             "companion .pdb/.map is stale or belongs to a sibling build"),
    cl::sub(LiftCmd), cl::sub(DecompileCmd), cl::sub(PatchCmd),
    cl::sub(InfoCmd), cl::sub(StringsCmd), cl::sub(XrefsCmd), cl::sub(FuncsCmd),
    cl::sub(DisasmCmd), cl::sub(CfgCmd), cl::sub(HexCmd), cl::sub(ImportsCmd),
    cl::sub(ExportsCmd), cl::sub(SegmentsCmd), cl::sub(ExportCmd),
    cl::sub(BookmarksCmd), cl::sub(AnnotateCmd), cl::sub(CallGraphCmd),
    cl::sub(RenameCmd), cl::sub(FunctionEditsCmd), cl::sub(ItemsCmd),
    cl::sub(OperandsCmd), cl::sub(SearchCmd), cl::sub(SectionsCmd),
    cl::sub(SymbolsCmd), cl::sub(RelocsCmd), cl::sub(HeadersCmd),
    cl::sub(EntryPointsCmd), cl::sub(SwitchesCmd), cl::sub(DashboardCmd),
    cl::sub(SigsCmd), cl::sub(SymbolicCmd), cl::sub(AuditCmd),
    cl::sub(HuntCmd));

cl::opt<std::string> LoadLoader(
    "loader",
    cl::desc("How to read an input that names no format itself: \"binary\" "
             "as code of --processor at --load-base, \"evm\" as EVM "
             "bytecode, \"auto\" as its header or contents say.  The choice "
             "is kept with the input"),
    cl::init(""), cl::value_desc("loader"), cl::sub(LiftCmd),
    cl::sub(DecompileCmd), cl::sub(PatchCmd), cl::sub(InfoCmd),
    cl::sub(StringsCmd), cl::sub(XrefsCmd), cl::sub(FuncsCmd),
    cl::sub(DisasmCmd), cl::sub(CfgCmd), cl::sub(HexCmd), cl::sub(ImportsCmd),
    cl::sub(ExportsCmd), cl::sub(SegmentsCmd), cl::sub(ExportCmd),
    cl::sub(BookmarksCmd), cl::sub(AnnotateCmd), cl::sub(CallGraphCmd),
    cl::sub(RenameCmd), cl::sub(FunctionEditsCmd), cl::sub(ItemsCmd),
    cl::sub(OperandsCmd), cl::sub(SearchCmd), cl::sub(SectionsCmd),
    cl::sub(SymbolsCmd), cl::sub(RelocsCmd), cl::sub(HeadersCmd),
    cl::sub(EntryPointsCmd), cl::sub(SwitchesCmd), cl::sub(DashboardCmd),
    cl::sub(SigsCmd), cl::sub(SymbolicCmd), cl::sub(AuditCmd),
    cl::sub(HuntCmd));
cl::opt<std::string> LoadProcessor(
    "processor",
    cl::desc("Processor a binary file's code runs on: x86, x86_64, arm, "
             "thumb or aarch64; auto, the default, reads it from the bytes"),
    cl::init(""), cl::value_desc("processor"), cl::sub(LiftCmd),
    cl::sub(DecompileCmd), cl::sub(PatchCmd), cl::sub(InfoCmd),
    cl::sub(StringsCmd), cl::sub(XrefsCmd), cl::sub(FuncsCmd),
    cl::sub(DisasmCmd), cl::sub(CfgCmd), cl::sub(HexCmd), cl::sub(ImportsCmd),
    cl::sub(ExportsCmd), cl::sub(SegmentsCmd), cl::sub(ExportCmd),
    cl::sub(BookmarksCmd), cl::sub(AnnotateCmd), cl::sub(CallGraphCmd),
    cl::sub(RenameCmd), cl::sub(FunctionEditsCmd), cl::sub(ItemsCmd),
    cl::sub(OperandsCmd), cl::sub(SearchCmd), cl::sub(SectionsCmd),
    cl::sub(SymbolsCmd), cl::sub(RelocsCmd), cl::sub(HeadersCmd),
    cl::sub(EntryPointsCmd), cl::sub(SwitchesCmd), cl::sub(DashboardCmd),
    cl::sub(SigsCmd), cl::sub(SymbolicCmd), cl::sub(AuditCmd),
    cl::sub(HuntCmd));
cl::opt<std::string> LoadPlatform(
    "platform",
    cl::desc("Platform whose conventions a binary file's code follows: "
             "\"auto\" reads it from the code, or \"sysv\", \"windows\" or "
             "\"darwin\""),
    cl::init(""), cl::value_desc("platform"), cl::sub(LiftCmd),
    cl::sub(DecompileCmd), cl::sub(PatchCmd), cl::sub(InfoCmd),
    cl::sub(StringsCmd), cl::sub(XrefsCmd), cl::sub(FuncsCmd),
    cl::sub(DisasmCmd), cl::sub(CfgCmd), cl::sub(HexCmd), cl::sub(ImportsCmd),
    cl::sub(ExportsCmd), cl::sub(SegmentsCmd), cl::sub(ExportCmd),
    cl::sub(BookmarksCmd), cl::sub(AnnotateCmd), cl::sub(CallGraphCmd),
    cl::sub(RenameCmd), cl::sub(FunctionEditsCmd), cl::sub(ItemsCmd),
    cl::sub(OperandsCmd), cl::sub(SearchCmd), cl::sub(SectionsCmd),
    cl::sub(SymbolsCmd), cl::sub(RelocsCmd), cl::sub(HeadersCmd),
    cl::sub(EntryPointsCmd), cl::sub(SwitchesCmd), cl::sub(DashboardCmd),
    cl::sub(SigsCmd), cl::sub(SymbolicCmd), cl::sub(AuditCmd),
    cl::sub(HuntCmd));
cl::opt<std::string>
    LoadBase("load-base", cl::desc("Address a binary file maps at (hex)"),
             cl::init(""), cl::value_desc("address"), cl::sub(LiftCmd),
             cl::sub(DecompileCmd), cl::sub(PatchCmd), cl::sub(InfoCmd),
             cl::sub(StringsCmd), cl::sub(XrefsCmd), cl::sub(FuncsCmd),
             cl::sub(DisasmCmd), cl::sub(CfgCmd), cl::sub(HexCmd),
             cl::sub(ImportsCmd), cl::sub(ExportsCmd), cl::sub(SegmentsCmd),
             cl::sub(ExportCmd), cl::sub(BookmarksCmd), cl::sub(AnnotateCmd),
             cl::sub(CallGraphCmd), cl::sub(RenameCmd),
             cl::sub(FunctionEditsCmd), cl::sub(ItemsCmd), cl::sub(OperandsCmd),
             cl::sub(SearchCmd), cl::sub(SectionsCmd), cl::sub(SymbolsCmd),
             cl::sub(RelocsCmd), cl::sub(HeadersCmd), cl::sub(EntryPointsCmd),
             cl::sub(SwitchesCmd), cl::sub(DashboardCmd), cl::sub(SigsCmd),
             cl::sub(SymbolicCmd), cl::sub(AuditCmd), cl::sub(HuntCmd));
cl::opt<std::string>
    LoadOffset("load-offset",
               cl::desc("Where in a binary file the mapped bytes start (hex)"),
               cl::init(""), cl::value_desc("offset"), cl::sub(LiftCmd),
               cl::sub(DecompileCmd), cl::sub(PatchCmd), cl::sub(InfoCmd),
               cl::sub(StringsCmd), cl::sub(XrefsCmd), cl::sub(FuncsCmd),
               cl::sub(DisasmCmd), cl::sub(CfgCmd), cl::sub(HexCmd),
               cl::sub(ImportsCmd), cl::sub(ExportsCmd), cl::sub(SegmentsCmd),
               cl::sub(ExportCmd), cl::sub(BookmarksCmd), cl::sub(AnnotateCmd),
               cl::sub(CallGraphCmd), cl::sub(RenameCmd),
               cl::sub(FunctionEditsCmd), cl::sub(ItemsCmd),
               cl::sub(OperandsCmd), cl::sub(SearchCmd), cl::sub(SectionsCmd),
               cl::sub(SymbolsCmd), cl::sub(RelocsCmd), cl::sub(HeadersCmd),
               cl::sub(EntryPointsCmd), cl::sub(SwitchesCmd),
               cl::sub(DashboardCmd), cl::sub(SigsCmd), cl::sub(SymbolicCmd),
               cl::sub(AuditCmd), cl::sub(HuntCmd));
cl::opt<std::string>
    LoadSize("load-size",
             cl::desc("How many bytes of a binary file to map (hex; the rest "
                      "of the file when absent)"),
             cl::init(""), cl::value_desc("size"), cl::sub(LiftCmd),
             cl::sub(DecompileCmd), cl::sub(PatchCmd), cl::sub(InfoCmd),
             cl::sub(StringsCmd), cl::sub(XrefsCmd), cl::sub(FuncsCmd),
             cl::sub(DisasmCmd), cl::sub(CfgCmd), cl::sub(HexCmd),
             cl::sub(ImportsCmd), cl::sub(ExportsCmd), cl::sub(SegmentsCmd),
             cl::sub(ExportCmd), cl::sub(BookmarksCmd), cl::sub(AnnotateCmd),
             cl::sub(CallGraphCmd), cl::sub(RenameCmd),
             cl::sub(FunctionEditsCmd), cl::sub(ItemsCmd), cl::sub(OperandsCmd),
             cl::sub(SearchCmd), cl::sub(SectionsCmd), cl::sub(SymbolsCmd),
             cl::sub(RelocsCmd), cl::sub(HeadersCmd), cl::sub(EntryPointsCmd),
             cl::sub(SwitchesCmd), cl::sub(DashboardCmd), cl::sub(SigsCmd),
             cl::sub(SymbolicCmd), cl::sub(AuditCmd), cl::sub(HuntCmd));
cl::opt<std::string>
    LoadEntry("load-entry",
              cl::desc("Where a binary file's execution starts (hex; the base "
                       "when absent)"),
              cl::init(""), cl::value_desc("address"), cl::sub(LiftCmd),
              cl::sub(DecompileCmd), cl::sub(PatchCmd), cl::sub(InfoCmd),
              cl::sub(StringsCmd), cl::sub(XrefsCmd), cl::sub(FuncsCmd),
              cl::sub(DisasmCmd), cl::sub(CfgCmd), cl::sub(HexCmd),
              cl::sub(ImportsCmd), cl::sub(ExportsCmd), cl::sub(SegmentsCmd),
              cl::sub(ExportCmd), cl::sub(BookmarksCmd), cl::sub(AnnotateCmd),
              cl::sub(CallGraphCmd), cl::sub(RenameCmd),
              cl::sub(FunctionEditsCmd), cl::sub(ItemsCmd),
              cl::sub(OperandsCmd), cl::sub(SearchCmd), cl::sub(SectionsCmd),
              cl::sub(SymbolsCmd), cl::sub(RelocsCmd), cl::sub(HeadersCmd),
              cl::sub(EntryPointsCmd), cl::sub(SwitchesCmd),
              cl::sub(DashboardCmd), cl::sub(SigsCmd), cl::sub(SymbolicCmd),
              cl::sub(AuditCmd), cl::sub(HuntCmd));

cl::list<std::string> ARMFunctionModeHints(
    arm_mode_cli::Option, cl::desc(arm_mode_cli::Description),
    cl::value_desc(arm_mode_cli::ValueDescription), cl::sub(LiftCmd),
    cl::sub(DecompileCmd), cl::sub(PatchCmd), cl::sub(InfoCmd),
    cl::sub(StringsCmd), cl::sub(XrefsCmd), cl::sub(FuncsCmd),
    cl::sub(DisasmCmd), cl::sub(CfgCmd), cl::sub(HexCmd), cl::sub(ImportsCmd),
    cl::sub(ExportsCmd), cl::sub(SegmentsCmd), cl::sub(ExportCmd),
    cl::sub(BookmarksCmd), cl::sub(AnnotateCmd), cl::sub(CallGraphCmd),
    cl::sub(RenameCmd), cl::sub(FunctionEditsCmd), cl::sub(ItemsCmd),
    cl::sub(OperandsCmd), cl::sub(SearchCmd), cl::sub(SectionsCmd),
    cl::sub(SymbolsCmd), cl::sub(RelocsCmd), cl::sub(HeadersCmd),
    cl::sub(EntryPointsCmd), cl::sub(SwitchesCmd), cl::sub(DashboardCmd),
    cl::sub(SigsCmd), cl::sub(SymbolicCmd), cl::sub(AuditCmd),
    cl::sub(HuntCmd));

cl::opt<std::string> PdbFile(
    "pdb",
    cl::desc("Load symbols from this .pdb instead of searching beside the "
             "binary.  Failing to read it is an error, not a fallback"),
    cl::init(""), cl::value_desc("file"), cl::sub(LiftCmd),
    cl::sub(DecompileCmd), cl::sub(PatchCmd), cl::sub(InfoCmd),
    cl::sub(StringsCmd), cl::sub(XrefsCmd), cl::sub(FuncsCmd),
    cl::sub(DisasmCmd), cl::sub(CfgCmd), cl::sub(HexCmd), cl::sub(ImportsCmd),
    cl::sub(ExportsCmd), cl::sub(SegmentsCmd), cl::sub(ExportCmd),
    cl::sub(BookmarksCmd), cl::sub(AnnotateCmd), cl::sub(CallGraphCmd),
    cl::sub(RenameCmd), cl::sub(FunctionEditsCmd), cl::sub(ItemsCmd),
    cl::sub(OperandsCmd), cl::sub(SearchCmd), cl::sub(SectionsCmd),
    cl::sub(SymbolsCmd), cl::sub(RelocsCmd), cl::sub(HeadersCmd),
    cl::sub(EntryPointsCmd), cl::sub(SwitchesCmd), cl::sub(DashboardCmd),
    cl::sub(SigsCmd), cl::sub(SymbolicCmd), cl::sub(AuditCmd),
    cl::sub(HuntCmd));

cl::opt<std::string> MapFile(
    "map",
    cl::desc("Load symbols from this linker .map (MSVC, lld, GNU ld, or "
             "ld64) instead of searching beside the binary"),
    cl::init(""), cl::value_desc("file"), cl::sub(LiftCmd),
    cl::sub(DecompileCmd), cl::sub(PatchCmd), cl::sub(InfoCmd),
    cl::sub(StringsCmd), cl::sub(XrefsCmd), cl::sub(FuncsCmd),
    cl::sub(DisasmCmd), cl::sub(CfgCmd), cl::sub(HexCmd), cl::sub(ImportsCmd),
    cl::sub(ExportsCmd), cl::sub(SegmentsCmd), cl::sub(ExportCmd),
    cl::sub(BookmarksCmd), cl::sub(AnnotateCmd), cl::sub(CallGraphCmd),
    cl::sub(RenameCmd), cl::sub(FunctionEditsCmd), cl::sub(ItemsCmd),
    cl::sub(OperandsCmd), cl::sub(SearchCmd), cl::sub(SectionsCmd),
    cl::sub(SymbolsCmd), cl::sub(RelocsCmd), cl::sub(HeadersCmd),
    cl::sub(EntryPointsCmd), cl::sub(SwitchesCmd), cl::sub(DashboardCmd),
    cl::sub(SigsCmd), cl::sub(SymbolicCmd), cl::sub(AuditCmd),
    cl::sub(HuntCmd));

cl::opt<bool> NoOpt("no-opt", cl::desc("Skip LLVM optimization passes"),
                    cl::sub(LiftCmd), cl::sub(DecompileCmd), cl::sub(PatchCmd),
                    cl::sub(CfgCmd));

cl::opt<size_t> MaxFunc("max-func", cl::desc("Limit to first N functions"),
                        cl::init(0), cl::sub(LiftCmd), cl::sub(DecompileCmd),
                        cl::sub(PatchCmd), cl::sub(MobileCmd),
                        cl::sub(ExportCmd));

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
    LlvmRoute("llvm", cl::desc("Emit LLVM-to-C (goto-style) instead of HighC"),
              cl::sub(DecompileCmd), cl::sub(ExportCmd));

cl::opt<bool> Devirtualize(
    "devirtualize",
    cl::desc("Recover an x64 interpreter (experimental; requires --func)"),
    cl::sub(DecompileCmd));
cl::opt<bool> VMMachineState(
    "vm-machine-state",
    cl::desc(
        "Explicit x64 state ABI; normal nonfaulting CPL3 execution, CET off"),
    cl::sub(DecompileCmd));
cl::list<std::string> VMControlRegisters(
    "vm-control",
    cl::desc("Full GPR used to separate interpreter contexts (repeatable)"),
    cl::ZeroOrMore, cl::sub(DecompileCmd));
cl::list<std::string> VMControlFrameSlots(
    "vm-control-stack",
    cl::desc("Entry-RSP-relative context slot offset:bytes (repeatable)"),
    cl::ZeroOrMore, cl::sub(DecompileCmd));
cl::opt<unsigned> VMMaxNodes("vm-max-nodes", cl::desc("Recovery node budget"),
                             cl::init(4096), cl::sub(DecompileCmd));
cl::opt<unsigned> VMMaxContexts("vm-max-contexts",
                                cl::desc("Contexts per native address"),
                                cl::init(64), cl::sub(DecompileCmd));
cl::opt<std::string> VMMaxRefinements(
    "vm-max-refinements",
    cl::desc("Control-state refinement rounds (positive; default: 16)"),
    cl::value_desc("count"), cl::init("16"), cl::sub(DecompileCmd));
cl::opt<std::string> VMMaxFields(
    "vm-max-fields",
    cl::desc(
        "Total manual and discovered control fields (positive; default: 16)"),
    cl::value_desc("count"), cl::init("16"), cl::sub(DecompileCmd));
cl::opt<std::string> VMMaxQueries(
    "vm-max-queries",
    cl::desc("Cumulative recovery solver queries (positive; default: 4096)"),
    cl::value_desc("count"), cl::init("4096"), cl::sub(DecompileCmd));
cl::opt<std::string> VMChainTransfers(
    "vm-chain-transfers",
    cl::desc("Proved singleton transfers per recovery node (0 disables)"),
    cl::value_desc("count"), cl::init("0"), cl::sub(DecompileCmd));
cl::opt<std::string> VMChainVisits(
    "vm-chain-visits",
    cl::desc("Visits per native destination in a chain (0 adds no cap; "
             "stop-at-repeat takes precedence)"),
    cl::value_desc("count"), cl::init("0"), cl::sub(DecompileCmd));
cl::opt<std::string> VMMaxEvaluations(
    "vm-max-evaluations",
    cl::desc("Cumulative recovery node evaluations (positive; default: 16384)"),
    cl::value_desc("count"), cl::init("16384"), cl::sub(DecompileCmd));
cl::opt<bool> VMChainStopAtRepeat(
    "vm-chain-stop-at-repeat",
    cl::desc("End chains at repeated destinations (may lose correlations)"),
    cl::sub(DecompileCmd));
cl::opt<std::string> VMMaxDiscoveryVisits(
    "vm-max-discovery-visits",
    cl::desc("Cumulative control dependency visits (positive; default: 65536)"),
    cl::value_desc("count"), cl::init("65536"), cl::sub(DecompileCmd));
cl::opt<std::string> VMEntryFrame(
    "vm-entry-frame",
    cl::desc(
        "Unchecked nonwrapping entry-RSP offsets for machine-state recovery"),
    cl::value_desc("begin:end"), cl::sub(DecompileCmd));
cl::opt<std::string> VMMaxSymbolicNodes(
    "vm-max-symbolic-nodes",
    cl::desc("Per-node recovery symbolic DAG budget (default: 262144)"),
    cl::value_desc("count"), cl::init("262144"), cl::sub(DecompileCmd));
cl::opt<std::string> VMEntryAlignment(
    "vm-entry-alignment",
    cl::desc("Checked entry-RSP congruence for machine-state recovery"),
    cl::value_desc("alignment:residue"), cl::sub(DecompileCmd));
cl::opt<bool> VMExternalStoresDisjointFrame(
    "vm-external-stores-disjoint-frame",
    cl::desc(
        "Assume external STORE extents avoid --vm-entry-frame (unchecked)"),
    cl::init(false), cl::sub(DecompileCmd));
cl::opt<bool> VMNoControlDiscovery(
    "vm-no-control-discovery",
    cl::desc("Disable automatic recovery control-state discovery"),
    cl::init(false), cl::sub(DecompileCmd));
cl::opt<uint64_t> VMMaxOperations("vm-max-operations",
                                  cl::desc("Recovery operation budget"),
                                  cl::init(262144), cl::sub(DecompileCmd));
cl::opt<std::string>
    VMRecoveryReport("recovery-report",
                     cl::desc("Write local recovery evidence JSON"),
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
#include "neverd/evm/bytecode/EVMHardforks.def"
                                   }),
                                   cl::init(evm::Hardfork::Latest),
                                   cl::sub(LiftCmd), cl::sub(DecompileCmd),
                                   cl::sub(DisasmCmd), cl::sub(CfgCmd));

cl::opt<bool> EVMRelaxed(
    "evm-relaxed",
    cl::desc("Keep unknown/inactive EVM opcodes as explicit fault nodes"),
    cl::sub(LiftCmd), cl::sub(DecompileCmd), cl::sub(DisasmCmd),
    cl::sub(CfgCmd));

cl::opt<sbf::Version> SBFVersion("sbf-version", cl::desc("Solana SBF version"),
                                 cl::ValuesClass({
#define SBF_VERSION_AUTO(NAME, SPELLING, DISPLAY_NAME)                         \
  clEnumValN(sbf::Version::NAME, SPELLING, DISPLAY_NAME),
#define SBF_VERSION(NAME, ELF_FLAGS, SPELLING, DISPLAY_NAME, FEATURES, STATUS) \
  clEnumValN(sbf::Version::NAME, SPELLING, DISPLAY_NAME),
#include "neverd/sbf/image/SBFVersions.def"
                                 }),
                                 cl::init(sbf::Version::Auto), cl::sub(LiftCmd),
                                 cl::sub(DecompileCmd));

cl::opt<bool> SBFRelaxed(
    "sbf-relaxed",
    cl::desc(
        "Keep invalid or version-inactive SBF instructions as fault nodes"),
    cl::sub(LiftCmd), cl::sub(DecompileCmd));

cl::opt<std::string>
    SBFIdl("sbf-idl",
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
#include "neverd/sbf/runtime/SBFRuntimeFeatures.def"
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
#include "neverd/sbf/runtime/SBFLoaders.def"
                               }),
                               cl::init(sbf::Loader::V3), cl::sub(LiftCmd),
                               cl::sub(DecompileCmd));

cl::opt<sbf::RuntimePurpose>
    SBFPurpose("sbf-purpose",
               cl::desc("Whether to answer for running or for deploying"),
               cl::ValuesClass({
#define SBF_RUNTIME_PURPOSE(ID, NAME, SUMMARY)                                 \
  clEnumValN(sbf::RuntimePurpose::ID, NAME, SUMMARY),
#include "neverd/sbf/runtime/SBFRuntimeFeatures.def"
               }),
               cl::init(sbf::RuntimePurpose::Execution), cl::sub(LiftCmd),
               cl::sub(DecompileCmd));

//===----------------------------------------------------------------------===//
// Strings-specific options
//===----------------------------------------------------------------------===//

cl::opt<unsigned> MinStrLen("min-len",
                            cl::desc("Minimum string length in display "
                                     "columns; a wide East Asian character "
                                     "counts two"),
                            cl::init(4), cl::sub(StringsCmd));
cl::list<std::string> StringEncodings(
    "encodings",
    cl::desc("Encodings to search, comma separated: ascii, utf-8, utf-16le, "
             "utf-16be, utf-32le, utf-32be and code pages (default ascii, "
             "utf-8, utf-16le, utf-32le, gbk, big5, shift_jis, euc-kr and "
             "windows-1252)"),
    cl::CommaSeparated, cl::value_desc("names"), cl::sub(StringsCmd));
cl::opt<std::string> StringCodePage(
    "code-page",
    cl::desc("Read C strings that are not UTF-8 in this code page first, so "
             "that it wins where other code pages read them too: gbk, big5, "
             "shift_jis, euc-kr, windows-1250 to windows-1258, iso-8859-2, "
             "iso-8859-5, iso-8859-7, koi8-r, koi8-u or ibm866"),
    cl::init(""), cl::value_desc("name"), cl::sub(StringsCmd));
cl::opt<bool> StringRefs(
    "refs",
    cl::desc("List the instructions that refer to the strings, reading a "
             "reference into a string from the character it reaches"),
    cl::sub(StringsCmd));
cl::opt<std::string> StringFilter(
    "filter",
    cl::desc("Keep the strings or references whose text contains this, "
             "ignoring the case of ASCII letters"),
    cl::init(""), cl::value_desc("text"), cl::sub(StringsCmd));

//===----------------------------------------------------------------------===//
// Xrefs-specific options
//===----------------------------------------------------------------------===//

cl::opt<std::string> XrefAddr("addr", cl::desc("Target address (hex)"),
                              cl::Required, cl::sub(XrefsCmd));
cl::opt<XrefSourceKind> XrefSource(
    "source", cl::desc("Where the references come from"),
    cl::values(clEnumValN(XrefSourceKind::Direct, "direct",
                          "What each instruction states, without analysis, "
                          "as the GUI lists them (default)"),
               clEnumValN(XrefSourceKind::IR, "ir",
                          "The constants of the analyzed IR")),
    cl::init(XrefSourceKind::Direct), cl::sub(XrefsCmd));

//===----------------------------------------------------------------------===//
// Funcs/Disasm/Cfg/Hex options
//===----------------------------------------------------------------------===//

cl::opt<std::string> DisasmFunc("func",
                                cl::desc("Function name or address (hex)"),
                                cl::sub(DisasmCmd), cl::sub(CfgCmd),
                                cl::sub(SymbolicCmd));

cl::opt<bool>
    DisasmAnnotate("annotate",
                   cl::desc("Show resolved address references as comments"),
                   cl::sub(DisasmCmd));

cl::opt<std::string> HexAddr("addr", cl::desc("Start address (hex)"),
                             cl::init(""), cl::sub(HexCmd));

cl::opt<unsigned> HexSize("size", cl::desc("Number of bytes"), cl::init(256),
                          cl::sub(HexCmd));
cl::opt<std::string> HexTextEncoding(
    "text-encoding",
    cl::desc("Read the text column in this encoding, such as utf-8, "
             "utf-16le or gbk (default ASCII)"),
    cl::init(""), cl::value_desc("name"), cl::sub(HexCmd));

cl::opt<bool> CfgDot("dot", cl::desc("Output in DOT format"), cl::sub(CfgCmd));

cl::opt<std::string> CfgSvg("svg", cl::desc("Output CFG as SVG file"),
                            cl::init(""), cl::sub(CfgCmd));

cl::opt<unsigned>
    SymbolicMaxPaths("max-paths", cl::desc("Maximum reachable paths to report"),
                     cl::init(64), cl::sub(SymbolicCmd));

cl::opt<unsigned>
    SymbolicMaxSteps("max-steps",
                     cl::desc("Maximum operations along each path"),
                     cl::init(1u << 16), cl::sub(SymbolicCmd));

cl::opt<unsigned>
    SymbolicMaxBlockVisits("max-block-visits",
                           cl::desc("Maximum visits to one block per path"),
                           cl::init(3), cl::sub(SymbolicCmd));

cl::opt<bool> SymbolicExpressions(
    "expressions", cl::desc("Include path predicates and unresolved targets"),
    cl::sub(SymbolicCmd));

//===----------------------------------------------------------------------===//
// Safety options (audit / hunt)
//===----------------------------------------------------------------------===//

cl::opt<std::string> SafetySinks(
    "sinks", cl::desc("Extend the sink catalog from a specification file"),
    cl::init(""), cl::value_desc("file"), cl::sub(AuditCmd), cl::sub(HuntCmd));

cl::opt<std::string> SafetySources(
    "sources", cl::desc("Extend the input-source catalog from a file"),
    cl::init(""), cl::value_desc("file"), cl::sub(AuditCmd), cl::sub(HuntCmd));

cl::opt<unsigned> SafetyMaxPaths("max-paths",
                                 cl::desc("Maximum reachable paths to explore"),
                                 cl::init(0), cl::sub(HuntCmd),
                                 cl::sub(AuditCmd));

cl::opt<unsigned> SafetyMaxSteps("max-steps",
                                 cl::desc("Maximum operations along each path"),
                                 cl::init(0), cl::sub(HuntCmd),
                                 cl::sub(AuditCmd));

cl::opt<unsigned>
    SafetyMaxLoop("max-loop", cl::desc("Maximum iterations of one loop header"),
                  cl::init(0), cl::sub(HuntCmd), cl::sub(AuditCmd));

cl::opt<unsigned> SafetyMaxCallDepth(
    "max-call-depth",
    cl::desc("Maximum internal call edges from a known entry (0 = default)"),
    cl::init(0), cl::sub(HuntCmd), cl::sub(AuditCmd));

cl::opt<unsigned> SafetyMaxSummaryIterations(
    "max-summary-iterations",
    cl::desc("Maximum interprocedural attacker-summary rounds (0 = default)"),
    cl::init(0), cl::sub(HuntCmd), cl::sub(AuditCmd));

cl::opt<unsigned long long>
    SafetySolverConflicts("solver-conflicts",
                          cl::desc("Solver conflict budget (0 = default)"),
                          cl::init(0), cl::sub(HuntCmd), cl::sub(AuditCmd));

//===----------------------------------------------------------------------===//
// LowIR concolic options
//===----------------------------------------------------------------------===//

cl::list<std::string> ConcolicInputFiles(cl::Positional, cl::desc("<binary>"),
                                         cl::ZeroOrMore, cl::sub(ConcolicCmd));

ConcolicStringList
    ConcolicOutputFiles("o", cl::desc("Write the JSON report to this file"),
                        cl::ZeroOrMore, cl::ValueOptional,
                        cl::value_desc("file"), cl::sub(ConcolicCmd));

ConcolicStringList
    ConcolicFunctions("func",
                      cl::desc("Function name or hexadecimal entry address"),
                      cl::ZeroOrMore, cl::ValueOptional,
                      cl::value_desc("name|hex-va"), cl::sub(ConcolicCmd));

ConcolicStringList ConcolicSeeds(
    "seed",
    cl::desc("Entry register seed: offset-or-ABI-alias:bytes=value; decimal "
             "and 0x-prefixed numbers are accepted"),
    cl::ZeroOrMore, cl::ValueOptional, cl::value_desc("location:bytes=value"),
    cl::sub(ConcolicCmd));

ConcolicStringList
    ConcolicMaxSteps("max-steps",
                     cl::desc("Maximum operations along the concrete trace"),
                     cl::ZeroOrMore, cl::ValueOptional, cl::value_desc("count"),
                     cl::sub(ConcolicCmd));

ConcolicStringList ConcolicMaxBlockVisits(
    "max-block-visits", cl::desc("Maximum visits to one block"), cl::ZeroOrMore,
    cl::ValueOptional, cl::value_desc("count"), cl::sub(ConcolicCmd));

ConcolicStringList
    ConcolicMaxLoopIterations("max-loop-iterations",
                              cl::desc("Maximum visits to one loop header"),
                              cl::ZeroOrMore, cl::ValueOptional,
                              cl::value_desc("count"), cl::sub(ConcolicCmd));

ConcolicStringList ConcolicMaxFlipAttempts(
    "max-flip-attempts", cl::desc("Maximum conditional-decision solver checks"),
    cl::ZeroOrMore, cl::ValueOptional, cl::value_desc("count"),
    cl::sub(ConcolicCmd));

ConcolicStringList
    ConcolicMaxCandidates("max-candidates",
                          cl::desc("Maximum distinct replay-verified seeds"),
                          cl::ZeroOrMore, cl::ValueOptional,
                          cl::value_desc("count"), cl::sub(ConcolicCmd));

ConcolicStringList ConcolicSolverConflicts(
    "solver-conflicts", cl::desc("SAT conflict budget (0 = bounded default)"),
    cl::ZeroOrMore, cl::ValueOptional, cl::value_desc("count"),
    cl::sub(ConcolicCmd));

ConcolicStringList ConcolicSolverPropagations(
    "solver-propagations",
    cl::desc("SAT propagation budget (0 = bounded default)"), cl::ZeroOrMore,
    cl::ValueOptional, cl::value_desc("count"), cl::sub(ConcolicCmd));

ConcolicStringList ConcolicSolverWatchVisits(
    "solver-watch-visits",
    cl::desc("SAT watched-clause visit budget (0 = bounded default)"),
    cl::ZeroOrMore, cl::ValueOptional, cl::value_desc("count"),
    cl::sub(ConcolicCmd));

ConcolicStringList ConcolicSolverGates(
    "solver-gates", cl::desc("Bit-blasting gate budget (0 = bounded default)"),
    cl::ZeroOrMore, cl::ValueOptional, cl::value_desc("count"),
    cl::sub(ConcolicCmd));

//===----------------------------------------------------------------------===//
// JSON output option (shared)
//===----------------------------------------------------------------------===//

cl::opt<bool>
    JsonOutput("json", cl::desc("Output as JSON"), cl::sub(InfoCmd),
               cl::sub(IdentifyCmd), cl::sub(FuncsCmd), cl::sub(DisasmCmd),
               cl::sub(CfgCmd), cl::sub(HexCmd), cl::sub(StringsCmd),
               cl::sub(XrefsCmd), cl::sub(ImportsCmd), cl::sub(ExportsCmd),
               cl::sub(SegmentsCmd), cl::sub(PluginsCmd), cl::sub(BookmarksCmd),
               cl::sub(AnnotateCmd), cl::sub(CallGraphCmd), cl::sub(RenameCmd),
               cl::sub(FunctionEditsCmd), cl::sub(ItemsCmd),
               cl::sub(OperandsCmd), cl::sub(SearchCmd), cl::sub(SectionsCmd),
               cl::sub(SymbolsCmd), cl::sub(RelocsCmd), cl::sub(HeadersCmd),
               cl::sub(EntryPointsCmd), cl::sub(SwitchesCmd),
               cl::sub(DashboardCmd), cl::sub(SigsCmd), cl::sub(AuditCmd),
               cl::sub(HuntCmd), cl::sub(MobileCmd), cl::sub(DecompileCmd));

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
    cl::values(
        clEnumValN(FmtDecompile, "decompile",
                   "Decompiled C (HighC; add --llvm for LLVM-to-C)"),
        clEnumValN(FmtIR, "ir", "LLVM IR"),
        clEnumValN(FmtFuncs, "funcs", "Function list (JSON)"),
        clEnumValN(FmtImports, "imports", "Import table (JSON)"),
        clEnumValN(FmtExports, "exports", "Export table (JSON)"),
        clEnumValN(FmtStrings, "strings", "String table (JSON)"),
        clEnumValN(FmtObjCMethods, "objc-methods",
                   "Mach-O native source and Objective-C methods (JSON)"),
        clEnumValN(
            FmtObjCMethodsSummary, "objc-methods-summary",
            "Objective-C coverage and diagnostics without source text (JSON)"),
        clEnumValN(FmtSwiftMethods, "swift-methods",
                   "Mach-O Swift method source and coverage (JSON)")),
    cl::sub(ExportCmd));

cl::opt<std::string> ExportSourceSignatures(
    "source-signatures", cl::desc("Structured Swift source signature JSON"),
    cl::value_desc("file"), cl::init(""), cl::sub(ExportCmd));

cl::opt<std::string> ExportOutput("o", cl::desc("Output file path"),
                                  cl::Required, cl::sub(ExportCmd));

cl::opt<std::string>
    ExportFunc("func", cl::desc("Function name or address (for decompile/ir)"),
               cl::init(""), cl::sub(ExportCmd), cl::sub(DecompileCmd));

//===----------------------------------------------------------------------===//
// Rename-specific options
//===----------------------------------------------------------------------===//

cl::opt<std::string> RenameFrom("func", cl::desc("Function name to rename"),
                                cl::init(""), cl::sub(RenameCmd));

cl::opt<std::string> RenameTo("to", cl::desc("New name"), cl::init(""),
                              cl::sub(RenameCmd));

cl::opt<std::string>
    RenameAddr("addr",
               cl::desc("Address (hex) to name: a function, data or a label"),
               cl::init(""), cl::value_desc("address"), cl::sub(RenameCmd));

cl::opt<bool> RenameClear("clear",
                          cl::desc("Take the user's name away from --addr"),
                          cl::sub(RenameCmd));

cl::opt<bool> RenameList("list", cl::desc("List all renames"),
                         cl::sub(RenameCmd));

//===----------------------------------------------------------------------===//
// Function edit options
//===----------------------------------------------------------------------===//

cl::opt<std::string>
    FunctionCreate("create",
                   cl::desc("Start a function at this address (hex), or "
                            "restore the deleted function there"),
                   cl::init(""), cl::value_desc("address"),
                   cl::sub(FunctionEditsCmd));

cl::opt<std::string>
    FunctionDelete("delete",
                   cl::desc("Stop treating the function at this address (hex) "
                            "as one"),
                   cl::init(""), cl::value_desc("address"),
                   cl::sub(FunctionEditsCmd));

cl::opt<bool> FunctionEditsList("list",
                                cl::desc("List the function edits (default)"),
                                cl::sub(FunctionEditsCmd));

//===----------------------------------------------------------------------===//
// Data item options
//===----------------------------------------------------------------------===//

cl::opt<std::string>
    ItemData("data",
             cl::desc("Make the bytes at this address (hex) a value of "
                      "--size bytes: 1, 2, 4 or 8"),
             cl::init(""), cl::value_desc("address"), cl::sub(ItemsCmd));
cl::opt<std::string>
    ItemString("string",
               cl::desc("Make the string that starts at this address (hex) "
                        "an item"),
               cl::init(""), cl::value_desc("address"), cl::sub(ItemsCmd));
cl::opt<std::string>
    ItemUndefine("undefine",
                 cl::desc("Show --size bytes at this address (hex) as bytes"),
                 cl::init(""), cl::value_desc("address"), cl::sub(ItemsCmd));
cl::opt<std::string>
    ItemClear("clear", cl::desc("Forget the user's item at this address (hex)"),
              cl::init(""), cl::value_desc("address"), cl::sub(ItemsCmd));
cl::opt<unsigned> ItemSize("size",
                           cl::desc("Bytes of a value or of undefined bytes"),
                           cl::init(1), cl::sub(ItemsCmd));
cl::opt<std::string> OperandAddr("addr",
                                 cl::desc("Address (hex) of the instruction"),
                                 cl::init(""), cl::value_desc("address"),
                                 cl::sub(OperandsCmd));
cl::opt<unsigned> OperandIndex("operand",
                               cl::desc("Operand of the instruction, from 0"),
                               cl::init(0), cl::sub(OperandsCmd));
cl::opt<std::string>
    OperandBase("base",
                cl::desc("number, hex, decimal, binary, char or offset"),
                cl::init("number"), cl::sub(OperandsCmd));
cl::opt<bool> OperandNegate("negate", cl::desc("Change the number's sign"),
                            cl::sub(OperandsCmd));
cl::opt<bool> OperandInvert("invert", cl::desc("Invert the number's bits"),
                            cl::sub(OperandsCmd));
cl::opt<bool> OperandClear("clear", cl::desc("Forget the operand's format"),
                           cl::sub(OperandsCmd));
cl::opt<std::string>
    ItemEncoding("encoding",
                 cl::desc("The encoding --string reads, as `neverd strings` "
                          "names encodings"),
                 cl::init(""), cl::sub(ItemsCmd));

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

OptionalStringList PatchSanitize(
    "sanitize",
    cl::desc("Patch the loaded binary with strict runtime bounds guards"),
    cl::ZeroOrMore, cl::ValueOptional,
    cl::value_desc(PatchSanitizeRequiredValue), cl::sub(PatchCmd));

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
                            cl::desc("Directory containing .pat files"),
                            cl::init(""), cl::sub(SigsCmd), cl::sub(AuditCmd),
                            cl::sub(HuntCmd));

cl::opt<std::string>
    SigFile("sig-file",
            cl::desc("Byte signature (.pat) or library feature pack (.json)"),
            cl::init(""), cl::sub(SigsCmd), cl::sub(AuditCmd), cl::sub(HuntCmd),
            cl::sub(DecompileCmd), cl::sub(FuncsCmd));

cl::opt<bool>
    SigAuto("auto", cl::desc("Auto-detect arch/format and load matching sigs"),
            cl::sub(SigsCmd), cl::sub(AuditCmd), cl::sub(HuntCmd),
            cl::sub(DecompileCmd), cl::sub(FuncsCmd));

cl::opt<std::string>
    SigBase("sig-base",
            cl::desc("Signature tree to select from as --auto does (default: "
                     "signatures/ beside neverd, then ./signatures)"),
            cl::init(""), cl::sub(SigsCmd), cl::sub(AuditCmd), cl::sub(HuntCmd),
            cl::sub(DecompileCmd), cl::sub(FuncsCmd));

//===----------------------------------------------------------------------===//
// Simplify-specific options
//===----------------------------------------------------------------------===//

cl::opt<std::string> SimplifyExpr(cl::Positional, cl::desc("<expression>"),
                                  cl::init(""), cl::sub(SimplifyCmd));

cl::opt<std::string> SimplifyFile(
    "f", cl::desc("Read one expression per line from a file, or '-' for stdin"),
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

cl::opt<unsigned> SimplifyMaxAtoms(
    "max-atoms",
    cl::desc(
        "Most distinct inputs one measurement may span; the cost is 2^n, "
        "so this is the dial between reach and time (0 keeps the default)"),
    cl::init(0), cl::sub(SimplifyCmd));

cl::opt<unsigned long long> SimplifyMaxWork(
    "max-work",
    cl::desc("Work budget for the layered walk and combinatorial polynomial "
             "search (0 keeps the default)"),
    cl::init(0), cl::sub(SimplifyCmd));

cl::opt<bool> SimplifyExhaustive(
    "exhaustive",
    cl::desc(
        "Remove parser and active simplification/search resource ceilings"),
    cl::sub(SimplifyCmd));

cl::opt<unsigned> SimplifyVerifySamples(
    "verify-samples",
    cl::desc("Random assignments each rewrite is checked against before it is "
             "returned (0 keeps the default)"),
    cl::init(0), cl::sub(SimplifyCmd));

cl::opt<bool> SimplifyAllowGrowth(
    "allow-growth",
    cl::desc("Return a rewrite even when it reads worse than what it "
             "replaces; for measuring the engine, not for using it"),
    cl::sub(SimplifyCmd));

cl::opt<bool> SimplifyStats(
    "stats",
    cl::desc("Print the work each expression cost and why any was left alone"),
    cl::sub(SimplifyCmd));

cl::opt<bool> SimplifyJson("json", cl::desc("Output as JSON"),
                           cl::sub(SimplifyCmd));

cl::opt<bool> SimplifySynthesize(
    "synthesize",
    cl::desc(
        "Search for a shorter expression and require an equivalence proof"),
    cl::sub(SimplifyCmd));

cl::opt<unsigned long long>
    SimplifyMaxCost("max-cost",
                    cl::desc("Largest synthesis candidate in grammar nodes"),
                    cl::init(0), cl::sub(SimplifyCmd));

cl::opt<unsigned long long> SimplifyMaxSamples(
    "max-samples", cl::desc("Discovery samples used to distinguish candidates"),
    cl::init(0), cl::sub(SimplifyCmd));

cl::opt<unsigned>
    SimplifyMaxLeaves("max-leaves",
                      cl::desc("Maximum independent synthesis leaves"),
                      cl::init(0), cl::sub(SimplifyCmd));

cl::opt<unsigned> SimplifyMaxConstants("max-constants",
                                       cl::desc("Maximum synthesis literals"),
                                       cl::init(0), cl::sub(SimplifyCmd));

cl::opt<unsigned>
    SimplifyStochasticSlots("stochastic-slots",
                            cl::desc("Straight-line stochastic search slots"),
                            cl::init(0), cl::sub(SimplifyCmd));

cl::opt<unsigned> SimplifyStochasticRestarts(
    "stochastic-restarts", cl::desc("Independent stochastic search attempts"),
    cl::init(0), cl::sub(SimplifyCmd));

cl::opt<unsigned long long>
    SimplifyStochasticIterations("stochastic-iterations",
                                 cl::desc("Mutations per stochastic restart"),
                                 cl::init(0), cl::sub(SimplifyCmd));

cl::opt<unsigned long long>
    SimplifySolverMaxConflicts("solver-max-conflicts",
                               cl::desc("SAT conflict budget"), cl::init(0),
                               cl::sub(SimplifyCmd));

cl::opt<unsigned long long>
    SimplifySolverMaxPropagations("solver-max-propagations",
                                  cl::desc("SAT propagation budget"),
                                  cl::init(0), cl::sub(SimplifyCmd));

cl::opt<unsigned long long>
    SimplifySolverMaxWatchVisits("solver-max-watch-visits",
                                 cl::desc("SAT watched-clause visit budget"),
                                 cl::init(0), cl::sub(SimplifyCmd));

cl::opt<std::string> SimplifySolver(
    "solver", cl::desc("Proof backend: builtin or z3 (optional build feature)"),
    cl::init("builtin"), cl::sub(SimplifyCmd));

cl::opt<unsigned>
    SimplifySolverTimeoutMs("solver-timeout-ms",
                            cl::desc("Z3 per-check timeout (default 1000 ms)"),
                            cl::init(0), cl::sub(SimplifyCmd));

//===----------------------------------------------------------------------===//
// Optimize-IR-specific options
//===----------------------------------------------------------------------===//

cl::opt<std::string> OptimizeIRInput(cl::Positional, cl::desc("<llvm-ir>"),
                                     cl::Required, cl::sub(OptimizeIRCmd));

cl::opt<std::string>
    OptimizeIROutput("o", cl::desc("Write committed LLVM IR to this file"),
                     cl::init(""), cl::sub(OptimizeIRCmd));

cl::opt<neverd_optimization_mode_t> OptimizeIRMode(
    "mode", cl::desc("Optimization pipeline mode"),
    cl::values(
        clEnumValN(NEVERD_OPTIMIZATION_MODE_CONSERVATIVE, "conservative",
                   "Standard LLVM optimization without semantic rewriting"),
        clEnumValN(NEVERD_OPTIMIZATION_MODE_THIN, "thin",
                   "Thin standard LLVM pipeline with semantic convergence"),
        clEnumValN(NEVERD_OPTIMIZATION_MODE_DEEP, "deep",
                   "Deep standard LLVM pipeline with semantic convergence")),
    cl::init(NEVERD_OPTIMIZATION_MODE_DEEP), cl::sub(OptimizeIRCmd));

cl::opt<neverd_llvm_optimization_level_t> OptimizeIRLevel(
    "llvm-level", cl::desc("Standard LLVM optimization level"),
    cl::values(clEnumValN(NEVERD_LLVM_OPTIMIZATION_O0, "O0", "LLVM O0"),
               clEnumValN(NEVERD_LLVM_OPTIMIZATION_O1, "O1", "LLVM O1"),
               clEnumValN(NEVERD_LLVM_OPTIMIZATION_O2, "O2", "LLVM O2"),
               clEnumValN(NEVERD_LLVM_OPTIMIZATION_O3, "O3", "LLVM O3")),
    cl::init(NEVERD_LLVM_OPTIMIZATION_O2), cl::sub(OptimizeIRCmd));

cl::opt<unsigned>
    OptimizeIRMaxRounds("max-rounds",
                        cl::desc("Semantic rounds per pass invocation"),
                        cl::init(0), cl::sub(OptimizeIRCmd));

cl::opt<bool>
    OptimizeIRSynthesize("synthesize",
                         cl::desc("Enable proof-gated expression synthesis"),
                         cl::sub(OptimizeIRCmd));

cl::opt<unsigned long long> OptimizeIRSynthesisMaxCost(
    "synthesis-max-cost",
    cl::desc("Largest synthesis candidate in grammar nodes"), cl::init(0),
    cl::sub(OptimizeIRCmd));
cl::opt<unsigned long long>
    OptimizeIRSynthesisMaxSamples("synthesis-max-samples",
                                  cl::desc("Synthesis discovery sample count"),
                                  cl::init(0), cl::sub(OptimizeIRCmd));
cl::opt<unsigned> OptimizeIRSynthesisVerifySamples(
    "synthesis-verify-samples", cl::desc("Synthesis pre-proof sample count"),
    cl::init(0), cl::sub(OptimizeIRCmd));
cl::opt<unsigned long long>
    OptimizeIRSynthesisMaxWork("synthesis-max-work",
                               cl::desc("Synthesis search-work budget"),
                               cl::init(0), cl::sub(OptimizeIRCmd));
cl::opt<unsigned> OptimizeIRSynthesisMaxLeaves(
    "synthesis-max-leaves", cl::desc("Maximum independent synthesis leaves"),
    cl::init(0), cl::sub(OptimizeIRCmd));
cl::opt<unsigned>
    OptimizeIRSynthesisMaxConstants("synthesis-max-constants",
                                    cl::desc("Maximum synthesis literals"),
                                    cl::init(0), cl::sub(OptimizeIRCmd));
cl::opt<unsigned>
    OptimizeIRSynthesisStochasticSlots("synthesis-stochastic-slots",
                                       cl::desc("Stochastic search slots"),
                                       cl::init(0), cl::sub(OptimizeIRCmd));
cl::opt<unsigned> OptimizeIRSynthesisStochasticRestarts(
    "synthesis-stochastic-restarts", cl::desc("Stochastic search restarts"),
    cl::init(0), cl::sub(OptimizeIRCmd));
cl::opt<unsigned long long> OptimizeIRSynthesisStochasticIterations(
    "synthesis-stochastic-iterations", cl::desc("Mutations per restart"),
    cl::init(0), cl::sub(OptimizeIRCmd));
cl::opt<unsigned long long>
    OptimizeIRSolverMaxConflicts("solver-max-conflicts",
                                 cl::desc("SAT conflict budget"), cl::init(0),
                                 cl::sub(OptimizeIRCmd));
cl::opt<unsigned long long>
    OptimizeIRSolverMaxPropagations("solver-max-propagations",
                                    cl::desc("SAT propagation budget"),
                                    cl::init(0), cl::sub(OptimizeIRCmd));
cl::opt<unsigned long long>
    OptimizeIRSolverMaxWatchVisits("solver-max-watch-visits",
                                   cl::desc("SAT watched-clause visit budget"),
                                   cl::init(0), cl::sub(OptimizeIRCmd));
cl::opt<bool> OptimizeIRExhaustive(
    "exhaustive", cl::desc("Remove convergence, search, and solver budgets"),
    cl::sub(OptimizeIRCmd));
cl::opt<bool> OptimizeIRJson("json",
                             cl::desc("Output result and telemetry as JSON"),
                             cl::sub(OptimizeIRCmd));

//===----------------------------------------------------------------------===//
// Driver-emulation-specific options
//===----------------------------------------------------------------------===//

cl::opt<std::string> EmulateDriverInput(cl::Positional,
                                        cl::desc("<driver.sys>"), cl::Required,
                                        cl::sub(EmulateDriverCmd));
cl::opt<unsigned long long> DriverInstructionLimit(
    "instruction-limit",
    cl::desc("Maximum guest instructions (must be positive)"),
    cl::init(emulation::profile::DefaultInstructionLimit),
    cl::sub(EmulateDriverCmd));
cl::opt<std::string> DriverBackend(execution_cli::BackendOption,
                                   cl::desc(execution_cli::BackendHelp),
                                   cl::init(emulation::execution::Auto),
                                   cl::sub(EmulateDriverCmd));
cl::opt<std::string> DriverExecutionContract(
    execution_cli::ContractOption, cl::desc(execution_cli::ContractHelp),
    cl::init(emulation::execution::Legacy), cl::sub(EmulateDriverCmd));
cl::opt<std::string> DriverScenarioFile(
    "scenario", cl::desc("Strict driver lifecycle scenario JSON file"),
    cl::value_desc("path"), cl::init(""), cl::sub(EmulateDriverCmd));

cl::opt<std::string>
    CPUConfiguration(execution_cli::ConfigurationOption,
                     cl::desc(execution_cli::ConfigurationHelp),
                     cl::value_desc(execution_cli::ConfigurationValue),
                     cl::init(emulation::execution_report::EmptyConfiguration),
                     cl::sub(CPUCapabilitiesCmd));
cl::opt<bool> CPUProbeHost(execution_cli::ProbeHostOption,
                           cl::desc(execution_cli::ProbeHostHelp),
                           cl::init(false), cl::sub(CPUCapabilitiesCmd));

cl::opt<std::string> ProcessInput(cl::Positional, cl::desc(process_cli::Input),
                                  cl::Required, cl::sub(EmulateProcessCmd));
cl::opt<std::string> ProcessProfile(process_cli::ProfileOption,
                                    cl::desc(process_cli::ProfileHelp),
                                    cl::value_desc(process_cli::ProfileValue),
                                    cl::Required, cl::sub(EmulateProcessCmd));
cl::opt<std::string>
    ProcessOptions(process_cli::OptionsOption,
                   cl::desc(process_cli::OptionsHelp),
                   cl::value_desc(process_cli::OptionsValue),
                   cl::init(emulation::process_report::EmptyOptions),
                   cl::sub(EmulateProcessCmd));
cl::opt<std::string> UnpackInput(cl::Positional, cl::desc(unpack_cli::Input),
                                 cl::Required, cl::sub(UnpackCmd));
cl::opt<std::string> UnpackOutput(unpack_cli::OutputOption,
                                  cl::desc(unpack_cli::OutputHelp),
                                  cl::value_desc(unpack_cli::OutputValue),
                                  cl::Required, cl::sub(UnpackCmd));
cl::opt<std::string> UnpackOptions(unpack_cli::OptionsOption,
                                   cl::desc(unpack_cli::OptionsHelp),
                                   cl::value_desc(unpack_cli::OptionsValue),
                                   cl::init(unpack::strings::EmptyOptions),
                                   cl::sub(UnpackCmd));

//===----------------------------------------------------------------------===//
// Translate-object-specific options
//===----------------------------------------------------------------------===//

cl::opt<std::string> TranslateObjectInput(cl::Positional,
                                          cl::desc("<exact-raw-x86-64-block>"),
                                          cl::Required,
                                          cl::sub(TranslateObjectCmd));

cl::opt<std::string> TranslateObjectOutput(
    "o", cl::desc("Write the AArch64 relocatable object to this file"),
    cl::value_desc("path"), cl::Required, cl::sub(TranslateObjectCmd));

cl::opt<TranslateObjectContainer> TranslateObjectFormat(
    "format", cl::desc("AArch64 object container"),
    cl::values(clEnumValN(TranslateObjectContainer::ELF, "elf", "ELF"),
               clEnumValN(TranslateObjectContainer::MachO, "macho", "Mach-O")),
    cl::init(TranslateObjectContainer::ELF), cl::sub(TranslateObjectCmd));

cl::opt<std::string> TranslateObjectEntry(
    "entry", cl::desc("Guest block entry address (hexadecimal)"),
    cl::value_desc("address"), cl::init("0"), cl::sub(TranslateObjectCmd));

cl::opt<unsigned long long> TranslateObjectGeneration(
    "generation",
    cl::desc("Executable-memory generation in the cache identity"),
    cl::value_desc("number"), cl::init(0), cl::sub(TranslateObjectCmd));

} // namespace neverd::cli
