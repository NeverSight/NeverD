//===- WindowsProcessEnvironment.cpp - PE64 initial thread state --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsProcess.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Endian.h"

#include <algorithm>

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

llvm::Expected<Environment>
prepareEnvironment(AddressSpace &Memory, const Image &Image,
                   const ProcessOptions &Options, llvm::StringRef ImageName,
                   llvm::ArrayRef<ModuleIdentity> Modules,
                   llvm::ArrayRef<size_t> InitOrder) {
  const ModuleIdentity Main{ImageName.str(), Image.Base, Image.Size,
                            Image.Entry};
  if (Modules.empty())
    Modules = llvm::ArrayRef(Main);
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
  uint64_t Cursor = ModuleEntry + Modules.size() * ModuleStride;
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
  if (Image.TLSIndex) {
    if (auto E = Memory.writeInteger(Image.TLSIndex, 0, DWordSize))
      return std::move(E);
    if (auto E = Memory.writeInteger(TLSVector, TLSData, PointerSize))
      return std::move(E);
    if (!Image.TLSBytes.empty())
      if (auto E = Memory.write(TLSData, Image.TLSBytes))
        return std::move(E);
  }
  return Environment{*CommandAddress, std::move(*Name)};
}
} // namespace neverd::emulation::windows_process
