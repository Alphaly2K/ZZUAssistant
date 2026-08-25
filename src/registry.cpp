#include "registry.h"

#include "cli/console.h"

#include <boost/describe/members.hpp>
#include <boost/mp11/algorithm.hpp>

#include <concepts>
#include <string>
#include <type_traits>

namespace zzu_assistant {
    namespace {
        using RegisteredServices = boost::describe::describe_members<
            Registry, boost::describe::mod_private>;

        template<typename Descriptor>
        using ReflectedService = std::remove_cvref_t<decltype(
            std::declval<Registry &>().*Descriptor::pointer)>;

        static_assert(boost::mp11::mp_all_of<
            RegisteredServices,
            std::is_class>::value);
    } // namespace

    int Registry::dispatch(ServiceContext &context, Arguments arguments) {
        if (arguments.empty() || arguments.front() == "help" ||
            arguments.front() == "--help" || arguments.front() == "-h") {
            print_usage(context.out, context.executable_name, context.color_enabled);
            return 0;
        }

        const std::string_view command = arguments.front();
        const Arguments service_arguments = arguments.subspan(1);
        int result = 0;
        bool found = false;

        boost::mp11::mp_for_each<RegisteredServices>([&](auto descriptor) {
            using Descriptor = decltype(descriptor);
            using ConcreteService = ReflectedService<Descriptor>;
            static_assert(std::derived_from<ConcreteService, Service>,
                          "Every reflected Registry member must derive from Service");

            if (!found && command == Descriptor::name) {
                Service &service = this->*Descriptor::pointer;
                result = service.execute(context, service_arguments);
                found = true;
            }
        });

        if (!found) {
            cli::status(context.err, "ERROR",
                        "Unknown command: " + std::string(command),
                        cli::Tone::red, context.color_enabled);
            context.err << '\n';
            print_usage(context.err, context.executable_name,
                        context.color_enabled);
            return 2;
        }

        return result;
    }

    void Registry::print_usage(std::ostream &output,
                               const std::string_view executable_name,
                               const bool color_enabled) const {
        output << cli::paint(model::constants::APP_LOGO, cli::Tone::cyan,
                             color_enabled, true) << "\n\n"
                << cli::paint("USAGE", cli::Tone::yellow, color_enabled, true)
                << "\n  " << executable_name << " <command> [arguments...]\n\n"
                << cli::paint("COMMANDS", cli::Tone::yellow, color_enabled, true)
                << '\n';

        boost::mp11::mp_for_each<RegisteredServices>([&](auto descriptor) {
            using Descriptor = decltype(descriptor);
            using ConcreteService = ReflectedService<Descriptor>;
            static_assert(std::derived_from<ConcreteService, Service>,
                          "Every reflected Registry member must derive from Service");

            if (std::string_view(Descriptor::name).starts_with('_')) return;
            const Service &service = this->*Descriptor::pointer;
            const std::string_view name = Descriptor::name;
            constexpr std::size_t command_width = 14;
            const std::size_t padding = name.size() < command_width
                                            ? command_width - name.size()
                                            : 1;
            output << "  "
                    << cli::paint(name, cli::Tone::green,
                                  color_enabled, true)
                    << std::string(padding, ' ')
                    << service.description() << '\n';
        });

        constexpr std::string_view help = "help";
        constexpr std::size_t command_width = 14;
        output << "  " << cli::paint(help, cli::Tone::green,
                                     color_enabled, true)
                << std::string(command_width - help.size(), ' ')
                << "Show help\n\n";
    }
} // namespace zzu_assistant
