#pragma once
#if defined(KK_DEV_TOOLS)
#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

namespace kk::coop::world {
// Small diagnostic JSON writer. Explicit fields only, never RAM/padding/pointers.
struct Json {
  std::string text;
  Json() : text("null") {}
  Json(bool v) : text(v ? "true" : "false") {}
  template<class T> requires(std::is_integral_v<T> && !std::is_same_v<T,bool>)
  Json(T v) : text(std::to_string(v)) {}
  Json(double v) {
    if (!std::isfinite(v)) { text="null"; return; }
    if (v==0) v=0; // Canonical negative zero.
    char b[64]; auto r=std::to_chars(b,b+64,v,std::chars_format::general);
    text.assign(b,r.ptr);
  }
  Json(float v) : Json(double(v)) {} // Preserve the native binary32 value exactly.
  Json(const char* v) : Json(std::string(v)) {}
  Json(const std::string& v) {
    text="\"";
    for (unsigned char c:v) {
      if (c=='"' || c=='\\') { text+='\\'; text+=char(c); }
      else if (c<32) { constexpr char hex[]="0123456789abcdef"; text+="\\u00"; text+=hex[c>>4]; text+=hex[c&15]; }
      else text+=char(c);
    }
    text+='"';
  }
  static Json Object(const std::map<std::string,Json>& values) {
    Json j; j.text="{"; bool first=true;
    for (const auto& [k,v]:values) { if (!first) j.text+=','; first=false; j.text+=Json(k).text+":"+v.text; }
    j.text+='}'; return j;
  }
  static Json Array(const std::vector<Json>& values) {
    Json j; j.text="["; bool first=true;
    for (const auto& v:values) { if (!first) j.text+=','; first=false; j.text+=v.text; }
    j.text+=']'; return j;
  }
};
inline std::string Hex(uint64_t v) {
  char b[17]; auto r=std::to_chars(b,b+16,v,16);
  return std::string(16-(r.ptr-b),'0')+std::string(b,r.ptr);
}
struct Capture {
  std::vector<std::string> errors;
  void Error(std::string s) { errors.push_back(std::move(s)); }
};
// Native reads only. Binding identities are metadata, never cached authority.
Json ObjectSnapshot(const uint8_t* base,Capture& capture);
Json EnemySnapshot(const uint8_t* base,Capture& capture);
Json StorySnapshot(const uint8_t* base,Capture& capture);
Json BodySnapshot(const uint8_t* base,Capture& capture);
void Frame(uint8_t* base);
void Loaded(const uint8_t* base);
bool RouteStopped();
uint32_t ConfirmedShots();
}
#endif
