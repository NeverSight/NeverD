//===- AndroidLog.cpp - Bounded liblog snapshots and event writes --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent API-28 log readers and exact finite event write results.
/// ABI references and deliberate transport limits: docs/android-log-reader.md.
#include "AndroidLog.h"

#include "AndroidInternal.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>
#include <tuple>

namespace neverd::emulation::android_model {
namespace {
constexpr uint32_t Nonblock = 0x800;
constexpr size_t HeaderSize = 28, MessageSize = 5124;
uint64_t negative(int Error) { return uint32_t(-Error); }
bool textBuffer(uint32_t ID) {
  return ID == 0 || ID == 1 || ID == 3 || ID == 4;
}

template <class Entry, class Matches>
const Entry *nextLogWrite(const std::optional<std::vector<Entry>> &Writes,
                          const std::optional<uint64_t> &Consumed,
                          Matches Match, llvm::StringRef Kind,
                          ProcessResult &Result) {
  const char *Reason = nullptr;
  if (!Writes)
    Reason = "contract is not supplied";
  else if (*Consumed == Writes->size())
    Reason = "contract exhausted";
  else if (!Match((*Writes)[*Consumed]))
    Reason = "does not match the next entry";
  if (Reason) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = ("Android " + Kind + " log write " + Reason).str();
    return nullptr;
  }
  return &(*Writes)[*Consumed];
}

template <class Entry, class Matches>
std::optional<uint32_t>
consumeLogWrite(const std::optional<std::vector<Entry>> &Writes,
                std::optional<uint64_t> &Consumed, Matches Match,
                llvm::StringRef Kind, ProcessResult &Result) {
  const auto *Write = nextLogWrite(Writes, Consumed, Match, Kind, Result);
  if (!Write)
    return std::nullopt;
  ++*Consumed;
  return uint32_t(Write->Result);
}

std::vector<uint8_t> textPayload(int32_t Priority, llvm::StringRef Tag,
                                 llvm::StringRef Message) {
  std::vector<uint8_t> Payload;
  Payload.reserve(
      std::min<size_t>(LogPayloadLimit, Tag.size() + Message.size() + 3));
  Payload.push_back(uint8_t(Priority));
  // The transport clips concatenated iovecs, without synthesizing a NUL.
  for (llvm::StringRef Text : {Tag, Message}) {
    auto Prefix = Text.take_front(LogPayloadLimit - Payload.size());
    Payload.insert(Payload.end(), Prefix.begin(), Prefix.end());
    if (Payload.size() < LogPayloadLimit)
      Payload.push_back(0);
  }
  return Payload;
}
} // namespace

llvm::Error
validateLogRecords(const std::optional<std::vector<NativeLogRecord>> &Records) {
  if (!Records)
    return llvm::Error::success();
  if (Records->size() > LogRecordLimit)
    return failure("too many Android log records");
  uint64_t Bytes = 0;
  std::pair<uint32_t, uint32_t> Previous{0, 0};
  for (const auto &Record : *Records) {
    auto Time = std::make_pair(Record.Seconds, Record.Nanoseconds);
    Bytes += HeaderSize + Record.Payload.size();
    if (Record.ID > 5 || Record.PID > INT32_MAX ||
        Record.Nanoseconds >= 1000000000 ||
        Record.Payload.size() > LogPayloadLimit || Bytes > LogSnapshotLimit ||
        Time < Previous)
      return failure("invalid Android log record or snapshot order/size");
    Previous = Time;
  }
  return llvm::Error::success();
}

llvm::Error validateEventLogWrites(
    const std::optional<std::vector<NativeEventLogWrite>> &Writes) {
  if (!Writes)
    return llvm::Error::success();
  if (Writes->size() > LogRecordLimit)
    return failure("too many Android event log writes");
  uint64_t Bytes = 0;
  for (const auto &Write : *Writes) {
    Bytes += Write.Payload.size() + sizeof(Write.Result);
    if (Write.Payload.size() < 4 || Write.Payload.size() > LogPayloadLimit ||
        Bytes > LogSnapshotLimit)
      return failure("invalid Android event log write payload or total size");
  }
  return llvm::Error::success();
}

std::optional<uint32_t> Bionic::eventLogWrite(llvm::ArrayRef<uint8_t> Payload) {
  return consumeLogWrite(
      Options.Android->EventLogWrites, Result.EventLogWritesConsumed,
      [&](const NativeEventLogWrite &Write) {
        return Payload == llvm::ArrayRef(Write.Payload);
      },
      "event", Result);
}

llvm::Error validateTextLogWrites(
    const std::optional<std::vector<NativeTextLogWrite>> &Writes) {
  if (!Writes)
    return llvm::Error::success();
  if (Writes->size() > LogRecordLimit)
    return failure("too many Android text log writes");
  uint64_t Bytes = 0;
  for (const auto &Write : *Writes) {
    const auto &P = Write.Payload;
    Bytes += P.size() + sizeof(Write.ID) + sizeof(Write.Result) +
             (Write.AsyncSafe ? sizeof(Write.OpenErrno) : 0);
    if ((!Write.AsyncSafe && Write.OpenErrno) || Write.OpenErrno > 4095 ||
        Write.OpenErrno == linux_model::InterruptedCall ||
        (Write.AsyncSafe &&
         (Write.Result < -4095 || Write.Result > int64_t(P.size()) + 11)))
      return failure("invalid Android async-safe log transport result");
    if (!textBuffer(Write.ID) || P.size() < 3 || P.size() > LogPayloadLimit ||
        Bytes > LogSnapshotLimit)
      return failure(
          "invalid Android text log write payload, buffer or total size");
    // Only a full transport payload may omit either string terminator.
    // A second NUL always ends the message; trailing bytes cannot be a prefix.
    const auto Terminators = std::count(P.begin() + 1, P.end(), uint8_t(0));
    if (Terminators > 2 || (Terminators == 2 && P.back() != 0) ||
        (Terminators < 2 && P.size() != LogPayloadLimit))
      return failure(
          "invalid Android text log write payload, buffer or total size");
  }
  return llvm::Error::success();
}

llvm::Expected<std::string> Bionic::logString(LogString Text) {
  if (auto *Address = std::get_if<uint64_t>(&Text))
    return string(*Address);
  return std::get<llvm::StringRef>(Text).str();
}

llvm::Expected<bool> Bionic::setAbortMessage(LogString Message) {
  const auto &Binding = Options.Android->AbortMessageSlot;
  if (!Binding) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic =
        "Android abort-message state requires android.abort_message_slot";
    return false;
  }
  const uint64_t Slot = *Binding;
  if (!Slot)
    return true; // API 28 can be called before libc binds the slot.
  if (auto E = access(Slot, 8, Read))
    return std::move(E);
  std::array<uint8_t, 8> Pointer;
  if (auto E = CPU.read(Slot, Pointer))
    return std::move(E);
  if (llvm::support::endian::read64le(Pointer.data()))
    return true; // First message wins without reading the new argument.
  auto Text = logString(Message);
  if (!Text)
    return Text.takeError();
  const uint64_t Size =
      8 + Text->size() + 1; // LP64 abort_msg_t, including NUL.
  ProcessServiceEvent Event{
      0,
      0,
      {0, Size, linux_model::ProtRead | linux_model::ProtWrite,
       linux_model::MapPrivate | linux_model::MapAnonymous, UINT64_MAX, 0},
      std::nullopt};
  auto Raw = kernelService(linux_model::ServiceKind::Mmap, Event);
  if (!Raw)
    return Raw.takeError();
  auto Mapped = kernelResult(*Raw);
  if (!Mapped)
    return Mapped.takeError();
  if (!*Mapped)
    return false;
  // Anonymous mmap completes synchronously; Bionic alone converts errno/-1.
  const auto Address = std::get<uint64_t>(**Mapped);
  if (Address == UINT64_MAX)
    return true;
  std::vector<uint8_t> Bytes(Size);
  llvm::support::endian::write64le(Bytes.data(), Size);
  std::copy(Text->begin(), Text->end(), Bytes.begin() + 8);
  if (auto E = CPU.write(Address, Bytes))
    return std::move(E);
  // Publication follows the copy. A bad output slot retains mmap's effects.
  if (auto E = access(Slot, 8, Write))
    return std::move(E);
  llvm::support::endian::write64le(Pointer.data(), Address);
  if (auto E = CPU.write(Slot, Pointer))
    return std::move(E);
  return true;
}

BionicResult Bionic::abortMessageCall(const NativeCallEvent &Call) {
  auto Stored = setAbortMessage(Call.Arguments[0]);
  if (!Stored)
    return Stored.takeError();
  return *Stored ? value(0) : BionicResult(std::optional<BionicValue>());
}

llvm::Expected<std::optional<uint32_t>>
Bionic::textLogWrite(uint32_t ID, int32_t Priority, LogString Tag,
                     LogString Message) {
  if (auto *Address = std::get_if<uint64_t>(&Tag); Address && !*Address)
    Tag = llvm::StringRef();
  std::string Routed;
  if (ID != 1) {
    // API 28 first inspects only the radio-dispatch prefix. Unrouted tags
    // are scanned by strlen after abort-message publication, not before it.
    auto ReadTagByte = [&](size_t Index) -> llvm::Expected<uint8_t> {
      if (auto *Address = std::get_if<uint64_t>(&Tag)) {
        if (Index > UINT64_MAX - *Address)
          return failure(diagnostic::GuestPointer);
        return byte(*Address + Index);
      }
      auto Text = std::get<llvm::StringRef>(Tag);
      return Index < Text.size() ? uint8_t(Text[Index]) : uint8_t(0);
    };
    auto First = ReadTagByte(0);
    if (!First)
      return First.takeError();
    struct RadioTag {
      llvm::StringLiteral Text;
      bool Prefix;
    };
    static constexpr RadioTag Tags[] = {
        {"HTC_RIL", false}, {"RIL", true},   {"QC_RIL", true}, {"IMS", true},
        {"AT", false},      {"GSM", false},  {"STK", false},   {"SMS", false},
        {"CDMA", false},    {"PHONE", false}};
    for (const auto &Candidate : Tags) {
      if (*First != Candidate.Text.front())
        continue;
      bool Match = true;
      for (size_t I = 1; I < Candidate.Text.size() + !Candidate.Prefix; ++I) {
        auto Byte = ReadTagByte(I);
        if (!Byte)
          return Byte.takeError();
        if (*Byte != (I < Candidate.Text.size() ? Candidate.Text[I] : 0)) {
          Match = false;
          break;
        }
      }
      if (!Match)
        continue;
      auto FullTag = logString(Tag);
      if (!FullTag)
        return FullTag.takeError();
      ID = 1;
      Routed = "use-Rlog/RLOG-" + *FullTag;
      Routed.resize(std::min<size_t>(Routed.size(), 31));
      Tag = llvm::StringRef(Routed);
      break;
    }
  }
  if (Priority == 7) {
    auto Stored = setAbortMessage(Message);
    if (!Stored)
      return Stored.takeError();
    if (!*Stored)
      return std::optional<uint32_t>();
  }
  auto TagBytes = logString(Tag);
  if (!TagBytes)
    return TagBytes.takeError();
  auto MessageBytes = logString(Message);
  if (!MessageBytes)
    return MessageBytes.takeError();
  if (!textBuffer(ID)) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic =
        "Android text log write requires a main/radio/system/crash buffer";
    return std::optional<uint32_t>();
  }
  const auto Payload = textPayload(Priority, *TagBytes, *MessageBytes);
  return consumeLogWrite(
      Options.Android->TextLogWrites, Result.TextLogWritesConsumed,
      [&](const NativeTextLogWrite &Write) {
        return !Write.AsyncSafe && ID == Write.ID && Payload == Write.Payload;
      },
      "text", Result);
}

BionicResult Bionic::fortifyBufferFailure(const NativeCallEvent &Call,
                                          llvm::StringRef Function,
                                          uint64_t Count, uint64_t Extent,
                                          bool Read) {
  return fortifyFatal(Call, Function + ": prevented " + llvm::Twine(Count) +
                                "-byte " +
                                (Read ? "read from " : "write into ") +
                                llvm::Twine(Extent) + "-byte buffer");
}

BionicResult Bionic::fortifyFatal(const NativeCallEvent &Call,
                                  const llvm::Twine &Detail) {
  const std::string Message = ("FORTIFY: " + Detail).str();
  // These diagnostics contain only fixed function/action names and decimal
  // LP64 arguments, so async_safe's 1024-byte buffer always contains them.
  assert(Message.size() < LogFormattedBufferBytes);
  const llvm::StringRef Buffers[] = {Message, "\n"};
  auto Written = writeStderr(Buffers);
  if (!Written || !*Written)
    return Written;

  const auto Payload = textPayload(7, "libc", Message);
  bool Retrying = false;
  for (;;) {
    const auto *Write = nextLogWrite(
        Options.Android->TextLogWrites, Result.TextLogWritesConsumed,
        [&](const NativeTextLogWrite &W) {
          return W.AsyncSafe && W.ID == 4 && W.Payload == Payload &&
                 (!Retrying || !W.OpenErrno);
        },
        "async-safe text", Result);
    if (!Write)
      return std::optional<BionicValue>();
    if (Write->OpenErrno) {
      if (auto E = setErrno(Write->OpenErrno))
        return std::move(E);
      const llvm::StringRef Fallback[] = {"libc", ": ", Message, "\n"};
      auto FallbackResult = writeStderr(Fallback);
      if (!FallbackResult || !*FallbackResult)
        return FallbackResult;
      if (int32_t(std::get<uint64_t>(**FallbackResult)) != Write->Result) {
        Result.Stop = ProcessStopReason::UnsupportedService;
        Result.Diagnostic = "Android async-safe stderr result does not match "
                            "the next entry";
        return std::optional<BionicValue>();
      }
    } else if (Write->Result < 0) {
      // The supplied negative result is -errno at the transport boundary;
      // async_safe's raw writev wrapper leaves that errno in the calling TLS.
      if (auto E = setErrno(uint32_t(-Write->Result)))
        return std::move(E);
    }
    ++*Result.TextLogWritesConsumed;
    if (Write->OpenErrno || Write->Result != -linux_model::InterruptedCall)
      break;
    Retrying = true;
    if (auto E = access(0, 0, 0))
      return std::move(E);
  }
  // Unlike liblog's FATAL path, async_safe publishes only after the log
  // attempt (including stderr fallback), then __fortify_fatal calls abort.
  auto Stored = setAbortMessage(llvm::StringRef(Message));
  if (!Stored)
    return Stored.takeError();
  return *Stored ? abortCall(Call) : BionicResult(std::optional<BionicValue>());
}

BionicResult Bionic::textLogCall(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  const unsigned Offset = Call.Name == symbol::TextLogBufferWrite ? 1 : 0;
  auto Written = textLogWrite(Offset ? uint32_t(A[0]) : 0, int32_t(A[Offset]),
                              A[Offset + 1], A[Offset + 2]);
  if (!Written)
    return Written.takeError();
  return *Written ? value(**Written)
                  : BionicResult(std::optional<BionicValue>());
}

BionicResult Bionic::eventLogCall(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  // logd clips the combined tag/payload iovecs before touching guest bytes.
  const auto Length = std::min<uint64_t>(A[2], LogPayloadLimit - 4);
  std::vector<uint8_t> Payload(4 + Length);
  llvm::support::endian::write32le(Payload.data(), uint32_t(A[0]));
  if (Length) {
    if (auto E = access(A[1], Length, Read))
      return std::move(E);
    if (auto E = CPU.read(A[1], llvm::MutableArrayRef(Payload).drop_front(4)))
      return std::move(E);
  }
  auto Written = eventLogWrite(Payload);
  return Written ? value(*Written) : BionicResult(std::optional<BionicValue>());
}

BionicResult Bionic::unsupportedLog(llvm::StringRef Detail) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = (llvm::Twine("Android log reader: ") + Detail).str();
  return std::optional<BionicValue>();
}

llvm::Expected<uint64_t> Bionic::logToken() {
  auto Token = allocate(PageSize);
  if (!Token || !*Token)
    return Token;
  ProcessServiceEvent Protect{
      0, 0, {*Token, PageSize, 0, 0, 0, 0}, std::nullopt};
  auto R = kernelService(linux_model::ServiceKind::Mprotect, Protect);
  if (!R)
    return R.takeError();
  if (!*R || **R)
    return failure("could not protect Android log handle");
  return *Token;
}

BionicResult Bionic::logCall(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  const auto &Name = Call.Name;
  if (Name == symbol::LoggerListAlloc || Name == symbol::LoggerListAllocTime) {
    uint32_t Mode = A[0], PID = A[2];
    if ((Mode & ~Nonblock) || PID > INT32_MAX)
      return unsupportedLog(
          "mode or PID filter is outside the snapshot contract");
    LogList List;
    List.Mode = Mode;
    List.PID = PID;
    if (Name == symbol::LoggerListAllocTime) {
      // AAPCS64 packs the two uint32_t log_time members into one x register.
      List.Seconds = uint32_t(A[1]);
      List.Nanoseconds = uint32_t(A[1] >> 32);
      if (List.Nanoseconds >= 1000000000)
        return unsupportedLog("invalid start timestamp");
    } else
      List.Tail = uint32_t(A[1]);
    auto Token = logToken();
    if (!Token)
      return Token.takeError();
    if (*Token)
      LogLists.emplace(*Token, std::move(List));
    return value(*Token);
  }

  if (!A[0]) {
    if (Name == symbol::LoggerListFree || Name == symbol::LoggerOpen)
      return value(0);
    return value(negative(linux_model::InvalidArgument));
  }
  auto I = LogLists.find(A[0]);
  if (I == LogLists.end())
    return failure("invalid or retired Android logger_list handle");
  if (auto E = opaqueHandle(A[0], "logger_list"))
    return std::move(E);
  auto &List = I->second;
  for (const auto &[ID, Token] : List.Loggers)
    if (auto E = opaqueHandle(Token, "logger"))
      return std::move(E);
  if (Name == symbol::LoggerListFree) {
    for (const auto &[ID, Token] : List.Loggers)
      if (auto E = release(Token))
        return std::move(E);
    if (auto E = release(A[0]))
      return std::move(E);
    LogLists.erase(I);
    return value(0); // Void ABI, no application result is inferred.
  }
  if (Name == symbol::LoggerOpen) {
    uint32_t ID = A[1];
    if (ID >= 8)
      return value(0);
    if (ID >= 6)
      return unsupportedLog(
          "security and kernel buffer admission is not modeled");
    auto Existing = List.Loggers.find(ID);
    if (Existing != List.Loggers.end())
      return value(Existing->second);
    auto Token = logToken();
    if (!Token)
      return Token.takeError();
    if (!*Token)
      return value(0);
    List.Loggers.emplace(ID, *Token);
    // Adding a new buffer reinitializes the transport with the original
    // filters.
    List.Started = false;
    List.Position = 0;
    List.Selected.clear();
    return value(*Token);
  }

  if (List.Loggers.empty())
    return value(negative(linux_model::InvalidArgument));
  if (!Options.Android->LogRecords)
    return unsupportedLog("missing explicit android.log_records snapshot");
  if (!List.Started) {
    const auto &Records = *Options.Android->LogRecords;
    for (size_t Index = 0; Index < Records.size(); ++Index) {
      const auto &Record = Records[Index];
      if (List.Loggers.count(Record.ID) &&
          (!List.PID || Record.PID == List.PID) &&
          ((!List.Seconds && !List.Nanoseconds) ||
           std::tie(Record.Seconds, Record.Nanoseconds) >
               std::tie(List.Seconds, List.Nanoseconds)))
        List.Selected.push_back(Index);
    }
    if (List.Tail && List.Tail < List.Selected.size())
      List.Selected.erase(List.Selected.begin(),
                          List.Selected.end() - List.Tail);
    List.Started = true;
  }
  const bool Empty = List.Position == List.Selected.size();
  if (Empty && !(List.Mode & Nonblock))
    return unsupportedLog("blocking read requires a live log producer");
  if (auto E = access(A[1], MessageSize, Write))
    return std::move(E);
  std::array<uint8_t, MessageSize> Bytes{};
  uint32_t Returned = negative(linux_model::TryAgain);
  if (!Empty) {
    const auto &Record =
        (*Options.Android->LogRecords)[List.Selected[List.Position]];
    llvm::support::endian::write16le(Bytes.data(), Record.Payload.size());
    llvm::support::endian::write16le(Bytes.data() + 2, HeaderSize);
    uint32_t Fields[] = {Record.PID,         Record.TID, Record.Seconds,
                         Record.Nanoseconds, Record.ID,  Record.UID};
    for (unsigned F = 0; F < 6; ++F)
      llvm::support::endian::write32le(Bytes.data() + 4 + F * 4, Fields[F]);
    std::copy(Record.Payload.begin(), Record.Payload.end(),
              Bytes.begin() + HeaderSize);
    Returned = HeaderSize + Record.Payload.size();
  }
  // logdRead clears sizeof(log_msg), including the final alignment padding,
  // before both a successful packet and nonblocking end-of-snapshot.
  if (auto E = CPU.write(A[1], Bytes))
    return std::move(E);
  if (!Empty)
    ++List.Position;
  return value(Returned);
}
} // namespace neverd::emulation::android_model
