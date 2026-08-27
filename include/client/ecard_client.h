#pragma once

#include "model/ecard.h"
#include "model/electricity.h"
#include "client/options.h"

#include <memory>
#include <string>
#include <string_view>

// Client for e-card operations.

namespace zzu_assistant::ecard {
    struct Session {
        std::string username;
        std::string access_token;
        std::string refresh_token;
        std::string access_token_expire;
        ElectricityProfiles profiles;
    };

    class EcardClient final {
    public:
        explicit EcardClient(NetworkOptions options = {});

        ~EcardClient();

        EcardClient(EcardClient &&) noexcept;

        EcardClient &operator=(EcardClient &&) noexcept;

        EcardClient(const EcardClient &) = delete;

        EcardClient &operator=(const EcardClient &) = delete;

        void login(std::string_view username,
                   std::string_view super_app_id_token);

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

        [[nodiscard]] Session session() const;

        void login(const Session &session);

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace zzu_assistant::ecard
