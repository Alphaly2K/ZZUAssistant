#include "service/completions_service.h"

#include "cli/console.h"

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace zzu_assistant::services {
    namespace {
        void usage(const ServiceContext &context) {
            context.out << cli::paint("USAGE", cli::Tone::yellow,
                                      context.color_enabled, true)
                    << "\n  " << context.executable_name
                    << " _completions <powershell|bash|zsh|fish>\n"
                    << "  " << context.executable_name
                    << " _completions powershell install <profile-path>\n"
                    << "  " << context.executable_name
                    << " _completions powershell uninstall <profile-path>\n"
                    << "  " << context.executable_name
                    << " _completions <bash|zsh|fish> <install|uninstall>\n"
                    << "      [config-path]\n\n"
                    << cli::paint("NOTES", cli::Tone::yellow,
                                  context.color_enabled, true)
                    << "\n  With no action, writes the script to stdout. PowerShell install\n"
                    << "  requires $PROFILE as <profile-path>.\n";
        }

        std::string single_quoted(std::string_view value) {
            std::string result("'");
            for (const char character: value) {
                if (character == '\'') result += "'\\''";
                else result += character;
            }
            result += '\'';
            return result;
        }

        std::string powershell_quoted(std::string_view value) {
            std::string result("'");
            for (const char character: value) {
                if (character == '\'') result += "''";
                else result += character;
            }
            result += '\'';
            return result;
        }

        void powershell(std::ostream &output, const std::string_view executable) {
            output << "Register-ArgumentCompleter -Native -CommandName "
                    << powershell_quoted(executable) << R"( -ScriptBlock {
    param($wordToComplete, $commandAst, $cursorPosition)
    $words = @($commandAst.CommandElements | ForEach-Object { $_.Extent.Text })
    $top = @('echo', 'electricity', 'ecard', 'course', 'sso', 'portal', 'app', 'help')
    $map = @{
        electricity = @('setup', 'show', 'recharge', '--help')
        ecard       = @('balance', 'recharge', '--help')
        course      = @('--semester', '-o', '--output', '--porcelain', '--help')
        sso         = @('login', 'logout', '--help')
        portal      = @('discover', 'login', 'logout', '--help')
        app         = @('login', 'logout', '--help')
    }
    if ($words.Count -le 2) { $candidates = $top }
    elseif ($words.Count -eq 3 -and $map.ContainsKey($words[1])) {
        $candidates = $map[$words[1]]
    } else {
        $candidates = switch ($words[1]) {
            'electricity' { @('-y', '--yes', '--porcelain', '--payment-password-env', 'lighting', 'air') }
            'ecard'       { @('-y', '--yes', '--porcelain') }
            'course'      { @('--semester', '-o', '--output', '--porcelain') }
            'sso'         { @('--mfa', '--mfa=phone', '--mfa=qr', '--porcelain') }
            'portal'      { @('--server', '--ip', '--isp', '--encrypt', '--porcelain') }
            default       { @() }
        }
    }
    $candidates | Where-Object { $_ -like "$wordToComplete*" } |
        ForEach-Object { [System.Management.Automation.CompletionResult]::new($_, $_, 'ParameterValue', $_) }
}
)";
        }

        constexpr std::string_view PROFILE_BEGIN =
                "# >>> ZZUAssistant completions >>>";
        constexpr std::string_view PROFILE_END =
                "# <<< ZZUAssistant completions <<<";

        std::string read_file(const std::filesystem::path &path) {
            std::ifstream input(path, std::ios::binary);
            if (!input) return {};
            return {
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>()
            };
        }

        void write_file(const std::filesystem::path &path,
                        const std::string_view contents) {
            if (path.empty()) throw std::invalid_argument("PowerShell profile path is empty");
            std::error_code error;
            if (!path.parent_path().empty())
                std::filesystem::create_directories(path.parent_path(), error);
            if (error)
                throw std::runtime_error("Cannot create the PowerShell profile directory");
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("Cannot write the PowerShell profile");
            output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            if (!output) throw std::runtime_error("Cannot finish writing the PowerShell profile");
        }

        bool replace_profile_block(std::string &profile,
                                   const std::string_view replacement) {
            const std::size_t begin = profile.find(PROFILE_BEGIN);
            const std::size_t end_marker = profile.find(PROFILE_END);
            if ((begin == std::string::npos) != (end_marker == std::string::npos))
                throw std::runtime_error(
                    "The PowerShell profile contains an incomplete ZZUAssistant block");
            if (begin == std::string::npos) return false;
            if (end_marker < begin)
                throw std::runtime_error(
                    "The PowerShell profile contains malformed ZZUAssistant markers");
            std::size_t end = end_marker + PROFILE_END.size();
            if (end < profile.size() && profile[end] == '\r') ++end;
            if (end < profile.size() && profile[end] == '\n') ++end;
            profile.replace(begin, end - begin, replacement);
            return true;
        }

        void install_powershell(const std::filesystem::path &path,
                                const std::string_view executable) {
            std::ostringstream generated;
            powershell(generated, executable);
            std::string block = std::string(PROFILE_BEGIN) + "\n" +
                                generated.str() + std::string(PROFILE_END) + "\n";
            std::string profile = read_file(path);
            if (!replace_profile_block(profile, block)) {
                if (!profile.empty() && !profile.ends_with('\n')) profile += '\n';
                if (!profile.empty()) profile += '\n';
                profile += block;
            }
            write_file(path, profile);
        }

        bool uninstall_powershell(const std::filesystem::path &path) {
            std::string profile = read_file(path);
            if (!replace_profile_block(profile, {})) return false;
            write_file(path, profile);
            return true;
        }

        void bash(ServiceContext &context) {
            context.out << R"(_zzuassistant_complete() {
    local cur="${COMP_WORDS[COMP_CWORD]}" top="echo electricity ecard course sso portal app help" words
    if (( COMP_CWORD == 1 )); then words="$top"
    elif (( COMP_CWORD == 2 )); then
        case "${COMP_WORDS[1]}" in
            electricity) words="setup show recharge --help" ;;
            ecard) words="balance recharge --help" ;;
            course) words="--semester -o --output --porcelain --help" ;;
            sso|app) words="login logout --help" ;;
            portal) words="discover login logout --help" ;;
        esac
    else
        case "${COMP_WORDS[1]}" in
            electricity) words="-y --yes --porcelain --payment-password-env lighting air" ;;
            ecard) words="-y --yes --porcelain" ;;
            course) words="--semester -o --output --porcelain" ;;
            sso) words="--mfa --mfa=phone --mfa=qr --porcelain" ;;
            portal) words="--server --ip --isp --encrypt --porcelain" ;;
        esac
    fi
    COMPREPLY=( $(compgen -W "$words" -- "$cur") )
}
complete -F _zzuassistant_complete )" << single_quoted(context.executable_name)
                    << '\n';
        }

        void zsh(ServiceContext &context) {
            context.out << "#compdef " << context.executable_name << R"(
autoload -Uz compinit
(( $+functions[compdef] )) || compinit
_zzuassistant() {
    local -a commands
    commands=(
        'echo:Print supplied text'
        'electricity:Configure, inspect and recharge electricity meters'
        'ecard:Query and recharge the campus card'
        'course:Export the course table as an ICS calendar'
        'sso:Manage Web SSO sessions'
        'portal:Manage campus Portal authentication'
        'app:Manage Super App authentication'
        'help:Show help'
    )
    if (( CURRENT == 2 )); then _describe 'command' commands; return; fi
    case $words[2] in
        electricity) _values 'argument' setup show recharge --help --porcelain -y --yes --payment-password-env ;;
        ecard) _values 'argument' balance recharge --help --porcelain -y --yes ;;
        course) _values 'argument' --semester -o --output --porcelain --help ;;
        sso) _values 'argument' login logout --help --mfa --porcelain ;;
        app) _values 'action' login logout --help ;;
        portal) _values 'argument' discover login logout --help --server --ip --isp --encrypt --porcelain ;;
    esac
}
compdef _zzuassistant )" << context.executable_name << '\n';
        }

        void fish(ServiceContext &context) {
            const std::string command(context.executable_name);
            const auto line = [&](const std::string_view arguments) {
                context.out << "complete -c " << command << ' ' << arguments << '\n';
            };
            line("-f -n '__fish_use_subcommand' -a 'echo electricity ecard course sso portal app help'");
            line("-f -n '__fish_seen_subcommand_from electricity' -a 'setup show recharge'");
            line("-f -n '__fish_seen_subcommand_from ecard' -a 'balance recharge'");
            line("-n '__fish_seen_subcommand_from course' -l semester -r");
            line("-n '__fish_seen_subcommand_from course' -s o -l output -r");
            line("-f -n '__fish_seen_subcommand_from sso app' -a 'login logout'");
            line("-f -n '__fish_seen_subcommand_from portal' -a 'discover login logout'");
            line("-n '__fish_seen_subcommand_from electricity ecard' -l yes -s y");
            line("-n '__fish_seen_subcommand_from electricity' -l payment-password-env -r");
            line("-n '__fish_seen_subcommand_from electricity ecard course sso portal' -l porcelain");
            line("-n '__fish_seen_subcommand_from sso' -l mfa -a 'phone qr' -r");
            line("-n '__fish_seen_subcommand_from portal' -l server -r");
            line("-n '__fish_seen_subcommand_from portal' -l ip -r");
            line("-n '__fish_seen_subcommand_from portal' -l isp -a 'cmcc unicom telecom' -r");
            line("-n '__fish_seen_subcommand_from portal' -l encrypt");
        }

        std::optional<std::string> environment(const char *name) {
#ifdef _WIN32
            char *value = nullptr;
            std::size_t size = 0;
            if (_dupenv_s(&value, &size, name) != 0 || value == nullptr)
                return std::nullopt;
            std::string result(value);
            std::free(value);
            return result;
#else
            if (const char *value = std::getenv(name); value && *value)
                return std::string(value);
            return std::nullopt;
#endif
        }

        std::filesystem::path default_config_path(
            const std::string_view shell, const std::string_view executable) {
            const auto home = environment("HOME");
            if (shell == "bash") {
                if (!home)
                    throw std::runtime_error(
                        "HOME is unavailable; provide the Bash config path explicitly");
                return std::filesystem::path(*home) / ".bashrc";
            }
            if (shell == "zsh") {
                if (!home)
                    throw std::runtime_error(
                        "HOME is unavailable; provide the Zsh config path explicitly");
                return std::filesystem::path(*home) / ".zshrc";
            }
            if (shell == "fish") {
                std::filesystem::path root;
                if (const auto xdg = environment("XDG_CONFIG_HOME")) root = *xdg;
                else if (home) root = std::filesystem::path(*home) / ".config";
                else
                    throw std::runtime_error(
                        "HOME and XDG_CONFIG_HOME are unavailable; provide the Fish completion path explicitly");
                return root / "fish" / "completions" /
                       (std::string(executable) + ".fish");
            }
            throw std::invalid_argument("Unsupported shell installer");
        }

        std::string generated_script(const std::string_view shell,
                                     const ServiceContext &context) {
            std::ostringstream output;
            ServiceContext generated{output, output, context.executable_name, false};
            if (shell == "bash") bash(generated);
            else if (shell == "zsh") zsh(generated);
            else if (shell == "fish") fish(generated);
            else throw std::invalid_argument("Unsupported shell: " + std::string(shell));
            return output.str();
        }

        void install_profile_script(const std::filesystem::path &path,
                                    const std::string_view script) {
            std::string block = std::string(PROFILE_BEGIN) + "\n" +
                                std::string(script) + std::string(PROFILE_END) + "\n";
            std::string profile = read_file(path);
            if (!replace_profile_block(profile, block)) {
                if (!profile.empty() && !profile.ends_with('\n')) profile += '\n';
                if (!profile.empty()) profile += '\n';
                profile += block;
            }
            write_file(path, profile);
        }

        bool uninstall_completion_file(const std::filesystem::path &path) {
            std::error_code error;
            const bool exists = std::filesystem::exists(path, error);
            if (error) throw std::runtime_error("Cannot inspect the completion file");
            if (!exists) return false;
            if (!std::filesystem::remove(path, error) || error)
                throw std::runtime_error("Cannot remove the completion file");
            return true;
        }
    } // namespace

    std::string_view CompletionsService::description() const noexcept {
        return "Generate shell completion definitions";
    }

    int CompletionsService::execute(ServiceContext &context, Arguments arguments) {
        if (arguments.empty() || arguments.front() == "--help" ||
            arguments.front() == "-h") {
            usage(context);
            return arguments.empty() ? 2 : 0;
        }
        try {
            const std::string_view shell = arguments.front();
            if (shell == "powershell" || shell == "pwsh") {
                if (arguments.size() == 1) {
                    powershell(context.out, context.executable_name);
                } else if (arguments.size() == 3 && arguments[1] == "install") {
                    install_powershell(std::filesystem::path(arguments[2]),
                                       context.executable_name);
                    cli::status(context.out, "OK",
                                "PowerShell completion installed; restart PowerShell or run '. $PROFILE'",
                                cli::Tone::green, context.color_enabled);
                } else if (arguments.size() == 3 && arguments[1] == "uninstall") {
                    const bool removed = uninstall_powershell(
                        std::filesystem::path(arguments[2]));
                    cli::status(context.out, removed ? "OK" : "INFO",
                                removed
                                    ? "PowerShell completion removed"
                                    : "No ZZUAssistant completion block was found",
                                removed ? cli::Tone::green : cli::Tone::cyan,
                                context.color_enabled);
                } else {
                    usage(context);
                    return 2;
                }
            } else if (shell == "bash" || shell == "zsh" || shell == "fish") {
                if (arguments.size() == 1) {
                    if (shell == "bash") bash(context);
                    else if (shell == "zsh") zsh(context);
                    else fish(context);
                } else if ((arguments.size() == 2 || arguments.size() == 3) &&
                           (arguments[1] == "install" ||
                            arguments[1] == "uninstall")) {
                    const std::filesystem::path path = arguments.size() == 3
                                                           ? std::filesystem::path(arguments[2])
                                                           : default_config_path(
                                                               shell, context.executable_name);
                    const bool install = arguments[1] == "install";
                    bool changed = true;
                    if (install) {
                        const std::string script = generated_script(shell, context);
                        if (shell == "fish") write_file(path, script);
                        else install_profile_script(path, script);
                    } else if (shell == "fish") {
                        changed = uninstall_completion_file(path);
                    } else {
                        changed = uninstall_powershell(path);
                    }
                    cli::status(context.out,
                                changed ? "OK" : "INFO",
                                changed
                                    ? std::string(install
                                                      ? "Completion installed at "
                                                      : "Completion removed from ") +
                                      path.string()
                                    : "No ZZUAssistant completion installation was found",
                                changed ? cli::Tone::green : cli::Tone::cyan,
                                context.color_enabled);
                } else {
                    usage(context);
                    return 2;
                }
            } else {
                cli::status(context.err, "ERROR", "Unsupported shell: " + std::string(shell),
                            cli::Tone::red, context.color_enabled);
                return 2;
            }
            return 0;
        } catch (const std::exception &error) {
            cli::status(context.err, "ERROR", error.what(), cli::Tone::red,
                        context.color_enabled);
            return 1;
        }
    }
} // namespace zzu_assistant::services
