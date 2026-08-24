#pragma once

#include "service.h"
#include "client/sso_client.h"
#include "model/constants.h"

namespace zzu_assistant::services {
    class SSOService final : public Service {
    public:
        [[nodiscard]] std::string_view description() const noexcept override;

        int execute(ServiceContext &context, Arguments arguments) override;

    private:
        sso::SsoClient client_;
    };
} // namespace zzu_assistant::services
