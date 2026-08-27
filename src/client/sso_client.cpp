#include "client/sso_client.h"

#include "model/sso.h"

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
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <format>
#include <iomanip>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace zzu_assistant::sso {
    namespace {
        namespace asio = boost::asio;
        namespace beast = boost::beast;
        namespace http = beast::http;
        namespace ssl = asio::ssl;
        using tcp = asio::ip::tcp;

        struct ParsedUrl {
            std::string scheme;
            std::string host;
            std::string port;
            std::string target;
        };

        struct HttpResponse {
            unsigned status{};
            std::string body;
            std::string final_url;
        };

        struct Cookie {
            std::string name;
            std::string value;
            std::string domain;
            std::string path{"/"};
            bool secure{true};
            bool host_only{true};
        };

        [[nodiscard]] ParsedUrl parse_url(const std::string_view url) {
            const std::size_t scheme_end = url.find("://");
            if (scheme_end == std::string_view::npos) {
                throw std::runtime_error("Invalid absolute URL");
            }
            ParsedUrl result;
            result.scheme = std::string(url.substr(0, scheme_end));
            if (result.scheme != "https") {
                throw std::runtime_error("SSO only permits HTTPS endpoints");
            }

            const std::size_t authority_start = scheme_end + 3;
            const std::size_t path_start = url.find('/', authority_start);
            const std::string_view authority = url.substr(
                authority_start,
                path_start == std::string_view::npos
                    ? url.size() - authority_start
                    : path_start - authority_start);
            const std::size_t colon = authority.rfind(':');
            if (colon != std::string_view::npos) {
                result.host = std::string(authority.substr(0, colon));
                result.port = std::string(authority.substr(colon + 1));
            } else {
                result.host = std::string(authority);
                result.port = "443";
            }
            result.target = path_start == std::string_view::npos
                                ? "/"
                                : std::string(url.substr(path_start));
            if (result.host.empty()) {
                throw std::runtime_error("URL has no host");
            }
            return result;
        }

        [[nodiscard]] std::string resolve_redirect(
            const std::string_view current_url, const std::string_view location) {
            if (location.starts_with("https://")) {
                return std::string(location);
            }
            const ParsedUrl current = parse_url(current_url);
            std::string origin = current.scheme + "://" + current.host;
            if (current.port != "443") {
                origin += ':' + current.port;
            }
            if (location.starts_with('/')) {
                return origin + std::string(location);
            }
            const std::size_t slash = current.target.rfind('/');
            return origin + current.target.substr(0, slash + 1) +
                   std::string(location);
        }

        [[nodiscard]] std::string url_encode(const std::string_view value) {
            constexpr char hex[] = "0123456789ABCDEF";
            std::string result;
            result.reserve(value.size() * 3);
            for (const unsigned char byte: value) {
                if (std::isalnum(byte) != 0 || byte == '-' || byte == '_' ||
                    byte == '.' || byte == '~') {
                    result.push_back(static_cast<char>(byte));
                } else {
                    result.push_back('%');
                    result.push_back(hex[byte >> 4]);
                    result.push_back(hex[byte & 0x0f]);
                }
            }
            return result;
        }

        [[nodiscard]] std::string json_escape(const std::string_view value) {
            std::string result;
            result.reserve(value.size());
            for (const char character: value) {
                if (character == '\\' || character == '"') {
                    result.push_back('\\');
                }
                result.push_back(character);
            }
            return result;
        }

        [[nodiscard]] std::vector<unsigned char> decode_base64(
            std::string_view encoded) {
            std::string compact;
            compact.reserve(encoded.size());
            for (const unsigned char character: encoded) {
                if (std::isspace(character) == 0) {
                    compact.push_back(static_cast<char>(character));
                }
            }
            if (compact.empty() || compact.size() % 4 != 0) {
                throw std::runtime_error("The SSO QR image has invalid Base64 data");
            }
            std::vector<unsigned char> decoded(compact.size() / 4 * 3);
            const int size = EVP_DecodeBlock(
                decoded.data(),
                reinterpret_cast<const unsigned char *>(compact.data()),
                static_cast<int>(compact.size()));
            if (size < 0) {
                throw std::runtime_error("The SSO QR image has invalid Base64 data");
            }
            std::size_t padding = 0;
            if (!compact.empty() && compact.back() == '=') {
                ++padding;
            }
            if (compact.size() > 1 && compact[compact.size() - 2] == '=') {
                ++padding;
            }
            decoded.resize(static_cast<std::size_t>(size) - padding);
            return decoded;
        }

        [[nodiscard]] std::string ascii_lower(std::string value) {
            std::ranges::transform(value, value.begin(), [](const unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            return value;
        }

        [[nodiscard]] std::string trim(std::string value) {
            const auto is_space = [](const unsigned char ch) {
                return std::isspace(ch) != 0;
            };
            value.erase(value.begin(), std::ranges::find_if_not(value, is_space));
            value.erase(std::ranges::find_if_not(value.rbegin(), value.rend(), is_space)
                        .base(),
                        value.end());
            return value;
        }

        class CookieJar final {
        public:
            CookieJar() = default;

            [[nodiscard]] std::string header(const std::string_view host,
                                             const std::string_view target) const {
                const std::string normalized_host = ascii_lower(std::string(host));
                const std::string_view request_path =
                        target.starts_with('/')
                            ? target.substr(0, target.find('?'))
                            : std::string_view{"/"};
                std::string result;
                for (const Cookie &cookie: cookies_) {
                    const bool domain_matches = cookie.host_only
                                                    ? normalized_host == cookie.domain
                                                    : normalized_host == cookie.domain ||
                                                      (normalized_host.size() > cookie.domain.size() &&
                                                       normalized_host.ends_with(cookie.domain) &&
                                                       normalized_host[normalized_host.size() -
                                                                       cookie.domain.size() - 1] ==
                                                       '.');
                    const bool path_matches = request_path == cookie.path ||
                                              (request_path.starts_with(cookie.path) &&
                                               (cookie.path.ends_with('/') ||
                                                request_path.size() > cookie.path.size() &&
                                                request_path[cookie.path.size()] == '/'));
                    if (!domain_matches || !path_matches) {
                        continue;
                    }
                    if (!result.empty()) {
                        result += "; ";
                    }
                    result += cookie.name + '=' + cookie.value;
                }
                return result;
            }

            void accept(const std::string_view request_host,
                        const std::string_view request_target,
                        const std::string_view set_cookie) {
                std::vector<std::string> parts;
                std::size_t start = 0;
                while (start <= set_cookie.size()) {
                    const std::size_t end = set_cookie.find(';', start);
                    parts.push_back(trim(std::string(set_cookie.substr(
                        start, end == std::string_view::npos
                                   ? set_cookie.size() - start
                                   : end - start))));
                    if (end == std::string_view::npos) {
                        break;
                    }
                    start = end + 1;
                }
                if (parts.empty()) {
                    return;
                }
                const std::size_t equals = parts.front().find('=');
                if (equals == std::string::npos || equals == 0) {
                    return;
                }

                Cookie cookie;
                cookie.name = parts.front().substr(0, equals);
                cookie.value = parts.front().substr(equals + 1);
                cookie.domain = ascii_lower(std::string(request_host));
                const std::string_view raw_path = request_target.substr(
                    0, request_target.find('?'));
                const std::size_t last_slash = raw_path.rfind('/');
                cookie.path = last_slash == std::string_view::npos || last_slash == 0
                                  ? "/"
                                  : std::string(raw_path.substr(0, last_slash));
                bool remove = cookie.value.empty();

                for (std::size_t index = 1; index < parts.size(); ++index) {
                    const std::size_t attribute_equals = parts[index].find('=');
                    const std::string attribute_name = ascii_lower(trim(
                        parts[index].substr(0, attribute_equals)));
                    const std::string attribute_value =
                            attribute_equals == std::string::npos
                                ? std::string{}
                                : trim(parts[index].substr(attribute_equals + 1));
                    if (attribute_name == "domain" && !attribute_value.empty()) {
                        cookie.domain = ascii_lower(attribute_value);
                        if (cookie.domain.starts_with('.')) {
                            cookie.domain.erase(cookie.domain.begin());
                        }
                        cookie.host_only = false;
                    } else if (attribute_name == "path" &&
                               attribute_value.starts_with('/')) {
                        cookie.path = attribute_value;
                    } else if (attribute_name == "secure") {
                        cookie.secure = true;
                    } else if (attribute_name == "max-age" &&
                               (attribute_value == "0" ||
                                attribute_value.starts_with('-'))) {
                        remove = true;
                    }
                }

                if (!cookie.host_only) {
                    const std::string normalized_host =
                            ascii_lower(std::string(request_host));
                    const bool valid_domain = normalized_host == cookie.domain ||
                                              (normalized_host.size() > cookie.domain.size() &&
                                               normalized_host.ends_with(cookie.domain) &&
                                               normalized_host[normalized_host.size() -
                                                               cookie.domain.size() - 1] == '.');
                    if (!valid_domain) {
                        return;
                    }
                }

                std::erase_if(cookies_, [&](const Cookie &current) {
                    return current.name == cookie.name &&
                           current.domain == cookie.domain &&
                           current.path == cookie.path;
                });
                if (!remove) {
                    cookies_.push_back(std::move(cookie));
                }
            }

            [[nodiscard]] std::string serialize() const {
                std::ostringstream output;
                for (const Cookie &cookie: cookies_)
                    output << std::quoted(cookie.name) << ' '
                           << std::quoted(cookie.value) << ' '
                           << std::quoted(cookie.domain) << ' '
                           << std::quoted(cookie.path) << ' '
                           << cookie.secure << ' ' << cookie.host_only << '\n';
                return output.str();
            }

            void deserialize(const std::string_view data) {
                cookies_.clear();
                std::istringstream input{std::string(data)};
                Cookie cookie;
                while (input >> std::quoted(cookie.name) >> std::quoted(cookie.value) >>
                       std::quoted(cookie.domain) >> std::quoted(cookie.path) >>
                       cookie.secure >> cookie.host_only)
                    cookies_.push_back(cookie);
            }

            void clear() { cookies_.clear(); }

        private:
            std::vector<Cookie> cookies_;
        };

        [[nodiscard]] std::string extract_input_value(
            const std::string_view html, const std::string_view name) {
            const std::string marker = std::format("name=\"{}\"", name);
            const std::size_t marker_position = html.find(marker);
            if (marker_position == std::string_view::npos) {
                throw std::runtime_error(std::format("Missing form field: {}", name));
            }
            const std::size_t tag_start = html.rfind("<input", marker_position);
            const std::size_t tag_end = html.find('>', marker_position);
            if (tag_start == std::string_view::npos || tag_end == std::string_view::npos) {
                throw std::runtime_error("Malformed login form");
            }
            const std::string_view tag = html.substr(tag_start, tag_end - tag_start);
            constexpr std::string_view value_marker = "value=\"";
            const std::size_t value_start = tag.find(value_marker);
            if (value_start == std::string_view::npos) {
                return {};
            }
            const std::size_t content_start = value_start + value_marker.size();
            const std::size_t content_end = tag.find('"', content_start);
            if (content_end == std::string_view::npos) {
                throw std::runtime_error("Malformed form field value");
            }
            return std::string(tag.substr(content_start, content_end - content_start));
        }

        [[nodiscard]] boost::property_tree::ptree parse_json(
            const std::string_view json) {
            std::istringstream stream{std::string(json)};
            boost::property_tree::ptree tree;
            boost::property_tree::read_json(stream, tree);
            return tree;
        }

        [[nodiscard]] std::string json_message(
            const boost::property_tree::ptree &tree,
            const std::string_view fallback) {
            return tree.get("message", tree.get("msg", std::string(fallback)));
        }

        struct BioDeleter {
            void operator()(BIO *bio) const noexcept { BIO_free(bio); }
        };

        struct PkeyDeleter {
            void operator()(EVP_PKEY *key) const noexcept { EVP_PKEY_free(key); }
        };

        struct PkeyContextDeleter {
            void operator()(EVP_PKEY_CTX *context) const noexcept {
                EVP_PKEY_CTX_free(context);
            }
        };

        [[nodiscard]] std::string rsa_encrypt_password(
            const std::string_view public_key_pem,
            const std::string_view password) {
            const std::unique_ptr<BIO, BioDeleter> bio(BIO_new_mem_buf(
                public_key_pem.data(), static_cast<int>(public_key_pem.size())));
            if (!bio) {
                throw std::runtime_error("Cannot allocate OpenSSL key buffer");
            }
            std::unique_ptr<EVP_PKEY, PkeyDeleter> key(
                PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr));
            if (!key) {
                throw std::runtime_error("Cannot parse SSO public key");
            }
            const std::unique_ptr<EVP_PKEY_CTX, PkeyContextDeleter> context(
                EVP_PKEY_CTX_new(key.get(), nullptr));
            if (!context || EVP_PKEY_encrypt_init(context.get()) <= 0 ||
                EVP_PKEY_CTX_set_rsa_padding(context.get(), RSA_PKCS1_PADDING) <= 0) {
                throw std::runtime_error("Cannot initialize RSA encryption");
            }

            std::size_t encrypted_size = 0;
            const auto *plaintext = reinterpret_cast<const unsigned char *>(
                password.data());
            if (EVP_PKEY_encrypt(context.get(), nullptr, &encrypted_size, plaintext,
                                 password.size()) <= 0) {
                throw std::runtime_error("RSA password encryption failed");
            }
            std::vector<unsigned char> encrypted(encrypted_size);
            if (EVP_PKEY_encrypt(context.get(), encrypted.data(), &encrypted_size,
                                 plaintext, password.size()) <= 0) {
                throw std::runtime_error("RSA password encryption failed");
            }
            encrypted.resize(encrypted_size);

            std::string encoded(4 * ((encrypted.size() + 2) / 3), '\0');
            const int encoded_size = EVP_EncodeBlock(
                reinterpret_cast<unsigned char *>(encoded.data()),
                encrypted.data(), static_cast<int>(encrypted.size()));
            if (encoded_size <= 0) {
                throw std::runtime_error("Cannot encode encrypted password");
            }
            encoded.resize(static_cast<std::size_t>(encoded_size));
            return "__RSA__" + encoded;
        }
    } // namespace

    class SsoClient::Impl final {
    public:
        explicit Impl(const NetworkOptions &options) : tls_(ssl::context::tls_client) {
            if (!options.ca_file.empty()) {
                tls_.load_verify_file(options.ca_file);
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
            if (username.empty()) {
                throw std::runtime_error("A username is required");
            }
            if (cookies_ && active_username_ == username) {
                return;
            }
            active_username_ = std::string(username);
            cookies_ = std::make_unique<CookieJar>();
            final_url_.clear();
            updated_at_ = 0;
            cas_cookie_.clear();
        }

        [[nodiscard]] HttpResponse request(
            std::string method, std::string url, std::string body = {},
            const std::string_view content_type = {}) {
            for (int redirect_count = 0; redirect_count < 10; ++redirect_count) {
                const ParsedUrl parsed = parse_url(url);
                asio::io_context io_context;
                tcp::resolver resolver(io_context);
                std::unique_ptr<beast::ssl_stream<beast::tcp_stream> > stream;
                for (int connection_attempt = 0; connection_attempt < 3;
                     ++connection_attempt) {
                    stream = std::make_unique<
                        beast::ssl_stream<beast::tcp_stream> >(io_context, tls_);
                    if (!SSL_set_tlsext_host_name(stream->native_handle(),
                                                  parsed.host.c_str())) {
                        throw std::runtime_error("Cannot configure TLS SNI");
                    }
                    stream->set_verify_callback(
                        ssl::host_name_verification(parsed.host));
                    try {
                        const auto endpoints =
                                resolver.resolve(parsed.host, parsed.port);
                        beast::get_lowest_layer(*stream).expires_after(
                            std::chrono::seconds(30));
                        beast::get_lowest_layer(*stream).connect(endpoints);
                        stream->handshake(ssl::stream_base::client);
                        break;
                    } catch (const boost::system::system_error &) {
                        stream.reset();
                        if (connection_attempt == 2) throw;
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(
                                150 * (connection_attempt + 1)));
                    }
                }

                const http::verb verb = method == "POST"
                                            ? http::verb::post
                                            : http::verb::get;
                http::request<http::string_body> request{verb, parsed.target, 11};
                request.set(http::field::host, parsed.host);
                request.set(http::field::user_agent, "ZZUAssistant/1.0");
                request.set(http::field::accept, "*/*");
                const std::string cookie_header = cookies_
                                                      ? cookies_->header(parsed.host, parsed.target)
                                                      : std::string{};
                if (parsed.host == "cas.s.zzu.edu.cn") {
                    if (!cas_cookie_.empty()) {
                        request.set(http::field::cookie, cas_cookie_);
                    } else if (!cookie_header.empty()) {
                        request.set(http::field::cookie, cookie_header);
                    }
                } else if (!cookie_header.empty()) {
                    request.set(http::field::cookie, cookie_header);
                }
                if (verb == http::verb::post) {
                    request.set(http::field::content_type, content_type);
                    request.body() = body;
                    request.prepare_payload();
                }

                http::write(*stream, request);
                beast::flat_buffer buffer;
                http::response<http::string_body> response;
                http::read(*stream, buffer, response);

                for (const auto &field: response.base()) {
                    if (field.name() == http::field::set_cookie) {
                        if (cookies_) {
                            cookies_->accept(parsed.host, parsed.target,
                                             field.value());
                        }
                    }
                }

                beast::error_code shutdown_error;
                stream->shutdown(shutdown_error);
                if (shutdown_error == asio::error::eof ||
                    shutdown_error == ssl::error::stream_truncated) {
                    shutdown_error = {};
                }

                const unsigned status = response.result_int();
                if (status == 301 || status == 302 || status == 303 ||
                    status == 307 || status == 308) {
                    const auto location = response.base().find(http::field::location);
                    if (location == response.base().end()) {
                        throw std::runtime_error("Redirect has no Location header");
                    }
                    url = resolve_redirect(url, location->value());
                    if (status == 301 || status == 302 || status == 303) {
                        method = "GET";
                        body.clear();
                    }
                    continue;
                }
                return {status, std::move(response.body()), std::move(url)};
            }
            throw std::runtime_error("Too many SSO redirects");
        }

        void clear_cookies() {
            if (cookies_) {
                cookies_->clear();
            }
        }

        [[nodiscard]] Session session() const {
            return {active_username_, final_url_, updated_at_,
                    cookies_ ? cookies_->serialize() : std::string{}, cas_cookie_};
        }

        void login(const Session &session) {
            active_username_ = session.username;
            final_url_ = session.final_url;
            updated_at_ = session.updated_at;
            cas_cookie_ = session.cas_cookie;
            cookies_ = std::make_unique<CookieJar>();
            cookies_->deserialize(session.cookies);
        }

        void mark_authenticated(const std::string_view final_url) {
            final_url_ = final_url;
            updated_at_ = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }

        [[nodiscard]] std::vector<unsigned char> qr_image(
            const std::string_view source, const std::string_view base_url) {
            constexpr std::string_view data_prefix = "data:image/png;base64,";
            if (source.starts_with(data_prefix)) {
                return decode_base64(source.substr(data_prefix.size()));
            }
            std::string url;
            if (source.starts_with("https://")) {
                url = std::string(source);
            } else if (source.starts_with("//")) {
                url = "https:" + std::string(source);
            } else {
                url = resolve_redirect(base_url, source);
            }
            const HttpResponse response = request("GET", std::move(url));
            if (response.status != 200) {
                throw std::runtime_error(std::format(
                    "SSO QR image returned HTTP {}", response.status));
            }
            return {response.body.begin(), response.body.end()};
        }

    private:
        ssl::context tls_;
        std::string active_username_;
        std::unique_ptr<CookieJar> cookies_;
        std::string final_url_;
        std::int64_t updated_at_{};
        std::string cas_cookie_;
    };

    SsoClient::SsoClient(NetworkOptions options)
        : impl_(std::make_unique<Impl>(options)) {
    }

    SsoClient::~SsoClient() = default;

    SsoClient::SsoClient(SsoClient &&) noexcept = default;

    SsoClient &SsoClient::operator=(SsoClient &&) noexcept = default;

    LoginResult SsoClient::probe() {
        try {
            const HttpResponse page =
                    impl_->request("GET", std::string(model::sso::LOGIN_URL));
            if (page.status != 200) {
                return {
                    LoginStatus::protocol_error,
                    "The SSO endpoint is unavailable", page.final_url
                };
            }
            const HttpResponse public_key =
                    impl_->request("GET", std::string(model::sso::PUBKEY_URL));
            if (public_key.status != 200 ||
                !rsa_encrypt_password(public_key.body, "probe").starts_with(
                    "__RSA__")) {
                return {
                    LoginStatus::protocol_error,
                    "The SSO RSA public key is unusable", public_key.final_url
                };
            }
            return {
                LoginStatus::success,
                "SSO TLS, session endpoint and RSA public key are ready",
                page.final_url
            };
        } catch (const std::exception &error) {
            return {LoginStatus::network_error, error.what(), {}};
        }
    }

    LoginResult SsoClient::resume(const std::string_view service_url) {
        try {
            if (impl_->session().username.empty())
                return {LoginStatus::no_session,
                        "SSO client is not logged in", {}};
            std::string login_url(model::sso::LOGIN_URL);
            if (!service_url.empty()) {
                login_url += "?service=" + url_encode(service_url);
            }
            const HttpResponse response = impl_->request("GET", login_url);
            if (response.status >= 400) {
                return {
                    LoginStatus::network_error,
                    std::format("Session check returned HTTP {}",
                                response.status),
                    response.final_url
                };
            }
            if (response.body.find("name=\"execution\"") != std::string::npos) {
                return {
                    LoginStatus::no_session,
                    "No active cached SSO session", response.final_url
                };
            }
            impl_->mark_authenticated(response.final_url);
            return {
                LoginStatus::success, "Cached SSO session restored",
                response.final_url
            };
        } catch (const std::exception &error) {
            return {LoginStatus::network_error, error.what(), {}};
        }
    }

    LoginResult SsoClient::login(const LoginOptions &options) {
        try {
            if (options.username.empty() || options.password.empty()) {
                return {
                    LoginStatus::protocol_error,
                    "Username and password are required", {}
                };
            }

            impl_->select_user(options.username);

            std::string login_url(model::sso::LOGIN_URL);
            if (!options.service_url.empty()) {
                login_url += "?service=" + url_encode(options.service_url);
            }
            const HttpResponse page = impl_->request("GET", login_url);
            if (page.status != 200) {
                return {
                    LoginStatus::network_error,
                    std::format("Login page returned HTTP {}", page.status),
                    page.final_url
                };
            }
            if (page.body.find("name=\"execution\"") == std::string::npos) {
                impl_->mark_authenticated(page.final_url);
                return {
                    LoginStatus::success, "Existing SSO session is active",
                    page.final_url
                };
            }
            const std::string execution =
                    extract_input_value(page.body, "execution");
            const HttpResponse public_key =
                    impl_->request("GET", std::string(model::sso::PUBKEY_URL));
            if (public_key.status != 200) {
                return {
                    LoginStatus::network_error,
                    "Cannot obtain the SSO public key", public_key.final_url
                };
            }
            const std::string encrypted_password =
                    rsa_encrypt_password(public_key.body, options.password);

            const std::string detect_body =
                    "username=" + url_encode(options.username) +
                    "&password=" + url_encode(encrypted_password) +
                    "&fpVisitorId=";
            const HttpResponse detect = impl_->request(
                "POST", std::string(model::sso::MFA_DETECT_URL),
                detect_body, "application/x-www-form-urlencoded; charset=UTF-8");
            const auto detect_json = parse_json(detect.body);
            if (detect.status != 200 || detect_json.get<int>("code", -1) != 0) {
                return {
                    LoginStatus::rejected,
                    json_message(detect_json, "SSO rejected the credentials"),
                    detect.final_url
                };
            }

            const std::string mfa_state = detect_json.get<std::string>("data.state");
            if (detect_json.get<bool>("data.need", false)) {
                const bool phone_mfa =
                        options.mfa_method == MfaMethod::secure_phone;
                const std::string method = phone_mfa ? "securephone" : "qrcode";
                const HttpResponse init = impl_->request(
                    "GET", std::string(model::sso::MFA_INIT_BY_TYPE_URL) +
                           method + "?state=" +
                           url_encode(mfa_state));
                const auto init_json = parse_json(init.body);
                if (init.status != 200 || init_json.get<int>("code", -1) != 0) {
                    return {
                        LoginStatus::mfa_rejected,
                        json_message(init_json,
                                     phone_mfa
                                         ? "Security-phone verification is unavailable"
                                         : "Super APP QR verification is unavailable"),
                        init.final_url
                    };
                }
                const std::string attest_url =
                        init_json.get<std::string>("data.attestServerUrl");
                const std::string gid = init_json.get<std::string>("data.gid");
                const std::string gid_json =
                        "{\"gid\":\"" + json_escape(gid) + "\"}";
                const HttpResponse send = impl_->request(
                    "POST", attest_url +
                            std::string(model::sso::ATTEST_GUARD_PATH) +
                            method +
                            std::string(model::sso::ATTEST_SEND_PATH),
                    gid_json,
                    "application/json; charset=UTF-8");
                const auto send_json = parse_json(send.body);
                if (send.status != 200 || send_json.get<int>("code", -1) != 0) {
                    return {
                        LoginStatus::mfa_rejected,
                        json_message(send_json,
                                     phone_mfa
                                         ? "Cannot send the phone verification code"
                                         : "Cannot obtain a Super APP QR code"),
                        send.final_url
                    };
                }

                if (phone_mfa) {
                    if (options.notify) {
                        const std::string phone = init_json.get(
                            "data.securePhone", std::string{});
                        options.notify(phone.empty()
                                           ? "A phone verification code was sent."
                                           : "A phone verification code was sent to " +
                                             phone + '.');
                    }
                    if (!options.prompt) {
                        return {
                            LoginStatus::protocol_error,
                            "Phone MFA requires a verification-code prompt", {}
                        };
                    }
                    const std::string code =
                            options.prompt("Phone verification code: ");
                    if (code.empty()) {
                        return {
                            LoginStatus::mfa_rejected,
                            "A phone verification code is required", {}
                        };
                    }
                    const std::string valid_body =
                            "{\"gid\":\"" + json_escape(gid) +
                            "\",\"code\":\"" + json_escape(code) + "\"}";
                    const HttpResponse valid = impl_->request(
                        "POST", attest_url +
                                std::string(model::sso::PHONE_VALID_PATH),
                        valid_body, "application/json; charset=UTF-8");
                    const auto valid_json = parse_json(valid.body);
                    if (valid.status != 200 ||
                        valid_json.get<int>("code", -1) != 0 ||
                        valid_json.get<int>("data.status", -1) != 2) {
                        return {
                            LoginStatus::mfa_rejected,
                            json_message(valid_json,
                                         "The phone verification code was rejected"),
                            valid.final_url
                        };
                    }
                } else {
                    const std::string scan_qrcode =
                            send_json.get("data.scanQrcode", std::string{});
                    const std::string callback_code =
                            send_json.get("data.callbackCode", std::string{});
                    if (scan_qrcode.empty()) {
                        return {
                            LoginStatus::mfa_rejected,
                            "The SSO server did not return a QR image",
                            send.final_url
                        };
                    }
                    if (!options.display_qr) {
                        return {
                            LoginStatus::protocol_error,
                            "QR MFA requires a terminal QR renderer", {}
                        };
                    }
                    options.display_qr(
                        impl_->qr_image(scan_qrcode, attest_url));
                    if (options.notify) {
                        options.notify(callback_code.empty()
                                           ? "Scan the QR code with the Super APP and approve the login."
                                           : std::format(
                                               "Scan with the Super APP and verify code {}.",
                                               callback_code));
                    }

                    const auto deadline =
                            std::chrono::steady_clock::now() + options.mfa_timeout;
                    bool verified = false;
                    while (std::chrono::steady_clock::now() < deadline) {
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                        const HttpResponse poll = impl_->request(
                            "POST", attest_url +
                                    std::string(model::sso::QR_STATUS_PATH),
                            gid_json, "application/json; charset=UTF-8");
                        const auto poll_json = parse_json(poll.body);
                        if (poll.status != 200 ||
                            poll_json.get<int>("code", -1) != 0) {
                            return {
                                LoginStatus::mfa_rejected,
                                json_message(poll_json,
                                             "MFA status request failed"),
                                poll.final_url
                            };
                        }
                        const int status =
                                poll_json.get<int>("data.status", -1);
                        if (status == 2) {
                            verified = true;
                            break;
                        }
                        if (status == 3 || status == 5 || status == 9) {
                            return {
                                status == 9
                                    ? LoginStatus::mfa_expired
                                    : LoginStatus::mfa_rejected,
                                status == 5
                                    ? "MFA request was declined"
                                    : "MFA verification failed or expired",
                                poll.final_url
                            };
                        }
                    }
                    if (!verified) {
                        return {
                            LoginStatus::mfa_expired,
                            "Timed out waiting for Super APP confirmation", {}
                        };
                    }
                }
            }

            const std::string form =
                    "username=" + url_encode(options.username) +
                    "&password=" + url_encode(encrypted_password) +
                    "&captcha=&currentMenu=1&failN=-1&mfaState=" +
                    url_encode(mfa_state) + "&execution=" +
                    url_encode(execution) +
                    "&_eventId=submit&geolocation=&fpVisitorId=&trustAgent=";
            const HttpResponse submitted = impl_->request(
                "POST", login_url, form,
                "application/x-www-form-urlencoded; charset=UTF-8");
            if (submitted.status >= 400) {
                return {
                    LoginStatus::network_error,
                    std::format("Login submission returned HTTP {}",
                                submitted.status),
                    submitted.final_url
                };
            }
            if (submitted.body.find("name=\"execution\"") != std::string::npos) {
                const bool captcha_required =
                        submitted.body.find("casServerCaptchaShow = true") !=
                        std::string::npos;
                return {
                    captcha_required
                        ? LoginStatus::captcha_required
                        : LoginStatus::rejected,
                    captcha_required
                        ? "The server requires an image captcha"
                        : "SSO login was rejected",
                    submitted.final_url
                };
            }
            impl_->mark_authenticated(submitted.final_url);
            return {
                LoginStatus::success,
                "SSO login succeeded",
                submitted.final_url
            };
        } catch (const boost::property_tree::json_parser::json_parser_error &error) {
            return {
                LoginStatus::protocol_error,
                std::string("Invalid SSO JSON response: ") + error.what(), {}
            };
        } catch (const std::exception &error) {
            return {LoginStatus::network_error, error.what(), {}};
        }
    }

    LoginResult SsoClient::logout() {
        std::string selected_username;
        try {
            const auto current = impl_->session();
            if (current.username.empty()) {
                return {
                    LoginStatus::no_session,
                    "SSO client is not logged in", {}
                };
            }
            selected_username = current.username;
            const HttpResponse response = impl_->request(
                "GET", std::string(model::sso::LOGOUT_URL));
            impl_->clear_cookies();
            if (response.status >= 400) {
                return {
                    LoginStatus::network_error,
                    std::format(
                        "Local session cleared, but CAS logout returned HTTP {}",
                        response.status),
                    response.final_url
                };
            }
            return {
                LoginStatus::success,
                "CAS session cleared", response.final_url
            };
        } catch (const std::exception &error) {
            if (!selected_username.empty()) {
                try {
                    impl_->select_user(selected_username);
                    impl_->clear_cookies();
                } catch (...) {
                }
            }
            return {
                LoginStatus::network_error,
                std::string("Session cleared; remote CAS logout failed: ") +
                error.what(),
                {}
            };
        }
    }

    LoginResult SsoClient::login(const Session &session) {
        if (session.username.empty())
            return {LoginStatus::protocol_error, "Session username is required", {}};
        if (session.cookies.empty() && session.cas_cookie.empty())
            return {LoginStatus::no_session, "Session credentials are required", {}};
        impl_->login(session);
        return {LoginStatus::success, "SSO session restored", session.final_url};
    }

    Session SsoClient::session() const { return impl_->session(); }
} // namespace zzu_assistant::sso
