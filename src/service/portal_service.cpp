#include "service/portal_service.h"

#include "auth/environment.h"
#include "cli/console.h"
#include "model/portal.h"

#include <openssl/crypto.h>

#ifdef _WIN32
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

#include <iostream>
#include <stdexcept>
#include <string>

#include "model/constants.h"

namespace zzu_assistant::services {
    namespace {
        void usage(const ServiceContext &context) {
            context.out << cli::paint(model::constants::PORTAL,
                                      cli::Tone::cyan,
                                      context.color_enabled, true) << '\n'
                    << cli::paint("USAGE", cli::Tone::yellow,
                                  context.color_enabled, true)
                    << "\n  " << context.executable_name
                    << " portal discover [--porcelain]\n"
                    << "  " << context.executable_name
                    << " portal login [username] [--server URL] [--ip IPv4]\n"
                    "      [--isp cmcc|unicom|telecom|@suffix] [--encrypt]\n"
                    << "  " << context.executable_name
                    << " portal logout [username] [--server URL] [--ip IPv4]\n"
                    "      [--isp cmcc|unicom|telecom|@suffix]\n\n";
        }

        std::string hidden_password(std::ostream &output,
                                    const bool color_enabled) {
            cli::prompt(output, "Portal password", "hidden", color_enabled);
#ifdef _WIN32
            std::string password;
            for (;;) {
                const int character = _getch();
                if (character == '\r' || character == '\n') {
                    output << '\n';
                    return password;
                }
                if (character == 3) {
                    output << '\n';
                    throw std::runtime_error("Portal login cancelled");
                }
                if (character == '\b') {
                    if (!password.empty()) password.pop_back();
                } else if (character == 0 || character == 0xe0) {
                    static_cast<void>(_getch());
                } else {
                    password.push_back(static_cast<char>(character));
                }
            }
#else
    termios original{};
    const bool changed = tcgetattr(STDIN_FILENO, &original) == 0;
    if (changed) {
        termios hidden = original;
        hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden);
    }
    std::string password;
    const bool read = static_cast<bool>(std::getline(std::cin, password));
    if (changed) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
        output << '\n';
    }
    if (!read) throw std::runtime_error("Cannot read Portal password");
    return password;
#endif
        }

        std::string isp_suffix(const std::string_view value) {
            if (value.empty()) return {};
            if (value == "cmcc") return "@cmcc";
            if (value == "unicom") return "@unicom";
            if (value == "telecom") return "@telecom";
            if (value.starts_with('@')) return std::string(value);
            throw std::invalid_argument("Unknown ISP suffix: " + std::string(value));
        }
    } // namespace

    std::string_view PortalService::description() const noexcept {
        return "Manage network Portal";
    }

    int PortalService::execute(ServiceContext &context, Arguments arguments) {
        client_ = portal::PortalClient(cli::SessionStore::network_options());
        if (arguments.empty() || arguments.front() == "--help" ||
            arguments.front() == "-h") {
            usage(context);
            return arguments.empty() ? 2 : 0;
        }
        bool porcelain = false;
        try {
            if (arguments.front() == "discover") {
                if (arguments.size() == 2 && arguments[1] == "--porcelain")
                    porcelain = true;
                else if (arguments.size() != 1)
                    throw std::invalid_argument(
                        "portal discover accepts only --porcelain");
                if (!porcelain)
                    cli::heading(context.out, "Campus Portal", "Detecting gateway",
                                 context.color_enabled);
                const portal::PortalInfo info = client_.discover();
                if (porcelain) {
                    context.out << "auth_url=" << info.auth_url << '\n'
                            << "server_url=" << info.server_url << '\n'
                            << "user_ip=" << info.user_ip << '\n';
                } else {
                    cli::status(context.out, "OK", "Campus Portal detected",
                                cli::Tone::green, context.color_enabled);
                    cli::field(context.out, "Auth page", info.auth_url,
                               context.color_enabled);
                    cli::field(context.out, "Portal server", info.server_url,
                               context.color_enabled);
                    cli::field(context.out, "Client IPv4", info.user_ip,
                               context.color_enabled);
                }
                return 0;
            }
            const bool login = arguments.front() == "login";
            const bool logout = arguments.front() == "logout";
            if (!login && !logout) {
                usage(context);
                return 2;
            }

            std::size_t option_start = 1;
            std::string username;
            if (arguments.size() > 1 && !arguments[1].starts_with('-')) {
                username = arguments[1];
                option_start = 2;
            } else if (const auto configured = auth::current_user_override()) {
                username = *configured;
            }
            std::string server(model::portal::DEFAULT_SERVER_URL);
            std::string ip;
            std::string suffix;
            const auto saved_portal = username.empty()
                                          ? sessions_.current_user("portal")
                                          : std::optional<std::string>(username);
            if (saved_portal) {
                if (const auto restored = sessions_.load_portal(*saved_portal))
                    client_.login(*restored);
            }
            if (const auto current = client_.session();
                current && (username.empty() || username == current->username)) {
                if (username.empty()) username = current->username;
                server = current->server_url;
                ip = current->user_ip;
                suffix = current->isp_suffix;
            }
            if (username.empty())
                throw std::runtime_error(
                    "No current Portal user; provide a username for the first login");
            bool encrypt = false;
            for (std::size_t index = option_start; index < arguments.size(); ++index) {
                const std::string_view argument = arguments[index];
                const auto next = [&](const std::string_view option) {
                    if (++index >= arguments.size()) {
                        throw std::invalid_argument(std::string(option) +
                                                    " requires a value");
                    }
                    return arguments[index];
                };
                if (argument == "--server") server = next(argument);
                else if (argument.starts_with("--server=")) server = argument.substr(9);
                else if (argument == "--ip") ip = next(argument);
                else if (argument.starts_with("--ip=")) ip = argument.substr(5);
                else if (argument == "--isp") suffix = isp_suffix(next(argument));
                else if (argument.starts_with("--isp="))
                    suffix = isp_suffix(argument.substr(6));
                else if (argument == "--encrypt" && login) encrypt = true;
                else
                    throw std::invalid_argument("Unexpected argument: " +
                                                std::string(argument));
            }

            if (ip.empty()) {
                ip = portal::PortalClient::local_ipv4();
            }
            cli::heading(context.out, "Campus Portal",
                         login ? "Sign in" : "Sign out",
                         context.color_enabled);
            cli::field(context.out, "User", username, context.color_enabled);
            cli::field(context.out, "Client IPv4", ip, context.color_enabled);
            if (login && server.starts_with("http://")) {
                cli::status(context.err, "WARN",
                            "Portal uses HTTP; Base64/XOR does not protect the password",
                            cli::Tone::yellow, context.color_enabled);
            }
            if (logout) {
                const portal::AuthResult result = client_.logout();
                if (result.success) sessions_.remove_portal(username);
                cli::status(result.success ? context.out : context.err,
                            result.success ? "OK" : "ERROR", result.message,
                            result.success ? cli::Tone::green : cli::Tone::red,
                            context.color_enabled);
                return result.success ? 0 : 1;
            }
            std::string password = hidden_password(context.out,
                                                   context.color_enabled);
            try {
                const portal::AuthResult result = client_.login({
                    .server_url = server,
                    .user_ip = ip,
                    .username = username,
                    .password = password,
                    .isp_suffix = suffix,
                    .encrypt_parameters = encrypt,
                });
                if (result.success && client_.session())
                    sessions_.save(*client_.session());
                OPENSSL_cleanse(password.data(), password.size());
                cli::status(result.success ? context.out : context.err,
                            result.success ? "OK" : "ERROR", result.message,
                            result.success ? cli::Tone::green : cli::Tone::red,
                            context.color_enabled);
                return result.success ? 0 : 1;
            } catch (...) {
                OPENSSL_cleanse(password.data(), password.size());
                throw;
            }
        } catch (const std::exception &error) {
            cli::status(context.err, "ERROR", error.what(), cli::Tone::red,
                        context.color_enabled && !porcelain);
            return 1;
        }
    }
} // namespace zzu_assistant::services
