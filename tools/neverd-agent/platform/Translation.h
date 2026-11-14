//===- Translation.h - Agent interface translations -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Install the Agent's translations for one of the workbench's interface
/// languages, in the desktop application and in its CLI alike.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_AGENT_PLATFORM_TRANSLATION_H
#define NEVERD_TOOLS_AGENT_PLATFORM_TRANSLATION_H

#include <QString>

namespace neverd::agent {

/// Install the Agent's and Qt's translations for \p code (English for an
/// unknown code) in place of earlier ones; returns the language installed.
QString installTranslations(const QString &code);
/// The language installTranslations() installed; English before any call.
QString interfaceLanguage();
/// The interface language's English name, as model instructions name it.
QString interfaceLanguageEnglishName();
/// Whether \p code's script runs right to left.
bool isRightToLeft(const QString &code);

} // namespace neverd::agent

#endif // NEVERD_TOOLS_AGENT_PLATFORM_TRANSLATION_H
