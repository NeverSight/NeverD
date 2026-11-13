//===- EvidenceCapture.cpp - Materialize ND snapshot pages ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "EvidenceCapture.h"

#include "models/ModelProfile.h"
#include "project/EvidenceStore.h"

#include <QJsonDocument>
#include <QUrlQuery>

namespace neverd::agent {
EvidenceCapture::EvidenceCapture(McpSession *engine)
    : QObject(engine), Engine(engine) {}
EvidenceCapture *
EvidenceCapture::captureAndSave(McpSession *engine, Project project,
                                AnalysisObject object, QString runId,
                                QString tool, QJsonObject arguments,
                                QJsonObject result, McpSession::Done done) {
  auto evidence = EvidenceStore::descriptor(project, object, runId, tool,
                                            arguments, result);
  evidence["analysisSession"] = engine->identity();
  auto *capture = new EvidenceCapture(engine);
  connect(
      capture, &EvidenceCapture::finished, capture,
      [project, result, evidence, done = std::move(done)](
          const QString &text, bool complete, const QString &issue) mutable {
        evidence["complete"] = complete;
        evidence["issue"] = issue;
        QString error;
        if (!EvidenceStore::save(project, evidence, text, error)) {
          done({{"error", "Cannot save ND evidence: " + error}});
          return;
        }
        result["evidence_id"] = evidence["id"];
        result["evidence_url"] = "nd-evidence://" + evidence["id"].toString();
        result["evidence_complete"] = complete;
        if (!complete)
          result["evidence_issue"] = issue;
        done(result);
      });
  QTimer::singleShot(0, capture, [capture, result] { capture->start(result); });
  return capture;
}
void EvidenceCapture::start(const QJsonObject &result) {
  const auto metadata = EvidenceStore::resultMetadata(result);
  URI = metadata["resource_uri"].toString();
  Revision = metadata["revision"].toString();
  if (URI.isEmpty()) {
    Text = EvidenceStore::resultText(result);
    finish(true);
    return;
  }
  Total = metadata["total_characters"].toInteger(-1);
  if (!URI.startsWith("neverd://result/") || Total < 0 ||
      Total > model_limits::kMaxResponseBytes) {
    finish(false, "Unsupported ND snapshot identity or size.");
    return;
  }
  next();
}
void EvidenceCapture::next() {
  if (Cancelled || !Engine) {
    finish(
        false,
        "Evidence capture was interrupted; only captured pages are available.");
    return;
  }
  QUrl uri(URI);
  QUrlQuery query;
  query.addQueryItem("offset", QString::number(Offset));
  if (!Revision.isEmpty())
    query.addQueryItem("expected_revision", Revision);
  uri.setQuery(query);
  Request =
      Engine->readResource(uri.toString(), [this](const QJsonObject &result) {
        Request = 0;
        if (result.contains("error")) {
          finish(false, result["error"].toString());
          return;
        }
        const auto page =
            QJsonDocument::fromJson(EvidenceStore::resultText(result).toUtf8())
                .object();
        if (page["offset"].toInteger(-1) != Offset ||
            !page["text"].isString() ||
            page["total_characters"].toInteger(-1) != Total ||
            (!Revision.isEmpty() && page["revision"] != Revision) ||
            !page["complete"].isBool()) {
          finish(false, "ND returned an inconsistent evidence snapshot page.");
          return;
        }
        Text += page["text"].toString();
        if (page["complete"].toBool()) {
          finish(true);
          return;
        }
        const auto nextOffset = page["next_offset"].toInteger(-1);
        if (nextOffset <= Offset || nextOffset > Total) {
          finish(false, "ND snapshot pagination did not advance.");
          return;
        }
        Offset = nextOffset;
        QTimer::singleShot(0, this, &EvidenceCapture::next);
      });
}
void EvidenceCapture::cancel() {
  Cancelled = true;
  if (Engine && Request)
    Engine->cancel(Request);
  else
    finish(false, "Evidence capture cancelled.");
}
void EvidenceCapture::finish(bool complete, const QString &issue) {
  if (Finished)
    return;
  Finished = true;
  emit finished(Text, complete, issue);
  deleteLater();
}
} // namespace neverd::agent
