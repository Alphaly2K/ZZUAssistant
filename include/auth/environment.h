#pragma once

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace zzu_assistant::auth {
    inline constexpr std::string_view USER_ENV = "ZZUASSISTANT_USER";
    inline constexpr std::string_view STATE_DIR_ENV = "ZZUASSISTANT_STATE_DIR";
    inline constexpr std::string_view SSO_TOKEN_ENV = "ZZUASSISTANT_SSO_TOKEN";
    inline constexpr std::string_view APP_TOKEN_ENV = "ZZUASSISTANT_APP_TOKEN";
    inline constexpr std::string_view ECARD_TOKEN_ENV = "ZZUASSISTANT_ECARD_TOKEN";
    inline constexpr std::string_view ECARD_REFRESH_TOKEN_ENV =
            "ZZUASSISTANT_ECARD_REFRESH_TOKEN";

    [[nodiscard]] inline std::optional<std::string> environment(
        const std::string_view name) {
        if (name.empty()) return std::nullopt;
#ifdef _WIN32
        char *value = nullptr;
        std::size_t size = 0;
        const std::string key(name);
        if (_dupenv_s(&value, &size, key.c_str()) != 0 || value == nullptr)
            return std::nullopt;
        std::string result(value);
        std::free(value);
#else
        const std::string key(name);
        const char *value = std::getenv(key.c_str());
        if (value == nullptr) return std::nullopt;
        std::string result(value);
#endif
        if (result.empty()) return std::nullopt;
        return result;
    }

    inline void validate_header_value(const std::string_view value,
                                      const std::string_view variable) {
        if (value.empty())
            throw std::runtime_error(std::string(variable) + " is empty");
        for (const unsigned char ch: value)
            if (ch == '\r' || ch == '\n' || ch == 0)
                throw std::runtime_error(std::string(variable) +
                                         " contains invalid control characters");
    }

    inline void validate_jwt(const std::string_view value,
                             const std::string_view variable) {
        validate_header_value(value, variable);
        if (value.find_first_of(" \t") != std::string_view::npos ||
            value.find('.') == std::string_view::npos ||
            value.find('.', value.find('.') + 1) == std::string_view::npos)
            throw std::runtime_error(std::string(variable) +
                                     " must contain a JWT without whitespace");
    }

    [[nodiscard]] inline std::optional<std::string> current_user_override() {
        return environment(USER_ENV);
    }

    [[nodiscard]] inline std::string resolve_username(
        const std::string_view explicit_user,
        const std::optional<std::string> &cached_user,
        const std::string_view authentication_name) {
        if (!explicit_user.empty()) return std::string(explicit_user);
        if (const auto configured = current_user_override()) return *configured;
        if (cached_user && !cached_user->empty()) return *cached_user;
        throw std::runtime_error("No current " + std::string(authentication_name) +
                                 " user; provide a username or set " +
                                 std::string(USER_ENV));
    }

    [[nodiscard]] inline std::filesystem::path state_directory() {
        if (const auto configured = environment(STATE_DIR_ENV)) return *configured;
#ifdef _WIN32
        if (const auto local = environment("LOCALAPPDATA"))
            return std::filesystem::path(*local) / "ZZUAssistant";
#elif defined(__APPLE__)
        if (const auto home = environment("HOME"))
            return std::filesystem::path(*home) / "Library" /
                   "Application Support" / "ZZUAssistant";
#else
        if (const auto state = environment("XDG_STATE_HOME"))
            return std::filesystem::path(*state) / "zzu-assistant";
        if (const auto home = environment("HOME"))
            return std::filesystem::path(*home) / ".local/state/zzu-assistant";
#endif
        throw std::runtime_error("Cannot determine the ZZUAssistant state directory");
    }
} // namespace zzu_assistant::auth
