//===- NeverDCmdBytePatch.cpp - Interactive byte patch CLI ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Keep byte overlays separate from whole-function source replacement.
//===----------------------------------------------------------------------===//
#include "../NeverDCLI.h"

#include "neverd/sdk/NeverDCAPIBytePatch.h"

#include "llvm/Support/WithColor.h"
using namespace llvm;
namespace neverd::cli {
namespace {
cl::opt<std::string>
    Address("address",
            cl::desc("Patch address (hexadecimal, optional 0x prefix)"),
            cl::sub(PatchBytesCmd));
cl::opt<std::string> Kind(
    "kind",
    cl::desc("bytes, assemble, nop, fill_nops, branch_always/never/invert, "
             "skip_call, skip_call_value, constant"),
    cl::init("bytes"), cl::sub(PatchBytesCmd));
cl::opt<std::string> Bytes("bytes", cl::desc("Replacement hex pairs"),
                           cl::sub(PatchBytesCmd));
cl::opt<std::string> Assembly("assemble",
                              cl::desc("One instruction at --address"),
                              cl::sub(PatchBytesCmd));
cl::opt<std::string>
    Symbols("symbols",
            cl::desc("Explicit assembly name:signed-offset bindings as JSON"),
            cl::sub(PatchBytesCmd));
cl::opt<bool> FillNops("fill-nops",
                       cl::desc("Pad assembly to whole instruction boundaries"),
                       cl::init(true), cl::sub(PatchBytesCmd));
cl::opt<std::string>
    Value("value",
          cl::desc("Unsigned immediate or return value (decimal or 0x hex)"),
          cl::init("0"), cl::sub(PatchBytesCmd));
cl::opt<uint64_t> Size("size",
                       cl::desc("Whole instruction range, or revert range"),
                       cl::init(0), cl::sub(PatchBytesCmd));
cl::opt<bool> Apply("apply", cl::desc("Commit and save the previewed patch"),
                    cl::sub(PatchBytesCmd));
cl::opt<bool> Revert("revert", cl::desc("Restore original bytes at --address"),
                     cl::sub(PatchBytesCmd));
cl::opt<bool> Export("export",
                     cl::desc("Replay saved patches to -o (exclusive)"),
                     cl::sub(PatchBytesCmd));
cl::opt<bool> Unsigned(
    "allow-unsigned",
    cl::desc("Explicitly remove invalidated signature metadata on export"),
    cl::sub(PatchBytesCmd));
cl::opt<bool>
    Overlap("allow-overlap",
            cl::desc("Allow assembly to replace following whole instructions"),
            cl::sub(PatchBytesCmd));
cl::opt<bool> ATT("att", cl::desc("Use AT&T syntax"), cl::sub(PatchBytesCmd));
} // namespace
int runPatchBytes(neverd_session_t Sess) {
  const auto Fail = [&] {
    const auto *Message = neverd_last_error(Sess);
    WithColor::error() << (Message ? Message : "byte patch failed") << '\n';
    neverd_free_string(Message);
    return 1;
  };
  if (unsigned(Apply) + unsigned(Revert) + unsigned(Export) > 1) {
    WithColor::error()
        << "select at most one of --apply, --revert or --export\n";
    return 1;
  }
  if (Export) {
    if (OutputFile.empty()) {
      WithColor::error() << "--export requires -o\n";
      return 1;
    }
    if (neverd_byte_patches_export(Sess, OutputFile.c_str(), Unsigned) != 0)
      return Fail();
    outs() << "Exported " << OutputFile << '\n';
    return 0;
  }
  if (Address.empty()) {
    if (Apply || Revert || !Bytes.empty() || !Assembly.empty()) {
      WithColor::error() << "patch operation requires --address\n";
      return 1;
    }
    const char *Rows = neverd_byte_patches_json(Sess);
    if (!Rows)
      return Fail();
    outs() << Rows << '\n';
    neverd_free_string(Rows);
    return 0;
  }
  const auto At = parseAddrArg(Address);
  auto Number = StringRef(Value);
  const unsigned Radix =
      Number.consume_front("0x") || Number.consume_front("0X") ? 16 : 10;
  uint64_t V = 0;
  if (!At || Number.empty() || Number.starts_with("-") ||
      Number.getAsInteger(Radix, V)) {
    WithColor::error() << "invalid patch address or value\n";
    return 1;
  }
  if (Revert) {
    if (neverd_byte_patch_revert(Sess, *At, Size) != 0 ||
        neverd_byte_patches_save(Sess) != 0)
      return Fail();
    return 0;
  }
  neverd_byte_patch_options_v1 O{};
  O.struct_size = sizeof(O);
  O.address = *At;
  O.kind = Assembly.empty() ? Kind.c_str() : "assemble";
  O.bytes = Bytes.c_str();
  O.text = Assembly.c_str();
  O.value = V;
  O.size = Size;
  O.symbols = Symbols.c_str();
  O.fill_nops = FillNops;
  O.allow_overlap = Overlap;
  O.dialect = ATT;
  const char *Result = Apply ? neverd_byte_patch_apply(Sess, &O)
                             : neverd_byte_patch_preview(Sess, &O);
  if (!Result)
    return Fail();
  const std::string Text(Result);
  neverd_free_string(Result);
  if (Apply && neverd_byte_patches_save(Sess) != 0)
    return Fail();
  outs() << Text << '\n';
  return 0;
}
} // namespace neverd::cli
