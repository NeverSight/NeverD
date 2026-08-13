//===- BinaryImage.h - Binary image data model ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the BinaryImage structure that holds all data parsed from a
/// binary file (segments, sections, symbols, imports, exports, relocations)
/// and the Loader interface that concrete format loaders implement.
///
/// Design follows LLVM conventions (see llvm/Object/ObjectFile.h):
///   - BinaryImage is the format-agnostic output of all loaders.
///   - Section models fine-grained regions (.text, .data, .bss, etc.).
///   - Segment models coarse load regions (ELF PT_LOAD, MachO LC_SEGMENT).
///   - RelocationEntry captures parsed relocation metadata for codegen.
///   - Loader::create() provides LLVM-style auto-detection factory.
///
/// This is an umbrella header.  Each part of the model lives in a header of
/// its own, and this one names them all so that a consumer keeps one include
/// for the whole data model.  Include a part directly when only that part is
/// needed.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_BINARYIMAGE_H
#define NEVERD_LOADER_BINARYIMAGE_H

#include "neverd/Common.h"
#include "neverd/evm/EVMImageMetadata.h"
#include "neverd/loader/BinaryImageDynamic.h"
#include "neverd/loader/BinaryImageFlags.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/loader/BinaryImageRelocation.h"
#include "neverd/loader/BinaryImageSection.h"
#include "neverd/loader/ExceptionInfo.h"
#include "neverd/loader/Loader.h"
#include "neverd/object/SectionNames.h"
#include "neverd/sbf/SBFMetadata.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <set>

// ===--------------------------------------------------------------------===//
// Well-known section names — avoids scattered string literals
// ===--------------------------------------------------------------------===//

// section_names are defined in neverd/object/SectionNames.h
// (included above) and re-exported here for backward compatibility.

#endif // NEVERD_LOADER_BINARYIMAGE_H
