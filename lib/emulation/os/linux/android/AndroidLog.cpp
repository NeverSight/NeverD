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
std::optional<uint32_t>
consumeLogWrite(const std::optional<std::vector<Entry>> &Writes,
                std::optional<uint64_t> &Consumed, Matches Match,
                llvm::StringRef Kind, ProcessResult &Result) {
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
    return std::nullopt;
  }
  return uint32_t((*Writes)[(*Consumed)++].Result);
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
    Bytes += P.size() + sizeof(Write.ID) + sizeof(Write.Result);
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

std::optional<uint32_t> Bionic::textLogWrite(uint32_t ID, int32_t Priority,
                                             llvm::StringRef Tag,
                                             llvm::StringRef Message) {
  std::string Routed;
  if (ID != 1 && (Tag == "HTC_RIL" || Tag.starts_with("RIL") ||
                  Tag.starts_with("QC_RIL") || Tag.starts_with("IMS") ||
                  Tag == "AT" || Tag == "GSM" || Tag == "STK" || Tag == "SMS" ||
                  Tag == "CDMA" || Tag == "PHONE")) {
    ID = 1;
    Routed = ("use-Rlog/RLOG-" + Tag).str();
    Routed.resize(std::min<size_t>(Routed.size(), 31)); // API 28 tmp_tag[32].
    Tag = Routed;
  }
  const char *Boundary = nullptr;
  if (!textBuffer(ID))
    Boundary = "requires a main/radio/system/crash buffer";
  else if (Priority == 7)
    Boundary = "requires native fatal abort-message state";
  if (Boundary) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic =
        (llvm::Twine("Android text log write ") + Boundary).str();
    return std::nullopt;
  }
  std::vector<uint8_t> Payload;
  Payload.reserve(
      std::min<size_t>(LogPayloadLimit, Tag.size() + Message.size() + 3));
  Payload.push_back(uint8_t(Priority));
  // logd clips the concatenated priority/tag/message iovecs. A cut may land
  // inside either string or just after its NUL; no replacement NUL is added.
  for (llvm::StringRef Text : {Tag, Message}) {
    auto Prefix = Text.take_front(LogPayloadLimit - Payload.size());
    Payload.insert(Payload.end(), Prefix.begin(), Prefix.end());
    if (Payload.size() < LogPayloadLimit)
      Payload.push_back(0);
  }
  return consumeLogWrite(
      Options.Android->TextLogWrites, Result.TextLogWritesConsumed,
      [&](const NativeTextLogWrite &Write) {
        return ID == Write.ID && Payload == Write.Payload;
      },
      "text", Result);
}

BionicResult Bionic::textLogCall(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  const unsigned Offset = Call.Name == symbol::TextLogBufferWrite ? 1 : 0;
  auto Tag =
      A[Offset + 1] ? string(A[Offset + 1]) : llvm::Expected<std::string>("");
  if (!Tag)
    return Tag.takeError();
  auto Message = string(A[Offset + 2]);
  if (!Message)
    return Message.takeError();
  auto Written = textLogWrite(Offset ? uint32_t(A[0]) : 0, int32_t(A[Offset]),
                              *Tag, *Message);
  return Written ? value(*Written) : BionicResult(std::optional<BionicValue>());
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
