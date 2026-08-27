#include "client/app_client.h"

#include "model/app.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <format>
#include <iomanip>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace zzu_assistant::app {
    namespace {
        namespace asio = boost::asio;
        namespace beast = boost::beast;
        namespace http = beast::http;
        namespace ssl = asio::ssl;
        using tcp = asio::ip::tcp;
        using boost::property_tree::ptree;

        struct ParsedUrl {
            std::string host, target;
        };

        struct Response {
            unsigned status{};
            std::string body;
        };

        ParsedUrl parse_https(const std::string_view url) {
            constexpr std::string_view prefix = "https://";
            if (!url.starts_with(prefix)) throw std::runtime_error("MFA URL must use HTTPS");
            const auto slash = url.find('/', prefix.size());
            return {
                std::string(url.substr(prefix.size(), slash - prefix.size())),
                slash == std::string_view::npos ? "/" : std::string(url.substr(slash))
            };
        }

        std::string url_encode(const std::string_view value) {
            constexpr char hex[] = "0123456789ABCDEF";
            std::string result;
            for (const unsigned char byte: value) {
                if (std::isalnum(byte) || byte == '-' || byte == '_' || byte == '.' || byte == '~')
                    result.push_back(static_cast<char>(byte));
                else {
                    result += '%';
                    result += hex[byte >> 4];
                    result += hex[byte & 15];
                }
            }
            return result;
        }

        std::string form(const std::vector<std::pair<std::string_view, std::string> > &items) {
            std::string result;
            for (const auto &[key, value]: items) {
                if (!result.empty()) result += '&';
                result += url_encode(key) + '=' + url_encode(value);
            }
            return result;
        }

        ptree parse_json(const std::string &body) {
            ptree tree;
            std::istringstream in(body);
            boost::property_tree::read_json(in, tree);
            return tree;
        }

        std::string json_body(const ptree &tree) {
            std::ostringstream out;
            boost::property_tree::write_json(out, tree, false);
            return out.str();
        }

        std::string response_message(const ptree &tree,
                                     const std::string_view fallback) {
            const auto message = tree.get_optional<std::string>("message");
            if (!message || message->empty() || *message == "null")
                return std::string(fallback);
            return *message;
        }

        struct BioDelete {
            void operator()(BIO *p) const { BIO_free(p); }
        };

        struct KeyDelete {
            void operator()(EVP_PKEY *p) const { EVP_PKEY_free(p); }
        };

        struct CtxDelete {
            void operator()(EVP_PKEY_CTX *p) const { EVP_PKEY_CTX_free(p); }
        };

        std::string rsa_encrypt(const std::string_view pem, const std::string_view value) {
            std::unique_ptr<BIO, BioDelete> bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
            std::unique_ptr<EVP_PKEY, KeyDelete> key(
                bio ? PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr) : nullptr);
            if (!key) throw std::runtime_error("Cannot parse Super App RSA public key");
            std::unique_ptr<EVP_PKEY_CTX, CtxDelete> ctx(EVP_PKEY_CTX_new(key.get(), nullptr));
            if (!ctx || EVP_PKEY_encrypt_init(ctx.get()) <= 0 ||
                EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_PKCS1_PADDING) <= 0)
                throw std::runtime_error("Cannot initialize Super App RSA encryption");
            std::size_t size = 0;
            const auto *data = reinterpret_cast<const unsigned char *>(value.data());
            if (EVP_PKEY_encrypt(ctx.get(), nullptr, &size, data, value.size()) <= 0)
                throw std::runtime_error("Super App RSA encryption failed");
            std::vector<unsigned char> encrypted(size);
            if (EVP_PKEY_encrypt(ctx.get(), encrypted.data(), &size, data, value.size()) <= 0)
                throw std::runtime_error("Super App RSA encryption failed");
            encrypted.resize(size);
            std::string encoded(4 * ((size + 2) / 3), '\0');
            const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()),
                                                encrypted.data(), static_cast<int>(size));
            if (written <= 0) throw std::runtime_error("Cannot encode Super App RSA value");
            encoded.resize(static_cast<std::size_t>(written));
            return "__RSA__" + encoded;
        }

        std::string uuid_v4() {
            std::array<unsigned char, 16> bytes{};
            std::random_device random;
            for (auto &byte: bytes) byte = static_cast<unsigned char>(random());
            bytes[6] = (bytes[6] & 0x0f) | 0x40;
            bytes[8] = (bytes[8] & 0x3f) | 0x80;
            std::ostringstream out;
            out << std::hex << std::setfill('0');
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                if (i == 4 || i == 6 || i == 8 || i == 10) out << '-';
                out << std::setw(2) << static_cast<unsigned>(bytes[i]);
            }
            return out.str();
        }
    }

    class AppClient::Impl final {
    public:
        explicit Impl(ClientOptions options)
            : options_(std::move(options)), tls_(ssl::context::tls_client) {
            if (!options_.ca_file.empty()) tls_.load_verify_file(options_.ca_file);
#ifdef ZZU_DEFAULT_CA_FILE
            else tls_.load_verify_file(ZZU_DEFAULT_CA_FILE);
#else
            else tls_.set_default_verify_paths();
#endif
            tls_.set_verify_mode(ssl::verify_peer);
        }

        Response request(const std::string_view url, const http::verb verb,
                         std::string body = {}, const std::string_view content_type = {},
                         const std::map<std::string, std::string> &headers = {}) {
            const ParsedUrl parsed = parse_https(url);
            asio::io_context io;
            tcp::resolver resolver(io);
            beast::ssl_stream<beast::tcp_stream> stream(io, tls_);
            if (!SSL_set_tlsext_host_name(stream.native_handle(), parsed.host.c_str()))
                throw std::runtime_error("Cannot configure Super App TLS SNI");
            stream.set_verify_callback(ssl::host_name_verification(parsed.host));
            beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(30));
            beast::get_lowest_layer(stream).connect(resolver.resolve(parsed.host, "443"));
            stream.handshake(ssl::stream_base::client);
            http::request<http::string_body> req{verb, parsed.target, 11};
            req.set(http::field::host, parsed.host);
            req.set(http::field::user_agent, model::app::SUPER_APP_LOGIN_UA);
            req.set(http::field::accept, "*/*");
            for (const auto &[name, value]: headers) req.set(name, value);
            if (!content_type.empty()) req.set(http::field::content_type, content_type);
            req.body() = std::move(body);
            req.prepare_payload();
            http::write(stream, req);
            beast::flat_buffer buffer;
            http::response<http::string_body> res;
            http::read(stream, buffer, res);
            beast::error_code ignored;
            stream.shutdown(ignored);
            return {res.result_int(), std::move(res.body())};
        }

        Response token_request(const std::string_view target, const http::verb verb,
                               std::string body = {}, const std::string_view type = {}) {
            return request(std::string(model::app::SUPER_APP_AUTH_BASE_URL) + std::string(target),
                           verb, std::move(body), type);
        }

        LoginResult login(const LoginOptions &options) {
            try {
                if (options.username.empty() || options.password.empty())
                    throw std::runtime_error("Super App username and password are required");
                if (!username_.empty() && username_ != options.username) state_.clear();
                username_ = options.username;
                if (!options_.device_id.empty()) {
                    if (options_.device_id.size() > 128 ||
                        std::ranges::any_of(options_.device_id, [](const unsigned char ch) {
                            return ch <= 0x20 || ch >= 0x7f;
                        }))
                        throw std::runtime_error(
                            "device_id must be 1-128 printable ASCII characters without spaces");
                    state_.put("deviceId", options_.device_id);
                } else if (state_.get("deviceId", "").empty()) {
                    state_.put("deviceId", uuid_v4());
                }
                const auto key_response = token_request(model::app::SUPER_APP_PUBLIC_KEY_TARGET, http::verb::get);
                if (key_response.status != 200) throw std::runtime_error("Cannot obtain Super App RSA public key");
                const std::string username = rsa_encrypt(key_response.body, options.username);
                const std::string password = rsa_encrypt(key_response.body, options.password);
                const std::string device = state_.get<std::string>("deviceId");
                const auto detected = token_request(model::app::SUPER_APP_MFA_DETECT_TARGET,
                                                    http::verb::post, form({
                                                        {"username", username}, {"password", password},
                                                        {"deviceId", device}
                                                    }),
                                                    "application/x-www-form-urlencoded");
                const ptree detect = parse_json(detected.body);
                if (detected.status != 200 || detect.get<int>("code", -1) != 0)
                    throw std::runtime_error(response_message(
                        detect, "Super App MFA detection failed"));
                const bool need = detect.get("data.need", false);
                const std::string mfa_state = detect.get<std::string>("data.state");
                if (need) {
                    if (!detect.get("data.mfaTypeSecurePhone", false))
                        throw std::runtime_error("Super App requires an unsupported non-phone MFA method");
                    const auto initialized = token_request(
                        std::string(model::app::SUPER_APP_MFA_PHONE_INIT_TARGET) + "?state=" + url_encode(mfa_state),
                        http::verb::get);
                    const ptree init = parse_json(initialized.body);
                    if (initialized.status != 200 || init.get<int>("code", -1) != 0)
                        throw std::runtime_error(response_message(
                            init, "Cannot initialize Super App phone MFA"));
                    const std::string gid = init.get<std::string>("data.gid");
                    std::string attest = init.get("data.attestServerUrl",
                                                  std::string(model::app::SUPER_APP_MFA_ATTEST_FALLBACK));
                    while (attest.ends_with('/')) attest.pop_back();
                    ptree gid_json;
                    gid_json.put("gid", gid);
                    const auto sent = request(attest + std::string(model::app::SUPER_APP_MFA_PHONE_SEND_PATH),
                                              http::verb::post, json_body(gid_json), "application/json");
                    const ptree send = parse_json(sent.body);
                    if (sent.status != 200 || send.get<int>("code", -1) != 0)
                        throw std::runtime_error(response_message(
                            send, "Cannot send Super App phone verification code"));
                    if (options.notify) options.notify("A Super App phone verification code was sent.");
                    if (!options.prompt) throw std::runtime_error("Super App phone MFA needs a code prompt");
                    const std::string code = options.prompt("Phone verification code: ");
                    ptree valid_json;
                    valid_json.put("gid", gid);
                    valid_json.put("code", code);
                    const auto checked = request(attest + std::string(model::app::SUPER_APP_MFA_PHONE_VALID_PATH),
                                                 http::verb::post, json_body(valid_json), "application/json");
                    const ptree valid = parse_json(checked.body);
                    if (checked.status != 200 || valid.get<int>("code", -1) != 0 ||
                        valid.get<int>("data.status", -1) != 2)
                        throw std::runtime_error(response_message(
                            valid, "Super App phone verification code was rejected"));
                }
                const auto logged = token_request(model::app::SUPER_APP_PASSWORD_LOGIN_TARGET,
                                                  http::verb::post,
                                                  form({
                                                      {"username", username}, {"password", password},
                                                      {"appId", std::string(model::app::SUPER_APP_ID)},
                                                      {"osType", std::string(model::app::SUPER_APP_OS_TYPE)},
                                                      {"geo", ""},
                                                      {"deviceId", device},
                                                      {"clientId", std::string(model::app::SUPER_APP_CLIENT_ID)},
                                                      {"mfaState", mfa_state}
                                                  }), "application/x-www-form-urlencoded");
                const ptree result = parse_json(logged.body);
                if (logged.status != 200 || result.get<int>("code", -1) != 0)
                    throw std::runtime_error(response_message(
                        result, "Super App login failed"));
                state_.put("idToken", result.get<std::string>("data.idToken"));
                state_.put("refreshToken", result.get<std::string>("data.refreshToken"));
                return {true, "Super App session is ready"};
            } catch (const std::exception &error) { return {false, error.what()}; }
        }

        LoginResult logout() {
            username_.clear();
            state_.clear();
            return {true, "Super App session cleared"};
        }

        std::string id_token() {
            const std::string token = state_.get("idToken", "");
            if (token.empty())
                throw std::runtime_error(
                    "Super App client is not logged in");
            return token;
        }

        double card_balance() {
            const std::string token = id_token();
            const Response response = request(model::app::CAMPUS_CARD_BALANCE_URL,
                                              http::verb::get, {}, {},
                                              {
                                                  {"X-Id-Token", token},
                                                  {
                                                      "User-Agent", std::string(
                                                          model::app::SUPER_APP_UA)
                                                  }
                                              });
            if (response.status != 200)
                throw std::runtime_error(std::format(
                    "Campus card balance returned HTTP {}", response.status));
            const ptree tree = parse_json(response.body);
            const auto data = tree.get_child_optional("data");
            if (!data || data->size() < 2)
                throw std::runtime_error("Campus card balance response is incomplete");
            auto item = data->begin();
            ++item;
            return item->second.get<double>("amount");
        }

        Session session() const {
            return {username_, state_.get("deviceId", ""),
                    state_.get("idToken", ""), state_.get("refreshToken", "")};
        }

        void restore_session(const Session &session) {
            username_ = session.username;
            state_.clear();
            state_.put("deviceId", options_.device_id.empty()
                                       ? session.device_id
                                       : options_.device_id);
            state_.put("idToken", session.id_token);
            state_.put("refreshToken", session.refresh_token);
        }

        ClientOptions options_;
        ssl::context tls_;
        std::string username_;
        ptree state_;
    };

    AppClient::AppClient(ClientOptions options)
        : impl_(std::make_unique<Impl>(std::move(options))) {
    }

    AppClient::~AppClient() = default;

    AppClient::AppClient(AppClient &&) noexcept = default;

    AppClient &AppClient::operator=(AppClient &&) noexcept = default;

    LoginResult AppClient::login(const LoginOptions &options) { return impl_->login(options); }
    LoginResult AppClient::logout() { return impl_->logout(); }

    std::string AppClient::id_token() const {
        return const_cast<Impl *>(impl_.get())->id_token();
    }

    double AppClient::card_balance() const {
        return const_cast<Impl *>(impl_.get())->card_balance();
    }
    Session AppClient::session() const { return impl_->session(); }
    LoginResult AppClient::login(const Session &session) {
        if (session.username.empty())
            return {false, "Session username is required"};
        if (session.id_token.empty())
            return {false, "Session idToken is required"};
        impl_->restore_session(session);
        return {true, "Super App session restored"};
    }
} // namespace zzu_assistant::app
