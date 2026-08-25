#pragma once

#include <ostream>
#include <string>
#include <string_view>

namespace zzu_assistant::cli {
    enum class Tone { normal, muted, cyan, green, yellow, red };

    [[nodiscard]] bool initialize_console();

    [[nodiscard]] std::string paint(std::string_view text, Tone tone,
                                    bool enabled, bool bold = false);

    void status(std::ostream &output, std::string_view label,
                std::string_view message, Tone tone, bool enabled);

    void heading(std::ostream &output, std::string_view title,
                 std::string_view subtitle, bool enabled);

    void field(std::ostream &output, std::string_view label,
               std::string_view value, bool enabled);

    void prompt(std::ostream &output, std::string_view message,
                std::string_view hint, bool enabled);
} // namespace zzu_assistant::cli
