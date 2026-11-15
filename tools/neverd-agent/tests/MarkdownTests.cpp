//===- MarkdownTests.cpp - Parsed chat content ---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise real Qt Markdown structure, streaming completion, copy and images.
//===----------------------------------------------------------------------===//
#include "app/MarkdownDocument.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QTextBlock>
#include <QTextTable>
#include <QtTest>

using namespace neverd::agent;
class MarkdownTests : public QObject {
  Q_OBJECT
private slots:
  void markdownUsesTheExplicitUiFont() {
    MarkdownDocument document;
    QFont font = QGuiApplication::font();
    font.setPointSizeF(11.5);
    document.render("Plain text and `code`.", true, Qt::white, Qt::black,
                    Qt::cyan, Qt::gray, font);
    QCOMPARE(document.textDocument().defaultFont(), font);
    for (auto it = document.textDocument().begin().begin(); !it.atEnd(); ++it)
      if (!it.fragment().charFormat().fontFixedPitch())
        QCOMPARE(it.fragment().charFormat().fontFamilies().toStringList(),
                 font.families());
    font.setPointSizeF(13.5);
    document.render("A resized paragraph.", true, Qt::white, Qt::black,
                    Qt::cyan, Qt::gray, font);
    QCOMPARE(document.textDocument().defaultFont(), font);
  }
  void richStructureAndCopy() {
    MarkdownDocument document;
    const QString source =
        "# Result\n\n**Verified** at `0x140001000`.\n\n"
        "- first\n- second\n\n```cpp\nreturn -5;\n```\n\n"
        "| value | meaning |\n| --- | --- |\n| -5 | checksum |\n\n"
        "[report](reports/result.md)\n";
    document.render(source, true, Qt::white, Qt::black, Qt::cyan, Qt::gray);
    const auto &text = document.textDocument();
    QCOMPARE(text.begin().blockFormat().headingLevel(), 1);
    QVERIFY(text.toPlainText().contains("return -5;"));
    QVERIFY(!text.toPlainText().contains("```"));
    bool table = false, list = false, bold = false, link = false;
    int code = -1;
    for (auto *frame : text.rootFrame()->childFrames())
      table |= qobject_cast<QTextTable *>(frame) != nullptr;
    for (auto block = text.begin(); block.isValid(); block = block.next()) {
      list |= block.textList() != nullptr;
      if (block.text().contains("return -5;"))
        code = block.position();
      for (auto it = block.begin(); !it.atEnd(); ++it) {
        const auto format = it.fragment().charFormat();
        bold |= format.fontWeight() >= QFont::Bold;
        link |= format.anchorHref() == "reports/result.md";
      }
    }
    QVERIFY(table && list && bold && link);
    QVERIFY(document.hasCodeAt(code));
    document.copyCodeAt(code);
    QCOMPARE(QGuiApplication::clipboard()->text(), "return -5;");
    document.copySource();
    QCOMPARE(QGuiApplication::clipboard()->text(), source);
  }
  void incompleteFenceAndLiteralHtml() {
    MarkdownDocument document;
    document.render("```cpp\nreturn 7;", true, Qt::white, Qt::black, Qt::cyan,
                    Qt::gray);
    QVERIFY(document.textDocument().toPlainText().contains("return 7;"));
    document.render("```cpp\nreturn 7;\n```\n\n**done**", true, Qt::white,
                    Qt::black, Qt::cyan, Qt::gray);
    QVERIFY(document.textDocument().toPlainText().endsWith("done"));
    document.render(
        "<b>literal</b>\n\n![remote](https://invalid.example/image.png)", true,
        Qt::white, Qt::black, Qt::cyan, Qt::gray);
    QVERIFY(document.textDocument().toPlainText().contains("<b>literal</b>"));
    QVERIFY(document.textDocument().toPlainText().contains("[remote]"));
    QVERIFY(!document.textDocument().toPlainText().contains(
        QChar::ObjectReplacementCharacter));
  }
};
QTEST_MAIN(MarkdownTests)
#include "MarkdownTests.moc"
