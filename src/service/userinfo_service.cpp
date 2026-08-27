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
            context.out << cli::paint("USAGE", cli::Tone::yellow,
                                  context.color_enabled, true)
                    << "\n  " << context.executable_name
                    << " userinfo [username] [--porcelain]\n\n";
        }

        void value(std::ostream &output, const std::string_view key,
                   const std::string_view data) {
            output << key << '=' << data << '\n';
        }
    } // namespace

    std::string_view UserInfoService::description() const noexcept {
        return "Show SSO user information";
    }

    int UserInfoService::execute(ServiceContext &context, Arguments arguments) {
        sso_client_ = sso::SsoClient(cli::SessionStore::network_options());
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

            const std::string username = auth::resolve_username(
                explicit_user, sessions_.current_user("sso"), "SSO");
            if (const auto saved = sessions_.load_sso(username))
                sso_client_.login(*saved);
            const auto result = sso_client_.resume(model::userinfo::SERVICE_URL);
            if (!result.succeeded())
                throw std::runtime_error(
                    "SSO authentication is unavailable: " + result.message +
                    "; run 'sso login " + username + "' first");
            sessions_.save(sso_client_.session());
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
                cli::heading(context.out, "User information",
                             "Verified through Web SSO",
                             context.color_enabled);
                cli::status(context.out, "OK", "CAS identity ticket accepted",
                            cli::Tone::green, context.color_enabled);
                cli::field(context.out, "Username", info.username,
                           context.color_enabled);
                cli::field(context.out, "Name", info.name,
                           context.color_enabled);
                cli::field(context.out, "Identity",
                           info.identity_type_name + " (" +
                           info.identity_type_code + ")",
                           context.color_enabled);
                cli::field(context.out, "Organization",
                           info.organization_name + " (" +
                           info.organization_code + ")",
                           context.color_enabled);
            }
            return 0;
        } catch (const std::exception &error) {
            cli::status(context.err, "ERROR", error.what(), cli::Tone::red,
                        context.color_enabled && !porcelain);
            return 1;
        }
    }
} // namespace zzu_assistant::services
