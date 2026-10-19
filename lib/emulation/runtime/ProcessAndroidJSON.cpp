//===- ProcessAndroidJSON.cpp - Android native request and report --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ProcessAndroidJSON.h"

#include "../os/linux/android/AndroidLog.h"
#include "../os/linux/android/AndroidObservation.h"
#include "ProcessNetworkJSON.h"

#include "neverd/dalvik/Budget.h"
#include "neverd/emulation/ProcessReportFields.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd::emulation {
namespace {
namespace field = process_report;
llvm::Error invalid(llvm::StringRef Key) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 llvm::Twine(field::AndroidOptions) + Key);
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
llvm::Expected<std::vector<NativeMemoryRead>>
memoryReads(const llvm::json::Value &Value) {
  const auto *Array = Value.getAsArray();
  if (!Array)
    return invalid(field::ReadMemory);
  std::vector<NativeMemoryRead> Reads;
  for (const auto &Item : *Array) {
    const auto *Object = Item.getAsObject();
    if (!Object || !Object->get(field::Address) || !Object->get(field::Size))
      return invalid(field::ReadMemory);
    NativeMemoryRead Read;
    for (const auto &[Key, V] : *Object) {
      llvm::StringRef Name = Key;
      if (Name == field::Address || Name == field::Size) {
        auto N = number(V, Name);
        if (!N)
          return N.takeError();
        (Name == field::Address ? Read.Address : Read.Size) = *N;
      } else if (Name == field::RequireMappedAtEntry) {
        auto B = V.getAsBoolean();
        if (!B)
          return invalid(Name);
        Read.RequireMappedAtEntry = *B;
      } else
        return invalid(Name);
    }
    Reads.push_back(Read);
  }
  return Reads;
}
llvm::Expected<std::vector<NativeRegisterMemoryRead>>
registerMemoryReads(const llvm::json::Value &Value) {
  const auto *Array = Value.getAsArray();
  if (!Array)
    return invalid(field::ReadRegisterMemory);
  std::vector<NativeRegisterMemoryRead> Reads;
  for (const auto &Item : *Array) {
    const auto *Object = Item.getAsObject();
    if (!Object || !Object->get(field::ObservationRegister) ||
        !Object->get(field::Size))
      return invalid(field::ReadRegisterMemory);
    NativeRegisterMemoryRead Read;
    for (const auto &[Key, V] : *Object) {
      llvm::StringRef Name = Key;
      if (Name == field::ObservationRegister) {
        const auto Text = V.getAsString();
        if (!Text)
          return invalid(Name);
        if (*Text == "sp")
          Read.Register = 31;
        else {
          auto Index = *Text;
          if (!Index.consume_front("x") || Index.empty() ||
              Index.find_first_not_of("0123456789") != llvm::StringRef::npos ||
              (Index.size() > 1 && Index.front() == '0') ||
              Index.getAsInteger(10, Read.Register) || Read.Register > 30)
            return invalid(Name);
        }
      } else if (Name == field::ObservationOffset) {
        // Keep INT64_MAX exact, and reject floating-point 2^63 before the
        // JSON integer accessor can narrow that rounded boundary.
        if (auto Unsigned = V.getAsUINT64()) {
          if (*Unsigned > uint64_t(INT64_MAX))
            return invalid(Name);
          Read.Offset = int64_t(*Unsigned);
          continue;
        }
        auto Numeric = V.getAsNumber();
        if (!Numeric || *Numeric < double(INT64_MIN) ||
            *Numeric >= -double(INT64_MIN))
          return invalid(Name);
        auto Offset = V.getAsInteger();
        if (!Offset)
          return invalid(Name);
        Read.Offset = *Offset;
      } else if (Name == field::Size) {
        auto Size = number(V, Name);
        if (!Size)
          return Size.takeError();
        Read.Size = *Size;
      } else
        return invalid(Name);
    }
    Reads.push_back(Read);
  }
  return Reads;
}
llvm::Expected<NativeExecutionObservationOptions>
observationOptions(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object)
    return invalid(field::ObserveExecution);
  NativeExecutionObservationOptions Out;
  for (const auto &[Key, V] : *Object) {
    llvm::StringRef Name = Key;
    if (Name == field::ObservationPCs) {
      const auto *PCs = V.getAsArray();
      if (!PCs)
        return invalid(Name);
      for (const auto &PC : *PCs) {
        auto N = number(PC, Name);
        if (!N)
          return N.takeError();
        Out.PCs.push_back(*N);
      }
    } else if (Name == field::ObservationLimit) {
      auto N = number(V, Name);
      if (!N)
        return N.takeError();
      Out.Limit = *N;
    } else if (Name == field::ReadMemory) {
      auto Reads = memoryReads(V);
      if (!Reads)
        return Reads.takeError();
      Out.ReadMemory = std::move(*Reads);
    } else if (Name == field::ReadRegisterMemory) {
      auto Reads = registerMemoryReads(V);
      if (!Reads)
        return Reads.takeError();
      Out.ReadRegisterMemory = std::move(*Reads);
    } else
      return invalid(Name);
  }
  if (auto E = android_model::validateExecutionObservation(Out))
    return std::move(E);
  return Out;
}
} // namespace
llvm::Expected<AndroidNativeOptions>
androidOptionsFromJSON(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object)
    return invalid(field::Android);
  AndroidNativeOptions Out;
  for (const auto &[Key, V] : *Object) {
    llvm::StringRef Name = Key;
    if (Name == field::ResolverQueries) {
      auto Queries = androidResolverQueriesFromJSON(V);
      if (!Queries)
        return Queries.takeError();
      Out.ResolverQueries = std::move(*Queries);
    } else if (Name == field::EntrySymbol) {
      auto S = string(V, Name);
      if (!S)
        return S.takeError();
      Out.EntrySymbol = std::move(*S);
    } else if (Name == field::EntryAddress || Name == field::LoadBias ||
               Name == field::TraceLimit || Name == field::ThreadLimit) {
      auto N = number(V, Name);
      if (!N)
        return N.takeError();
      if (Name == field::EntryAddress)
        Out.EntryAddress = *N;
      else if (Name == field::LoadBias)
        Out.LoadBias = *N;
      else if (Name == field::ThreadLimit)
        Out.ThreadLimit = *N;
      else
        Out.TraceLimit = *N;
    } else if (Name == field::MallocFillByte) {
      auto N = number(V, Name);
      if (!N)
        return N.takeError();
      if (*N > UINT8_MAX)
        return invalid(Name);
      Out.MallocFillByte = static_cast<uint8_t>(*N);
    } else if (Name == field::Initialize || Name == field::DrainThreads ||
               Name == field::JNI) {
      auto B = V.getAsBoolean();
      if (!B)
        return invalid(Name);
      (Name == field::Initialize ? Out.Initialize
       : Name == field::JNI      ? Out.JNI
                                 : Out.DrainThreads) = *B;
    } else if (Name == field::LogRecords) {
      const auto *Records = V.getAsArray();
      if (!Records || Records->size() > android_model::LogRecordLimit)
        return invalid(Name);
      Out.LogRecords.emplace();
      uint64_t Remaining = android_model::LogSnapshotLimit;
      for (const auto &Item : *Records) {
        const auto *Record = Item.getAsObject();
        if (!Record || Record->size() != 7)
          return invalid(Name);
        NativeLogRecord R;
        for (const auto &[Key, Entry] : *Record) {
          llvm::StringRef K = Key;
          if (K == field::LogPayload) {
            auto Text = Entry.getAsString();
            if (!Text || Text->size() % 2 ||
                Text->size() / 2 > android_model::LogPayloadLimit ||
                !llvm::all_of(*Text, llvm::isHexDigit))
              return invalid(K);
            auto Bytes = llvm::fromHex(*Text);
            R.Payload.assign(Bytes.begin(), Bytes.end());
          } else {
            uint32_t *Destination = K == field::LogID         ? &R.ID
                                    : K == field::LogPID      ? &R.PID
                                    : K == field::LogTID      ? &R.TID
                                    : K == field::Seconds     ? &R.Seconds
                                    : K == field::Nanoseconds ? &R.Nanoseconds
                                    : K == field::FileUID     ? &R.UID
                                                              : nullptr;
            if (!Destination)
              return invalid(K);
            auto N = number(Entry, K);
            if (!N)
              return N.takeError();
            if (*N > UINT32_MAX)
              return invalid(K);
            *Destination = uint32_t(*N);
          }
        }
        const uint64_t Size = 28 + R.Payload.size();
        if (Size > Remaining)
          return invalid(Name);
        Remaining -= Size;
        Out.LogRecords->push_back(std::move(R));
      }
    } else if (Name == field::ApplicationSnapshot) {
      const auto *Input = V.getAsObject();
      if (!Input || Input->empty())
        return invalid(Name);
      mobile::dalvik::AndroidApplicationSnapshot Snapshot;
      bool HasPhase = false;
      for (const auto &[Key, Value] : *Input) {
        if (Key == field::ApplicationClassLookupFailures) {
          const auto *Failures = Value.getAsArray();
          if (!Failures || Failures->size() > 256)
            return invalid(Key);
          for (const auto &Entry : *Failures) {
            const auto *Failure = Entry.getAsObject();
            if (!Failure || Failure->size() != 3 ||
                !Failure->getString("loader") ||
                !Failure->getString("binary_name") || !Failure->get("message"))
              return invalid(Key);
            using Outcome =
                mobile::dalvik::AndroidApplicationSnapshot::ClassLookupFailure;
            Outcome Result;
            auto Loader = *Failure->getString("loader");
            if (Loader == "boot")
              Result.loader = Outcome::Loader::Boot;
            else if (Loader == "system")
              Result.loader = Outcome::Loader::System;
            else if (Loader == "application")
              Result.loader = Outcome::Loader::Application;
            else
              return invalid(Key);
            Result.binary_name = Failure->getString("binary_name")->str();
            const auto &Message = *Failure->get("message");
            if (auto Text = Message.getAsString())
              Result.message = Text->str();
            else if (Message.kind() != llvm::json::Value::Null)
              return invalid(Key);
            Snapshot.class_lookup_failures.push_back(std::move(Result));
          }
          continue;
        }
        auto Text = Value.getAsString();
        if (!Text)
          return invalid(Key);
        if (Key == field::ApplicationPhase) {
          using Phase = mobile::dalvik::AndroidApplicationSnapshot::Phase;
          if (*Text == "unattached")
            Snapshot.phase = Phase::Unattached;
          else if (*Text == "attached")
            Snapshot.phase = Phase::Attached;
          else if (*Text == "application")
            Snapshot.phase = Phase::Application;
          else
            return invalid(Key);
          HasPhase = true;
        } else if (Key == field::ApplicationPackage) {
          if (Text->empty() || Text->size() > 1024 || Text->contains('\0'))
            return invalid(Key);
          Snapshot.package_name = Text->str();
        } else if (Key == field::ApplicationFilesDirectory) {
          if (Text->empty() || !Text->starts_with('/') || Text->size() > 4095 ||
              Text->contains('\0'))
            return invalid(Key);
          Snapshot.files_directory = Text->str();
        } else if (Key == field::ApplicationInstrumentation) {
          using State =
              mobile::dalvik::AndroidApplicationSnapshot::Instrumentation;
          if (*Text == "uninitialized")
            Snapshot.instrumentation = State::Uninitialized;
          else if (*Text == "platform")
            Snapshot.instrumentation = State::Platform;
          else
            return invalid(Key);
        } else
          return invalid(Key);
      }
      if (!HasPhase ||
          (Snapshot.instrumentation &&
           Snapshot.phase ==
               mobile::dalvik::AndroidApplicationSnapshot::Phase::Unattached) ||
          ((Snapshot.package_name || Snapshot.files_directory) &&
           Snapshot.phase !=
               mobile::dalvik::AndroidApplicationSnapshot::Phase::Application))
        return invalid(Name);
      try {
        mobile::dalvik::validateAndroidClassLookupFailures(Snapshot);
      } catch (const mobile::Error &) {
        return invalid(field::ApplicationClassLookupFailures);
      }
      Out.Application = std::move(Snapshot);
    } else if (Name == field::JNIClassLookupFailures) {
      const auto *Failures = V.getAsObject();
      if (!Failures || Failures->size() > 4096)
        return invalid(Name);
      for (const auto &[Class, Exception] : *Failures) {
        auto Type = string(Exception, Name);
        if (!Type)
          return Type.takeError();
        Out.JNIClassLookupFailures.emplace(Class.str(), std::move(*Type));
      }
    } else if (Name == field::DexImages) {
      const auto *Images = V.getAsArray();
      if (!Images || Images->size() > 16)
        return invalid(Name);
      uint64_t Remaining = 1024 * 1024;
      for (const auto &Image : *Images) {
        auto Hex = Image.getAsString();
        if (!Hex || Hex->empty() || Hex->size() % 2 ||
            Hex->size() / 2 > Remaining ||
            !llvm::all_of(*Hex, llvm::isHexDigit))
          return invalid(Name);
        Remaining -= Hex->size() / 2;
        std::string Bytes = llvm::fromHex(*Hex);
        Out.DexImages.emplace_back(Bytes.begin(), Bytes.end());
      }
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
    } else if (Name == field::AuxiliaryVector) {
      const auto *Entries = V.getAsArray();
      if (!Entries)
        return invalid(Name);
      Out.AuxiliaryVector.emplace();
      for (const auto &Entry : *Entries) {
        const auto *O = Entry.getAsObject();
        if (!O || O->size() != 2 || !O->get(field::AuxiliaryType) ||
            !O->get(field::AuxiliaryValue))
          return invalid(Name);
        auto Type = number(*O->get(field::AuxiliaryType), field::AuxiliaryType);
        if (!Type)
          return Type.takeError();
        auto Value =
            number(*O->get(field::AuxiliaryValue), field::AuxiliaryValue);
        if (!Value)
          return Value.takeError();
        Out.AuxiliaryVector->push_back({*Type, *Value});
      }
    } else if (Name == field::FixedTimeZone) {
      const auto *Zone = V.getAsObject();
      if (!Zone || Zone->size() != 2)
        return invalid(Name);
      auto Offset = Zone->getInteger(field::ZoneOffsetSeconds);
      const auto *Text = Zone->get(field::ZoneName);
      if (!Offset || !Text || *Offset < INT32_MIN || *Offset > INT32_MAX)
        return invalid(Name);
      auto Label = string(*Text, field::ZoneName);
      if (!Label)
        return Label.takeError();
      Out.FixedTimeZone =
          AndroidFixedTimeZone{int32_t(*Offset), std::move(*Label)};
    } else if (Name == field::DefaultScope) {
      const auto *Scope = V.getAsArray();
      if (!Scope)
        return invalid(Name);
      Out.DefaultScope.emplace();
      for (const auto &Library : *Scope) {
        auto S = string(Library, Name);
        if (!S)
          return S.takeError();
        Out.DefaultScope->push_back(std::move(*S));
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
    } else if (Name == field::ObserveExecution) {
      auto Observation = observationOptions(V);
      if (!Observation)
        return Observation.takeError();
      Out.ObserveExecution = std::move(*Observation);
    } else if (Name == field::ReadMemory) {
      auto Reads = memoryReads(V);
      if (!Reads)
        return Reads.takeError();
      Out.ReadMemory = std::move(*Reads);
    } else if (Name == field::Memory) {
      const auto *A = V.getAsArray();
      if (!A)
        return invalid(Name);
      for (const auto &Item : *A) {
        const auto *O = Item.getAsObject();
        if (!O || !O->get(field::Address) || !O->get(field::Size))
          return invalid(Name);
        if (O->get(field::Path) && O->get(field::Bytes))
          return invalid(field::Path);
        NativeMemoryRegion Region;
        for (const auto &[K, Entry] : *O) {
          llvm::StringRef Field = K;
          if (Field == field::Address || Field == field::Size) {
            auto N = number(Entry, Field);
            if (!N)
              return N.takeError();
            (Field == field::Address ? Region.Address : Region.Size) = *N;
          } else if (Field == field::Bytes) {
            auto S = Entry.getAsString();
            if (!S || S->size() % 2 || !llvm::all_of(*S, llvm::isHexDigit))
              return invalid(Field);
            std::string Data = llvm::fromHex(*S);
            Region.Bytes.assign(Data.begin(), Data.end());
          } else if (Field == field::Path) {
            auto S = string(Entry, Field);
            if (!S)
              return S.takeError();
            if (S->empty())
              return invalid(Field);
            Region.File = std::filesystem::path(std::u8string(
                reinterpret_cast<const char8_t *>(S->data()), S->size()));
          } else if (Field == field::Executable) {
            auto B = Entry.getAsBoolean();
            if (!B)
              return invalid(Field);
            Region.Executable = *B;
          } else
            return invalid(Field);
        }
        Out.Memory.push_back(std::move(Region));
      }
    } else
      return invalid(Name);
  }
  if (auto E = android_model::validateLogRecords(Out.LogRecords))
    return std::move(E);
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
    if (Event.ThreadID)
      Call[field::ThreadID] = *Event.ThreadID;
    Calls.push_back(std::move(Call));
  }
  for (uint64_t PC : Result.Trace)
    Trace.push_back(bits(PC));
  for (const auto &Snapshot : Result.MemorySnapshots)
    Memory.push_back(
        llvm::json::Object{{field::Address, bits(Snapshot.Address)},
                           {field::Bytes, llvm::toHex(Snapshot.Bytes, true)}});
  llvm::json::Object Out{{field::Initialize, Result.InitializersEnabled},
                         {field::NativeCalls, std::move(Calls)},
                         {field::Trace, std::move(Trace)},
                         {field::TraceTruncated, Result.TraceTruncated},
                         {field::Memory, std::move(Memory)}};
  if (!Result.JNIRegistrations.empty()) {
    llvm::json::Array Bindings;
    for (const auto &Binding : Result.JNIRegistrations)
      Bindings.push_back(
          llvm::json::Object{{field::JNIClass, Binding.Class},
                             {field::Name, Binding.Name},
                             {field::JNISignature, Binding.Signature},
                             {field::Address, bits(Binding.Address)}});
    Out[field::JNIRegistrations] = std::move(Bindings);
  }
  if (Result.ExecutionObservationsEnabled) {
    llvm::json::Array Observations;
    for (const auto &Record : Result.ExecutionObservations) {
      llvm::json::Array Registers, Reads;
      for (uint64_t R : Record.Registers)
        Registers.push_back(bits(R));
      for (const auto &Read : Record.Memory)
        Reads.push_back(
            llvm::json::Object{{field::Address, bits(Read.Address)},
                               {field::Bytes, llvm::toHex(Read.Bytes, true)}});
      Observations.push_back(llvm::json::Object{
          {field::PC, bits(Record.PC)},
          {field::ThreadID, Record.ThreadID},
          {field::Instructions, Record.Instructions},
          {field::ObservationSP, bits(Record.SP)},
          {field::ObservationNZCV, bits(Record.NZCV)},
          {field::ObservationRegisters, std::move(Registers)},
          {field::Memory, std::move(Reads)}});
    }
    Out[field::ExecutionObservations] = std::move(Observations);
    Out[field::ExecutionObservationsTruncated] =
        Result.ExecutionObservationsTruncated;
  }
  if (!Result.NativeThreads.empty()) {
    llvm::json::Array Threads, Spans;
    for (const auto &T : Result.NativeThreads)
      Threads.push_back(llvm::json::Object{
          {field::ThreadID, T.ID},
          {field::Handle, bits(T.Handle)},
          {field::TLS, bits(T.TLS)},
          {field::StackBase, bits(T.StackBase)},
          {field::StackBytes, T.StackSize},
          {field::GuardSize, T.GuardSize},
          {field::Finished, T.Finished},
          {field::Detached, T.Detached},
          {field::Retired, T.Retired},
          {field::Waiting, T.Waiting},
          {field::ReturnValue,
           T.ReturnValue ? llvm::json::Value(bits(*T.ReturnValue)) : nullptr}});
    for (const auto &Span : Result.TraceThreads)
      Spans.push_back(llvm::json::Object{{field::TraceIndex, Span.Index},
                                         {field::ThreadID, Span.ID}});
    Out[field::Threads] = std::move(Threads);
    Out[field::TraceThreads] = std::move(Spans);
  }
  return Out;
}
} // namespace neverd::emulation
