#include "cli/console.h"

#include <cstdlib>
#include <iostream>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace zzu_assistant::cli {
    bool initialize_console() {
#ifdef _WIN32
        // The application and HTTP clients use UTF-8 internally. Configure the
        // attached Windows console before any output or interactive input;
        // color support is detected independently below.
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);

        char *no_color = nullptr;
        std::size_t no_color_size = 0;
        const bool color_disabled =
                _dupenv_s(&no_color, &no_color_size, "NO_COLOR") == 0 &&
                no_color != nullptr;
        std::free(no_color);
        if (color_disabled) {
#else
    if (std::getenv("NO_COLOR") != nullptr) {
#endif
            return false;
        }
#ifdef _WIN32
        if (_isatty(_fileno(stdout)) == 0) {
            return false;
        }
        const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (output == INVALID_HANDLE_VALUE || !GetConsoleMode(output, &mode)) {
            return false;
        }
        return SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
    }

    std::string paint(const std::string_view text, const Tone tone,
                      const bool enabled, const bool bold) {
        if (!enabled) {
            return std::string(text);
        }
        std::string code = bold ? "\x1b[1" : "\x1b[0";
        switch (tone) {
            case Tone::normal: code += ";39m";
                break;
            case Tone::muted: code += ";90m";
                break;
            case Tone::cyan: code += ";36m";
                break;
            case Tone::green: code += ";32m";
                break;
            case Tone::yellow: code += ";33m";
                break;
            case Tone::red: code += ";31m";
                break;
        }
        return code + std::string(text) + "\x1b[0m";
    }

    void status(std::ostream &output, const std::string_view label,
                const std::string_view message, const Tone tone,
                const bool enabled) {
        output << paint("[" + std::string(label) + "]", tone, enabled, true)
                << ' ' << message << '\n';
    }
} // namespace zzu_assistant::cli
