#include "service/electricity_service.h"

#include "cli/console.h"

#include <openssl/crypto.h>
#ifdef _WIN32
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <format>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "model/constants.h"

namespace zzu_assistant::services {
    namespace {
        void usage(const ServiceContext &context) {
            context.out << cli::paint(model::constants::ELECTRICITY,
                                      cli::Tone::cyan,
                                      context.color_enabled, true) << '\n'
                    << cli::paint("USAGE", cli::Tone::yellow,
                                  context.color_enabled, true)
                    << "\n  " << context.executable_name
                    << " electricity setup [username] [-y|--yes]\n"
                    << "  " << context.executable_name
                    << " electricity show [username] [--porcelain]\n"
                    << "  " << context.executable_name
                    << " electricity recharge <lighting|air> <amount-yuan> [username]\n"
                    << "      [-y|--yes] [--payment-password-env NAME]\n\n"
                    << cli::paint("OPTIONS", cli::Tone::yellow,
                                  context.color_enabled, true)
                    << "\n"
                    << "  -y, --yes                     Skip confirmation.\n"
                    << "  --payment-password-env NAME   Read payment password from NAME.\n"
                    << "  --porcelain                   Print key=value output.\n\n";
        }

        std::string resolve_username(app::AppClient &state,
                                     const std::string_view supplied = {}) {
            if (!supplied.empty()) return std::string(supplied);
            const auto current = state.current_user();
            if (!current) {
                throw std::runtime_error(
                    "No current Super App user; provide a username or run 'app login' first");
            }
            return *current;
        }

        std::string hidden_payment_password(std::ostream &output,
                                            const bool color_enabled) {
            cli::prompt(output, "eCard payment password", "hidden",
                        color_enabled);
#ifdef _WIN32
            std::string password;
            for (;;) {
                const int character = _getch();
                if (character == '\r' || character == '\n') {
                    output << '\n';
                    return password;
                }
                if (character == 3) throw std::runtime_error("Recharge cancelled");
                if (character == '\b') { if (!password.empty()) password.pop_back(); } else if (
                    character == 0 || character == 0xe0) static_cast<void>(_getch());
                else password.push_back(static_cast<char>(character));
            }
#else
            termios original{}; const bool changed = tcgetattr(STDIN_FILENO, &original) == 0;
            if (changed) { termios hidden = original; hidden.c_lflag &= ~ECHO;
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden); }
            std::string password; const bool read = static_cast<bool>(std::getline(std::cin, password));
            if (changed) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &original); output << '\n'; }
            if (!read) throw std::runtime_error("Cannot read the eCard payment password");
            return password;
#endif
        }

        std::optional<std::string> environment_variable(const std::string &name) {
            if (name.empty()) return std::nullopt;
#ifdef _WIN32
            char *value = nullptr;
            std::size_t size = 0;
            if (_dupenv_s(&value, &size, name.c_str()) != 0 || value == nullptr)
                return std::nullopt;
            std::string result(value);
            OPENSSL_cleanse(value, size);
            std::free(value);
            return result;
#else
            if (const char *value = std::getenv(name.c_str()))
                return std::string(value);
            return std::nullopt;
#endif
        }

        std::string ascii_lower(std::string value) {
            std::ranges::transform(value, value.begin(), [](const unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            return value;
        }

        void set_location(ecard::LocationPath &path, const std::string_view type,
                          const std::string &value) {
            if (type == "bigArea") path.big_area = value;
            else if (type == "area") path.area = value;
            else if (type == "building") path.building = value;
            else if (type == "unit") path.unit = value;
            else if (type == "level") path.level = value;
            else if (type == "room") path.room = value;
            else if (type == "subArea") path.sub_area = value;
            else
                throw std::runtime_error("Unsupported eCard location type: " +
                                         std::string(type));
        }

        ecard::LocationOption choose_option(
            ServiceContext &context, const std::string_view profile,
            const std::string_view type,
            const std::vector<ecard::LocationOption> &options) {
            if (options.empty())
                throw std::runtime_error("The eCard server returned no location options");

            std::vector<std::size_t> visible(options.size());
            for (std::size_t index = 0; index < visible.size(); ++index)
                visible[index] = index;

            for (;;) {
                context.out << '\n';
                cli::heading(context.out, std::format("{} / {}", profile, type),
                             "Choose a location", context.color_enabled);
                if (visible.size() <= 20) {
                    for (std::size_t index = 0; index < visible.size(); ++index) {
                        context.out << "  "
                                << cli::paint(std::format("{:>2}", index + 1),
                                              cli::Tone::cyan,
                                              context.color_enabled, true)
                                << "  "
                                << options[visible[index]].name << '\n';
                    }
                    cli::prompt(context.out, "Selection", "number or name",
                                context.color_enabled);
                } else {
                    cli::prompt(context.out,
                                std::format("Search {} choices", visible.size()),
                                "exact name or keyword", context.color_enabled);
                }

                std::string input;
                if (!std::getline(std::cin, input))
                    throw std::runtime_error("Location setup was cancelled");
                if (input.empty()) continue;

                try {
                    std::size_t used = 0;
                    const unsigned long selection = std::stoul(input, &used);
                    if (used == input.size() && selection >= 1 &&
                        selection <= visible.size()) {
                        return options[visible[selection - 1]];
                    }
                } catch (const std::exception &) {
                    // Treat non-numeric input as a name filter.
                }

                const std::string needle = ascii_lower(input);
                std::vector<std::size_t> matches;
                for (std::size_t index = 0; index < options.size(); ++index) {
                    const std::string candidate = ascii_lower(options[index].name);
                    if (candidate == needle) return options[index];
                    if (candidate.find(needle) != std::string::npos)
                        matches.push_back(index);
                }
                if (matches.empty()) {
                    cli::status(context.err, "WARN", "No matching location",
                                cli::Tone::yellow, context.color_enabled);
                    visible.resize(options.size());
                    for (std::size_t index = 0; index < visible.size(); ++index)
                        visible[index] = index;
                } else {
                    visible = std::move(matches);
                }
            }
        }

        ecard::LocationPath configure_profile(ServiceContext &context,
                                              ecard::EcardClient &client,
                                              const std::string_view label) {
            ecard::LocationPath path;
            std::string type = "area";
            for (unsigned depth = 0; depth < 10; ++depth) {
                const ecard::LocationPage page = client.locations(type, path);
                const ecard::LocationOption option =
                        choose_option(context, label, page.location_type, page.options);
                set_location(path, page.location_type, option.id);
                if (page.end) return path;
                if (page.next_location_type.empty()) {
                    throw std::runtime_error(
                        "The eCard location response has no nextLocationType");
                }
                type = page.next_location_type;
            }
            throw std::runtime_error("The eCard location tree is unexpectedly deep");
        }

        void print_reading(ServiceContext &context, const std::string_view label,
                           const ecard::ElectricityReading &reading) {
            std::string value = std::format("{:.2f} kWh", reading.quantity_kwh);
            if (reading.price_yuan_per_kwh > 0)
                value += std::format("  ·  {:.2f} yuan/kWh",
                                     reading.price_yuan_per_kwh);
            cli::field(context.out, label, value, context.color_enabled);
        }

        bool confirm(ServiceContext &context) {
            cli::prompt(context.out, "Save both electricity profiles", "y/N",
                        context.color_enabled);
            std::string answer;
            if (!std::getline(std::cin, answer)) return false;
            answer = ascii_lower(answer);
            return answer == "y" || answer == "yes";
        }
    } // namespace

    std::string_view ElectricityService::description() const noexcept {
        return "Manage electricity";
    }

    int ElectricityService::execute(ServiceContext &context, Arguments arguments) {
        if (arguments.empty() || arguments.front() == "--help" ||
            arguments.front() == "-h") {
            usage(context);
            return arguments.empty() ? 2 : 0;
        }
        bool porcelain = false;
        try {
            const std::string command(arguments.front());
            const bool recharge = command == "recharge";
            if (command != "setup" && command != "show" && !recharge) {
                throw std::invalid_argument("Unknown electricity action: " + command);
            }

            bool assume_yes = false;
            std::optional<std::string> payment_password_environment;
            std::vector<std::string_view> positionals;
            for (std::size_t index = 1; index < arguments.size(); ++index) {
                const std::string_view argument = arguments[index];
                if (argument == "-y" || argument == "--yes") {
                    assume_yes = true;
                } else if (argument == "--porcelain") {
                    if (porcelain)
                        throw std::invalid_argument("--porcelain was specified more than once");
                    porcelain = true;
                } else if (argument == "--payment-password-env") {
                    if (++index == arguments.size())
                        throw std::invalid_argument(
                            "--payment-password-env requires a variable name");
                    payment_password_environment = std::string(arguments[index]);
                } else if (argument.starts_with("--payment-password-env=")) {
                    payment_password_environment = std::string(
                        argument.substr(std::string_view("--payment-password-env=").size()));
                    if (payment_password_environment->empty())
                        throw std::invalid_argument(
                            "--payment-password-env requires a variable name");
                } else if (argument.starts_with('-')) {
                    throw std::invalid_argument("Unknown electricity option: " +
                                                std::string(argument));
                } else {
                    positionals.push_back(argument);
                }
            }

            const std::size_t minimum = recharge ? 2 : 0;
            const std::size_t maximum = recharge ? 3 : 1;
            if (positionals.size() < minimum || positionals.size() > maximum) {
                usage(context);
                return 2;
            }
            if (command != "setup" && assume_yes && !recharge)
                throw std::invalid_argument("--yes is only valid for setup and recharge");
            if (!recharge && payment_password_environment)
                throw std::invalid_argument(
                    "--payment-password-env is only valid for recharge");
            if (porcelain && command != "show")
                throw std::invalid_argument("--porcelain is only valid for show");

            unsigned long amount = 0;
            std::string_view meter;
            if (recharge) {
                meter = positionals[0];
                if (meter != "lighting" && meter != "air")
                    throw std::invalid_argument("Meter must be 'lighting' or 'air'");
                std::size_t consumed = 0;
                amount = std::stoul(std::string(positionals[1]), &consumed);
                if (consumed != positionals[1].size() || amount == 0 || amount > 1000)
                    throw std::invalid_argument(
                        "Amount must be an integer from 1 to 1000 yuan");
                if (assume_yes && !payment_password_environment)
                    payment_password_environment = std::string(
                        ecard::PAYMENT_PASSWORD_ENVIRONMENT);
            }

            const std::string_view supplied_username = recharge
                                                           ? (positionals.size() == 3
                                                                  ? positionals[2]
                                                                  : std::string_view{})
                                                           : (positionals.empty()
                                                                  ? std::string_view{}
                                                                  : positionals[0]);
            const std::string username = resolve_username(app_state_, supplied_username);

            client_.select_user(username);
            if (!porcelain) {
                const std::string_view subtitle = recharge ? "Recharge" :
                                                  command == "show" ? "Balance" :
                                                  "Room setup";
                cli::heading(context.out, "Electricity", subtitle,
                             context.color_enabled);
                cli::field(context.out, "User", username,
                           context.color_enabled);
                cli::status(context.out, "INFO", "Authorizing eCard for " + username,
                            cli::Tone::cyan, context.color_enabled);
            }
            client_.authorize(app_state_.id_token(username));

            if (recharge) {
                ecard::ElectricityProfiles profiles;
                if (!client_.load_profiles(profiles))
                    throw std::runtime_error("No electricity profiles; run electricity setup first");
                const ecard::LocationPath &profile = meter == "lighting"
                                                         ? profiles.lighting
                                                         : profiles.air_conditioning;
                const double card_balance = app_state_.card_balance(username);
                if (!std::isfinite(card_balance) || card_balance < 0)
                    throw std::runtime_error("Campus card balance response is invalid");
                const auto before = client_.account(profile);
                cli::field(context.out, "Card balance",
                           std::format("{:.2f} yuan", card_balance),
                           context.color_enabled);
                print_reading(context, meter == "lighting" ? "Lighting current" : "Air current", before);
                if (card_balance + 0.0001 < static_cast<double>(amount))
                    throw std::runtime_error(std::format(
                        "Insufficient campus card balance: {:.2f} yuan available, {} yuan required",
                        card_balance, amount));
                cli::field(context.out, "Target",
                           meter == "lighting" ? "Lighting" : "Air conditioning",
                           context.color_enabled);
                cli::field(context.out, "Amount",
                           std::format("{} yuan", amount),
                           context.color_enabled);
                cli::field(context.out, "Room ID", profile.room,
                           context.color_enabled);
                if (!assume_yes) {
                    cli::prompt(context.out, "Submit a real payment",
                                "type RECHARGE " + std::to_string(amount),
                                context.color_enabled);
                    std::string confirmation;
                    std::getline(std::cin, confirmation);
                    if (confirmation != "RECHARGE " + std::to_string(amount)) {
                        cli::status(context.err, "CANCEL", "Recharge was not submitted",
                                    cli::Tone::yellow, context.color_enabled);
                        return 2;
                    }
                }
                std::string payment_password;
                if (payment_password_environment) {
                    auto supplied = environment_variable(*payment_password_environment);
                    if (!supplied || supplied->empty())
                        throw std::runtime_error(std::format(
                            "Payment-password environment variable {} is missing or empty",
                            *payment_password_environment));
                    payment_password = std::move(*supplied);
                } else {
                    payment_password = hidden_payment_password(
                        context.out, context.color_enabled);
                }
                try {
                    const auto result = client_.recharge(
                        profile, payment_password, static_cast<unsigned>(amount));
                    OPENSSL_cleanse(payment_password.data(), payment_password.size());
                    cli::status(result.success ? context.out : context.err,
                                result.success ? "OK" : "ERROR", result.message,
                                result.success ? cli::Tone::green : cli::Tone::red,
                                context.color_enabled);
                    return result.success ? 0 : 1;
                } catch (...) {
                    OPENSSL_cleanse(payment_password.data(), payment_password.size());
                    throw;
                }
            }

            if (command == "show") {
                ecard::ElectricityProfiles profiles;
                if (!client_.load_profiles(profiles))
                    throw std::runtime_error(
                        "No electricity profiles; run electricity setup first");
                const auto lighting = client_.account(profiles.lighting);
                const auto air = client_.account(profiles.air_conditioning);
                if (porcelain) {
                    context.out
                            << std::format("lighting_quantity_kwh={:.6f}\n",
                                           lighting.quantity_kwh)
                            << std::format("lighting_price_yuan_per_kwh={:.6f}\n",
                                           lighting.price_yuan_per_kwh)
                            << std::format("air_conditioning_quantity_kwh={:.6f}\n",
                                           air.quantity_kwh)
                            << std::format("air_conditioning_price_yuan_per_kwh={:.6f}\n",
                                           air.price_yuan_per_kwh);
                } else {
                    print_reading(context, "Lighting", lighting);
                    print_reading(context, "Air conditioning", air);
                }
                return 0;
            }

            cli::status(context.out, "STEP", "Configure the lighting meter first",
                        cli::Tone::cyan, context.color_enabled);
            context.out << "  Select the server level that represents lighting electricity.\n";
            ecard::ElectricityProfiles profiles;
            profiles.lighting = configure_profile(context, client_, "Lighting");
            const auto lighting = client_.account(profiles.lighting);
            print_reading(context, "Lighting check", lighting);

            context.out << '\n';
            cli::status(context.out, "STEP", "Configure the air-conditioning meter",
                        cli::Tone::cyan, context.color_enabled);
            context.out << "  Select the server level that represents air-conditioning electricity.\n";
            profiles.air_conditioning =
                    configure_profile(context, client_, "Air conditioning");
            const auto air = client_.account(profiles.air_conditioning);
            print_reading(context, "Air-conditioning check", air);

            if (!assume_yes && !confirm(context)) {
                cli::status(context.err, "CANCEL", "Profiles were not changed",
                            cli::Tone::yellow, context.color_enabled);
                return 2;
            }
            client_.save_profiles(profiles);
            cli::status(context.out, "OK",
                        "Lighting and air-conditioning profiles saved",
                        cli::Tone::green, context.color_enabled);
            return 0;
        } catch (const std::exception &error) {
            cli::status(context.err, "ERROR", error.what(), cli::Tone::red,
                        context.color_enabled && !porcelain);
            return 1;
        }
    }
} // namespace zzu_assistant::services
