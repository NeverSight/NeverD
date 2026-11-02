//===- UnwindInfoWords.h - Unwind inspection constants ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared vocabulary for the GUI query admission and presentation adapter.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_APP_UNWINDINFOWORDS_H
#define NEVERD_GUI_APP_UNWINDINFOWORDS_H

#include <QtGlobal>

namespace neverd::gui::unwind_info {
#define NEVERD_UNWIND_UI_TEXT(Name, Text) inline constexpr char Name[] = Text;
#define NEVERD_UNWIND_UI_VALUE(Name, Value) inline constexpr int Name = Value;
#include "UnwindInfo.def"
} // namespace neverd::gui::unwind_info

#endif // NEVERD_GUI_APP_UNWINDINFOWORDS_H
