//===- UnwindInfoTests.cpp - Exact-PC query presentation ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Observe real dispatcher admission, stale-context rejection, and distinct
/// source scopes without a worker process or private engine session seam.
//===----------------------------------------------------------------------===//

#include "QueryService.h"
#include "Theme.h"
#include "UnwindInfoDialog.h"
#include "UnwindInfoWords.h"

#include <QCoreApplication>
#include <QDir>
#include <QFontInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <memory>

using namespace neverd::gui;

namespace {
namespace fixture {
#define NEVERD_UNWIND_GUI_TEST_TEXT(Name, Text) constexpr char Name[] = Text;
#define NEVERD_UNWIND_GUI_TEST_VALUE(Type, Name, Value)                        \
  constexpr Type Name = Value;
#include "UnwindInfoFixtures.def"
} // namespace fixture

QString translated(const char *Text) {
  return QCoreApplication::translate(unwind_info::kContext, Text);
}

QJsonObject report(const char *Text) {
  return QJsonDocument::fromJson(Text).object();
}

struct Harness {
  QList<QJsonObject> Sent;
  QueryService Queries;
  Harness()
      : Queries([this](const QString &Operation, const QJsonObject &Payload,
                       const QString &Revision) {
          const auto ID = QString::number(Sent.size() + 1);
          Sent.append({{fixture::kWireRequestID, ID},
                       {fixture::kWireOperation, Operation},
                       {fixture::kWirePayload, Payload},
                       {fixture::kWireRevision, Revision}});
          return ID;
        }) {
    Queries.resetReadContext(fixture::kProject, fixture::kFirstRevision);
  }
  QList<QJsonObject> requests() const {
    QList<QJsonObject> Result;
    for (const auto &Request : Sent)
      if (Request.value(fixture::kWireOperation) ==
          QLatin1String(fixture::kOperation))
        Result.append(Request);
    return Result;
  }
  void reply(int Index, const QJsonValue &Payload,
             const QString &Revision = fixture::kFirstRevision) {
    const auto Request = requests().at(Index);
    Queries.receive(
        {{fixture::kWireType, fixture::kWireResponse},
         {fixture::kWireRequestID, Request.value(fixture::kWireRequestID)},
         {fixture::kWireOperation, fixture::kOperation},
         {fixture::kWireProject, fixture::kProject},
         {fixture::kWireRevision, Revision},
         {fixture::kWireStatus, fixture::kOK},
         {fixture::kWirePayload, Payload}});
  }
};

QPlainTextEdit *dwarf(UnwindInfoDialog &Dialog) {
  return Dialog.findChild<QPlainTextEdit *>(unwind_info::kDwarfObject);
}
QPlainTextEdit *compact(UnwindInfoDialog &Dialog) {
  return Dialog.findChild<QPlainTextEdit *>(unwind_info::kCompactObject);
}
QLabel *status(UnwindInfoDialog &Dialog) {
  return Dialog.findChild<QLabel *>(unwind_info::kStatusObject);
}
void refresh(UnwindInfoDialog &Dialog) {
  Dialog.findChild<QPushButton *>(unwind_info::kRefreshObject)->click();
}
} // namespace

class UnwindInfoTests final : public QObject {
  Q_OBJECT
  QTemporaryDir Settings;
private slots:
  void initTestCase() {
    QVERIFY(Settings.isValid());
    QCoreApplication::setOrganizationName(fixture::kOrganization);
    QCoreApplication::setApplicationName(fixture::kApplication);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       Settings.path());
  }

  void exactHighPCAndZeroSigningPCPreserveIndependentSources() {
    Harness H;
    UnwindInfoDialog Dialog(H.Queries, fixture::kHighPC);
    Dialog.setProviderAvailable(true, true);
    QTRY_COMPARE(H.requests().size(), 1);
    const auto Request = H.requests().first();
    QCOMPARE(Request.value(fixture::kWirePayload).toObject(),
             QJsonObject({{fixture::kAddress, fixture::kHighAddress}}));
    QCOMPARE(Request.value(fixture::kWireRevision).toString(),
             fixture::kFirstRevision);
    QVERIFY(Dialog.windowTitle().contains(fixture::kHighAddress));
    H.reply(0, report(fixture::kStates));
    QTRY_VERIFY(dwarf(Dialog)->toPlainText().contains(fixture::kUnknown));
    QVERIFY(dwarf(Dialog)->toPlainText().contains(fixture::kPlainDiagnostic));
    QVERIFY(!dwarf(Dialog)->toPlainText().contains(fixture::kUnsigned));
    QVERIFY(compact(Dialog)->toPlainText().contains(fixture::kZeroSigningPC));
    QVERIFY(dwarf(Dialog)->isReadOnly());
    QVERIFY(compact(Dialog)->isReadOnly());
    QCOMPARE(H.Queries.revision(), fixture::kFirstRevision);
    Dialog.show();
    QTRY_VERIFY(Dialog.isVisible());
    const auto Capture = qEnvironmentVariable(fixture::kCaptureEnvironment);
    if (!Capture.isEmpty()) {
      QVERIFY(QDir().mkpath(Capture));
      for (const auto Mode : {Theme::Mode::Light, Theme::Mode::Dark}) {
        Theme::instance().setMode(Mode);
        Theme::instance().apply();
        Dialog.repaint();
        const auto Name = Mode == Theme::Mode::Light ? fixture::kCaptureLight
                                                     : fixture::kCaptureDark;
        QVERIFY(Dialog.grab().save(QDir(Capture).filePath(Name)));
      }
      Dialog.setLayoutDirection(Qt::RightToLeft);
      Dialog.repaint();
      QVERIFY(Dialog.grab().save(QDir(Capture).filePath(fixture::kCaptureRTL)));
    }
    QTest::keyClick(&Dialog, Qt::Key_Escape);
    QTRY_VERIFY(!Dialog.isVisible());
  }

  void zeroQueryPCAndAbsentSourceRemainDistinct() {
    Harness H;
    UnwindInfoDialog Dialog(H.Queries, fixture::kZeroPC);
    Dialog.setProviderAvailable(true, true);
    QTRY_COMPARE(H.requests().size(), 1);
    QCOMPARE(H.requests().first().value(fixture::kWirePayload).toObject(),
             QJsonObject({{fixture::kAddress, fixture::kZeroAddress}}));
    H.reply(0, report(fixture::kZeroQuery));
    QTRY_VERIFY(dwarf(Dialog)->toPlainText().contains(fixture::kUnsigned));
    QCOMPARE(compact(Dialog)->toPlainText(),
             translated(unwind_info::kAbsentSource));
  }

  void nullLegacyAbsentAndWrongPCNeverAcquireUnsignedState() {
    Harness H;
    UnwindInfoDialog Dialog(H.Queries, fixture::kHighPC);
    Dialog.setProviderAvailable(true, true);
    QTRY_COMPARE(H.requests().size(), 1);
    H.reply(0, QJsonValue(QJsonValue::Null));
    QTRY_COMPARE(status(Dialog)->text(), translated(unwind_info::kNoFrame));
    QVERIFY(dwarf(Dialog)->toPlainText().isEmpty());
    QVERIFY(compact(Dialog)->toPlainText().isEmpty());
    refresh(Dialog);
    QTRY_COMPARE(H.requests().size(), 2);
    H.reply(1, report(fixture::kLegacy));
    QTRY_COMPARE(status(Dialog)->text(), translated(unwind_info::kLegacy));
    QVERIFY(dwarf(Dialog)->toPlainText().isEmpty());
    refresh(Dialog);
    QTRY_COMPARE(H.requests().size(), 3);
    H.reply(2, report(fixture::kAbsentDwarf));
    QTRY_COMPARE(dwarf(Dialog)->toPlainText(),
                 translated(unwind_info::kAbsentSource));
    QVERIFY(compact(Dialog)->toPlainText().contains(fixture::kUnknown));
    refresh(Dialog);
    QTRY_COMPARE(H.requests().size(), 4);
    H.reply(3, report(fixture::kWrongPC));
    QTRY_COMPARE(status(Dialog)->text(),
                 translated(unwind_info::kInvalidReport));
    QVERIFY(dwarf(Dialog)->toPlainText().isEmpty());
    QVERIFY(compact(Dialog)->toPlainText().isEmpty());
  }

  void aQueryAtTheHalfOpenRangeEndCannotDisplayKnownState() {
    Harness H;
    UnwindInfoDialog Dialog(H.Queries, fixture::kHighPC);
    Dialog.setProviderAvailable(true, true);
    QTRY_COMPARE(H.requests().size(), 1);
    H.reply(0, report(fixture::kAtRangeEnd));
    QTRY_COMPARE(status(Dialog)->text(),
                 translated(unwind_info::kInvalidReport));
    QVERIFY(dwarf(Dialog)->toPlainText().isEmpty());
    QVERIFY(compact(Dialog)->toPlainText().isEmpty());
  }

  void destroyingTheDialogRetiresItsPendingSubscription() {
    Harness H;
    auto *Dialog = new UnwindInfoDialog(H.Queries, fixture::kHighPC);
    Dialog->setProviderAvailable(true, true);
    QTRY_COMPARE(H.requests().size(), 1);
    delete Dialog;
    H.reply(0, report(fixture::kStates));
    QTRY_VERIFY(!H.Queries.hasPending());
    QCOMPARE(H.requests().size(), 1);
  }

  void unavailableStatusesRemainVisibleWithoutInventedState() {
    Harness H;
    UnwindInfoDialog Dialog(H.Queries, fixture::kHighPC);
    Dialog.setProviderAvailable(true, true);
    int Index = 0;
    for (const auto *State :
         {fixture::kUnsupported, fixture::kMalformed, fixture::kResourceLimit,
          fixture::kOutOfRange, fixture::kFutureStatus}) {
      QTRY_COMPARE(H.requests().size(), Index + 1);
      auto Frame = report(fixture::kStates);
      auto Signing = Frame.value(unwind_info::kReturnAddress).toObject();
      Signing[unwind_info::kDwarfState] =
          QJsonObject{{unwind_info::kStatus, State}};
      Frame[unwind_info::kReturnAddress] = Signing;
      H.reply(Index++, Frame);
      QTRY_COMPARE(dwarf(Dialog)->toPlainText(),
                   translated(unwind_info::kStatusLine).arg(State));
      QVERIFY(!dwarf(Dialog)->toPlainText().contains(fixture::kUnsigned));
      QVERIFY(compact(Dialog)->toPlainText().contains(fixture::kZeroSigningPC));
      refresh(Dialog);
    }
  }

  void revisionRefreshRejectsThePreviousReplyAndKeepsTheExactPC() {
    Harness H;
    UnwindInfoDialog Dialog(H.Queries, fixture::kHighPC);
    Dialog.setProviderAvailable(true, true);
    QTRY_COMPARE(H.requests().size(), 1);
    // A newer revision retires the old subscription before it can render.
    H.reply(0, report(fixture::kStates), fixture::kNextRevision);
    QTRY_COMPARE(H.requests().size(), 2);
    QVERIFY(dwarf(Dialog)->toPlainText().isEmpty());
    QCOMPARE(H.requests().last().value(fixture::kWireRevision).toString(),
             fixture::kNextRevision);
    QCOMPARE(H.requests().last().value(fixture::kWirePayload).toObject(),
             QJsonObject({{fixture::kAddress, fixture::kHighAddress}}));
    H.reply(0, report(fixture::kStates));
    H.reply(1, QJsonValue(QJsonValue::Null), fixture::kNextRevision);
    QTRY_COMPARE(status(Dialog)->text(), translated(unwind_info::kNoFrame));
    QVERIFY(compact(Dialog)->toPlainText().isEmpty());
  }

  void projectChangeAndDisconnectClearStateAndRejectLateDelivery() {
    Harness H;
    UnwindInfoDialog Dialog(H.Queries, fixture::kHighPC);
    Dialog.setProviderAvailable(true, true);
    QTRY_COMPARE(H.requests().size(), 1);
    H.reply(0, report(fixture::kStates));
    QTRY_VERIFY(!compact(Dialog)->toPlainText().isEmpty());
    refresh(Dialog);
    QTRY_COMPARE(H.requests().size(), 2);
    H.Queries.resetReadContext(fixture::kOtherProject, fixture::kFirstRevision);
    QTRY_COMPARE(status(Dialog)->text(),
                 translated(unwind_info::kProgramChanged));
    H.reply(1, report(fixture::kStates));
    QVERIFY(dwarf(Dialog)->toPlainText().isEmpty());
    QVERIFY(compact(Dialog)->toPlainText().isEmpty());
    QVERIFY(!Dialog.findChild<QPushButton *>(unwind_info::kRefreshObject)
                 ->isEnabled());
    QCOMPARE(H.requests().size(), 2);
    H.Queries.setAvailable(false);
    QVERIFY(dwarf(Dialog)->toPlainText().isEmpty());
  }

  void largeFontAndLongDiagnosticRemainReadableAndKeyboardAccessible() {
    Harness H;
    UnwindInfoDialog Dialog(H.Queries, fixture::kHighPC);
    auto LargeFont = Dialog.font();
    LargeFont.setPointSizeF(QFontInfo(LargeFont).pointSizeF() *
                            fixture::kLargeFontScale);
    Dialog.setFont(LargeFont);
    Dialog.resize(Dialog.fontMetrics().averageCharWidth() *
                      fixture::kNarrowWidthCharacters,
                  Dialog.fontMetrics().height() * fixture::kNarrowHeightLines);
    Dialog.setProviderAvailable(true, true);
    Dialog.show();
    QTRY_COMPARE(H.requests().size(), 1);
    const auto Diagnostic = QString::fromLatin1(fixture::kLongDiagnosticChunk)
                                .repeated(fixture::kLongDiagnosticRepeats);
    auto Frame = report(fixture::kStates);
    auto Signing = Frame.value(unwind_info::kReturnAddress).toObject();
    Signing[unwind_info::kDwarfState] =
        QJsonObject{{unwind_info::kStatus, fixture::kUnknown},
                    {unwind_info::kDiagnostic, Diagnostic}};
    Frame[unwind_info::kReturnAddress] = Signing;
    H.reply(0, Frame);
    QTRY_VERIFY(dwarf(Dialog)->toPlainText().contains(Diagnostic));
    QTRY_VERIFY(dwarf(Dialog)->verticalScrollBar()->maximum() > 0);
    QVERIFY(Dialog.rect().contains(dwarf(Dialog)->geometry()));
    QVERIFY(Dialog.rect().contains(compact(Dialog)->geometry()));
    const auto Capture = qEnvironmentVariable(fixture::kCaptureEnvironment);
    if (!Capture.isEmpty()) {
      QVERIFY(QDir().mkpath(Capture));
      QVERIFY(
          Dialog.grab().save(QDir(Capture).filePath(fixture::kCaptureLarge)));
    }
    dwarf(Dialog)->setFocus();
    QTRY_COMPARE(Dialog.focusWidget(), dwarf(Dialog));
    QTest::keyClick(dwarf(Dialog), Qt::Key_Tab);
    QTRY_COMPARE(Dialog.focusWidget(), compact(Dialog));
    QTest::keyClick(compact(Dialog), Qt::Key_Escape);
    QTRY_VERIFY(!Dialog.isVisible());
  }

  void destroyingTheQueryServiceBeforeTheDialogClearsTheView() {
    int Requests = 0;
    auto Queries = std::make_unique<QueryService>(
        [&](const QString &, const QJsonObject &, const QString &) {
          return QString::number(++Requests);
        });
    Queries->resetReadContext(fixture::kProject, fixture::kFirstRevision);
    auto Dialog =
        std::make_unique<UnwindInfoDialog>(*Queries, fixture::kHighPC);
    Dialog->setProviderAvailable(true, true);
    QTRY_COMPARE(Requests, 1);
    Queries.reset();
    QTRY_COMPARE(status(*Dialog)->text(),
                 translated(unwind_info::kDisconnected));
    QVERIFY(dwarf(*Dialog)->toPlainText().isEmpty());
    Dialog.reset();
  }

  void missingCapabilityDoesNotIssueAQuery() {
    Harness H;
    UnwindInfoDialog Dialog(H.Queries, fixture::kHighPC);
    Dialog.setProviderAvailable(true, false);
    QCOMPARE(status(Dialog)->text(), translated(unwind_info::kUnavailable));
    QVERIFY(H.requests().isEmpty());
    Dialog.setProviderAvailable(false, false);
    QCOMPARE(status(Dialog)->text(), translated(unwind_info::kDisconnected));
    QVERIFY(H.requests().isEmpty());
  }
};

QTEST_MAIN(UnwindInfoTests)
#include "UnwindInfoTests.moc"
