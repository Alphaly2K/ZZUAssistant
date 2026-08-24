#pragma once

#include <string>
#include <vector>

namespace zzu_assistant::ecard {
    struct CampusCardRechargeConfig {
        double balance_yuan{};
        std::vector<unsigned> amounts_yuan;
    };

    struct CampusCardRechargeOrder {
        unsigned amount_yuan{};
        std::string checkout_url;
    };
} // namespace zzu_assistant::ecard
