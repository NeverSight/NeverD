//===- NdTypes.h - NeverD IR type system --------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the NdType structure representing types in the NeverD IR,
/// including integer, float, pointer, array, struct, and function types.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_NDTYPES_H
#define NEVERD_IR_NDTYPES_H

#include "neverd/Common.h"
#include "neverd/types/CallingConvention.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neverd {

enum class NdTypeKind : uint8_t {
  Void,
  Int,
  Float,
  Ptr,
  Array,
  Struct,
  Func,
  Unknown
};

/// How a calling convention passes a record argument.  Unknown where the
/// source does not say; a convention then places no argument from it on.
enum class NdRecordPassing : uint8_t {
  Unknown,
  /// As its bytes, as C passes its records.
  ByValue,
  /// By the address of a copy, as the Itanium C++ ABI passes a class with a
  /// non-trivial copy constructor or destructor.
  ByReference,
  /// A complex number, whose two parts most conventions pass as such a
  /// record's bytes but some return in registers where they would return
  /// the record through memory.
  Complex,
  /// A record of a program that follows the Microsoft C++ ABI, which does
  /// not say whether the class is trivially copyable: i386 passes it as its
  /// bytes, x64 as its bytes or as the address of a copy in the same
  /// position.
  Microsoft,
};

/// How an access holds the value it reads or writes, which picks one of a
/// union's alternatives of its width: `v.d` for a double the code holds,
/// `v.i` for an integer.
enum class NdAccessClass : uint8_t {
  /// Not known: alternatives of the access's width stay raw.
  Unknown,
  Integer,
  Pointer,
  Floating,
};

/// A scalar in a value's bytes: where it starts, its size, and whether it is
/// floating point.  A bit field is the bytes that hold its bits.
struct NdScalarLeaf {
  uint16_t Offset = 0;
  uint16_t Size = 0;
  bool Floating = false;
};

struct NdType {
  NdTypeKind Kind = NdTypeKind::Unknown;
  uint16_t Size = 0;
  bool IsSigned = false;

  /// For Ptr
  std::shared_ptr<NdType> Pointee;

  /// For Array
  uint32_t ArrayCount = 0;
  std::shared_ptr<NdType> ElemType;

  /// Natural record layout for source projection. Member names are not
  /// recovered; the ordered types and byte offsets define structural identity.
  std::vector<std::shared_ptr<NdType>> Fields;
  std::vector<uint16_t> FieldOffsets;
  uint16_t Alignment = 0;

  /// For Func
  std::shared_ptr<NdType> RetType;
  std::vector<std::shared_ptr<NdType>> ParamTypes;
  /// The callback's declared source convention. Unspecified is the platform
  /// C default; null means a source convention that cannot be represented.
  /// This does not authorize an ordinary-C ABI projection for other
  /// conventions.
  std::optional<types::CallingConvention> FunctionConvention =
      types::CallingConvention::Unspecified;

  /// Display-only C++/C spelling from a validated type graph
  /// (`char`, `ProbeError`, `Ns::Outer::Inner`).  A SourceName-only
  /// struct is not an ABI layout and must not authorize extents.
  std::string SourceName;
  /// Display-only: the typedef a declaration names this type by, such as
  /// PKDEFERRED_ROUTINE for a pointer to a function, which a signature
  /// prints where its unit defines the typedef.  Never an ABI fact.
  std::string TypedefName;
  /// Display-only TPI member names.  Parallel to FieldDisplayOffsets
  /// and FieldDisplayTypes; never an ABI layout and must not authorize
  /// extents.  An empty name at an offset is a tombstone for an
  /// ambiguous union/bitfield slot.
  std::vector<std::string> FieldDisplayNames;
  std::vector<uint16_t> FieldDisplayOffsets;
  /// Display-only TPI member types, used to name nested slots such as
  /// `TPtr<T>::p` at `outer+8`.  Not an ABI layout.
  std::vector<std::shared_ptr<NdType>> FieldDisplayTypes;
  /// Alignment of the current validated C display definition. Like its
  /// display members, this does not authorize an ABI or native memory extent.
  uint16_t FieldDisplayAlignment = 0;
  /// LF_ENUM records share the named-record encoding; they return in RAX.
  bool IsEnum = false;
  /// A record of the registers a multi-register return fills, one scalar
  /// member per register in the order the call site packs them.  It is no
  /// source layout: C spells and builds it, and no source calling convention
  /// classifies it, so its members may mix integer and floating registers.
  bool RegisterReturnRecord = false;
  /// For a record: how a call passes it (NdRecordPassing).
  NdRecordPassing Passing = NdRecordPassing::Unknown;
  /// For a record the debug information lays out: its scalars in offset
  /// order, through nested records and arrays, all of them or none.  They
  /// say where a calling convention passes the record; they are no C layout
  /// and no display members, and authorize no spelling or extents.
  std::vector<NdScalarLeaf> ScalarLeaves;

  /// The class of an access that holds a value of this type: an integer's
  /// or an enumeration's, a pointer's or a floating value's.
  NdAccessClass accessClass() const {
    switch (Kind) {
    case NdTypeKind::Int:
      return NdAccessClass::Integer;
    case NdTypeKind::Ptr:
      return NdAccessClass::Pointer;
    case NdTypeKind::Float:
      return NdAccessClass::Floating;
    case NdTypeKind::Struct:
      return IsEnum ? NdAccessClass::Integer : NdAccessClass::Unknown;
    default:
      return NdAccessClass::Unknown;
    }
  }

  /// The index of the unique display field at exactly \p Offset.
  /// Ambiguous or unnamed slots return nullopt; do not guess a member.  A
  /// nonzero \p AccessSize keeps the unique overlay whose type width
  /// matches (CPtred `_m_nRef` vs `_m_pNext` at +8), a scalar's over a
  /// record's of that width, which no scalar access reads whole
  /// (`QuadPart` over the `u` of a LARGE_INTEGER).  Same-width scalar
  /// overlays stay raw unless the access's \p Class picks one
  /// (scalarOfClassAt).
  std::optional<size_t>
  displayFieldIndexAt(uint64_t Offset, uint16_t AccessSize = 0,
                      NdAccessClass Class = NdAccessClass::Unknown) const {
    if (Kind != NdTypeKind::Struct ||
        FieldDisplayNames.size() != FieldDisplayOffsets.size())
      return std::nullopt;
    const bool FilterSize = AccessSize != 0 && FieldDisplayTypes.size() ==
                                                   FieldDisplayOffsets.size();
    std::optional<size_t> Found;
    bool FoundRecord = false;
    for (size_t I = 0; I < FieldDisplayOffsets.size(); ++I) {
      if (FieldDisplayOffsets[I] != Offset || FieldDisplayNames[I].empty())
        continue;
      bool Record = false;
      if (FilterSize) {
        const auto &Ty = FieldDisplayTypes[I];
        // An array of no stated length holds no access whole: one of a
        // flexible array member reads its elements.
        if (Ty && ((Ty->Size != 0 && Ty->Size != AccessSize) ||
                   (Ty->Kind == NdTypeKind::Array && Ty->Size == 0)))
          continue;
        Record = Ty && Ty->Kind == NdTypeKind::Struct && !Ty->IsEnum;
      }
      if (Found && FieldDisplayNames[*Found] != FieldDisplayNames[I]) {
        if (FoundRecord == Record)
          return Record ? std::nullopt
                        : scalarOfClassAt(Offset, AccessSize, Class);
        if (FoundRecord) {
          Found = I;
          FoundRecord = false;
        }
        continue;
      }
      Found = I;
      FoundRecord = Record;
    }
    return Found;
  }

  /// Of the scalar alternatives of \p AccessSize bytes at \p Offset, the
  /// one an access of \p Class holds: the only one of that class, else, as
  /// the machine holds a pointer as an integer of its width, the only
  /// integer for a pointer or the only pointer for an integer.  None where
  /// the class leaves two, or none.
  std::optional<size_t> scalarOfClassAt(uint64_t Offset, uint16_t AccessSize,
                                        NdAccessClass Class) const {
    if (Class == NdAccessClass::Unknown || !AccessSize ||
        FieldDisplayTypes.size() != FieldDisplayOffsets.size() ||
        FieldDisplayNames.size() != FieldDisplayOffsets.size())
      return std::nullopt;
    struct Choice {
      std::optional<size_t> Index;
      bool Ambiguous = false;
    };
    const auto choose = [&](NdAccessClass Want) {
      Choice Chosen;
      for (size_t I = 0; I < FieldDisplayOffsets.size(); ++I) {
        const auto &Ty = FieldDisplayTypes[I];
        if (FieldDisplayOffsets[I] != Offset || FieldDisplayNames[I].empty() ||
            !Ty || Ty->Size != AccessSize || Ty->accessClass() != Want)
          continue;
        if (Chosen.Index &&
            FieldDisplayNames[*Chosen.Index] != FieldDisplayNames[I]) {
          Chosen.Ambiguous = true;
          break;
        }
        if (!Chosen.Index)
          Chosen.Index = I;
      }
      return Chosen;
    };
    const Choice Exact = choose(Class);
    if (Exact.Ambiguous || Exact.Index || Class == NdAccessClass::Floating)
      return Exact.Ambiguous ? std::nullopt : Exact.Index;
    const Choice Other =
        choose(Class == NdAccessClass::Integer ? NdAccessClass::Pointer
                                               : NdAccessClass::Integer);
    return Other.Ambiguous ? std::nullopt : Other.Index;
  }

  /// The name of the unique display field at exactly \p Offset
  /// (displayFieldIndexAt).
  std::optional<std::string>
  displayFieldNameAt(uint64_t Offset, uint16_t AccessSize = 0,
                     NdAccessClass Class = NdAccessClass::Unknown) const {
    if (const std::optional<size_t> Index =
            displayFieldIndexAt(Offset, AccessSize, Class))
      return FieldDisplayNames[*Index];
    return std::nullopt;
  }

  /// \p ArrayElements false keeps an array a single member, as a writer that
  /// names no elements reads it.
  std::shared_ptr<NdType>
  displayFieldTypeAt(uint64_t Offset, uint16_t AccessSize = 0,
                     bool ArrayElements = true,
                     NdAccessClass Class = NdAccessClass::Unknown) const {
    if (Kind != NdTypeKind::Struct ||
        FieldDisplayNames.size() != FieldDisplayOffsets.size() ||
        FieldDisplayTypes.size() != FieldDisplayOffsets.size())
      return nullptr;
    if (const std::optional<size_t> Exact =
            displayFieldIndexAt(Offset, AccessSize, Class)) {
      const std::shared_ptr<NdType> &ExactTy = FieldDisplayTypes[*Exact];
      if (ExactTy && ExactTy->Kind == NdTypeKind::Struct && !ExactTy->IsEnum)
        if (auto Inner = ExactTy->displayFieldTypeAt(0, AccessSize,
                                                     ArrayElements, Class))
          return Inner;
      return ExactTy;
    }
    const std::optional<size_t> Best =
        displayContainerAt(Offset, ArrayElements);
    if (!Best)
      return nullptr;
    const std::shared_ptr<NdType> &Container = FieldDisplayTypes[*Best];
    const uint64_t Rel =
        Offset - static_cast<uint64_t>(FieldDisplayOffsets[*Best]);
    if (Container->Kind == NdTypeKind::Array)
      return displayElementTypeAt(*Container, Rel, AccessSize, Class);
    if (Rel == 0) {
      if (auto Inner = Container->displayFieldTypeAt(0, AccessSize,
                                                     ArrayElements, Class))
        return Inner;
      // The record itself only where the access reads it whole.
      if (AccessSize != 0 && Container->Size != 0 &&
          Container->Size != AccessSize)
        return nullptr;
      return Container;
    }
    return Container->displayFieldTypeAt(Rel, AccessSize, ArrayElements, Class);
  }

  /// Unique display path at \p Offset, including nested record steps
  /// (`holder.p`, `record.id`).  No exact slot and no unique
  /// containing record field stay raw. A nonzero \p AccessSize may
  /// pick one overlay at a shared offset; same-width overlays stay raw.
  std::optional<std::string>
  displayFieldPathAt(uint64_t Offset, uint16_t AccessSize = 0,
                     bool EnterNestedAtZero = true, bool ArrayElements = true,
                     NdAccessClass Class = NdAccessClass::Unknown) const {
    if (auto Exact = displayFieldNameAt(Offset, AccessSize, Class)) {
      if (EnterNestedAtZero &&
          FieldDisplayTypes.size() == FieldDisplayOffsets.size()) {
        for (size_t I = 0; I < FieldDisplayOffsets.size(); ++I) {
          if (FieldDisplayOffsets[I] != Offset ||
              FieldDisplayNames[I] != *Exact || !FieldDisplayTypes[I] ||
              FieldDisplayTypes[I]->Kind != NdTypeKind::Struct ||
              FieldDisplayTypes[I]->IsEnum)
            continue;
          if (auto Inner = FieldDisplayTypes[I]->displayFieldPathAt(
                  0, AccessSize, EnterNestedAtZero, ArrayElements, Class))
            return *Exact + "." + *Inner;
        }
      }
      return Exact;
    }
    const std::optional<size_t> Best =
        displayContainerAt(Offset, ArrayElements);
    if (!Best)
      return std::nullopt;
    const std::shared_ptr<NdType> &Container = FieldDisplayTypes[*Best];
    const std::string &Name = FieldDisplayNames[*Best];
    const uint64_t Rel =
        Offset - static_cast<uint64_t>(FieldDisplayOffsets[*Best]);
    if (Container->Kind == NdTypeKind::Array)
      return displayElementPathAt(Name, *Container, Rel, AccessSize,
                                  EnterNestedAtZero, Class);
    if (Rel == 0) {
      if (EnterNestedAtZero) {
        if (auto Inner = Container->displayFieldPathAt(
                0, AccessSize, EnterNestedAtZero, ArrayElements, Class))
          return Name + "." + *Inner;
      }
      // The record itself only where the access reads it whole: a byte of a
      // header is no header.
      if (AccessSize != 0 && Container->Size != 0 &&
          Container->Size != AccessSize)
        return std::nullopt;
      return Name;
    }
    auto Inner = Container->displayFieldPathAt(
        Rel, AccessSize, EnterNestedAtZero, ArrayElements, Class);
    if (!Inner)
      return std::nullopt;
    return Name + "." + *Inner;
  }

  /// Whether display field \p I is a flexible array member, an array of no
  /// stated length that ends its record (`const fde *array[]`): it holds
  /// every byte from its start.
  bool isFlexibleArrayAt(size_t I) const {
    if (FieldDisplayTypes.size() != FieldDisplayOffsets.size() ||
        I >= FieldDisplayTypes.size())
      return false;
    const auto &Ty = FieldDisplayTypes[I];
    if (!Ty || Ty->Kind != NdTypeKind::Array || Ty->Size != 0 ||
        !Ty->ElemType || Ty->ElemType->Size == 0)
      return false;
    for (const uint16_t Offset : FieldDisplayOffsets)
      if (Offset > FieldDisplayOffsets[I])
        return false;
    return true;
  }

  /// The display field of a record type that holds the byte at \p Offset
  /// with others: the record or array member that starts last at or before
  /// it.  None where none holds it, or two such members start together
  /// under different names, as a union's alternatives do.
  std::optional<size_t> displayContainerAt(uint64_t Offset,
                                           bool ArrayElements = true) const {
    if (Kind != NdTypeKind::Struct ||
        FieldDisplayNames.size() != FieldDisplayOffsets.size() ||
        FieldDisplayTypes.size() != FieldDisplayOffsets.size())
      return std::nullopt;
    std::optional<size_t> Best;
    for (size_t I = 0; I < FieldDisplayOffsets.size(); ++I) {
      const auto &Ty = FieldDisplayTypes[I];
      const bool Flexible = ArrayElements && isFlexibleArrayAt(I);
      if (FieldDisplayNames[I].empty() || !Ty ||
          (Ty->Kind != NdTypeKind::Struct && !Flexible &&
           (!ArrayElements || Ty->Kind != NdTypeKind::Array || !Ty->ElemType ||
            Ty->ElemType->Size == 0 || Ty->Size == 0)) ||
          FieldDisplayOffsets[I] > Offset)
        continue;
      const uint64_t Rel =
          Offset - static_cast<uint64_t>(FieldDisplayOffsets[I]);
      if (Rel != 0 && !Flexible && (Ty->Size == 0 || Rel >= Ty->Size))
        continue;
      if (!Best || FieldDisplayOffsets[I] > FieldDisplayOffsets[*Best]) {
        Best = I;
        continue;
      }
      if (FieldDisplayOffsets[I] == FieldDisplayOffsets[*Best] &&
          FieldDisplayNames[I] != FieldDisplayNames[*Best]) {
        // A record alternative names its bytes as before arrays held
        // elements; an array overlaying it gives way.
        const bool BestArray =
            FieldDisplayTypes[*Best]->Kind == NdTypeKind::Array;
        const bool Array = Ty->Kind == NdTypeKind::Array;
        if (BestArray && !Array)
          Best = I;
        else if (BestArray == Array)
          return std::nullopt;
      }
    }
    return Best;
  }

  /// The element of the array \p Array, named \p Name, that holds the
  /// \p AccessSize bytes \p Rel bytes into it: `Name[i]` where the access
  /// reads the element whole or takes an address in the array, or a path
  /// into a record or array element (`Name[i].x`, `Name[i][j]`).  None
  /// where no element holds the bytes whole.  An array of no stated length
  /// (a flexible array member) holds every element.
  static std::optional<std::string>
  displayElementPathAt(const std::string &Name, const NdType &Array,
                       uint64_t Rel, uint16_t AccessSize,
                       bool EnterNestedAtZero,
                       NdAccessClass Class = NdAccessClass::Unknown) {
    const std::shared_ptr<NdType> &Elem = Array.ElemType;
    if (!Elem || Elem->Size == 0 || (Array.Size != 0 && Rel >= Array.Size))
      return std::nullopt;
    const std::string Element =
        Name + "[" + std::to_string(Rel / Elem->Size) + "]";
    const uint64_t Inner = Rel % Elem->Size;
    const bool Record = Elem->Kind == NdTypeKind::Struct && !Elem->IsEnum;
    if (Inner == 0) {
      if (Record && EnterNestedAtZero)
        if (auto Member = Elem->displayFieldPathAt(
                0, AccessSize, EnterNestedAtZero, true, Class))
          return Element + "." + *Member;
      if (AccessSize == 0 || AccessSize == Elem->Size)
        return Element;
    }
    if (Record) {
      if (Inner == 0)
        return std::nullopt;
      auto Member = Elem->displayFieldPathAt(Inner, AccessSize,
                                             EnterNestedAtZero, true, Class);
      if (!Member)
        return std::nullopt;
      return Element + "." + *Member;
    }
    if (Elem->Kind == NdTypeKind::Array)
      return displayElementPathAt(Element, *Elem, Inner, AccessSize,
                                  EnterNestedAtZero, Class);
    return std::nullopt;
  }

  /// The type of what displayElementPathAt names.
  static std::shared_ptr<NdType>
  displayElementTypeAt(const NdType &Array, uint64_t Rel, uint16_t AccessSize,
                       NdAccessClass Class = NdAccessClass::Unknown) {
    const std::shared_ptr<NdType> &Elem = Array.ElemType;
    if (!Elem || Elem->Size == 0 || (Array.Size != 0 && Rel >= Array.Size))
      return nullptr;
    const uint64_t Inner = Rel % Elem->Size;
    const bool Record = Elem->Kind == NdTypeKind::Struct && !Elem->IsEnum;
    if (Inner == 0) {
      if (Record)
        if (auto Member = Elem->displayFieldTypeAt(0, AccessSize, true, Class))
          return Member;
      if (AccessSize == 0 || AccessSize == Elem->Size)
        return Elem;
    }
    if (Record)
      return Inner == 0
                 ? nullptr
                 : Elem->displayFieldTypeAt(Inner, AccessSize, true, Class);
    if (Elem->Kind == NdTypeKind::Array)
      return displayElementTypeAt(*Elem, Inner, AccessSize, Class);
    return nullptr;
  }

  NdType() = default;
  NdType(const NdType &) = default;
  NdType(NdType &&) = default;
  NdType &operator=(const NdType &) = default;
  NdType &operator=(NdType &&) = default;
  /// Scalar factories never populate display fields. A smashed vector
  /// header there is leaked rather than walked during Session teardown.
  ~NdType();

  static std::shared_ptr<NdType> makeVoid() {
    auto T = std::make_shared<NdType>();
    T->Kind = NdTypeKind::Void;
    T->Size = 0;
    return T;
  }
  static std::shared_ptr<NdType> makeInt(uint16_t Sz, bool S = true) {
    auto T = std::make_shared<NdType>();
    T->Kind = NdTypeKind::Int;
    T->Size = Sz;
    T->IsSigned = S;
    return T;
  }
  static std::shared_ptr<NdType> makeFloat(uint16_t Sz) {
    auto T = std::make_shared<NdType>();
    T->Kind = NdTypeKind::Float;
    T->Size = Sz;
    return T;
  }
  static std::shared_ptr<NdType> makePtr(std::shared_ptr<NdType> Pt = nullptr) {
    auto T = std::make_shared<NdType>();
    T->Kind = NdTypeKind::Ptr;
    T->Size = 8;
    T->Pointee = Pt ? Pt : makeVoid();
    return T;
  }
  /// Construct a bounded naturally aligned record. Unsupported, cyclic or
  /// malformed member layouts return null rather than guessing padding.
  static std::shared_ptr<NdType>
  makeStruct(std::vector<std::shared_ptr<NdType>> Fields);
  static std::shared_ptr<NdType>
  makeNamedRecord(std::string Name, uint16_t Size = 0, bool Enum = false) {
    auto T = std::make_shared<NdType>();
    T->Kind = NdTypeKind::Struct;
    T->Size = Size;
    T->SourceName = std::move(Name);
    T->IsEnum = Enum;
    return T;
  }
  static std::shared_ptr<NdType> makeArray(std::shared_ptr<NdType> Elem,
                                           uint32_t Count, uint16_t Size) {
    auto T = std::make_shared<NdType>();
    T->Kind = NdTypeKind::Array;
    T->ElemType = std::move(Elem);
    T->ArrayCount = Count;
    T->Size = Size;
    return T;
  }
  /// A fixed function type with its source declaration convention. An ABI
  /// projection must separately validate that it supports this convention.
  static std::shared_ptr<NdType>
  makeFunc(std::shared_ptr<NdType> Return,
           std::vector<std::shared_ptr<NdType>> Parameters = {},
           std::optional<types::CallingConvention> Convention =
               types::CallingConvention::Unspecified) {
    auto T = std::make_shared<NdType>();
    T->Kind = NdTypeKind::Func;
    T->RetType = std::move(Return);
    T->ParamTypes = std::move(Parameters);
    T->FunctionConvention = Convention;
    return T;
  }

  std::string str() const;
};

using TypeRef = std::shared_ptr<NdType>;

} // namespace neverd

#endif // NEVERD_IR_NDTYPES_H
