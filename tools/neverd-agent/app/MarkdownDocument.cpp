//===- MarkdownDocument.cpp - Native chat document -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Style parsed document structure rather than implementing Markdown with
/// regular expressions. Streaming reparses only the changed message.
//===----------------------------------------------------------------------===//
#include "MarkdownDocument.h"

#include <QClipboard>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextTable>
#include <algorithm>

namespace neverd::agent {
namespace {
constexpr int kCodeFrameProperty = QTextFormat::UserProperty;
constexpr int kCodeSpacerProperty = QTextFormat::UserProperty + 1;
} // namespace
MarkdownDocument::MarkdownDocument(QObject *parent) : QObject(parent) {
  Document.setUndoRedoEnabled(false);
  Document.setDocumentMargin(0);
}
void MarkdownDocument::setDocument(QQuickTextDocument *document) {
  if (Target == document)
    return;
  Target = document;
  if (Target) {
    Document.setDefaultFont(Target->textDocument()->defaultFont());
    Target->setTextDocument(&Document);
  }
  emit documentChanged();
}
void MarkdownDocument::render(const QString &source, bool markdown, QColor text,
                              QColor code, QColor link, QColor border,
                              const QFont &font) {
  Document.setDefaultFont(font);
  Source = source;
  CodeBlocks.clear();
  if (markdown)
    Document.setMarkdown(source, QTextDocument::MarkdownFeatures(
                                     QTextDocument::MarkdownDialectGitHub) |
                                     QTextDocument::MarkdownNoHTML);
  else
    Document.setPlainText(source);
  // The document is attached to a live TextEdit. One edit block makes the
  // formatting passes below relayout once instead of once per fragment.
  QTextCursor batch(&Document);
  batch.beginEditBlock();
  // Replace images with labels. Both automatic remote loads and invisible
  // broken-image placeholders are inappropriate in a research transcript.
  QList<QPair<int, QString>> images;
  for (auto block = Document.begin(); block.isValid(); block = block.next())
    for (auto it = block.begin(); !it.atEnd(); ++it) {
      const auto fragment = it.fragment();
      if (fragment.charFormat().isImageFormat())
        images.append(
            {fragment.position(), fragment.charFormat()
                                      .property(QTextFormat::ImageAltText)
                                      .toString()});
    }
  for (auto it = images.crbegin(); it != images.crend(); ++it) {
    QTextCursor cursor(&Document);
    cursor.setPosition(it->first);
    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    cursor.insertText("[" + (it->second.isEmpty() ? tr("图片") : it->second) +
                      "]");
  }
  const auto mono =
      QFontDatabase::hasFamily("JetBrains Mono NL")
          ? QString("JetBrains Mono NL")
          : QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
  const auto isCode = [markdown](const QTextBlockFormat &format) {
    return markdown && (format.hasProperty(QTextFormat::BlockCodeFence) ||
                        format.hasProperty(QTextFormat::BlockCodeLanguage) ||
                        format.nonBreakableLines());
  };
  // A text frame gives the whole code block one background, including padding
  // and empty lines. Character backgrounds alone leave a jagged highlight.
  QList<QPair<int, int>> codeRanges;
  for (auto block = Document.begin(); block.isValid(); block = block.next()) {
    if (!isCode(block.blockFormat()))
      continue;
    const int end = block.position() + block.length() - 1;
    if (!codeRanges.isEmpty() &&
        codeRanges.last().second + 1 == block.position())
      codeRanges.last().second = end;
    else
      codeRanges.append({block.position(), end});
  }
  for (auto it = codeRanges.crbegin(); it != codeRanges.crend(); ++it) {
    QTextCursor cursor(&Document);
    cursor.setPosition(it->first);
    cursor.setPosition(it->second, QTextCursor::KeepAnchor);
    QTextFrameFormat format;
    format.setProperty(kCodeFrameProperty, true);
    format.setBackground(code);
    format.setPadding(10);
    format.setTopMargin(8);
    format.setBottomMargin(8);
    format.setWidth(QTextLength(QTextLength::PercentageLength, 100));
    auto *frame = cursor.insertFrame(format);
    // Qt inserts empty outer paragraphs to delimit a frame. They are layout
    // separators, not blank lines supplied by the author.
    for (const auto &block : {frame->firstCursorPosition().block().previous(),
                              frame->lastCursorPosition().block().next()}) {
      if (!block.isValid() || !block.text().isEmpty())
        continue;
      QTextCursor spacer(block);
      auto spacerFormat = block.blockFormat();
      spacerFormat.setProperty(kCodeSpacerProperty, true);
      spacer.setBlockFormat(spacerFormat);
    }
  }
  bool previousCode = false;
  for (auto block = Document.begin(); block.isValid(); block = block.next()) {
    auto format = block.blockFormat();
    QTextCursor cursor(block);
    const bool fenced =
        cursor.currentFrame()->frameFormat().boolProperty(kCodeFrameProperty);
    const bool compact = format.headingLevel() > 0 || cursor.currentTable();
    format.setLineHeight(fenced    ? 130
                         : compact ? 115
                                   : 140,
                         QTextBlockFormat::ProportionalHeight);
    if (fenced) {
      format.setBackground(code);
      format.setLeftMargin(0);
      format.setRightMargin(0);
      // Long code wraps inside its bubble; no content is clipped.
      format.setNonBreakableLines(false);
      format.setTopMargin(0);
      format.setBottomMargin(0);
      if (previousCode && !CodeBlocks.isEmpty()) {
        CodeBlocks.last().End = block.position() + block.length();
        CodeBlocks.last().Text += '\n' + block.text();
      } else
        CodeBlocks.append({block.position(), block.position() + block.length(),
                           block.text()});
    } else {
      format.setTopMargin(block.blockNumber() == 0 ? 0
                          : block.textList()       ? 3
                                                   : 8);
      format.setBottomMargin(block.textList() ? 3 : 4);
      if (format.hasProperty(QTextFormat::BlockQuoteLevel)) {
        format.setLeftMargin(12);
        format.setBackground(code);
      }
    }
    previousCode = fenced;
    if (format.boolProperty(kCodeSpacerProperty)) {
      format.setLineHeight(1, QTextBlockFormat::FixedHeight);
      format.setTopMargin(0);
      format.setBottomMargin(0);
    }
    cursor.setBlockFormat(format);
    for (auto it = block.begin(); !it.atEnd(); ++it) {
      const auto fragment = it.fragment();
      if (!fragment.isValid())
        continue;
      auto character = fragment.charFormat();
      character.setForeground(character.isAnchor() ? link : text);
      if (fenced || character.fontFixedPitch()) {
        character.setFontFamilies({mono});
        character.setFontFixedPitch(true);
        character.setBackground(code);
      } else
        character.setFontFamilies(font.families());
      cursor.setPosition(fragment.position());
      cursor.setPosition(fragment.position() + fragment.length(),
                         QTextCursor::KeepAnchor);
      cursor.setCharFormat(character);
    }
  }
  const auto styleTables = [&](auto &&self, QTextFrame *frame) -> void {
    for (auto *child : frame->childFrames()) {
      if (auto *table = qobject_cast<QTextTable *>(child)) {
        auto format = table->format();
        format.setBorder(0.5);
        format.setBorderBrush(border);
        format.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
        format.setCellPadding(6);
        format.setCellSpacing(0);
        format.setWidth(QTextLength(QTextLength::PercentageLength, 100));
        table->setFormat(format);
        for (int column = 0; column < table->columns(); ++column) {
          auto cell = table->cellAt(0, column);
          auto cellFormat = cell.format();
          cellFormat.setBackground(code);
          cell.setFormat(cellFormat);
        }
      }
      self(self, child);
    }
  };
  styleTables(styleTables, Document.rootFrame());
  batch.endEditBlock();
  Document.setModified(false);
}
void MarkdownDocument::copySource() const {
  QGuiApplication::clipboard()->setText(Source);
}
bool MarkdownDocument::hasCodeAt(int position) const {
  return std::any_of(CodeBlocks.cbegin(), CodeBlocks.cend(),
                     [position](const auto &block) {
                       return position >= block.Begin && position < block.End;
                     });
}
void MarkdownDocument::copyCodeAt(int position) const {
  for (const auto &block : CodeBlocks)
    if (position >= block.Begin && position < block.End) {
      QGuiApplication::clipboard()->setText(block.Text);
      return;
    }
}
} // namespace neverd::agent
