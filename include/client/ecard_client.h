#pragma once

#include "model/ecard.h"
#include "model/electricity.h"

#include <memory>
#include <string_view>

// Client for e-card operations.

namespace zzu_assistant::ecard {
    class EcardClient final {
    public:
        EcardClient();

        ~EcardClient();

        EcardClient(EcardClient &&) noexcept;

        EcardClient &operator=(EcardClient &&) noexcept;

        EcardClient(const EcardClient &) = delete;

        EcardClient &operator=(const EcardClient &) = delete;

        void select_user(std::string_view username);

        void authorize(std::string_view super_app_id_token = {});

        [[nodiscard]] LocationPage locations(std::string_view location_type,
                                             const LocationPath &path);

        [[nodiscard]] ElectricityReading account(const LocationPath &path);

        [[nodiscard]] RechargeResult recharge(const LocationPath &path,
                                              std::string_view payment_password,
                                              unsigned amount_yuan);

        [[nodiscard]] CampusCardRechargeConfig campus_card_recharge_config();

        [[nodiscard]] CampusCardRechargeOrder campus_card_recharge(
            unsigned amount_yuan);

        [[nodiscard]] bool load_profiles(ElectricityProfiles &profiles) const;

        void save_profiles(const ElectricityProfiles &profiles) const;

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace zzu_assistant::ecard
