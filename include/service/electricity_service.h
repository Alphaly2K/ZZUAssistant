#pragma once

#include "client/ecard_client.h"
#include "client/app_client.h"
#include "service.h"
#include "cli/session_store.h"

namespace zzu_assistant::services {
    class ElectricityService final : public Service {
    public:
        [[nodiscard]] std::string_view description() const noexcept override;

        int execute(ServiceContext &context, Arguments arguments) override;

    private:
        ecard::EcardClient client_;
        app::AppClient app_state_;
        cli::SessionStore sessions_;
    };
} // namespace zzu_assistant::services
