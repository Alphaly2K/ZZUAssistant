#pragma once

#include "service.h"

namespace zzu_assistant::services {
    class CourseService final : public Service {
    public:
        [[nodiscard]] std::string_view description() const noexcept override;

        int execute(ServiceContext &context, Arguments arguments) override;
    };
} // namespace zzu_assistant::services
