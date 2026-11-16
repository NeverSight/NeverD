//===- RenamedPages.h - A source text's pages under a new name --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A function's new name reaches the source text of every function that links
/// it: the worker writes the name in each place its text links the address and
/// moves the spans after them (neverd::worker::CodeEdits::takeFunctionNames).
/// The pages a view holds say all that takes, so a view may read them as the
/// rename leaves them before the worker has answered it, and reads what the
/// worker made of them when it has.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_RENAMED_PAGES_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_RENAMED_PAGES_H

#include "../PageBytes.h"
#include "Address.h"

#include <QJsonObject>
#include <QString>
#include <QVector>
#include <optional>
#include <vector>

namespace neverd::gui {

/// What a new name of something writes into the pages of a source text that
/// link it: read from the pages once, and written into them when asked.
class PagesRenaming {
public:
  /// What calling \p address \p name writes into \p pages, the whole of a
  /// source text in order as a worker gave them.
  ///
  /// Nothing where the pages do not say what the worker would give: where
  /// the name is none a worker takes (it is empty, holds a space or a control
  /// character, or is one the engine gives by itself, such as `sub_1234`;
  /// whether another address has the name only the worker knows), the text
  /// spells it already as a word,
  /// which may then link there too, no place links the address, or a page
  /// does not name its places by bytes as a decorated source page does.
  static std::optional<PagesRenaming>
  read(const QVector<QJsonObject> &pages, Address address, const QString &name);
  /// The places the name is written.
  int places() const { return int(places_.size()); }
  /// One of them names data and not a function.  Its name is the engine's
  /// text's too, which the worker reads again for what else the name moves.
  bool data() const { return data_; }
  /// The pages as that worker gives them once the address has the name and
  /// nothing else has changed: the name in each place a page links the
  /// address, every span of bytes after one moved by what the name's length
  /// differs, and the name where a page states whose text it is or whom a
  /// library operation of it calls.
  QVector<QJsonObject> pages() const;

private:
  /// The bytes of the text a name is written over, by their place in it.
  struct Place {
    qint64 begin, end;
  };
  PagesRenaming() = default;
  QVector<QJsonObject> pages_;
  /// Each page's place in the text, and where each byte of it stands in its
  /// characters.
  QVector<qint64> offsets_;
  std::vector<PageBytes> texts_;
  QVector<Place> places_;
  Address address_ = 0;
  QString name_;
  qsizetype nameBytes_ = 0;
  bool data_ = false;
};

/// The pages of a source text once something they link has another name.
struct RenamedPages {
  QVector<QJsonObject> pages;
  int places = 0;
  bool data = false;
};

/// PagesRenaming::read of \p pages and its pages, in one.
std::optional<RenamedPages> pagesRenamed(const QVector<QJsonObject> &pages,
                                         Address address, const QString &name);

} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_APP_RENAMED_PAGES_H
