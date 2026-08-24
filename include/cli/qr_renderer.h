#pragma once

#include <iosfwd>
#include <string_view>
#include <vector>

namespace zzu_assistant::cli {
    // Decodes a PNG QR image and draws square terminal cells. ANSI background
    // colors keep the code readable on both light and dark terminal themes.
    void render_qr_png(std::ostream &output,
                       const std::vector<unsigned char> &png,
                       bool ansi_enabled);

    // Encodes arbitrary text locally and uses a compact half-block terminal
    // representation, which keeps long payment URLs within typical widths.
    void render_qr_text(std::ostream &output, std::string_view text,
                        bool ansi_enabled);
} // namespace zzu_assistant::cli
