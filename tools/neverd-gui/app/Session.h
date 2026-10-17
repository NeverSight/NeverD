#pragma once

#include "Address.h"
#include "AnalysisService.h"
#include "EngineClient.h"
#include "QueryService.h"

#include <QFileSystemWatcher>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <functional>
#include <optional>

namespace neverd::gui {

/// How to load a binary, as the load dialog chooses it.
struct LoadOptions {
  /// Read the debug information that belongs to the input.
  bool debugInfo = true;
  /// Analyze in idle time: function discovery and the reference index.
  bool analysis = true;
  /// The loader the user chose for a file that names no format itself:
  /// "evm" for EVM bytecode, or "binary" as the fields below place it; empty
  /// reads the file as its header or contents say.
  QString loader;
  /// A binary file: the processor its code is read as ("x86_64", "thumb",
  /// ...), the address its bytes map at, the bytes from offset on (size 0
  /// for the rest of the file), and where execution starts (the base when
  /// unset).
  QString processor;
  quint64 base = 0, offset = 0, size = 0;
  std::optional<quint64> entry;
  /// The platform whose conventions a binary file's code follows: "sysv",
  /// "windows" or "darwin"; empty reads it from the code.
  QString platform;
  /// A manually selected PDB, with its import mode and file stamp, or
  /// {"mode":"none"} for a PE the user opened without the PDB it names.
  QJsonObject pdb;
  /// {"mode":"none"} when the user declined the DWARF of an ELF or Mach-O
  /// image, or a DWARF file chosen for it with its file stamp; empty reads
  /// the one the image has.
  QJsonObject dwarf;
  /// {"mode":"none"} when the user declined the linker MAP beside the input,
  /// or a MAP file chosen for it with its mode ("checked", or "unchecked"
  /// for one of another build) and file stamp; empty reads the one beside
  /// it when no PDB or DWARF describes the image.
  QJsonObject map;
  /// The MAP beside the input the user agreed to load, which the open reads
  /// only when it holds against the image as loaded; empty otherwise.  The
  /// project does not keep it.
  QString acceptedMap;
  /// {"mode":"none"} when the user declined the image's static symbol table,
  /// which loads it as if stripped; empty reads it.
  QJsonObject symbols;
};

/// The project's writable worker and a bounded read-only analysis pool.
/// Browsing and edits keep the owner dispatcher while expensive views use a
/// verified replica. Save/Discard/Cancel transitions belong to the owner.
class Session final : public QObject {
  Q_OBJECT
public:
  explicit Session(QString workerPath, QObject *parent = nullptr);
  ~Session() override;

  QueryService &queries() { return queries_; }
  QueryService &analysisQueries(const QJsonObject &payload, QObject *owner) {
    return analysis_.queries(payload, owner);
  }
  void cancelAnalysisReads(QObject *owner) {
    analysis_.unsubscribeOwner(owner);
  }

  bool connected() const { return connected_; }
  /// The configured NeverD worker, also used by separate offline workbenches.
  QString workerPath() const { return workerPath_; }
  bool loaded() const { return loaded_; }
  bool opening() const { return opening_; }
  /// Whether quitting or opening another file must ask first: edits are
  /// staged or in flight, or a database the user opened lacks its edits.
  bool dirty() const {
    return dirty_ || pendingWrites_ > 0 ||
           (openedFromDatabase_ && databaseDirty_);
  }
  /// Whether the user opened a NeverD database, which saving writes back.
  /// A binary's project is the files beside it, which its edits update.
  bool openedFromDatabase() const { return openedFromDatabase_; }
  /// Whether the database the user opened lacks edits made since.
  bool databaseOutOfDate() const {
    return openedFromDatabase_ && databaseDirty_;
  }
  bool readOnly() const { return metadata_.value("read_only").toBool(); }
  /// The input the worker analyzes (a database's unpacked copy).
  QString filePath() const { return filePath_; }
  /// What the user opened: a binary or a NeverD database.
  QString projectPath() const { return projectPath_; }
  /// The database this project saves to (it may not exist yet).
  QString databasePath() const { return databasePath_; }
  /// Workbench state recorded in the database when the project opened.
  const QHash<QString, QByteArray> &databaseState() const {
    return databaseState_;
  }
  /// Supplies the workbench state that saving packs into the database.
  void setStateProvider(std::function<QHash<QString, QByteArray>()> provider) {
    stateProvider_ = std::move(provider);
  }
  QString fileName() const;
  const QJsonObject &metadata() const { return metadata_; }
  QString architecture() const {
    return metadata_.value("architecture").toString();
  }
  QString format() const { return metadata_.value("format").toString(); }
  /// The platform whose conventions a binary file's code follows, as the
  /// user reads it; empty for a file that names its own format.
  QString platformName() const;
  int bitness() const { return metadata_.value("bitness").toInt(64); }
  Address entryAddress() const;
  QString revision() const { return queries_.revision(); }
  QString lastError() const { return error_; }

  /// Background reference-index state from the latest heartbeat.
  const QJsonObject &background() const { return background_; }
  bool indexReady() const;
  /// Changes whenever listing text can change without a revision change.
  QString generation() const { return generation_; }
  /// Requests are queued or the background index is still building.
  bool busy() const;

  const QJsonObject &history() const { return history_; }
  bool canUndo() const { return history_.value("can_undo").toBool(); }
  bool canRedo() const { return history_.value("can_redo").toBool(); }

  // Session transitions.
  void open(const QString &path, LoadOptions options = {});
  /// Whether the worker lists the ways a file can be loaded (identify).
  bool identifiesFiles() const {
    return capabilities_.contains(QStringLiteral("identify"));
  }
  bool loadsPdbFiles() const {
    return capabilities_.contains(QStringLiteral("pdb_load"));
  }
  /// Whether the worker lists the program's local types (local_types,
  /// local_type, local_types_source).
  bool listsLocalTypes() const {
    return capabilities_.contains(QStringLiteral("local_types"));
  }
  /// Whether the worker adds and removes local types (local_types_declare,
  /// local_types_forget).
  bool declaresLocalTypes() const {
    return capabilities_.contains(QStringLiteral("local_types_declare"));
  }
  /// Whether the worker finds the PDB a PE names before opening it
  /// (pdb_reference), and opens a PE without it.
  bool findsPdbFiles() const {
    return capabilities_.contains(QStringLiteral("pdb_reference"));
  }
  /// Whether the worker finds the DWARF of an ELF or Mach-O image before
  /// opening it (dwarf_reference), and opens the image without it.
  bool findsDwarf() const {
    return capabilities_.contains(QStringLiteral("dwarf_reference"));
  }
  /// Whether the worker finds an image's static symbol table before opening
  /// it (symbol_table_reference), and opens the image without it.
  bool findsSymbolTables() const {
    return capabilities_.contains(QStringLiteral("symbol_table_reference"));
  }
  /// Whether the worker finds the linker MAP beside an input before opening
  /// it (map_reference), and opens the image without it.
  bool findsMapFiles() const {
    return capabilities_.contains(QStringLiteral("map_reference"));
  }
  /// Whether the worker loads a MAP file into the open image (map_inspect,
  /// map_load).
  bool loadsMapFiles() const {
    return capabilities_.contains(QStringLiteral("map_load"));
  }
  /// Whether the worker loads a DWARF file into the open image (dwarf_load).
  bool loadsDwarfFiles() const {
    return capabilities_.contains(QStringLiteral("dwarf_load"));
  }
  void loadPdb(const QJsonObject &selection);
  /// Load the MAP file \p selection names ({path,mode,stamp}) into the open
  /// image, after its PDB or DWARF, whose names outrank the MAP's.
  void loadMap(const QJsonObject &selection);
  /// Load the DWARF file \p selection names ({path}) into the open image.
  void loadDwarf(const QJsonObject &selection);
  void closeFile();
  /// Read the input file again through a fresh worker, as IDA's "Reload the
  /// input file": its bytes as they are now, with the saved names, comments
  /// and other annotations.  \p options reads a binary file another way.
  void reload(std::optional<LoadOptions> options = std::nullopt);
  void restart();
  /// How the engine loaded the open file (its load_options): the loader, and
  /// for a binary file the processor, base, offset, size, entry and platform.
  QJsonObject loadOptionsJson() const {
    return metadata_.value(QStringLiteral("load_options")).toObject();
  }
  /// Returns true when quitting may proceed immediately; otherwise a
  /// transition was requested and quitApproved() follows a decision.
  bool requestQuit();
  /// "save", "discard" or "cancel" for the pending transition.
  void resolveTransition(const QString &choice);

  /// Changes whenever the worker session changes (open, restart, close).
  /// Commands captured under an older epoch are refused.
  quint64 epoch() const { return queries_.sessionEpoch(); }

  // Edits.  \p epoch is the session the user aimed at, when an edit was
  // prepared across a dialog or other asynchronous step.
  /// Commit staged edits beside the binary, and write a database the user
  /// opened back.
  void save();
  /// Commit staged edits and pack the project, its binary included, into
  /// the database \p path: one file to keep or send.
  void packDatabase(const QString &path);
  /// Remember where the user is: in the database the user opened, or in a
  /// file of the user's own beside a binary.
  void saveDatabaseState();
  /// Keep \p rows as the project's bookmarks, shared with it beside the
  /// binary.
  void saveBookmarks(const QJsonArray &rows);
  /// Take the project's files as they are on disk now, after git, another
  /// tool or a person changed them.  It runs by itself when they change.
  void syncProject();
  /// Whether the worker's engine keeps function edits.
  bool keepsFunctionEdits() const {
    return capabilities_.contains(QStringLiteral("function_create"));
  }
  /// Whether the worker's engine keeps how the user shows operands' numbers.
  bool keepsOperandFormats() const {
    return capabilities_.contains(QStringLiteral("operand_format"));
  }
  /// Show a number operand of the instruction at \p address as \p action
  /// says: a base of OperandFormats.def, "negate" or "invert" to toggle the
  /// sign or the bits.  The worker formats \p operand if it is a number, and
  /// else the instruction's last number.  Commits at once, like a rename.
  void formatOperand(Address address, std::optional<int> operand,
                     const QString &action, std::optional<quint64> epoch = {});
  /// Whether the worker's engine shows structure offsets of local types
  /// and ranks the types by the code's accesses (local_types_fit).
  bool showsStructOffsets() const {
    return capabilities_.contains(QStringLiteral("operand_struct"));
  }
  /// Whether the worker's engine gives a function the user's C
  /// declaration, as IDA's Y does (function_type, function_edit's "type").
  bool typesFunctions() const {
    return capabilities_.contains(QStringLiteral("function_type"));
  }
  /// Whether it also ranks the local types a parameter points at by the
  /// code's accesses, as Convert to struct * offers them.
  bool convertsParameters() const {
    return typesFunctions() &&
           capabilities_.contains(QStringLiteral("local_types_fit"));
  }
  /// Show the operand of the instruction at \p address as a member of the
  /// local type \p type, the register \p delta bytes into it, and the
  /// same in each instruction of \p others: one step of the history for up
  /// to 64 instructions.  Commits at once, like a rename.
  void formatOperandStruct(Address address, std::optional<int> operand,
                           const QString &type, qint64 delta,
                           const QStringList &others,
                           std::optional<quint64> epoch = {});
  /// Whether the worker's engine keeps the user's data items.
  bool keepsDataItems() const {
    return capabilities_.contains(QStringLiteral("item_define"));
  }
  /// Define the item at \p address: \p action "data" makes a value, the
  /// next size each time; "string" the string that starts there; "undefine"
  /// shows the item's bytes as bytes.  Each commits at once, like a rename.
  void defineItem(Address address, const QString &action,
                  std::optional<quint64> epoch = {});
  /// Start a function at \p address, or stop treating the function at
  /// \p entry as one.  Each commits at once, like a rename.
  void createFunction(Address address, std::optional<quint64> epoch = {});
  void deleteFunction(Address entry, std::optional<quint64> epoch = {});
  /// Whether the worker's engine keeps what the user states of a function.
  bool statesFunctionAttributes() const {
    return capabilities_.contains(QStringLiteral("function_edit"));
  }
  /// State what the function at \p entry is, as IDA's Edit function does:
  /// each member of \p attributes one of FunctionAttributes.def's, null to
  /// hand it back to analysis.  Commits at once, like a rename.
  void editFunction(Address entry, const QJsonObject &attributes,
                    std::optional<quint64> epoch = {});
  /// Name \p function \p name; \p then runs once the rename is made, never
  /// when the worker refuses the name.
  void rename(Address function, const QString &name,
              std::optional<quint64> epoch = {},
              std::function<void()> then = {});
  void editCode(const QJsonObject &edit, quint64 epoch);
  void setComment(Address address, const QString &text,
                  std::optional<quint64> epoch = {});
  void undo();
  void redo();
  void loadSignatures(const QString &path, bool tree);
  void analyzeWholeProgram();
  /// Search strings in \p encodings (engine names), C strings that are not
  /// UTF-8 in the code page \p preferred first (none when empty), and of at
  /// least \p minLength display columns.  Remembered and applied to every
  /// worker that starts.
  void setStringOptions(const QStringList &encodings, const QString &preferred,
                        int minLength);
  void cancelReads();

  // Declarative extension manifests.
  const QJsonArray &contributions() const { return contributions_; }
  void registerContributions(const QString &manifestPath);
  void unregisterContributions(const QString &nameSpace);
  void executeContribution(const QString &id, std::optional<Address> address);

  // Reads.
  using Reply = std::function<void(const QJsonObject &payload)>;
  using Failure =
      std::function<void(const QString &code, const QString &message)>;
  /// Issue a read owned by \p owner.  Failures are reported to \p failed when
  /// given, otherwise logged.  Cancellation and session changes are silent.
  QueryService::SubscriptionId read(const QString &operation,
                                    const QJsonObject &payload, QObject *owner,
                                    Reply done, Failure failed = {});

  // Local types.
  /// Adds the types \p source declares in \p language (c, cpp, rust or go)
  /// to the program's local types; \p done has the types declared or the
  /// problems, each at its line and column.
  void declareTypes(const QString &language, const QString &source, Reply done,
                    Failure failed = {});
  /// Removes the local types \p names; \p done has the types removed and
  /// those that still name one.
  void forgetTypes(const QStringList &names, Reply done, Failure failed = {});

  // MCP bridge.
  void externalQuery(const QString &id, const QString &operation,
                     const QJsonObject &payload, const QString &revision);
  void publishSelection(Address address, std::optional<Address> function,
                        const QString &view);
  QJsonObject selection() const { return selection_; }

signals:
  void stateChanged();
  void opened();
  void unloaded();
  void revisionChanged();
  void generationChanged();
  /// The worker lists a different number of functions, for example after its
  /// function detector ran.
  void functionsChanged();
  void backgroundChanged();
  void historyChanged();
  void loadProgress(const QString &phase, double fraction);
  /// level: 0 information, 1 warning, 2 error.
  void message(const QString &text, int level);
  void transitionRequested(const QString &action);
  void quitApproved();
  void selectionChanged(const QJsonObject &selection);
  void externalResponse(const QString &id, const QJsonObject &response);
  void renamed(Address address, const QString &name);
  void commented(Address address, const QString &text);
  void contributionsChanged();
  void databaseSaved(bool ok);
  void contributionResult(const QJsonObject &result);
  /// The worker searches strings with new options; string views reload.
  void stringOptionsChanged();
  /// The workbench's own project file \p suffix changed on disk.
  void projectFileChanged(const QString &suffix);

private:
  /// The operations the worker offers (its hello).
  QSet<QString> capabilities_;
  using Callback = std::function<void(const QJsonObject &payload)>;
  void command(const QString &operation, const QJsonObject &payload,
               Callback done, Failure failed = {});
  void receive(const QJsonObject &message);
  void startWorker();
  void openPending();
  void sendOpen(const QString &requested, const QString &path,
                const QString &database,
                const QHash<QString, QByteArray> &state, LoadOptions options);
  /// Pack the project into the database \p path.
  void writeDatabase(const QString &path);
  /// Keep the debug files chosen for the input with the project, beside it.
  /// \p loadedNow is a file the user loaded into the open project, which
  /// a database the user opened then lacks.
  void recordDebugChoices(bool loadedNow);
  void requestTransition(const QString &action);
  void finishTransition();
  void refreshHistory();
  /// Watch the project's files beside the input for changes on disk.
  void watchProject();
  QFileSystemWatcher projectWatcher_;
  QTimer projectSync_;
  /// Each project file's content when the workbench last looked at it.
  QHash<QString, QByteArray> projectFiles_;
  void refreshContributions();
  /// Send the remembered string options, if any; \p announce reports them.
  void applyStringOptions(bool announce);
  /// Whether the remembered string options predate the preferred code page.
  bool stringOptionsNeedMigration() const;
  /// Rewrites string options that named at most one code page, the one
  /// searched, given the engine's \p encodings: it becomes the preferred
  /// page, and the encodings searched by default join the list.
  void migrateStringOptions(const QJsonArray &encodings);
  void resetState();
  void setError(const QString &text);
  void reportPdb(const QJsonObject &result);
  /// Say which MAP files \p debug, an open's or load's debug_info, read.
  void reportMaps(const QJsonObject &debug);
  /// Whether an edit aimed at \p epoch may be applied now.
  bool acceptsEdit(std::optional<quint64> epoch);

  EngineClient client_;
  QueryService queries_;
  AnalysisPool analysis_;
  QObject reads_, external_;
  QString workerPath_, filePath_, pendingFile_, transition_, error_;
  /// How the pending file loads, and how the open file loaded, which a
  /// restart repeats.
  LoadOptions pendingOptions_, loadOptions_;
  /// How a reload waiting for a Save/Discard decision reads the file.
  std::optional<LoadOptions> reloadOptions_;
  /// The open in flight reads the open file again.
  bool reloading_ = false;
  QString projectPath_, databasePath_;
  QHash<QString, QByteArray> databaseState_;
  std::function<QHash<QString, QByteArray>()> stateProvider_;
  QString generation_;
  qint64 functionCount_ = -1;
  QJsonObject metadata_, background_, history_, selection_;
  QJsonArray contributions_;
  quint64 sessionEpoch_ = 0;
  int pendingWrites_ = 0;
  bool connected_ = false, loaded_ = false, opening_ = false, dirty_ = false;
  /// The project is a database the user opened, and edits since it was
  /// last written are only in its unpacked files.
  bool openedFromDatabase_ = false, databaseDirty_ = false;
  bool transitionReady_ = false, restartPending_ = false, starting_ = false;
};

} // namespace neverd::gui
