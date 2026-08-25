#include "client/userinfo_client.h"

#include "model/userinfo.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace zzu_assistant::userinfo {
    namespace {
        using boost::property_tree::ptree;

        std::string percent_decode(const std::string_view input) {
            const auto hex = [](const char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                return -1;
            };
            std::string output;
            output.reserve(input.size());
            for (std::size_t index = 0; index < input.size(); ++index) {
                if (input[index] == '%' && index + 2 < input.size()) {
                    const int high = hex(input[index + 1]);
                    const int low = hex(input[index + 2]);
                    if (high < 0 || low < 0)
                        throw std::runtime_error("SSO redirect contains invalid percent encoding");
                    output.push_back(static_cast<char>((high << 4) | low));
                    index += 2;
                } else if (input[index] == '+') output.push_back(' ');
                else output.push_back(input[index]);
            }
            return output;
        }

        std::string query_value(const std::string_view url,
                                const std::string_view name) {
            const auto question = url.find('?');
            if (question == std::string_view::npos) return {};
            const auto fragment = url.find('#', question + 1);
            const std::size_t query_end = fragment == std::string_view::npos
                                              ? url.size()
                                              : fragment;
            std::size_t start = question + 1;
            while (start <= query_end) {
                const auto end = url.find('&', start);
                const auto bounded_end = end == std::string_view::npos || end > query_end
                                             ? query_end
                                             : end;
                const auto part = url.substr(start, bounded_end - start);
                const auto equals = part.find('=');
                if (percent_decode(part.substr(0, equals)) == name)
                    return percent_decode(equals == std::string_view::npos
                                              ? std::string_view{}
                                              : part.substr(equals + 1));
                if (end == std::string_view::npos || end >= query_end) break;
                start = end + 1;
            }
            return {};
        }

        bool is_portal_url(const std::string_view url) {
            constexpr std::string_view scheme = "https://";
            if (!url.starts_with(scheme)) return false;
            const std::string_view remainder = url.substr(scheme.size());
            const auto separator = remainder.find_first_of("/?#");
            const std::string_view authority = remainder.substr(0, separator);
            return authority == model::userinfo::PORTAL_HOST ||
                   authority == std::string(model::userinfo::PORTAL_HOST) + ":443";
        }

        std::string jwt_payload(const std::string_view token) {
            const auto first = token.find('.');
            const auto second = first == std::string_view::npos
                                    ? std::string_view::npos
                                    : token.find('.', first + 1);
            if (first == std::string_view::npos || second == std::string_view::npos)
                throw std::runtime_error("CAS returned a malformed JWT ticket");
            std::string encoded(token.substr(first + 1, second - first - 1));
            std::ranges::replace(encoded, '-', '+');
            std::ranges::replace(encoded, '_', '/');
            while (encoded.size() % 4 != 0) encoded.push_back('=');
            std::vector<unsigned char> decoded(3 * encoded.size() / 4 + 1);
            const int size = EVP_DecodeBlock(
                decoded.data(),
                reinterpret_cast<const unsigned char *>(encoded.data()),
                static_cast<int>(encoded.size()));
            if (size < 0) throw std::runtime_error("CAS returned invalid JWT encoding");
            std::size_t padding = 0;
            if (!encoded.empty() && encoded.back() == '=') ++padding;
            if (encoded.size() > 1 && encoded[encoded.size() - 2] == '=') ++padding;
            return {reinterpret_cast<const char *>(decoded.data()),
                    static_cast<std::size_t>(size) - padding};
        }

        ptree jwt_claims(const std::string_view token) {
            ptree claims;
            std::istringstream input(jwt_payload(token));
            boost::property_tree::read_json(input, claims);
            return claims;
        }

        std::string claim(const ptree &tree,
                          const std::initializer_list<std::string_view> names) {
            for (const auto name: names)
                if (const auto value = tree.get_optional<std::string>(
                        std::string(name)); value && !value->empty()) return *value;
            return {};
        }

        void validate_claims(const ptree &claims,
                             const std::string_view expected_username,
                             const bool outer) {
            const std::string issuer = claims.get<std::string>("iss", {});
            const std::string_view expected_issuer = outer
                                                         ? model::userinfo::SSO_ISSUER
                                                         : model::userinfo::IDENTITY_ISSUER;
            if (issuer != expected_issuer)
                throw std::runtime_error("CAS ticket issuer is invalid");
            const auto expires = claims.get<std::int64_t>("exp", 0);
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (expires <= now) throw std::runtime_error("CAS ticket has expired");
            const std::string username = outer
                                             ? claims.get<std::string>("sub", {})
                                             : claim(claims, {"ATTR_accountName",
                                                              "ATTR_userNo", "sub"});
            if (username.empty() || username != expected_username)
                throw std::runtime_error("CAS ticket belongs to another user");
            if (outer && claims.get<std::string>("aud", {}) !=
                         model::userinfo::SERVICE_URL)
                throw std::runtime_error("CAS ticket audience is invalid");
        }

        void validate_identity_value(const std::string_view value) {
            if (value.find_first_of("\r\n\0") != std::string_view::npos)
                throw std::runtime_error(
                    "CAS identity contains invalid control characters");
        }
    } // namespace

    class UserInfoClient::Impl final {
    public:
        model::userinfo::UserInfo parse(
            const std::string_view final_url,
            const std::string_view expected_username) const {
            if (!is_portal_url(final_url))
                throw std::runtime_error("SSO user-info redirect has an unexpected host");
            const std::string ticket = query_value(final_url, "ticket");
            if (ticket.empty())
                throw std::runtime_error("SSO user-info redirect contains no ticket");
            const ptree outer = jwt_claims(ticket);
            validate_claims(outer, expected_username, true);
            const std::string id_token = outer.get<std::string>("idToken", {});
            if (id_token.empty())
                throw std::runtime_error("CAS ticket contains no identity token");
            const ptree identity = jwt_claims(id_token);
            validate_claims(identity, expected_username, false);

            model::userinfo::UserInfo info;
            info.username = std::string(expected_username);
            info.name = claim(identity, {"ATTR_name", "ATTR_userName"});
            info.identity_type_code = claim(identity, {"ATTR_identityTypeCode"});
            info.identity_type_name = claim(identity, {"ATTR_identityTypeName"});
            info.organization_code = claim(identity, {"ATTR_organizationCode"});
            info.organization_name = claim(identity, {"ATTR_organizationName"});
            info.account_id = claim(identity, {"ATTR_accountId"});
            info.user_id = claim(identity, {"ATTR_userId"});
            info.uid = claim(identity, {"ATTR_uid"});
            info.expires_at = identity.get<std::int64_t>("exp", 0);
            validate_identity_value(info.username);
            validate_identity_value(info.name);
            validate_identity_value(info.identity_type_code);
            validate_identity_value(info.identity_type_name);
            validate_identity_value(info.organization_code);
            validate_identity_value(info.organization_name);
            validate_identity_value(info.account_id);
            validate_identity_value(info.user_id);
            validate_identity_value(info.uid);
            return info;
        }
    };

    UserInfoClient::UserInfoClient() : impl_(std::make_unique<Impl>()) {}
    UserInfoClient::~UserInfoClient() = default;
    UserInfoClient::UserInfoClient(UserInfoClient &&) noexcept = default;
    UserInfoClient &UserInfoClient::operator=(UserInfoClient &&) noexcept = default;

    model::userinfo::UserInfo UserInfoClient::parse_sso_redirect(
        const std::string_view final_url,
        const std::string_view expected_username) const {
        return impl_->parse(final_url, expected_username);
    }
} // namespace zzu_assistant::userinfo
