//===- LoadCancellationTests.cpp - Transactional input preparation --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Cancellation and retry of captured native images and their byte identity.
//===----------------------------------------------------------------------===//

#include "../web/NativeFixture.h"
#include "gtest/gtest.h"

#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/support/BinaryLoading.h"
#include "neverd/support/Limits.h"
#include "neverd/support/LoadProgress.h"
#include "neverd/support/SHA256.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/SHA256.h"

#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace neverd {
namespace {

struct StopAtPhase {
  std::string Phase;
  unsigned long long Minimum = 0;
  bool Requested = false;
  unsigned long long Observed = 0, Total = 0;
  static void report(void *User, const char *Phase, unsigned long long Done,
                     unsigned long long Total, const char *) {
    auto &State = *static_cast<StopAtPhase *>(User);
    if (State.Phase == Phase && Done >= State.Minimum) {
      State.Observed = Done;
      State.Total = Total;
      State.Requested = true;
    }
  }
  static int stop(void *User) {
    return static_cast<StopAtPhase *>(User)->Requested;
  }
  LoadProgress control() { return {report, this, stop, this}; }
};

TEST(InputDigest, ControlledHashRetainsLLVMIdentityAcrossChunkAndPaddingEdges) {
  constexpr size_t Chunk = limits::support::kLoadByteCheckpointBytes;
  std::mt19937 Random(0x71ae);
  std::vector<uint8_t> Bytes(Chunk * 2 + 65);
  for (auto &Byte : Bytes)
    Byte = static_cast<uint8_t>(Random());
  LoadProgress Progress;
  Progress.Stop = [](void *) { return 0; };
  for (const size_t Base : {size_t(0), Chunk, Chunk * 2}) {
    for (const size_t Tail : {0U, 1U, 55U, 56U, 63U, 64U, 65U}) {
      const llvm::ArrayRef<uint8_t> Data(Bytes.data(), Base + Tail);
      EXPECT_EQ(sha256(Data, Progress), llvm::SHA256::hash(Data))
          << Base + Tail;
    }
  }
}

TEST(InputDigest, ControlledHashStopsBeforeReadingTheWholeImageAndRetries) {
  std::vector<uint8_t> Bytes(limits::support::kLoadByteCheckpointBytes * 3, 37);
  StopAtPhase State{"hash", 1};
  EXPECT_THROW(sha256(Bytes, State.control()), LoadCancelled);
  EXPECT_GT(State.Observed, 0U);
  EXPECT_LT(State.Observed, Bytes.size());
  State.Requested = false;
  State.Phase.clear();
  EXPECT_EQ(sha256(Bytes, State.control()), llvm::SHA256::hash(Bytes));
}

TEST(NativeLoadCancellation, EveryFormatDiscardsStoppedPreparationAndRetries) {
  for (auto Bytes : {web::test::nativeELF(), web::test::nativeELF(true),
                     web::test::nativePE(), web::test::nativeMachO()}) {
    // Extra file bytes exercise hashing/copying independently of mapped code.
    Bytes.resize(limits::support::kLoadByteCheckpointBytes * 2 + 37, '\x5a');
    llvm::MemoryBufferRef Buffer(Bytes, "captured-native-input");
    auto Fresh = loadBinaryBuffer(Buffer);
    ASSERT_TRUE(bool(Fresh)) << llvm::toString(Fresh.takeError());
    SCOPED_TRACE(static_cast<unsigned>(Fresh->Format));
    for (const auto *Phase :
         {"identify", "hash", "copy", "parse", "load-discovery", "metadata"}) {
      SCOPED_TRACE(Phase);
      StopAtPhase State{Phase, 1};
      BinaryLoadOptions Options;
      Options.Progress = State.control();
      auto Stopped = loadBinaryBuffer(Buffer, Options);
      ASSERT_FALSE(bool(Stopped));
      EXPECT_EQ(llvm::errorToErrorCode(Stopped.takeError()),
                std::make_error_code(std::errc::operation_canceled));
      ASSERT_TRUE(State.Requested);
      if (State.Phase == "hash" || State.Phase == "copy")
        EXPECT_LT(State.Observed, State.Total);
      State.Requested = false;
      State.Phase.clear();
      auto Retried = loadBinaryBuffer(Buffer, Options);
      ASSERT_TRUE(bool(Retried)) << llvm::toString(Retried.takeError());
      EXPECT_EQ(Retried->InputFileSHA256, Fresh->InputFileSHA256);
      EXPECT_EQ(Retried->InputFileSHA256,
                llvm::SHA256::hash(llvm::arrayRefFromStringRef(Bytes)));
      EXPECT_EQ(Retried->Raw, Fresh->Raw);
      EXPECT_EQ(Retried->Entry, Fresh->Entry);
      EXPECT_EQ(Retried->Arch, Fresh->Arch);
      EXPECT_EQ(Retried->Symbols.size(), Fresh->Symbols.size());
      ASSERT_EQ(Retried->Segments.size(), Fresh->Segments.size());
      for (size_t I = 0; I < Fresh->Segments.size(); ++I)
        EXPECT_EQ(Retried->Segments[I].Data, Fresh->Segments[I].Data);
    }
  }
}

TEST(NativeLoadCancellation, AReusedLoaderNeverRetainsTheStopContext) {
  const auto Bytes = web::test::nativeELF();
  llvm::MemoryBufferRef Buffer(Bytes, "captured-native-input");
  auto Reader = Loader::create(BinaryFormat::ELF);
  ASSERT_TRUE(Reader);
  {
    StopAtPhase State{"copy"};
    auto Stopped = Reader->loadBufferWithProgress(Buffer, State.control());
    ASSERT_FALSE(bool(Stopped));
    EXPECT_EQ(llvm::errorToErrorCode(Stopped.takeError()),
              std::make_error_code(std::errc::operation_canceled));
    ASSERT_TRUE(State.Requested);
    // The same loader's legacy entry point must not consult that stop.
    auto Fresh = Reader->loadBuffer(Buffer);
    ASSERT_TRUE(bool(Fresh)) << llvm::toString(Fresh.takeError());
  }
  auto Fresh = Reader->loadBufferWithProgress(Buffer, {});
  ASSERT_TRUE(bool(Fresh)) << llvm::toString(Fresh.takeError());
}

TEST(NativeLoadCancellation, DataPointerScanningStopsAfterAPartialPrefix) {
  const auto MakeImage = [] {
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::ELF;
    Segment Code;
    Code.VA = 0x10000000;
    Code.Size = Code.FileSz = 32;
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Code.Data.resize(Code.Size, 0xcc);
    for (const size_t Offset : {0U, 16U}) {
      Code.Data[Offset] = 0x55;
      Code.Data[Offset + 1] = 0x48;
      Code.Data[Offset + 2] = 0x89;
      Code.Data[Offset + 3] = 0xe5;
      Code.Data[Offset + 4] = 0xc3;
    }
    Segment Data;
    Data.VA = 0x20000000;
    Data.Size = Data.FileSz = limits::support::kLoadByteCheckpointBytes * 4;
    Data.Flags = SegmentFlags::Readable;
    Data.Data.resize(Data.Size, 0);
    llvm::support::endian::write64le(Data.Data.data(), Code.VA);
    llvm::support::endian::write64le(
        Data.Data.data() + limits::support::kLoadByteCheckpointBytes * 3,
        Code.VA + 16);
    Image.Segments.push_back(std::move(Code));
    Image.Segments.push_back(std::move(Data));
    return Image;
  };
  auto Fresh = MakeImage();
  scanDataFuncPointers(Fresh);
  ASSERT_EQ(Fresh.Symbols.size(), 2U);
  auto Interrupted = MakeImage();
  LoadProgress Progress;
  Progress.StopUser = &Interrupted;
  Progress.Stop = [](void *Context) {
    return !static_cast<BinaryImage *>(Context)->Symbols.empty() ? 1 : 0;
  };
  EXPECT_THROW(scanDataFuncPointers(Interrupted, Progress), LoadCancelled);
  ASSERT_EQ(Interrupted.Symbols.size(), 1U);
  scanDataFuncPointers(Interrupted);
  ASSERT_EQ(Interrupted.Symbols.size(), Fresh.Symbols.size());
  for (size_t I = 0; I < Fresh.Symbols.size(); ++I)
    EXPECT_EQ(Interrupted.Symbols[I].Addr, Fresh.Symbols[I].Addr);
}

TEST(NativeLoadCancellation, AStoppedFileLoadDoesNotNeedToReadThePath) {
  auto Reader = Loader::create(BinaryFormat::ELF);
  ASSERT_TRUE(Reader);
  LoadProgress Progress;
  Progress.Stop = [](void *) { return 1; };
  auto Stopped =
      Reader->loadWithProgress("/nonexistent/cancelled.elf", Progress);
  ASSERT_FALSE(bool(Stopped));
  EXPECT_EQ(llvm::errorToErrorCode(Stopped.takeError()),
            std::make_error_code(std::errc::operation_canceled));
}

} // namespace
} // namespace neverd
