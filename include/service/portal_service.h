#pragma once

#include "service.h"
#include "client/portal_client.h"

namespace zzu_assistant::services {
    class PortalService final : public Service {
    public:
        [[nodiscard]] std::string_view description() const noexcept override;

        int execute(ServiceContext &context, Arguments arguments) override;

    private:
        portal::PortalClient client_;
    };
} // namespace zzu_assistant::services
