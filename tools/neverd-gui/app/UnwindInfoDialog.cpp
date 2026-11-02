//===- UnwindInfoDialog.cpp - Exact-PC unwind inspection ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve the loader query's source scopes through the read dispatcher.
//===----------------------------------------------------------------------===//

#include "UnwindInfoDialog.h"

#include "Icons.h"
#include "QueryService.h"
#include "UnwindInfoWords.h"

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

namespace neverd::gui {
namespace {
using namespace unwind_info;

QString translated(const char *Text) {
  return QCoreApplication::translate(kContext, Text);
}

QString sourceText(const QJsonValue &Value) {
  if (Value.isNull())
    return translated(kAbsentSource);
  if (!Value.isObject())
    return translated(kInvalidReport);
  const auto Source = Value.toObject();
  const auto State = Source.value(kStatus);
  if (!State.isString() || State.toString().isEmpty())
    return translated(kInvalidReport);
  if (State == QLatin1String(kKnown) && !Source.value(kKind).isString())
    return translated(kInvalidReport);
  QStringList Lines{translated(kStatusLine).arg(State.toString())};
  // Retain the engine's vocabulary, including future statuses. Presentation
  // never turns an unknown state or an absent source into unsigned state.
  const auto Append = [&](const char *Field, const char *Label) {
    const auto FieldValue = Source.value(Field);
    if (FieldValue.isString())
      Lines.append(translated(Label).arg(FieldValue.toString()));
  };
  if (State == QLatin1String(kKnown)) {
    Append(kKind, kKindLine);
    Append(kKey, kKeyLine);
    Append(kRecipe, kRecipeLine);
    Append(kSigningPC, kSigningPCLine);
  }
  Append(kDiagnostic, kDiagnosticLine);
  return Lines.join(QLatin1String(kLineSeparator));
}
} // namespace

UnwindInfoDialog::UnwindInfoDialog(QueryService &Service, Address PC,
                                   QWidget *Parent)
    : QDialog(Parent), Queries(&Service), QueryPC(PC),
      Epoch(Service.sessionEpoch()), Project(Service.projectId()),
      Revision(Service.revision()), Status(new QLabel(this)),
      Dwarf(new QPlainTextEdit(this)), Compact(new QPlainTextEdit(this)),
      Reload(new QPushButton(translated(kRefresh), this)) {
  setObjectName(kDialogObject);
  setWindowTitle(translated(kTitle).arg(hexAddress(QueryPC)));
  setWindowIcon(icon(kIcon));
  resize(fontMetrics().averageCharWidth() * kWidthCharacters,
         fontMetrics().height() * kHeightLines);
  auto *Layout = new QVBoxLayout(this);
  auto *Intro = new QLabel(translated(kIntro), this);
  Intro->setTextFormat(Qt::PlainText);
  Intro->setWordWrap(true);
  Layout->addWidget(Intro);
  Status->setObjectName(kStatusObject);
  Status->setText(translated(kDisconnected));
  Status->setTextFormat(Qt::PlainText);
  Status->setWordWrap(true);
  Status->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                  Qt::TextSelectableByKeyboard);
  Layout->addWidget(Status);
  const auto AddSource = [&](QPlainTextEdit *View, const char *Title,
                             const char *Object) {
    auto *Label = new QLabel(translated(Title), this);
    Label->setTextFormat(Qt::PlainText);
    Label->setWordWrap(true);
    Label->setBuddy(View);
    View->setObjectName(Object);
    View->setAccessibleName(translated(Title));
    View->setReadOnly(true);
    View->setTabChangesFocus(true);
    Layout->addWidget(Label);
    Layout->addWidget(View);
  };
  AddSource(Dwarf, kDwarfTitle, kDwarfObject);
  AddSource(Compact, kCompactTitle, kCompactObject);
  auto *Buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  Reload->setObjectName(kRefreshObject);
  Reload->setEnabled(false);
  Buttons->addButton(Reload, QDialogButtonBox::ActionRole);
  connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(Reload, &QPushButton::clicked, this, &UnwindInfoDialog::refresh);
  Layout->addWidget(Buttons);
  connect(&Service, &QueryService::contextChanged, this,
          &UnwindInfoDialog::contextChanged);
  connect(&Service, &QObject::destroyed, this, [this] {
    Queries = nullptr;
    clear();
    Reload->setEnabled(false);
    Status->setText(translated(kDisconnected));
  });
}

UnwindInfoDialog::~UnwindInfoDialog() {
  if (Queries)
    Queries->unsubscribeOwner(this);
}

bool UnwindInfoDialog::sameProgram() const {
  return Queries && Epoch == Queries->sessionEpoch() &&
         Project == Queries->projectId();
}

void UnwindInfoDialog::clear() {
  ++Generation;
  if (Queries)
    Queries->unsubscribeOwner(this);
  Dwarf->clear();
  Compact->clear();
}

void UnwindInfoDialog::contextChanged() {
  if (!sameProgram()) {
    clear();
    Reload->setEnabled(false);
    Status->setText(translated(kProgramChanged));
    return;
  }
  if (Revision != Queries->revision()) {
    Revision = Queries->revision();
    refresh();
  }
}

void UnwindInfoDialog::setProviderAvailable(bool IsConnected,
                                            bool IsSupported) {
  if (Connected == IsConnected && Supported == IsSupported)
    return;
  Connected = IsConnected;
  Supported = IsSupported;
  refresh();
}

void UnwindInfoDialog::refresh() {
  clear();
  Reload->setEnabled(sameProgram() && Connected && Supported);
  if (!sameProgram()) {
    Status->setText(translated(kProgramChanged));
    return;
  }
  if (!Connected) {
    Status->setText(translated(kDisconnected));
    return;
  }
  if (!Supported) {
    Status->setText(translated(kUnavailable));
    return;
  }
  Status->setText(translated(kLoading));
  const auto Current = Generation;
  Queries->subscribe(
      {kOperation,
       {{kAddress, hexAddress(QueryPC)}},
       QueryService::QuerySpec::Exact,
       Revision},
      this, [this, Current](const QJsonObject &Response) {
        if (Current != Generation || !sameProgram() ||
            Revision != Queries->revision())
          return;
        if (Response.value(kStatus) != QLatin1String(kOK)) {
          Status->setText(translated(kFailed).arg(
              Response.value(kError).toObject().value(kMessage).toString(
                  Response.value(kStatus).toString())));
          return;
        }
        present(Response.value(kPayload));
      });
}

void UnwindInfoDialog::present(const QJsonValue &Value) {
  if (Value.isNull()) {
    Status->setText(translated(kNoFrame));
    return;
  }
  if (!Value.isObject()) {
    Status->setText(translated(kInvalidReport));
    return;
  }
  const auto Frame = Value.toObject();
  if (!Frame.contains(kReturnAddress)) {
    Status->setText(translated(kLegacy));
    return;
  }
  const auto Report = Frame.value(kReturnAddress).toObject();
  const auto PC = addressValue(Report.value(kQueryPC));
  const auto Begin = addressValue(Frame.value(kBegin));
  const auto End = addressValue(Frame.value(kEnd));
  if (!PC || *PC != QueryPC || !Begin || !End || *Begin >= *End ||
      QueryPC < *Begin || QueryPC >= *End || !Report.contains(kDwarfState) ||
      !Report.contains(kCompactRecipe)) {
    Status->setText(translated(kInvalidReport));
    return;
  }
  Status->setText(
      translated(kFrameRange).arg(hexAddress(*Begin), hexAddress(*End)));
  Dwarf->setPlainText(sourceText(Report.value(kDwarfState)));
  Compact->setPlainText(sourceText(Report.value(kCompactRecipe)));
}
} // namespace neverd::gui
