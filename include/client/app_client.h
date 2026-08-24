#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// New authentication client using the super app authentication method.

namespace zzu_assistant::app {
    struct LoginOptions {
        std::string_view username;
        std::string_view password;
        std::function<void(std::string_view)> notify;
        std::function<std::string(std::string_view)> prompt;
    };

    struct LoginResult {
        bool success{false};
        std::string message;
    };

    class AppClient final {
    public:
        AppClient();

        ~AppClient();

        AppClient(AppClient &&) noexcept;

        AppClient &operator=(AppClient &&) noexcept;

        AppClient(const AppClient &) = delete;

        AppClient &operator=(const AppClient &) = delete;

        [[nodiscard]] LoginResult login(const LoginOptions &options);

        [[nodiscard]] LoginResult logout(std::string_view username = {});

        [[nodiscard]] std::optional<std::string> current_user() const;

        [[nodiscard]] std::string id_token(std::string_view username) const;

        [[nodiscard]] double card_balance(std::string_view username) const;

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace zzu_assistant::app
