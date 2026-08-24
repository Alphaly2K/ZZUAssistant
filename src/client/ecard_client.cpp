#include "client/ecard_client.h"

#include "model/app.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include <array>
#include <chrono>
#include <ctime>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <format>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace zzu_assistant::ecard {
    namespace {
        namespace asio = boost::asio;
        namespace beast = boost::beast;
        namespace http = beast::http;
        namespace ssl = asio::ssl;
        using tcp = asio::ip::tcp;
        using boost::property_tree::ptree;

        struct Response {
            unsigned status{};
            std::string body;
            std::string location;
        };

        std::optional<std::string> environment(const char *name) {
#ifdef _WIN32
            char *value = nullptr;
            std::size_t size = 0;
            if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
                return std::nullopt;
            }
            std::string result(value);
            std::free(value);
            return result;
#else
    if (const char* value = std::getenv(name)) return std::string(value);
    return std::nullopt;
#endif
        }

        std::filesystem::path state_directory() {
            if (const auto configured = environment("ZZUASSISTANT_STATE_DIR");
                configured && !configured->empty())
                return *configured;
#ifdef _WIN32
            if (const auto value = environment("LOCALAPPDATA"))
                return std::filesystem::path(*value) / "ZZUAssistant";
#elif defined(__APPLE__)
    if (const auto value = environment("HOME"))
        return std::filesystem::path(*value) / "Library" /
               "Application Support" / "ZZUAssistant";
#else
    if (const auto value = environment("XDG_STATE_HOME"))
        return std::filesystem::path(*value) / "zzu-assistant";
    if (const auto value = environment("HOME"))
        return std::filesystem::path(*value) / ".local" / "state" /
               "zzu-assistant";
#endif
            throw std::runtime_error("Cannot determine the eCard state directory");
        }

        std::string user_key(const std::string_view username) {
            std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
            unsigned int size = 0;
            if (EVP_Digest(username.data(), username.size(), digest.data(), &size,
                           EVP_sha256(), nullptr) != 1) {
                throw std::runtime_error("Cannot derive the eCard user key");
            }
            std::ostringstream out;
            out << std::hex << std::setfill('0');
            for (unsigned index = 0; index < size; ++index)
                out << std::setw(2) << static_cast<unsigned>(digest[index]);
            return out.str();
        }

        ptree parse_json(const std::string &body) {
            std::istringstream input(body);
            ptree tree;
            boost::property_tree::read_json(input, tree);
            return tree;
        }

        std::string json_body(const ptree &tree) {
            std::ostringstream output;
            boost::property_tree::write_json(output, tree, false);
            return output.str();
        }

        std::string url_encode(const std::string_view value) {
            constexpr char hex[] = "0123456789ABCDEF";
            std::string result;
            for (const unsigned char byte: value) {
                if (std::isalnum(byte) || byte == '-' || byte == '_' ||
                    byte == '.' || byte == '~')
                    result.push_back(static_cast<char>(byte));
                else {
                    result += '%';
                    result += hex[byte >> 4];
                    result += hex[byte & 15];
                }
            }
            return result;
        }

        std::string query_value(const std::string_view url, const std::string_view key) {
            const auto question = url.find('?');
            if (question == std::string_view::npos) return {};
            std::string_view query = url.substr(question + 1);
            while (!query.empty()) {
                const auto amp = query.find('&');
                const auto item = query.substr(0, amp);
                const auto equals = item.find('=');
                if (item.substr(0, equals) == key)
                    return equals == std::string_view::npos ? std::string{} : std::string(item.substr(equals + 1));
                if (amp == std::string_view::npos) break;
                query.remove_prefix(amp + 1);
            }
            return {};
        }

        std::vector<unsigned char> hex_decode(const std::string_view value) {
            if (value.size() % 2 != 0) throw std::runtime_error("Invalid hexadecimal data");
            const auto digit = [](const char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                return -1;
            };
            std::vector<unsigned char> result(value.size() / 2);
            for (std::size_t index = 0; index < result.size(); ++index) {
                const int high = digit(value[index * 2]);
                const int low = digit(value[index * 2 + 1]);
                if (high < 0 || low < 0) throw std::runtime_error("Invalid hexadecimal data");
                result[index] = static_cast<unsigned char>((high << 4) | low);
            }
            return result;
        }

        std::string hex_encode(const std::vector<unsigned char> &value) {
            constexpr char digits[] = "0123456789abcdef";
            std::string result;
            result.reserve(value.size() * 2);
            for (const unsigned char byte: value) {
                result.push_back(digits[byte >> 4]);
                result.push_back(digits[byte & 15]);
            }
            return result;
        }

        std::vector<unsigned char> base64_decode(std::string value) {
            value.erase(std::remove_if(value.begin(), value.end(), [](const unsigned char ch) {
                return std::isspace(ch) != 0;
            }), value.end());
            if (value.empty() || value.size() % 4 != 0)
                throw std::runtime_error("Invalid eCard encryption key encoding");
            std::vector<unsigned char> result(value.size() / 4 * 3);
            const int size = EVP_DecodeBlock(result.data(),
                                             reinterpret_cast<const unsigned char *>(value.data()),
                                             static_cast<int>(value.size()));
            if (size < 0) throw std::runtime_error("Invalid eCard encryption key encoding");
            std::size_t padding = (!value.empty() && value.back() == '=') +
                                  (value.size() > 1 && value[value.size() - 2] == '=');
            result.resize(static_cast<std::size_t>(size) - padding);
            return result;
        }

        std::string sm4_decrypt_public_key(const std::string &encoded) {
            const auto ciphertext = base64_decode(encoded);
            const auto key = hex_decode(model::app::ECARD_ENCRYPTION_SM4_KEY_HEX);
            std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context(
                EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
            if (!context || EVP_DecryptInit_ex(context.get(), EVP_sm4_ecb(), nullptr,
                                               key.data(), nullptr) != 1)
                throw std::runtime_error("Cannot initialize eCard SM4 decryption");
            std::vector<unsigned char> plaintext(ciphertext.size() + EVP_MAX_BLOCK_LENGTH);
            int first = 0, last = 0;
            if (EVP_DecryptUpdate(context.get(), plaintext.data(), &first,
                                  ciphertext.data(), static_cast<int>(ciphertext.size())) != 1 ||
                EVP_DecryptFinal_ex(context.get(), plaintext.data() + first, &last) != 1)
                throw std::runtime_error("Cannot decrypt the eCard SM2 public key");
            plaintext.resize(static_cast<std::size_t>(first + last));
            return std::string(reinterpret_cast<const char *>(plaintext.data()), plaintext.size());
        }

        struct GroupDelete {
            void operator()(EC_GROUP *p) const { EC_GROUP_free(p); }
        };

        struct PointDelete {
            void operator()(EC_POINT *p) const { EC_POINT_free(p); }
        };

        struct BnDelete {
            void operator()(BIGNUM *p) const { BN_free(p); }
        };

        struct BnCtxDelete {
            void operator()(BN_CTX *p) const { BN_CTX_free(p); }
        };

        struct StringCleanser {
            std::string &value;
            ~StringCleanser() { if (!value.empty()) OPENSSL_cleanse(value.data(), value.size()); }
        };

        struct VectorCleanser {
            std::vector<unsigned char> &value;
            ~VectorCleanser() { if (!value.empty()) OPENSSL_cleanse(value.data(), value.size()); }
        };

        std::array<unsigned char, 32> sm3(const std::vector<unsigned char> &value) {
            std::array<unsigned char, 32> digest{};
            unsigned size = 0;
            if (EVP_Digest(value.data(), value.size(), digest.data(), &size,
                           EVP_sm3(), nullptr) != 1 || size != digest.size())
                throw std::runtime_error("SM3 digest failed");
            return digest;
        }

        std::vector<unsigned char> sm2_kdf(const std::vector<unsigned char> &source,
                                           const std::size_t size) {
            std::vector<unsigned char> result;
            result.reserve(size);
            for (std::uint32_t counter = 1; result.size() < size; ++counter) {
                std::vector<unsigned char> block(source);
                block.push_back(static_cast<unsigned char>(counter >> 24));
                block.push_back(static_cast<unsigned char>(counter >> 16));
                block.push_back(static_cast<unsigned char>(counter >> 8));
                block.push_back(static_cast<unsigned char>(counter));
                const auto digest = sm3(block);
                const std::size_t count = std::min(digest.size(), size - result.size());
                result.insert(result.end(), digest.begin(), digest.begin() + count);
            }
            return result;
        }

        std::vector<unsigned char> point_bytes(const EC_GROUP *group, const EC_POINT *point,
                                               BN_CTX *context) {
            std::vector<unsigned char> result(65);
            const std::size_t size = EC_POINT_point2oct(group, point,
                                                        POINT_CONVERSION_UNCOMPRESSED, result.data(), result.size(),
                                                        context);
            if (size != result.size()) throw std::runtime_error("Cannot encode SM2 point");
            return result;
        }

        std::vector<unsigned char> sm2_encrypt_c1c3c2(std::string public_key_hex,
                                                      const std::string_view plaintext) {
            while (!public_key_hex.empty() && std::isspace(
                       static_cast<unsigned char>(public_key_hex.back())))
                public_key_hex.pop_back();
            while (!public_key_hex.empty() && std::isspace(
                       static_cast<unsigned char>(public_key_hex.front())))
                public_key_hex.erase(public_key_hex.begin());
            auto public_bytes = hex_decode(public_key_hex);
            if (public_bytes.size() == 64) public_bytes.insert(public_bytes.begin(), 0x04);
            if (public_bytes.size() != 65 || public_bytes.front() != 0x04)
                throw std::runtime_error("eCard returned an invalid SM2 public key");
            std::unique_ptr<EC_GROUP, GroupDelete> group(EC_GROUP_new_by_curve_name(NID_sm2));
            std::unique_ptr<BN_CTX, BnCtxDelete> bn_context(BN_CTX_new());
            std::unique_ptr<EC_POINT, PointDelete> public_point(
                group ? EC_POINT_new(group.get()) : nullptr);
            std::unique_ptr<BIGNUM, BnDelete> order(BN_new());
            if (!group || !bn_context || !public_point || !order ||
                EC_POINT_oct2point(group.get(), public_point.get(), public_bytes.data(),
                                   public_bytes.size(), bn_context.get()) != 1 ||
                EC_GROUP_get_order(group.get(), order.get(), bn_context.get()) != 1)
                throw std::runtime_error("Cannot initialize the eCard SM2 public key");

            std::vector<unsigned char> message(plaintext.begin(), plaintext.end());
            VectorCleanser message_cleanser{message};
            for (;;) {
                std::unique_ptr<BIGNUM, BnDelete> random(BN_new());
                std::unique_ptr<EC_POINT, PointDelete> c1(EC_POINT_new(group.get()));
                std::unique_ptr<EC_POINT, PointDelete> shared(EC_POINT_new(group.get()));
                if (!random || !c1 || !shared ||
                    BN_priv_rand_range(random.get(), order.get()) != 1 || BN_is_zero(random.get()))
                    continue;
                if (EC_POINT_mul(group.get(), c1.get(), random.get(), nullptr, nullptr,
                                 bn_context.get()) != 1 ||
                    EC_POINT_mul(group.get(), shared.get(), nullptr, public_point.get(),
                                 random.get(), bn_context.get()) != 1)
                    throw std::runtime_error("SM2 point multiplication failed");
                const auto shared_bytes = point_bytes(group.get(), shared.get(), bn_context.get());
                std::vector<unsigned char> coordinates(shared_bytes.begin() + 1, shared_bytes.end());
                auto mask = sm2_kdf(coordinates, message.size());
                if (std::all_of(mask.begin(), mask.end(), [](const unsigned char byte) { return byte == 0; }))
                    continue;
                std::vector<unsigned char> c2(message.size());
                for (std::size_t index = 0; index < message.size(); ++index)
                    c2[index] = message[index] ^ mask[index];
                std::vector<unsigned char> c3_input;
                VectorCleanser c3_cleanser{c3_input};
                c3_input.insert(c3_input.end(), coordinates.begin(), coordinates.begin() + 32);
                c3_input.insert(c3_input.end(), message.begin(), message.end());
                c3_input.insert(c3_input.end(), coordinates.begin() + 32, coordinates.end());
                const auto c3 = sm3(c3_input);
                auto c1_bytes = point_bytes(group.get(), c1.get(), bn_context.get());
                std::vector<unsigned char> result(c1_bytes.begin() + 1, c1_bytes.end());
                result.insert(result.end(), c3.begin(), c3.end());
                result.insert(result.end(), c2.begin(), c2.end());
                return result;
            }
        }

        std::string json_escape(const std::string_view value) {
            std::string result;
            for (const unsigned char ch: value) {
                switch (ch) {
                    case '\\': result += "\\\\";
                        break;
                    case '"': result += "\\\"";
                        break;
                    case '\b': result += "\\b";
                        break;
                    case '\f': result += "\\f";
                        break;
                    case '\n': result += "\\n";
                        break;
                    case '\r': result += "\\r";
                        break;
                    case '\t': result += "\\t";
                        break;
                    default: result.push_back(static_cast<char>(ch));
                }
            }
            return result;
        }

        std::string local_datetime() {
            const std::time_t now = std::time(nullptr);
            std::tm local{};
#ifdef _WIN32
            if (localtime_s(&local, &now) != 0)
                throw std::runtime_error("Cannot determine the local time");
#else
            if (localtime_r(&now, &local) == nullptr)
                throw std::runtime_error("Cannot determine the local time");
#endif
            std::ostringstream output;
            output << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
            return output.str();
        }

        void put_path(ptree &tree, const LocationPath &path) {
            tree.put("bigArea", path.big_area);
            tree.put("area", path.area);
            tree.put("building", path.building);
            tree.put("unit", path.unit);
            tree.put("level", path.level);
            tree.put("room", path.room);
            tree.put("subArea", path.sub_area);
        }

        LocationPath get_path(const ptree &tree) {
            return {
                tree.get("bigArea", ""), tree.get("area", ""),
                tree.get("building", ""), tree.get("unit", ""),
                tree.get("level", ""), tree.get("room", ""),
                tree.get("subArea", ""),
            };
        }

        void ensure_success(const ptree &tree, const std::string_view operation) {
            if (!tree.get("success", false)) {
                throw std::runtime_error(std::format(
                    "eCard {} failed: {}", operation,
                    tree.get("message", "unknown server error")));
            }
        }
    } // namespace

    class EcardClient::Impl final {
    public:
        Impl() : tls_(ssl::context::tls_client) {
            if (const auto ca = environment("SSL_CERT_FILE"); ca && !ca->empty()) {
                tls_.load_verify_file(*ca);
#ifdef ZZU_DEFAULT_CA_FILE
            } else {
                tls_.load_verify_file(ZZU_DEFAULT_CA_FILE);
#else
        } else {
            tls_.set_default_verify_paths();
#endif
            }
            tls_.set_verify_mode(ssl::verify_peer);
        }

        void select_user(const std::string_view username) {
            if (username.empty()) throw std::invalid_argument("Username is required");
            username_ = username;
            file_ = state_directory() / "sessions" /
                    (user_key(username) + ".ecard.db");
            state_.clear();
            std::ifstream input(file_);
            if (input) {
                try { boost::property_tree::read_json(input, state_); } catch (const
                    boost::property_tree::json_parser::json_parser_error &) {
                    throw std::runtime_error("The selected user's eCard DB is corrupt");
                }
            }
        }

        Response request(const http::verb verb, const std::string_view target,
                         std::string body = {},
                         const std::map<std::string, std::string> &headers = {}) {
            asio::io_context context;
            tcp::resolver resolver(context);
            beast::ssl_stream<beast::tcp_stream> stream(context, tls_);
            if (!SSL_set_tlsext_host_name(
                stream.native_handle(),
                model::app::CAMPUS_ECARD_HOST.data()))
                throw std::runtime_error("Cannot configure eCard TLS SNI");
            stream.set_verify_callback(
                ssl::host_name_verification(
                    std::string(model::app::CAMPUS_ECARD_HOST)));
            const auto endpoints = resolver.resolve(
                model::app::CAMPUS_ECARD_HOST, "443");
            beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(30));
            beast::get_lowest_layer(stream).connect(endpoints);
            stream.handshake(ssl::stream_base::client);

            http::request<http::string_body> request{verb, target, 11};
            request.set(http::field::host, model::app::CAMPUS_ECARD_HOST);
            request.set(http::field::user_agent, model::app::SUPER_APP_UA);
            request.set(http::field::accept, "*/*");
            if (verb == http::verb::post) {
                request.set(http::field::content_type, "application/json");
                request.set(http::field::origin, model::app::CAMPUS_ECARD_BASE_URL);
                request.set(http::field::referer,
                            model::app::CAMPUS_ECARD_REFERER);
                request.set("V8-Language", "zh-CN");
                request.set("transactionId", "");
                request.body() = std::move(body);
            }
            for (const auto &[name, value]: headers) request.set(name, value);
            if (verb == http::verb::post) request.prepare_payload();
            http::write(stream, request);
            beast::flat_buffer buffer;
            http::response<http::string_body> response;
            http::read(stream, buffer, response);
            const auto location = response.base().find(http::field::location);
            Response result{
                response.result_int(), std::move(response.body()),
                location == response.base().end() ? std::string{} : std::string(location->value())
            };
            beast::error_code error;
            stream.shutdown(error);
            return result;
        }

        void authorize(const std::string_view super_app_id_token) {
            if (const auto token = environment("ZZUASSISTANT_ECARD_ACCESS_TOKEN");
                token && !token->empty()) {
                state_.put("accessToken", *token);
                if (const auto refresh =
                            environment("ZZUASSISTANT_ECARD_REFRESH_TOKEN");
                    refresh && !refresh->empty()) {
                    state_.put("refreshToken", *refresh);
                }
                save();
                return;
            }
            if (!state_.get("accessToken", "").empty()) return;
            if (super_app_id_token.empty())
                throw std::runtime_error(
                    "No eCard token and no Super App idToken; run 'app login <username>' first");
            const std::string open_target =
                    std::string(model::app::ECARD_HOST_OPEN_TARGET) +
                    "?host=11&org=2&X-Id-Token=" + url_encode(super_app_id_token);
            const Response opened = request(http::verb::get, open_target);
            if (opened.status != 301 && opened.status != 302 && opened.status != 303)
                throw std::runtime_error("eCard rejected the Super App idToken");
            const std::string tid = query_value(opened.location, "tid");
            if (tid.empty()) throw std::runtime_error("eCard launch did not return a tid");
            ptree payload;
            payload.put("tid", tid);
            const Response exchanged = request(http::verb::post,
                                               model::app::ECARD_GET_TOKEN_TARGET,
                                               json_body(payload));
            if (exchanged.status != 200)
                throw std::runtime_error("eCard token exchange endpoint is unavailable");
            const ptree result = parse_json(exchanged.body);
            ensure_success(result, "Super App token exchange");
            const auto &data = result.get_child("resultData");
            state_.put("accessToken", data.get<std::string>("accessToken"));
            state_.put("refreshToken", data.get<std::string>("refreshToken"));
            state_.put("accessTokenExpire", data.get("accessTokenExpire", ""));
            save();
        }

        void refresh_access_token() {
            const std::string refresh = state_.get("refreshToken", "");
            if (refresh.empty()) {
                throw std::runtime_error(
                    "The eCard session expired and has no cached refresh token");
            }
            ptree payload;
            payload.put("refreshToken", refresh);
            const std::string token = state_.get("accessToken", "");
            const Response response = request(http::verb::post,
                                              model::app::ECARD_UPDATE_TOKEN_TARGET,
                                              json_body(payload),
                                              {{"Authorization", token}});
            if (response.status != 200)
                throw std::runtime_error(
                    "The eCard token refresh endpoint is unavailable");
            const ptree tree = parse_json(response.body);
            ensure_success(tree, "token refresh");
            const auto &result = tree.get_child("resultData");
            state_.put("accessToken", result.get<std::string>("accessToken"));
            state_.put("refreshToken", result.get("refreshToken", refresh));
            state_.put("accessTokenExpire", result.get("accessTokenExpire", ""));
            save();
        }

        ptree protected_post_body(const std::string_view target,
                                  const std::string_view body,
                                  const std::string_view transaction_id = {}) {
            for (int attempt = 0; attempt < 2; ++attempt) {
                const std::string token = state_.get("accessToken", "");
                if (token.empty())
                    throw std::runtime_error("eCard is not authorized");
                std::map<std::string, std::string> headers{
                    {"Authorization", token}
                };
                if (!transaction_id.empty())
                    headers.emplace("transactionId", transaction_id);
                const Response response = request(http::verb::post, target,
                                                  std::string(body), headers);
                if (response.status == 401 || response.status == 403) {
                    if (attempt == 0) {
                        refresh_access_token();
                        continue;
                    }
                    throw std::runtime_error("The refreshed eCard session was rejected");
                }
                if (response.status != 200)
                    throw std::runtime_error(std::format(
                        "eCard returned HTTP {}", response.status));
                ptree tree = parse_json(response.body);
                const std::string code = tree.get("code", "");
                if (!tree.get("success", false) && code == "401" && attempt == 0) {
                    refresh_access_token();
                    continue;
                }
                return tree;
            }
            throw std::runtime_error("The eCard request failed after token refresh");
        }

        ptree protected_post(const std::string_view target, const ptree &payload,
                             const std::string_view transaction_id = {}) {
            return protected_post_body(target, json_body(payload),
                                       transaction_id);
        }

        ptree protected_empty_post(const std::string_view target) {
            for (int attempt = 0; attempt < 2; ++attempt) {
                const std::string token = state_.get("accessToken", "");
                if (token.empty()) throw std::runtime_error("eCard is not authorized");
                const Response response = request(http::verb::post, target, {},
                                                  {{"Authorization", token}});
                if (response.status == 401 || response.status == 403) {
                    if (attempt == 0) {
                        refresh_access_token();
                        continue;
                    }
                    throw std::runtime_error("The refreshed eCard session was rejected");
                }
                if (response.status != 200)
                    throw std::runtime_error(std::format("eCard returned HTTP {}", response.status));
                ptree tree = parse_json(response.body);
                if (!tree.get("success", false) && tree.get("code", "") == "401" && attempt == 0) {
                    refresh_access_token();
                    continue;
                }
                return tree;
            }
            throw std::runtime_error("The eCard request failed after token refresh");
        }

        LocationPage locations(const std::string_view type,
                               const LocationPath &path) {
            ptree payload;
            payload.put("utilityType", "electric");
            payload.put("locationType", type);
            put_path(payload, path);
            payload.put("appId", "electric");
            const ptree tree = protected_post(
                model::app::ECARD_ELECTRICITY_LOCATION_TARGET, payload);
            ensure_success(tree, "location discovery");
            const auto &result = tree.get_child("resultData");
            LocationPage page;
            page.location_type = result.get("locationType", std::string(type));
            page.next_location_type = result.get("nextLocationType", "");
            page.end = result.get("end", false);
            if (const auto list = result.get_child_optional("locationList")) {
                for (const auto &[_, node]: *list) {
                    page.options.push_back({
                        node.get("id", ""),
                        node.get("name", "")
                    });
                }
            }
            return page;
        }

        ElectricityReading account(const LocationPath &path) {
            ptree payload;
            payload.put("utilityType", "electric");
            put_path(payload, path);
            payload.put("appId", "electric");
            const ptree tree = protected_post(
                model::app::ECARD_ELECTRICITY_ACCOUNT_TARGET, payload);
            ensure_success(tree, "electricity query");
            const auto &result = tree.get_child("resultData");
            ElectricityReading reading;
            if (const auto list = result.get_child_optional("templateList")) {
                for (const auto &[_, item]: *list) {
                    const std::string code = item.get("code", "");
                    const auto value = item.get_optional<double>("value");
                    if (code == "quantity" && value) reading.quantity_kwh = *value;
                    if (code == "price" && value) reading.price_yuan_per_kwh = *value;
                }
            }
            return reading;
        }

        RechargeResult recharge(const LocationPath &path,
                                const std::string_view payment_password,
                                const unsigned amount_yuan) {
            if (amount_yuan == 0) throw std::invalid_argument("Recharge amount must be positive");
            if (amount_yuan > 1000)
                throw std::invalid_argument(
                    "Recharge amount exceeds the interactive safety limit of 1000 yuan");
            if (payment_password.empty()) throw std::invalid_argument("Payment password is required");
            if (path.area.empty() || path.building.empty() || path.level.empty() || path.room.empty())
                throw std::invalid_argument("Electricity profile is incomplete");

            const ptree encryption = protected_empty_post(model::app::ECARD_GET_ENCRYPT_TARGET);
            ensure_success(encryption, "payment encryption setup");
            const auto &encryption_data = encryption.get_child("resultData");
            const std::string payment_id = encryption_data.get<std::string>("id");
            const std::string public_key = sm4_decrypt_public_key(
                encryption_data.get<std::string>("publicKey"));
            const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            std::string plaintext = std::format(
                "{{\"utilityType\":\"electric\",\"payCode\":\"06\","
                "\"password\":\"{}\",\"amt\":\"{}\",\"timestamp\":{},"
                "\"bigArea\":\"{}\",\"area\":\"{}\",\"building\":\"{}\","
                "\"unit\":\"{}\",\"level\":\"{}\",\"room\":\"{}\","
                "\"subArea\":\"{}\",\"customfield\":{{}}}}",
                json_escape(payment_password), amount_yuan, timestamp,
                json_escape(path.big_area), json_escape(path.area),
                json_escape(path.building), json_escape(path.unit),
                json_escape(path.level), json_escape(path.room),
                json_escape(path.sub_area));
            StringCleanser plaintext_cleanser{plaintext};
            const auto encrypted_parameters = sm2_encrypt_c1c3c2(public_key, plaintext);
            ptree payload;
            payload.put("id", payment_id);
            payload.put("params", hex_encode(encrypted_parameters));
            const ptree paid = protected_post(model::app::ECARD_ELECTRICITY_PAY_TARGET, payload);
            const bool success = paid.get("success", false);
            std::string message = paid.get("message", success ? "Recharge succeeded" : "Recharge failed");
            if (message.empty() || message == "null")
                message = success ? "Recharge succeeded" : "Recharge failed";
            return {success, std::move(message)};
        }

        CampusCardRechargeConfig campus_card_recharge_config() {
            const ptree tree = protected_empty_post(
                model::app::ECARD_RECHARGE_CONFIG_TARGET);
            ensure_success(tree, "campus card recharge configuration");
            const auto &data = tree.get_child("resultData");
            CampusCardRechargeConfig config;
            config.balance_yuan = data.get("balance", 0.0);
            if (const auto amounts = data.get_child_optional("amtGroupList")) {
                for (const auto &[_, item]: *amounts) {
                    const unsigned amount = item.get("amt", 0U);
                    if (amount > 0) config.amounts_yuan.push_back(amount);
                }
            }
            return config;
        }

        CampusCardRechargeOrder campus_card_recharge(
            const unsigned amount_yuan) {
            if (amount_yuan == 0 || amount_yuan > 1000)
                throw std::invalid_argument(
                    "Campus card recharge amount must be from 1 to 1000 yuan");

            ptree transaction_payload;
            transaction_payload.put("datetime", local_datetime());
            const ptree transaction = protected_post(
                model::app::ECARD_TRANSACTION_TARGET, transaction_payload);
            ensure_success(transaction, "transaction authorization");
            const std::string transaction_id =
                    transaction.get("resultData", "");
            if (transaction_id.empty())
                throw std::runtime_error(
                    "eCard transaction authorization returned no transaction ID");

            const auto timestamp = std::chrono::duration_cast<
                std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const std::string pay_body = std::format(
                "{{\"payCode\":\"{}\",\"amt\":\"{}\","
                "\"returnUrl\":\"{}/\",\"timestamp\":{}}}",
                model::app::ECARD_RECHARGE_PAY_CODE, amount_yuan,
                model::app::CAMPUS_ECARD_BASE_URL, timestamp);
            const ptree paid = protected_post_body(
                model::app::ECARD_RECHARGE_PAY_TARGET, pay_body,
                transaction_id);
            ensure_success(paid, "campus card recharge order");

            const auto &data = paid.get_child("resultData");
            const auto &wap = data.get_child("wapPayInfo");
            const auto &order = wap.get_child("orderInfo");
            const auto &extend = wap.get_child("extend");
            const std::string applet_result = std::format(
                "/pages_plugins/recharge/webSuccess?money={}", amount_yuan);
            const std::string order_json = std::format(
                "{{\"partnerid\":\"{}\",\"journo\":\"{}\","
                "\"merchantno\":\"{}\",\"channelno\":\"{}\","
                "\"payResultUrl\":\"{}\",\"goodsname\":\"{}\","
                "\"txamt\":\"{}\",\"openid\":\"{}\",\"extend\":{{"
                "\"source\":\"{}\",\"qyweixinid\":\"{}\","
                "\"aliuserid\":null,\"accessToken\":\"{}\","
                "\"appletPayResultUrl\":\"{}\"}}}}",
                json_escape(order.get<std::string>("partnerid")),
                json_escape(order.get<std::string>("journo")),
                json_escape(order.get<std::string>("merchantno")),
                json_escape(order.get<std::string>("channelno")),
                json_escape(order.get<std::string>("payResultUrl")),
                json_escape(order.get<std::string>("goodsname")),
                json_escape(order.get<std::string>("txamt")),
                json_escape(order.get<std::string>("openid")),
                json_escape(extend.get("source", "browser")),
                json_escape(extend.get("qyweixinid", "")),
                json_escape(extend.get<std::string>("accessToken")),
                json_escape(applet_result));
            const std::string checkout_url = std::format(
                "{}{}?orderInfo={}&callAccountid={}",
                model::app::ECARD_PAYMENT_BASE_URL,
                model::app::ECARD_PAYMENT_PAYWAYS_TARGET,
                url_encode(order_json),
                url_encode(wap.get("callAccountid", "")));
            return {amount_yuan, checkout_url};
        }

        bool load_profiles(ElectricityProfiles &profiles) const {
            const auto root = state_.get_child_optional("profiles");
            if (!root) return false;
            const auto lighting = root->get_child_optional("lighting");
            const auto air = root->get_child_optional("airConditioning");
            if (!lighting || !air) return false;
            profiles.lighting = get_path(*lighting);
            profiles.air_conditioning = get_path(*air);
            return !profiles.lighting.room.empty() &&
                   !profiles.air_conditioning.room.empty();
        }

        void save_profiles(const ElectricityProfiles &profiles) {
            ptree lighting;
            ptree air;
            put_path(lighting, profiles.lighting);
            put_path(air, profiles.air_conditioning);
            state_.put_child("profiles.lighting", lighting);
            state_.put_child("profiles.airConditioning", air);
            save();
        }

        void save() const {
            std::error_code error;
            std::filesystem::create_directories(file_.parent_path(), error);
            if (error) throw std::runtime_error("Cannot create eCard state directory");
            const std::filesystem::path temporary = file_.string() + ".tmp"; {
                std::ofstream output(temporary, std::ios::trunc);
                if (!output) throw std::runtime_error("Cannot save the eCard DB");
                boost::property_tree::write_json(output, state_, true);
            }
#ifndef _WIN32
        std::filesystem::permissions(
                temporary,
                std::filesystem::perms::owner_read |
                        std::filesystem::perms::owner_write,
                std::filesystem::perm_options::replace, error);
        if (error) throw std::runtime_error("Cannot protect the eCard DB");
#endif
            std::filesystem::rename(temporary, file_, error);
            if (error) {
                std::filesystem::remove(file_, error);
                error.clear();
                std::filesystem::rename(temporary, file_, error);
            }
            if (error) throw std::runtime_error("Cannot replace the eCard DB");
        }

        ssl::context tls_;
        std::string username_;
        std::filesystem::path file_;
        ptree state_;
    };

    EcardClient::EcardClient() : impl_(std::make_unique<Impl>()) {
    }

    EcardClient::~EcardClient() = default;

    EcardClient::EcardClient(EcardClient &&) noexcept = default;

    EcardClient &EcardClient::operator=(EcardClient &&) noexcept = default;

    void EcardClient::select_user(const std::string_view username) {
        impl_->select_user(username);
    }

    void EcardClient::authorize(const std::string_view token) { impl_->authorize(token); }

    LocationPage EcardClient::locations(const std::string_view type,
                                        const LocationPath &path) {
        return impl_->locations(type, path);
    }

    ElectricityReading EcardClient::account(const LocationPath &path) {
        return impl_->account(path);
    }

    RechargeResult EcardClient::recharge(const LocationPath &path,
                                         const std::string_view payment_password,
                                         const unsigned amount_yuan) {
        return impl_->recharge(path, payment_password, amount_yuan);
    }

    CampusCardRechargeConfig EcardClient::campus_card_recharge_config() {
        return impl_->campus_card_recharge_config();
    }

    CampusCardRechargeOrder EcardClient::campus_card_recharge(
        const unsigned amount_yuan) {
        return impl_->campus_card_recharge(amount_yuan);
    }

    bool EcardClient::load_profiles(ElectricityProfiles &profiles) const {
        return impl_->load_profiles(profiles);
    }

    void EcardClient::save_profiles(const ElectricityProfiles &profiles) const {
        impl_->save_profiles(profiles);
    }
} // namespace zzu_assistant::ecard
