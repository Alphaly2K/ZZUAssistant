#pragma once

#include <string_view>

namespace zzu_assistant::model::app {
    inline constexpr std::string_view SUPER_APP_AUTH_HOST =
            "token.s.zzu.edu.cn";
    inline constexpr std::string_view SUPER_APP_AUTH_BASE_URL =
            "https://token.s.zzu.edu.cn";
    // Hard-coded user-agent string from the SuperApp iOS app. Modify it as you wish.
    inline constexpr std::string_view SUPER_APP_LOGIN_UA =
            "SWSuperApp/2.5.2 (iPhone; iOS 18.7; Scale/3.00)";
    // Hard-coded app ID from the SuperApp iOS app. Modify it as you wish.
    inline constexpr std::string_view SUPER_APP_ID =
            "com.lantu.MobileCampus.zzu";
    // Modify it as you wish.
    inline constexpr std::string_view SUPER_APP_CLIENT_ID = "CLIENT_ID";
    // Modify it as you wish.
    inline constexpr std::string_view SUPER_APP_OS_TYPE = "iOS";
    inline constexpr std::string_view SUPER_APP_DEVICE_ID_ENVIRONMENT =
            "ZZUASSISTANT_APP_DEVICE_ID";
    inline constexpr std::string_view SUPER_APP_PUBLIC_KEY_TARGET =
            "/jwt/publicKey";
    inline constexpr std::string_view SUPER_APP_MFA_DETECT_TARGET =
            "/mfa/detect";
    inline constexpr std::string_view SUPER_APP_MFA_PHONE_INIT_TARGET =
            "/mfa/initByType/securephone";
    inline constexpr std::string_view SUPER_APP_PASSWORD_LOGIN_TARGET =
            "/password/passwordLogin";
    inline constexpr std::string_view SUPER_APP_MFA_ATTEST_FALLBACK =
            "https://cas.s.zzu.edu.cn/attest";
    inline constexpr std::string_view SUPER_APP_MFA_PHONE_SEND_PATH =
            "/api/guard/securephone/send";
    inline constexpr std::string_view SUPER_APP_MFA_PHONE_VALID_PATH =
            "/api/guard/securephone/valid";
    inline constexpr std::string_view CAMPUS_CARD_BALANCE_URL =
            "https://info.s.zzu.edu.cn/portal-api/v1/thrid-adapter/"
            "get-person-info-card-list";

    inline constexpr std::string_view CAMPUS_ECARD_HOST =
            "ecard.v.zzu.edu.cn";
    inline constexpr std::string_view CAMPUS_ECARD_BASE_URL =
            "https://ecard.v.zzu.edu.cn";
    // How could 'electricity' be spelled as 'electricty'?
    inline constexpr std::string_view CAMPUS_ECARD_ELECTRICITY_URL =
            "https://ecard.v.zzu.edu.cn/#/pages_payment/electrictyFees/electrictyFees";

    // Hard-coded user-agent string from the SuperApp iOS app. Modify it as you wish.
    inline constexpr std::string_view SUPER_APP_UA =
            "Mozilla/5.0 (iPhone; CPU iPhone OS 18_7 like Mac OS X) "
            "AppleWebKit/605.1.15 (KHTML, like Gecko) Mobile/15E148 "
            "SuperApp SuperApp-10460 appVersion-2.5.2";

    inline constexpr std::string_view CAMPUS_ECARD_REFERER =
            "https://ecard.v.zzu.edu.cn/?orgId=2";
    inline constexpr std::string_view ECARD_UPDATE_TOKEN_TARGET =
            "/server/auth/updateToken";
    inline constexpr std::string_view ECARD_GET_TOKEN_TARGET =
            "/server/auth/getToken";
    inline constexpr std::string_view ECARD_GET_ENCRYPT_TARGET =
            "/server/auth/getEncrypt";
    inline constexpr std::string_view ECARD_ELECTRICITY_PAY_TARGET =
            "/server/utilities/pay";
    inline constexpr std::string_view ECARD_ENCRYPTION_SM4_KEY_HEX =
            "773638372d392b33435f48266a655f35";
    inline constexpr std::string_view ECARD_HOST_OPEN_TARGET =
            "/server/auth/host/open";
    inline constexpr std::string_view ECARD_ELECTRICITY_LOCATION_TARGET =
            "/server/utilities/location";
    inline constexpr std::string_view ECARD_ELECTRICITY_ACCOUNT_TARGET =
            "/server/utilities/account";
    inline constexpr std::string_view ECARD_TRANSACTION_TARGET =
            "/server/consume/transaction";
    inline constexpr std::string_view ECARD_RECHARGE_CONFIG_TARGET =
            "/server/recharge/config";
    inline constexpr std::string_view ECARD_RECHARGE_PAY_TARGET =
            "/server/recharge/pay";
    inline constexpr std::string_view ECARD_RECHARGE_PAY_CODE = "07";
    inline constexpr std::string_view ECARD_PAYMENT_BASE_URL =
            "https://payment.v.zzu.edu.cn";
    inline constexpr std::string_view ECARD_PAYMENT_PAYWAYS_TARGET =
            "/WapCashDesk/e-pay/payways.html";
} // namespace zzu_assistant::model::app
