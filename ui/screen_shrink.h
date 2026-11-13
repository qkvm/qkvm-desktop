#pragma once

#include <optional>

#include <QByteArray>

namespace qkvm::ui {

// A JPEG as this PC's screen watches compare it (core::ScreenWatch::fingerprint, c7): decoded by Qt, in whatever
// thread calls it (the pool's); nothing where it is no JPEG, or larger than a 4K screen by what it says of itself
// before a pixel of it is decoded. Core has no pictures of Qt's: the program hands it this (ScreenWatch::shrinkWith).
std::optional<QByteArray> shrunkScreen(const QByteArray& jpeg);

} // namespace qkvm::ui
