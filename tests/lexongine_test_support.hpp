#pragma once

#include "lexongine/model.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace lexongine::test {
inline void require(bool condition, std::string_view reason) {
    if (!condition) { throw std::runtime_error(std::string{reason}); }
}
template<class T> T take(std::variant<T, Diagnostic> result) {
    if (const auto* error = std::get_if<Diagnostic>(&result)) {
        throw std::runtime_error(std::string{error->text()});
    }
    return std::get<T>(std::move(result));
}
inline void success(Result<void> result) {
    if (const auto* error = std::get_if<Diagnostic>(&result)) {
        throw std::runtime_error(std::string{error->text()});
    }
}
template<class T> void rejects(std::variant<T, Diagnostic> result, FailureCode code) {
    const auto* error = std::get_if<Diagnostic>(&result);
    require(error != nullptr, "expected a diagnostic, received success");
    require(error->code == code, "wrong rejection code");
}
struct Case { const char* name; void (*run)(); };
template<std::size_t N> int run(const Case (&cases)[N]) {
    std::size_t failed = 0;
    for (const auto& entry : cases) {
        try { entry.run(); std::cout << "PASS " << entry.name << '\n'; }
        catch (const std::exception& error) {
            ++failed; std::cerr << "FAIL " << entry.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (N - failed) << '/' << N << " cases passed\n";
    return failed == 0 ? 0 : 1;
}
} // namespace lexongine::test
