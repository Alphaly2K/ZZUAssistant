#include "service/ecard_service.h"

#include "cli/console.h"
#include "cli/qr_renderer.h"
#include "auth/environment.h"

#include <cmath>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "model/constants.h"

namespace zzu_assistant::services {
    namespace {
        void usage(const ServiceContext &context) {
            context.out << cli::paint(model::constants::ECARD,
                                      cli::Tone::cyan,
                                      context.color_enabled, true) << '\n'
                    << cli::paint("USAGE", cli::Tone::yellow,
                                  context.color_enabled, true)
                    << "\n  " << context.executable_name
                    << " ecard balance [username] [--porcelain]\n  "
                    << context.executable_name
                    << " ecard recharge <amount-yuan> [username] [-y|--yes] [--porcelain]\n\n"
                    << cli::paint("OPTIONS", cli::Tone::yellow,
                                  context.color_enabled, true)
                    << "\n  -y, --yes     Skip confirmation.\n"
                    << "  --porcelain   Print key=value output.\n\n";
        }

        std::string resolve_username(cli::SessionStore &sessions,
                                     const std::string_view supplied) {
            return auth::resolve_username(supplied, sessions.current_user("app"), "App");
        }
    } // namespace

    std::string_view EcardService::description() const noexcept {
        return "Manage campus card";
    }

    int EcardService::execute(ServiceContext &context, Arguments arguments) {
        app_state_ = app::AppClient(cli::SessionStore::app_options());
        client_ = ecard::EcardClient(cli::SessionStore::network_options());
        if (arguments.empty() || arguments.front() == "--help" ||
            arguments.front() == "-h") {
            usage(context);
            return arguments.empty() ? 2 : 0;
        }
        bool porcelain = false;
        try {
            const std::string_view command = arguments.front();
            if (command != "balance" && command != "recharge")
                throw std::invalid_argument("Unknown eCard action: " +
                                            std::string(arguments.front()));
            bool assume_yes = false;
            std::vector<std::string_view> positionals;
            for (std::size_t index = 1; index < arguments.size(); ++index) {
                if (arguments[index] == "--porcelain") {
                    if (porcelain)
                        throw std::invalid_argument("--porcelain was specified more than once");
                    porcelain = true;
                } else if (arguments[index] == "-y" ||
                           arguments[index] == "--yes") {
                    assume_yes = true;
                } else if (!arguments[index].starts_with('-')) {
                    positionals.push_back(arguments[index]);
                } else {
                    throw std::invalid_argument("Unexpected argument: " +
                                                std::string(arguments[index]));
                }
            }
            if (command == "balance") {
                if (assume_yes)
                    throw std::invalid_argument(
                        "--yes is only valid for ecard recharge");
                if (positionals.size() > 1) {
                    usage(context);
                    return 2;
                }
            } else {
                if (positionals.empty() || positionals.size() > 2) {
                    usage(context);
                    return 2;
                }
                if (porcelain && !assume_yes)
                    throw std::invalid_argument(
                        "ecard recharge --porcelain requires --yes");
            }

            unsigned long amount = 0;
            if (command == "recharge") {
                std::size_t consumed = 0;
                amount = std::stoul(std::string(positionals[0]), &consumed);
                if (consumed != positionals[0].size() || amount == 0 ||
                    amount > 1000)
                    throw std::invalid_argument(
                        "Amount must be an integer from 1 to 1000 yuan");
            }
            const std::string_view supplied_username = command == "recharge"
                                                           ? (positionals.size() == 2
                                                                  ? positionals[1]
                                                                  : std::string_view{})
                                                           : (positionals.empty()
                                                                  ? std::string_view{}
                                                                  : positionals[0]);
            const std::string username = resolve_username(sessions_, supplied_username);
            const auto app_session = sessions_.load_app(username);
            if (!app_session) throw std::runtime_error(
                "No Super App session; run 'app login <username>' first");
            static_cast<void>(app_state_.login(*app_session));
            if (const auto ecard_session = sessions_.load_ecard(username))
                client_.login(*ecard_session);
            const double balance = app_state_.card_balance();
            if (!std::isfinite(balance) || balance < 0)
                throw std::runtime_error("Campus card balance response is invalid");
            if (command == "balance") {
                if (porcelain)
                    context.out << std::format("balance_yuan={:.2f}\n", balance);
                else {
                    cli::heading(context.out, "Campus card", "Balance",
                                 context.color_enabled);
                    cli::field(context.out, "User", username,
                               context.color_enabled);
                    cli::field(context.out, "Available",
                               std::format("{:.2f} yuan", balance),
                               context.color_enabled);
                }
                return 0;
            }

            if (!porcelain) {
                cli::heading(context.out, "Campus card", "Recharge order",
                             context.color_enabled);
                cli::field(context.out, "User", username,
                           context.color_enabled);
                cli::status(context.out, "INFO",
                            "Authorizing eCard for " + username,
                            cli::Tone::cyan, context.color_enabled);
            }
            client_.login(username, app_state_.id_token());
            sessions_.save(client_.session());
            const auto config = client_.campus_card_recharge_config();
            if (config.amounts_yuan.empty())
                throw std::runtime_error(
                    "eCard returned no available recharge amounts");
            bool allowed = false;
            for (const unsigned candidate: config.amounts_yuan)
                if (candidate == amount) allowed = true;
            if (!allowed) {
                std::string available;
                for (const unsigned candidate: config.amounts_yuan) {
                    if (!available.empty()) available += ", ";
                    available += std::to_string(candidate);
                }
                throw std::invalid_argument(std::format(
                    "Amount must be one of the server-provided values: {} yuan",
                    available));
            }

            if (!porcelain) {
                cli::field(context.out, "Balance",
                           std::format("{:.2f} yuan", balance),
                           context.color_enabled);
                cli::field(context.out, "Recharge",
                           std::format("{} yuan", amount),
                           context.color_enabled);
                if (!assume_yes) {
                    cli::prompt(context.out,
                                "Create a real payment order",
                                "type RECHARGE " + std::to_string(amount),
                                context.color_enabled);
                    std::string confirmation;
                    std::getline(std::cin, confirmation);
                    if (confirmation != "RECHARGE " + std::to_string(amount)) {
                        cli::status(context.err, "CANCEL",
                                    "Payment order was not created",
                                    cli::Tone::yellow, context.color_enabled);
                        return 2;
                    }
                }
            }

            const auto order = client_.campus_card_recharge(
                static_cast<unsigned>(amount));
            sessions_.save(client_.session());
            if (porcelain) {
                context.out << std::format("balance_yuan={:.2f}\n", balance)
                        << "amount_yuan=" << order.amount_yuan << '\n'
                        << "checkout_url=" << order.checkout_url << '\n';
            } else {
                cli::status(context.out, "OK", "Payment order created",
                            cli::Tone::green, context.color_enabled);
                cli::heading(context.out, "Scan to pay",
                             "Alipay or WeChat", context.color_enabled);
                cli::render_qr_text(context.out, order.checkout_url,
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
