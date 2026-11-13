//===- PageBytes.h - Where a page's bytes stand in its text -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The engine names the places of a source page by the bytes of its UTF-8,
/// and a view holds the page as a QString.  PageBytes answers where a byte
/// stands in that text and what a run of the bytes spells, as
/// QString::fromUtf8 reads the bytes: for the many names of a page without
/// reading the bytes before each of them again.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_PAGE_BYTES_H
#define NEVERD_TOOLS_NEVERD_GUI_PAGE_BYTES_H

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QVector>

namespace neverd::gui {

/// The UTF-8 of a page's text, and the places of its bytes in the text.
class PageBytes {
public:
  explicit PageBytes(const QString &text)
      : bytes_(text.toUtf8()), ascii_(bytes_.size() == text.size()),
        counted_(!ascii_ && !bytes_.startsWith("\xef\xbb\xbf")) {}

  /// The bytes of the page.
  qsizetype size() const { return bytes_.size(); }

  /// The characters, in UTF-16 units, that QString::fromUtf8 reads from the
  /// first \p byte bytes of the page: where that byte stands in the text.
  /// \p byte is from 0 to size().
  int charsBefore(qsizetype byte) const {
    // A page of as many bytes as its text has units has each where its
    // unit is.
    if (ascii_)
      return int(byte);
    // Of another, the units before every byte that begins a character are
    // counted once.  QString::fromUtf8 reads a mark of byte order at the
    // beginning as nothing, and the bytes of half a character as none of
    // it: no place the engine names, and both are read as it reads them.
    if (counted_) {
      if (before_.isEmpty())
        count();
      if (const int known = before_[byte]; known >= 0)
        return known;
    }
    return int(QString::fromUtf8(QByteArrayView(bytes_).first(byte)).size());
  }

  /// What QString::fromUtf8 reads from the \p count bytes at \p begin.
  QString spelled(qsizetype begin, qsizetype count) const {
    const QByteArrayView run = QByteArrayView(bytes_).sliced(begin, count);
    return ascii_ ? QString::fromLatin1(run) : QString::fromUtf8(run);
  }

private:
  void count() const {
    before_.fill(-1, bytes_.size() + 1);
    int units = 0;
    for (qsizetype at = 0; at < bytes_.size();) {
      const auto lead = static_cast<unsigned char>(bytes_[at]);
      before_[at] = units;
      // What QString::toUtf8 wrote is well formed: a character of four
      // bytes is two units of the text, any other is one.
      const int length = lead < 0x80   ? 1
                         : lead < 0xe0 ? 2
                         : lead < 0xf0 ? 3
                                       : 4;
      units += length == 4 ? 2 : 1;
      at += length;
    }
    before_[bytes_.size()] = units;
  }

  QByteArray bytes_;
  /// Every byte is a unit of the text.
  bool ascii_;
  /// The units before a byte are those counted in before_.
  bool counted_;
  /// The units before each byte that begins a character, -1 for the bytes
  /// inside one; empty until first asked for.
  mutable QVector<int> before_;
};

} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_PAGE_BYTES_H
