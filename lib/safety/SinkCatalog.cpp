//===- SinkCatalog.cpp - Dangerous-call and input-source catalog ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/SinkCatalog.h"

#include "neverd/Common.h"

#include "llvm/ADT/StringSwitch.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

#include <cstdlib>
#include <limits>
#include <optional>

using namespace neverd;
using namespace neverd::safety;

std::string SinkCatalog::normalize(llvm::StringRef StatedName) {
  llvm::StringRef S = StatedName;
  if (auto Bang = S.rfind('!'); Bang != llvm::StringRef::npos)
    S = S.drop_front(Bang + 1);
  while (S.starts_with("__imp_"))
    S = S.drop_front(6);
  S = stripLeadingUnderscores(S);
  if (const size_t At = S.rfind('@');
      At != llvm::StringRef::npos && At > 0 && At + 1 < S.size()) {
    bool IsStdcallSuffix = true;
    for (const char C : S.drop_front(At + 1))
      if (C < '0' || C > '9') {
        IsStdcallSuffix = false;
        break;
      }
    if (IsStdcallSuffix) {
      S = S.take_front(At);
      if (S.starts_with('@'))
        S = S.drop_front();
      if (S.ends_with('@'))
        S = S.drop_back();
    }
  }
  return S.str();
}

static std::string demangledKey(llvm::StringRef StatedName) {
  if (char *Dem = llvm::itaniumDemangle(StatedName.str())) {
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
  unsigned Idx = static_cast<unsigned>(SinkList.size());
  std::vector<std::string> Keys = E.Aliases;
  Keys.push_back(E.Name);
  SinkList.push_back(std::move(E));
  for (const std::string &K : Keys) {
    std::string Norm = normalize(K);
    if (!Norm.empty())
      SinkIndex[Norm] = Idx; // later entries win: overrides replace defaults.
  }
}

void SinkCatalog::addSource(SourceEntry E) {
  if (E.Name.empty())
    return;
  unsigned Idx = static_cast<unsigned>(SourceList.size());
  std::string Norm = normalize(E.Name);
  SourceList.push_back(std::move(E));
  if (!Norm.empty())
    SourceIndex[Norm] = Idx;
}

const SourceEntry *SinkCatalog::matchSource(llvm::StringRef StatedName) const {
  auto It = SourceIndex.find(normalize(StatedName));
  if (It == SourceIndex.end())
    return nullptr;
  return &SourceList[It->second];
}

void SinkCatalog::addSinkAlias(llvm::StringRef Canonical,
                               llvm::StringRef Alias) {
  auto It = SinkIndex.find(normalize(Canonical));
  if (It == SinkIndex.end())
    return;
  const unsigned Idx = It->second;
  SinkList[Idx].Aliases.emplace_back(Alias.str());
  std::string Norm = normalize(Alias);
  if (!Norm.empty())
    SinkIndex[Norm] = Idx;
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

SinkEntry freeSink(const char *Name, int Handle, unsigned Sev) {
  SinkEntry E;
  E.Name = Name;
  E.Class = VulnClass::Unknown;
  E.Kind = SinkKind::Free;
  E.HandleArg = Handle;
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
  std::vector<SinkEntry> Pending;
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
    if (E.UnboundedWrite && (E.Kind != SinkKind::Copy || E.DstArg < 0 ||
                             E.SrcArg >= 0 || E.LenArg >= 0))
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "an unbounded sink must be a destination-only copy");
    llvm::Expected<unsigned> Severity = severityFieldOr(*O, 50);
    if (!Severity)
      return Severity.takeError();
    E.Severity = *Severity;
    Pending.push_back(std::move(E));
  }
  for (SinkEntry &E : Pending)
    addSink(std::move(E));
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
  for (SourceEntry &E : Pending)
    addSource(std::move(E));
  return llvm::Error::success();
}
