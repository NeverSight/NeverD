#include "MobileIOSInternal.h"

#include "llvm/Support/ConvertUTF.h"

#include <charconv>
#include <cstring>
#include <set>

namespace neverd::mobile::ios {
namespace {
void utf8(std::string &s, uint32_t c) {
  if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) || c == 0)
    throw Error("invalid plist Unicode scalar");
  if (c < 0x80)
    s += char(c);
  else if (c < 0x800) {
    s += char(0xc0 | (c >> 6));
    s += char(0x80 | (c & 63));
  } else if (c < 0x10000) {
    s += char(0xe0 | (c >> 12));
    s += char(0x80 | ((c >> 6) & 63));
    s += char(0x80 | (c & 63));
  } else {
    s += char(0xf0 | (c >> 18));
    s += char(0x80 | ((c >> 12) & 63));
    s += char(0x80 | ((c >> 6) & 63));
    s += char(0x80 | (c & 63));
  }
}
class Plist {
  std::string_view data;
  Budget &budget;
  size_t pos = 0;
  uint64_t objects = 0, offset_table = 0;
  uint64_t decoded_bytes = 0;
  unsigned offset_size = 0, ref_size = 0;
  std::set<uint64_t> active;
  void allocation(uint64_t bytes) {
    // A binary plist can refer to the same collection repeatedly. Bound its
    // expanded value graph, not merely its compact on-disk object table.
    auto limit = std::min<uint64_t>(budget.limits.max_bytes, 64 * 1024 * 1024);
    if (bytes > limit - decoded_bytes)
      throw Error("expanded Info.plist exceeds its decoded byte budget");
    decoded_bytes += bytes;
  }
  uint64_t get(uint64_t p, unsigned n) {
    if (!n || n > 8 || p > data.size() || n > data.size() - p)
      throw Error("truncated binary plist integer");
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
      v = (v << 8) | uint8_t(data[p + i]);
    return v;
  }
  void range(uint64_t p, uint64_t n) {
    if (p > offset_table || n > offset_table - p)
      throw Error("binary plist object extends outside object table");
  }
  Value binary(uint64_t id, unsigned depth) {
    budget.tick();
    allocation(128);
    if (depth > 128 || id >= objects || !active.insert(id).second)
      throw Error("cyclic or oversized binary plist object graph");
    struct Pop {
      std::set<uint64_t> &s;
      uint64_t id;
      ~Pop() { s.erase(id); }
    } pop{active, id};
    uint64_t p = get(offset_table + id * offset_size, offset_size);
    range(p, 1);
    unsigned tag = uint8_t(data[p++]), kind = tag >> 4;
    uint64_t count = tag & 15;
    if (count == 15 &&
        (kind == 4 || kind == 5 || kind == 6 || kind == 0xa || kind == 0xd)) {
      range(p, 1);
      auto t = uint8_t(data[p++]);
      if ((t >> 4) != 1 || (t & 15) > 3)
        throw Error("invalid binary plist length");
      unsigned n = 1u << (t & 15);
      range(p, n);
      count = get(p, n);
      p += n;
    }
    if (kind == 0) {
      if (tag == 8)
        return false;
      if (tag == 9)
        return true;
      if (tag == 0)
        return nullptr;
      throw Error("unsupported binary plist singleton");
    }
    if (kind == 1) {
      if (count > 3)
        throw Error("oversized binary plist integer");
      unsigned n = 1u << count;
      range(p, n);
      return int64_t(get(p, n));
    }
    if (kind == 2 || kind == 3) {
      unsigned n = kind == 3 ? 8 : (1u << count);
      if (n != 4 && n != 8)
        throw Error("invalid binary plist real");
      range(p, n);
      return nullptr;
    }
    if (kind == 4 || kind == 5 || kind == 6) {
      unsigned width = kind == 6 ? 2 : 1;
      if (count > offset_table / width)
        throw Error("oversized binary plist string");
      range(p, count * width);
      if (kind == 4)
        return nullptr;
      allocation(count * (kind == 6 ? 3 : 1));
      std::string s;
      if (kind == 5) {
        s = std::string(data.substr(p, count));
        if (std::any_of(s.begin(), s.end(),
                        [](unsigned char c) { return c >= 128 || c == 0; }))
          throw Error("invalid binary plist ASCII string");
      } else
        for (uint64_t i = 0; i < count; ++i) {
          auto c = get(p + 2 * i, 2);
          if (c >= 0xd800 && c <= 0xdbff) {
            if (++i >= count)
              throw Error("invalid binary plist surrogate");
            auto d = get(p + 2 * i, 2);
            if (d < 0xdc00 || d > 0xdfff)
              throw Error("invalid binary plist surrogate");
            c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00);
          }
          utf8(s, c);
        }
      return s;
    }
    if (kind == 0xa || kind == 0xd) {
      if (count > 100000 ||
          count > offset_table / ref_size / (kind == 0xd ? 2 : 1))
        throw Error("oversized binary plist collection");
      range(p, count * ref_size * (kind == 0xd ? 2 : 1));
      if (kind == 0xa) {
        Array a;
        for (uint64_t i = 0; i < count; ++i)
          a.push_back(binary(get(p + i * ref_size, ref_size), depth + 1));
        return a;
      }
      Object o;
      for (uint64_t i = 0; i < count; ++i) {
        auto key = binary(get(p + i * ref_size, ref_size), depth + 1);
        auto s = key.getAsString();
        if (!s || o.get(*s))
          throw Error("invalid or duplicate binary plist dictionary key");
        // JSON StringRef keys borrow their bytes. The decoded key Value is
        // local to this iteration, so the dictionary must own its spelling.
        o[s->str()] =
            binary(get(p + (count + i) * ref_size, ref_size), depth + 1);
      }
      return o;
    }
    throw Error("unsupported binary plist object type");
  }
  void skip() {
    for (;;) {
      while (pos < data.size() &&
             std::isspace(static_cast<unsigned char>(data[pos])))
        ++pos;
      if (data.substr(pos).starts_with("<!--")) {
        auto end = data.find("-->", pos + 4);
        if (end == data.npos)
          throw Error("unterminated plist comment");
        pos = end + 3;
      } else if (data.substr(pos).starts_with("<?xml")) {
        auto end = data.find("?>", pos + 5);
        if (end == data.npos)
          throw Error("unterminated plist declaration");
        pos = end + 2;
      } else if (data.substr(pos).starts_with("<!DOCTYPE")) {
        auto end = data.find('>', pos + 9);
        if (end == data.npos ||
            data.substr(pos, end - pos).find('[') != data.npos)
          throw Error("plist entity declarations are unsupported");
        pos = end + 1;
      } else
        break;
    }
  }
  std::pair<std::string, bool> tag() {
    skip();
    if (pos >= data.size() || data[pos++] != '<')
      throw Error("invalid plist XML tag");
    auto end = data.find('>', pos);
    if (end == data.npos)
      throw Error("unterminated plist XML tag");
    auto text = llvm::StringRef(data.data() + pos, end - pos).trim();
    pos = end + 1;
    bool empty = text.ends_with('/');
    if (empty)
      text = text.drop_back().rtrim();
    auto split = text.find_first_of(" \t\r\n");
    auto name = text.take_front(split);
    if (split != llvm::StringRef::npos && name != "plist")
      throw Error("unsupported plist XML attribute");
    return {name.str(), empty};
  }
  std::string content(std::string_view name) {
    auto end = data.find('<', pos);
    if (end == data.npos)
      throw Error("unterminated plist XML text");
    auto raw = data.substr(pos, end - pos);
    allocation(raw.size());
    std::string s;
    for (size_t i = 0; i < raw.size(); ++i) {
      if (raw[i] != '&') {
        if (raw[i] == 0)
          throw Error("invalid plist NUL");
        s += raw[i];
        continue;
      }
      auto semi = raw.find(';', i + 1);
      if (semi == raw.npos)
        throw Error("unterminated plist entity");
      auto e = raw.substr(i + 1, semi - i - 1);
      if (e == "amp")
        s += '&';
      else if (e == "lt")
        s += '<';
      else if (e == "gt")
        s += '>';
      else if (e == "quot")
        s += '"';
      else if (e == "apos")
        s += '\'';
      else if (e.starts_with('#')) {
        e.remove_prefix(1);
        int base = 10;
        if (e.starts_with('x')) {
          e.remove_prefix(1);
          base = 16;
        }
        uint32_t c = 0;
        auto r = std::from_chars(e.data(), e.data() + e.size(), c, base);
        if (r.ec != std::errc() || r.ptr != e.data() + e.size())
          throw Error("invalid plist numeric entity");
        utf8(s, c);
      } else
        throw Error("unknown plist XML entity");
      i = semi;
    }
    pos = end;
    if (tag().first != "/" + std::string(name))
      throw Error("mismatched plist XML closing tag");
    if (!llvm::json::isUTF8(s))
      throw Error("invalid plist UTF-8");
    return s;
  }
  Value xml(unsigned depth) {
    budget.tick();
    allocation(128);
    if (depth > 128)
      throw Error("plist nesting exceeds 128 levels");
    auto [name, empty] = tag();
    if (name == "dict") {
      Object o;
      if (empty)
        return o;
      for (;;) {
        skip();
        if (data.substr(pos).starts_with("</dict>")) {
          pos += 7;
          return o;
        }
        auto [key, closed] = tag();
        if (key != "key")
          throw Error("plist dictionary key expected");
        auto k = closed ? std::string() : content("key");
        if (o.get(k))
          throw Error("duplicate plist dictionary key");
        o[k] = xml(depth + 1);
      }
    }
    if (name == "array") {
      Array a;
      if (empty)
        return a;
      for (;;) {
        skip();
        if (data.substr(pos).starts_with("</array>")) {
          pos += 8;
          return a;
        }
        if (a.size() >= 100000)
          throw Error("oversized plist array");
        a.push_back(xml(depth + 1));
      }
    }
    if (name == "true" || name == "false") {
      if (!empty && !content(name).empty())
        throw Error("invalid plist boolean");
      return name == "true";
    }
    if (name == "string" || name == "key")
      return empty ? std::string() : content(name);
    if (name == "integer") {
      auto s = empty ? std::string() : content(name);
      auto t = llvm::StringRef(s).trim();
      int64_t n = 0;
      if (t.getAsInteger(0, n))
        throw Error("invalid plist integer");
      return n;
    }
    if (name == "real" || name == "date" || name == "data") {
      if (!empty)
        content(name);
      return nullptr;
    }
    throw Error("unsupported plist XML element: " + name);
  }

public:
  Plist(std::string_view d, Budget &b) : data(d), budget(b) {}
  Object parse() {
    if (data.size() > 16 * 1024 * 1024)
      throw Error("Info.plist exceeds 16 MiB");
    Value root(nullptr);
    if (data.starts_with("bplist00")) {
      if (data.size() < 40)
        throw Error("truncated binary plist");
      auto t = data.size() - 32;
      offset_size = get(t + 6, 1);
      ref_size = get(t + 7, 1);
      objects = get(t + 8, 8);
      auto top = get(t + 16, 8);
      offset_table = get(t + 24, 8);
      if (!objects || objects > 100000 || !offset_size || offset_size > 8 ||
          !ref_size || ref_size > 8 || offset_table < 8 || offset_table > t ||
          objects > (t - offset_table) / offset_size)
        throw Error("invalid binary plist trailer");
      root = binary(top, 0);
    } else {
      if (data.starts_with("\xef\xbb\xbf"))
        pos = 3;
      auto [name, empty] = tag();
      if (name != "plist" || empty)
        throw Error("plist XML root expected");
      root = xml(0);
      if (tag().first != "/plist")
        throw Error("unclosed plist root");
      skip();
      if (pos != data.size())
        throw Error("trailing plist XML data");
    }
    return object(root, "Info.plist root");
  }
};
} // namespace
Object parsePlist(std::string_view bytes, Budget &budget) {
  return Plist(bytes, budget).parse();
}
} // namespace neverd::mobile::ios
