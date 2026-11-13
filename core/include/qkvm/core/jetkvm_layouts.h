#pragma once

#include <vector>

#include <QString>
#include <QStringList>

#include <qkvm/base/result.h>
#include <qkvm/core/strict_json.h>
#include <qkvm/core/typed_keys.h>

// Text typed on a JetKVM in the keyboard layout its computer is set to (the web's W14d: jetkvm.py text_reports and
// _layout). A JetKVM is told at its own page which layout its computer uses, and says it (`getKeyboardLayout`), by
// names such as "de-DE"; the computer turns the keys it is sent into characters by that layout, so a text is pressed
// by that layout's keys. The tables are the web's (app/qkvm/layouts.py, made by its tools/make_layouts.py from Unicode
// CLDR's keyboard data, release 43, under Unicode's licence): a copy is kept in core/data (where it came from is
// written beside it) and made into C++ at build time by core/tools/make_jetkvm_layouts.cpp, the notice with it.
namespace qkvm::core::jetkvm {

inline constexpr const char* usLayout = "en-US";

// The layout a JetKVM said, as the web takes it: nothing ("", a layout never chosen) is US English, as its own page
// takes it; an older JetKVM's "en_US" is "en-US"; what does not look like a layout's name is said in brackets, and has
// no table.
QString layoutOf(const json::Value& said);

// What to press, key by key, to type `text` on a computer set to `layout`: US English by the US table (typed_keys.h),
// any other by its table, a character a dead key makes as that key and the next. Refused ("invalid") before anything
// is typed: a character the layout cannot type (said by its place, never itself: a text may be a password), or a
// layout there is no table for. A "\r\n" is one Enter.
base::Result<std::vector<typed::Report>> textReports(const QString& text, const QString& layout);

// The layouts there are tables for, by the JetKVM's names; and what CLDR calls one (empty: none).
QStringList layouts();
QString titleOf(const QString& layout);

} // namespace qkvm::core::jetkvm
