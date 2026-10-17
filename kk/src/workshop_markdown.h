// Job J8: a small Markdown subset for the Workshop's README view, as GitHub READMEs use it: headings,
// paragraphs, **bold** / *italic* / `code`, lists, links, pictures, fenced code blocks, simple tables and
// quotes, plus the HTML that READMEs mix in (<img>, <a>, <b>, <br>, <h1>..<h6>, <table> cells, <div>
// and <p> as paragraph breaks; other tags are left out, entities decoded). No window needed: unit tested.

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace kk::workshop::md {

struct Span {
  std::string text;  // "\n" is a line break
  bool bold = false, italic = false, code = false;
  std::string link;  // the address it opens ("" for plain text)
};

struct Image {
  std::string src, alt;
  std::string link;     // a picture inside a link (a badge's link)
  int width_px = 0;     // <img width="260">
  int width_pct = 0;    // <img width="100%">
};

enum class BlockKind { kHeading, kParagraph, kListItem, kQuote, kCode, kImages, kRule, kTableRow };

struct Block {
  BlockKind kind = BlockKind::kParagraph;
  int level = 0;    // heading 1..6; list nesting 0..
  int number = 0;   // an ordered list item's number (0: a bullet)
  bool header = false;  // a table's first row
  std::vector<Span> spans;
  std::vector<Image> images;              // kImages: side by side
  std::string code;                       // kCode
  std::vector<std::vector<Span>> cells;   // kTableRow
};

// Makes a README's address absolute (picture = true for <img> / ![]()); "" drops a link (anchors).
using Resolver = std::function<std::string(const std::string& url, bool picture)>;

std::vector<Block> Parse(const std::string& markdown, const Resolver& resolve = nullptr);

// HTML entities (&amp; &lt; &#39; &#x2014; ...) to UTF-8.
std::string DecodeEntities(const std::string& text);

// Plain text of spans (for tests and logs).
std::string PlainText(const std::vector<Span>& spans);

}  // namespace kk::workshop::md
