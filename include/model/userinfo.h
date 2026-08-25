#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace zzu_assistant::model::userinfo {
    inline constexpr std::string_view PORTAL_HOST = "info.s.zzu.edu.cn";
    inline constexpr std::string_view SSO_ISSUER = "https://cas.s.zzu.edu.cn/cas";
    inline constexpr std::string_view IDENTITY_ISSUER = "cas.s.zzu.edu.cn";
    inline constexpr std::string_view SERVICE_URL =
            "https://info.s.zzu.edu.cn/?path=https%3A%2F%2Finfo.s.zzu.edu.cn%2Fmain.html%23%2F";

    struct UserInfo {
        std::string username;
        std::string name;
        std::string identity_type_code;
        std::string identity_type_name;
        std::string organization_code;
        std::string organization_name;
        std::string account_id;
        std::string user_id;
        std::string uid;
        std::int64_t expires_at{};
    };
} // namespace zzu_assistant::model::userinfo
