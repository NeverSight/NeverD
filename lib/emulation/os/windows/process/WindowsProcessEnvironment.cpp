//===- WindowsProcessEnvironment.cpp - PE64 initial thread state --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsProcessModules.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <set>

namespace neverd::emulation::windows_process {
namespace {
using namespace value;
llvm::Expected<std::u16string> utf16(llvm::StringRef Input) {
  llvm::SmallVector<llvm::UTF16> Units;
  if (Input.contains('\0') || Input.size() > StringCapacity ||
      !llvm::convertUTF8ToUTF16String(Input, Units) ||
      Units.size() >= MaxStringUnits)
    return failure(text::Strings);
  return std::u16string(Units.begin(), Units.end());
}
std::u16string quote(const std::u16string &Input) {
  // Encode argv using the documented Microsoft CRT backslash/quote rules.
  // Quote every argument so empty strings and trailing backslashes survive.
  std::u16string Out(1, u'"');
  size_t Backslashes = 0;
  for (char16_t C : Input) {
    if (C == u'\\') {
      ++Backslashes;
      continue;
    }
    Out.append(Backslashes * (C == u'"' ? 2 : 1), u'\\');
    Backslashes = 0;
    if (C == u'"')
      Out += u'\\';
    Out += C;
  }
  Out.append(Backslashes * 2, u'\\');
  Out += u'"';
  return Out;
}
} // namespace

llvm::Expected<Environment> prepareEnvironment(AddressSpace &Memory,
                                               Program &Program,
                                               const ProcessOptions &Options) {
  if (Program.Modules.empty() ||
      Program.Modules.size() != Program.Identities.size())
    return failure(text::Layout);
  const auto &Image = Program.Modules.front().Loaded;
  const auto &Modules = Program.Identities;
  const auto &InitOrder = Program.LoaderInitializationOrder;
  const llvm::StringRef ImageName = Modules.front().Name;
  if (Modules.size() > windows_process_limits::Modules + 1)
    return failure(text::ModuleBudget);
  auto Name = utf16(ImageName);
  if (!Name)
    return Name.takeError();
  std::u16string Command;
  auto Arguments = Options.Arguments;
  if (Arguments.empty())
    Arguments.push_back(ImageName.str());
  if (Arguments.size() > MaxStringUnits ||
      Options.Environment.size() > MaxStringUnits)
    return failure(text::Strings);
  for (const auto &Arg : Arguments) {
    auto Text = utf16(Arg);
    if (!Text)
      return Text.takeError();
    if (Command.empty()) {
      // CRT treats argv[0] separately: backslashes do not escape its quote.
      if (Text->find(u'"') != std::u16string::npos)
        return failure(text::Strings);
      Command = std::u16string(1, u'"') + *Text + u'"';
    } else {
      Command += u' ';
      Command += quote(*Text);
    }
    if (Command.size() >= MaxStringUnits)
      return failure(text::Strings);
  }
  std::map<std::string, std::u16string> Sorted;
  size_t EnvironmentUnits = 1;
  for (const auto &Item : Options.Environment) {
    const auto Equal = Item.find('=');
    if (Equal == std::string::npos || !Equal)
      return failure(text::Strings);
    const llvm::StringRef Key(Item.data(), Equal);
    // Unicode values are supported. ASCII names avoid inventing an incomplete
    // Windows Unicode case-folding table for environment identity.
    if (!llvm::all_of(Key, [](char C) { return llvm::isPrint(C); }))
      return failure(text::Strings);
    auto Text = utf16(Item);
    if (!Text)
      return Text.takeError();
    EnvironmentUnits += Text->size() + 1;
    if (EnvironmentUnits > MaxStringUnits ||
        !Sorted.emplace(Key.lower(), std::move(*Text)).second)
      return failure(text::Strings);
  }
  std::u16string EnvironmentBlock;
  for (const auto &[Key, Text] : Sorted) {
    EnvironmentBlock += Text;
    EnvironmentBlock += u'\0';
  }
  if (EnvironmentBlock.empty())
    EnvironmentBlock += u'\0';
  EnvironmentBlock += u'\0';
  if (auto E =
          Memory.map(TEB, EnvironmentEnd - TEB, Read | Write | UserAccessible))
    return std::move(E);
  Environment Out{};
  uint64_t Cursor =
      ModuleEntry + (windows_process_limits::Modules + 1) * ModuleStride;
  auto Store = [&](const std::u16string &Text) -> llvm::Expected<uint64_t> {
    const uint64_t Size = (Text.size() + 1) * WideSize;
    if (Size > EnvironmentEnd - Cursor)
      return failure(text::Strings);
    std::vector<uint8_t> Bytes(Size);
    for (size_t I = 0; I < Text.size(); ++I)
      llvm::support::endian::write16le(Bytes.data() + I * WideSize, Text[I]);
    const uint64_t Address = Cursor;
    if (auto E = Memory.write(Address, Bytes))
      return std::move(E);
    Cursor += Size;
    return Address;
  };
  auto ImageAddress = Store(*Name);
  if (!ImageAddress)
    return ImageAddress.takeError();
  auto CommandAddress = Store(Command);
  if (!CommandAddress)
    return CommandAddress.takeError();
  auto EnvAddress = Store(EnvironmentBlock);
  if (!EnvAddress)
    return EnvAddress.takeError();
  // Keep a bounded mutable block separate from subsequent module names. The
  // serialized guest block is the authority for all environment APIs.
  if (EnvironmentCapacity > EnvironmentEnd - *EnvAddress)
    return failure(text::Strings);
  Out.Variables = *EnvAddress;
  Cursor = *EnvAddress + EnvironmentCapacity;
  auto Unicode = [&](uint64_t Address, uint64_t Buffer,
                     size_t Length) -> llvm::Error {
    if (auto E = Memory.writeInteger(Address + UnicodeLength, Length * WideSize,
                                     WideSize))
      return E;
    if (auto E = Memory.writeInteger(Address + UnicodeMaximumLength,
                                     (Length + 1) * WideSize, WideSize))
      return E;
    return Memory.writeInteger(Address + UnicodeBuffer, Buffer, PointerSize);
  };
  struct Field {
    uint64_t Address, Value;
    unsigned Size = PointerSize;
  };
  const Field Fields[] = {
      {TEB + TebStackBase, StackTop},
      {TEB + TebStackLimit, StackTop - Options.StackSize},
      {TEB + TebDeallocationStack, StackTop - Options.StackSize},
      {TEB + TebSelf, TEB},
      {TEB + TebProcessID, ProcessID},
      {TEB + TebThreadID, ThreadID},
      {TEB + TebPEB, PEB},
      {TEB + TebTLSVector, TLSVector},
      {PEB + PebImageBase, Image.Base},
      {PEB + PebParameters, Parameters},
      {PEB + PebHeap, HeapHandle},
      {PEB + PebLdr, Ldr},
      {Parameters + ParamsMaximumLength, ParameterSize, DWordSize},
      {Parameters + ParamsLength, ParameterSize, DWordSize},
      {Parameters + ParamsFlags, ParamsNormalized, DWordSize},
      {Parameters + ParamsStdInput, StandardInput},
      {Parameters + ParamsStdOutput, StandardOutput},
      {Parameters + ParamsStdError, StandardError},
      {Parameters + ParamsEnvironment, *EnvAddress},
      {Ldr, LdrSize, DWordSize},
      {Ldr + LdrInitialized, 1, 1}};
  for (const auto &F : Fields)
    if (auto E = Memory.writeInteger(F.Address, F.Value, F.Size))
      return std::move(E);
  // Publish only actually mapped images. API models have no synthetic DLL.
  for (size_t I = 0; I < Modules.size(); ++I) {
    const auto &M = Modules[I];
    const uint64_t Node = ModuleEntry + I * ModuleStride;
    auto Text = utf16(M.Name);
    if (!Text)
      return Text.takeError();
    auto Address = I ? Store(*Text) : llvm::Expected<uint64_t>(*ImageAddress);
    if (!Address)
      return Address.takeError();
    Out.ModuleNames.emplace(I, *Address);
    for (const auto &F : {Field{Node + ModuleBase, M.Base},
                          Field{Node + ModuleEntryPoint, M.Entry},
                          Field{Node + ModuleImageSize, M.Size, DWordSize}})
      if (auto E = Memory.writeInteger(F.Address, F.Value, F.Size))
        return std::move(E);
    for (uint64_t Offset : {ModuleFullName, ModuleBaseName})
      if (auto E = Unicode(Node + Offset, *Address, Text->size()))
        return std::move(E);
  }
  auto Link = [&](uint64_t Head, uint64_t LinkOffset,
                  llvm::ArrayRef<size_t> Order) -> llvm::Error {
    uint64_t Previous = Head;
    for (size_t I : Order) {
      if (I >= Modules.size())
        return failure(text::Layout);
      const uint64_t Node = ModuleEntry + I * ModuleStride + LinkOffset;
      if (auto E = Memory.writeInteger(Previous, Node, PointerSize))
        return E;
      if (auto E =
              Memory.writeInteger(Node + PointerSize, Previous, PointerSize))
        return E;
      Previous = Node;
    }
    if (auto E = Memory.writeInteger(Previous, Head, PointerSize))
      return E;
    return Memory.writeInteger(Head + PointerSize, Previous, PointerSize);
  };
  std::vector<size_t> Order;
  for (size_t I = 0; I < Modules.size(); ++I)
    Order.push_back(I);
  if (auto E = Link(Ldr + LdrLoadList, 0, Order))
    return std::move(E);
  if (auto E = Link(Ldr + LdrMemoryList, ModuleMemoryLink, Order))
    return std::move(E);
  if (auto E = Link(Ldr + LdrInitList, ModuleInitLink, InitOrder))
    return std::move(E);
  if (auto E =
          Unicode(Parameters + ParamsImagePath, *ImageAddress, Name->size()))
    return std::move(E);
  if (auto E = Unicode(Parameters + ParamsCommandLine, *CommandAddress,
                       Command.size()))
    return std::move(E);
  uint64_t TLSCursor = TLSData, TLSCount = 0;
  for (size_t I = 0; I < Program.Modules.size(); ++I) {
    const auto &M = Program.Modules[I].Loaded;
    if (!M.TLSIndex)
      continue;
    TLSCursor = (TLSCursor + M.TLSAlignment - 1) & ~(M.TLSAlignment - 1);
    const uint64_t Size = std::max(M.TLSSize, PointerSize);
    if (TLSCursor > TLSData + TLSCapacity ||
        Size > TLSData + TLSCapacity - TLSCursor ||
        (TLSCount + 1) * PointerSize > TLSData - TLSVector)
      return failure(text::ModuleTLSBudget);
    // Snapshot the relocated and linked image before publishing its index.
    // TLS templates can contain pointers fixed by the image linker.
    std::vector<uint8_t> Bytes(Size);
    if (M.TLSTemplateSize)
      if (auto E = Memory.read(
              M.TLSTemplate,
              llvm::MutableArrayRef(Bytes).take_front(M.TLSTemplateSize)))
        return std::move(E);
    if (auto E = Memory.writeInteger(M.TLSIndex, TLSCount, DWordSize))
      return std::move(E);
    if (auto E = Memory.writeInteger(TLSVector + TLSCount * PointerSize,
                                     TLSCursor, PointerSize))
      return std::move(E);
    if (auto E = Memory.write(TLSCursor, Bytes))
      return std::move(E);
    Out.TLS.emplace(I, Environment::TLSAllocation{TLSCount, TLSCursor, Size});
    ++TLSCount;
    TLSCursor += Size;
  }
  Out.CommandLine = *CommandAddress;
  Out.ImageName = std::move(*Name);
  Out.StringCursor = Cursor;
  for (const auto &[Address, Size] :
       {std::pair{TEB + TebPEB, PointerSize},
        std::pair{PEB + PebLdr, PointerSize},
        std::pair{PEB + PebImageBase, PointerSize}, std::pair{Ldr, LdrSize},
        std::pair{ModuleEntry,
                  (windows_process_limits::Modules + 1) * ModuleStride}}) {
    auto &Bytes = Out.LoaderMetadata[Address];
    Bytes.resize(Size);
    if (auto E = Memory.read(Address, Bytes))
      return std::move(E);
  }
  for (const auto &[Index, Address] : Out.ModuleNames) {
    auto Name = utf16(Program.Identities[Index].Name);
    if (!Name)
      return Name.takeError();
    auto &Bytes = Out.LoaderMetadata[Address];
    Bytes.resize((Name->size() + 1) * WideSize);
    if (auto E = Memory.read(Address, Bytes))
      return std::move(E);
  }
  return Out;
}
namespace {
llvm::Error validateEnvironment(AddressSpace &Memory, Program &Program,
                                const Environment &Env,
                                const ExecutionBudget &Budget) {
  auto Charge = [&](uint64_t Size) -> llvm::Error {
    if (!Budget.remainingMicroseconds())
      return failure(text::ModuleTimeout);
    if (Size > Program.Reads.MetadataBytes)
      return failure(text::ExportBudget);
    Program.Reads.MetadataBytes -= Size;
    return llvm::Error::success();
  };
  for (const auto &[Address, Expected] : Env.LoaderMetadata) {
    if (auto E = Charge(Expected.size()))
      return E;
    std::vector<uint8_t> Actual(Expected.size());
    if (auto E = Memory.read(Address, Actual))
      return E;
    if (Actual != Expected)
      return failure(text::LoaderChanged);
  }
  if (auto E = Charge(PointerSize))
    return E;
  auto Vector = Memory.readInteger(TEB + TebTLSVector, PointerSize);
  if (!Vector)
    return Vector.takeError();
  if (*Vector != TLSVector)
    return failure(text::LoaderChanged);
  for (const auto &[Index, Block] : Env.TLS) {
    if (auto E = Charge(PointerSize + DWordSize))
      return E;
    auto Value =
        Memory.readInteger(TLSVector + Block.Index * PointerSize, PointerSize);
    if (!Value)
      return Value.takeError();
    if (*Value != Block.Address)
      return failure(text::LoaderChanged);
    auto TLSIndex =
        Memory.readInteger(Program.Modules[Index].Loaded.TLSIndex, DWordSize);
    if (!TLSIndex)
      return TLSIndex.takeError();
    if (*TLSIndex != Block.Index)
      return failure(text::LoaderChanged);
  }
  return llvm::Error::success();
}
llvm::Error snapshotEnvironment(AddressSpace &Memory, Environment &Env) {
  for (auto &[Address, Bytes] : Env.LoaderMetadata)
    if (auto E = Memory.read(Address, Bytes))
      return E;
  return llvm::Error::success();
}
} // namespace
llvm::Error releaseModuleEnvironment(AddressSpace &Memory, Program &Program,
                                     Environment &Env,
                                     llvm::ArrayRef<ModuleRef> Modules,
                                     const ExecutionBudget &Budget) {
  if (auto E = validateEnvironment(Memory, Program, Env, Budget))
    return E;
  for (auto Ref : Modules) {
    if (!current(Program, Ref))
      return failure(text::Lifetime);
    auto I = Env.TLS.find(Ref.Index);
    if (I == Env.TLS.end())
      continue;
    if (auto E = Memory.writeInteger(TLSVector + I->second.Index * PointerSize,
                                     0, PointerSize))
      return E;
    std::vector<uint8_t> Zero(I->second.Size);
    if (auto E = Memory.write(I->second.Address, Zero))
      return E;
    Env.TLS.erase(I);
  }
  return llvm::Error::success();
}
llvm::Error updateEnvironment(AddressSpace &Memory, Program &Program,
                              Environment &Env, const ExecutionBudget &Budget) {
  if (auto E = validateEnvironment(Memory, Program, Env, Budget))
    return E;
  for (size_t I = 0; I < Program.Modules.size(); ++I) {
    const auto &Module = Program.Modules[I];
    const auto &M = Module.Loaded;
    const uint64_t Node = ModuleEntry + I * ModuleStride;
    if (!resident(Module)) {
      std::vector<uint8_t> Zero(ModuleStride);
      if (auto E = Memory.write(Node, Zero))
        return E;
      continue;
    }
    auto Name = utf16(Program.Identities[I].Name);
    if (!Name)
      return Name.takeError();
    if (!Env.ModuleNames.contains(I)) {
      const uint64_t Size = (Name->size() + 1) * WideSize;
      if (Size > EnvironmentEnd - Env.StringCursor)
        return failure(text::Strings);
      std::vector<uint8_t> Bytes(Size);
      for (size_t N = 0; N < Name->size(); ++N)
        llvm::support::endian::write16le(Bytes.data() + N * WideSize,
                                         (*Name)[N]);
      if (auto E = Memory.write(Env.StringCursor, Bytes))
        return E;
      Env.ModuleNames.emplace(I, Env.StringCursor);
      Env.LoaderMetadata.emplace(Env.StringCursor, Bytes);
      Env.StringCursor += Size;
    }
    for (const auto &[Offset, Value] :
         {std::pair{ModuleBase, M.Base}, std::pair{ModuleEntryPoint, M.Entry}})
      if (auto E = Memory.writeInteger(Node + Offset, Value, PointerSize))
        return E;
    if (auto E = Memory.writeInteger(Node + ModuleImageSize, M.Size, DWordSize))
      return E;
    for (uint64_t Offset : {ModuleFullName, ModuleBaseName}) {
      if (auto E = Memory.writeInteger(Node + Offset + UnicodeLength,
                                       Name->size() * WideSize, WideSize))
        return E;
      if (auto E = Memory.writeInteger(Node + Offset + UnicodeMaximumLength,
                                       (Name->size() + 1) * WideSize, WideSize))
        return E;
      if (auto E = Memory.writeInteger(Node + Offset + UnicodeBuffer,
                                       Env.ModuleNames.at(I), PointerSize))
        return E;
    }
    if (!M.TLSIndex || Env.TLS.contains(I))
      continue;
    std::vector<Environment::TLSAllocation> Blocks;
    std::set<uint64_t> Indices;
    for (const auto &[Owner, Block] : Env.TLS) {
      Blocks.push_back(Block);
      Indices.insert(Block.Index);
    }
    llvm::sort(Blocks, [](const auto &A, const auto &B) {
      return A.Address < B.Address;
    });
    const uint64_t Size = std::max(M.TLSSize, PointerSize);
    uint64_t Address = TLSData;
    auto Align = [&] {
      Address = (Address + M.TLSAlignment - 1) & ~(M.TLSAlignment - 1);
    };
    Align();
    for (const auto &Block : Blocks) {
      if (Address <= Block.Address && Size <= Block.Address - Address)
        break;
      Address = Block.Address + Block.Size;
      Align();
    }
    uint64_t Index = 0;
    while (Indices.contains(Index))
      ++Index;
    if (Address > TLSData + TLSCapacity ||
        Size > TLSData + TLSCapacity - Address ||
        (Index + 1) * PointerSize > TLSData - TLSVector)
      return failure(text::ModuleTLSBudget);
    std::vector<uint8_t> Bytes(Size);
    if (M.TLSTemplateSize)
      if (auto E = Memory.read(
              M.TLSTemplate,
              llvm::MutableArrayRef(Bytes).take_front(M.TLSTemplateSize)))
        return E;
    if (auto E = Memory.write(Address, Bytes))
      return E;
    if (auto E = Memory.writeInteger(M.TLSIndex, Index, DWordSize))
      return E;
    if (auto E = Memory.writeInteger(TLSVector + Index * PointerSize, Address,
                                     PointerSize))
      return E;
    Env.TLS.emplace(I, Environment::TLSAllocation{Index, Address, Size});
  }
  auto Link = [&](uint64_t Head, uint64_t Offset,
                  llvm::ArrayRef<size_t> Order) -> llvm::Error {
    uint64_t Previous = Head;
    for (size_t I : Order) {
      const uint64_t Node = ModuleEntry + I * ModuleStride + Offset;
      if (auto E = Memory.writeInteger(Previous, Node, PointerSize))
        return E;
      if (auto E =
              Memory.writeInteger(Node + PointerSize, Previous, PointerSize))
        return E;
      Previous = Node;
    }
    if (auto E = Memory.writeInteger(Previous, Head, PointerSize))
      return E;
    return Memory.writeInteger(Head + PointerSize, Previous, PointerSize);
  };
  // Generation order is load order even when a retired catalogue slot is
  // reused.
  std::vector<size_t> Order;
  for (size_t I = 0; I < Program.Modules.size(); ++I)
    if (resident(Program.Modules[I]))
      Order.push_back(I);
  llvm::sort(Order, [&](size_t A, size_t B) {
    return Program.Modules[A].Generation < Program.Modules[B].Generation;
  });
  if (auto E = Link(Ldr + LdrLoadList, 0, Order))
    return E;
  if (auto E = Link(Ldr + LdrMemoryList, ModuleMemoryLink, Order))
    return E;
  // Native FreeLibrary removes this membership before detach callbacks, while
  // the image remains mapped and visible through the other lists and lookup.
  std::vector<size_t> InitOrder;
  for (size_t I : Program.LoaderInitializationOrder)
    if (Program.Modules[I].State != ModuleState::Detaching)
      InitOrder.push_back(I);
  if (auto E = Link(Ldr + LdrInitList, ModuleInitLink, InitOrder))
    return E;
  return snapshotEnvironment(Memory, Env);
}
} // namespace neverd::emulation::windows_process
