#pragma once

#include <ostream>
#include <span>
#include <string_view>

namespace zzu_assistant {
    using Arguments = std::span<const std::string_view>;

    struct ServiceContext {
        std::ostream &out;
        std::ostream &err;
        std::string_view executable_name;
        bool color_enabled{false};
    };

    class Service {
    public:
        virtual ~Service() = default;

        [[nodiscard]] virtual std::string_view description() const noexcept = 0;

        virtual int execute(ServiceContext &context, Arguments arguments) = 0;
    };
} // namespace zzu_assistant
