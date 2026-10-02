//===- ProcessAndroidJSON.cpp - Android native request and report --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ProcessAndroidJSON.h"

#include "neverd/emulation/ProcessReportFields.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd::emulation {
namespace {
namespace field = process_report;
llvm::Error invalid(llvm::StringRef Key) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "invalid Android native option: " + Key);
}
llvm::Expected<uint64_t> number(const llvm::json::Value &V,
                                llvm::StringRef Name) {
  if (auto N = V.getAsUINT64())
    return *N;
  if (auto Text = V.getAsString()) {
    unsigned Base = Text->consume_front("0x") ? 16 : 10;
    uint64_t N;
    if (!Text->empty() && !Text->getAsInteger(Base, N))
      return N;
  }
  return invalid(Name);
}
llvm::Expected<std::string> string(const llvm::json::Value &V,
                                   llvm::StringRef Name) {
  auto S = V.getAsString();
  if (!S || S->contains('\0'))
    return invalid(Name);
  return S->str();
}
std::string bits(uint64_t V) { return llvm::utohexstr(V, true); }
} // namespace
llvm::Expected<AndroidNativeOptions>
androidOptionsFromJSON(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object)
    return invalid(field::Android);
  AndroidNativeOptions Out;
  for (const auto &[Key, V] : *Object) {
    llvm::StringRef Name = Key;
    if (Name == field::EntrySymbol) {
      auto S = string(V, Name);
      if (!S)
        return S.takeError();
      Out.EntrySymbol = std::move(*S);
    } else if (Name == field::EntryAddress || Name == field::LoadBias ||
               Name == field::TraceLimit) {
      auto N = number(V, Name);
      if (!N)
        return N.takeError();
      if (Name == field::EntryAddress)
        Out.EntryAddress = *N;
      else if (Name == field::LoadBias)
        Out.LoadBias = *N;
      else
        Out.TraceLimit = *N;
    } else if (Name == field::Initialize) {
      auto B = V.getAsBoolean();
      if (!B)
        return invalid(Name);
      Out.Initialize = *B;
    } else if (Name == field::Arguments) {
      const auto *A = V.getAsArray();
      if (!A)
        return invalid(Name);
      for (const auto &Item : *A) {
        auto N = number(Item, Name);
        if (!N)
          return N.takeError();
        Out.Arguments.push_back(*N);
      }
    } else if (Name == field::Properties) {
      const auto *P = V.getAsObject();
      if (!P)
        return invalid(Name);
      for (const auto &[Key, Item] : *P) {
        auto S = string(Item, Name);
        if (!S)
          return S.takeError();
        if (llvm::StringRef(Key).contains('\0'))
          return invalid(Name);
        Out.Properties.emplace(Key.str(), std::move(*S));
      }
    } else if (Name == field::Libraries) {
      const auto *Libraries = V.getAsObject();
      if (!Libraries)
        return invalid(Name);
      for (const auto &[Library, Entry] : *Libraries) {
        const auto *Symbols = Entry.getAsArray();
        if (!Symbols || llvm::StringRef(Library).empty() ||
            llvm::StringRef(Library).contains('\0'))
          return invalid(Name);
        auto &Names = Out.Libraries[Library.str()];
        for (const auto &Symbol : *Symbols) {
          auto S = string(Symbol, Name);
          if (!S)
            return S.takeError();
          if (S->empty() || llvm::is_contained(Names, *S))
            return invalid(Name);
          Names.push_back(std::move(*S));
        }
      }
    } else if (Name == field::Memory || Name == field::ReadMemory) {
      const auto *A = V.getAsArray();
      if (!A)
        return invalid(Name);
      for (const auto &Item : *A) {
        const auto *O = Item.getAsObject();
        if (!O || !O->get(field::Address) || !O->get(field::Size))
          return invalid(Name);
        NativeMemoryRegion Region;
        for (const auto &[K, Entry] : *O) {
          llvm::StringRef Field = K;
          if (Field == field::Address || Field == field::Size) {
            auto N = number(Entry, Field);
            if (!N)
              return N.takeError();
            (Field == field::Address ? Region.Address : Region.Size) = *N;
          } else if (Name == field::Memory && Field == field::Bytes) {
            auto S = Entry.getAsString();
            if (!S || S->size() % 2 || !llvm::all_of(*S, llvm::isHexDigit))
              return invalid(Field);
            std::string Data = llvm::fromHex(*S);
            Region.Bytes.assign(Data.begin(), Data.end());
          } else if (Name == field::Memory && Field == field::Executable) {
            auto B = Entry.getAsBoolean();
            if (!B)
              return invalid(Field);
            Region.Executable = *B;
          } else
            return invalid(Field);
        }
        if (Name == field::Memory)
          Out.Memory.push_back(std::move(Region));
        else
          Out.ReadMemory.push_back({Region.Address, Region.Size});
      }
    } else
      return invalid(Name);
  }
  return Out;
}
llvm::json::Object androidResultJSON(const ProcessResult &Result) {
  llvm::json::Array Calls, Trace, Memory;
  for (const auto &Event : Result.NativeCalls) {
    llvm::json::Array Args;
    for (uint64_t A : Event.Arguments)
      Args.push_back(bits(A));
    llvm::json::Object Call{
        {field::PC, bits(Event.PC)},
        {field::Name, Event.Name},
        {field::Arguments, std::move(Args)},
        {field::Result,
         Event.Result ? llvm::json::Value(bits(*Event.Result)) : nullptr}};
    if (!Event.Library.empty())
      Call[field::Library] = Event.Library;
    if (!Event.Symbol.empty())
      Call[field::Symbol] = Event.Symbol;
    Calls.push_back(std::move(Call));
  }
  for (uint64_t PC : Result.Trace)
    Trace.push_back(bits(PC));
  for (const auto &Snapshot : Result.MemorySnapshots)
    Memory.push_back(
        llvm::json::Object{{field::Address, bits(Snapshot.Address)},
                           {field::Bytes, llvm::toHex(Snapshot.Bytes, true)}});
  return llvm::json::Object{{field::Initialize, Result.InitializersEnabled},
                            {field::NativeCalls, std::move(Calls)},
                            {field::Trace, std::move(Trace)},
                            {field::TraceTruncated, Result.TraceTruncated},
                            {field::Memory, std::move(Memory)}};
}
} // namespace neverd::emulation
