#pragma once

#include <cassert>
#include <string>
#include <utility>
#include <variant>

namespace qkvm::base {

// What went wrong, as a value. `code` is for the program and never changes its spelling ("busy",
// "not_found": the server's own where the server said it). `message` is a sentence written to be
// shown to a person as it is.
struct Error {
    std::string code;
    std::string message;
};

// What a function that can fail gives back: the thing, or why not. Our own code throws nothing
// (DESIGN.md section 7), so this is how a failure travels. Asking for the value of a failure, or for
// the error of a success, is a mistake in the program and stops a debug build.
template <typename T>
class [[nodiscard]] Result {
public:
    Result(T value) : m_state(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : m_state(std::in_place_index<1>, std::move(error)) {}

    bool ok() const noexcept { return m_state.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    const T& value() const& noexcept
    {
        assert(ok());
        return *std::get_if<0>(&m_state);
    }
    T& value() & noexcept
    {
        assert(ok());
        return *std::get_if<0>(&m_state);
    }
    T&& value() && noexcept
    {
        assert(ok());
        return std::move(*std::get_if<0>(&m_state));
    }

    const Error& error() const noexcept
    {
        assert(!ok());
        return *std::get_if<1>(&m_state);
    }

private:
    std::variant<T, Error> m_state;
};

// The same for a function that gives nothing back when it succeeds.
template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : m_error(std::in_place_index<1>, std::move(error)) {}

    bool ok() const noexcept { return m_error.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    const Error& error() const noexcept
    {
        assert(!ok());
        return *std::get_if<1>(&m_error);
    }

private:
    std::variant<std::monostate, Error> m_error;
};

} // namespace qkvm::base
