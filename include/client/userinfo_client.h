#pragma once

#include "model/userinfo.h"

#include <memory>
#include <string_view>

namespace zzu_assistant::userinfo {
    class UserInfoClient final {
    public:
        UserInfoClient();
        ~UserInfoClient();
        UserInfoClient(UserInfoClient &&) noexcept;
        UserInfoClient &operator=(UserInfoClient &&) noexcept;
        UserInfoClient(const UserInfoClient &) = delete;
        UserInfoClient &operator=(const UserInfoClient &) = delete;

        [[nodiscard]] model::userinfo::UserInfo parse_sso_redirect(
            std::string_view final_url,
            std::string_view expected_username) const;

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace zzu_assistant::userinfo
