//===- AndroidStdioRead.cpp - Buffered and direct guest input ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Read API 28 byte streams through one kernel-owned descriptor cursor.
#include "AndroidInternal.h"
#include "AndroidThreads.h"

#include "neverd/emulation/CPU.h"

#include <algorithm>

namespace neverd::emulation::android_model {
llvm::Expected<std::optional<bool>>
Bionic::prepareStreamRefill(InputStream &Stream) {
  using Reply = std::optional<bool>;
  auto Readable = prepareStreamRead(Stream);
  if (!Readable || !*Readable || !**Readable)
    return Readable;
  auto Ready = prepareStreamBuffer(Stream);
  if (!Ready)
    return Ready.takeError();
  if (!*Ready)
    return Reply();
  if (Stream.BufferMode) {
    for (auto &[Token, Other] : InputStreams) {
      if (&Other == &Stream || Other.BufferMode != 1)
        continue;
      auto Flushed = flushStream(Other);
      if (!Flushed)
        return Flushed.takeError();
      if (!*Flushed)
        return Reply();
      // API 28 deliberately ignores other streams' IO errors.
    }
  }
  return Reply(true);
}

llvm::Error Bionic::copyStream(InputStream &Stream, uint64_t Output,
                               uint64_t Size) {
  if (auto E = access(Stream.Buffer + Stream.Begin, Size, Read))
    return E;
  if (auto E = access(Output, Size, Write))
    return E;
  if (Size && (Output <= Stream.Buffer + Stream.Begin
                   ? Stream.Buffer + Stream.Begin - Output < Size
                   : Output - Stream.Buffer - Stream.Begin < Size))
    return failure("overlapping stdio buffer and read destination");
  std::vector<uint8_t> Bytes(Size);
  if (auto E = CPU.read(Stream.Buffer + Stream.Begin, Bytes))
    return E;
  if (auto E = CPU.write(Output, Bytes))
    return E;
  Stream.Begin += Size;
  return llvm::Error::success();
}

class Bionic::StreamReadWait final : public NativeWait {
public:
  explicit StreamReadWait(const NativeCallEvent &Call)
      : Block(Call.Name == symbol::Fread ||
              Call.Name == symbol::FreadUnlocked ||
              Call.Name == symbol::FreadChk),
        Line(Call.Name == symbol::Fgets || Call.Name == symbol::FgetsUnlocked ||
             Call.Name == symbol::FgetsChk),
        Handle(Call.Arguments[Block  ? 3
                              : Line ? 2
                                     : 0]),
        Output(Call.Arguments[0]), Size(Call.Arguments[1]),
        Count(Call.Arguments[2]) {}
  bool ready() const override { return Pending->ready(); }
  std::optional<uint64_t> nextWake() const override {
    return Pending->nextWake();
  }
  BionicResult resume(Bionic &LibC) override {
    auto Found = LibC.inputStream(Handle);
    if (!Found)
      return Found.takeError();
    auto &Stream = **Found;
    Stream.Reader = LibC.threadID();
    auto R = run(LibC, Stream);
    if (!R || !*R || !std::holds_alternative<GuestThreadWait>(**R))
      Stream.Reader.reset();
    return R;
  }

private:
  bool Block, Line, Started = false, Direct = false;
  uint64_t Handle, Output, Size, Count, Requested = 1, Done = 0;
  std::unique_ptr<linux_model::KernelWait> Pending;

  llvm::Expected<std::optional<uint64_t>> read(Bionic &LibC,
                                               InputStream &Stream) {
    // The kernel retains the open description and timeout of this particular
    // read. The outer libc operation retains its buffer, arguments and
    // progress.
    const bool Resuming = bool(Pending);
    auto R =
        Resuming
            ? Pending->resume(LibC.Result)
            : LibC.streamService(linux_model::ServiceKind::Read, Stream.FD,
                                 Direct ? Output + Done : Stream.Buffer,
                                 Direct ? Requested - Done : Stream.Capacity);
    if (!R)
      return R.takeError();
    if (!*R) {
      if (!Pending && LibC.Threads && LibC.Threads->kernel())
        Pending = std::move(LibC.Threads->kernel()->Pending);
      return std::optional<uint64_t>();
    }
    Pending.reset();
    if (**R >= uint64_t(0) - 4095) {
      if (Resuming)
        if (auto E = LibC.setErrno(uint64_t(0) - **R))
          return std::move(E);
      Stream.Error = true;
      return std::optional<uint64_t>(0);
    }
    if (!**R)
      Stream.EndOfFile = true;
    return *R;
  }

  BionicResult run(Bionic &LibC, InputStream &Stream) {
    if (!Started) {
      Started = true;
      if (Block) {
        if (Size && Count > UINT64_MAX / Size) {
          Stream.Error = true;
          return LibC.streamError(linux_model::ValueOverflow, 0);
        }
        Requested = Size * Count;
        if (!Requested)
          return value(0);
        if (Requested > INT32_MAX)
          return failure("stdio read exceeds the supported signed-int extent");
        auto Ready = LibC.prepareStreamBuffer(Stream);
        if (!Ready)
          return Ready.takeError();
        if (!*Ready)
          return std::optional<BionicValue>();
      } else if (Line) {
        if (int32_t(Size) <= 0)
          return LibC.streamError(linux_model::InvalidArgument, 0);
        Requested = uint32_t(Size) - 1;
      }
    }
    while (Done < Requested) {
      if (auto E = LibC.access(0, 0, 0))
        return std::move(E);
      if (Output > UINT64_MAX - Done)
        return failure("stdio destination address overflow");
      uint64_t Available = Stream.End - Stream.Begin;
      if (!Available) {
        if (!Pending) {
          Direct |= Block && Requested - Done > Stream.Capacity;
          if (!Direct) {
            auto Prepared = LibC.prepareStreamRefill(Stream);
            if (!Prepared)
              return Prepared.takeError();
            if (!*Prepared)
              return std::optional<BionicValue>();
            if (!**Prepared) {
              Stream.Begin = Stream.End = 0;
              break;
            }
          }
        }
        auto Bytes = read(LibC, Stream);
        if (!Bytes)
          return Bytes.takeError();
        if (!*Bytes) {
          if (Pending && !Pending->ready())
            return BionicValue(GuestThreadWait{});
          return std::optional<BionicValue>();
        }
        if (Direct) {
          if (!**Bytes)
            break;
          Done += **Bytes;
          continue;
        }
        Stream.Begin = 0;
        Stream.End = Available = **Bytes;
        if (!Available)
          break;
      }
      if (!Block && !Line) {
        auto Character = LibC.byte(Stream.Buffer + Stream.Begin);
        if (!Character)
          return Character.takeError();
        ++Stream.Begin;
        return value(*Character);
      }
      uint64_t Bytes = std::min(Available, Requested - Done);
      bool Newline = false;
      if (Line) {
        for (uint64_t I = 0; I < Bytes; ++I) {
          auto Character = LibC.byte(Stream.Buffer + Stream.Begin + I);
          if (!Character)
            return Character.takeError();
          if (*Character == '\n') {
            Bytes = I + 1;
            Newline = true;
            break;
          }
        }
      }
      if (auto E = LibC.copyStream(Stream, Output + Done, Bytes))
        return std::move(E);
      Done += Bytes;
      if (Newline)
        break;
    }
    if (Block)
      return value(Done / Size);
    if (!Line)
      return value(UINT32_MAX);
    if (!Done && Requested)
      return value(0);
    if (Output > UINT64_MAX - Done)
      return failure("stdio destination address overflow");
    if (auto E = LibC.access(Output + Done, 1, Write))
      return std::move(E);
    const uint8_t Nul = 0;
    if (auto E = LibC.CPU.write(Output + Done, llvm::ArrayRef(&Nul, 1)))
      return std::move(E);
    return value(Output);
  }
};

BionicResult Bionic::readStream(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  if (Call.Name == symbol::FgetsChk) {
    if (int32_t(A[1]) < 0)
      return fortifyFatal(Call, "fgets: buffer size " +
                                    llvm::Twine(int32_t(A[1])) + " < 0");
    if (uint32_t(A[1]) > A[3])
      return fortifyBufferFailure(Call, "fgets", uint32_t(A[1]), A[3]);
  }
  // Overflow deliberately takes fread's ordinary EOVERFLOW path in API 28.
  if (Call.Name == symbol::FreadChk && (!A[1] || A[2] <= UINT64_MAX / A[1]) &&
      A[1] * A[2] > A[4])
    return fortifyBufferFailure(Call, "fread", A[1] * A[2], A[4]);
  auto Pending = std::make_unique<StreamReadWait>(Call);
  auto R = Pending->resume(*this);
  if (R && *R && std::holds_alternative<GuestThreadWait>(**R))
    Threads->waitNative(std::move(Pending));
  return R;
}
} // namespace neverd::emulation::android_model
