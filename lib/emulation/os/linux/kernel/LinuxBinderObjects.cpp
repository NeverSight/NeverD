//===- LinuxBinderObjects.cpp - Binder nodes, handles and references ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// LP64 strong flat objects and process-local handles. Node ownership remains
/// in the shared Linux open description; adapters never manufacture handles.
//===----------------------------------------------------------------------===//
#include "LinuxFiles.h"

#include "neverd/emulation/Limits.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <set>

namespace neverd::emulation::linux_model {
namespace {
constexpr uint32_t Binder = 0x73622a85, Handle = 0x73682a85;
constexpr uint32_t IncRefs = 0x40046304, Acquire = 0x40046305;
constexpr uint32_t Release = 0x40046306, DecRefs = 0x40046307;
constexpr uint32_t IncRefsDone = 0x40106308, AcquireDone = 0x40106309;
} // namespace

std::shared_ptr<LinuxDeviceState>
LinuxFiles::binderOwner(LinuxDeviceState &Device) const {
  for (const auto &Weak : DeviceDescriptions)
    if (auto Open = Weak.lock(); Open.get() == &Device)
      return Open;
  return {};
}

static void binderQueueNode(const std::shared_ptr<BinderNode> &Node,
                            std::optional<uint32_t> Thread = {}) {
  auto Owner = Node->Owner.lock();
  if (!Owner)
    return;
  const bool Strong = Node->InternalStrong || Node->LocalStrong;
  const bool Weak = Node->References || Node->LocalWeak || Strong;
  if (!Strong && !Weak && !Node->HasStrong && !Node->HasWeak) {
    if (Node->Queued) {
      auto &Old = Node->QueuedThread
                      ? Owner->BinderThreadWork[*Node->QueuedThread]
                      : Owner->BinderProcessWork;
      std::erase_if(Old, [&](const auto &Work) { return Work.Node == Node; });
      Node->Queued = false;
      Node->QueuedThread.reset();
    }
    Owner->BinderNodes.erase(Node->Pointer);
    return;
  }
  if (Node->Queued) {
    if (!Thread)
      return;
    auto &Old = Node->QueuedThread
                    ? Owner->BinderThreadWork[*Node->QueuedThread]
                    : Owner->BinderProcessWork;
    std::erase_if(Old, [&](const auto &Work) { return Work.Node == Node; });
  } else {
    if (Strong == Node->HasStrong && Weak == Node->HasWeak)
      return;
  }
  Node->Queued = true;
  Node->QueuedThread = Thread;
  auto &Queue =
      Thread ? Owner->BinderThreadWork[*Thread] : Owner->BinderProcessWork;
  BinderWork Work;
  Work.Node = Node;
  Queue.push_back(std::move(Work));
}

bool LinuxDeviceState::hasReadyBinderThreadWork(uint32_t Thread) const {
  auto I = BinderThreadWork.find(Thread);
  // Exported-node work and synchronous completion are deferred until a
  // reply/error activates the thread queue (binder_thread.process_todo).
  return I != BinderThreadWork.end() &&
         std::any_of(I->second.begin(), I->second.end(),
                     [](const auto &Work) { return !Work.Node; });
}

LinuxDeviceState::~LinuxDeviceState() {
  // Last descriptor/VMA retirement drops each process reference, independently
  // of the number of retained transaction or userspace references to it.
  for (const auto &[ID, Reference] : BinderHandles) {
    if (Reference.Strong)
      --Reference.Node->InternalStrong;
    --Reference.Node->References;
    binderQueueNode(Reference.Node);
  }
}

uint32_t LinuxFiles::binderRetainHandle(LinuxDeviceState &Device,
                                        const std::shared_ptr<BinderNode> &Node,
                                        bool Strong,
                                        std::optional<uint32_t> OwnerThread) {
  auto Found = Device.BinderHandlesByNode.find(Node.get());
  uint32_t ID;
  if (Found != Device.BinderHandlesByNode.end())
    ID = Found->second;
  else {
    const auto Owner = Node->Owner.lock();
    ID = Owner && Owner->BinderManager && !Node->Pointer ? 0 : 1;
    for (auto I = Device.BinderHandles.lower_bound(ID);
         I != Device.BinderHandles.end() && I->first == ID; ++I)
      ++ID;
    Device.BinderHandles.emplace(ID, BinderReference{Node});
    Device.BinderHandlesByNode.try_emplace(Node.get(), ID);
    ++Node->References;
  }
  auto &Ref = Device.BinderHandles.at(ID);
  if (Strong) {
    if (!Ref.Strong++) {
      ++Node->InternalStrong;
      binderQueueNode(Node, Node->HasStrong ? std::nullopt : OwnerThread);
    }
  } else if (!Ref.Weak++)
    binderQueueNode(Node);
  return ID;
}

void LinuxFiles::binderReleaseHandle(LinuxDeviceState &Device, uint32_t ID,
                                     bool Strong) {
  auto I = Device.BinderHandles.find(ID);
  auto Node = I->second.Node;
  auto &Ref = I->second;
  if (Strong) {
    if (!--Ref.Strong)
      --Node->InternalStrong;
  } else
    --Ref.Weak;
  if (!Ref.Strong && !Ref.Weak) {
    --Node->References;
    Device.BinderHandlesByNode.erase(Node.get());
    Device.BinderHandles.erase(I);
  }
  binderQueueNode(Node);
}

bool LinuxFiles::binderReleaseBuffer(LinuxDeviceState &Device,
                                     const BinderBuffer &Buffer,
                                     ProcessResult &Result) {
  llvm::DenseMap<uint32_t, uint32_t> Required;
  for (const auto &Object : Buffer.References)
    if (Object.Handle) {
      auto I = Device.BinderHandles.find(*Object.Handle);
      if (I == Device.BinderHandles.end() || I->second.Node != Object.Node ||
          I->second.Strong < ++Required[*Object.Handle]) {
        unsupported(
            Result,
            "Binder buffer references were released outside buffer ownership");
        return false;
      }
    }
  for (const auto &Object : Buffer.References) {
    if (Object.Handle)
      binderReleaseHandle(Device, *Object.Handle, true);
    else {
      --Object.Node->LocalStrong;
      binderQueueNode(Object.Node);
    }
  }
  if (Buffer.Target) {
    --Buffer.Target->LocalStrong;
    binderQueueNode(Buffer.Target);
  }
  return true;
}

std::optional<uint64_t>
LinuxFiles::binderReferenceCommand(LinuxDeviceState &Device, uint32_t Command,
                                   llvm::ArrayRef<uint8_t> Data,
                                   ProcessResult &Result) {
  if (Command == IncRefsDone || Command == AcquireDone) {
    const uint64_t Pointer = llvm::support::endian::read64le(Data.data());
    const uint64_t Cookie = llvm::support::endian::read64le(Data.data() + 8);
    auto I = Device.BinderNodes.find(Pointer);
    if (I == Device.BinderNodes.end() || I->second->Cookie != Cookie)
      return unsupported(Result,
                         "Binder reference acknowledgment has no owned node");
    auto &Node = *I->second;
    auto &Pending =
        Command == AcquireDone ? Node.PendingStrong : Node.PendingWeak;
    if (!Pending)
      return unsupported(Result,
                         "Binder reference acknowledgment is not pending");
    Pending = false;
    --(Command == AcquireDone ? Node.LocalStrong : Node.LocalWeak);
    binderQueueNode(I->second);
    return 0;
  }
  const uint32_t ID = llvm::support::endian::read32le(Data.data());
  auto I = Device.BinderHandles.find(ID);
  if (I == Device.BinderHandles.end() && !ID &&
      (Command == IncRefs || Command == Acquire)) {
    for (const auto &Weak : DeviceDescriptions)
      if (auto Owner = Weak.lock(); Owner && Owner->Options == Device.Options &&
                                    Owner->BinderManager &&
                                    Owner.get() != &Device) {
        if (Device.BinderHandles.size() ==
            ::neverd::limits::emulation::kMaxBinderHandlesPerOpen)
          return unsupported(Result, "Binder handle budget exhausted");
        binderRetainHandle(Device, Owner->BinderNodes.at(0),
                           Command == Acquire);
        return 0;
      }
  }
  if (I == Device.BinderHandles.end())
    return unsupported(Result, "Binder reference command has no owned handle");
  auto &Ref = I->second;
  const bool Strong = Command == Acquire || Command == Release;
  if (Command == IncRefs || Command == Acquire) {
    if ((Strong ? Ref.Strong : Ref.Weak) == UINT32_MAX)
      return unsupported(Result, "Binder reference count exhausted");
    const auto Owner = Ref.Node->Owner.lock();
    const bool Manager = Owner && Owner->BinderManager && !Ref.Node->Pointer &&
                         Ref.Node->HasStrong;
    if (Strong && !Ref.Strong && !Ref.Node->InternalStrong && !Manager)
      return unsupported(Result, "Binder weak-to-strong acquisition requires "
                                 "another strong reference");
    binderRetainHandle(Device, Ref.Node, Strong);
  } else {
    if (!(Strong ? Ref.Strong : Ref.Weak))
      return unsupported(Result, "Binder reference release is not owned");
    binderReleaseHandle(Device, ID, Strong);
  }
  return 0;
}

bool LinuxFiles::binderTranslateObjects(LinuxDeviceState &Source,
                                        LinuxDeviceState &Target,
                                        std::vector<uint8_t> &Payload,
                                        BinderBuffer &Buffer,
                                        ProcessResult &Result) {
  auto Fail = [&](const char *Message) {
    unsupported(Result, Message);
    return false;
  };
  const auto SourceOwner =
      Buffer.Objects.empty() ? nullptr : binderOwner(Source);
  std::map<uint64_t, std::shared_ptr<BinderNode>> NewNodes;
  std::vector<std::shared_ptr<BinderNode>> Nodes;
  std::set<const BinderNode *> NewHandles;
  llvm::DenseMap<const BinderNode *, uint32_t> Acquisitions;
  Nodes.reserve(Buffer.Objects.size());
  Buffer.References.reserve(Buffer.Objects.size());
  uint64_t Minimum = 0;
  for (auto Offset : Buffer.Objects) {
    if (Offset < Minimum || Offset % 4 || Offset > Payload.size() ||
        Payload.size() - Offset < 24)
      return Fail("Binder object offsets require aligned, ordered complete "
                  "flat objects");
    Minimum = Offset + 24;
    auto *Object = Payload.data() + Offset;
    const auto Type = llvm::support::endian::read32le(Object);
    std::shared_ptr<BinderNode> Node;
    if (Type == Binder) {
      const auto Pointer = llvm::support::endian::read64le(Object + 8);
      const auto Cookie = llvm::support::endian::read64le(Object + 16);
      if (auto I = Source.BinderNodes.find(Pointer);
          I != Source.BinderNodes.end())
        Node = I->second;
      else if (auto I = NewNodes.find(Pointer); I != NewNodes.end())
        Node = I->second;
      else {
        if (Source.BinderNodes.size() + NewNodes.size() ==
            ::neverd::limits::emulation::kMaxBinderNodesPerOpen)
          return Fail("Binder node budget exhausted");
        Node = std::make_shared<BinderNode>();
        Node->Owner = SourceOwner;
        Node->Pointer = Pointer;
        Node->Cookie = Cookie;
        NewNodes.emplace(Pointer, Node);
      }
      if (Node->Cookie != Cookie)
        return Fail("Binder object cookie disagrees with its owned node");
    } else if (Type == Handle) {
      const auto ID = llvm::support::endian::read32le(Object + 8);
      auto I = Source.BinderHandles.find(ID);
      if (I == Source.BinderHandles.end() || !I->second.Strong)
        return Fail("Binder object requires an owned strong handle");
      Node = I->second.Node;
    } else
      return Fail("Binder weak, descriptor and pointer object translation is "
                  "not modeled");
    if (Node->Owner.lock().get() != &Target &&
        !Target.BinderHandlesByNode.contains(Node.get()))
      NewHandles.insert(Node.get());
    const auto Count = ++Acquisitions[Node.get()];
    if (auto I = Target.BinderHandlesByNode.find(Node.get());
        I != Target.BinderHandlesByNode.end() &&
        Target.BinderHandles.at(I->second).Strong > UINT32_MAX - Count)
      return Fail("Binder reference count exhausted");
    Nodes.push_back(std::move(Node));
  }
  if (Target.BinderHandles.size() + NewHandles.size() >
      ::neverd::limits::emulation::kMaxBinderHandlesPerOpen)
    return Fail("Binder handle budget exhausted");
  Source.BinderNodes.merge(NewNodes);
  for (size_t I = 0; I != Nodes.size(); ++I) {
    auto &Node = Nodes[I];
    auto *Object = Payload.data() + Buffer.Objects[I];
    if (Node->Owner.lock().get() == &Target) {
      llvm::support::endian::write32le(Object, Binder);
      llvm::support::endian::write64le(Object + 8, Node->Pointer);
      llvm::support::endian::write64le(Object + 16, Node->Cookie);
      ++Node->LocalStrong;
      Buffer.References.push_back({Node, {}});
    } else {
      const auto ID = binderRetainHandle(Target, Node, true,
                                         Node->Owner.lock().get() == &Source
                                             ? std::optional(CurrentTask)
                                             : std::nullopt);
      llvm::support::endian::write32le(Object, Handle);
      llvm::support::endian::write64le(Object + 8, ID);
      llvm::support::endian::write64le(Object + 16, 0);
      Buffer.References.push_back({Node, ID});
    }
  }
  return true;
}

uint8_t LinuxFiles::binderNodeCommands(const BinderNode &Node,
                                       std::vector<uint8_t> &Output) const {
  const bool Strong = Node.InternalStrong || Node.LocalStrong;
  const bool Weak = Node.References || Node.LocalWeak || Strong;
  uint8_t Events = 0;
  auto Emit = [&](uint8_t Bit, uint32_t Command) {
    Events |= Bit;
    const auto At = Output.size();
    Output.resize(At + 20);
    llvm::support::endian::write32le(Output.data() + At, Command);
    llvm::support::endian::write64le(Output.data() + At + 4, Node.Pointer);
    llvm::support::endian::write64le(Output.data() + At + 12, Node.Cookie);
  };
  if (Weak && !Node.HasWeak)
    Emit(1, 0x80107207); // BR_INCREFS
  if (Strong && !Node.HasStrong)
    Emit(2, 0x80107208); // BR_ACQUIRE
  if (!Strong && Node.HasStrong)
    Emit(4, 0x80107209); // BR_RELEASE
  if (!Weak && Node.HasWeak)
    Emit(8, 0x8010720a); // BR_DECREFS
  return Events;
}

void LinuxFiles::binderCommitNode(const std::shared_ptr<BinderNode> &Node,
                                  uint8_t Events) {
  Node->Queued = false;
  Node->QueuedThread.reset();
  if (Events & 1) {
    Node->HasWeak = Node->PendingWeak = true;
    ++Node->LocalWeak;
  }
  if (Events & 2) {
    Node->HasStrong = Node->PendingStrong = true;
    ++Node->LocalStrong;
  }
  if (Events & 4)
    Node->HasStrong = false;
  if (Events & 8)
    Node->HasWeak = false;
  if (!Node->HasWeak && !Node->HasStrong && !Node->References &&
      !Node->LocalWeak && !Node->LocalStrong)
    if (auto Owner = Node->Owner.lock())
      Owner->BinderNodes.erase(Node->Pointer);
}
} // namespace neverd::emulation::linux_model
