#pragma once

#include "client/sso_client.h"
#include "client/userinfo_client.h"
#include "service.h"
#include "cli/session_store.h"

namespace zzu_assistant::services {
    class UserInfoService final : public Service {
    public:
        [[nodiscard]] std::string_view description() const noexcept override;
        int execute(ServiceContext &context, Arguments arguments) override;

    private:
        sso::SsoClient sso_client_;
        userinfo::UserInfoClient client_;
        cli::SessionStore sessions_;
    };
} // namespace zzu_assistant::services
