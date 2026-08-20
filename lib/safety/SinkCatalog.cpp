//===- SinkCatalog.cpp - Dangerous-call and input-source catalog ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/SinkCatalog.h"

#include "neverd/loader/LanguageRuntime.h"

#include "llvm/ADT/StringSwitch.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

#include <cstdlib>

using namespace neverd;
using namespace neverd::safety;

std::string SinkCatalog::normalize(llvm::StringRef StatedName) {
  llvm::StringRef S = StatedName;
  if (auto Bang = S.rfind('!'); Bang != llvm::StringRef::npos)
    S = S.drop_front(Bang + 1);
  while (S.starts_with("__imp_"))
    S = S.drop_front(6);
  return stripLeadingUnderscores(S).str();
}

static llvm::StringRef lastIdentifier(llvm::StringRef Dem) {
  if (auto P = Dem.find('('); P != llvm::StringRef::npos)
    Dem = Dem.take_front(P);
  Dem = Dem.rtrim();
  if (auto P = Dem.rfind(':'); P != llvm::StringRef::npos)
    Dem = Dem.drop_front(P + 1);
  return Dem;
}

static std::string demangledKey(llvm::StringRef StatedName) {
  if (std::string Rust = demangleRustName(StatedName); !Rust.empty())
    return SinkCatalog::normalize(lastIdentifier(Rust));
  if (char *Dem = llvm::itaniumDemangle(StatedName.str())) {
    std::string Owned(Dem);
    std::free(Dem);
    return SinkCatalog::normalize(lastIdentifier(Owned));
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

SinkEntry formatSink(const char *Name, int Dst, int Fmt, int Cap,
                     unsigned Sev) {
  SinkEntry E;
  E.Name = Name;
  E.Class = VulnClass::FormatString;
  E.Kind = SinkKind::Format;
  E.DstArg = Dst;
  E.FmtArg = Fmt;
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
#define SAFETY_FORMAT_SINK(NAME, DST, FMT, CAP, SEV)                           \
  C.addSink(formatSink(#NAME, DST, FMT, CAP, SEV));
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
#include "neverd/safety/SafetySources.def"

  return C;
}

namespace {

int fieldOr(const llvm::json::Object &O, llvm::StringRef Key, int Default) {
  if (auto V = O.getInteger(Key))
    return static_cast<int>(*V);
  return Default;
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
  for (const llvm::json::Value &V : *Items) {
    const llvm::json::Object *O = V.getAsObject();
    if (!O)
      continue;
    SinkEntry E;
    if (auto N = O->getString("name"))
      E.Name = N->str();
    if (E.Name.empty())
      continue;
    if (const llvm::json::Array *A = O->getArray("aliases"))
      for (const llvm::json::Value &AV : *A)
        if (auto S = AV.getAsString())
          E.Aliases.push_back(S->str());
    llvm::StringRef Kind =
        O->getString("kind").value_or(toString(SinkKind::Copy));
    E.Kind = llvm::StringSwitch<SinkKind>(Kind)
#define SAFETY_SINK_KIND(ID, SPELLING) .Case(SPELLING, SinkKind::ID)
#include "neverd/safety/SafetyEnums.def"
                 .Default(SinkKind::Copy);
    E.Class = E.Kind == SinkKind::Format ? VulnClass::FormatString
                                         : VulnClass::BufferOverflow;
    E.DstArg = fieldOr(*O, "dst", -1);
    E.SrcArg = fieldOr(*O, "src", -1);
    E.LenArg = fieldOr(*O, "len", -1);
    E.CapArg = fieldOr(*O, "cap", -1);
    E.FmtArg = fieldOr(*O, "fmt", -1);
    E.HandleArg = fieldOr(*O, "handle", -1);
    E.Severity = static_cast<unsigned>(fieldOr(*O, "severity", 50));
    addSink(std::move(E));
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
  for (const llvm::json::Value &V : *Items) {
    const llvm::json::Object *O = V.getAsObject();
    if (!O)
      continue;
    SourceEntry E;
    if (auto N = O->getString("name"))
      E.Name = N->str();
    if (E.Name.empty())
      continue;
    E.OutArg = fieldOr(*O, "out", -1);
    addSource(std::move(E));
  }
  return llvm::Error::success();
}
