#include "service/sso_service.h"

#include "cli/console.h"
#include "cli/qr_renderer.h"
#include "model/constants.h"

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

namespace zzu_assistant::services {
    namespace {
        [[nodiscard]] std::string read_hidden_password(
            std::ostream &output, const bool color_enabled) {
            cli::prompt(output, "SSO password", "hidden", color_enabled);
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
                    throw std::runtime_error("Login cancelled");
                }
                if (character == '\b') {
                    if (!password.empty()) {
                        password.pop_back();
                    }
                    continue;
                }
                if (character == 0 || character == 0xe0) {
                    static_cast<void>(_getch());
                    continue;
                }
                password.push_back(static_cast<char>(character));
            }
#else
    termios original{};
    const bool terminal_changed = tcgetattr(STDIN_FILENO, &original) == 0;
    if (terminal_changed) {
        termios hidden = original;
        hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden);
    }

    std::string password;
    const bool read = static_cast<bool>(std::getline(std::cin, password));
    if (terminal_changed) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
        output << '\n';
    }
    if (!read) {
        throw std::runtime_error("Cannot read password");
    }
    return password;
#endif
        }

        void usage(const ServiceContext &context) {
            context.out
                    << cli::paint(model::constants::SSO_LOGIN, cli::Tone::cyan,
                                  context.color_enabled, true) << '\n'
                    << cli::paint("USAGE", cli::Tone::yellow,
                                  context.color_enabled, true)
                    << "\n  " << context.executable_name
                    << " sso login [username] [service-url] [--mfa phone|qr]\n"
                    << "  " << context.executable_name
                    << " sso login --probe [--porcelain]\n"
                    << "  " << context.executable_name
                    << " sso logout [username]\n\n"
                    << cli::paint("OPTIONS", cli::Tone::yellow,
                                  context.color_enabled, true)
                    << "\n  --mfa phone|qr   Select phone SMS or terminal QR.\n"
                    << "  --porcelain      Machine-readable probe output.\n\n"
                    << cli::paint("NOTES", cli::Tone::yellow,
                                  context.color_enabled, true)
                    << "\n  User priority: argument, ZZUASSISTANT_USER, saved SSO user.\n"
                    << "  ZZUASSISTANT_SSO_TOKEN supplies a TGC/Cookie for this process only.\n"
                    << "  Passwords are never saved.\n";
        }
    } // namespace

    std::string_view SSOService::description() const noexcept {
        return "Manage independent Web SSO/CAS login and logout";
    }

    int SSOService::execute(ServiceContext &context, Arguments arguments) {
        if (arguments.empty() || arguments.front() == "--help" ||
            arguments.front() == "-h") {
            usage(context);
            return arguments.empty() ? 2 : 0;
        }

        const std::string_view action = arguments.front();
        arguments = arguments.subspan(1);
        if (action == "logout") {
            if (arguments.size() > 1) {
                usage(context);
                return 2;
            }
            cli::heading(context.out, "Web SSO", "Sign out",
                         context.color_enabled);
            const std::string_view username = arguments.empty()
                                                  ? std::string_view{}
                                                  : arguments.front();
            const sso::LoginResult result = client_.logout(username);
            cli::status(result.succeeded() ? context.out : context.err,
                        result.succeeded() ? "OK" : "ERROR", result.message,
                        result.succeeded() ? cli::Tone::green : cli::Tone::red,
                        context.color_enabled);
            return result.succeeded() ? 0 : 1;
        }
        if (action != "login") {
            cli::status(context.err, "ERROR",
                        "Unknown SSO action: " + std::string(action),
                        cli::Tone::red, context.color_enabled);
            usage(context);
            return 2;
        }

        bool probe = false;
        bool porcelain = false;
        bool invalid_probe_argument = false;
        bool other_probe_argument = false;
        for (const std::string_view argument: arguments) {
            if (argument == "--probe") {
                if (probe) invalid_probe_argument = true;
                probe = true;
            } else if (argument == "--porcelain") {
                if (porcelain) invalid_probe_argument = true;
                porcelain = true;
            } else {
                other_probe_argument = true;
            }
        }
        if (probe) {
            if (invalid_probe_argument || other_probe_argument) {
                cli::status(context.err, "ERROR",
                            "SSO probe accepts only --probe and --porcelain",
                            cli::Tone::red, false);
                return 2;
            }
            const sso::LoginResult result = client_.probe();
            if (result.succeeded() && porcelain)
                context.out << "ready=true\n";
            else
                cli::status(result.succeeded() ? context.out : context.err,
                            result.succeeded() ? "OK" : "ERROR", result.message,
                            result.succeeded() ? cli::Tone::green : cli::Tone::red,
                            context.color_enabled && !porcelain);
            return result.succeeded() ? 0 : 1;
        }
        if (porcelain) {
            cli::status(context.err, "ERROR",
                        "--porcelain is only valid with --probe",
                        cli::Tone::red, false);
            return 2;
        }

        cli::heading(context.out, "Web SSO", "CAS sign in",
                     context.color_enabled);

        std::string password;
        try {
            std::string username;
            std::size_t option_start = 0;
            if (!arguments.empty() && !arguments.front().starts_with('-')) {
                username = arguments.front();
                option_start = 1;
            } else if (const auto current = client_.current_user()) {
                username = current->username;
            }
            if (username.empty())
                throw std::runtime_error(
                    "No current SSO user; provide a username for the first login");
            cli::field(context.out, "User", username, context.color_enabled);
            std::string_view service_url;
            sso::MfaMethod mfa_method = sso::MfaMethod::secure_phone;
            for (std::size_t index = option_start; index < arguments.size(); ++index) {
                const std::string_view argument = arguments[index];
                if (argument == "--mfa") {
                    if (++index >= arguments.size()) {
                        throw std::invalid_argument("--mfa requires phone or qr");
                    }
                    const std::string_view value = arguments[index];
                    if (value == "phone") {
                        mfa_method = sso::MfaMethod::secure_phone;
                    } else if (value == "qr") {
                        mfa_method = sso::MfaMethod::qr_code;
                    } else {
                        throw std::invalid_argument("Unknown MFA method: " +
                                                    std::string(value));
                    }
                } else if (argument.starts_with("--mfa=")) {
                    const std::string_view value = argument.substr(6);
                    if (value == "phone") {
                        mfa_method = sso::MfaMethod::secure_phone;
                    } else if (value == "qr") {
                        mfa_method = sso::MfaMethod::qr_code;
                    } else {
                        throw std::invalid_argument("Unknown MFA method: " +
                                                    std::string(value));
                    }
                } else if (service_url.empty()) {
                    service_url = argument;
                } else {
                    throw std::invalid_argument("Unexpected argument: " +
                                                std::string(argument));
                }
            }

            const sso::LoginResult cached =
                    client_.resume(username, service_url);
            if (cached.succeeded()) {
                cli::status(context.out, "SESSION", cached.message,
                            cli::Tone::green, context.color_enabled);
                if (!cached.final_url.empty()) {
                    cli::field(context.out, "Final URL", cached.final_url,
                               context.color_enabled);
                }
                return 0;
            }
            if (cached.status != sso::LoginStatus::no_session) {
                cli::status(context.err, "ERROR",
                            "Cannot restore SSO session: " + cached.message,
                            cli::Tone::red, context.color_enabled);
                return 1;
            }

            password = read_hidden_password(context.out, context.color_enabled);
            const sso::LoginOptions options{
                .username = username,
                .password = password,
                .service_url = service_url,
                .mfa_method = mfa_method,
                .notify = [&](const std::string_view message) {
                    cli::status(context.out, "MFA", message,
                                cli::Tone::cyan, context.color_enabled);
                },
                .prompt = [&](const std::string_view prompt) {
                    cli::prompt(context.out, prompt.ends_with(": ")
                                                 ? prompt.substr(0, prompt.size() - 2)
                                                 : prompt,
                                "SMS", context.color_enabled);
                    std::string value;
                    if (!std::getline(std::cin, value)) {
                        throw std::runtime_error(
                            "Cannot read the phone verification code");
                    }
                    return value;
                },
                .display_qr = [&](const std::vector<unsigned char> &png) {
                    cli::render_qr_png(context.out, png,
                                       context.color_enabled);
                },
            };
            const sso::LoginResult result = client_.login(options);
            if (!password.empty()) {
                OPENSSL_cleanse(password.data(), password.size());
            }

            if (result.succeeded()) {
                cli::status(context.out, "OK", result.message,
                            cli::Tone::green, context.color_enabled);
                if (!result.final_url.empty()) {
                    cli::field(context.out, "Final URL", result.final_url,
                               context.color_enabled);
                }
                return 0;
            }

            cli::status(context.err, "ERROR",
                        "SSO login failed: " + result.message,
                        cli::Tone::red, context.color_enabled);
            return result.status == sso::LoginStatus::captcha_required ? 3 : 1;
        } catch (const std::exception &error) {
            if (!password.empty()) {
                OPENSSL_cleanse(password.data(), password.size());
            }
            cli::status(context.err, "ERROR", error.what(), cli::Tone::red,
                        context.color_enabled);
            return 1;
        }
    }
} // namespace zzu_assistant::services
