#include "ProjectDatabase.h"

#include "../ResourceLimits.h"
#include "Address.h"

#include "neverd/support/ProjectDatabase.h"
#include "neverd/support/ProjectFiles.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>
#include <QtConcurrent/QtConcurrentMap>
#include <algorithm>
#include <map>
#include <vector>

namespace neverd::gui {
namespace {
using project_database::FormatVersion;
using project_database::InputBlob;
constexpr int CompressionLevel =
    limits::support::kProjectDatabaseCompressionLevel;
constexpr char DatabasesDirectory[] = "databases";
QString recoveryMarker(const QString &binary) {
  return ProjectDatabase::projectFolder(binary) +
         QStringLiteral("/local/database-recovery.json");
}
QByteArray sidecarIdentity(const ProjectDatabase::Contents &contents) {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData(contents.inputSha256.toUtf8());
  auto keys = contents.sidecars.keys();
  std::sort(keys.begin(), keys.end());
  for (const auto &key : keys) {
    const auto &bytes = contents.sidecars[key];
    hash.addData(key.toUtf8());
    hash.addData(QByteArray::number(bytes.size()) + ':');
    hash.addData(bytes);
  }
  return hash.result().toHex();
}
bool sidecarsMatch(const QString &binary,
                   const ProjectDatabase::Contents &contents) {
  for (const auto &suffix : contents.packedSuffixes) {
    if (!ProjectDatabase::sidecarSuffixes().contains(suffix))
      continue;
    QFile file(ProjectDatabase::projectFilePath(binary, suffix));
    if (contents.sidecars.contains(suffix)) {
      if (!file.open(QIODevice::ReadOnly) ||
          file.readAll() != contents.sidecars[suffix])
        return false;
    } else if (file.exists())
      return false;
  }
  return true;
}
using project_database::Schema;

/// One SQLite connection for the calling thread, closed on scope exit.
class Connection {
public:
  enum class Access { ReadOnly, ReadWrite };
  explicit Connection(const QString &path, Access access = Access::ReadWrite)
      : name_(QStringLiteral("nddb-") + QUuid::createUuid().toString()) {
    auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name_);
    database.setDatabaseName(path);
    if (access == Access::ReadOnly)
      database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    if (!database.open())
      error_ = database.lastError().text();
  }
  ~Connection() {
    {
      auto database = QSqlDatabase::database(name_, false);
      if (database.isOpen())
        database.close();
    }
    QSqlDatabase::removeDatabase(name_);
  }
  QSqlDatabase database() const { return QSqlDatabase::database(name_, false); }
  const QString &error() const { return error_; }

private:
  QString name_, error_;
};

QString lastError(const QSqlQuery &query) { return query.lastError().text(); }

bool exec(QSqlDatabase &database, const QString &statement, QString &error) {
  QSqlQuery query(database);
  if (!query.exec(statement)) {
    error = lastError(query);
    return false;
  }
  return true;
}

QString sha256(const QString &path, QString &error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    error = QCoreApplication::translate("ProjectDatabase", "Cannot read %1: %2")
                .arg(path, file.errorString());
    return {};
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&file)) {
    error = QCoreApplication::translate("ProjectDatabase", "Cannot read %1")
                .arg(path);
    return {};
  }
  return QString::fromLatin1(hash.result().toHex());
}

QString metaValue(QSqlDatabase &database, const QString &key) {
  QSqlQuery query(database);
  query.prepare(QStringLiteral("SELECT value FROM meta WHERE key = ?"));
  query.addBindValue(key);
  return query.exec() && query.next() ? query.value(0).toString() : QString();
}

/// The format of \p database, or an empty string when it is not a NeverD
/// database: its header carries another application's id, or it has no
/// format row.  A database written before the id was stamped carries none and
/// is known by its format row; its next write stamps the id.
QString databaseFormat(QSqlDatabase &database) {
  QSqlQuery query(database);
  if (!query.exec(QStringLiteral("PRAGMA application_id")) || !query.next())
    return {};
  const qint64 id = query.value(0).toLongLong();
  if (id != ProjectDatabase::ApplicationId && id != 0)
    return {};
  return metaValue(database, QStringLiteral("format"));
}

/// Stamp the application id into the header within the open transaction.
bool writeApplicationId(QSqlDatabase &database, QString &error) {
  return exec(database,
              QStringLiteral("PRAGMA application_id = %1")
                  .arg(ProjectDatabase::ApplicationId),
              error);
}

bool writeMeta(QSqlDatabase &database, const QString &key, const QString &value,
               QString &error) {
  QSqlQuery query(database);
  query.prepare(
      QStringLiteral("INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)"));
  query.addBindValue(key);
  query.addBindValue(value);
  if (!query.exec()) {
    error = lastError(query);
    return false;
  }
  return true;
}

bool writeState(QSqlDatabase &database, const QHash<QString, QByteArray> &state,
                QString &error) {
  for (auto it = state.cbegin(); it != state.cend(); ++it) {
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO state(key, value) VALUES(?, ?)"));
    query.addBindValue(it.key());
    query.addBindValue(it.value());
    if (!query.exec()) {
      error = lastError(query);
      return false;
    }
  }
  return true;
}

/// One stored piece of the input: its expanded size and compressed bytes.
struct Chunk {
  qint64 size = 0;
  QByteArray data;
};

/// The input in chunks compressed concurrently on the global thread pool.
std::optional<std::vector<Chunk>> compressInput(const QString &path,
                                                QString &error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    error = QCoreApplication::translate("ProjectDatabase", "Cannot read %1: %2")
                .arg(path, file.errorString());
    return std::nullopt;
  }
  std::vector<Chunk> chunks;
  do {
    Chunk chunk;
    chunk.data =
        file.read(::neverd::limits::support::kProjectDatabaseChunkBytes);
    chunk.size = chunk.data.size();
    chunks.push_back(std::move(chunk));
  } while (!file.atEnd());
  if (file.error() != QFileDevice::NoError) {
    error = QCoreApplication::translate("ProjectDatabase", "Cannot read %1: %2")
                .arg(path, file.errorString());
    return std::nullopt;
  }
  QtConcurrent::blockingMap(chunks, [](Chunk &chunk) {
    chunk.data = qCompress(chunk.data, CompressionLevel);
  });
  return chunks;
}
} // namespace

namespace {
/// What a database saved before it recorded its packed files packed: the
/// tables the worker's edit history kept then, its history, and how a binary
/// file is read.  Fixed: a table added since, as the type declarations, is
/// one such a database never packed, which unpacking it leaves alone.
const QStringList &legacySidecarSuffixes() {
  static const QStringList suffixes = [] {
    QStringList keys;
    for (const auto &key : project_database::legacyFileKeys())
      keys.append(QString::fromStdString(key));
    return keys;
  }();
  return suffixes;
}

QString databaseKey(const project_files::Description &file) {
  const auto key = project_database::fileKey(file);
  return QString::fromLatin1(key.data(), static_cast<qsizetype>(key.size()));
}

ProjectDatabase::Contents qtContents(const project_database::Contents &value) {
  ProjectDatabase::Contents result;
  result.inputName = QString::fromStdString(value.InputName);
  result.inputSha256 = QString::fromStdString(value.InputSHA256);
  result.inputSize = static_cast<qint64>(value.InputSize);
  result.generator = QString::fromStdString(value.Generator);
  result.savedAt = QString::fromStdString(value.SavedAt);
  for (const auto &key : value.PackedFiles)
    result.packedSuffixes.append(QString::fromStdString(key));
  for (const auto &[key, bytes] : value.Sidecars)
    result.sidecars.insert(QString::fromStdString(key),
                           QByteArray::fromStdString(bytes));
  for (const auto &[key, bytes] : value.State)
    result.state.insert(QString::fromStdString(key),
                        QByteArray::fromStdString(bytes));
  return result;
}

class InputSink final : public project_database::InputSink {
public:
  explicit InputSink(QSaveFile &file) : file_(file) {}
  void write(std::span<const uint8_t> bytes) override {
    const auto *data = reinterpret_cast<const char *>(bytes.data());
    if (file_.write(data, bytes.size()) != qsizetype(bytes.size()))
      throw project_database::Error(file_.errorString().toStdString());
    hash_.addData(QByteArrayView(data, bytes.size()));
  }
  std::string sha256() override { return hash_.result().toHex().toStdString(); }

private:
  QSaveFile &file_;
  QCryptographicHash hash_{QCryptographicHash::Sha256};
};

/// \p path as the standard library names files, without a lossy encoding.
std::filesystem::path filePath(const QString &path) {
  return std::filesystem::path(path.toStdU16String());
}
} // namespace

const QStringList &ProjectDatabase::sidecarSuffixes() {
  // What a database packs: the project's shared files and its history; a
  // save's journal, the desktop and the lock stay each user's own.
  static const QStringList suffixes = [] {
    QStringList keys = legacySidecarSuffixes();
    for (const auto &file : project_files::Files)
      if (const QString key = databaseKey(file);
          !keys.contains(key) && project_database::isPackedFile(file))
        keys.append(key);
    return keys;
  }();
  return suffixes;
}

QString ProjectDatabase::projectFilePath(const QString &input,
                                         const QString &key) {
  for (const auto &file : project_files::Files)
    if (databaseKey(file) == key)
      return QDir(projectFolder(input))
          .filePath(QString::fromLatin1(
              file.Path.data(), static_cast<qsizetype>(file.Path.size())));
  return input + key;
}

QString ProjectDatabase::projectFolder(const QString &input) {
  return input + QStringLiteral(".neverd");
}

void ProjectDatabase::migrate(const QString &input) {
  project_files::migrate(filePath(input));
}

QString ProjectDatabase::prepareFolder(const QString &input) {
  if (const auto error = project_files::prepare(filePath(input)))
    return QCoreApplication::translate("ProjectDatabase", "Cannot write %1: %2")
        .arg(projectFolder(input), QString::fromStdString(error.message()));
  return {};
}

std::optional<QJsonDocument>
ProjectDatabase::readProjectFile(const QString &input, const char *suffix) {
  QFile file(projectFilePath(input, QLatin1String(suffix)));
  if (!file.open(QIODevice::ReadOnly))
    return std::nullopt;
  QJsonParseError error;
  auto document = QJsonDocument::fromJson(file.readAll(), &error);
  if (error.error != QJsonParseError::NoError)
    return std::nullopt;
  return document;
}

QString ProjectDatabase::writeProjectFile(const QString &input,
                                          const char *suffix,
                                          const QJsonDocument &document) {
  // Qt indents by four spaces: halve each line's indentation, which is all
  // the leading space a JSON line has.
  QByteArray text;
  for (const QByteArray &line :
       document.toJson(QJsonDocument::Indented).split('\n')) {
    qsizetype indent = 0;
    while (indent < line.size() && line[indent] == ' ')
      ++indent;
    const QByteArray content = line.mid(indent);
    // An empty array or object closes on the line that opens it, as the
    // engine and the command line write it ("[]", not "[\n]").  A string
    // holds no raw line break, so the break before it is the writer's.
    if ((content.startsWith(']') && text.endsWith("[\n")) ||
        (content.startsWith('}') && text.endsWith("{\n"))) {
      text.chop(1);
      text += content + '\n';
      continue;
    }
    text += QByteArray(indent / 2, ' ') + content + '\n';
  }
  text.chop(1);
  if (const auto error = prepareFolder(input); !error.isEmpty())
    return error;
  QSaveFile file(projectFilePath(input, QLatin1String(suffix)));
  if (!file.open(QIODevice::WriteOnly) || file.write(text) != text.size() ||
      !file.commit())
    return QCoreApplication::translate("ProjectDatabase", "Cannot write %1: %2")
        .arg(file.fileName(), file.errorString());
  return {};
}

QJsonArray ProjectDatabase::bookmarkRows(const QJsonArray &file) {
  QJsonArray rows;
  for (const auto &value : file) {
    const auto row = value.toObject();
    const auto address = parseAddress(row.value("addr").toString());
    if (!address)
      continue;
    QJsonObject bookmark{{"address", hexAddress(*address)},
                         {"text", row.value("name").toString()}};
    if (const auto note = row.value("note").toString(); !note.isEmpty())
      bookmark.insert(QStringLiteral("note"), note);
    rows.append(bookmark);
  }
  return rows;
}

QJsonArray ProjectDatabase::bookmarkFile(const QJsonArray &rows) {
  // The last bookmark at an address names it.
  std::map<Address, QJsonObject> byAddress;
  for (const auto &value : rows) {
    const auto row = value.toObject();
    if (const auto address = parseAddress(row.value("address").toString()))
      byAddress[*address] = QJsonObject{{"addr", hexAddress(*address)},
                                        {"name", row.value("text").toString()},
                                        {"note", row.value("note").toString()}};
  }
  QJsonArray file;
  for (const auto &[address, row] : byAddress)
    file.append(row);
  return file;
}

QString ProjectDatabase::pathFor(const QString &binary) {
  return binary + QLatin1String(Extension);
}

bool ProjectDatabase::hasState(const QString &path) {
  if (isDatabase(path) || QFileInfo::exists(pathFor(path)) ||
      QFileInfo::exists(projectFolder(path)))
    return true;
  // A project an older NeverD kept as files beside the input.
  return std::any_of(
      sidecarSuffixes().cbegin(), sidecarSuffixes().cend(),
      [&](const QString &suffix) { return QFileInfo::exists(path + suffix); });
}

bool ProjectDatabase::isDatabase(const QString &path) {
  return project_database::isDatabase(filePath(path));
}

QString ProjectDatabase::workingDirectory(const QString &database) {
  const auto key =
      QCryptographicHash::hash(QFileInfo(database).absoluteFilePath().toUtf8(),
                               QCryptographicHash::Sha1);
  return QDir(QStandardPaths::writableLocation(
                  QStandardPaths::AppLocalDataLocation))
      .filePath(QLatin1String(DatabasesDirectory) + QLatin1Char('/') +
                QString::fromLatin1(key.toHex().left(16)));
}

bool ProjectDatabase::canKeepBeside(const QString &binary) {
  return QFileInfo(QFileInfo(binary).absolutePath()).isWritable();
}

QString ProjectDatabase::workingCopy(const QString &binary, QString *error) {
  const auto failed = [&](const QString &message) {
    if (error)
      *error = message;
    return QString();
  };
  const QFileInfo original(binary);
  const QString directory = workingDirectory(binary);
  if (!QDir().mkpath(directory))
    return failed(
        QCoreApplication::translate("ProjectDatabase", "Cannot create %1")
            .arg(directory));
  const QString copy = QDir(directory).filePath(original.fileName());
  const QFileInfo current(copy);
  const QDateTime modified =
      original.fileTime(QFileDevice::FileModificationTime);
  if (current.exists() && current.size() == original.size() &&
      current.fileTime(QFileDevice::FileModificationTime) == modified)
    return copy;
  // Copy beside the old one and replace it in one step, so a copy that
  // stops halfway is never taken for the input.
  const QString partial = copy + QStringLiteral(".copying");
  QFile::remove(partial);
  QFile written(partial);
  if (!QFile::copy(binary, partial) ||
      !written.open(QIODevice::ReadWrite | QIODevice::ExistingOnly) ||
      !written.setFileTime(modified, QFileDevice::FileModificationTime)) {
    QFile::remove(partial);
    return failed(
        QCoreApplication::translate("ProjectDatabase", "Cannot copy %1 to %2")
            .arg(QDir::toNativeSeparators(binary),
                 QDir::toNativeSeparators(directory)));
  }
  written.close();
  QFile::remove(copy);
  if (!QFile::rename(partial, copy)) {
    QFile::remove(partial);
    return failed(
        QCoreApplication::translate("ProjectDatabase", "Cannot create %1")
            .arg(copy));
  }
  return copy;
}

QString ProjectDatabase::save(const QString &database, const QString &binary,
                              const QHash<QString, QByteArray> &state) {
  if (!project_database::validInputName(
          QFileInfo(binary).fileName().toStdString()))
    return QCoreApplication::translate(
        "ProjectDatabase",
        "The input filename cannot be stored in a portable project.");
  QString error;
  const QString digest = sha256(binary, error);
  if (digest.isEmpty())
    return error;
  // A new database is assembled beside its final name and renamed into place
  // only when complete; an existing one is updated in one transaction.
  const bool fresh = !QFileInfo::exists(database);
  const QString target =
      fresh ? database + QStringLiteral(".saving") : database;
  if (fresh)
    QFile::remove(target);
  {
    Connection connection(target);
    if (!connection.error().isEmpty())
      return connection.error();
    auto db = connection.database();
    // Never add NeverD tables to a file that is not a NeverD database.
    if (!fresh) {
      const QString format = databaseFormat(db);
      if (format.isEmpty())
        return QCoreApplication::translate("ProjectDatabase",
                                           "%1 is not a NeverD database.")
            .arg(database);
      if (format.toInt() != FormatVersion)
        return QCoreApplication::translate("ProjectDatabase",
                                           "%1 uses database format %2, which "
                                           "this NeverD cannot update.")
            .arg(database, format);
    }
    for (const char *statement : Schema)
      if (!exec(db, QString::fromLatin1(statement), error))
        return error;
    const bool inputCurrent =
        metaValue(db, QStringLiteral("input_sha256")) == digest;
    std::optional<std::vector<Chunk>> chunks;
    if (!inputCurrent && !(chunks = compressInput(binary, error)))
      return error;
    if (!db.transaction())
      return db.lastError().text();
    const auto fail = [&](const QString &message) {
      db.rollback();
      return message;
    };
    if (chunks) {
      QSqlQuery remove(db);
      remove.prepare(QStringLiteral("DELETE FROM blobs WHERE name = ?"));
      remove.addBindValue(QString::fromLatin1(InputBlob));
      if (!remove.exec())
        return fail(lastError(remove));
      for (std::size_t i = 0; i < chunks->size(); ++i) {
        QSqlQuery insert(db);
        insert.prepare(QStringLiteral(
            "INSERT INTO blobs(name, chunk, size, data) VALUES(?, ?, ?, ?)"));
        insert.addBindValue(QString::fromLatin1(InputBlob));
        insert.addBindValue(qint64(i));
        insert.addBindValue((*chunks)[i].size);
        insert.addBindValue((*chunks)[i].data);
        if (!insert.exec())
          return fail(lastError(insert));
      }
    }
    // The sidecars beside the input are the project's committed state.
    QSqlQuery clear(db);
    if (!clear.exec(QStringLiteral("DELETE FROM sidecars")))
      return fail(lastError(clear));
    // A project an older NeverD kept beside the input packs from its folder.
    migrate(binary);
    for (const auto &suffix : sidecarSuffixes()) {
      QFile sidecar(projectFilePath(binary, suffix));
      if (!sidecar.exists())
        continue;
      if (!sidecar.open(QIODevice::ReadOnly))
        return fail(
            QCoreApplication::translate("ProjectDatabase", "Cannot read %1: %2")
                .arg(sidecar.fileName(), sidecar.errorString()));
      QSqlQuery insert(db);
      insert.prepare(
          QStringLiteral("INSERT INTO sidecars(suffix, data) VALUES(?, ?)"));
      insert.addBindValue(suffix);
      insert.addBindValue(sidecar.readAll());
      if (!insert.exec())
        return fail(lastError(insert));
    }
    const QFileInfo input(binary);
    if (!writeApplicationId(db, error) ||
        !writeMeta(db, QStringLiteral("format"), QString::number(FormatVersion),
                   error) ||
        !writeMeta(db, QStringLiteral("generator"),
                   QStringLiteral("NeverD ") +
                       QCoreApplication::applicationVersion(),
                   error) ||
        !writeMeta(db, QStringLiteral("input_name"), input.fileName(), error) ||
        !writeMeta(db, QStringLiteral("input_sha256"), digest, error) ||
        !writeMeta(db, QStringLiteral("input_size"),
                   QString::number(input.size()), error) ||
        !writeMeta(db, QStringLiteral("saved_at"),
                   QDateTime::currentDateTimeUtc().toString(Qt::ISODate),
                   error) ||
        !writeMeta(db, QStringLiteral("sidecar_suffixes"),
                   sidecarSuffixes().join(QLatin1Char(' ')), error) ||
        !writeState(db, state, error))
      return fail(error);
    if (!db.commit())
      return fail(db.lastError().text());
  }
  if (fresh) {
    QFile::remove(database);
    if (!QFile::rename(target, database))
      return QCoreApplication::translate("ProjectDatabase", "Cannot create %1")
          .arg(database);
  }
  QFile marker(recoveryMarker(binary));
  if (marker.open(QIODevice::ReadOnly)) {
    const auto recovery = QJsonDocument::fromJson(marker.read(4096)).object();
    marker.close();
    if (recovery.value("database").toString() ==
        QFileInfo(database).canonicalFilePath())
      return finishRecovery(binary);
  }
  return {};
}

QString ProjectDatabase::finishRecovery(const QString &binary) {
  QFile marker(recoveryMarker(binary));
  if (marker.exists() && !marker.remove())
    return QCoreApplication::translate("ProjectDatabase",
                                       "Cannot remove %1: %2")
        .arg(marker.fileName(), marker.errorString());
  return {};
}

QString ProjectDatabase::saveState(const QString &database,
                                   const QHash<QString, QByteArray> &state) {
  Connection connection(database);
  if (!connection.error().isEmpty())
    return connection.error();
  auto db = connection.database();
  QString error;
  if (databaseFormat(db).toInt() != FormatVersion)
    return QCoreApplication::translate("ProjectDatabase",
                                       "%1 is not a NeverD database.")
        .arg(database);
  if (!db.transaction())
    return db.lastError().text();
  if (!writeApplicationId(db, error) || !writeState(db, state, error)) {
    db.rollback();
    return error;
  }
  return db.commit() ? QString() : db.lastError().text();
}

std::optional<ProjectDatabase::Contents>
ProjectDatabase::read(const QString &database, QString *error,
                      ReadScope scope) {
  const auto failed = [&](const QString &message) {
    if (error)
      *error = message;
    return std::nullopt;
  };
  try {
    project_database::Reader reader(filePath(database));
    if (scope == ReadScope::Project)
      reader.readProject();
    return qtContents(reader.contents());
  } catch (const project_database::Error &failure) {
    return failed(QString::fromUtf8(failure.what()));
  }
}

QString ProjectDatabase::unpack(const QString &database,
                                const QString &directory, QString *error,
                                bool *recoveredEdits) {
  if (recoveredEdits)
    *recoveredEdits = false;
  const auto failed = [&](const QString &message) {
    if (error)
      *error = message;
    return QString();
  };
  std::unique_ptr<project_database::Reader> reader;
  std::optional<Contents> contents;
  try {
    reader = std::make_unique<project_database::Reader>(filePath(database));
    reader->readProject();
    contents = qtContents(reader->contents());
  } catch (const project_database::Error &failure) {
    return failed(QString::fromUtf8(failure.what()));
  }
  if (!QDir().mkpath(directory))
    return failed(
        QCoreApplication::translate("ProjectDatabase", "Cannot create %1")
            .arg(directory));
  const QString input = QDir(directory).filePath(contents->inputName);
  bool preserveRecovery = false;
  QFile marker(recoveryMarker(input));
  if (marker.exists()) {
    if (!marker.open(QIODevice::ReadOnly))
      return failed(marker.errorString());
    const auto saved = QJsonDocument::fromJson(marker.read(4096)).object();
    marker.close();
    preserveRecovery = saved.value("database").toString() ==
                           QFileInfo(database).canonicalFilePath() &&
                       saved.value("sidecars").toString().toLatin1() ==
                           sidecarIdentity(*contents);
    if (!preserveRecovery) {
      // A crash after a successful pack may leave only marker cleanup.
      // Other conflicts must not replace the only recovered copy.
      if (!sidecarsMatch(input, *contents))
        return failed(QCoreApplication::translate(
            "ProjectDatabase",
            "The database changed while recovered edits are pending. Preserve "
            "the working copy before reopening."));
      const auto cleanup = finishRecovery(input);
      if (!cleanup.isEmpty())
        return failed(cleanup);
    }
  }
  QString digestError;
  if (!QFileInfo::exists(input) ||
      sha256(input, digestError) != contents->inputSha256) {
    QSaveFile file(input);
    if (!file.open(QIODevice::WriteOnly))
      return failed(file.errorString());
    try {
      InputSink sink(file);
      reader->unpackInput(sink);
    } catch (const project_database::Error &failure) {
      file.cancelWriting();
      return failed(QString::fromUtf8(failure.what()));
    }
    if (!file.commit())
      return failed(file.errorString());
  }

  reader.reset();
  // A pending journal means the worker will recover newer edits itself.
  migrate(input);
  if (QFileInfo::exists(
          projectFilePath(input, QStringLiteral(".neverd-journal.json")))) {
    // The worker removes its journal after recovery. Preserve the database
    // relationship independently until the recovered content is packed.
    if (!QDir().mkpath(QFileInfo(recoveryMarker(input)).absolutePath()))
      return failed(QCoreApplication::translate(
          "ProjectDatabase", "Cannot create the recovery marker directory."));
    QSaveFile pending(recoveryMarker(input));
    const auto bytes =
        QJsonDocument(
            QJsonObject{
                {"database", QFileInfo(database).canonicalFilePath()},
                {"sidecars", QString::fromLatin1(sidecarIdentity(*contents))}})
            .toJson();
    if (!pending.open(QIODevice::WriteOnly) ||
        pending.write(bytes) != bytes.size() || !pending.commit())
      return failed(pending.errorString());
    preserveRecovery = true;
  }
  if (recoveredEdits)
    *recoveredEdits = preserveRecovery;
  if (!preserveRecovery)
    for (const auto &suffix : sidecarSuffixes()) {
      const QString path = projectFilePath(input, suffix);
      const auto stored = contents->sidecars.constFind(suffix);
      if (stored == contents->sidecars.cend()) {
        // A file kind the database never packed may hold the only copy.
        if (contents->packedSuffixes.contains(suffix))
          QFile::remove(path);
        continue;
      }
      if (const auto error = prepareFolder(input); !error.isEmpty())
        return failed(error);
      QFile sidecar(path + QStringLiteral(".unpacking"));
      if (!sidecar.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
          sidecar.write(*stored) != stored->size()) {
        sidecar.remove();
        return failed(
            QCoreApplication::translate("ProjectDatabase", "Cannot write %1")
                .arg(path));
      }
      sidecar.close();
      QFile::remove(path);
      if (!QFile::rename(sidecar.fileName(), path))
        return failed(
            QCoreApplication::translate("ProjectDatabase", "Cannot write %1")
                .arg(path));
    }
  return input;
}

} // namespace neverd::gui
