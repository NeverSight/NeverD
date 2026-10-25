//===- NeverDCAPISigs.cpp - C API: FLIRT signatures -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// FLIRT signature loading, matching, and result queries.
///
//===----------------------------------------------------------------------===//

#include "JSONText.h"
#include "SessionImpl.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/sigs/SignatureCache.h"
#include "neverd/sigs/SignatureMatcher.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <future>
#include <vector>

using namespace neverd;
using namespace neverd::sdk;

void Session::refreshFunctionNames() {
  auto Names = SigDB.buildNameMap();
  // Deriving the names again from what they were derived from gives the
  // names the list holds.
  if (NamesDerivedFrom && NamesDerivedFrom->Revision == FunctionListRevision &&
      NamesDerivedFrom->List == Functions.data() &&
      NamesDerivedFrom->Count == Functions.size() &&
      NamesDerivedFrom->Renames == Renames &&
      NamesDerivedFrom->Signatures == Names)
    return;
  // The list is in address order, as the map is, so each name lands at or
  // just before where the last one did instead of being searched for.
  auto Next = OriginalNames.begin();
  const std::optional<va_t> Main = programMain();
  for (auto &F : Functions) {
    if (F.LinkageName.empty()) {
      F.LinkageName = F.Name;
      F.LinkageOrigin = F.Origin;
    }
    F.Name = F.LinkageName;
    F.Origin = F.LinkageOrigin;
    if (F.Origin < NameOrigin::Stated) {
      if (auto I = Names.find(F.Entry);
          I != Names.end() && !I->second.empty()) {
        F.Name = I->second;
        F.Origin = NameOrigin::Analysis;
      } else if (F.Entry == Main && isSynthesizedFuncName(F.Name)) {
        // The function the start routine runs as main, as C names it.
        F.Name = "main";
        F.Origin = NameOrigin::Analysis;
      }
    }
    Next = std::next(OriginalNames.insert_or_assign(Next, F.Entry, F.Name));
    if (auto Rename = Renames.find(F.Entry); Rename != Renames.end()) {
      F.Name = Rename->second;
      F.Origin = NameOrigin::User;
    }
  }
  functionListChanged();
  NamesDerivedFrom =
      FunctionNameInputs{FunctionListRevision, Functions.data(),
                         Functions.size(), Renames, std::move(Names)};
}

/// The addresses signatures are tried at: every function the session lists,
/// and, until the pipeline has run and published the functions it found,
/// every entry its detector finds too.  The session lists what the image's
/// own tables state, which in a stripped image is little of its code -- a
/// static musl program states 15 of its routines, and an optimized x86 PE
/// leaves hundreds of its runtime's routines out of every table.  An entry
/// the pipeline would later reject costs nothing here: a name needs a whole
/// signature to match there.
static std::vector<uint64_t> signatureEntries(Session &S) {
  std::vector<uint64_t> Entries;
  Entries.reserve(S.Functions.size());
  for (const auto &F : S.Functions)
    Entries.push_back(F.Entry);
  const bool Native = S.Img.Arch == Arch::X86 || S.Img.Arch == Arch::X64 ||
                      S.Img.Arch == Arch::ARM || S.Img.Arch == Arch::AArch64;
  if (Native && !(S.PipeRan && S.NativeFunctionsSynchronized)) {
    Decoder Dec;
    if (Dec.init(S.Img)) {
      FuncDetector Detector;
      for (const auto &Entry : Detector.detect(S.Img, Dec))
        Entries.push_back(Entry.first);
    }
    std::sort(Entries.begin(), Entries.end());
    Entries.erase(std::unique(Entries.begin(), Entries.end()), Entries.end());
  }
  return Entries;
}

/// signatureEntries for \p S, found while the caller loads signature files:
/// it reads only the image.
static std::future<std::vector<uint64_t>> findSignatureEntries(Session &S) {
  return std::async(std::launch::async, [&S] { return signatureEntries(S); });
}

/// Match the loaded signature set against \p S and report how many hits it
/// produced.
///
/// The personality pass runs after the general one because the general one
/// starts by clearing the match list, and running second is also what puts an
/// adopted name in front of the rename loop below.  It is safe to run
/// unconditionally: it only speaks for an address a frame installs as its
/// personality and the image itself cannot name, so a binary that carries its
/// own symbols is left exactly as it was.
static int matchLoadedSignatures(Session &S,
                                 std::future<std::vector<uint64_t>> Entries) {
  const auto Candidates = Entries.get();
  // Matching can publish image symbols and personality metadata independently
  // of library-feature generations. Retire their borrowed indexes first.
  S.clearPipeline();
  S.SigDB.apply(S.Img, Candidates);
  S.SigDB.identifyPersonalityRoutines(S.Img);

  S.refreshFunctionNames();

  return static_cast<int>(S.SigDB.matches().size());
}

/// Load signature files through the user's signature cache, which the
/// environment may move or turn off; see neverd::sigs::SignatureCache.
static void useSignatureCache(Session &S) {
  S.SigDB.setCache(sigs::SignatureCache::fromEnvironment());
}

int neverd_apply_signatures(neverd_session_t Sess, const char *SigDir) {
  auto *S = static_cast<Session *>(Sess);
  if (!S || !S->Loaded || !SigDir)
    return -1;
  S->clearError();

  auto Entries = findSignatureEntries(*S);
  useSignatureCache(*S);
  auto Err = S->SigDB.loadDirectory(std::filesystem::u8path(SigDir));
  if (Err) {
    S->setError(llvm::toString(std::move(Err)));
    return -1;
  }

  return matchLoadedSignatures(*S, std::move(Entries));
}

int neverd_apply_signature_file(neverd_session_t Sess, const char *SigPath) {
  auto *S = static_cast<Session *>(Sess);
  if (!S || !S->Loaded || !SigPath)
    return -1;
  S->clearError();

  auto Entries = findSignatureEntries(*S);
  useSignatureCache(*S);
  auto Err = S->SigDB.loadFile(std::filesystem::u8path(SigPath));
  if (Err) {
    S->setError(llvm::toString(std::move(Err)));
    return -1;
  }

  return matchLoadedSignatures(*S, std::move(Entries));
}

int neverd_auto_apply_signatures(neverd_session_t Sess,
                                 const char *SigBaseDir) {
  auto *S = static_cast<Session *>(Sess);
  if (!S || !S->Loaded || !SigBaseDir)
    return -1;
  S->clearError();

  if (!sigs::SignatureDB::treeDirectory(S->Img))
    return 0;
  auto Entries = findSignatureEntries(*S);
  useSignatureCache(*S);
  auto Err = S->SigDB.loadForImage(S->Img, std::filesystem::u8path(SigBaseDir));
  if (Err) {
    S->setError(llvm::toString(std::move(Err)));
    return -1;
  }

  return matchLoadedSignatures(*S, std::move(Entries));
}

// ===--------------------------------------------------------------------===//
// Signature generation utilities
// ===--------------------------------------------------------------------===//

unsigned short neverd_sig_compute_crc16(const unsigned char *Data, int Length) {
  if (!Data || Length <= 0)
    return 0;
  return sigs::SignatureMatcher::computeCRC16(Data,
                                              static_cast<size_t>(Length));
}

int neverd_sig_match_count(neverd_session_t Sess) {
  auto *S = static_cast<Session *>(Sess);
  if (!S)
    return 0;
  return static_cast<int>(S->SigDB.matches().size());
}

const char *neverd_sig_matches_json(neverd_session_t Sess) {
  auto *S = static_cast<Session *>(Sess);
  if (!S)
    return dupStr("[]");

  // Stream in json::Object's sorted key order. Large candidate sets need not
  // retain a second object graph, allocate every key, or sort every row.
  std::string Buffer;
  llvm::raw_string_ostream OS(Buffer);
  llvm::json::OStream JSON(OS);
  JSON.array([&] {
    for (const auto &M : S->SigDB.matches())
      JSON.object([&] {
        const auto *Decision = S->SigDB.nameDecision(M.Address);
        JSON.attribute("addr", vaHex(M.Address));
        if (!M.Aliases.empty())
          JSON.attributeArray("aliases", [&] {
            for (const std::string &Alias : M.Aliases)
              JSON.value(jsonSafeText(Alias));
          });
        JSON.attribute("auto_name",
                       Decision ? jsonSafeText(Decision->Name) : "");
        JSON.attribute("caller_confirmed", M.CallerConfirmed);
        JSON.attribute("candidate_reason",
                       Decision ? "" : "independent_identity_missing");
        JSON.attribute("confirmed", M.Confirmed);
        JSON.attribute("func_len", static_cast<int64_t>(M.FuncLen));
        JSON.attribute("library", jsonSafeText(M.LibraryName));
        JSON.attribute("name", jsonSafeText(M.Name));
        JSON.attribute("name_evidence",
                       !Decision ? "none"
                       : Decision->Evidence ==
                               sigs::SignatureNameEvidence::ImageSymbol
                           ? "image_symbol"
                           : "exception_personality");
      });
  });
  OS.flush();
  return dupStr(Buffer);
}
