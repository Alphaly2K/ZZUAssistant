#include "client/portal_client.h"

#include "model/portal.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <openssl/evp.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <format>
#include <iomanip>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace zzu_assistant::portal {
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

        ParsedUrl parse_url(const std::string_view url) {
            const std::size_t scheme_end = url.find("://");
            if (scheme_end == std::string_view::npos) {
                throw std::runtime_error("Portal URL must be absolute");
            }
            ParsedUrl parsed;
            parsed.scheme = std::string(url.substr(0, scheme_end));
            if (parsed.scheme != "http" && parsed.scheme != "https") {
                throw std::runtime_error("Portal URL must use HTTP or HTTPS");
            }
            const std::size_t authority_start = scheme_end + 3;
            const std::size_t path_start = url.find_first_of("/?", authority_start);
            const std::string_view authority = url.substr(
                authority_start,
                path_start == std::string_view::npos
                    ? url.size() - authority_start
                    : path_start - authority_start);
            const std::size_t colon = authority.rfind(':');
            if (colon == std::string_view::npos) {
                parsed.host = std::string(authority);
                parsed.port = parsed.scheme == "https" ? "443" : "80";
            } else {
                parsed.host = std::string(authority.substr(0, colon));
                parsed.port = std::string(authority.substr(colon + 1));
            }
            parsed.target = path_start == std::string_view::npos
                                ? "/"
                                : url[path_start] == '?'
                                      ? "/" + std::string(url.substr(path_start))
                                      : std::string(url.substr(path_start));
            if (parsed.host.empty()) {
                throw std::runtime_error("Portal URL has no host");
            }
            return parsed;
        }

        std::string origin(const ParsedUrl &url) {
            std::string result = url.scheme + "://" + url.host;
            if ((url.scheme == "http" && url.port != "80") ||
                (url.scheme == "https" && url.port != "443")) {
                result += ':' + url.port;
            }
            return result;
        }

        std::string resolve_url(const std::string_view current,
                                const std::string_view location) {
            if (location.starts_with("http://") || location.starts_with("https://")) {
                return std::string(location);
            }
            const ParsedUrl base = parse_url(current);
            if (location.starts_with("//")) {
                return base.scheme + ':' + std::string(location);
            }
            if (location.starts_with('/')) {
                return origin(base) + std::string(location);
            }
            const std::size_t slash = base.target.rfind('/');
            return origin(base) + base.target.substr(0, slash + 1) +
                   std::string(location);
        }

        std::string url_encode(const std::string_view value) {
            constexpr char digits[] = "0123456789ABCDEF";
            std::string result;
            for (const unsigned char byte: value) {
                if (std::isalnum(byte) != 0 || byte == '-' || byte == '_' ||
                    byte == '.' || byte == '~') {
                    result.push_back(static_cast<char>(byte));
                } else {
                    result.push_back('%');
                    result.push_back(digits[byte >> 4]);
                    result.push_back(digits[byte & 15]);
                }
            }
            return result;
        }

        std::string url_decode(const std::string_view value) {
            const auto hex = [](const char value) -> int {
                if (value >= '0' && value <= '9') return value - '0';
                if (value >= 'a' && value <= 'f') return value - 'a' + 10;
                if (value >= 'A' && value <= 'F') return value - 'A' + 10;
                return -1;
            };
            std::string result;
            for (std::size_t index = 0; index < value.size(); ++index) {
                if (value[index] == '%' && index + 2 < value.size()) {
                    const int high = hex(value[index + 1]);
                    const int low = hex(value[index + 2]);
                    if (high >= 0 && low >= 0) {
                        result.push_back(static_cast<char>((high << 4) | low));
                        index += 2;
                        continue;
                    }
                }
                result.push_back(value[index] == '+' ? ' ' : value[index]);
            }
            return result;
        }

        std::string query_value(const std::string_view url,
                                const std::string_view key) {
            const std::size_t question = url.find('?');
            if (question == std::string_view::npos) return {};
            std::string_view query = url.substr(question + 1);
            while (!query.empty()) {
                const std::size_t separator = query.find('&');
                const std::string_view item = query.substr(0, separator);
                const std::size_t equals = item.find('=');
                if (url_decode(item.substr(0, equals)) == key) {
                    return equals == std::string_view::npos
                               ? std::string{}
                               : url_decode(item.substr(equals + 1));
                }
                if (separator == std::string_view::npos) break;
                query.remove_prefix(separator + 1);
            }
            return {};
        }

        std::string base64_encode(const std::string_view value) {
            std::string encoded(4 * ((value.size() + 2) / 3), '\0');
            const int size = EVP_EncodeBlock(
                reinterpret_cast<unsigned char *>(encoded.data()),
                reinterpret_cast<const unsigned char *>(value.data()),
                static_cast<int>(value.size()));
            if (size < 0) throw std::runtime_error("Cannot Base64-encode Portal password");
            encoded.resize(static_cast<std::size_t>(size));
            return encoded;
        }

        std::string xor_encrypt(const std::string_view value,
                                const std::string_view ip) {
            unsigned char key = 0;
            for (const unsigned char byte: ip) key ^= byte;
            constexpr char digits[] = "0123456789abcdef";
            std::string result;
            result.reserve(value.size() * 2);
            for (const unsigned char byte: value) {
                const unsigned char encrypted = byte ^ key;
                result.push_back(digits[encrypted >> 4]);
                result.push_back(digits[encrypted & 15]);
            }
            return result;
        }

        std::string first_anchor_href(const std::string &html) {
            const std::regex pattern(
                R"(<a\b[^>]*\bhref\s*=\s*(["'])(.*?)\1)",
                std::regex::icase);
            std::smatch match;
            return std::regex_search(html, match, pattern)
                       ? match[2].str()
                       : std::string{};
        }

        std::string jsonp_payload(const std::string_view response) {
            const std::size_t open = response.find('(');
            const std::size_t close = response.rfind(')');
            if (open == std::string_view::npos || close <= open) {
                throw std::runtime_error("Portal returned malformed JSONP");
            }
            return std::string(response.substr(open + 1, close - open - 1));
        }
    } // namespace

    class PortalClient::Impl final {
    public:
        explicit Impl(const NetworkOptions &options) : tls_(ssl::context::tls_client) {
            if (!options.ca_file.empty()) {
                tls_.load_verify_file(options.ca_file);
#ifdef ZZU_DEFAULT_CA_FILE
            } else tls_.load_verify_file(ZZU_DEFAULT_CA_FILE);
#else
            } else tls_.set_default_verify_paths();
#endif
            tls_.set_verify_mode(ssl::verify_peer);
        }

        HttpResponse get(std::string url, const std::string_view bind_ip = {}) {
            for (int redirects = 0; redirects < 10; ++redirects) {
                const ParsedUrl parsed = parse_url(url);
                HttpResponse response = parsed.scheme == "https"
                                            ? get_https(parsed, url, bind_ip)
                                            : get_http(parsed, url, bind_ip);
                if (response.status != 301 && response.status != 302 &&
                    response.status != 303 && response.status != 307 &&
                    response.status != 308) {
                    return response;
                }
                if (redirect_location_.empty()) {
                    throw std::runtime_error("Portal redirect has no Location");
                }
                url = resolve_url(url, redirect_location_);
            }
            throw std::runtime_error("Too many Portal redirects");
        }

    private:
        void bind(beast::tcp_stream &stream, const std::string_view ip) {
            if (ip.empty()) return;
            boost::system::error_code error;
            const auto address = asio::ip::make_address(std::string(ip), error);
            if (error || !address.is_v4()) {
                throw std::runtime_error("Portal bind address is not a valid IPv4 address");
            }
            stream.socket().open(tcp::v4(), error);
            if (!error) stream.socket().bind({address.to_v4(), 0}, error);
            if (error) {
                // The redirect may expose an address owned by an upstream router.
                // Keep it in wlan_user_ip but fall back to the OS-selected source,
                // matching the reference client's force_bind behavior.
                boost::system::error_code ignored;
                stream.socket().close(ignored);
            }
        }

        template<typename Stream>
        HttpResponse exchange(Stream &stream, const ParsedUrl &parsed,
                              const std::string &url) {
            http::request<http::empty_body> request{
                http::verb::get,
                parsed.target, 11
            };
            request.set(http::field::host, parsed.host);
            request.set(http::field::user_agent, "ZZUAssistant/1.0");
            request.set(http::field::accept, "*/*");
            http::write(stream, request);
            beast::flat_buffer buffer;
            http::response<http::string_body> response;
            http::read(stream, buffer, response);
            const auto location = response.base().find(http::field::location);
            redirect_location_ = location == response.base().end()
                                     ? std::string{}
                                     : std::string(location->value());
            return {response.result_int(), std::move(response.body()), url};
        }

        HttpResponse get_http(const ParsedUrl &parsed, const std::string &url,
                              const std::string_view bind_ip) {
            asio::io_context context;
            tcp::resolver resolver(context);
            beast::tcp_stream stream(context);
            bind(stream, bind_ip);
            stream.expires_after(std::chrono::seconds(10));
            stream.connect(resolver.resolve(parsed.host, parsed.port));
            HttpResponse result = exchange(stream, parsed, url);
            beast::error_code ignored;
            stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
            return result;
        }

        HttpResponse get_https(const ParsedUrl &parsed, const std::string &url,
                               const std::string_view bind_ip) {
            asio::io_context context;
            tcp::resolver resolver(context);
            beast::ssl_stream<beast::tcp_stream> stream(context, tls_);
            if (!SSL_set_tlsext_host_name(stream.native_handle(),
                                          parsed.host.c_str())) {
                throw std::runtime_error("Cannot configure Portal TLS SNI");
            }
            stream.set_verify_callback(ssl::host_name_verification(parsed.host));
            bind(beast::get_lowest_layer(stream), bind_ip);
            beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(10));
            beast::get_lowest_layer(stream).connect(
                resolver.resolve(parsed.host, parsed.port));
            stream.handshake(ssl::stream_base::client);
            HttpResponse result = exchange(stream, parsed, url);
            beast::error_code ignored;
            stream.shutdown(ignored);
            return result;
        }

        ssl::context tls_;
        std::string redirect_location_;
    };

    PortalClient::PortalClient(NetworkOptions options)
        : impl_(std::make_unique<Impl>(options)) {
    }

    PortalClient::~PortalClient() = default;

    PortalClient::PortalClient(PortalClient &&) noexcept = default;

    PortalClient &PortalClient::operator=(PortalClient &&) noexcept = default;

    std::string PortalClient::local_ipv4() {
        try {
            asio::io_context context;
            asio::ip::udp::socket socket(context);
            socket.open(asio::ip::udp::v4());
            socket.connect({asio::ip::make_address_v4("8.8.8.8"), 80});
            return socket.local_endpoint().address().to_string();
        } catch (...) {
            return {};
        }
    }

    PortalInfo PortalClient::discover() {
        const HttpResponse probe =
                impl_->get(std::string(model::portal::CONNECTIVITY_PROBE_URL));
        if (probe.final_url.starts_with("https://")) {
            throw std::runtime_error(
                "No Portal interception detected; the network may already be authenticated");
        }
        std::string portal_url =
                probe.final_url != model::portal::CONNECTIVITY_PROBE_URL
                    ? probe.final_url
                    : first_anchor_href(probe.body);
        if (portal_url.empty()) {
            throw std::runtime_error("Cannot find a Portal authentication URL");
        }
        if (!portal_url.starts_with("http://") &&
            !portal_url.starts_with("https://")) {
            portal_url = resolve_url(probe.final_url, portal_url);
        }
        std::string user_ip = query_value(portal_url, "userip");
        if (user_ip.empty()) user_ip = query_value(portal_url, "wlanuserip");
        if (user_ip.empty()) {
            throw std::runtime_error("Portal redirect does not contain a user IP");
        }
        const ParsedUrl portal = parse_url(portal_url);
        const std::string auth_url = origin(portal);
        std::string server_url = "http://" + portal.host + ":801";
        try {
            const HttpResponse config = impl_->get(
                auth_url + std::string(model::portal::CONFIG_SCRIPT_PATH));
            const auto value = [&](const std::string &name, const int fallback) {
                std::smatch match;
                const std::regex pattern("var\\s+" + name + "\\s*=\\s*(\\d+)\\s*;");
                return std::regex_search(config.body, match, pattern)
                           ? std::stoi(match[1].str())
                           : fallback;
            };
            if (value("enableHttps", 0) == 0) {
                server_url = "http://" + portal.host + ':' +
                             std::to_string(value("epHTTPPort", 801));
            } else {
                server_url = "https://" + portal.host + ':' +
                             std::to_string(value("enHTTPSPort", 802));
            }
        } catch (...) {
            // Older portal deployments do not expose a41.js; port 801 is their
            // conventional endpoint and matches the reference implementation.
        }
        return {auth_url, server_url, user_ip};
    }

    AuthResult PortalClient::login(const AuthOptions &options) {
        if (options.server_url.empty() || options.username.empty() ||
            options.password.empty()) {
            throw std::runtime_error("Portal server, username and password are required");
        }
        const std::string ip = options.user_ip.empty()
                                   ? local_ipv4()
                                   : std::string(options.user_ip);
        if (ip.empty()) {
            throw std::runtime_error("Cannot determine the Portal client IPv4 address");
        }
        const std::string account = std::string(options.username) +
                                    std::string(options.isp_suffix);
        const std::string encoded_password = base64_encode(options.password);
        const auto encode = [&](const std::string_view value) {
            return options.encrypt_parameters
                       ? xor_encrypt(value, ip)
                       : std::string(value);
        };
        std::random_device seed;
        std::mt19937 generator(seed());
        std::uniform_int_distribution value(500, 10499);
        std::vector<std::pair<std::string, std::string> > parameters{
            {"callback", encode("dr1003")},
            {"login_method", encode("1")},
            {"user_account", encode(",0," + account)},
            {"user_password", encode(encoded_password)},
            {"wlan_user_ip", encode(ip)},
            {"wlan_user_ipv6", ""},
            {"wlan_user_mac", encode("000000000000")},
            {"wlan_vlan_id", encode("0")},
            {"wlan_ac_ip", ""}, {"wlan_ac_name", ""},
            {"authex_enable", ""}, {"jsVersion", encode("4.2.2")},
            {"terminal_type", encode("3")}, {"lang", encode("zh-cn")},
        };
        if (options.encrypt_parameters) parameters.emplace_back("encrypt", "1");
        parameters.emplace_back("v", std::to_string(value(generator)));
        parameters.emplace_back("lang", "zh");

        std::string url(options.server_url);
        while (url.ends_with('/')) url.pop_back();
        url += model::portal::LOGIN_PATH;
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            if (index != 0) url += '&';
            url += url_encode(parameters[index].first) + '=' +
                    url_encode(parameters[index].second);
        }
        const HttpResponse response = impl_->get(url, ip);
        if (response.status >= 400) {
            throw std::runtime_error(std::format(
                "Portal login returned HTTP {}", response.status));
        }
        std::istringstream json(jsonp_payload(response.body));
        boost::property_tree::ptree root;
        boost::property_tree::read_json(json, root);
        AuthResult result;
        result.result = root.get<int>("result", 0);
        result.success = result.result == 1;
        result.message = root.get("msg", std::string{"Portal returned no message"});
        if (const auto code = root.get_optional<int>("ret_code")) {
            result.return_code = *code;
        } else if (const auto code = root.get_optional<int>("retCode")) {
            result.return_code = *code;
        }
        if (result.success) {
            session_ = Session{
                std::string(options.username),
                std::string(options.server_url), ip,
                std::string(options.isp_suffix)
            };
        }
        return result;
    }

    AuthResult PortalClient::logout() {
        if (!session_)
            return {false, 0, std::nullopt,
                    "Portal client is not logged in"};
        const Session &options = *session_;
        if (options.server_url.empty() || options.username.empty())
            throw std::runtime_error("Portal server and username are required");
        const std::string ip = options.user_ip.empty()
                                   ? local_ipv4()
                                   : std::string(options.user_ip);
        if (ip.empty())
            throw std::runtime_error("Cannot determine the Portal client IPv4 address");
        const std::string account = std::string(options.username) +
                                    std::string(options.isp_suffix);
        std::random_device seed;
        std::mt19937 generator(seed());
        std::uniform_int_distribution value(500, 10499);
        std::vector<std::pair<std::string, std::string> > parameters{
            {"callback", "dr1004"}, {"login_method", "1"},
            {"user_account", ",0," + account}, {"wlan_user_ip", ip},
            {"wlan_user_ipv6", ""}, {"wlan_vlan_id", "0"},
            {"wlan_ac_name", ""}, {"jsVersion", "4.2.2"},
            {"terminal_type", "3"}, {"lang", "zh-cn"},
            {"v", std::to_string(value(generator))}, {"lang", "zh"},
        };
        std::string url(options.server_url);
        while (url.ends_with('/')) url.pop_back();
        url += model::portal::LOGOUT_PATH;
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            if (index) url += '&';
            url += url_encode(parameters[index].first) + '=' +
                    url_encode(parameters[index].second);
        }
        const HttpResponse response = impl_->get(url, ip);
        if (response.status >= 400)
            throw std::runtime_error(std::format(
                "Portal logout returned HTTP {}", response.status));
        std::istringstream json(jsonp_payload(response.body));
        boost::property_tree::ptree root;
        boost::property_tree::read_json(json, root);
        AuthResult result;
        result.result = root.get<int>("result", 0);
        result.success = result.result == 1;
        result.message = root.get("msg", std::string{"Portal returned no message"});
        if (const auto code = root.get_optional<int>("ret_code"))
            result.return_code = *code;
        else if (const auto code = root.get_optional<int>("retCode"))
            result.return_code = *code;
        if (result.success) session_.reset();
        return result;
    }

    std::optional<Session> PortalClient::session() const {
        return session_;
    }

    void PortalClient::login(const Session &session) {
        if (session.username.empty() || session.server_url.empty())
            throw std::invalid_argument(
                "Portal session username and server URL are required");
        session_ = session;
    }
} // namespace zzu_assistant::portal
