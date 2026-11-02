//===- LinkOpener.cpp - Native URL events ---------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Receive operating-system URL events and route them to the owning workbench.
///
//===----------------------------------------------------------------------===//

#include "LinkOpener.h"

#include "DeepLink.h"
#include "LinkDispatch.h"
#include "MainWindow.h"
#include "Session.h"

#include <QCoreApplication>
#include <QFileOpenEvent>
#include <QFutureWatcher>
#include <QProcess>
#include <QtConcurrent/QtConcurrentRun>

namespace neverd::gui {

LinkOpener::LinkOpener(MainWindow &window, Session &session)
    : QObject(&window), window_(window), session_(session) {}

bool LinkOpener::eventFilter(QObject *object, QEvent *event) {
  if (event->type() != QEvent::FileOpen)
    return QObject::eventFilter(object, event);
  const QString text = static_cast<QFileOpenEvent *>(event)->url().toString();
  if (!looksLikeDeepLink(text))
    return QObject::eventFilter(object, event);
  const ParsedLink parsed = parseDeepLink(text);
  if (!parsed.link) {
    window_.reportLinkError(parsed.error);
    return true;
  }
  // The receiver may itself own the input. Probe on a worker thread so its
  // socket and navigation callbacks can still run on the GUI thread.
  auto *watcher = new QFutureWatcher<Dispatched>(this);
  connect(watcher, &QFutureWatcher<Dispatched>::finished, this,
          [this, watcher, text, link = *parsed.link] {
            const auto result = watcher->result();
            watcher->deleteLater();
            if (!result.sent)
              openRecent(text, link);
            else if (!result.opened)
              window_.reportLinkError(result.message);
          });
  watcher->setFuture(QtConcurrent::run(
      [text, link = *parsed.link] { return dispatchLink(text, link); }));
  return true;
}

void LinkOpener::openRecent(const QString &text, const DeepLink &link) {
  const auto file = recentFileFor(link);
  if (!file) {
    window_.reportLinkError(QCoreApplication::translate(
        "LinkDispatch", "No running NeverD has the link's program open."));
    return;
  }
  if (!session_.loaded() && !session_.opening()) {
    window_.openLinkAfterOpen(link);
    window_.openFile(*file);
    return;
  }
  QProcess launcher;
  launcher.setProgram(QCoreApplication::applicationFilePath());
  launcher.setArguments({*file, text});
  if (!launcher.startDetached())
    window_.reportLinkError(launcher.errorString());
}

} // namespace neverd::gui
