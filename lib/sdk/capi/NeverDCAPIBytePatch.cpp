//===- NeverDCAPIBytePatch.cpp - Byte patch SDK ---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Adapt public requests to the shared patch owner and session lifetime.
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDCAPIBytePatch.h"

#include "SessionImpl.h"

#include "neverd/decode/Decoder.h"
#include "neverd/patch/Limits.h"
#include "neverd/patch/PatchActions.h"
#include "neverd/support/FilePath.h"
#include "neverd/support/ProjectFiles.h"
#include "neverd/support/ProjectWriteLock.h"
#include "neverd/support/SHA256.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>

using namespace neverd;
using namespace neverd::sdk;
namespace patchLimits = neverd::limits::patch;

namespace {
llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::make_error<llvm::StringError>(Message,
                                             llvm::inconvertibleErrorCode());
}
llvm::Expected<llvm::StringRef> bounded(const char *Text, size_t Limit) {
  if (!Text)
    return llvm::StringRef();
  size_t N = 0;
  while (N <= Limit && Text[N])
    ++N;
  if (N > Limit)
    return invalid("patch text exceeds its size limit");
  return llvm::StringRef(Text, N);
}
llvm::Expected<std::map<std::string, int64_t>> symbols(const char *Text) {
  auto Bounded = bounded(Text, patchLimits::kMaxAssemblyTextBytes);
  if (!Bounded)
    return Bounded.takeError();
  std::map<std::string, int64_t> Result;
  if (Bounded->empty())
    return Result;
  auto Parsed = llvm::json::parse(*Bounded);
  if (!Parsed)
    return Parsed.takeError();
  const auto *Object = Parsed->getAsObject();
  if (!Object || Object->size() > patchLimits::kMaxAssemblySymbols)
    return invalid(
        "assembly symbols must be a bounded object of signed integers");
  for (const auto &[Name, Value] : *Object) {
    auto Integer = Value.getAsInteger();
    if (!Integer)
      return invalid("assembly symbol value must be a signed integer");
    Result.emplace(Name.str(), *Integer);
  }
  return Result;
}
bool ready(Session *S) {
  if (!S)
    return false;
  S->clearError();
  if (!S->Loaded || !S->requireFileBacked()) {
    S->setError("byte patches require a loaded file-backed session");
    return false;
  }
  if (!S->PatchInput) {
    auto Input = patch::InputFile::open(S->FilePath, S->Img);
    if (!Input) {
      S->setError(llvm::toString(Input.takeError()));
      return false;
    }
    S->PatchInput = std::move(*Input);
  }
  return true;
}
void changed(Session &S) {
  S.retireStaleDiscovery();
  S.DiscoveredFunctions.reset();
  S.ExceptionHandlersSettled = false;
  S.ProgramMainFound.reset();
  S.ImageStrings.reset();
  S.ImmediateIndex = {};
  S.invalidatePipeline();
}
llvm::Expected<patch::PatchRequest>
request(const neverd_byte_patch_options_v1 *O) {
  if (!O || O->struct_size < sizeof(*O))
    return invalid("truncated byte patch options");
  auto Kind = bounded(O->kind, patchLimits::kMaxAssemblySymbolBytes);
  auto Bytes = bounded(O->bytes, patchLimits::kMaxApplyBytes * 2);
  auto Text = bounded(O->text, patchLimits::kMaxAssemblyTextBytes);
  if (!Kind)
    return Kind.takeError();
  if (!Bytes)
    return Bytes.takeError();
  if (!Text)
    return Text.takeError();
  auto K = patch::parsePatchKind(*Kind);
  if (!K || O->dialect < 0 || O->dialect > 1 ||
      (O->fill_nops != 0 && O->fill_nops != 1) ||
      (O->allow_overlap != 0 && O->allow_overlap != 1))
    return invalid("invalid patch kind, dialect or flags");
  auto B = patch::parseHex(*Bytes, patchLimits::kMaxApplyBytes);
  if (!B)
    return B.takeError();
  auto Symbols = symbols(O->symbols);
  if (!Symbols)
    return Symbols.takeError();
  patch::PatchRequest R;
  R.Symbols = std::move(*Symbols);
  R.Address = O->address;
  R.Kind = *K;
  R.Bytes = std::move(*B);
  R.Text = Text->str();
  R.Value = O->value;
  R.Size = O->size;
  R.FillNops = O->fill_nops;
  R.AllowOverlap = O->allow_overlap;
  R.ATT = O->dialect;
  return R;
}
const char *edit(neverd_session_t Sess, const neverd_byte_patch_options_v1 *O,
                 bool Apply) {
  auto *S = toSession(Sess);
  if (!ready(S))
    return nullptr;
  auto R = request(O);
  if (!R) {
    S->setError(llvm::toString(R.takeError()));
    return nullptr;
  }
  auto Plan = patch::planPatch(S->Img, *S->PatchInput, S->BytePatches, *R);
  if (!Plan) {
    S->setError(llvm::toString(Plan.takeError()));
    return nullptr;
  }
  if (Apply && O->expected) {
    auto Text = bounded(O->expected, patchLimits::kMaxApplyBytes * 2);
    if (!Text) {
      S->setError(llvm::toString(Text.takeError()));
      return nullptr;
    }
    auto Expected = patch::parseHex(*Text, patchLimits::kMaxApplyBytes);
    if (!Expected) {
      S->setError(llvm::toString(Expected.takeError()));
      return nullptr;
    }
    if (*Expected != Plan->Before) {
      S->setError("patch preview is stale; preview the current bytes again");
      return nullptr;
    }
  }
  // Prepare the returned value before publishing any changes.
  const auto Result = jsonToString(Plan->json());
  if (Apply && Plan->Before != Plan->After) {
    if (auto E = S->BytePatches.apply(S->Img, *S->PatchInput, R->Address,
                                      Plan->After, R->Kind)) {
      S->setError(llvm::toString(std::move(E)));
      return nullptr;
    }
    changed(*S);
  }
  return dupStr(Result);
}
} // namespace

const char *neverd_byte_patch_preview(neverd_session_t S,
                                      const neverd_byte_patch_options_v1 *O) {
  return edit(S, O, false);
}
const char *neverd_byte_patch_apply(neverd_session_t S,
                                    const neverd_byte_patch_options_v1 *O) {
  return edit(S, O, true);
}
int neverd_byte_patch_revert(neverd_session_t Sess, neverd_va_t Address,
                             uint64_t Size) {
  auto *S = toSession(Sess);
  if (!ready(S))
    return -1;
  auto N = S->BytePatches.revert(S->Img, Address, Size);
  if (!N) {
    S->setError(llvm::toString(N.takeError()));
    return -1;
  }
  if (*N)
    changed(*S);
  return 0;
}
const char *neverd_byte_patches_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded)
    return nullptr;
  return dupStr(jsonToString(patch::patchRowsJSON(S->BytePatches)));
}
const char *neverd_byte_patch_ranges_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded)
    return nullptr;
  llvm::json::Array Ranges;
  for (const auto &[Offset, Row] : S->BytePatches.rows())
    Ranges.push_back(llvm::json::Object{
        {"address", "0x" + llvm::utohexstr(Row.Address, true)},
        {"size", Row.size()}});
  return dupStr(jsonToString(std::move(Ranges)));
}
const char *neverd_byte_patch_context_json(neverd_session_t Sess,
                                           neverd_va_t Address) {
  auto *S = toSession(Sess);
  if (!ready(S))
    return nullptr;
  llvm::json::Array Actions;
  llvm::json::Object Result{{"address", "0x" + llvm::utohexstr(Address, true)}};
  const auto *Seg = S->Img.getSegmentFor(Address);
  if (!Seg || Address - Seg->VA >= Seg->Data.size()) {
    S->setError("address has no mapped bytes");
    return nullptr;
  }
  patch::PatchRequest R;
  R.Address = Address;
  R.Bytes = {Seg->Data[Address - Seg->VA]};
  R.Kind = patch::PatchKind::Bytes;
  uint64_t InstructionSize = 1;
  if (auto P = patch::planPatch(S->Img, *S->PatchInput, S->BytePatches, R)) {
    Actions.push_back("bytes");
    Result["bytes"] = llvm::toHex(P->Before, true);
  } else
    llvm::consumeError(P.takeError());
  for (auto K : {patch::PatchKind::Nop, patch::PatchKind::BranchAlways,
                 patch::PatchKind::BranchNever, patch::PatchKind::BranchInvert,
                 patch::PatchKind::SkipCall, patch::PatchKind::SkipCallValue,
                 patch::PatchKind::Constant}) {
    R.Kind = K;
    if (auto P = patch::planPatch(S->Img, *S->PatchInput, S->BytePatches, R)) {
      Actions.push_back(patch::patchKindName(K));
      if (K == patch::PatchKind::Nop) {
        InstructionSize = P->Before.size();
        Actions.push_back("assemble");
        Result["bytes"] = llvm::toHex(P->Before, true);
        if (!P->Covers.empty())
          Result["text"] = P->Covers.front()
                               .getAsObject()
                               ->getString("text")
                               .value_or("")
                               .str();
      }
    } else
      llvm::consumeError(P.takeError());
  }
  llvm::json::Array RevertRanges;
  for (const auto *Row : S->BytePatches.rowsIn(Address, InstructionSize))
    RevertRanges.push_back(llvm::json::Object{
        {"address", "0x" + llvm::utohexstr(Row->Address, true)},
        {"size", Row->size()}});
  if (!RevertRanges.empty())
    Actions.push_back("revert");
  Result["revert_ranges"] = std::move(RevertRanges);
  Result["actions"] = std::move(Actions);
  return dupStr(jsonToString(std::move(Result)));
}
static llvm::Expected<std::optional<std::array<uint8_t, 32>>>
patchDigest(Session &S) {
  const auto Path =
      project_files::path(S.projectInputPath(), project_files::File::Patches);
  std::error_code EC;
  const bool Exists = std::filesystem::exists(Path, EC);
  if (EC)
    return llvm::errorCodeToError(EC);
  if (!Exists)
    return std::optional<std::array<uint8_t, 32>>{};
  const auto Size = std::filesystem::file_size(Path, EC);
  if (EC)
    return llvm::errorCodeToError(EC);
  if (Size > patchLimits::kMaxPatchesFileBytes)
    return invalid("patches.json exceeds its size limit");
  auto Buffer =
      llvm::MemoryBuffer::getFile(pathToUTF8(Path), false, false, true);
  if (!Buffer)
    return llvm::errorCodeToError(Buffer.getError());
  return std::optional(
      sha256(llvm::arrayRefFromStringRef((*Buffer)->getBuffer())));
}
int neverd_byte_patches_save(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!ready(S))
    return -1;
  ProjectWriteLock Lock(S->FilePath, S->projectNamespaceSuffix());
  if (!Lock) {
    S->setError("project writer unavailable: " + Lock.error());
    return -1;
  }
  auto Current = patchDigest(*S);
  if (!Current) {
    S->setError(llvm::toString(Current.takeError()));
    return -1;
  }
  if (*Current != S->BytePatchesSavedDigest) {
    S->setError(
        "patches.json changed outside this session; reload it before saving");
    return -1;
  }
  if (!saveSidecar(S,
                   project_files::path(S->projectInputPath(),
                                       project_files::File::Patches),
                   patch::patchRowsJSON(S->BytePatches), "byte patches"))
    return -1;
  Current = patchDigest(*S);
  if (!Current) {
    S->setError(llvm::toString(Current.takeError()));
    return -1;
  }
  S->BytePatchesSavedDigest = *Current;
  return 0;
}
int neverd_byte_patches_load(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded || !S->requireFileBacked())
    return -1;
  S->clearError();
  const auto Path =
      project_files::path(S->projectInputPath(), project_files::File::Patches);
  std::error_code EC;
  const bool Exists = std::filesystem::exists(Path, EC);
  if (EC) {
    S->setError(EC.message());
    return -1;
  }
  if (!Exists) {
    S->BytePatchesSavedDigest.reset();
    if (!S->BytePatches.empty()) {
      S->BytePatches.clear(S->Img);
      changed(*S);
    }
    return 0;
  }
  auto Size = std::filesystem::file_size(Path, EC);
  if (EC || Size > patchLimits::kMaxPatchesFileBytes) {
    S->setError("cannot read patches.json within its size limit");
    return -1;
  }
  auto Buffer =
      llvm::MemoryBuffer::getFile(pathToUTF8(Path), false, false, true);
  if (!Buffer) {
    S->setError(Buffer.getError().message());
    return -1;
  }
  auto Rows = patch::parsePatchRows((*Buffer)->getBuffer());
  if (!Rows) {
    S->setError(llvm::toString(Rows.takeError()));
    return -1;
  }
  if (!ready(S))
    return -1;
  const auto Digest =
      sha256(llvm::arrayRefFromStringRef((*Buffer)->getBuffer()));
  const auto &Current = S->BytePatches.rows();
  // A worker commit persists the already-admitted live overlay. Acknowledge
  // those exact rows without clearing/replaying the image or hex-encoding it
  // for comparison. A saved digest alone cannot detect unsaved live edits.
  if (std::equal(Rows->begin(), Rows->end(), Current.begin(), Current.end(),
                 [](const patch::PatchRow &Row, const auto &Entry) {
                   return Row == Entry.second;
                 })) {
    S->BytePatchesSavedDigest = Digest;
    return 0;
  }
  const auto Before = Current;
  if (auto E =
          S->BytePatches.replace(S->Img, *S->PatchInput, std::move(*Rows))) {
    S->setError(llvm::toString(std::move(E)));
    return -1;
  }
  S->BytePatchesSavedDigest = Digest;
  if (Before != S->BytePatches.rows())
    changed(*S);
  return 0;
}
int neverd_byte_patches_export(neverd_session_t Sess, const char *Output,
                               int AllowUnsigned) {
  auto *S = toSession(Sess);
  if (!ready(S))
    return -1;
  if (!Output || !*Output || (AllowUnsigned != 0 && AllowUnsigned != 1)) {
    S->setError("invalid patch export options");
    return -1;
  }
  if (auto E = patch::exportPatches(S->Img, *S->PatchInput, S->BytePatches,
                                    std::filesystem::u8path(Output),
                                    AllowUnsigned)) {
    S->setError(llvm::toString(std::move(E)));
    return -1;
  }
  return 0;
}
const char *neverd_assemble_json(neverd_session_t Sess,
                                 const neverd_assemble_options_v1 *O) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded)
    return nullptr;
  S->clearError();
  if (!O || O->struct_size < sizeof(*O) || O->dialect < 0 || O->dialect > 1 ||
      O->mode < 0 || O->mode > 2) {
    S->setError("invalid assembly options");
    return nullptr;
  }
  auto Mode = S->Img.instructionModeAt(O->pc);
  if (!Mode || (O->mode && *Mode != (O->mode == 1 ? InstructionMode::ARM
                                                  : InstructionMode::Thumb))) {
    S->setError("assembly mode does not match image evidence");
    return nullptr;
  }
  auto Text = bounded(O->text, patchLimits::kMaxAssemblyTextBytes);
  if (!Text) {
    S->setError(llvm::toString(Text.takeError()));
    return nullptr;
  }
  auto Symbols = symbols(O->symbols);
  if (!Symbols) {
    S->setError(llvm::toString(Symbols.takeError()));
    return nullptr;
  }
  patch::AssemblyRequest R;
  R.Symbols = std::move(*Symbols);
  R.Address = O->pc;
  R.Mode = *Mode;
  R.ATT = O->dialect;
  R.Text = Text->str();
  auto Bytes = patch::assemble(S->Img.Arch, R);
  if (!Bytes) {
    S->setError(llvm::toString(Bytes.takeError()));
    return nullptr;
  }
  Decoder Dec;
  DecodedInsn Insn{};
  if (!Dec.init(S->Img) || !Dec.selectMode(S->Img, O->pc) ||
      !Dec.decodeOne(Bytes->data(), Bytes->size(), O->pc, Insn)) {
    S->setError("assembled instruction does not decode in the image mode");
    return nullptr;
  }
  return dupStr(jsonToString(llvm::json::Object{
      {"hex", llvm::toHex(*Bytes, true)},
      {"size", Bytes->size()},
      {"address", "0x" + llvm::utohexstr(O->pc, true)},
      {"text", std::string(Insn.Raw->mnemonic) + " " + Insn.Raw->op_str}}));
}
