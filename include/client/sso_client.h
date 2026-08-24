#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// CAS SSO client for ZZU's CAS server, supporting MFA and session persistence.
// Less ability supported than the app client, soon will be deprecated.

namespace zzu_assistant::sso {
    enum class LoginStatus {
        success,
        no_session,
        rejected,
        captcha_required,
        mfa_rejected,
        mfa_expired,
        protocol_error,
        network_error,
    };

    enum class MfaMethod {
        secure_phone,
        qr_code,
    };

    struct LoginOptions {
        std::string_view username;
        std::string_view password;
        std::string_view service_url;
        MfaMethod mfa_method{MfaMethod::secure_phone};
        std::chrono::seconds mfa_timeout{120};
        std::function<void(std::string_view)> notify;
        std::function<std::string(std::string_view)> prompt;
        std::function<void(const std::vector<unsigned char> &)> display_qr;
    };

    struct LoginResult {
        LoginStatus status{LoginStatus::protocol_error};
        std::string message;
        std::string final_url;

        [[nodiscard]] bool succeeded() const noexcept {
            return status == LoginStatus::success;
        }
    };

    struct CachedUserInfo {
        std::string username;
        std::string final_url;
        std::int64_t updated_at{};
    };

    class SsoClient final {
    public:
        SsoClient();

        ~SsoClient();

        SsoClient(SsoClient &&) noexcept;

        SsoClient &operator=(SsoClient &&) noexcept;

        SsoClient(const SsoClient &) = delete;

        SsoClient &operator=(const SsoClient &) = delete;

        LoginResult probe();

        LoginResult resume(std::string_view username,
                           std::string_view service_url = {});

        LoginResult login(const LoginOptions &options);

        LoginResult logout(std::string_view username = {});

        [[nodiscard]] std::optional<CachedUserInfo> current_user() const;

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace zzu_assistant::sso
