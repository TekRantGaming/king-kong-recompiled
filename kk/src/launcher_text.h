// The launcher's text in the game's languages: it follows the Language
// setting (user_language): English, German, Spanish, French or Italian.
// Text is looked up by its English wording (the launcher's code and the UI
// toolkit pass English and translate as they draw); anything without a
// translation stays in English.

#pragma once

#include <initializer_list>
#include <string>

namespace kk::text {

enum class Lang { kEnglish, kGerman, kSpanish, kFrench, kItalian };
Lang Current();

const char* T(const char* english);
std::string T(const std::string& english);

// Translates a template, then fills in {0}, {1}... (already translated or
// numbers), e.g. F("{0} of {1} unlocked", {"3", "9"}).
std::string F(const char* english, std::initializer_list<std::string> args);

// Upper case, including accented Latin letters (é -> É).
std::string Upper(std::string utf8);

}  // namespace kk::text
