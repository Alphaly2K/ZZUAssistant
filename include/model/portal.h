#pragma once

#include <string_view>

namespace zzu_assistant::model::portal {
    inline constexpr std::string_view CONNECTIVITY_PROBE_URL =
            "http://bilibili.com";
    inline constexpr std::string_view DEFAULT_SERVER_URL =
            "http://172.16.4.14:801";
    inline constexpr std::string_view CONFIG_SCRIPT_PATH = "/a41.js";
    inline constexpr std::string_view LOGIN_PATH = "/eportal/portal/login?";
    inline constexpr std::string_view LOGOUT_PATH = "/eportal/portal/logout?";
} // namespace zzu_assistant::model::portal
