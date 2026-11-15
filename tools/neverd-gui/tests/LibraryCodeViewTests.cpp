#include "LibraryCodeView.h"
#include "PageBytes.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QTest>

namespace {
QJsonObject region(QString id, qint64 begin, qint64 end) {
  return {{"id", id},
          {"display_name", "std::vector<unsigned int>::size"},
          {"foldable", true},
          {"mapping_status", "mapped"},
          {"spans",
           QJsonArray{QJsonObject{{"begin_byte", begin}, {"end_byte", end}}}}};
}
} // namespace

class LibraryCodeViewTests final : public QObject {
  Q_OBJECT
private slots:
  void generatedDetailsPreserveCopyNavigationAndVisibleBehavior() {
    for (const QString &alignment : {QString("_Alignas"), QString("alignas")}) {
      const QString source =
          "/* neverd.exception: encoding=dwarf-fde, status=complete, "
          "personality=__gxx_personality_v0\n"
          " * highir.structured_regions=1, fallback_regions=0\n"
          " * itanium.call_site[0]: [0x1000, 0x1004) pad=0x1010\n"
          " */\n"
          "void f() {\n  " +
          alignment +
          "(16) struct frame_1000 {\n"
          "    uint8_t gap0[8];\n    int64_t var_m8;\n"
          "  } stack_storage;\n"
          "  try {\n    call(&stack_storage.var_m8);\n  } catch (...) {\n"
          "    cleanup();\n    throw;\n  }\n}\n";
      const QVariantList rows{
          QVariantMap{{"line", 7}, {"addresses", QVariantList{"0x1000"}}},
          QVariantMap{{"line", 12}, {"addresses", QVariantList{"0x1010"}}}};
      LibraryCodeView view;
      view.reset(source, rows, {}, 0, false, true);
      QCOMPARE(view.defaultDetailBlocks().size(), 2);
      view.setFoldedBlocks(view.defaultDetailBlocks());
      QVERIFY(!view.text().contains("gap0"));
      QVERIFY(!view.text().contains("itanium.call_site"));
      QVERIFY(view.text().contains("status=complete"));
      QVERIFY(view.text().contains("catch (...)"));
      QVERIFY(view.text().contains("cleanup();\n    throw;"));
      QCOMPARE(view.originalSelection(0, view.text().size()), source);
      QCOMPARE(view.mappings().size(), 1);
      QVERIFY(view.unfoldSourceLine(7));
      QVERIFY(view.text().contains("var_m8;"));
      QCOMPARE(view.mappings().size(), 2);
      const auto folded = view.foldedBlocks();
      view.reset(source, rows, {}, 0, false, true);
      view.setFoldedBlocks(folded);
      QVERIFY(view.text().contains("gap0"));
      QVERIFY(!view.text().contains("itanium.call_site"));
      QVERIFY(view.unfoldSourceLine(2));
      QCOMPARE(view.text(), source);
      QCOMPARE(view.mappings(), rows);
    }
  }

  void aPagesBytesStandWhereFromUtf8ReadsThem() {
    // The engine names the places of a page by bytes, and the views read
    // each as QString::fromUtf8 reads the bytes before it.  PageBytes
    // answers every place and every run of a text as that does: of ASCII, of
    // characters of two, three and four bytes, with a mark of byte order at
    // the beginning, which QString::fromUtf8 passes over, and inside, and
    // with half a pair of surrogates, which QString::toUtf8 writes as a
    // question mark.
    const QString smile = QString::fromUcs4(U"\U0001F600");
    const QStringList texts{
        QString(),
        QStringLiteral("int v7 = 0;\n"),
        QStringLiteral("café = naïve;"),
        QStringLiteral("// 中文 v7\n  v7 += v0;\n"),
        QStringLiteral("a") + smile + QStringLiteral("b") + smile,
        QString(QChar(0xfeff)) + QStringLiteral("ab"),
        QString(QChar(0xfeff)) + QStringLiteral("中") + smile,
        QStringLiteral("a") + QChar(0xfeff) + QStringLiteral("bé"),
        QStringLiteral("a") + QChar(0xd800) + QStringLiteral("b"),
        QStringLiteral("é") + QChar(0xdc00) + QStringLiteral("中"),
    };
    int nonAscii = 0;
    for (qsizetype which = 0; which < texts.size(); ++which) {
      const QString &text = texts[which];
      const QByteArray bytes = text.toUtf8();
      nonAscii += bytes.size() != text.size();
      const neverd::gui::PageBytes page(text);
      QCOMPARE(page.size(), bytes.size());
      for (qsizetype byte = 0; byte <= bytes.size(); ++byte) {
        const auto where = [&](qsizetype end) {
          return QStringLiteral("text %1, bytes %2 to %3")
              .arg(which)
              .arg(byte)
              .arg(end)
              .toUtf8();
        };
        QVERIFY2(page.charsBefore(byte) ==
                     int(QString::fromUtf8(bytes.left(byte)).size()),
                 where(byte).constData());
        for (qsizetype end = byte + 1; end <= bytes.size(); ++end)
          QVERIFY2(page.spelled(byte, end - byte) ==
                       QString::fromUtf8(bytes.mid(byte, end - byte)),
                   where(end).constData());
      }
    }
    // The text that is ASCII but for half a pair has a byte to a unit too.
    QCOMPARE(nonAscii, 7);
  }

  void aHoldProjectsTheSourceAndItsFoldsOnce() {
    // A text read again takes its source and then each kind of fold it had:
    // under a hold that is one projection, the one each step by itself ends
    // in.
    const QString source = "#include <stdint.h>\n"
                           "int f(int v0) {\n"
                           "  if (v0) {\n"
                           "    v0 = g(v0);\n"
                           "  }\n"
                           "  return h(v0);\n"
                           "}\n";
    const QVariantList rows{
        QVariantMap{{"line", 3}, {"addresses", QVariantList{"0x1004"}}},
        QVariantMap{{"line", 5}, {"addresses", QVariantList{"0x1010"}}},
        QVariantMap{{"line", 9}, {"addresses", QVariantList{"0x1020"}}},
        QVariantMap{{"addresses", QVariantList{"0x1030"}}}};
    const QJsonArray regions{
        region("call", source.indexOf("h(v0)"), source.indexOf("h(v0)") + 5),
        QJsonObject{{"id", "prelude"},
                    {"kind", "prelude"},
                    {"display_name", "1 lines of includes and declarations"},
                    {"foldable", true},
                    {"mapping_status", "mapped"},
                    {"spans", QJsonArray{QJsonObject{{"begin_byte", 0},
                                                     {"end_byte", 18}}}}}};
    const auto fold = [&](LibraryCodeView &view) {
      view.reset(source, rows, regions, 0, false, true);
      view.setFolded(true);
      view.setRegionFolded("prelude", true);
      QSet<QString> blocks;
      for (const auto &block : view.blocks())
        blocks.insert(block.id);
      view.setFoldedBlocks(blocks);
    };
    LibraryCodeView stepwise;
    int stepwiseBuilt = 0;
    connect(&stepwise, &LibraryCodeView::changed, this,
            [&] { ++stepwiseBuilt; });
    fold(stepwise);
    QVERIFY(stepwise.libraryFolded());
    QVERIFY(stepwise.isFolded("prelude"));
    QVERIFY(!stepwise.foldedBlocks().isEmpty());
    LibraryCodeView held;
    int heldBuilt = 0;
    connect(&held, &LibraryCodeView::changed, this, [&] { ++heldBuilt; });
    {
      const LibraryCodeView::Hold hold(held);
      // The blocks are the source's, whatever is projected of it.
      held.reset(source, rows, regions, 0, false, true);
      held.setFolded(true);
      held.setRegionFolded("prelude", true);
      held.setFoldedBlocks(stepwise.foldedBlocks());
      QCOMPARE(heldBuilt, 0);
    }
    QCOMPARE(heldBuilt, 1);
    QVERIFY(stepwiseBuilt > 1);
    QCOMPARE(held.text(), stepwise.text());
    QCOMPARE(held.mappings(), stepwise.mappings());
    QCOMPARE(held.foldedBlocks(), stepwise.foldedBlocks());
    QCOMPARE(held.foldedRanges(), stepwise.foldedRanges());
    QCOMPARE(held.regions(), stepwise.regions());
    for (int line = -1; line < 9; ++line)
      QCOMPARE(held.displayLine(line), stepwise.displayLine(line));
    // A change outside any hold builds at once, as before.
    held.setFolded(false);
    QCOMPARE(heldBuilt, 2);
    QVERIFY(!held.libraryFolded());
  }

  void aRowsLineIsTheOneItsMappingShowsOn() {
    // mappedLine says of one source row what mappings() says of all of them:
    // the display line it shows on, and none for a row a folded block hides
    // or that is no line of the text.
    const QString source = "void f() {\n"
                           "  if (a) {\n"
                           "    b();\n"
                           "    c();\n"
                           "  }\n"
                           "  d();\n"
                           "}\n";
    QVariantList rows;
    for (int line = 0; line < 9; ++line)
      rows.append(QVariantMap{
          {"line", line},
          {"addresses", QVariantList{QStringLiteral("0x%1").arg(line)}}});
    LibraryCodeView view;
    view.reset(source, rows, {}, 0, false, true);
    const auto agree = [&] {
      QHash<QString, int> shown;
      for (const auto &row : view.mappings())
        shown.insert(
            row.toMap().value("addresses").toList().value(0).toString(),
            row.toMap().value("line").toInt());
      for (int line = -2; line < 12; ++line) {
        const QString address = QStringLiteral("0x%1").arg(line);
        QCOMPARE(view.mappedLine(line), shown.value(address, -1));
      }
    };
    agree();
    QCOMPARE(view.mappedLine(5), 5);
    QCOMPARE(view.mappedLine(8), -1);
    QSet<QString> blocks;
    for (const auto &block : view.blocks())
      if (block.line == 1)
        blocks.insert(block.id);
    QCOMPARE(blocks.size(), 1);
    view.setFoldedBlocks(blocks);
    agree();
    // The statements the fold hides map nowhere; what follows moves up.
    QCOMPARE(view.mappedLine(2), -1);
    QCOMPARE(view.mappedLine(3), -1);
    QVERIFY(view.mappedLine(5) < 5);
    view.setFoldedBlocks({});
    agree();
    QCOMPARE(view.mappedLine(5), 5);
  }

  void warningsAndOrdinaryRecordsDoNotCollapseByDefault() {
    for (const QString &status : {QString("complete"), QString("malformed")}) {
      const QString source =
          "/* neverd.exception: status=" + status +
          ", personality=__gxx_personality_v0\n"
          " * highir.structured_regions=0, fallback_regions=1\n"
          " * cannot recover cleanup\n */\n"
          "void f() {\n  struct frame_1000 {\n    int value;\n  } "
          "user_record;\n"
          "  if (user_record.value) {\n    effect();\n  }\n}\n";
      LibraryCodeView view;
      view.reset(source, {}, {}, 0, false, true);
      QVERIFY(view.defaultDetailBlocks().isEmpty());
      view.setFoldedBlocks(view.defaultDetailBlocks());
      QCOMPARE(view.text(), source);
    }
  }

  void nestedBlocksRetainChildrenCopyAndOriginalRows() {
    const QString source = QString::fromUtf8("int f(int v7) {\n"
                                             "  // 中文 😀\n"
                                             "  if (v7) {\n"
                                             "    while (v7) {\n"
                                             "      --v7;\n"
                                             "    }\n"
                                             "  } else {\n"
                                             "    v7 = 2;\n"
                                             "  }\n"
                                             "  return v7;\n"
                                             "}\n");
    const QVariantList rows{
        QVariantMap{{"line", 4}, {"addresses", QVariantList{"0x1234"}}}};
    LibraryCodeView view;
    view.reset(source, rows, {}, 0, false, true);
    QCOMPARE(view.blocks().size(), 4);
    QCOMPARE(view.foldableCount(), 0);
    view.setRegionFolded("source-block:3", true);
    view.setRegionFolded("source-block:2", true);
    QVERIFY(view.isFolded("source-block:3"));
    QCOMPARE(view.blocks().size(), 3); // The while header is hidden by if.
    QVERIFY(view.text().contains("if (v7) { /* … */\n  } else {"));
    QVERIFY(!view.text().contains("while"));
    QVERIFY(
        view.mappings().isEmpty()); // Hidden addresses do not map the header.
    QCOMPARE(view.originalSelection(0, view.text().size()), source);
    QCOMPARE(view.sourceLineAt(view.text().indexOf("  } else")), 6);
    view.setRegionFolded("source-block:2", false);
    QVERIFY(view.text().contains("while (v7) { /* … */"));
    QVERIFY(!view.text().contains("--v7"));
    QVERIFY(view.unfoldSourceLine(4));
    QCOMPARE(view.text(), source);
    QCOMPARE(view.mappings(), rows);
    view.setFolded(true); // Library fold-all does not fold source blocks.
    QVERIFY(!view.anyFolded());
  }

  void bracesInCommentsCharactersAndRawStringsDoNotFold() {
    const QString source = "int f() {\n"
                           "  // {\n"
                           "  /* } /* nested { */ still comment */\n"
                           "  const char *a = \"\\\" } {\";\n"
                           "  const char *b = R\"tag(\" {\n}\")tag\";\n"
                           "  auto c = r##\"\" {\n}\"##;\n"
                           "  auto d = `\" {\n}`;\n"
                           "  auto e = '{';\n"
                           "  if (e) {\n"
                           "    return 1;\n"
                           "  }\n"
                           "}\n";
    LibraryCodeView view;
    view.reset(source, {}, {}, 0, false, true);
    QCOMPARE(view.blocks().size(), 2);
    view.setRegionFolded("source-block:2", true);
    QVERIFY(view.text().contains("if (e) { /* … */"));
    QVERIFY(view.text().contains("R\"tag(\" {\n}\")tag\""));
    QCOMPARE(view.originalSelection(0, view.text().size()), source);
    for (const QString &malformed :
         {QString("if (x) {\n  x();\n"), QString("if (x) {\n  /*\n}\n"),
          QString("if (x) {\n  \"unterminated\n}\n")}) {
      view.reset(malformed, {}, {}, 0, false, true);
      QVERIFY(view.blocks().isEmpty());
      QVERIFY(!view.canFold());
      QCOMPARE(view.text(), malformed);
    }
  }

  void branchFoldStateSurvivesPresentationEdits() {
    LibraryCodeView view;
    const QString source = "int f() {\n  if (v7) {\n    --v7;\n  }\n}\n";
    view.reset(source, {}, {}, 0, false, true);
    view.setRegionFolded("source-block:2", true);
    const auto structure = view.blockStructure();
    const auto folded = view.foldedBlocks();
    view.reset(QString(source).replace("v7", "counter"), {}, {}, 0, false,
               true);
    QCOMPARE(view.blockStructure(), structure);
    view.setFoldedBlocks(folded);
    QVERIFY(view.text().contains("if (counter) { /* … */"));
    QVERIFY(!view.text().contains("--counter"));
  }

  void nestedLibrarySummariesStayIndependentAndCrossingBlocksStayOpen() {
    LibraryCodeView view;
    const QString source = "int f() {\n  if (x) {\n    call();\n  }\n}\n";
    const int call = source.indexOf("call");
    view.reset(source, {}, {region("call", call, call + 6)}, 0, false, true);
    view.setFolded(true);
    view.setRegionFolded("source-block:2", true);
    view.setRegionFolded("source-block:2", false);
    QVERIFY(view.libraryFolded());
    QVERIFY(view.text().contains("std::vector<unsigned int>::size"));
    view.setRegionFolded("source-block:2", true);
    QVERIFY(view.unfoldSourceLine(2));
    QVERIFY(!view.libraryFolded());
    QCOMPARE(view.text(), source);
    const int closing = source.indexOf("  }");
    view.reset(source, {}, {region("cross", call, closing + 3)}, 0, false,
               true);
    QVERIFY(!view.hasRegion("source-block:2"));
    QCOMPARE(view.foldableCount(), 1);
  }

  void manyBlocksDoNotConsumeTheWorkerRegionBudget() {
    QString source = "int f() {\n";
    for (int i = 0; i < 1000; ++i)
      source += "  if (x) {\n    call();\n  }\n";
    source += "}\n";
    LibraryCodeView view;
    view.reset(source, {}, {}, 0, false, true);
    QCOMPARE(view.blocks().size(), 1001);
    view.setRegionFolded("source-block:1", true);
    QCOMPARE(view.blocks().size(), 1);
    QCOMPARE(view.originalSelection(0, view.text().size()), source);
    QVERIFY(view.unfoldSourceLine(2500));
    QCOMPARE(view.blocks().size(), 1001);
  }

  void visibleHeaderRowsLeaveTheirBlocksFolded() {
    const QString source = "int f() {\n"
                           "  if (x) {\n"
                           "    call();\n"
                           "  }\n"
                           "  done();\n"
                           "}\n";
    LibraryCodeView view;
    view.reset(source, {}, {}, 0, false, true);
    view.setRegionFolded("source-block:2", true);
    // Revealing the header or closing row keeps the block folded; only a
    // hidden row expands it.
    QVERIFY(!view.unfoldSourceLine(1));
    QVERIFY(!view.unfoldSourceLine(3));
    QVERIFY(view.isFolded("source-block:2"));
    // The summary is the fold, the newline ending the header's row is not.
    const int summary = int(view.text().indexOf(QStringLiteral("/* … */")));
    QCOMPARE(view.regionAt(summary), QStringLiteral("source-block:2"));
    QVERIFY(view.regionAt(int(view.text().indexOf('\n', summary))).isEmpty());
    QVERIFY(view.unfoldSourceLine(2));
    QVERIFY(!view.isFolded("source-block:2"));

    // A library operation over whole rows, newlines included, expands from
    // its last row and not from the row after it.
    const QString rows = "a();\nb();\nc();\nd();\n";
    view.reset(rows, {}, {region("op", 5, 15)});
    view.setRegionFolded("op", true);
    QVERIFY(!view.unfoldSourceLine(3));
    QVERIFY(view.unfoldSourceLine(2));
  }

  void foldUnfoldAndCopyUseOriginalUtf8() {
    LibraryCodeView view;
    const QString source =
        QString::fromUtf8("/* 中文 😀 */\nreturn end - begin;\nnext();\n");
    const int begin = source.indexOf("end");
    const int end = source.indexOf(';');
    const auto item = region("one", source.left(begin).toUtf8().size(),
                             source.left(end).toUtf8().size());
    view.reset(source, {}, {item});
    QCOMPARE(view.text(), source);
    QCOMPARE(view.foldableCount(), 1);
    view.setFolded(true);
    QVERIFY(view.anyFolded());
    QVERIFY(!view.text().contains("end - begin"));
    QCOMPARE(view.regionAt(begin), QString("one"));
    QCOMPARE(view.originalSelection(0, view.text().size()), source);
    const int summaryEnd = view.text().indexOf(';');
    QCOMPARE(view.originalSelection(begin + 3, summaryEnd - 2),
             QString("end - begin"));
    QCOMPARE(view.sourceLineAt(summaryEnd + 2), 2);
    view.copySelection(0, view.text().size());
    QCOMPARE(QGuiApplication::clipboard()->text(), source);
    view.toggleRegion("one");
    QCOMPARE(view.text(), source);
    QVERIFY(!view.anyFolded());
  }

  void preludeFoldsApartFromLibraryOperations() {
    LibraryCodeView view;
    const QString source =
        "#include <stdint.h>\n\nint f(void) {\n  return size();\n}\n";
    const int call = source.indexOf("size()");
    auto prelude = region("prelude", 0, source.indexOf("int f") - 1);
    prelude["kind"] = "prelude";
    prelude["display_name"] = "2 lines of includes and declarations";
    view.reset(source, {},
               {prelude, region("call", call, call + int(qstrlen("size()")))});
    // Only library operations count for the fold-all switch.
    QCOMPARE(view.foldableCount(), 1);
    QVERIFY(view.canFold());
    view.setRegionFolded("prelude", true);
    QVERIFY(view.anyFolded());
    QVERIFY(!view.libraryFolded());
    QVERIFY(view.text().startsWith(
        "/* 2 lines of includes and declarations … */\nint f(void) {"));
    view.setFolded(true);
    QVERIFY(view.libraryFolded());
    view.setFolded(false);
    QVERIFY(view.isFolded("prelude"));
    QVERIFY(!view.libraryFolded());
    // The summary line copies as the lines it stands for.
    const int summaryEnd = view.text().indexOf('\n');
    QCOMPARE(view.originalSelection(0, summaryEnd),
             QString("#include <stdint.h>\n"));
    QCOMPARE(view.sourceLineAt(summaryEnd + 1), 2);
    // Source line 2 shows on display line 1, under the summary.
    QVERIFY(view.hasRegion("prelude"));
    QCOMPARE(view.displayLine(2), 1);
    QCOMPARE(view.displayLine(99), -1);
    view.toggleRegion("prelude");
    QCOMPARE(view.text(), source);
    QCOMPARE(view.displayLine(2), 2);
  }

  void disjointSpansPreserveInterleavedCodeAndNavigation() {
    LibraryCodeView view;
    const QString source = "first\nkeep\nlast\ntail\n";
    auto item = region("one", 0, 6);
    item["spans"] =
        QJsonArray{QJsonObject{{"begin_byte", 0}, {"end_byte", 6}},
                   QJsonObject{{"begin_byte", 11}, {"end_byte", 16}}};
    const QVariantList rows{
        QVariantMap{{"line", 3}, {"addresses", QVariantList{"0x1234"}}}};
    view.reset(source, rows, {item});
    view.setFolded(true);
    QVERIFY(view.text().contains("keep\n"));
    QCOMPARE(view.originalSelection(0, view.text().size()), source);
    const int tail = view.text().indexOf("tail");
    QCOMPARE(view.sourceLineAt(tail), 3);
    QCOMPARE(view.mappings().first().toMap()["line"].toInt(),
             view.text().left(tail).count('\n'));
    view.setFolded(false);
    QCOMPARE(view.mappings(), rows);
    QCOMPARE(view.text(), source);
  }

  void incompleteAndAmbiguousSpansStayExpanded() {
    LibraryCodeView view;
    const QString source = "0123456789";
    for (const QJsonArray &items :
         {QJsonArray{region("outside", 2, 11)},
          QJsonArray{region("one", 1, 6), region("two", 4, 8)},
          QJsonArray{region("same", 1, 3), region("same", 6, 8)}}) {
      view.reset(source, {}, items);
      QCOMPARE(view.foldableCount(), 0);
      view.setFolded(true);
      QCOMPARE(view.text(), source);
    }
    auto unknown = region("unknown", 0, 6);
    unknown["mapping_status"] = "unknown";
    view.reset(source, {}, {unknown});
    QCOMPARE(view.foldableCount(), 0);
  }

  void byteBoundariesDoNotSplitCharacters() {
    LibraryCodeView view;
    view.reset(QString::fromUtf8("甲乙"), {}, {region("one", 1, 4)});
    QCOMPARE(view.foldableCount(), 0);
    auto fractional = region("one", 0, 3);
    fractional["spans"] =
        QJsonArray{QJsonObject{{"begin_byte", 0.5}, {"end_byte", 3}}};
    view.reset(QString::fromUtf8("甲乙"), {}, {fractional});
    QCOMPARE(view.foldableCount(), 0);
    view.reset(QString::fromUtf8("甲乙"), {}, {region("one", 100, 103)}, 100);
    QCOMPARE(view.foldableCount(), 1);
    view.setFolded(true);
    QVERIFY(view.text().endsWith(QString::fromUtf8("乙")));
  }

  void appendAndReplacementInvalidateUnavailableRegions() {
    LibraryCodeView view;
    view.reset("01234", {}, {region("one", 0, 3)});
    view.setFolded(true);
    view.reset("0123456789", {}, {region("one", 0, 3)}, 0, true);
    QVERIFY(view.anyFolded());
    view.reset("0123456789", {}, {});
    QVERIFY(!view.anyFolded());
    QCOMPARE(view.text(), QString("0123456789"));
    view.reset("replacement", {}, {region("one", 0, 3)});
    QVERIFY(!view.anyFolded());
  }
};

QTEST_MAIN(LibraryCodeViewTests)
#include "LibraryCodeViewTests.moc"
