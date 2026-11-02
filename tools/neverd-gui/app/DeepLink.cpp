//===- DeepLink.cpp - A link to a place in a program -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "DeepLink.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>
#include <algorithm>
#include <limits>

namespace neverd::gui {
namespace {

constexpr struct {
  LinkResource Resource;
  const char *Name;
} Resources[] = {{LinkResource::Functions, "functions"},
                 {LinkResource::Addresses, "addresses"},
                 {LinkResource::Strings, "strings"},
                 {LinkResource::Segments, "segments"},
                 {LinkResource::Names, "names"},
                 {LinkResource::Imports, "imports"},
                 {LinkResource::Exports, "exports"},
                 {LinkResource::Types, "types"},
                 {LinkResource::Bookmarks, "bookmarks"}};

constexpr struct {
  LinkView View;
  const char *Name;
} Views[] = {
    {LinkView::Disasm, "disasm"},         {LinkView::Graph, "graph"},
    {LinkView::Pseudocode, "pseudocode"}, {LinkView::Hexdump, "hexdump"},
    {LinkView::Functions, "functions"},   {LinkView::Segments, "segments"},
    {LinkView::Names, "names"},           {LinkView::Imports, "imports"},
    {LinkView::Exports, "exports"},       {LinkView::Strings, "strings"},
    {LinkView::Types, "types"},           {LinkView::Bookmarks, "bookmarks"}};

/// The digits of a SHA-256 in hexadecimal, and the fewest a link may give
/// of them: the link format's.
constexpr int HashDigits = 64, MinHashDigits = 8;

/// A number as a link writes it: decimal, or hexadecimal after 0x.
std::optional<Address> linkNumber(const QString &text) {
  const bool hexadecimal =
      text.startsWith(QLatin1String("0x"), Qt::CaseInsensitive);
  const QStringView digits =
      hexadecimal ? QStringView(text).sliced(2) : QStringView(text);
  if (digits.isEmpty() ||
      !std::all_of(digits.begin(), digits.end(), [hexadecimal](QChar c) {
        return (c >= u'0' && c <= u'9') ||
               (hexadecimal &&
                ((c >= u'a' && c <= u'f') || (c >= u'A' && c <= u'F')));
      }))
    return std::nullopt;
  bool ok = false;
  const Address value = hexadecimal ? text.mid(2).toULongLong(&ok, 16)
                                    : text.toULongLong(&ok, 10);
  if (!ok)
    return std::nullopt;
  return value;
}

ParsedLink failure(const QString &error) { return {std::nullopt, error}; }

} // namespace

LinkView DeepLink::shownView() const {
  return view.value_or(linkViews(resource).front());
}

std::optional<Address> DeepLink::address(Address base) const {
  if (ea)
    return ea;
  if (rva && *rva <= std::numeric_limits<Address>::max() - base)
    return base + *rva;
  return std::nullopt;
}

bool DeepLink::namesHash(const QString &whole) const {
  return sha256.isEmpty() || whole.startsWith(sha256, Qt::CaseInsensitive);
}

QList<LinkView> linkViews(LinkResource resource) {
  // IDA's table, where an address also shows in the graph and at its line
  // of the pseudocode.
  switch (resource) {
  case LinkResource::Functions:
    return {LinkView::Functions, LinkView::Disasm, LinkView::Graph,
            LinkView::Hexdump, LinkView::Pseudocode};
  case LinkResource::Addresses:
    return {LinkView::Disasm, LinkView::Graph, LinkView::Hexdump,
            LinkView::Pseudocode};
  case LinkResource::Strings:
    return {LinkView::Strings, LinkView::Disasm, LinkView::Hexdump};
  case LinkResource::Segments:
    return {LinkView::Segments, LinkView::Disasm, LinkView::Hexdump};
  case LinkResource::Names:
    return {LinkView::Names, LinkView::Disasm, LinkView::Hexdump};
  case LinkResource::Imports:
    return {LinkView::Imports, LinkView::Disasm, LinkView::Hexdump};
  case LinkResource::Exports:
    return {LinkView::Exports, LinkView::Disasm, LinkView::Hexdump};
  case LinkResource::Types:
    return {LinkView::Types};
  case LinkResource::Bookmarks:
    return {LinkView::Disasm, LinkView::Graph, LinkView::Hexdump,
            LinkView::Pseudocode, LinkView::Bookmarks};
  }
  return {LinkView::Disasm};
}

QString linkResourceName(LinkResource resource) {
  for (const auto &entry : Resources)
    if (entry.Resource == resource)
      return QString::fromLatin1(entry.Name);
  return {};
}

QString linkViewName(LinkView view) {
  for (const auto &entry : Views)
    if (entry.View == view)
      return QString::fromLatin1(entry.Name);
  return {};
}

bool looksLikeDeepLink(const QString &text) {
  const QString trimmed = text.trimmed();
  return trimmed.startsWith(QLatin1String("neverd:"), Qt::CaseInsensitive) ||
         trimmed.startsWith(QLatin1String("ida:"), Qt::CaseInsensitive);
}

ParsedLink parseDeepLink(const QString &text) {
  const QUrl url(text.trimmed(), QUrl::StrictMode);
  if (!url.isValid() || !looksLikeDeepLink(text))
    return failure(QCoreApplication::translate(
        "DeepLink", "A link starts with neverd:// or ida://."));
  DeepLink link;
  link.scheme = url.scheme().toLower();
  if (link.scheme != QLatin1String("neverd") &&
      link.scheme != QLatin1String("ida"))
    return failure(QCoreApplication::translate(
        "DeepLink", "A link starts with neverd:// or ida://."));
  // IDA's hcli writes this machine as localhost where the URL must name a
  // host.
  link.source = url.host();
  if (link.source == QLatin1String("localhost"))
    link.source.clear();
  QStringList path = url.path(QUrl::FullyDecoded).split(QLatin1Char('/'));
  if (!path.isEmpty() && path.front().isEmpty())
    path.removeFirst();
  if (path.size() >= 3 && path[0] == QLatin1String("api"))
    return failure(QCoreApplication::translate(
        "DeepLink", "A Knowledge Exchange link names a file to download, "
                    "not a place in a program."));
  if (path.size() == 2) {
    link.input = path.takeFirst();
    if (link.input.isEmpty())
      return failure(QCoreApplication::translate(
          "DeepLink", "A link names its input before its resource."));
  }
  if (path.size() != 1)
    return failure(QCoreApplication::translate(
        "DeepLink", "A link names one resource, after its input's name "
                    "when it has one."));
  bool known = false;
  for (const auto &entry : Resources)
    if (path.front() == QLatin1String(entry.Name)) {
      link.resource = entry.Resource;
      known = true;
    }
  if (!known)
    return failure(QCoreApplication::translate(
                       "DeepLink",
                       "A link names functions, addresses, strings, segments, "
                       "names, imports, exports, types or bookmarks, not "
                       "\"%1\".")
                       .arg(path.front()));

  QStringList seen;
  for (const auto &[key, value] :
       QUrlQuery(url).queryItems(QUrl::FullyDecoded)) {
    // Parameters the form does not have are another tool's.
    static const QStringList Keys{
        QStringLiteral("rva"), QStringLiteral("ea"), QStringLiteral("name"),
        QStringLiteral("view"), QStringLiteral("sha256")};
    if (!Keys.contains(key))
      continue;
    if (seen.contains(key))
      return failure(
          QCoreApplication::translate("DeepLink", "A link gives %1 once.")
              .arg(key));
    seen << key;
    if (key == QLatin1String("rva") || key == QLatin1String("ea")) {
      const auto number = linkNumber(value);
      if (!number)
        return failure(
            QCoreApplication::translate(
                "DeepLink", "%1 is a number, decimal or after 0x, not \"%2\".")
                .arg(key, value));
      (key == QLatin1String("rva") ? link.rva : link.ea) = number;
    } else if (key == QLatin1String("name")) {
      link.name = value;
    } else if (key == QLatin1String("view")) {
      bool found = false;
      for (const auto &entry : Views)
        if (value == QLatin1String(entry.Name)) {
          link.view = entry.View;
          found = true;
        }
      if (!found)
        return failure(QCoreApplication::translate("DeepLink",
                                                   "A link has no view \"%1\".")
                           .arg(value));
    } else {
      static const QString Digits = QStringLiteral("0123456789abcdef");
      const QString hash = value.toLower();
      if (hash.size() < MinHashDigits || hash.size() > HashDigits ||
          std::any_of(hash.begin(), hash.end(),
                      [](QChar c) { return !Digits.contains(c); }))
        return failure(QCoreApplication::translate(
                           "DeepLink",
                           "sha256 is 8 to 64 hexadecimal digits of the "
                           "input's SHA-256, not \"%1\".")
                           .arg(value));
      link.sha256 = hash;
    }
  }
  if (link.rva && link.ea)
    return failure(QCoreApplication::translate(
        "DeepLink", "A link gives rva or ea, not both."));
  const QString resource = path.front();
  if (link.resource == LinkResource::Types) {
    if (link.rva || link.ea)
      return failure(QCoreApplication::translate(
          "DeepLink", "A types link names its type with name=, not an "
                      "address."));
    if (link.name.isEmpty())
      return failure(QCoreApplication::translate(
          "DeepLink", "A types link names its type with name=."));
  } else {
    if (!link.rva && !link.ea)
      return failure(
          QCoreApplication::translate(
              "DeepLink", "%1 links give their address with rva= or ea=.")
              .arg(resource));
    if (!link.name.isEmpty())
      return failure(QCoreApplication::translate(
                         "DeepLink", "%1 links name an address, not a type.")
                         .arg(resource));
  }
  const QList<LinkView> views = linkViews(link.resource);
  if (link.view && !views.contains(*link.view)) {
    QStringList names;
    for (const LinkView view : views)
      names << linkViewName(view);
    return failure(QCoreApplication::translate(
                       "DeepLink", "%1 links show in %2, not in %3.")
                       .arg(resource, names.join(QLatin1String(", ")),
                            linkViewName(*link.view)));
  }
  return {std::move(link), {}};
}

QString formatDeepLink(const DeepLink &link) {
  QString text =
      link.scheme + QStringLiteral("://") + link.source + QLatin1Char('/');
  if (!link.input.isEmpty())
    text += QString::fromLatin1(QUrl::toPercentEncoding(link.input)) +
            QLatin1Char('/');
  text += linkResourceName(link.resource);
  QStringList parameters;
  if (link.rva)
    parameters << QStringLiteral("rva=") + hexAddress(*link.rva);
  else if (link.ea)
    parameters << QStringLiteral("ea=") + hexAddress(*link.ea);
  if (!link.name.isEmpty())
    parameters << QStringLiteral("name=") +
                      QString::fromLatin1(QUrl::toPercentEncoding(link.name));
  if (link.view)
    parameters << QStringLiteral("view=") + linkViewName(*link.view);
  if (!link.sha256.isEmpty())
    parameters << QStringLiteral("sha256=") + link.sha256;
  if (!parameters.isEmpty())
    text += QLatin1Char('?') + parameters.join(QLatin1Char('&'));
  return text;
}

bool linkNamesInput(const QString &linkInput, const QString &fileName) {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
  constexpr auto Case = Qt::CaseInsensitive;
#else
  constexpr auto Case = Qt::CaseSensitive;
#endif
  if (linkInput.compare(fileName, Case) == 0)
    return true;
  // IDA's database of foo.exe: foo.exe.i64, or foo.i64 as older IDAs name
  // it.
  for (const char *extension : {".i64", ".idb"}) {
    if (!linkInput.endsWith(QLatin1String(extension), Qt::CaseInsensitive))
      continue;
    const QString stem = linkInput.chopped(4);
    if (stem.compare(fileName, Case) == 0 ||
        stem.compare(QFileInfo(fileName).completeBaseName(), Case) == 0)
      return true;
  }
  return false;
}

} // namespace neverd::gui
