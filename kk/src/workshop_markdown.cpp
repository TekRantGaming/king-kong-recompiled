// This file is UTF-8.
#include "workshop_markdown.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "workshop_logic.h"

namespace kk::workshop::md {
namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool IsPunct(char c) { return std::ispunct(static_cast<unsigned char>(c)) != 0; }
bool IsAlnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || (static_cast<unsigned char>(c) & 0x80); }

std::string Trim(std::string_view s) {
  size_t a = 0, b = s.size();
  while (a < b && IsSpace(s[a])) ++a;
  while (b > a && IsSpace(s[b - 1])) --b;
  return std::string(s.substr(a, b - a));
}

// An entity at text[i] ('&'): appends its UTF-8 and returns its length, or 0 if it is not one.
size_t EntityAt(std::string_view text, size_t i, std::string& out) {
  const size_t semi = text.find(';', i + 1);
  if (semi == std::string_view::npos || semi - i > 10) return 0;
  const std::string name(text.substr(i + 1, semi - i - 1));
  if (name.empty()) return 0;
  if (name[0] == '#') {
    const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
    const char* digits = name.c_str() + (hex ? 2 : 1);
    if (!*digits) return 0;
    char* end = nullptr;
    const unsigned long cp = std::strtoul(digits, &end, hex ? 16 : 10);
    if (*end || cp == 0 || cp > 0x10FFFF) return 0;
    AppendUtf8(out, uint32_t(cp));
    return semi - i + 1;
  }
  static constexpr std::pair<const char*, const char*> kNames[] = {
      {"amp", "&"},          {"lt", "<"},           {"gt", ">"},           {"quot", "\""},
      {"apos", "'"},         {"nbsp", " "},         {"copy", "\xC2\xA9"},  {"reg", "\xC2\xAE"},
      {"trade", "\xE2\x84\xA2"}, {"mdash", "\xE2\x80\x94"}, {"ndash", "\xE2\x80\x93"},
      {"hellip", "\xE2\x80\xA6"}, {"middot", "\xC2\xB7"}, {"bull", "\xE2\x80\xA2"}, {"rarr", "\xE2\x86\x92"},
      {"larr", "\xE2\x86\x90"}, {"times", "\xC3\x97"}, {"deg", "\xC2\xB0"}, {"laquo", "\xC2\xAB"},
      {"raquo", "\xC2\xBB"}, {"rsquo", "\xE2\x80\x99"}, {"lsquo", "\xE2\x80\x98"}, {"rdquo", "\xE2\x80\x9D"},
      {"ldquo", "\xE2\x80\x9C"}, {"ensp", " "}, {"emsp", " "}, {"thinsp", " "}, {"zwj", ""}, {"zwnj", ""}};
  for (const auto& [n, v] : kNames)
    if (name == n) {
      out += v;
      return semi - i + 1;
    }
  return 0;
}

// ------------------------------------------------------------- inline ---
enum class Tok { kText, kBreak, kPara, kCell, kRow, kImage, kHeadStart, kHeadEnd, kRule, kBullet };

struct Token {
  Tok kind = Tok::kText;
  Span span;
  Image image;
  int level = 0;
};

class InlineParser {
 public:
  InlineParser(const Resolver& resolve, std::vector<Token>& out) : resolve_(resolve), out_(out) {}

  void Run(std::string_view s) {
    for (size_t i = 0; i < s.size();) i = Step(s, i);
    Flush();
  }

 private:
  std::string Resolve(const std::string& url, bool picture) const {
    return resolve_ ? resolve_(url, picture) : url;
  }
  std::string Link() const { return links_.empty() ? std::string() : links_.back(); }

  void Flush() {
    if (buf_.empty()) return;
    Token t;
    t.span.text = std::move(buf_);
    t.span.bold = bold_;
    t.span.italic = italic_;
    t.span.code = code_;
    t.span.link = Link();
    out_.push_back(std::move(t));
    buf_.clear();
  }
  void Emit(Tok kind, int level = 0) {
    Flush();
    Token t;
    t.kind = kind;
    t.level = level;
    out_.push_back(std::move(t));
  }

  // The index after a bracketed run starting at s[i] (open), matching nested pairs; npos if unclosed.
  static size_t Match(std::string_view s, size_t i, char open, char close) {
    int depth = 0;
    for (size_t k = i; k < s.size(); ++k) {
      if (s[k] == '\\') {
        ++k;
        continue;
      }
      if (s[k] == open) ++depth;
      else if (s[k] == close && --depth == 0) return k;
    }
    return std::string_view::npos;
  }

  // "(url "title")" at s[i] == '(': the address and the index after ')'.
  static bool Target(std::string_view s, size_t i, std::string& url, size_t& after) {
    if (i >= s.size() || s[i] != '(') return false;
    const size_t end = Match(s, i, '(', ')');
    if (end == std::string_view::npos) return false;
    std::string inner = Trim(s.substr(i + 1, end - i - 1));
    if (!inner.empty() && inner[0] == '<') {
      const size_t gt = inner.find('>');
      url = inner.substr(1, gt == std::string::npos ? std::string::npos : gt - 1);
    } else {
      const size_t sp = inner.find_first_of(" \t\n");
      url = inner.substr(0, sp);
    }
    after = end + 1;
    return true;
  }

  size_t Step(std::string_view s, size_t i) {
    const char c = s[i];
    const char prev = i > 0 ? s[i - 1] : ' ';
    if (c == '\\' && i + 1 < s.size()) {
      if (s[i + 1] == '\n') {
        Emit(Tok::kBreak);
        return i + 2;
      }
      if (IsPunct(s[i + 1])) {
        buf_ += s[i + 1];
        return i + 2;
      }
    }
    if (c == '`') {
      size_t n = 0;
      while (i + n < s.size() && s[i + n] == '`') ++n;
      for (size_t k = s.find(std::string(n, '`'), i + n); k != std::string_view::npos;
           k = s.find(std::string(n, '`'), k + 1)) {
        if (k + n < s.size() && s[k + n] == '`') continue;  // a longer run
        Flush();
        std::string code(s.substr(i + n, k - i - n));
        std::replace(code.begin(), code.end(), '\n', ' ');
        if (code.size() > 2 && code.front() == ' ' && code.back() == ' ') code = code.substr(1, code.size() - 2);
        const bool was = code_;
        code_ = true;
        buf_ = code;
        Flush();
        code_ = was;
        return k + n;
      }
      buf_.append(n, '`');
      return i + n;
    }
    if (code_) {  // inside <code>: no other markup but tags
      if (c != '<' && c != '&') {
        buf_ += c;
        return i + 1;
      }
    }
    if (c == '*' || c == '_') {
      size_t n = 0;
      while (i + n < s.size() && s[i + n] == c) ++n;
      const char next = i + n < s.size() ? s[i + n] : ' ';
      const bool left = !IsSpace(next) && (!IsPunct(next) || IsSpace(prev) || IsPunct(prev));
      const bool right = !IsSpace(prev) && (!IsPunct(prev) || IsSpace(next) || IsPunct(next));
      bool can_open = left, can_close = right;
      if (c == '_') {  // snake_case stays as it is
        can_open = left && (!right || IsPunct(prev));
        can_close = right && (!left || IsPunct(next));
      }
      size_t used = 0;
      auto toggle = [&](bool& flag, size_t count) {
        if (flag && can_close) flag = false;
        else if (!flag && can_open) flag = true;
        else return;
        used += count;
      };
      Flush();
      if (n >= 2) toggle(bold_, 2);
      if (n == 1 || n >= 3) toggle(italic_, 1);
      if (used == 0) {
        buf_.append(n, c);
        return i + n;
      }
      if (used < n) buf_.append(n - used, c);
      return i + n;
    }
    if (c == '~' && i + 1 < s.size() && s[i + 1] == '~') return i + 2;  // strikethrough: as plain text
    if (c == '!' && i + 1 < s.size() && s[i + 1] == '[') {
      const size_t close = Match(s, i + 1, '[', ']');
      std::string url;
      size_t after = 0;
      if (close != std::string_view::npos && Target(s, close + 1, url, after)) {
        Flush();
        Token t;
        t.kind = Tok::kImage;
        t.image.alt = DecodeEntities(std::string(s.substr(i + 2, close - i - 2)));
        t.image.src = Resolve(url, true);
        t.image.link = Link();
        out_.push_back(std::move(t));
        return after;
      }
    }
    if (c == '[') {
      const size_t close = Match(s, i, '[', ']');
      std::string url;
      size_t after = 0;
      if (close != std::string_view::npos && !(i + 1 < s.size() && s[i + 1] == '^') && Target(s, close + 1, url, after)) {
        Flush();
        links_.push_back(Resolve(url, false));
        for (size_t k = i + 1; k < close;) k = Step(s.substr(0, close), k);
        Flush();
        links_.pop_back();
        return after;
      }
    }
    if (c == '<') {
      if (s.substr(i, 4) == "<!--") {
        const size_t end = s.find("-->", i + 4);
        return end == std::string_view::npos ? s.size() : end + 3;
      }
      if (s.substr(i, 9) == "<https://" || s.substr(i, 8) == "<http://") {
        const size_t gt = s.find('>', i);
        if (gt != std::string_view::npos && s.substr(i, gt - i).find_first_of(" \n") == std::string_view::npos) {
          const std::string url(s.substr(i + 1, gt - i - 1));
          Flush();
          links_.push_back(Resolve(url, false));
          buf_ = url;
          Flush();
          links_.pop_back();
          return gt + 1;
        }
      }
      if (const size_t after = Tag(s, i)) return after;
    }
    if (c == '&') {
      std::string decoded;
      if (const size_t n = EntityAt(s, i, decoded)) {
        buf_ += decoded;
        return i + n;
      }
    }
    if (c == '\n') {
      if (buf_.size() >= 2 && buf_.compare(buf_.size() - 2, 2, "  ") == 0) {
        while (!buf_.empty() && buf_.back() == ' ') buf_.pop_back();
        Emit(Tok::kBreak);
      } else {
        buf_ += ' ';
      }
      return i + 1;
    }
    if ((c == 'h' || c == 'H') && links_.empty() && !IsAlnum(prev) &&
        (s.substr(i, 8) == "https://" || s.substr(i, 7) == "http://")) {
      size_t end = i;
      while (end < s.size() && !IsSpace(s[end]) && s[end] != '<') ++end;
      std::string url(s.substr(i, end - i));
      while (!url.empty() && std::strchr(".,;:!?*_)'\"", url.back())) {
        if (url.back() == ')' && std::count(url.begin(), url.end(), '(') >= std::count(url.begin(), url.end(), ')'))
          break;
        url.pop_back();
      }
      Flush();
      links_.push_back(Resolve(url, false));
      buf_ = url;
      Flush();
      links_.pop_back();
      return i + url.size();
    }
    buf_ += c;
    return i + 1;
  }

  // An HTML tag at s[i]: what it means here, and the index after it (0 if it is not a tag).
  size_t Tag(std::string_view s, size_t i) {
    size_t k = i + 1;
    const bool closing = k < s.size() && s[k] == '/';
    if (closing) ++k;
    const size_t name_start = k;
    while (k < s.size() && (std::isalnum(static_cast<unsigned char>(s[k])) || s[k] == '-')) ++k;
    if (k == name_start || !std::isalpha(static_cast<unsigned char>(s[name_start]))) return 0;
    const std::string name = Lower(std::string(s.substr(name_start, k - name_start)));
    // Attributes up to '>' (quoted values may hold '>').
    char quote = 0;
    for (; k < s.size(); ++k) {
      if (quote) {
        if (s[k] == quote) quote = 0;
      } else if (s[k] == '"' || s[k] == '\'') {
        quote = s[k];
      } else if (s[k] == '<') {
        return 0;
      } else if (s[k] == '>') {
        break;
      }
    }
    if (k >= s.size()) return 0;
    const std::string_view tag = s.substr(i, k - i);
    const size_t after = k + 1;
    auto is = [&](std::initializer_list<const char*> names) {
      for (const char* n : names)
        if (name == n) return true;
      return false;
    };
    if (name == "img") {
      Flush();
      Token t;
      t.kind = Tok::kImage;
      t.image.src = Resolve(DecodeEntities(HtmlAttr(tag, "src")), true);
      t.image.alt = DecodeEntities(HtmlAttr(tag, "alt"));
      const std::string w = HtmlAttr(tag, "width");
      if (w.find('%') != std::string::npos) t.image.width_pct = std::atoi(w.c_str());
      else t.image.width_px = HtmlPixels(w);
      t.image.link = Link();
      out_.push_back(std::move(t));
    } else if (name == "a") {
      Flush();
      if (closing) {
        if (!links_.empty()) links_.pop_back();
      } else {
        const std::string href = HtmlAttr(tag, "href");
        links_.push_back(href.empty() ? std::string() : Resolve(DecodeEntities(href), false));
      }
    } else if (is({"b", "strong"})) {
      Flush();
      bold_ = !closing;
    } else if (is({"i", "em"})) {
      Flush();
      italic_ = !closing;
    } else if (is({"code", "kbd", "samp", "tt"})) {
      Flush();
      code_ = !closing;
    } else if (name == "br") {
      Emit(Tok::kBreak);
    } else if (name == "hr") {
      Emit(Tok::kRule);
    } else if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
      Emit(closing ? Tok::kHeadEnd : Tok::kHeadStart, name[1] - '0');
    } else if (name == "li") {
      Emit(Tok::kPara);
      if (!closing) Emit(Tok::kBullet);
    } else if (is({"td", "th"})) {
      Emit(Tok::kCell);
    } else if (name == "tr") {
      Emit(Tok::kRow);
    } else if (is({"p", "div", "center", "details", "summary", "section", "article", "header", "footer",
                   "blockquote", "ul", "ol", "table", "thead", "tbody", "tfoot", "figure", "figcaption", "dl", "dt",
                   "dd", "pre", "picture", "video", "nav", "main"})) {
      Emit(Tok::kPara);
    }
    // Any other tag (sub, sup, span, source, font...) is left out; its text stays.
    return after;
  }

  const Resolver& resolve_;
  std::vector<Token>& out_;
  std::string buf_;
  bool bold_ = false, italic_ = false, code_ = false;
  std::vector<std::string> links_;
};

// ------------------------------------------------------------- blocks ---
bool HasText(const std::vector<Span>& spans) {
  for (const Span& s : spans)
    for (const char c : s.text)
      if (!IsSpace(c)) return true;
  return false;
}

// Runs of white space as one space (not in code), none at the start, the end or around a line break.
void Normalise(std::vector<Span>& spans) {
  bool at_line_start = true;
  for (Span& sp : spans) {
    if (sp.text == "\n") {
      at_line_start = true;
      continue;
    }
    std::string out;
    for (const char c : sp.text) {
      if (IsSpace(c)) {
        if (at_line_start || (!out.empty() && out.back() == ' ')) continue;
        out += ' ';
      } else {
        out += c;
        at_line_start = false;
      }
    }
    sp.text = std::move(out);
  }
  // Spaces before a line break and at the end.
  for (size_t i = 0; i < spans.size(); ++i) {
    const bool before_break = i + 1 == spans.size() || spans[i + 1].text == "\n";
    if (before_break)
      while (!spans[i].text.empty() && spans[i].text.back() == ' ') spans[i].text.pop_back();
  }
  // A space at the end of a span followed by a span starting at a line start is fine; drop empty spans and
  // breaks at the ends.
  std::vector<Span> kept;
  for (Span& sp : spans)
    if (!sp.text.empty()) kept.push_back(std::move(sp));
  while (!kept.empty() && kept.front().text == "\n") kept.erase(kept.begin());
  while (!kept.empty() && kept.back().text == "\n") kept.pop_back();
  // Collapse "\n" repeats to at most two (a blank line).
  spans.clear();
  int breaks = 0;
  for (Span& sp : kept) {
    if (sp.text == "\n") {
      if (++breaks > 2) continue;
    } else {
      breaks = 0;
    }
    spans.push_back(std::move(sp));
  }
}

void Flatten(std::vector<Token>& tokens, const Block& base, std::vector<Block>& out) {
  Block text = base;
  text.spans.clear();
  bool images_open = false;
  int heading = 0;
  bool bullet = false;
  bool first_item_block = true;  // a list item's marker goes on its first block only
  auto flush = [&] {
    if (!HasText(text.spans)) {
      text.spans.clear();
      return;
    }
    Normalise(text.spans);
    Block b = base;
    b.spans = std::move(text.spans);
    if (heading) {
      b.kind = BlockKind::kHeading;
      b.level = heading;
    } else if (bullet) {
      b.kind = BlockKind::kListItem;
      b.level = base.kind == BlockKind::kListItem ? base.level + 1 : 0;
      b.number = 0;
      bullet = false;
    } else if (base.kind == BlockKind::kListItem) {
      if (!first_item_block) b.number = -1;  // more of the same item: no marker
      first_item_block = false;
    }
    out.push_back(std::move(b));
    text.spans.clear();
    images_open = false;
  };
  for (Token& t : tokens) {
    switch (t.kind) {
      case Tok::kText:
        if (images_open && !HasText({t.span})) break;  // spaces between pictures
        if (HasText({t.span})) images_open = false;
        text.spans.push_back(std::move(t.span));
        break;
      case Tok::kBreak:
        if (HasText(text.spans)) text.spans.push_back(Span{"\n"});
        break;
      case Tok::kPara:
        flush();
        images_open = false;
        break;
      case Tok::kCell:
        flush();
        break;
      case Tok::kRow:
        flush();
        images_open = false;
        break;
      case Tok::kImage:
        flush();
        if (!images_open || out.empty() || out.back().kind != BlockKind::kImages) {
          Block b;
          b.kind = BlockKind::kImages;
          b.level = base.kind == BlockKind::kListItem ? base.level + 1 : 0;
          out.push_back(std::move(b));
          images_open = true;
        }
        out.back().images.push_back(std::move(t.image));
        break;
      case Tok::kHeadStart:
        flush();
        heading = t.level;
        break;
      case Tok::kHeadEnd:
        flush();
        heading = 0;
        break;
      case Tok::kRule: {
        flush();
        images_open = false;
        Block b;
        b.kind = BlockKind::kRule;
        out.push_back(std::move(b));
        break;
      }
      case Tok::kBullet:
        flush();
        bullet = true;
        break;
    }
  }
  flush();
}

void ParseInto(const std::string& text, const Block& base, const Resolver& resolve, std::vector<Block>& out) {
  std::vector<Token> tokens;
  InlineParser(resolve, tokens).Run(text);
  Flatten(tokens, base, out);
}

std::vector<Span> CellSpans(const std::string& text, const Resolver& resolve) {
  std::vector<Token> tokens;
  InlineParser(resolve, tokens).Run(text);
  std::vector<Span> spans;
  for (Token& t : tokens) {
    if (t.kind == Tok::kText) spans.push_back(std::move(t.span));
    else if (t.kind == Tok::kImage && !t.image.alt.empty()) spans.push_back(Span{t.image.alt, false, false, false, t.image.link});
  }
  Normalise(spans);
  return spans;
}

bool IsFence(const std::string& trimmed, std::string* marker) {
  if (trimmed.size() < 3) return false;
  const char c = trimmed[0];
  if (c != '`' && c != '~') return false;
  size_t n = 0;
  while (n < trimmed.size() && trimmed[n] == c) ++n;
  if (n < 3) return false;
  if (marker) *marker = std::string(n, c);
  return true;
}

bool IsRule(const std::string& trimmed) {
  if (trimmed.empty()) return false;
  const char c = trimmed[0];
  if (c != '-' && c != '*' && c != '_') return false;
  int n = 0;
  for (const char k : trimmed) {
    if (k == c) ++n;
    else if (k != ' ' && k != '\t') return false;
  }
  return n >= 3;
}

bool IsTableSeparator(const std::string& line) {
  const std::string t = Trim(line);
  if (t.find('|') == std::string::npos || t.find('-') == std::string::npos) return false;
  for (const char c : t)
    if (c != '|' && c != '-' && c != ':' && c != ' ' && c != '\t') return false;
  return true;
}

std::vector<std::string> SplitRow(const std::string& line) {
  std::string t = Trim(line);
  if (!t.empty() && t.front() == '|') t.erase(t.begin());
  if (!t.empty() && t.back() == '|' && (t.size() < 2 || t[t.size() - 2] != '\\')) t.pop_back();
  std::vector<std::string> cells;
  std::string cur;
  for (size_t i = 0; i < t.size(); ++i) {
    if (t[i] == '\\' && i + 1 < t.size() && t[i + 1] == '|') {
      cur += '|';
      ++i;
    } else if (t[i] == '|') {
      cells.push_back(Trim(cur));
      cur.clear();
    } else {
      cur += t[i];
    }
  }
  cells.push_back(Trim(cur));
  return cells;
}

// "- item", "* item", "+ item", "1. item", "2) item": indent, number (0 bullet) and the text after.
bool ListItem(const std::string& line, int& indent, int& number, std::string& rest) {
  size_t i = 0;
  while (i < line.size() && line[i] == ' ') ++i;
  indent = int(i);
  if (i >= line.size()) return false;
  if ((line[i] == '-' || line[i] == '*' || line[i] == '+') && i + 1 < line.size() && line[i + 1] == ' ') {
    number = 0;
    rest = line.substr(i + 2);
    return true;
  }
  size_t d = i;
  while (d < line.size() && std::isdigit(static_cast<unsigned char>(line[d])) && d - i < 9) ++d;
  if (d > i && d + 1 < line.size() && (line[d] == '.' || line[d] == ')') && line[d + 1] == ' ') {
    number = std::max(1, std::atoi(line.substr(i, d - i).c_str()));
    rest = line.substr(d + 2);
    return true;
  }
  return false;
}

}  // namespace

std::string DecodeEntities(const std::string& text) {
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '&') {
      if (const size_t n = EntityAt(text, i, out)) {
        i += n - 1;
        continue;
      }
    }
    out += text[i];
  }
  return out;
}

std::string PlainText(const std::vector<Span>& spans) {
  std::string out;
  for (const Span& s : spans) out += s.text;
  return out;
}

std::vector<Block> Parse(const std::string& markdown, const Resolver& resolve) {
  // Lines, with tabs as four spaces and Windows line ends removed.
  std::vector<std::string> lines;
  {
    std::string cur;
    for (const char c : markdown) {
      if (c == '\r') continue;
      if (c == '\n') {
        lines.push_back(std::move(cur));
        cur.clear();
      } else if (c == '\t') {
        cur += "    ";
      } else {
        cur += c;
      }
    }
    lines.push_back(std::move(cur));
  }
  std::vector<Block> out;
  std::string para;
  Block base;
  auto flush = [&] {
    if (!Trim(para).empty()) ParseInto(para, base, resolve, out);
    para.clear();
    base = Block{};
  };
  bool in_comment = false;
  for (size_t i = 0; i < lines.size(); ++i) {
    std::string line = lines[i];
    // HTML comments, also over several lines.
    if (in_comment) {
      const size_t end = line.find("-->");
      if (end == std::string::npos) continue;
      line = line.substr(end + 3);
      in_comment = false;
    }
    for (size_t at; (at = line.find("<!--")) != std::string::npos;) {
      const size_t end = line.find("-->", at + 4);
      if (end == std::string::npos) {
        line = line.substr(0, at);
        in_comment = true;
        break;
      }
      line.erase(at, end + 3 - at);
    }
    const std::string trimmed = Trim(line);
    size_t indent = 0;
    while (indent < line.size() && line[indent] == ' ') ++indent;
    std::string marker;
    if (indent <= 3 && IsFence(trimmed, &marker)) {
      flush();
      Block code;
      code.kind = BlockKind::kCode;
      for (++i; i < lines.size(); ++i) {
        const std::string t = Trim(lines[i]);
        if (t.size() >= marker.size() && t.compare(0, marker.size(), marker) == 0 &&
            t.find_first_not_of(marker[0]) == std::string::npos)
          break;
        if (!code.code.empty()) code.code += '\n';
        code.code += lines[i];
      }
      out.push_back(std::move(code));
      continue;
    }
    if (trimmed.empty()) {
      flush();
      continue;
    }
    if (indent <= 3 && trimmed[0] == '#') {
      size_t n = 0;
      while (n < trimmed.size() && trimmed[n] == '#') ++n;
      if (n <= 6 && (n == trimmed.size() || trimmed[n] == ' ')) {
        flush();
        std::string text = Trim(trimmed.substr(n));
        while (!text.empty() && text.back() == '#') text.pop_back();
        Block h;
        h.kind = BlockKind::kHeading;
        h.level = int(n);
        ParseInto(Trim(text), h, resolve, out);
        continue;
      }
    }
    // Setext headings: a paragraph line underlined with === or ---.
    if (!para.empty() && base.kind == BlockKind::kParagraph && indent <= 3 &&
        trimmed.find_first_not_of(trimmed[0]) == std::string::npos && (trimmed[0] == '=' || trimmed[0] == '-')) {
      Block h;
      h.kind = BlockKind::kHeading;
      h.level = trimmed[0] == '=' ? 1 : 2;
      base = h;
      flush();
      continue;
    }
    if (indent <= 3 && IsRule(trimmed)) {
      flush();
      Block r;
      r.kind = BlockKind::kRule;
      out.push_back(std::move(r));
      continue;
    }
    if (trimmed.find('|') != std::string::npos && i + 1 < lines.size() && IsTableSeparator(lines[i + 1])) {
      flush();
      Block row;
      row.kind = BlockKind::kTableRow;
      row.header = true;
      for (const std::string& cell : SplitRow(line)) row.cells.push_back(CellSpans(cell, resolve));
      out.push_back(row);
      for (i += 2; i < lines.size() && !Trim(lines[i]).empty() && lines[i].find('|') != std::string::npos; ++i) {
        Block r;
        r.kind = BlockKind::kTableRow;
        for (const std::string& cell : SplitRow(lines[i])) r.cells.push_back(CellSpans(cell, resolve));
        out.push_back(std::move(r));
      }
      --i;
      continue;
    }
    int list_indent = 0, number = 0;
    std::string rest;
    if (ListItem(line, list_indent, number, rest) && !(base.kind == BlockKind::kParagraph && !para.empty() && number > 1)) {
      flush();
      base.kind = BlockKind::kListItem;
      base.level = std::min(list_indent / 2, 4);
      base.number = number;
      // Task lists: "[ ] " / "[x] ".
      if (rest.rfind("[ ] ", 0) == 0) rest = "\xE2\x98\x90 " + rest.substr(4);
      else if (rest.rfind("[x] ", 0) == 0 || rest.rfind("[X] ", 0) == 0) rest = "\xE2\x98\x91 " + rest.substr(4);
      para = rest + "\n";
      continue;
    }
    if (indent <= 3 && trimmed[0] == '>') {
      if (base.kind != BlockKind::kQuote) {
        flush();
        base.kind = BlockKind::kQuote;
      }
      std::string q = trimmed.substr(1);
      if (!q.empty() && q[0] == ' ') q.erase(q.begin());
      // GitHub's alerts: > [!NOTE]
      if (q.size() > 3 && q.rfind("[!", 0) == 0 && q.back() == ']') {
        std::string kind = q.substr(2, q.size() - 3);
        for (size_t k = 1; k < kind.size(); ++k) kind[k] = char(std::tolower(static_cast<unsigned char>(kind[k])));
        q = "**" + kind + "**  ";
      }
      para += q + "\n";
      continue;
    }
    para += line + "\n";
  }
  flush();
  return out;
}

}  // namespace kk::workshop::md
