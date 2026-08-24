#pragma once

#include "client/app_client.h"
#include "service.h"

namespace zzu_assistant::services {
    class AppService final : public Service {
    public:
        [[nodiscard]] std::string_view description() const noexcept override;

        int execute(ServiceContext &context, Arguments arguments) override;

    private:
        app::AppClient client_;
    };
} // namespace zzu_assistant::services
