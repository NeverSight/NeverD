//===- FixtureMcp.cpp - Disposable snapshot transport fixture -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>
#include <QtEndian>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

namespace {
int nativeFixture(QFile &input, QFile &output) {
  auto send = [&](QJsonObject value) {
    const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
    QByteArray frame(4, '\0');
    qToBigEndian<quint32>(bytes.size(), frame.data());
    output.write(frame + bytes);
    output.flush();
  };
  send({{"type", "hello"},
        {"protocol_major", 1},
        {"capabilities",
         QJsonArray{"annotation_set", "history", "save", "resolve"}}});
  QString path, mode, hash;
  bool saved = false;
  while (!input.atEnd()) {
    const auto header = input.read(4);
    if (header.size() != 4)
      return 0;
    const auto size = qFromBigEndian<quint32>(header.constData());
    QByteArray body;
    while (body.size() < size) {
      const auto part = input.read(size - body.size());
      if (part.isEmpty())
        return 0;
      body += part;
    }
    const auto request = QJsonDocument::fromJson(body).object();
    const auto operation = request["operation"].toString();
    QJsonObject result{{"type", "response"},
                       {"protocol_major", 1},
                       {"request_id", request["request_id"]},
                       {"revision", "1"},
                       {"project_id", "fixture"},
                       {"status", "ok"}};
    QJsonObject payload;
    if (operation == "open") {
      auto progress = result;
      progress["status"] = "progress";
      send(progress);
      path = request["payload"].toObject()["path"].toString();
      QFile file(path);
      file.open(QIODevice::ReadOnly);
      const auto bytes = file.readAll();
      mode = QString::fromUtf8(bytes);
      hash = QString::fromLatin1(
          QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
      payload = {{"sha256", hash}};
    } else if (operation == "resolve")
      payload = {{"address", "0x1000"}, {"name", "Fixture"}, {"comment", ""}};
    else if (operation == "history") {
      if (saved) {
        result["status"] = "error";
        result["error"] = QJsonObject{{"message", "Injected receipt failure"}};
      } else
        payload = {{"source_sha256", hash},
                   {"cursor", 0},
                   {"total", 0},
                   {"available", true}};
    } else if (operation == "annotation_set") {
      QFile marker(path + ".accepted");
      marker.open(QIODevice::Append);
      marker.write("accepted\n");
      marker.close();
      if (mode == "lost_write")
        return 0;
    } else if (operation == "save") {
      saved = true;
      payload = {{"saved", true}};
    }
    result["payload"] = payload;
    send(result);
  }
  return 0;
}
} // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
#ifdef Q_OS_WIN
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  QFile input, output;
  input.open(stdin, QIODevice::ReadOnly | QIODevice::Unbuffered);
  output.open(stdout, QIODevice::WriteOnly | QIODevice::Unbuffered);
  if (!app.arguments().contains("--file"))
    return nativeFixture(input, output);
  const QString content = QString(600000, QChar(0x7ed3)) + " snapshot end";
  bool created = false;
  while (!input.atEnd()) {
    const auto request = QJsonDocument::fromJson(input.readLine()).object();
    if (!request.contains("id"))
      continue;
    const auto method = request["method"].toString();
    QJsonObject result;
    if (method == "initialize")
      result = {{"protocolVersion", "2025-06-18"}};
    else if (method == "tools/list")
      result = {{"tools",
                 QJsonArray{QJsonObject{
                     {"name", "read_cfg"},
                     {"annotations", QJsonObject{{"readOnlyHint", true}}}}}}};
    else if (method == "tools/call") {
      created = true;
      result = {{"structuredContent",
                 QJsonObject{{"resource_uri", "neverd://result/1"},
                             {"revision", "r1"},
                             {"total_characters", content.size()}}},
                {"content", QJsonArray{QJsonObject{{"type", "resource_link"},
                                                   {"uri", "neverd://result/1"},
                                                   {"name", "snapshot"}}}}};
    } else if (method == "resources/read") {
      if (!created) {
        output.write(
            QJsonDocument(
                QJsonObject{
                    {"jsonrpc", "2.0"},
                    {"id", request["id"]},
                    {"error",
                     QJsonObject{
                         {"code", -32000},
                         {"message",
                          "Snapshot does not belong to this process."}}}})
                .toJson(QJsonDocument::Compact) +
            '\n');
        output.flush();
        continue;
      }
      const QUrlQuery query(
          QUrl(request["params"].toObject()["uri"].toString()));
      const auto offset = query.queryItemValue("offset").toLongLong();
      const auto text = content.mid(offset, 32768);
      const auto next = offset + text.size();
      const QJsonObject page{{"offset", offset},
                             {"total_characters", content.size()},
                             {"revision", "r1"},
                             {"text", text},
                             {"complete", next == content.size()},
                             {"next_offset", next}};
      result = {{"contents",
                 QJsonArray{QJsonObject{
                     {"text", QString::fromUtf8(QJsonDocument(page).toJson(
                                  QJsonDocument::Compact))}}}}};
    }
    output.write(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"},
                                           {"id", request["id"]},
                                           {"result", result}})
                     .toJson(QJsonDocument::Compact) +
                 '\n');
    output.flush();
  }
  return 0;
}
