#include "cli/console.h"
#include "registry.h"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
    std::string_view executable_leaf(const std::string_view value) {
        const std::size_t separator = value.find_last_of("/\\");
        return separator == std::string_view::npos ? value : value.substr(separator + 1);
    }
}

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {
    std::string utf8_argument(const std::wstring_view value) {
        if (value.empty()) return {};
        const int size = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (size <= 0) return {};
        std::string result(static_cast<std::size_t>(size), '\0');
        if (WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                static_cast<int>(value.size()), result.data(), size,
                nullptr, nullptr) <= 0)
            return {};
        return result;
    }
}

int wmain(const int argc, wchar_t *wide_argv[]) {
    std::vector<std::string> owned_arguments;
    owned_arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index)
        owned_arguments.push_back(utf8_argument(wide_argv[index]));

    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index)
        arguments.emplace_back(owned_arguments[static_cast<std::size_t>(index)]);

    const std::string_view executable_name = executable_leaf(
        argc > 0
            ? std::string_view(owned_arguments.front())
            : std::string_view("ZZUAssistant"));
#else
int main(const int argc, char *argv[]) {
    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));

    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    const std::string_view executable_name = executable_leaf(
            argc > 0 ? std::string_view(argv[0]) : std::string_view("ZZUAssistant"));
#endif
    zzu_assistant::ServiceContext context{
        std::cout, std::cerr, executable_name,
        zzu_assistant::cli::initialize_console()
    };
    zzu_assistant::Registry registry;
    return registry.dispatch(context, arguments);
}
