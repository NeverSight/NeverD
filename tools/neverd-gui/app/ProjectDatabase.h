#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QString>
#include <QStringList>
#include <optional>

namespace neverd::gui {

/// A NeverD database (`.nddb`): one SQLite file holding the analyzed input,
/// the project's sidecar state (comments, renames, types, bookmarks, edit
/// history) and the workbench state (location, desktop).  It is the packed
/// form of a project, written when the user packs one or saves a project
/// opened from a database.  A binary's project otherwise is the JSON files
/// beside it, which its edits update and version control can share.
///
/// Every write is one SQLite transaction, so a database is never left half
/// written.  The input is stored in independently compressed chunks that are
/// compressed and expanded in parallel.  A file that is not a NeverD database
/// is never written to.
class ProjectDatabase final {
public:
  static constexpr char Extension[] = ".nddb";
  /// The project files the workbench itself keeps, by the keys a database
  /// names them (sidecarSuffixes): the bookmarks and the debug files chosen,
  /// shared with the project, and the user's own place and desktop in it.
  static constexpr char BookmarksSuffix[] = ".neverd-bookmarks.json";
  static constexpr char DebugFilesSuffix[] = ".neverd-debug.json";
  static constexpr char WorkbenchSuffix[] = ".neverd-workbench.json";
  /// Workbench state keys (Contents::state).
  static constexpr char LocationState[] = "location";
  static constexpr char BookmarksState[] = "bookmarks";
  static constexpr char DesktopState[] = "desktop";
  /// The SQLite application id in every database header: "NDDB" in ASCII,
  /// so NeverD and tools such as file(1) tell a database from other SQLite
  /// files whatever its name.
  static constexpr qint32 ApplicationId = 0x4E444442;
  /// The database of \p binary: the binary's file name plus `.nddb`.
  static QString pathFor(const QString &binary);
  /// Whether \p path is opened as a database: it has the `.nddb` suffix or
  /// its header carries the application id.
  static bool isDatabase(const QString &path);
  /// Whether NeverD keeps a project for \p path: \p path is a database, or
  /// the binary has a database or a sidecar beside it.
  static bool hasState(const QString &path);

  struct Contents {
    QString inputName, inputSha256;
    qint64 inputSize = 0;
    QString generator, savedAt;
    /// A project file by the key the database names it
    /// (sidecarSuffixes(): ".neverd-annotations.json") -> bytes.
    QHash<QString, QByteArray> sidecars;
    /// The suffixes the database packed when it was saved, stored or not; a
    /// database saved before a kind of file was packed leaves that kind
    /// alone when it unpacks.
    QStringList packedSuffixes;
    /// Workbench state key -> JSON bytes.
    QHash<QString, QByteArray> state;
  };

  /// Pack \p binary, the sidecars beside it and \p state into \p database.
  /// The input is rewritten only when its digest changed.  Returns an error
  /// message, or an empty string on success.
  static QString save(const QString &database, const QString &binary,
                      const QHash<QString, QByteArray> &state);
  /// Replace only the workbench state of an existing database.
  static QString saveState(const QString &database,
                           const QHash<QString, QByteArray> &state);
  /// Read everything but the input bytes.
  static std::optional<Contents> read(const QString &database,
                                      QString *error = nullptr);
  /// Unpack the input and its sidecars into \p directory, returning the
  /// input path.  An existing input with the recorded digest is reused, and
  /// sidecars with a pending crash-recovery journal are left for the worker.
  static QString unpack(const QString &database, const QString &directory,
                        QString *error = nullptr);
  /// Where a database without its original input is unpacked.
  static QString workingDirectory(const QString &database);
  /// Whether the database, sidecars and lock of \p binary can sit beside
  /// it: its folder is writable.
  static bool canKeepBeside(const QString &binary);
  /// A copy of \p binary in the user's data directory, beside which its
  /// database, sidecars and lock can sit; made, or refreshed when the
  /// original's size or time changed.  Empty, with \p error set, when it
  /// cannot be made.
  static QString workingCopy(const QString &binary, QString *error = nullptr);

  /// The project's files beside an input that a database packs: the
  /// worker's edit tables and history, its load options and local types,
  /// and the workbench's shared bookmarks and debug files.
  static const QStringList &sidecarSuffixes();
  /// The project folder beside \p input, `<input>.neverd/`, which holds the
  /// project's files (neverd/support/ProjectFiles.def).
  static QString projectFolder(const QString &input);
  /// Where the project of \p input keeps the file a database names \p key:
  /// one of sidecarSuffixes(), the suffix NeverD gave the file beside the
  /// input before projects had a folder.
  static QString projectFilePath(const QString &input, const QString &key);
  /// Move the files an older NeverD kept beside \p input into its folder.
  static void migrate(const QString &input);
  /// Create the project folder of \p input with its .gitignore and
  /// .gitattributes; an error message, or an empty string.
  static QString prepareFolder(const QString &input);
  /// The project file \p suffix of \p input, if it exists and parses.
  static std::optional<QJsonDocument> readProjectFile(const QString &input,
                                                      const char *suffix);
  /// Replace the project file \p suffix of \p input with \p document as
  /// version control reads it best: two-space indentation, sorted keys, a
  /// final newline.  Returns an error message, or an empty string.
  static QString writeProjectFile(const QString &input, const char *suffix,
                                  const QJsonDocument &document);
  /// The bookmarks a bookmarks file holds, {"addr","name","note"} rows as
  /// the command line's bookmarks command writes them, as the bookmarks
  /// window lists them: {"address","text","note"}.
  static QJsonArray bookmarkRows(const QJsonArray &file);
  /// The bookmarks file of \p rows: by address, one bookmark at each.
  static QJsonArray bookmarkFile(const QJsonArray &rows);
};

} // namespace neverd::gui
