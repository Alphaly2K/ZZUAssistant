#include "service/course_service.h"

#include "cli/console.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <process.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#else
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

namespace zzu_assistant::services {
    namespace {
        void usage(const ServiceContext &context) {
            context.out
                    << cli::paint("USAGE", cli::Tone::yellow,
                                  context.color_enabled, true)
                    << "\n  " << context.executable_name
                    << " course [username] [--semester <current|id|code>]"
                    " [-o|--output <file>] [--porcelain]\n\n"
                    << cli::paint("OPTIONS", cli::Tone::yellow,
                                  context.color_enabled, true)
                    << "\n  --semester VALUE   Semester ID/code; default is current.\n"
                    << "  -o, --output FILE   ICS path; default is course-<semester>.ics.\n"
                    << "  --porcelain         Machine-readable key=value output.\n\n"
                    << cli::paint("NOTES", cli::Tone::yellow,
                                  context.color_enabled, true)
                    << "\n  User priority: argument, ZZUASSISTANT_USER, saved App user.\n"
                    << "  ZZUASSISTANT_APP_TOKEN supplies auth for this process. Requires Python 3.\n";
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

        std::filesystem::path executable_directory() {
#ifdef _WIN32
            std::wstring path(32768, L'\0');
            const DWORD length = GetModuleFileNameW(
                nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (length == 0 || length == path.size()) return {};
            path.resize(length);
            return std::filesystem::path(path).parent_path();
#elif defined(__APPLE__)
            std::uint32_t size = 0;
            _NSGetExecutablePath(nullptr, &size);
            std::string path(size, '\0');
            if (_NSGetExecutablePath(path.data(), &size) != 0) return {};
            path.resize(std::char_traits<char>::length(path.c_str()));
            return std::filesystem::weakly_canonical(path).parent_path();
#else
            std::error_code error;
            const auto path = std::filesystem::read_symlink(
                "/proc/self/exe", error);
            return error ? std::filesystem::path{} : path.parent_path();
#endif
        }

        std::filesystem::path find_course_tool() {
            std::vector<std::filesystem::path> candidates;
            if (const auto configured = environment("ZZUASSISTANT_TOOL_DIR");
                configured && !configured->empty())
                candidates.emplace_back(
                    std::filesystem::path(*configured) / "course.py");
            const auto executable = executable_directory();
            if (!executable.empty()) {
                candidates.emplace_back(executable / "tool" / "course.py");
                candidates.emplace_back(executable.parent_path() / "tool" /
                                        "course.py");
            }
            std::error_code error;
            const auto current = std::filesystem::current_path(error);
            if (!error) {
                candidates.emplace_back(current / "tool" / "course.py");
                candidates.emplace_back(current.parent_path() / "tool" /
                                        "course.py");
            }
            for (const auto &candidate: candidates) {
                error.clear();
                if (std::filesystem::is_regular_file(candidate, error) && !error)
                    return std::filesystem::absolute(candidate);
            }
            throw std::runtime_error(
                "Cannot find tool/course.py; set ZZUASSISTANT_TOOL_DIR");
        }

        struct Interpreter {
            std::string program;
            std::vector<std::string> prefix;
        };

        std::vector<Interpreter> interpreters() {
            std::vector<Interpreter> result;
            if (const auto configured = environment("ZZUASSISTANT_PYTHON");
                configured && !configured->empty())
                result.push_back({*configured, {}});
#ifdef ZZU_PYTHON_EXECUTABLE
            result.push_back({ZZU_PYTHON_EXECUTABLE, {}});
#endif
#ifdef _WIN32
            result.push_back({"python.exe", {}});
            result.push_back({"py.exe", {"-3"}});
#else
            result.push_back({"python3", {}});
            result.push_back({"python", {}});
#endif
            return result;
        }

#ifdef _WIN32
        std::wstring wide(const std::string_view value) {
            if (value.empty()) return {};
            const int size = MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                static_cast<int>(value.size()), nullptr, 0);
            if (size <= 0)
                throw std::runtime_error(
                    "Cannot convert a course argument to UTF-16");
            std::wstring result(static_cast<std::size_t>(size), L'\0');
            if (MultiByteToWideChar(
                    CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                    static_cast<int>(value.size()), result.data(), size) <= 0)
                throw std::runtime_error(
                    "Cannot convert a course argument to UTF-16");
            return result;
        }
#endif

        std::optional<int> spawn(const Interpreter &interpreter,
                                 const std::filesystem::path &script,
                                 const Arguments arguments) {
            std::vector<std::string> values;
            values.push_back(interpreter.program);
            values.insert(values.end(), interpreter.prefix.begin(),
                          interpreter.prefix.end());
#ifdef _WIN32
            const auto script_utf8 = script.u8string();
            values.emplace_back(
                reinterpret_cast<const char *>(script_utf8.data()),
                script_utf8.size());
#else
            values.push_back(script.string());
#endif
            for (const auto argument: arguments)
                values.emplace_back(argument);
#ifdef _WIN32
            std::vector<std::wstring> storage;
            storage.reserve(values.size());
            for (const auto &value: values) storage.push_back(wide(value));
            std::vector<const wchar_t *> argv;
            argv.reserve(storage.size() + 1);
            for (const auto &value: storage) argv.push_back(value.c_str());
            argv.push_back(nullptr);
            errno = 0;
            const intptr_t result = _wspawnvp(
                _P_WAIT, storage.front().c_str(), argv.data());
            if (result == -1 && errno == ENOENT) return std::nullopt;
            if (result == -1)
                throw std::runtime_error("Cannot start the Python course tool");
            return static_cast<int>(result);
#else
            std::vector<char *> argv;
            argv.reserve(values.size() + 1);
            for (auto &value: values) argv.push_back(value.data());
            argv.push_back(nullptr);
            pid_t child{};
            const int started = posix_spawnp(
                &child, values.front().c_str(), nullptr, nullptr,
                argv.data(), environ);
            if (started == ENOENT) return std::nullopt;
            if (started != 0)
                throw std::runtime_error("Cannot start the Python course tool");
            int status = 0;
            if (waitpid(child, &status, 0) < 0)
                throw std::runtime_error("Cannot wait for the Python course tool");
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            return 1;
#endif
        }
    } // namespace

    std::string_view CourseService::description() const noexcept {
        return "Export the course table as an ICS calendar";
    }

    int CourseService::execute(ServiceContext &context, Arguments arguments) {
        if ((!arguments.empty() &&
             (arguments.front() == "--help" || arguments.front() == "-h"))) {
            usage(context);
            return 0;
        }
        try {
            const auto script = find_course_tool();
            for (const auto &interpreter: interpreters()) {
                if (const auto result = spawn(interpreter, script, arguments))
                    return *result;
            }
            throw std::runtime_error(
                "Python 3 was not found; set ZZUASSISTANT_PYTHON");
        } catch (const std::exception &error) {
            cli::status(context.err, "ERROR", error.what(), cli::Tone::red,
                        context.color_enabled);
            return 1;
        }
    }
} // namespace zzu_assistant::services
