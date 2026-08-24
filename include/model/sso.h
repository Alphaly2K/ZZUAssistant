#pragma once

#include <string_view>

namespace zzu_assistant::model::sso {
    inline constexpr std::string_view URL_BASE = "https://cas.s.zzu.edu.cn";
    inline constexpr std::string_view LOGIN_URL = "https://cas.s.zzu.edu.cn/cas/a/login";
    inline constexpr std::string_view PUBKEY_URL = "https://cas.s.zzu.edu.cn/cas/jwt/publicKey";
    inline constexpr std::string_view MFA_DETECT_URL =
            "https://cas.s.zzu.edu.cn/cas/mfa/detect";
    inline constexpr std::string_view MFA_INIT_BY_TYPE_URL =
            "https://cas.s.zzu.edu.cn/cas/mfa/initByType/";
    inline constexpr std::string_view LOGOUT_URL =
            "https://cas.s.zzu.edu.cn/cas/logout";

    // CAS supplies the attestation origin; only its fixed paths live here.
    inline constexpr std::string_view ATTEST_GUARD_PATH = "/api/guard/";
    inline constexpr std::string_view ATTEST_SEND_PATH = "/send";
    inline constexpr std::string_view PHONE_VALID_PATH =
            "/api/guard/securephone/valid";
    inline constexpr std::string_view QR_STATUS_PATH =
            "/api/guard/qrcode/status";
} // namespace zzu_assistant::model::sso
