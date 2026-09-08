#include "neverd/loader/ObjC/ObjCBlocks.h"

#include "ObjCRuntimeData.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"

#include "llvm/Support/Endian.h"

namespace neverd {
namespace {
constexpr uint32_t NoEscape = 1U << 23;
constexpr uint32_t HasCopyDispose = 1U << 25;
constexpr uint32_t HasCtor = 1U << 26;
constexpr uint32_t IsGlobal = 1U << 28;
constexpr uint32_t HasStret = 1U << 29;
constexpr uint32_t HasSignature = 1U << 30;
constexpr uint32_t HasExtendedLayout = 1U << 31;
constexpr uint64_t HeaderSize = 32;
constexpr uint64_t MaxLiteralSize = 1U << 20;

bool supportedImage(const BinaryImage &Image) {
  return Image.Format == BinaryFormat::MachO && Image.Bits == Bitness::Bits64 &&
         !Image.IsRelocatable &&
         (Image.Arch == Arch::AArch64 || Image.Arch == Arch::X64);
}

std::optional<uint64_t> integer64(const objc::RuntimeData &Data, va_t Address) {
  const auto *Bytes = Data.bytes(Address, 8);
  return Bytes ? std::optional(llvm::support::endian::read64le(Bytes))
               : std::nullopt;
}

bool skipOffset(llvm::StringRef Signature, size_t &Offset) {
  const size_t Start = Offset;
  while (Offset < Signature.size() && Signature[Offset] >= '0' &&
         Signature[Offset] <= '9')
    ++Offset;
  return Offset - Start <= 10;
}

bool globalIsa(llvm::StringRef Name) {
  return Name == "__NSConcreteGlobalBlock" || Name == "_NSConcreteGlobalBlock";
}

bool stackIsa(llvm::StringRef Name) {
  return Name == "__NSConcreteStackBlock" || Name == "_NSConcreteStackBlock";
}

void captureLayout(const objc::RuntimeData &Data, ObjCBlockDescriptor &Result,
                   va_t LayoutSlot) {
  using Kind = ObjCBlockCaptureRange::Kind;
  uint64_t Cursor = HeaderSize;
  auto Append = [&](Kind StorageKind, uint64_t Size) {
    if (Size > Result.LiteralSize - Cursor)
      return false;
    if (Size) {
      Result.Captures.push_back({StorageKind, Cursor, Size});
      Cursor += Size;
    }
    return true;
  };
  auto Unknown = [&](const char *Reason) {
    Result.Limitations.push_back(Reason);
    Append(Kind::Unknown, Result.LiteralSize - Cursor);
  };
  if (!(Result.Flags & HasExtendedLayout)) {
    if (Result.LiteralSize != HeaderSize)
      Unknown("block capture field types are absent from this descriptor");
    return;
  }
  const auto RawLayout = integer64(Data, LayoutSlot);
  if (!RawLayout) {
    Unknown("extended block layout field is truncated");
    return;
  }
  Result.LayoutValue = *RawLayout;
  if (*RawLayout < 4096) {
    // Inline xyz counts represent strong, byref, weak pointers in that order.
    // These are scalar bits and must not require a pointer fixup certificate.
    if (!Append(Kind::Strong, ((*RawLayout >> 8) & 15) * 8) ||
        !Append(Kind::Byref, ((*RawLayout >> 4) & 15) * 8) ||
        !Append(Kind::Weak, (*RawLayout & 15) * 8)) {
      Unknown("inline block ownership layout exceeds capture storage");
      return;
    }
    Append(Kind::NonObjectBytes, Result.LiteralSize - Cursor);
    return;
  }
  const auto Layout = Data.pointer(LayoutSlot);
  if (!Layout) {
    Unknown("block layout pointer has an unresolved fixup");
    return;
  }
  Result.LayoutValue = *Layout;
  for (uint64_t Index = 0; Index < 4096 && *Layout <= InvalidVA - Index;
       ++Index) {
    const auto *Byte = Data.bytes(*Layout + Index, 1);
    if (!Byte) {
      Unknown("block ownership layout is not terminated in file-backed data");
      return;
    }
    Result.LayoutBytes.push_back(*Byte);
    if (!*Byte) {
      Append(Kind::NonObjectBytes, Result.LiteralSize - Cursor);
      return;
    }
    const unsigned Opcode = *Byte >> 4;
    const uint64_t Count = (*Byte & 15) + 1;
    const auto StorageKind = Opcode == 1 || Opcode == 2 ? Kind::NonObjectBytes
                             : Opcode == 3              ? Kind::Strong
                             : Opcode == 4              ? Kind::Byref
                             : Opcode == 5              ? Kind::Weak
                             : Opcode == 6              ? Kind::Unretained
                                                        : Kind::Unknown;
    if (StorageKind == Kind::Unknown) {
      Unknown("block ownership layout contains an unsupported opcode");
      return;
    }
    if (!Append(StorageKind, Opcode == 1 ? Count : Count * 8)) {
      Unknown("block ownership layout exceeds capture storage");
      return;
    }
  }
  Unknown("block ownership layout exceeds the decoding budget");
}
} // namespace

std::optional<SourceFunctionTypeHint>
parseObjCBlockSignature(llvm::StringRef Signature, Arch Architecture,
                        std::string &Diagnostic) {
  Diagnostic.clear();
  auto Reject = [&]() -> std::optional<SourceFunctionTypeHint> {
    Diagnostic = "block invoke signature is absent or not a fixed scalar ABI";
    return std::nullopt;
  };
  if (Signature.empty() || Signature.size() > 4096)
    return Reject();
  size_t Offset = 0;
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::BlockRuntime;
  Hint.ReturnType = parseObjCScalarType(Signature, Offset);
  if (!Hint.ReturnType || !skipOffset(Signature, Offset))
    return Reject();
  // This is the only hidden invoke parameter. A selector/self pair would
  // describe an Objective-C method, and cannot be silently accepted here.
  if (!Signature.substr(Offset).starts_with("@?"))
    return Reject();
  Offset += 2;
  Hint.Parameters.push_back(
      {"block_object", NdType::makePtr(NdType::makeVoid())});
  if (!skipOffset(Signature, Offset))
    return Reject();
  while (Offset < Signature.size() && Hint.Parameters.size() < 64) {
    auto Type = parseObjCScalarType(Signature, Offset);
    if (!Type || Type->Kind == NdTypeKind::Void ||
        !skipOffset(Signature, Offset))
      return Reject();
    Hint.Parameters.push_back(
        {"block_arg" + std::to_string(Hint.Parameters.size() - 1),
         std::move(Type)});
  }
  if (Offset != Signature.size())
    return Reject();
  if (!assignDarwinScalarSourceABI(Hint, Architecture, Diagnostic))
    return std::nullopt;
  return Hint;
}

std::optional<ObjCBlockDescriptor>
readObjCBlockDescriptor(const BinaryImage &Image, va_t Address, uint32_t Flags,
                        std::string &Diagnostic) {
  Diagnostic.clear();
  auto Reject = [&](const char *Reason) -> std::optional<ObjCBlockDescriptor> {
    Diagnostic = Reason;
    return std::nullopt;
  };
  if (!supportedImage(Image))
    return Reject("block metadata requires linked Darwin arm64/x86_64");
  const objc::RuntimeData Data(Image);
  if (!Data.bytes(Address, 16))
    return Reject("block descriptor header is not file-backed");
  const auto Reserved = integer64(Data, Address);
  const auto Size = integer64(Data, Address + 8);
  if (!Reserved || *Reserved || !Size || *Size < HeaderSize ||
      *Size > MaxLiteralSize)
    return Reject("block descriptor size or reserved field is invalid");
  ObjCBlockDescriptor Result;
  Result.Address = Address;
  Result.Flags = Flags;
  Result.LiteralSize = *Size;
  va_t Cursor = Address + 16;
  if (Flags & HasCopyDispose) {
    if (!Data.bytes(Cursor, 16))
      return Reject("block copy/dispose descriptor fields are truncated");
    const auto Copy = Data.pointer(Cursor);
    const auto Dispose = Data.pointer(Cursor + 8);
    if (!Copy || !Dispose || !Image.isCodeAddress(*Copy) ||
        !Image.isCodeAddress(*Dispose))
      return Reject(
          "block copy/dispose functions have no resolved code identity");
    Result.CopyHelper = *Copy;
    Result.DisposeHelper = *Dispose;
    Cursor += 16;
    Result.Limitations.push_back(
        "block copy/dispose helper bodies require recovery");
  }
  if (Flags & HasCtor)
    Result.Limitations.push_back(
        "block captures require C++ construction semantics");
  constexpr uint32_t KnownFlags = NoEscape | HasCopyDispose | HasCtor |
                                  IsGlobal | HasStret | HasSignature |
                                  HasExtendedLayout;
  if (Flags & ~KnownFlags)
    Result.Limitations.push_back(
        "block has unsupported runtime or descriptor flags");
  if (!(Flags & HasSignature)) {
    Result.Limitations.push_back("block has no ABI.2010.3.16 invoke signature");
    if (Result.LiteralSize > HeaderSize)
      Result.Captures.push_back({ObjCBlockCaptureRange::Kind::Unknown,
                                 HeaderSize, Result.LiteralSize - HeaderSize});
    return Result;
  }
  if (!Data.bytes(Cursor, 8))
    return Reject("block signature descriptor field is truncated");
  const auto SignatureAddress = Data.pointer(Cursor);
  if (!SignatureAddress)
    return Reject("block signature pointer has an unresolved fixup");
  Result.SignatureAddress = *SignatureAddress;
  const auto Signature = Data.string(*SignatureAddress);
  if (Signature)
    Result.Signature = *Signature;
  std::string SignatureError;
  if (Flags & HasStret)
    Result.Limitations.push_back("block stret return ABI is unsupported");
  else {
    Result.InvokeTypeHint =
        parseObjCBlockSignature(Result.Signature, Image.Arch, SignatureError);
    if (!Result.InvokeTypeHint)
      Result.Limitations.push_back(std::move(SignatureError));
  }
  captureLayout(Data, Result, Cursor + 8);
  return Result;
}

static std::optional<ObjCBlockLiteral>
readLiteral(const BinaryImage &Image,
            const ImportStorageSlotCollection &Imports, va_t Address,
            std::string &Diagnostic) {
  Diagnostic.clear();
  auto Reject = [&](const char *Reason) -> std::optional<ObjCBlockLiteral> {
    Diagnostic = Reason;
    return std::nullopt;
  };
  if (!supportedImage(Image))
    return Reject("block literal requires linked Darwin arm64/x86_64");
  const objc::RuntimeData Data(Image);
  if (!Data.bytes(Address, HeaderSize))
    return Reject("block literal header is not file-backed");
  const auto Isa = Imports.Slots.find(Address);
  if (Isa == Imports.Slots.end() || Imports.Conflicts.count(Address) ||
      Isa->second.Addend ||
      (!globalIsa(Isa->second.Name) && !stackIsa(Isa->second.Name)))
    return Reject("block isa field has no exact concrete-block import binding");
  const auto Flags = Data.u32(Address + 8);
  const auto Invoke = Data.pointer(Address + 16);
  const auto Descriptor = Data.pointer(Address + 24);
  if (!Flags || !Invoke || !Image.isCodeAddress(*Invoke) || !Descriptor ||
      !*Descriptor)
    return Reject("block invoke or descriptor lacks a resolved file identity");
  const bool Global = globalIsa(Isa->second.Name);
  if ((Global && !(*Flags & IsGlobal)) ||
      (!Global && (*Flags & IsGlobal) && !(*Flags & NoEscape)))
    return Reject("block concrete class disagrees with its storage flags");
  auto Metadata =
      readObjCBlockDescriptor(Image, *Descriptor, *Flags, Diagnostic);
  if (!Metadata)
    return std::nullopt;
  if (!Data.bytes(Address, Metadata->LiteralSize))
    return Reject("block capture storage is not completely file-backed");
  ObjCBlockLiteral Result;
  Result.StorageKind =
      Global ? ObjCBlockLiteral::Kind::Global : ObjCBlockLiteral::Kind::Stack;
  Result.Address = Address;
  Result.InvokeEntry = *Invoke;
  Result.Descriptor = std::move(*Metadata);
  return Result;
}

std::optional<ObjCBlockLiteral> readObjCBlockLiteral(const BinaryImage &Image,
                                                     va_t Address,
                                                     std::string &Diagnostic) {
  return readLiteral(Image, Image.collectImportStorageSlots(), Address,
                     Diagnostic);
}

std::vector<ObjCBlockLiteral>
findObjCGlobalBlocks(const BinaryImage &Image,
                     std::map<va_t, std::string> *RejectedCandidates) {
  std::vector<ObjCBlockLiteral> Result;
  if (!supportedImage(Image))
    return Result;
  const auto Imports = Image.collectImportStorageSlots();
  size_t Candidates = 0;
  for (const auto &[Address, Import] : Imports.Slots) {
    if (!globalIsa(Import.Name))
      continue;
    if (++Candidates > 65536) {
      if (RejectedCandidates)
        (*RejectedCandidates)[Address] =
            "block discovery candidate budget exceeded";
      break;
    }
    std::string Diagnostic;
    auto Literal = readLiteral(Image, Imports, Address, Diagnostic);
    if (Literal)
      Result.push_back(std::move(*Literal));
    else if (RejectedCandidates)
      (*RejectedCandidates)[Address] = std::move(Diagnostic);
  }
  return Result;
}
} // namespace neverd
