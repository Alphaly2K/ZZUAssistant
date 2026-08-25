#include "../../include/service/echo_service.h"

#include "cli/console.h"

#include <thread>
#include <chrono>
#include <boost/utility/string_view_fwd.hpp>

namespace zzu_assistant::services {
    std::string_view EchoService::description() const noexcept {
        return "Print text";
    }

    inline constexpr std::string_view CHARACTERS = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

    int EchoService::execute(ServiceContext &context, Arguments arguments) {
        using namespace std::chrono_literals;

        if (arguments.size() == 1 &&
            (arguments.front() == "--help" || arguments.front() == "-h")) {
            context.out << cli::paint("USAGE", cli::Tone::yellow,
                                      context.color_enabled, true)
                    << "\n  " << context.executable_name << " echo [text...]\n\n";
            return 0;
        }

        for (std::size_t index = 0; index < arguments.size(); ++index) {
            if (index != 0) {
                context.out << ' ';
            }

            for (const char ch: arguments[index]) {
                bool found = false;
                for (const char curr: CHARACTERS) {
                    if (ch == curr) {
                        context.out << curr;
                        context.out.flush();
                        found = true;
                        break;
                    }
                    context.out << curr;
                    context.out.flush();
                    std::this_thread::sleep_for(15ms);
                    context.out << '\b';
                }
                if (!found) {
                    context.out << ch;
                    context.out.flush();
                }
            }
        }
        context.out << '\n';
        context.out.flush();
        return 0;
    }
} // namespace zzu_assistant::services
