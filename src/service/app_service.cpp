#include "service/app_service.h"

#include "cli/console.h"

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
            context.out
                    << cli::paint(model::constants::APP, cli::Tone::cyan,
                                  context.color_enabled, true) << '\n'
                    << cli::paint("USAGE", cli::Tone::yellow,
                                  context.color_enabled, true)
                    << "\n  " << context.executable_name
                    << " app login [username]\n  " << context.executable_name
                    << " app logout [username]\n\n"
                    << cli::paint("NOTES", cli::Tone::yellow,
                                  context.color_enabled, true)
                    << "\n  User priority: argument, ZZUASSISTANT_USER, saved App user.\n"
                    << "  ZZUASSISTANT_APP_TOKEN supplies an idToken for this process only.\n"
                    << "  Login uses phone-SMS MFA.\n"
                    << "  Set ZZUASSISTANT_APP_DEVICE_ID to reuse the phone device ID.\n";
        }

        std::string hidden_password(std::ostream &output) {
            output << "Super App password: " << std::flush;
#ifdef _WIN32
            std::string value;
            for (;;) {
                const int ch = _getch();
                if (ch == '\r' || ch == '\n') {
                    output << '\n';
                    return value;
                }
                if (ch == 3) throw std::runtime_error("App login cancelled");
                if (ch == '\b') { if (!value.empty()) value.pop_back(); } else if (ch == 0 || ch == 0xe0) static_cast<
                    void>(_getch());
                else value.push_back(static_cast<char>(ch));
            }
#else
            termios original{}; const bool changed = tcgetattr(STDIN_FILENO, &original) == 0;
            if (changed) { termios hidden = original; hidden.c_lflag &= ~ECHO; tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden); }
            std::string value; const bool read = static_cast<bool>(std::getline(std::cin, value));
            if (changed) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &original); output << '\n'; }
            if (!read) throw std::runtime_error("Cannot read Super App password"); return value;
#endif
        }
    }

    std::string_view AppService::description() const noexcept {
        return "Manage independent Super App JWT login and logout";
    }

    int AppService::execute(ServiceContext &context, Arguments arguments) {
        if (arguments.empty() || arguments.front() == "--help" || arguments.front() == "-h") {
            usage(context);
            return arguments.empty() ? 2 : 0;
        }
        const std::string_view action = arguments.front();
        if (action == "logout") {
            context.out << cli::paint(model::constants::APP_LOGOUT,
                                      cli::Tone::cyan,
                                      context.color_enabled, true) << '\n';
            if (arguments.size() > 2) {
                usage(context);
                return 2;
            }
            const auto result = client_.logout(arguments.size() == 2 ? arguments[1] : std::string_view{});
            cli::status(result.success ? context.out : context.err,
                        result.success ? "OK" : "ERROR", result.message,
                        result.success ? cli::Tone::green : cli::Tone::red,
                        context.color_enabled);
            return result.success ? 0 : 1;
        }
        if (action != "login" || arguments.size() > 2) {
            usage(context);
            return 2;
        }
        std::string password;
        context.out << cli::paint(model::constants::APP_LOGIN, cli::Tone::cyan, true) << '\n';
        try {
            std::string username;
            if (arguments.size() == 2) username = arguments[1];
            else username = client_.current_user().value_or("");
            if (username.empty())
                throw std::runtime_error(
                    "No current Super App user; provide a username for the first login");
            password = hidden_password(context.out);
            const auto result = client_.login({
                .username = username, .password = password,
                .notify = [&](const std::string_view message) {
                    cli::status(context.out, "MFA", message, cli::Tone::cyan, context.color_enabled);
                },
                .prompt = [&](const std::string_view prompt) {
                    context.out << prompt << std::flush;
                    std::string code;
                    if (!std::getline(std::cin, code)) throw std::runtime_error("Cannot read phone code");
                    return code;
                },
            });
            OPENSSL_cleanse(password.data(), password.size());
            cli::status(result.success ? context.out : context.err,
                        result.success ? "OK" : "ERROR", result.message,
                        result.success ? cli::Tone::green : cli::Tone::red,
                        context.color_enabled);
            return result.success ? 0 : 1;
        } catch (const std::exception &error) {
            if (!password.empty()) OPENSSL_cleanse(password.data(), password.size());
            cli::status(context.err, "ERROR", error.what(), cli::Tone::red, context.color_enabled);
            return 1;
        }
    }
} // namespace zzu_assistant::services
