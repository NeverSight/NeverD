#include "McpConnectionManager.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QSaveFile>
#include <QSslConfiguration>
#include <QSysInfo>
#include <cstdio>
#include <memory>

namespace {
const QStringList Cases{"mcp",     "network",          "tls",
                        "process", "network-then-mcp", "tls-then-mcp"};

template <class T> double constructorMs() {
  QElapsedTimer clock;
  clock.start();
  auto value = std::make_unique<T>();
  return clock.nsecsElapsed() / 1.0e6;
}

QJsonObject sample(const QString &name) {
  QJsonObject result{{"case", name}};
  if (name == "network")
    result["constructor_ms"] = constructorMs<QNetworkAccessManager>();
  else if (name == "tls")
    result["constructor_ms"] = constructorMs<QSslConfiguration>();
  else if (name == "process")
    result["constructor_ms"] = constructorMs<QProcess>();
  else {
    if (name == "network-then-mcp")
      result["prefill_ms"] = constructorMs<QNetworkAccessManager>();
    else if (name == "tls-then-mcp")
      result["prefill_ms"] = constructorMs<QSslConfiguration>();
    result["constructor_ms"] = constructorMs<McpConnectionManager>();
  }
  return result;
}

QString sha256(const QString &path) {
  QFile file(path);
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file))
    return {};
  return QString::fromLatin1(hash.result().toHex());
}

bool writeReport(const QString &path, const QJsonObject &report) {
  const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
  if (path.isEmpty())
    return std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout) ==
           size_t(bytes.size());
  QSaveFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() &&
         file.commit();
}
} // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QCommandLineParser parser;
  parser.setApplicationDescription(
      "Real MCP client constructor timing in fresh processes");
  parser.addHelpOption();
  parser.addOption({"case", "Run one fixed constructor/control case", "name"});
  parser.addOption(
      {"samples", "Fresh-process samples for each fixed case", "count", "3"});
  parser.addOption({"output", "Write JSON here instead of stdout", "path"});
  parser.process(app);
  if (parser.isSet("case")) {
    const auto name = parser.value("case");
    if (!Cases.contains(name)) {
      qCritical("Unknown constructor case");
      return 2;
    }
    return writeReport(parser.value("output"), sample(name)) ? 0 : 2;
  }

  bool valid = false;
  const auto count = parser.value("samples").toInt(&valid);
  if (!valid || count < 1 || count > 100) {
    qCritical("samples must be 1-100");
    return 2;
  }
  const auto executable = QCoreApplication::applicationFilePath();
  QJsonObject report{
      {"schema_version", 1},
      {"kind", "mcp-constructor-probe"},
      {"command", QJsonArray::fromStringList(QCoreApplication::arguments())},
      {"application", "QCoreApplication"},
      {"qt_version", qVersion()},
      {"os", QSysInfo::prettyProductName()},
      {"architecture", QSysInfo::currentCpuArchitecture()},
      {"executable_sha256", sha256(executable)},
      {"manager_cpp_sha256", MCP_PROBE_CPP_SHA256},
      {"manager_header_sha256", MCP_PROBE_HEADER_SHA256},
      {"samples_per_case", count},
      {"measurement", "Constructor time after QCoreApplication initialization; "
                      "excludes destruction"},
      {"caveat", "Fresh processes with warm OS caches; not GUI startup or "
                 "hardware presentation"}};
  QJsonArray samples;
  bool success = true;
  for (int iteration = 0; iteration < count && success; ++iteration) {
    for (const auto &name : Cases) {
      QProcess child;
      child.setProgram(executable);
      child.setArguments({"--case", name});
      child.start();
      QJsonObject result{{"case", name}, {"iteration", iteration}};
      if (!child.waitForStarted(5000) || !child.waitForFinished(5000)) {
        child.kill();
        child.waitForFinished(1000);
        result["error"] = "Constructor subprocess failed or timed out: " +
                          child.errorString();
        success = false;
      } else {
        QJsonParseError error;
        const auto document =
            QJsonDocument::fromJson(child.readAllStandardOutput(), &error);
        success = child.exitStatus() == QProcess::NormalExit &&
                  child.exitCode() == 0 &&
                  error.error == QJsonParseError::NoError &&
                  document.isObject() &&
                  document.object()["constructor_ms"].isDouble();
        if (success) {
          result = document.object();
          result["iteration"] = iteration;
        } else {
          result["error"] = "Invalid constructor subprocess result";
        }
      }
      result["returncode"] = child.error() == QProcess::FailedToStart
                                 ? QJsonValue()
                                 : QJsonValue(child.exitCode());
      result["diagnostics"] = QString::fromUtf8(child.readAllStandardError());
      samples.append(result);
      if (!success)
        break;
    }
  }
  report["success"] = success;
  report["samples"] = samples;
  if (!writeReport(parser.value("output"), report))
    return 2;
  return success ? 0 : 1;
}
