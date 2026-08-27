#pragma once

#include "service.h"
#include "client/portal_client.h"
#include "cli/session_store.h"

namespace zzu_assistant::services {
    class PortalService final : public Service {
    public:
        [[nodiscard]] std::string_view description() const noexcept override;

        int execute(ServiceContext &context, Arguments arguments) override;

    private:
        portal::PortalClient client_;
        cli::SessionStore sessions_;
    };
} // namespace zzu_assistant::services
