#include "cli/qr_renderer.h"

#include "third_party/qrcodegen.hpp"

#include <png.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <optional>
#include <stdexcept>
#include <vector>

namespace zzu_assistant::cli {
    namespace {
        struct Image {
            std::size_t width{};
            std::size_t height{};
            std::vector<unsigned char> rgba;
        };

        Image decode_png(const std::vector<unsigned char> &bytes) {
            if (bytes.empty()) {
                throw std::runtime_error("The SSO server returned an empty QR image");
            }
            png_image image{};
            image.version = PNG_IMAGE_VERSION;
            if (!png_image_begin_read_from_memory(&image, bytes.data(), bytes.size())) {
                throw std::runtime_error("The SSO QR image is not a valid PNG");
            }
            image.format = PNG_FORMAT_RGBA;
            Image result{
                image.width, image.height,
                std::vector<unsigned char>(PNG_IMAGE_SIZE(image))
            };
            if (!png_image_finish_read(&image, nullptr, result.rgba.data(), 0,
                                       nullptr)) {
                const std::string message = image.message;
                png_image_free(&image);
                throw std::runtime_error("Cannot decode the SSO QR image: " + message);
            }
            png_image_free(&image);
            return result;
        }

        bool is_black(const Image &image, const std::size_t x, const std::size_t y) {
            const std::size_t offset = (y * image.width + x) * 4;
            const unsigned luminance =
                    (299U * image.rgba[offset] + 587U * image.rgba[offset + 1] +
                     114U * image.rgba[offset + 2]) /
                    1000U;
            return image.rgba[offset + 3] >= 128 && luminance < 150;
        }

        struct ModuleGrid {
            std::size_t symbol_size{};
            std::vector<bool> modules;
        };

        bool expected_finder_module(const std::size_t x, const std::size_t y) {
            return x == 0 || x == 6 || y == 0 || y == 6 ||
                   (x >= 2 && x <= 4 && y >= 2 && y <= 4);
        }

        bool sample(const Image &image, const double x, const double y) {
            return is_black(image,
                            std::min(static_cast<std::size_t>(std::max(0.0, x)),
                                     image.width - 1),
                            std::min(static_cast<std::size_t>(std::max(0.0, y)),
                                     image.height - 1));
        }

        double finder_score(const Image &image, const double origin_x,
                            const double origin_y, const double pitch,
                            const std::size_t symbol_size) {
            std::size_t correct = 0;
            std::size_t total = 0;
            const auto score_one = [&](const double finder_x,
                                       const double finder_y) {
                for (std::size_t y = 0; y < 7; ++y) {
                    for (std::size_t x = 0; x < 7; ++x) {
                        const bool actual = sample(
                            image, finder_x + (x + 0.5) * pitch,
                            finder_y + (y + 0.5) * pitch);
                        correct += actual == expected_finder_module(x, y) ? 1U : 0U;
                        ++total;
                    }
                }
            };
            score_one(origin_x, origin_y);
            score_one(origin_x + (symbol_size - 7) * pitch, origin_y);
            score_one(origin_x, origin_y + (symbol_size - 7) * pitch);
            return static_cast<double>(correct) / static_cast<double>(total);
        }

        ModuleGrid extract_modules(const Image &image) {
            std::size_t top = image.height;
            std::size_t left = image.width;
            for (std::size_t y = 0; y < image.height && top == image.height; ++y) {
                for (std::size_t x = 0; x < image.width; ++x) {
                    if (is_black(image, x, y)) {
                        top = y;
                        left = x;
                        break;
                    }
                }
            }
            if (top == image.height) {
                throw std::runtime_error("The SSO QR image contains no dark modules");
            }
            std::size_t finder_run = 0;
            while (left + finder_run < image.width &&
                   is_black(image, left + finder_run, top)) {
                ++finder_run;
            }
            if (finder_run < 7) {
                throw std::runtime_error("Cannot locate the QR finder pattern");
            }
            const double pitch = static_cast<double>(finder_run) / 7.0;

            std::optional<std::size_t> best_size;
            double best_score = 0.0;
            // ISO/IEC 18004 QR versions have 21 + 4 * (version - 1) modules.
            for (std::size_t size = 21; size <= 177; size += 4) {
                if (left + size * pitch > image.width + pitch ||
                    top + size * pitch > image.height + pitch) {
                    break;
                }
                const double score = finder_score(image, left, top, pitch, size);
                if (score > best_score) {
                    best_score = score;
                    best_size = size;
                }
            }
            if (!best_size || best_score < 0.90) {
                throw std::runtime_error(
                    "Cannot infer the QR module grid from the SSO image");
            }

            ModuleGrid grid{
                *best_size,
                std::vector<bool>(*best_size * *best_size, false)
            };
            for (std::size_t module_y = 0; module_y < *best_size; ++module_y) {
                for (std::size_t module_x = 0; module_x < *best_size; ++module_x) {
                    const std::size_t x0 = static_cast<std::size_t>(
                        std::floor(left + module_x * pitch));
                    const std::size_t x1 = std::max(x0 + 1,
                                                    static_cast<std::size_t>(std::ceil(
                                                        left + (module_x + 1) * pitch)));
                    const std::size_t y0 = static_cast<std::size_t>(
                        std::floor(top + module_y * pitch));
                    const std::size_t y1 = std::max(y0 + 1,
                                                    static_cast<std::size_t>(std::ceil(
                                                        top + (module_y + 1) * pitch)));
                    std::size_t dark = 0;
                    std::size_t total = 0;
                    for (std::size_t y = y0; y < std::min(y1, image.height); ++y) {
                        for (std::size_t x = x0; x < std::min(x1, image.width); ++x) {
                            dark += is_black(image, x, y) ? 1U : 0U;
                            ++total;
                        }
                    }
                    grid.modules[module_y * *best_size + module_x] =
                            total != 0 && dark * 2 >= total;
                }
            }
            return grid;
        }
    } // namespace

    void render_qr_png(std::ostream &output,
                       const std::vector<unsigned char> &png,
                       const bool ansi_enabled) {
        const Image image = decode_png(png);
        if (image.width == 0 || image.height == 0) {
            throw std::runtime_error("The SSO QR image has invalid dimensions");
        }

        const ModuleGrid grid = extract_modules(image);
        // Terminal cells are visually large (two character columns per module).
        // Two cells retain a clear separator while avoiding an oversized frame.
        constexpr std::size_t quiet_zone = 2;
        const std::size_t cells = grid.symbol_size + quiet_zone * 2;
        output << '\n';
        for (std::size_t cell_y = 0; cell_y < cells; ++cell_y) {
            bool previous_black = false;
            bool have_previous = false;
            for (std::size_t cell_x = 0; cell_x < cells; ++cell_x) {
                const bool inside = cell_x >= quiet_zone && cell_y >= quiet_zone &&
                                    cell_x < quiet_zone + grid.symbol_size &&
                                    cell_y < quiet_zone + grid.symbol_size;
                const bool current_black = inside && grid.modules[
                                               (cell_y - quiet_zone) * grid.symbol_size +
                                               (cell_x - quiet_zone)];
                if (ansi_enabled && (!have_previous || current_black != previous_black)) {
                    output << (current_black ? "\x1b[40m" : "\x1b[47m");
                }
                if (ansi_enabled) {
                    output << "  ";
                } else {
                    output << (current_black ? "##" : "  ");
                }
                previous_black = current_black;
                have_previous = true;
            }
            if (ansi_enabled) {
                output << "\x1b[0m";
            }
            output << '\n';
        }
        output << '\n';
    }

    void render_qr_text(std::ostream &output, const std::string_view text,
                        const bool ansi_enabled) {
        if (text.empty())
            throw std::invalid_argument(
                "Cannot render an empty QR payload");
        const std::string payload(text);
        const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(
            payload.c_str(), qrcodegen::QrCode::Ecc::LOW);
        constexpr int quiet_zone = 2;
        const int cells = qr.getSize() + quiet_zone * 2;
        const auto dark = [&](const int x, const int y) {
            return x >= quiet_zone && y >= quiet_zone &&
                   x < quiet_zone + qr.getSize() &&
                   y < quiet_zone + qr.getSize() &&
                   qr.getModule(x - quiet_zone, y - quiet_zone);
        };

        output << '\n';
        for (int y = 0; y < cells; y += 2) {
            if (ansi_enabled) output << "\x1b[30;47m";
            for (int x = 0; x < cells; ++x) {
                const bool top = dark(x, y);
                const bool bottom = y + 1 < cells && dark(x, y + 1);
                if (top && bottom) output << "█";
                else if (top) output << "▀";
                else if (bottom) output << "▄";
                else output << ' ';
            }
            if (ansi_enabled) output << "\x1b[0m";
            output << '\n';
        }
        output << '\n';
    }
} // namespace zzu_assistant::cli
