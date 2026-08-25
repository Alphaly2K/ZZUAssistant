#include "service/userinfo_service.h"

#include "auth/environment.h"
#include "cli/console.h"
#include "model/userinfo.h"

#include <optional>
#include <stdexcept>
#include <string>

namespace zzu_assistant::services {
    namespace {
        void usage(const ServiceContext &context) {
            context.out
                    << cli::paint("USER INFO", cli::Tone::cyan,
                                  context.color_enabled, true) << '\n'
                    << cli::paint("USAGE", cli::Tone::yellow,
                                  context.color_enabled, true)
                    << "\n  " << context.executable_name
                    << " userinfo [username] [--porcelain]\n\n"
                    << cli::paint("NOTES", cli::Tone::yellow,
                                  context.color_enabled, true)
                    << "\n  Authenticates through the saved SSO session or "
                       "ZZUASSISTANT_SSO_TOKEN.\n"
                    << "  Username priority: argument, ZZUASSISTANT_USER, saved SSO user.\n";
        }

        void value(std::ostream &output, const std::string_view key,
                   const std::string_view data) {
            output << key << '=' << data << '\n';
        }
    } // namespace

    std::string_view UserInfoService::description() const noexcept {
        return "Get the current user's identity through Web SSO";
    }

    int UserInfoService::execute(ServiceContext &context, Arguments arguments) {
        if (!arguments.empty() && (arguments.front() == "--help" ||
                                   arguments.front() == "-h")) {
            usage(context);
            return 0;
        }
        bool porcelain = false;
        try {
            std::string_view explicit_user;
            for (const auto argument: arguments) {
                if (argument == "--porcelain") {
                    if (porcelain) throw std::invalid_argument(
                        "--porcelain was specified more than once");
                    porcelain = true;
                } else if (!argument.starts_with('-') && explicit_user.empty()) {
                    explicit_user = argument;
                } else {
                    throw std::invalid_argument("Unexpected userinfo argument: " +
                                                std::string(argument));
                }
            }

            std::optional<std::string> cached;
            if (const auto current = sso_client_.current_user())
                cached = current->username;
            const std::string username = auth::resolve_username(
                explicit_user, cached, "SSO");
            const auto result = sso_client_.resume(
                username, model::userinfo::SERVICE_URL);
            if (!result.succeeded())
                throw std::runtime_error(
                    "SSO authentication is unavailable: " + result.message +
                    "; run 'sso login " + username + "' first");
            const auto info = client_.parse_sso_redirect(result.final_url, username);
            if (porcelain) {
                value(context.out, "username", info.username);
                value(context.out, "name", info.name);
                value(context.out, "identity_type_code", info.identity_type_code);
                value(context.out, "identity_type_name", info.identity_type_name);
                value(context.out, "organization_code", info.organization_code);
                value(context.out, "organization_name", info.organization_name);
                value(context.out, "account_id", info.account_id);
                value(context.out, "user_id", info.user_id);
                value(context.out, "uid", info.uid);
                context.out << "expires_at=" << info.expires_at << '\n';
            } else {
                cli::status(context.out, "OK", "SSO identity verified",
                            cli::Tone::green, context.color_enabled);
                context.out << "Username:      " << info.username << '\n'
                        << "Name:          " << info.name << '\n'
                        << "Identity:      " << info.identity_type_name
                        << " (" << info.identity_type_code << ")\n"
                        << "Organization:  " << info.organization_name
                        << " (" << info.organization_code << ")\n";
            }
            return 0;
        } catch (const std::exception &error) {
            cli::status(context.err, "ERROR", error.what(), cli::Tone::red,
                        context.color_enabled && !porcelain);
            return 1;
        }
    }
} // namespace zzu_assistant::services
