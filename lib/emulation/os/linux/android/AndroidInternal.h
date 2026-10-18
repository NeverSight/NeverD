//===- AndroidInternal.h - Android environment boundaries ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_ANDROIDINTERNAL_H
#define NEVERD_EMULATION_ANDROIDINTERNAL_H
#include "../kernel/LinuxServices.h"
#include "AndroidJNI.h"
#include "AndroidProperties.h"

#include "neverd/emulation/AndroidNative.h"
#include "neverd/emulation/IntegerABI.h"
#include "neverd/loader/ELF/ELFProgramLinking.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <map>
#include <variant>

namespace neverd::emulation::android_model {
namespace symbol {
#define NEVERD_ANDROID_SYMBOL(Name, Text) inline constexpr char Name[] = Text;
#include "AndroidSymbols.def"
#undef NEVERD_ANDROID_SYMBOL
} // namespace symbol
namespace diagnostic {
#define NEVERD_ANDROID_DIAGNOSTIC(Name, Text)                                  \
  inline constexpr char Name[] = Text;
#include "AndroidDiagnostics.def"
#undef NEVERD_ANDROID_DIAGNOSTIC
} // namespace diagnostic
inline constexpr uint64_t PageSize = 4096;
enum class SysconfName : uint32_t {
#define NEVERD_ANDROID_SYSCONF(Name, Value) Name = Value,
#include "AndroidSysconf.def"
#undef NEVERD_ANDROID_SYSCONF
};
namespace thread_attribute_abi {
#define NEVERD_ANDROID_THREAD_ATTRIBUTE_VALUE(Name, Value)                     \
  inline constexpr unsigned Name = Value;
#include "AndroidThreadAttributes.def"
#undef NEVERD_ANDROID_THREAD_ATTRIBUTE_VALUE
} // namespace thread_attribute_abi
namespace once_abi {
#define NEVERD_ANDROID_ONCE_VALUE(Name, Value)                                 \
  inline constexpr unsigned Name = Value;
#include "AndroidOnce.def"
#undef NEVERD_ANDROID_ONCE_VALUE
} // namespace once_abi
inline constexpr uint64_t TLSAddress = 0x7000000000;
inline constexpr uint64_t StdioAddress = TLSAddress - PageSize;
inline constexpr uint64_t ThunkBase = TLSAddress + PageSize;
inline constexpr uint64_t ErrnoAddress = TLSAddress + 2 * 8;
inline constexpr uint64_t GuardAddress = TLSAddress + 5 * 8;
inline constexpr uint64_t LinkerErrorAddress = TLSAddress + 0x200;
inline constexpr uint64_t LinkerErrorSlot = TLSAddress + 6 * 8;
inline constexpr uint64_t StackGuard = 0x91ab62e5347c2800;
inline constexpr uint16_t ModelTrap = 0x4e44;
inline constexpr uint64_t ReturnPC = ThunkBase;
inline llvm::Error failure(llvm::Twine Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 diagnostic::FailurePrefix + Message);
}
struct LinkedImage {
  uint64_t Entry, InitialBreak;
  std::vector<uint64_t> Constructors;
  std::map<uint64_t, std::string> Imports;
  std::map<uint64_t, std::string> DynamicProviders;
  std::map<std::string, std::map<std::string, uint64_t>> Libraries;
  PropertyTable Properties;
  std::vector<uint8_t> JNIImage;
  uint64_t JNIThunkBegin = 0, JNIThunkEnd = 0;
};
llvm::Error validateAuxiliaryVector(const AndroidNativeOptions &Options);
llvm::Error validateCalendar(const AndroidNativeOptions &Options);
llvm::Error initializeCalendar(AddressSpace &Space,
                               const AndroidNativeOptions &Options,
                               const LinkedImage &Linked);
llvm::Error initializeJNI(AddressSpace &Space, LinkedImage &Linked);
llvm::Expected<PropertyTable>
initializeProperties(AddressSpace &Space, const ProcessOptions &Options,
                     const ExecutionBudget &Budget);
llvm::Expected<LinkedImage> loadImage(AddressSpace &Space,
                                      const BinaryImage &Image,
                                      const ProcessOptions &Options);
/// Populate an already admitted region. File input is bounded by its mapped
/// extent and the workload's preparation deadline, before any CPU is created.
llvm::Error initializeMemoryRegion(AddressSpace &Space,
                                   const NativeMemoryRegion &Region,
                                   const ExecutionBudget &Budget);
struct OnceCallback {
  uint64_t Control;
};
struct FinalizeCallback {
  uint64_t DSO;
};
struct PropertyCallback {
  size_t Next;
  uint64_t Entry, Cookie;
};
struct PropertyReadCallback {};
struct GuestCallback {
  uint64_t Entry;
  std::vector<uint64_t> Arguments;
  std::variant<OnceCallback, FinalizeCallback, PropertyCallback,
               PropertyReadCallback>
      Continuation;
};
struct GuestThreadWait {};
struct GuestThreadExit {
  uint64_t Value;
};
/// A returning model either completes now or suspends for a guest callback.
using BionicValue =
    std::variant<uint64_t, GuestCallback, GuestThreadWait, GuestThreadExit>;
using BionicResult = llvm::Expected<std::optional<BionicValue>>;
class GuestThreads;
class Bionic {
public:
  Bionic(ExecutionBackend &CPU, linux_model::LinuxServices &Kernel,
         const linux_model::MemoryLayout &Layout, const ProcessOptions &Options,
         ProcessResult &Result, ExecutionBudget &Budget,
         const LinkedImage &Linked, GuestThreads *Threads = nullptr)
      : CPU(CPU), Kernel(Kernel), Layout(Layout), Options(Options),
        Result(Result), Budget(Budget), Linked(Linked), Threads(Threads) {}
  bool timedOut() const { return Expired; }
  BionicResult invoke(NativeCallEvent &Call);
  BionicResult finishCallback(const GuestCallback &Callback);
  BionicResult resumeMutex(uint64_t Address, uint16_t Attributes);
  BionicResult resumeSemaphore(uint64_t Address, bool Shared);
  llvm::Error jniThreadExit();
  llvm::Error initializeDex();

private:
  ExecutionBackend &CPU;
  linux_model::LinuxServices &Kernel;
  const linux_model::MemoryLayout &Layout;
  const ProcessOptions &Options;
  ProcessResult &Result;
  ExecutionBudget &Budget;
  const LinkedImage &Linked;
  GuestThreads *Threads;
  uint64_t tlsAddress() const;
  uint64_t threadID() const;
  bool Expired = false;
  // Bionic's default generator belongs to the guest process, not a thread or
  // the host C library. Calls run atomically inside the guest scheduler.
  uint32_t RandomWords[31] = {};
  unsigned RandomRear = 0;
  bool RandomInitialized = false;
  void seedRandom(uint32_t Seed);
  uint32_t nextRandom();
  BionicResult randomCall(const NativeCallEvent &Call);
  struct LibraryState {
    uint64_t Handle = 0, References = 0;
  };
  std::map<std::string, LibraryState> OpenLibraries;
  std::map<uint64_t, std::string> Handles;
  uint64_t NextHandle = 1;
  bool isResident(const std::string &Library) const;
  struct Allocation {
    uint64_t Size, MappedSize;
  };
  std::map<uint64_t, Allocation> Allocations;
  struct ExitCallback {
    uint64_t Entry, Argument, DSO;
  };
  std::vector<ExitCallback> ExitCallbacks;
  JNIState JNI;
  BionicResult jniCall(const NativeCallEvent &Call);
  BionicResult jniVM(const NativeCallEvent &Call);
  BionicResult jniObjects(unsigned Slot, const NativeCallEvent &Call);
  BionicResult jniStrings(unsigned Slot, const NativeCallEvent &Call);
  BionicResult jniArrays(unsigned Slot, const NativeCallEvent &Call);
  bool jniAssignable(llvm::StringRef Source, llvm::StringRef Destination) const;
  BionicResult jniMethods(unsigned Slot, const NativeCallEvent &Call);
  BionicResult jniFields(unsigned Slot, const NativeCallEvent &Call);
  BionicResult
  runDex(const std::function<mobile::dalvik::ExecutionResult(
             const mobile::dalvik::AdmitInstruction &, uint64_t)> &Body);
  BionicResult allocateDex(llvm::StringRef Class, bool Initialized);
  // Null Method requests class initialization without a method invocation.
  BionicResult executeDex(const mobile::dalvik::Method *Method,
                          llvm::ArrayRef<uint64_t> Arguments,
                          llvm::StringRef Class = {});
  llvm::Error jniIdentity(uint64_t Address, uint64_t Expected);
  llvm::Expected<JNIState::Thread *> jniThread(uint64_t Environment);
  llvm::Expected<std::shared_ptr<JNIObject>> jniObject(uint64_t Reference);
  llvm::Expected<uint64_t> jniReference(std::shared_ptr<JNIObject> Object,
                                        bool Global = false);
  llvm::Expected<std::shared_ptr<JNIObject>> jniClass(llvm::StringRef Name);
  llvm::Expected<std::shared_ptr<JNIObject>>
  jniNewObject(JNIObject::Kind Kind, llvm::StringRef Class, uint64_t Bytes);
  llvm::Error jniException(llvm::StringRef Class);
  llvm::Expected<uint64_t> jniBorrow(std::shared_ptr<JNIObject> Object,
                                     llvm::ArrayRef<uint8_t> Bytes,
                                     uint64_t IsCopy, unsigned Slot);
  BionicResult jniRelease(unsigned Slot, const NativeCallEvent &Call);
  llvm::Error jniPut(uint64_t Address, uint64_t Value, unsigned Size);
  llvm::Error access(uint64_t Address, uint64_t Size, unsigned Permissions);
  llvm::Expected<uint8_t> byte(uint64_t Address);
  llvm::Expected<std::string> string(uint64_t Address);
  llvm::Expected<uint64_t> allocate(uint64_t Size, uint8_t FillByte = 0);
  llvm::Error release(uint64_t Address);
  llvm::Error setErrno(uint32_t Value);
  BionicResult linkerError(llvm::StringRef Message, uint64_t ReturnValue = 0);
  static std::optional<BionicValue> value(uint64_t Value) {
    return BionicValue(Value);
  }
  BionicResult unsupportedLinking(llvm::StringRef Detail);
  BionicResult signalAction(const NativeCallEvent &Call);
  llvm::Expected<std::optional<uint64_t>>
  kernelService(linux_model::ServiceKind Kind,
                const ProcessServiceEvent &Event);
  BionicResult kernelCall(std::optional<linux_model::ServiceKind> Kind,
                          const NativeCallEvent &Call);
  BionicResult kernelResult(std::optional<uint64_t> Returned);
  BionicResult usleep(const NativeCallEvent &Call);
  BionicResult openLibrary(NativeCallEvent &Call);
  BionicResult lookupSymbol(NativeCallEvent &Call);
  BionicResult closeLibrary(NativeCallEvent &Call);
  BionicResult getLinkerError(NativeCallEvent &Call);
  BionicResult unsupportedImport(const NativeCallEvent &Call);
  BionicResult failCall(const NativeCallEvent &Call);
  struct DirectoryStream {
    uint32_t FD;
    uint64_t Token, Begin = 0, End = 0, Position = 0;
  };
  std::map<uint64_t, DirectoryStream> DirectoryStreams;
  BionicResult openDirectory(const NativeCallEvent &Call);
  BionicResult directoryCall(const NativeCallEvent &Call);
  llvm::Error opaqueHandle(uint64_t Handle, llvm::StringRef Kind = "FILE");
  struct LogList {
    uint32_t Mode = 0, Tail = 0, PID = 0, Seconds = 0, Nanoseconds = 0;
    std::map<uint32_t, uint64_t> Loggers;
    std::vector<size_t> Selected;
    size_t Position = 0;
    bool Started = false;
  };
  std::map<uint64_t, LogList> LogLists;
  llvm::Expected<uint64_t> logToken();
  BionicResult logCall(const NativeCallEvent &Call);
  BionicResult unsupportedLog(llvm::StringRef Detail);
  struct InputStream {
    uint32_t FD;
    uint64_t Token, Buffer = 0, Capacity = 0, Begin = 0, End = 0;
    bool OwnBuffer = false, EndOfFile = false, Error = false;
    bool CanRead = true, CanWrite = false, Writing = false;
    uint32_t BufferMode = 0;
    uint64_t Pending = 0;
  };
  std::map<uint64_t, InputStream> InputStreams;
  BionicResult openStream(const NativeCallEvent &Call);
  BionicResult streamCall(const NativeCallEvent &Call);
  BionicResult readStream(const NativeCallEvent &Call);
  BionicResult writeStream(const NativeCallEvent &Call);
  llvm::Expected<std::optional<bool>> flushStream(InputStream &Stream);
  llvm::Expected<std::optional<bool>> prepareStreamRead(InputStream &Stream);
  BionicResult bufferStream(const NativeCallEvent &Call);
  llvm::Expected<InputStream *> inputStream(uint64_t Handle);
  llvm::Expected<std::optional<uint64_t>> streamBufferSize(uint32_t FD);
  llvm::Expected<bool> prepareStreamBuffer(InputStream &Stream);
  llvm::Expected<std::optional<uint64_t>>
  streamService(linux_model::ServiceKind Kind, uint32_t FD, uint64_t A1 = 0,
                uint64_t A2 = 0);
  llvm::Expected<std::optional<uint64_t>>
  streamRead(InputStream &Stream, uint64_t Output, uint64_t Size);
  llvm::Expected<bool> refillStream(InputStream &Stream);
  llvm::Error copyStream(InputStream &Stream, uint64_t Output, uint64_t Size);
  BionicResult streamError(uint32_t Error, uint64_t ReturnValue);
  BionicResult malloc(const NativeCallEvent &Call);
  BionicResult calloc(const NativeCallEvent &Call);
  BionicResult realloc(const NativeCallEvent &Call);
  BionicResult free(const NativeCallEvent &Call);
  BionicResult copyMemory(const NativeCallEvent &Call);
  BionicResult setMemory(const NativeCallEvent &Call);
  BionicResult compareMemory(const NativeCallEvent &Call);
  BionicResult descriptorSet(const NativeCallEvent &Call);
  BionicResult stringLength(const NativeCallEvent &Call);
  BionicResult decimalInteger(const NativeCallEvent &Call);
  llvm::Expected<std::optional<uint64_t>>
  decimalValue(bool Narrow,
               llvm::function_ref<llvm::Expected<uint8_t>(uint64_t)> ReadByte);
  BionicResult compareString(const NativeCallEvent &Call);
  BionicResult getErrno(const NativeCallEvent &Call);
  BionicResult getPageSize(const NativeCallEvent &Call);
  BionicResult getAPILevel(const NativeCallEvent &Call);
  BionicResult queryConfiguration(const NativeCallEvent &Call);
  BionicResult queryAuxiliaryVector(const NativeCallEvent &Call);
  BionicResult getTime(const NativeCallEvent &Call);
  BionicResult calendarTime(const NativeCallEvent &Call);
  BionicResult getProperty(const NativeCallEvent &Call);
  llvm::Expected<const PropertyRecord *> propertyAt(uint64_t Address);
  llvm::Expected<const PropertyRecord *> propertyNamed(llvm::StringRef Name);
  llvm::Error propertyString(uint64_t Address, llvm::StringRef Text);
  BionicResult findProperty(const NativeCallEvent &Call);
  BionicResult findPropertyNth(const NativeCallEvent &Call);
  BionicResult readProperty(const NativeCallEvent &Call);
  BionicResult propertySerial(const NativeCallEvent &Call);
  BionicResult readPropertyCallback(const NativeCallEvent &Call);
  BionicResult forEachProperty(const NativeCallEvent &Call);
  BionicResult nextProperty(const PropertyCallback &Callback);
  llvm::Error propertyCallbackEntry(uint64_t Entry);
  BionicResult syscall(const NativeCallEvent &Call);
  BionicResult socketTransfer(const NativeCallEvent &Call);
  BionicResult addressText(const NativeCallEvent &Call);
  BionicResult resolveAddress(const NativeCallEvent &Call);
  BionicResult freeAddresses(const NativeCallEvent &Call);
  std::map<uint64_t, uint64_t> ResolverNodes;
  BionicResult threadCall(const NativeCallEvent &Call);
  BionicResult finalizeCall(const NativeCallEvent &Call);
  BionicResult once(const NativeCallEvent &Call);
  class ThreadAttributes;
  BionicResult threadAttributes(const NativeCallEvent &Call);
  llvm::Error finishOnce(const OnceCallback &Callback);
  BionicResult registerExit(const NativeCallEvent &Call);
  BionicResult finalize(uint64_t DSO);
  BionicResult tokenize(const NativeCallEvent &Call);
  BionicResult findCharacter(const NativeCallEvent &Call);
  class ArgumentReader;
  class StringFormatter;
  BionicResult format(const NativeCallEvent &Call);
  class IntegerScanner;
  BionicResult scan(const NativeCallEvent &Call);
  class Mutex;
  BionicResult semaphore(const NativeCallEvent &Call);
  BionicResult semaphoreWait(uint64_t Address, uint32_t Word, bool TryOnly);
  llvm::Error semaphoreAccess(uint64_t Address, unsigned Permissions);
  llvm::Expected<uint32_t> readSemaphore(uint64_t Address);
  llvm::Error writeSemaphore(uint64_t Address, uint32_t Word);
  BionicResult mutex(const NativeCallEvent &Call);
  BionicResult resetMutexAttribute(const NativeCallEvent &Call, uint64_t Value);
  BionicResult initializeMutexAttribute(const NativeCallEvent &Call);
  BionicResult destroyMutexAttribute(const NativeCallEvent &Call);
  BionicResult mutexAttributes(const NativeCallEvent &Call);
  BionicResult initializeMutex(const NativeCallEvent &Call);
  std::optional<BionicValue> unsupportedMutex(llvm::StringRef Name,
                                              llvm::StringRef Reason);
  llvm::Error mutexAccess(uint64_t Address, unsigned Size, unsigned Permissions,
                          unsigned Alignment);
  llvm::Expected<uint64_t> readMutexWord(uint64_t Address, unsigned Size,
                                         unsigned Alignment);
  llvm::Error writeMutexWord(uint64_t Address, unsigned Size, uint64_t Value);
};
llvm::Expected<ProcessResult> runNative(const std::filesystem::path &Path,
                                        const ProcessOptions &Options);
} // namespace neverd::emulation::android_model
#endif
