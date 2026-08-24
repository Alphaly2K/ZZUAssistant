#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace zzu_assistant::ecard {
    inline constexpr std::string_view PAYMENT_PASSWORD_ENVIRONMENT =
            "ZZUASSISTANT_ECARD_PAYMENT_PASSWORD";

    struct LocationPath {
        std::string big_area;
        std::string area;
        std::string building;
        std::string unit;
        std::string level;
        std::string room;
        std::string sub_area;
    };

    struct LocationOption {
        std::string id;
        std::string name;
    };

    struct LocationPage {
        std::string location_type;
        std::string next_location_type;
        bool end{false};
        std::vector<LocationOption> options;
    };

    struct ElectricityReading {
        double quantity_kwh{};
        double price_yuan_per_kwh{};
    };

    struct RechargeResult {
        bool success{false};
        std::string message;
    };

    struct ElectricityProfiles {
        LocationPath lighting;
        LocationPath air_conditioning;
    };
} // namespace zzu_assistant::ecard
