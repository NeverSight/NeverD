#include "EngineClient.h"

#include <QJsonDocument>
#include <QtEndian>

namespace {
constexpr qsizetype MaxFrame = 8 * 1024 * 1024;
constexpr qint64 MaxWriteQueue = 16 * 1024 * 1024;
} // namespace

void WorkerTransport::start(const QString &program, quint64 epoch) {
  stop();
  epoch_ = epoch;
  process_ = new QProcess(this);
  process_->setProcessChannelMode(QProcess::SeparateChannels);
  connect(process_, &QProcess::readyReadStandardOutput, this,
          &WorkerTransport::receive);
  connect(process_, &QProcess::readyReadStandardError, this, [this] {
    const auto bytes = process_->readAllStandardError();
    emit diagnostic(QString::fromUtf8(bytes.right(32768)), epoch_);
  });
  connect(process_, &QProcess::errorOccurred, this,
          [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
              emit failure(process_->errorString(), epoch_);
              emit stopped(epoch_);
            }
          });
  connect(
      process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
      [this](int code, QProcess::ExitStatus status) {
        input_.clear();
        if (status == QProcess::CrashExit || code != 0)
          emit failure(
              tr("Analysis worker exited (%1). Restart to continue.").arg(code),
              epoch_);
        emit stopped(epoch_);
      });
  process_->start(program, QStringList{});
}

void WorkerTransport::send(const QJsonObject &request) {
  if (!process_ || process_->state() != QProcess::Running) {
    emit message(
        {{"type", "response"},
         {"request_id", request["request_id"]},
         {"status", "error"},
         {"error",
          QJsonObject{{"code", "transport_unavailable"},
                      {"message", tr("Analysis worker is not running.")}}}},
        epoch_);
    return;
  }
  const QByteArray data = QJsonDocument(request).toJson(QJsonDocument::Compact);
  if (data.size() > MaxFrame ||
      process_->bytesToWrite() + data.size() > MaxWriteQueue) {
    emit message(
        {{"type", "response"},
         {"request_id", request["request_id"]},
         {"status", "error"},
         {"error",
          QJsonObject{{"code", "queue_full"},
                      {"message", tr("Analysis request queue is full.")}}}},
        epoch_);
    return;
  }
  QByteArray frame(4, '\0');
  qToBigEndian<quint32>(static_cast<quint32>(data.size()), frame.data());
  frame += data;
  process_->write(frame);
}

void WorkerTransport::receive() {
  // Consume in bounded pieces; a producer cannot force an unbounded readAll.
  while (process_->bytesAvailable() > 0) {
    input_ += process_->read(qMin<qint64>(65536, process_->bytesAvailable()));
    while (input_.size() >= 4) {
      const auto length = qFromBigEndian<quint32>(input_.constData());
      if (length == 0 || length > MaxFrame) {
        emit failure(tr("Invalid analysis protocol frame."), epoch_);
        emit stopped(epoch_);
        stop();
        return;
      }
      if (input_.size() < qsizetype(length) + 4)
        break;
      QJsonParseError error;
      const auto document =
          QJsonDocument::fromJson(input_.mid(4, length), &error);
      input_.remove(0, length + 4);
      if (error.error != QJsonParseError::NoError || !document.isObject()) {
        emit failure(tr("Invalid analysis protocol JSON."), epoch_);
        emit stopped(epoch_);
        stop();
        return;
      }
      emit message(document.object(), epoch_);
    }
  }
}

void WorkerTransport::stop() {
  if (process_) {
    process_->disconnect(this);
    if (process_->state() != QProcess::NotRunning) {
      process_->kill();
      process_->waitForFinished(2000); // Off the UI thread.
    }
    delete process_;
    process_ = nullptr;
  }
  input_.clear();
}

EngineClient::EngineClient(QObject *parent)
    : QObject(parent), transport_(new WorkerTransport) {
  transport_->moveToThread(&thread_);
  connect(&thread_, &QThread::finished, transport_, &QObject::deleteLater);
  connect(this, &EngineClient::startRequested, transport_,
          &WorkerTransport::start);
  connect(this, &EngineClient::sendRequested, transport_,
          &WorkerTransport::send);
  connect(this, &EngineClient::stopRequested, transport_,
          &WorkerTransport::stop);
  connect(transport_, &WorkerTransport::message, this,
          [this](const QJsonObject &message, quint64 epoch) {
            if (epoch == epoch_)
              emit this->message(message);
          });
  connect(transport_, &WorkerTransport::diagnostic, this,
          [this](const QString &message, quint64 epoch) {
            if (epoch == epoch_)
              emit diagnostic(message);
          });
  connect(transport_, &WorkerTransport::failure, this,
          [this](const QString &message, quint64 epoch) {
            if (epoch == epoch_)
              emit failure(message);
          });
  connect(transport_, &WorkerTransport::stopped, this, [this](quint64 epoch) {
    if (epoch == epoch_)
      emit stopped();
  });
  thread_.start();
}

EngineClient::~EngineClient() {
  QMetaObject::invokeMethod(transport_, &WorkerTransport::stop,
                            Qt::BlockingQueuedConnection);
  thread_.quit();
  thread_.wait();
}

void EngineClient::start(const QString &program) {
  emit startRequested(program, ++epoch_);
}
void EngineClient::stop() {
  ++epoch_;
  emit stopRequested();
  emit stopped();
}

QString EngineClient::request(const QString &operation,
                              const QJsonObject &payload,
                              const QString &revision) {
  const auto id = QString::number(++nextRequest_);
  QJsonObject request{{"protocol_major", 1},
                      {"request_id", id},
                      {"operation", operation},
                      {"payload", payload}};
  if (!revision.isEmpty())
    request["expected_revision"] = revision;
  emit sendRequested(request);
  return id;
}
