//===- SinkCatalog.cpp - Dangerous-call and input-source catalog ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/SinkCatalog.h"

#include "neverd/Common.h"
#include "neverd/safety/CountedWriteSemantics.h"

#include "llvm/ADT/StringSwitch.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <optional>

using namespace neverd;
using namespace neverd::safety;

std::string SinkCatalog::normalize(llvm::StringRef StatedName) {
  return counted_write::normalizeCABIName(StatedName);
}

static std::string demangledKey(llvm::StringRef StatedName) {
  llvm::StringRef Candidate = StatedName;
  if (auto Bang = Candidate.rfind('!'); Bang != llvm::StringRef::npos)
    Candidate = Candidate.drop_front(Bang + 1);
  // A platform underscore may precede MinGW's import decoration.
  while (Candidate.starts_with("___imp_"))
    Candidate = Candidate.drop_front();
  while (Candidate.starts_with("__imp_"))
    Candidate = Candidate.drop_front(6);
  // Mach-O and MinGW may add another platform underscore in front of the
  // Itanium `_Z` introducer.  Remove only decoration, preserving `_Z` itself.
  while (Candidate.starts_with("__") && !Candidate.starts_with("_Z"))
    Candidate = Candidate.drop_front();
  if (char *Dem = llvm::itaniumDemangle(Candidate.str())) {
    std::string Owned(Dem);
    std::free(Dem);
    llvm::StringRef Name(Owned);
    if (auto Paren = Name.find('('); Paren != llvm::StringRef::npos)
      Name = Name.take_front(Paren);
    Name = Name.trim();
    if (Name.empty() || Name.contains("::"))
      return {};
    return SinkCatalog::normalize(Name);
  }
  return {};
}

const SinkEntry *SinkCatalog::matchSink(llvm::StringRef StatedName) const {
  auto It = SinkIndex.find(normalize(StatedName));
  if (It != SinkIndex.end())
    return &SinkList[It->second];
  std::string Dem = demangledKey(StatedName);
  if (Dem.empty())
    return nullptr;
  It = SinkIndex.find(Dem);
  return It == SinkIndex.end() ? nullptr : &SinkList[It->second];
}

void SinkCatalog::addSink(SinkEntry E) {
  if (E.Name.empty())
    return;
  const std::string Canonical = normalize(E.Name);
  for (unsigned Idx = 0; Idx < SinkList.size(); ++Idx) {
    if (normalize(SinkList[Idx].Name) != Canonical)
      continue;
    for (const std::string &Alias : SinkList[Idx].Aliases) {
      const std::string NormalizedAlias = normalize(Alias);
      const bool AlreadyPresent = std::any_of(
          E.Aliases.begin(), E.Aliases.end(), [&](const std::string &Current) {
            return normalize(Current) == NormalizedAlias;
          });
      if (!AlreadyPresent)
        E.Aliases.push_back(Alias);
    }
    SinkList[Idx] = std::move(E);
    SinkIndex[Canonical] = Idx;
    for (const std::string &Alias : SinkList[Idx].Aliases) {
      const std::string NormalizedAlias = normalize(Alias);
      if (!NormalizedAlias.empty())
        SinkIndex[NormalizedAlias] = Idx;
    }
    // A programmatic replacement carries discovery metadata only.  Shadow the
    // old executable contract until a typed JSON effect is published by the
    // transactional merge path below.
    setConfiguredSinkEffect(SinkList[Idx], {});
    return;
  }

  unsigned Idx = static_cast<unsigned>(SinkList.size());
  std::vector<std::string> Keys = E.Aliases;
  Keys.push_back(E.Name);
  const bool RebindsKnownIdentity =
      std::any_of(Keys.begin(), Keys.end(), [&](const std::string &Key) {
        const std::string Normalized = normalize(Key);
        return !Normalized.empty() &&
               (SinkIndex.find(Normalized) != SinkIndex.end() ||
                SourceIndex.find(Normalized) != SourceIndex.end() ||
                ConfiguredEffectIndex.find(Normalized) !=
                    ConfiguredEffectIndex.end() ||
                ConfiguredSourceEffectShadows.contains(Normalized));
      });
  SinkList.push_back(std::move(E));
  for (const std::string &K : Keys) {
    std::string Norm = normalize(K);
    if (!Norm.empty())
      SinkIndex[Norm] = Idx; // later entries win: overrides replace defaults.
  }
  if (RebindsKnownIdentity)
    // Discovery-only programmatic aliases must not borrow an executable
    // contract from the identity they displaced.  A JSON merge publishes its
    // validated typed effect immediately after addSink returns.
    setConfiguredSinkEffect(SinkList[Idx], {});
}

void SinkCatalog::addSource(SourceEntry E) {
  if (E.Name.empty())
    return;
  unsigned Idx = static_cast<unsigned>(SourceList.size());
  std::string Norm = normalize(E.Name);
  const bool ReplacesExisting = SourceIndex.find(Norm) != SourceIndex.end();
  SourceList.push_back(std::move(E));
  if (!Norm.empty()) {
    SourceIndex[Norm] = Idx;
    if (ReplacesExisting)
      ConfiguredSourceEffectShadows[Norm] = true;
  }
}

const SourceEntry *SinkCatalog::matchSource(llvm::StringRef StatedName) const {
  auto It = SourceIndex.find(normalize(StatedName));
  if (It == SourceIndex.end())
    return nullptr;
  return &SourceList[It->second];
}

const ConfiguredCallEffect *
SinkCatalog::matchConfiguredCallEffect(llvm::StringRef StatedName) const {
  auto Find = [&](llvm::StringRef Key) -> const ConfiguredCallEffect * {
    auto It = ConfiguredEffectIndex.find(normalize(Key));
    return It == ConfiguredEffectIndex.end()
               ? nullptr
               : &ConfiguredEffectList[It->second];
  };
  if (const ConfiguredCallEffect *Exact = Find(StatedName))
    return Exact;
  if (const SinkEntry *Entry = matchSink(StatedName)) {
    // Canonical fallback is valid only while that canonical spelling still
    // resolves to this entry.  A later entry may deliberately claim the old
    // canonical as an alias; retained aliases of the displaced entry must not
    // borrow the replacement's typed effect.
    if (matchSink(Entry->Name) == Entry)
      if (const ConfiguredCallEffect *Canonical = Find(Entry->Name))
        return Canonical;
  }
  if (ConfiguredSourceEffectShadows.contains(normalize(StatedName)))
    return &ConfiguredNoEffect;
  return nullptr;
}

void SinkCatalog::setConfiguredSinkEffect(const SinkEntry &Entry,
                                          ConfiguredCallEffect Effect) {
  const unsigned Idx = static_cast<unsigned>(ConfiguredEffectList.size());
  ConfiguredEffectList.push_back(Effect);
  auto Publish = [&](llvm::StringRef Key) {
    const std::string Normalized = normalize(Key);
    if (!Normalized.empty())
      ConfiguredEffectIndex[Normalized] = Idx;
  };
  Publish(Entry.Name);
  for (const std::string &Alias : Entry.Aliases)
    Publish(Alias);
}

void SinkCatalog::addSinkAlias(llvm::StringRef Canonical,
                               llvm::StringRef Alias) {
  auto It = SinkIndex.find(normalize(Canonical));
  if (It == SinkIndex.end())
    return;
  const unsigned Idx = It->second;
  SinkList[Idx].Aliases.emplace_back(Alias.str());
  std::string Norm = normalize(Alias);
  if (Norm.empty())
    return;
  const auto PreviousSink = SinkIndex.find(Norm);
  const bool RebindsKnownIdentity =
      (PreviousSink != SinkIndex.end() && PreviousSink->second != Idx) ||
      SourceIndex.find(Norm) != SourceIndex.end() ||
      ConfiguredEffectIndex.find(Norm) != ConfiguredEffectIndex.end() ||
      ConfiguredSourceEffectShadows.contains(Norm);
  SinkIndex[Norm] = Idx;

  const std::string CanonicalNorm = normalize(SinkList[Idx].Name);
  if (Norm == CanonicalNorm)
    return;
  std::optional<unsigned> CanonicalEffect;
  if (auto It = ConfiguredEffectIndex.find(CanonicalNorm);
      It != ConfiguredEffectIndex.end())
    CanonicalEffect = It->second;
  if (auto It = ConfiguredEffectIndex.find(Norm);
      It != ConfiguredEffectIndex.end())
    ConfiguredEffectIndex.erase(It);
  if (CanonicalEffect) {
    ConfiguredEffectIndex[Norm] = *CanonicalEffect;
    return;
  }
  if (!RebindsKnownIdentity)
    return;

  // A newly rebound alias must not retain its old dynamic effect or borrow a
  // same-spelled static descriptor.  Without a typed canonical effect, fail
  // closed for this alias only.
  const unsigned NoEffectIdx =
      static_cast<unsigned>(ConfiguredEffectList.size());
  ConfiguredEffectList.push_back({});
  ConfiguredEffectIndex[Norm] = NoEffectIdx;
}

namespace {

// A copy-family sink: destination, source, optional explicit length, optional
// fortified destination-capacity argument.
SinkEntry copySink(const char *Name, int Dst, int Src, int Len, int Cap,
                   unsigned Sev) {
  SinkEntry E;
  E.Name = Name;
  E.Class = VulnClass::BufferOverflow;
  E.Kind = SinkKind::Copy;
  E.DstArg = Dst;
  E.SrcArg = Src;
  E.LenArg = Len;
  E.CapArg = Cap;
  E.Severity = Sev;
  return E;
}

SinkEntry unboundedCopySink(const char *Name, int Dst, unsigned Sev) {
  SinkEntry E = copySink(Name, Dst, -1, -1, -1, Sev);
  E.UnboundedWrite = true;
  return E;
}

SinkEntry formatSink(const char *Name, int Dst, int Fmt, int Len, int Cap,
                     unsigned Sev) {
  SinkEntry E;
  E.Name = Name;
  E.Class = VulnClass::FormatString;
  E.Kind = SinkKind::Format;
  E.DstArg = Dst;
  E.FmtArg = Fmt;
  E.LenArg = Len;
  E.CapArg = Cap;
  E.Severity = Sev;
  return E;
}

SinkEntry allocSink(const char *Name, int SizeArg, int SrcArg, int HandleArg,
                    unsigned Sev) {
  SinkEntry E;
  E.Name = Name;
  E.Class = VulnClass::Unknown; // audit assigns leak/uaf/double-free per path.
  E.Kind = SinkKind::Alloc;
  E.LenArg = SizeArg;
  E.SrcArg = SrcArg;
  E.HandleArg = HandleArg;
  E.Severity = Sev;
  return E;
}

SinkEntry stackAllocSink(const char *Name, int SizeArg, unsigned Sev) {
  SinkEntry E = allocSink(Name, SizeArg, -1, -1, Sev);
  E.Kind = SinkKind::StackAlloc;
  return E;
}

SinkEntry freeSink(const char *Name, int Handle, unsigned Sev,
                   bool MayFail = false) {
  SinkEntry E;
  E.Name = Name;
  E.Class = VulnClass::Unknown;
  E.Kind = SinkKind::Free;
  E.HandleArg = Handle;
  E.ReleaseMayFail = MayFail;
  E.Severity = Sev;
  return E;
}

SinkEntry reallocSink(const char *Name, int Handle, int Len, unsigned Sev) {
  SinkEntry E;
  E.Name = Name;
  E.Kind = SinkKind::Realloc;
  E.HandleArg = Handle;
  E.LenArg = Len;
  E.Severity = Sev;
  return E;
}

} // namespace

SinkCatalog SinkCatalog::defaults() {
  SinkCatalog C;

#define SAFETY_COPY_SINK(NAME, DST, SRC, LEN, CAP, SEV)                        \
  C.addSink(copySink(#NAME, DST, SRC, LEN, CAP, SEV));
#define SAFETY_UNBOUNDED_COPY_SINK(NAME, DST, SEV)                             \
  C.addSink(unboundedCopySink(#NAME, DST, SEV));
#define SAFETY_FORMAT_SINK(NAME, DST, FMT, LEN, CAP, SEV)                      \
  C.addSink(formatSink(#NAME, DST, FMT, LEN, CAP, SEV));
#define SAFETY_ALLOC_SINK(NAME, SIZE, SRC, HANDLE, SEV)                        \
  C.addSink(allocSink(#NAME, SIZE, SRC, HANDLE, SEV));
#define SAFETY_STACK_ALLOC_SINK(NAME, SIZE, SEV)                               \
  C.addSink(stackAllocSink(#NAME, SIZE, SEV));
#define SAFETY_FREE_SINK(NAME, HANDLE, SEV)                                    \
  C.addSink(freeSink(#NAME, HANDLE, SEV));
#define SAFETY_FALLIBLE_FREE_SINK(NAME, HANDLE, SEV)                           \
  C.addSink(freeSink(#NAME, HANDLE, SEV, true));
#define SAFETY_REALLOC_SINK(NAME, HANDLE, LEN, SEV)                            \
  C.addSink(reallocSink(#NAME, HANDLE, LEN, SEV));
#define SAFETY_SINK_ALIAS(NAME, ALIAS) C.addSinkAlias(#NAME, ALIAS);
#include "neverd/safety/SafetySinks.def"

#define SAFETY_SOURCE(NAME, OUT) C.addSource(SourceEntry{#NAME, OUT});
#define SAFETY_SOURCE_RETURN(NAME, OUT)                                        \
  C.addSource(SourceEntry{#NAME, OUT, true});
#define SAFETY_SOURCE_NO_RETURN(NAME, OUT)                                     \
  C.addSource(SourceEntry{#NAME, OUT, false});
#include "neverd/safety/SafetySources.def"

  return C;
}

namespace {

llvm::Expected<int> indexFieldOr(const llvm::json::Object &O,
                                 llvm::StringRef Key, int Default) {
  const llvm::json::Value *Raw = O.get(Key);
  if (!Raw)
    return Default;
  auto Value = Raw->getAsInteger();
  if (!Value || *Value < -1 ||
      *Value > static_cast<int64_t>(std::numeric_limits<int>::max()))
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "sink field '%s' is out of range",
                                   Key.str().c_str());
  return static_cast<int>(*Value);
}

llvm::Expected<unsigned> severityFieldOr(const llvm::json::Object &O,
                                         unsigned Default) {
  const llvm::json::Value *Raw = O.get("severity");
  if (!Raw)
    return Default;
  auto Value = Raw->getAsInteger();
  if (!Value || *Value < 0 ||
      static_cast<uint64_t>(*Value) > std::numeric_limits<unsigned>::max())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "sink field 'severity' is out of range");
  return static_cast<unsigned>(*Value);
}

std::optional<SinkKind> parseSinkKind(llvm::StringRef Kind) {
  return llvm::StringSwitch<std::optional<SinkKind>>(Kind)
#define SAFETY_SINK_KIND(ID, SPELLING) .Case(SPELLING, SinkKind::ID)
#include "neverd/safety/SafetyEnums.def"
      .Default(std::nullopt);
}

llvm::Expected<unsigned> arityFieldOr(const llvm::json::Object &O,
                                      llvm::StringRef Key, unsigned Default,
                                      bool AllowVariadic) {
  const llvm::json::Value *Raw = O.get(Key);
  if (!Raw)
    return Default;
  if (AllowVariadic)
    if (auto Value = Raw->getAsString()) {
      if (*Value == "variadic")
        return ConfiguredCallEffect::VariadicArity;
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "sink effect field '%s' must be an arity or 'variadic'",
          Key.str().c_str());
    }
  auto Value = Raw->getAsInteger();
  if (!Value || *Value < 0 ||
      static_cast<uint64_t>(*Value) > std::numeric_limits<unsigned>::max())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "sink effect field '%s' is out of range",
                                   Key.str().c_str());
  return static_cast<unsigned>(*Value);
}

llvm::Expected<ConfiguredCallEffect::Format>
formatMaskFieldOr(const llvm::json::Object &O,
                  ConfiguredCallEffect::Format Default) {
  const llvm::json::Value *Raw = O.get("formats");
  if (!Raw)
    return Default;
  const llvm::json::Array *Items = Raw->getAsArray();
  if (!Items || Items->empty())
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "sink effect field 'formats' must be a non-empty array");
  uint8_t Mask = 0;
  for (const llvm::json::Value &Item : *Items) {
    auto Spelling = Item.getAsString();
    if (!Spelling)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "sink effect format must be a string");
    const uint8_t Bit = llvm::StringSwitch<uint8_t>(*Spelling)
                            .Case("elf", uint8_t{1} << 0)
                            .Case("coff", uint8_t{1} << 1)
                            .Case("macho", uint8_t{1} << 2)
                            .Default(0);
    if (!Bit)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "unknown sink effect format '%s'",
                                     Spelling->str().c_str());
    Mask |= Bit;
  }
  return static_cast<ConfiguredCallEffect::Format>(Mask);
}

llvm::Expected<ConfiguredCallEffect::ABI>
abiMaskFieldOr(const llvm::json::Object &O, ConfiguredCallEffect::ABI Default) {
  const llvm::json::Value *Raw = O.get("abis");
  if (!Raw)
    return Default;
  const llvm::json::Array *Items = Raw->getAsArray();
  if (!Items || Items->empty())
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "sink effect field 'abis' must be a non-empty array");
  uint8_t Mask = 0;
  for (const llvm::json::Value &Item : *Items) {
    auto Spelling = Item.getAsString();
    if (!Spelling)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "sink effect ABI must be a string");
    const uint8_t Bit = llvm::StringSwitch<uint8_t>(*Spelling)
                            .Case("sysv", uint8_t{1} << 0)
                            .Case("microsoft", uint8_t{1} << 1)
                            .Case("darwin", uint8_t{1} << 2)
                            .Case("aapcs", uint8_t{1} << 3)
                            .Default(0);
    if (!Bit)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "unknown sink effect ABI '%s'",
                                     Spelling->str().c_str());
    Mask |= Bit;
  }
  return static_cast<ConfiguredCallEffect::ABI>(Mask);
}

llvm::Expected<ConfiguredCallEffect>
configuredEffectFor(const llvm::json::Object &O, const SinkEntry &Entry) {
  ConfiguredCallEffect Effect;
  if (Entry.Kind == SinkKind::Copy && Entry.DstArg >= 0 &&
      (Entry.UnboundedWrite || Entry.SrcArg >= 0 || Entry.LenArg >= 0))
    Effect.TheFamily = ConfiguredCallEffect::Family::Copy;
  else if (Entry.Kind == SinkKind::Format && Entry.FmtArg >= 0)
    Effect.TheFamily = ConfiguredCallEffect::Family::Format;

  const int HighestArg =
      std::max({Entry.DstArg, Entry.SrcArg, Entry.LenArg, Entry.CapArg,
                Entry.FmtArg, Entry.HandleArg});
  const unsigned RequiredArity =
      HighestArg < 0 ? 0u : static_cast<unsigned>(HighestArg) + 1;
  Effect.MinArity = RequiredArity;
  Effect.MaxArity = Entry.Kind == SinkKind::Format
                        ? ConfiguredCallEffect::VariadicArity
                        : RequiredArity;

  const llvm::json::Value *Raw = O.get("effect");
  if (!Raw)
    return Effect;
  const llvm::json::Object *EffectObject = Raw->getAsObject();
  if (!EffectObject)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "sink field 'effect' is not an object");
  if (Effect.TheFamily == ConfiguredCallEffect::Family::None)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "sink effect requires a complete copy or format layout");

  llvm::Expected<unsigned> MinArity =
      arityFieldOr(*EffectObject, "min_arity", Effect.MinArity, false);
  if (!MinArity)
    return MinArity.takeError();
  llvm::Expected<unsigned> MaxArity =
      arityFieldOr(*EffectObject, "max_arity", Effect.MaxArity, true);
  if (!MaxArity)
    return MaxArity.takeError();
  if (*MinArity < RequiredArity)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "sink effect minimum arity does not cover its argument layout");
  if (*MaxArity < *MinArity)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "sink effect maximum arity is less than its minimum arity");
  Effect.MinArity = *MinArity;
  Effect.MaxArity = *MaxArity;

  llvm::Expected<ConfiguredCallEffect::Format> Formats =
      formatMaskFieldOr(*EffectObject, Effect.Formats);
  if (!Formats)
    return Formats.takeError();
  Effect.Formats = *Formats;
  llvm::Expected<ConfiguredCallEffect::ABI> ABIs =
      abiMaskFieldOr(*EffectObject, Effect.ABIs);
  if (!ABIs)
    return ABIs.takeError();
  Effect.ABIs = *ABIs;
  return Effect;
}

} // namespace

llvm::Error SinkCatalog::mergeSinksFromFile(llvm::StringRef Path) {
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> Buf =
      llvm::MemoryBuffer::getFile(Path);
  if (!Buf)
    return llvm::createStringError(Buf.getError(),
                                   "cannot read sink specification");
  llvm::Expected<llvm::json::Value> Parsed =
      llvm::json::parse((*Buf)->getBuffer());
  if (!Parsed)
    return Parsed.takeError();
  const llvm::json::Array *Items = nullptr;
  if (const llvm::json::Object *Root = Parsed->getAsObject())
    Items = Root->getArray("sinks");
  else
    Items = Parsed->getAsArray();
  if (!Items)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "sink specification has no sink list");
  std::vector<std::pair<SinkEntry, ConfiguredCallEffect>> Pending;
  Pending.reserve(Items->size());
  for (const llvm::json::Value &V : *Items) {
    const llvm::json::Object *O = V.getAsObject();
    if (!O)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "sink entry is not an object");
    SinkEntry E;
    if (auto N = O->getString("name"))
      E.Name = N->str();
    if (E.Name.empty())
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "sink entry has no name");
    if (const llvm::json::Array *A = O->getArray("aliases"))
      for (const llvm::json::Value &AV : *A)
        if (auto S = AV.getAsString())
          E.Aliases.push_back(S->str());
    llvm::StringRef Kind = toString(SinkKind::Copy);
    if (const llvm::json::Value *KindValue = O->get("kind")) {
      auto KindString = KindValue->getAsString();
      if (!KindString)
        return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                       "sink kind is not a string");
      Kind = *KindString;
    }
    std::optional<SinkKind> ParsedKind = parseSinkKind(Kind);
    if (!ParsedKind)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "unknown sink kind '%s'",
                                     Kind.str().c_str());
    E.Kind = *ParsedKind;
    E.Class = E.Kind == SinkKind::Copy     ? VulnClass::BufferOverflow
              : E.Kind == SinkKind::Format ? VulnClass::FormatString
                                           : VulnClass::Unknown;
    auto readIndex = [&](llvm::StringRef Key, int &Out) -> llvm::Error {
      llvm::Expected<int> Value = indexFieldOr(*O, Key, -1);
      if (!Value)
        return Value.takeError();
      Out = *Value;
      return llvm::Error::success();
    };
    if (llvm::Error Err = readIndex("dst", E.DstArg))
      return Err;
    if (llvm::Error Err = readIndex("src", E.SrcArg))
      return Err;
    if (llvm::Error Err = readIndex("len", E.LenArg))
      return Err;
    if (llvm::Error Err = readIndex("cap", E.CapArg))
      return Err;
    if (llvm::Error Err = readIndex("fmt", E.FmtArg))
      return Err;
    if (llvm::Error Err = readIndex("handle", E.HandleArg))
      return Err;
    if (const llvm::json::Value *Raw = O->get("unbounded")) {
      std::optional<bool> Value = Raw->getAsBoolean();
      if (!Value)
        return llvm::createStringError(
            llvm::inconvertibleErrorCode(),
            "sink field 'unbounded' is not a boolean");
      E.UnboundedWrite = *Value;
    }
    if (const llvm::json::Value *Raw = O->get("release_may_fail")) {
      std::optional<bool> Value = Raw->getAsBoolean();
      if (!Value)
        return llvm::createStringError(
            llvm::inconvertibleErrorCode(),
            "sink field 'release_may_fail' is not a boolean");
      E.ReleaseMayFail = *Value;
    }
    if (E.UnboundedWrite && (E.Kind != SinkKind::Copy || E.DstArg < 0 ||
                             E.SrcArg >= 0 || E.LenArg >= 0))
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "an unbounded sink must be a destination-only copy");
    if (E.ReleaseMayFail && E.Kind != SinkKind::Free)
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "only a release sink may have 'release_may_fail'");
    llvm::Expected<unsigned> Severity = severityFieldOr(*O, 50);
    if (!Severity)
      return Severity.takeError();
    E.Severity = *Severity;
    llvm::Expected<ConfiguredCallEffect> Effect = configuredEffectFor(*O, E);
    if (!Effect)
      return Effect.takeError();
    Pending.emplace_back(std::move(E), *Effect);
  }
  for (auto &[Entry, Effect] : Pending) {
    const std::string Canonical = normalize(Entry.Name);
    addSink(std::move(Entry));
    if (const SinkEntry *Published = matchSink(Canonical))
      setConfiguredSinkEffect(*Published, Effect);
  }
  return llvm::Error::success();
}

llvm::Error SinkCatalog::mergeSourcesFromFile(llvm::StringRef Path) {
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> Buf =
      llvm::MemoryBuffer::getFile(Path);
  if (!Buf)
    return llvm::createStringError(Buf.getError(),
                                   "cannot read source specification");
  llvm::Expected<llvm::json::Value> Parsed =
      llvm::json::parse((*Buf)->getBuffer());
  if (!Parsed)
    return Parsed.takeError();
  const llvm::json::Array *Items = nullptr;
  if (const llvm::json::Object *Root = Parsed->getAsObject())
    Items = Root->getArray("sources");
  else
    Items = Parsed->getAsArray();
  if (!Items)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "source specification has no source list");
  std::vector<SourceEntry> Pending;
  Pending.reserve(Items->size());
  for (const llvm::json::Value &V : *Items) {
    const llvm::json::Object *O = V.getAsObject();
    if (!O)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "source entry is not an object");
    SourceEntry E;
    if (auto N = O->getString("name"))
      E.Name = N->str();
    if (E.Name.empty())
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "source entry has no name");
    llvm::Expected<int> OutArg = indexFieldOr(*O, "out", -1);
    if (!OutArg)
      return OutArg.takeError();
    E.OutArg = *OutArg;
    if (const llvm::json::Value *Raw = O->get("return_tainted")) {
      std::optional<bool> Value = Raw->getAsBoolean();
      if (!Value)
        return llvm::createStringError(
            llvm::inconvertibleErrorCode(),
            "source field 'return_tainted' is not a boolean");
      E.TaintedReturn = *Value;
    }
    Pending.push_back(std::move(E));
  }
  for (SourceEntry &E : Pending) {
    const std::string Name = E.Name;
    addSource(std::move(E));
    // Configurable source discovery does not yet carry enough semantics to
    // execute a taint summary.  Publish an explicit NoEffect marker so an
    // override cannot accidentally inherit a same-named built-in contract.
    const std::string Normalized = normalize(Name);
    if (!Normalized.empty())
      ConfiguredSourceEffectShadows[Normalized] = true;
  }
  return llvm::Error::success();
}
