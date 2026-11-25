#pragma once
// nobcpp - a single header C++ build tool in the spirit of tsoding's nob.h
//
// Bootstrap once with any C++20 compiler, afterwards the build script rebuilds itself:
//     c++ -std=c++20 -o nobcpp nobcpp.cpp        (Linux / MSYS2)
//     cl /nologo /std:c++20 /EHsc nobcpp.cpp     (MSVC developer prompt)
//
// Run `./nobcpp help` for the command line.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace nob
{

namespace fs = std::filesystem;
using SteadyClock = std::chrono::steady_clock;

// ----------------------------------------------------------------------------------
// Utils
// ----------------------------------------------------------------------------------

inline double seconds_since(SteadyClock::time_point start)
{
    return std::chrono::duration<double>(SteadyClock::now() - start).count();
}

inline std::string format_duration(double seconds)
{
    char buf[32];
    if (seconds < 0.001)
    {
        std::snprintf(buf, sizeof(buf), "%.0fus", seconds * 1e6);
    }
    else if (seconds < 1.0)
    {
        std::snprintf(buf, sizeof(buf), "%.0fms", seconds * 1e3);
    }
    else if (seconds < 60.0)
    {
        std::snprintf(buf, sizeof(buf), "%.2fs", seconds);
    }
    else
    {
        int total = static_cast<int>(seconds + 0.5);
        std::snprintf(buf, sizeof(buf), "%dm%02ds", total / 60, total % 60);
    }
    return buf;
}

inline std::string str_lower(std::string s)
{
    for (char& c : s)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

inline void strip_cr(std::string& s)
{
    if (!s.empty() && s.back() == '\r')
    {
        s.pop_back();
    }
}

inline std::vector<std::string> split(const std::string& s, char delim)
{
    std::vector<std::string> parts;
    std::string item;
    std::istringstream ss(s);
    while (std::getline(ss, item, delim))
    {
        if (!item.empty())
        {
            parts.push_back(item);
        }
    }
    return parts;
}

inline std::string join(const std::vector<std::string>& parts, const std::string& sep)
{
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (i != 0)
        {
            out += sep;
        }
        out += parts[i];
    }
    return out;
}

template <typename T>
void append(std::vector<T>& dst, const std::vector<T>& src)
{
    dst.insert(dst.end(), src.begin(), src.end());
}

// Stable across runs and platforms (unlike std::hash), used to detect flag changes.
inline uint64_t hash_args(const std::vector<std::string>& args)
{
    uint64_t h = 1469598103934665603ull;
    for (const auto& arg : args)
    {
        for (unsigned char c : arg)
        {
            h = (h ^ c) * 1099511628211ull;
        }
        h = (h ^ 0xffu) * 1099511628211ull;
    }
    return h;
}

// Removes CSI (colors) and OSC (hyperlinks) escape sequences.
inline std::string strip_ansi(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[')
        {
            i += 2;
            while (i < s.size() && !(s[i] >= 0x40 && s[i] <= 0x7e))
            {
                ++i;
            }
        }
        else if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == ']')
        {
            i += 2;
            while (i < s.size() && s[i] != '\a' && !(s[i] == '\x1b' && i + 1 < s.size()))
            {
                ++i;
            }
            if (i < s.size() && s[i] == '\x1b')
            {
                ++i;
            }
        }
        else
        {
            out += s[i];
        }
    }
    return out;
}

inline std::string json_escape(std::string_view s)
{
    std::string out;
    for (char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
            {
                out += c;
            }
        }
    }
    return out;
}

inline std::string display_path(const fs::path& p)
{
    return p.lexically_normal().generic_string();
}

// last_write_time without exceptions: a missing file simply has no time.
inline std::optional<fs::file_time_type> file_mtime(const fs::path& p)
{
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    if (ec)
    {
        return std::nullopt;
    }
    return t;
}

inline int64_t file_clock_now()
{
    return fs::file_time_type::clock::now().time_since_epoch().count();
}

// ----------------------------------------------------------------------------------
// Console: every line of output goes through here, so nothing interleaves and the
// progress bar can be redrawn below whatever was printed.
// ----------------------------------------------------------------------------------

enum class ColorMode
{
    Auto,
    Always,
    Never
};

class Console
{
  public:
    static Console& get()
    {
        static Console console;
        return console;
    }

    void configure(ColorMode mode)
    {
        if (mode == ColorMode::Always)
        {
            color_ = true;
        }
        else if (mode == ColorMode::Never)
        {
            color_ = false;
        }
        else
        {
            const char* no_color = std::getenv("NO_COLOR");
            const char* force = std::getenv("CLICOLOR_FORCE");
            const char* term = std::getenv("TERM");
            color_ = (tty_ && !(no_color && *no_color) &&
                      !(term && std::string_view(term) == "dumb")) ||
                     (force && std::string_view(force) != "0");
        }
    }

    bool tty() const
    {
        return tty_;
    }

    bool color() const
    {
        return color_;
    }

    int width() const
    {
#ifdef _WIN32
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
        {
            return info.srWindow.Right - info.srWindow.Left + 1;
        }
#else
        winsize ws{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        {
            return ws.ws_col;
        }
#endif
        return 80;
    }

    std::string paint(std::string_view codes, std::string_view text) const
    {
        if (!color_)
        {
            return std::string(text);
        }
        return "\x1b[" + std::string(codes) + "m" + std::string(text) + "\x1b[0m";
    }
    std::string bold(std::string_view t) const { return paint("1", t); }
    std::string dim(std::string_view t) const { return paint("2", t); }
    std::string red(std::string_view t) const { return paint("1;31", t); }
    std::string green(std::string_view t) const { return paint("1;32", t); }
    std::string yellow(std::string_view t) const { return paint("1;33", t); }
    std::string cyan(std::string_view t) const { return paint("36", t); }
    std::string blue(std::string_view t) const { return paint("1;34", t); }

    // Atomically prints `text` above the status line.
    void print(std::string_view text)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_shown_)
        {
            write_raw("\r\x1b[2K");
        }
        write_raw(text);
        status_shown_ = false;
        draw_status();
        std::fflush(stdout);
    }

    void println(std::string_view text)
    {
        print(std::string(text) + "\n");
    }

    void error(std::string_view text)
    {
        println(red("error: ") + std::string(text));
    }

    void warning(std::string_view text)
    {
        println(yellow("warning: ") + std::string(text));
    }

    void set_status(std::string status)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = std::move(status);
        draw_status();
        std::fflush(stdout);
    }

    void clear_status()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_shown_)
        {
            write_raw("\r\x1b[2K");
            std::fflush(stdout);
        }
        status_.clear();
        status_shown_ = false;
    }

    // Cuts a string to `width` visible characters, skipping escape codes and
    // counting UTF-8 code points.
    static std::string truncate_visible(std::string_view s, int width)
    {
        std::string out;
        int visible = 0;
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '\x1b')
            {
                size_t j = i + 1;
                while (j < s.size() && !(s[j] >= 0x40 && s[j] <= 0x7e && j > i + 1))
                {
                    ++j;
                }
                out.append(s.substr(i, j - i + 1));
                i = j;
                continue;
            }
            bool continuation = (static_cast<unsigned char>(s[i]) & 0xc0) == 0x80;
            if (!continuation)
            {
                if (visible == width)
                {
                    break;
                }
                ++visible;
            }
            out += s[i];
        }
        return out;
    }

  private:
    Console()
    {
#ifdef _WIN32
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (_isatty(_fileno(stdout)) && GetConsoleMode(out, &mode))
        {
            tty_ = SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
            SetConsoleOutputCP(CP_UTF8);
        }
#else
        tty_ = isatty(STDOUT_FILENO) != 0;
#endif
        configure(ColorMode::Auto);
    }

    void write_raw(std::string_view text)
    {
        std::fwrite(text.data(), 1, text.size(), stdout);
    }

    void draw_status()
    {
        if (!tty_ || status_.empty())
        {
            return;
        }
        write_raw("\r\x1b[2K");
        write_raw(truncate_visible(status_, width() - 1));
        if (color_)
        {
            write_raw("\x1b[0m");
        }
        status_shown_ = true;
    }

    std::mutex mutex_;
    std::string status_;
    bool status_shown_ = false;
    bool tty_ = false;
    bool color_ = false;
};

inline Console& console()
{
    return Console::get();
}

// ----------------------------------------------------------------------------------
// Processes
// ----------------------------------------------------------------------------------

struct ProcessResult
{
    int exit_code = -1;
    std::string output; // stdout and stderr merged, in the order they were written
    bool started = false;
};

// Pipes must not leak into children spawned concurrently by other threads, otherwise
// a job's output pipe only reaches EOF once unrelated jobs exit. Pipe creation and
// spawning are therefore serialized.
inline std::mutex& spawn_mutex()
{
    static std::mutex m;
    return m;
}

#ifdef _WIN32

inline std::string quote_arg_windows(const std::string& arg)
{
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos)
    {
        return arg;
    }
    std::string out = "\"";
    for (size_t i = 0;; ++i)
    {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == '\\')
        {
            ++i;
            ++backslashes;
        }
        if (i == arg.size())
        {
            out.append(backslashes * 2, '\\');
            break;
        }
        if (arg[i] == '"')
        {
            out.append(backslashes * 2 + 1, '\\');
        }
        else
        {
            out.append(backslashes, '\\');
        }
        out += arg[i];
    }
    out += '"';
    return out;
}

inline std::string windows_command_line(const std::vector<std::string>& args)
{
    std::string cmdline;
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (i != 0)
        {
            cmdline += ' ';
        }
        cmdline += quote_arg_windows(args[i]);
    }
    return cmdline;
}

inline std::string last_error_message()
{
    char buf[512] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   GetLastError(), 0, buf, sizeof(buf), nullptr);
    return buf;
}

inline ProcessResult run_capture(const std::vector<std::string>& args)
{
    std::string cmdline = windows_command_line(args);
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    PROCESS_INFORMATION pi{};
    BOOL ok = FALSE;
    {
        std::lock_guard<std::mutex> lock(spawn_mutex());
        if (!CreatePipe(&read_end, &write_end, &sa, 0))
        {
            return {-1, "CreatePipe failed: " + last_error_message(), false};
        }
        SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &sa, OPEN_EXISTING, 0, nullptr);
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nul;
        si.hStdOutput = write_end;
        si.hStdError = write_end;
        ok = CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE, 0, nullptr,
                            nullptr, &si, &pi);
        CloseHandle(write_end);
        if (nul != INVALID_HANDLE_VALUE)
        {
            CloseHandle(nul);
        }
    }
    if (!ok)
    {
        std::string msg = "failed to start '" + args[0] + "': " + last_error_message();
        CloseHandle(read_end);
        return {127, msg, false};
    }

    ProcessResult result;
    result.started = true;
    char buffer[4096];
    DWORD count = 0;
    while (ReadFile(read_end, buffer, sizeof(buffer), &count, nullptr) && count > 0)
    {
        result.output.append(buffer, count);
    }
    CloseHandle(read_end);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    result.exit_code = static_cast<int>(code);
    return result;
}

// Runs with inherited stdin/stdout/stderr, for running the built program.
inline int run_interactive(const std::vector<std::string>& args)
{
    std::string cmdline = windows_command_line(args);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &si, &pi))
    {
        console().error("failed to start '" + args[0] + "': " + last_error_message());
        return 127;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
}

#else

inline std::vector<char*> make_argv(const std::vector<std::string>& args)
{
    std::vector<char*> argv;
    for (const auto& arg : args)
    {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    return argv;
}

inline int decode_wait_status(int status, std::string* output)
{
    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status))
    {
        if (output)
        {
            *output += "\nprocess killed by signal " + std::to_string(WTERMSIG(status)) +
                       " (" + strsignal(WTERMSIG(status)) + ")\n";
        }
        return 128 + WTERMSIG(status);
    }
    return 1;
}

inline ProcessResult run_capture(const std::vector<std::string>& args)
{
    std::vector<char*> argv = make_argv(args);
    int fds[2];
    pid_t pid = 0;
    int err = 0;
    {
        std::lock_guard<std::mutex> lock(spawn_mutex());
        if (pipe(fds) != 0)
        {
            return {-1, std::string("pipe failed: ") + std::strerror(errno), false};
        }
        fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        fcntl(fds[1], F_SETFD, FD_CLOEXEC);

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
        err = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        close(fds[1]);
    }
    if (err != 0)
    {
        close(fds[0]);
        return {127, "failed to start '" + args[0] + "': " + std::strerror(err), false};
    }

    ProcessResult result;
    result.started = true;
    char buffer[4096];
    while (true)
    {
        ssize_t count = read(fds[0], buffer, sizeof(buffer));
        if (count > 0)
        {
            result.output.append(buffer, static_cast<size_t>(count));
        }
        else if (count == -1 && errno == EINTR)
        {
            continue;
        }
        else
        {
            break;
        }
    }
    close(fds[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) == -1 && errno == EINTR)
    {
    }
    result.exit_code = decode_wait_status(status, &result.output);
    return result;
}

inline int run_interactive(const std::vector<std::string>& args)
{
    std::vector<char*> argv = make_argv(args);
    pid_t pid = 0;
    int err = posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ);
    if (err != 0)
    {
        console().error("failed to start '" + args[0] + "': " + std::strerror(err));
        return 127;
    }
    int status = 0;
    while (waitpid(pid, &status, 0) == -1 && errno == EINTR)
    {
    }
    std::string msg;
    int code = decode_wait_status(status, &msg);
    if (!msg.empty())
    {
        console().print(msg);
    }
    return code;
}

#endif

// ----------------------------------------------------------------------------------
// Rebuild self
// ----------------------------------------------------------------------------------

inline std::string& self_source_path()
{
    static std::string path;
    return path;
}

inline fs::path self_exe_path(const char* argv0)
{
    std::error_code ec;
#ifdef _WIN32
    char buf[MAX_PATH * 4];
    DWORD len = GetModuleFileNameA(nullptr, buf, sizeof(buf));
    if (len > 0 && len < sizeof(buf))
    {
        return fs::path(std::string(buf, len));
    }
#elif defined(__linux__)
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec)
    {
        return p;
    }
#endif
    fs::path p2 = fs::absolute(argv0, ec);
    return ec ? fs::path(argv0) : p2;
}

// The compiler used to rebuild the build script itself. Defaults to the compiler
// family that built the currently running binary. Override with -DNOBCPP_SELF_CXX=...
inline std::vector<std::string> self_compile_command(const std::string& src,
                                                     const std::string& out)
{
#if defined(_MSC_VER)
#if defined(NOBCPP_SELF_CXX)
    std::string cxx = NOBCPP_SELF_CXX;
#elif defined(__clang__)
    std::string cxx = "clang-cl";
#else
    std::string cxx = "cl";
#endif
    std::string obj = (fs::path(out).parent_path() / fs::path(out).stem()).string() + ".obj";
    return {cxx, "/nologo", "/std:c++20", "/EHsc", "/Fo" + obj, "/Fe" + out, src};
#else
#if defined(NOBCPP_SELF_CXX)
    std::string cxx = NOBCPP_SELF_CXX;
#elif defined(__clang__)
    std::string cxx = "clang++";
#else
    std::string cxx = "g++";
#endif
    // unoptimized: it only starts processes, and every edit of the script is a rebuild
    return {cxx, "-std=c++20", "-O0", "-Wall", "-Wextra", "-pthread", "-o", out, src};
#endif
}

// Where the build script's source is. __FILE__ is baked into the binary when it is
// compiled (absolute after a self-rebuild), so it would still point into the original
// checkout after the folder was copied or moved. Only its file name is trusted: the
// source is looked up next to the running binary first, then relative to the current
// directory, and only then at the compiled in path.
inline fs::path find_self_source(const std::string& source_filename, const fs::path& bin)
{
    std::error_code ec;
    fs::path compiled_in(source_filename);
    std::vector<fs::path> candidates = {bin.parent_path() / compiled_in.filename()};
    if (compiled_in.is_relative())
    {
        candidates.push_back(fs::current_path(ec) / compiled_in);
    }
    candidates.push_back(compiled_in);
    for (const auto& c : candidates)
    {
        if (fs::is_regular_file(c, ec))
        {
            return fs::absolute(c, ec).lexically_normal();
        }
    }
    return fs::absolute(compiled_in, ec);
}

// Recompiles and restarts the build script if its source (or `deps`, relative to the
// source's directory) is newer than the running binary.
inline void rebuild_self(const std::string& source_filename, int argc, char** argv,
                         const std::vector<std::string>& deps = {})
{
    std::error_code ec;
    fs::path bin = self_exe_path(argv[0]);
    fs::path src = find_self_source(source_filename, bin);
    self_source_path() = src.string();
    fs::path old_bin = bin.string() + ".old";
    fs::remove(old_bin, ec); // leftover from the previous rebuild (locked on Windows)

    // Guards against endless rebuild loops, e.g. caused by timestamps in the future.
    if (const char* rebuilt = std::getenv("NOBCPP_REBUILT"); rebuilt && *rebuilt)
    {
#ifdef _WIN32
        _putenv_s("NOBCPP_REBUILT", "");
#else
        unsetenv("NOBCPP_REBUILT");
#endif
        return;
    }

    auto bin_time = file_mtime(bin);
    auto src_time = file_mtime(src);
    if (!src_time)
    {
        console().warning("build script source '" + src.string() +
                          "' not found, skipping self-rebuild");
        return;
    }
    if (*src_time > fs::file_time_type::clock::now())
    {
        console().warning("'" + src.string() + "' has a modification time in the future, " +
                          "the build script will be rebuilt on every run");
    }
    bool needs_rebuild = !bin_time || *src_time > *bin_time;
    for (const auto& dep : deps)
    {
        fs::path dep_path = src.parent_path() / dep;
        auto dep_time = file_mtime(dep_path);
        if (!dep_time)
        {
            console().warning("build script dependency '" + dep_path.string() +
                              "' not found");
        }
        else if (bin_time && *dep_time > *bin_time)
        {
            needs_rebuild = true;
        }
    }
    if (!needs_rebuild)
    {
        return;
    }

    console().println(console().cyan("nobcpp:") + " build script changed, rebuilding " +
                      display_path(bin.filename()) + "...");
    // Renaming works even while the binary is running (also on Windows), and lets us
    // restore it if compilation fails.
    fs::rename(bin, old_bin, ec);
    bool moved = !ec;
    auto cmd = self_compile_command(src.string(), bin.string());
    ProcessResult result = run_capture(cmd);
    if (!result.output.empty())
    {
        console().print(result.output);
    }
    if (result.exit_code != 0)
    {
        if (moved)
        {
            fs::remove(bin, ec);
            fs::rename(old_bin, bin, ec);
        }
        console().error("rebuilding the build script failed: " + join(cmd, " "));
        std::exit(1);
    }

    std::vector<std::string> args = {bin.string()};
    for (int i = 1; i < argc; ++i)
    {
        args.push_back(argv[i]);
    }
#ifdef _WIN32
    _putenv_s("NOBCPP_REBUILT", "1");
    std::exit(run_interactive(args));
#else
    setenv("NOBCPP_REBUILT", "1", 1);
    std::vector<char*> new_argv = make_argv(args);
    execv(new_argv[0], new_argv.data());
    console().error(std::string("execv failed: ") + std::strerror(errno));
    std::exit(1);
#endif
}

// ----------------------------------------------------------------------------------
// Flags and profiles (compiler independent)
// ----------------------------------------------------------------------------------

enum class Opt
{
    O0,
    Og,
    O1,
    O2,
    O3,
    Os
};

enum class Warnings
{
    Off,
    On,   // -Wall      | /W3
    Extra // -Wall -Wextra -Wpedantic | /W4
};

// Compiler independent build settings. Unset optionals mean "compiler default".
// Raw `compile_flags`/`link_flags` are passed through unchanged, check
// Build::toolchain().family if you need compiler specific ones.
// Members are declared in the order designated initializers have to use.
struct Flags
{
    std::optional<int> std{};        // 17, 20, 23, ...
    std::optional<Opt> opt{};
    std::optional<bool> debug_info{};
    std::optional<Warnings> warnings{};
    std::optional<bool> werror{};
    std::optional<bool> lto{};
    std::vector<std::string> defines{};      // "NDEBUG", "VERSION=3"
    std::vector<std::string> include_dirs{};
    std::vector<std::string> system_include_dirs{}; // warnings in these headers are hidden
    std::vector<std::string> sanitizers{};   // "address", "undefined", "thread", ...
    std::vector<std::string> libs{};         // system libraries: "m" -> -lm / m.lib
    std::vector<std::string> compile_flags{};
    std::vector<std::string> link_flags{};

    // Values of `other` win, lists are appended.
    void merge(const Flags& other)
    {
        if (other.std) std = other.std;
        if (other.opt) opt = other.opt;
        if (other.debug_info) debug_info = other.debug_info;
        if (other.warnings) warnings = other.warnings;
        if (other.werror) werror = other.werror;
        if (other.lto) lto = other.lto;
        append(defines, other.defines);
        append(include_dirs, other.include_dirs);
        append(system_include_dirs, other.system_include_dirs);
        append(sanitizers, other.sanitizers);
        append(libs, other.libs);
        append(compile_flags, other.compile_flags);
        append(link_flags, other.link_flags);
    }
};

using Profile = Flags;

// ----------------------------------------------------------------------------------
// Toolchain
// ----------------------------------------------------------------------------------

enum class Kind
{
    Executable,
    StaticLib,
    SharedLib,
    HeaderOnly // nothing to build, only passes its public flags to dependents
};

enum class Family
{
    Gcc,
    Clang,
    Msvc,   // cl.exe
    ClangCl // clang-cl.exe, msvc compatible command line
};

inline const char* family_name(Family family)
{
    switch (family)
    {
    case Family::Gcc: return "gcc";
    case Family::Clang: return "clang";
    case Family::Msvc: return "msvc";
    case Family::ClangCl: return "clang-cl";
    }
    return "?";
}

#if defined(_WIN32) || defined(__CYGWIN__)
inline constexpr bool target_windows = true;
#else
inline constexpr bool target_windows = false;
#endif
#if defined(__APPLE__)
inline constexpr bool target_apple = true;
#else
inline constexpr bool target_apple = false;
#endif

inline void warn_once(const std::string& message)
{
    static std::mutex m;
    static std::set<std::string> seen;
    std::lock_guard<std::mutex> lock(m);
    if (seen.insert(message).second)
    {
        console().warning(message);
    }
}

// Flags that matter when compiling and when linking (gcc/clang): given as a compile flag
// they are put on the link line as well.
inline bool compile_and_link_flag(const std::string& a)
{
    return a.starts_with("-fopenmp") || a == "-pthread" || a.starts_with("-fsanitize") ||
           a.starts_with("-fno-sanitize") || a.starts_with("-flto") || a == "-m32" || a == "-m64" ||
           a == "--coverage" || a == "-fprofile-arcs" || a.starts_with("-fprofile-generate") ||
           a.starts_with("-fprofile-use");
}

// Removes repeated switches (-fopenmp from the script and from OpenBLAS, the same -D or
// -isystem from two libraries). Only flags known to stand alone are touched, everything
// else (libraries, files, -Wl,..., options with a separate value like "-z now") keeps
// its order and repetitions: the linker depends on them.
inline std::vector<std::string> dedupe_flags(const std::vector<std::string>& args)
{
    auto standalone = [](const std::string& a) {
        if (a.starts_with("-Wl,") || a.starts_with("-Wa,") || a.starts_with("-Wp,")) return false;
        bool gcc = a.starts_with("-f") || a.starts_with("-W") || a.starts_with("-m") ||
                   a.starts_with("-O") || a.starts_with("-g") || a.starts_with("-std=") ||
                   a == "-pthread" || a == "-c" || a == "-MMD" || a == "-MD" || a == "-shared" ||
                   (a.size() > 2 && (a.starts_with("-D") || a.starts_with("-I")));
        // msvc options carry their value in the same token; /link starts the linker part
        bool msvc = a.size() > 1 && a[0] == '/' && a.find('/', 1) == std::string::npos && a != "/link";
        return gcc || msvc;
    };
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (size_t i = 0; i < args.size(); ++i)
    {
        const std::string& a = args[i];
        if ((a == "-isystem" || a == "/external:I" || a == "-I" || a == "-D") && i + 1 < args.size())
        {
            if (seen.insert(a + '\n' + args[i + 1]).second)
            {
                out.push_back(a);
                out.push_back(args[i + 1]);
            }
            ++i;
            continue;
        }
        if (standalone(a) && !seen.insert(a).second)
        {
            continue;
        }
        out.push_back(a);
    }
    return out;
}

struct Toolchain
{
    std::vector<std::string> cxx; // optional launcher (e.g. ccache) + compiler
    std::string ar;
    Family family = Family::Gcc;
    std::string version;

    bool msvc_like() const
    {
        return family == Family::Msvc || family == Family::ClangCl;
    }

    std::string obj_ext() const
    {
        return msvc_like() ? ".obj" : ".o";
    }

    std::string output_name(const std::string& name, Kind kind) const
    {
        switch (kind)
        {
        case Kind::Executable: return name + (target_windows ? ".exe" : "");
        case Kind::StaticLib: return msvc_like() ? name + ".lib" : "lib" + name + ".a";
        case Kind::SharedLib:
            if (msvc_like()) return name + ".dll";
            if (target_windows) return "lib" + name + ".dll";
            if (target_apple) return "lib" + name + ".dylib";
            return "lib" + name + ".so";
        case Kind::HeaderOnly: return "";
        }
        return name;
    }

    // Not part of the command hash or compile_commands.json.
    std::vector<std::string> color_flags() const
    {
        switch (family)
        {
        case Family::Gcc: return {"-fdiagnostics-color=always"};
        case Family::Clang:
            if (target_windows) return {"-fcolor-diagnostics", "-fansi-escape-codes"};
            return {"-fcolor-diagnostics"};
        case Family::ClangCl: return {"-fcolor-diagnostics", "-fansi-escape-codes"};
        case Family::Msvc: return {};
        }
        return {};
    }

    std::vector<std::string> sanitizers(const Flags& f) const
    {
        if (f.sanitizers.empty())
        {
            return {};
        }
        if (family == Family::Msvc)
        {
            std::vector<std::string> out;
            for (const auto& s : f.sanitizers)
            {
                if (s == "address")
                    out.push_back("/fsanitize=address");
                else
                    warn_once("sanitizer '" + s + "' is not supported by msvc, ignored");
            }
            return out;
        }
        return {"-fsanitize=" + join(f.sanitizers, ","), "-fno-omit-frame-pointer"};
    }

    std::vector<std::string> compile_flags(const Flags& f, bool pic) const
    {
        std::vector<std::string> out;
        if (msvc_like())
        {
            out = {"/nologo", "/EHsc"};
            if (f.std) out.push_back(*f.std > 20 ? "/std:c++latest"
                                                 : "/std:c++" + std::to_string(*f.std));
            if (f.opt)
            {
                switch (*f.opt)
                {
                case Opt::O0:
                case Opt::Og: out.push_back("/Od"); break;
                case Opt::O1:
                case Opt::Os: out.push_back("/O1"); break;
                case Opt::O2:
                case Opt::O3: out.push_back("/O2"); break;
                }
            }
            if (f.debug_info.value_or(false)) out.push_back("/Z7");
            if (f.warnings)
            {
                const char* w[] = {"/W0", "/W3", "/W4"};
                out.push_back(w[static_cast<int>(*f.warnings)]);
            }
            if (f.werror.value_or(false)) out.push_back("/WX");
            if (f.lto.value_or(false)) out.push_back(family == Family::Msvc ? "/GL" : "-flto");
            for (const auto& d : f.defines) out.push_back("/D" + d);
            for (const auto& i : f.include_dirs) out.push_back("/I" + i);
            for (const auto& i : f.system_include_dirs) append(out, {"/external:I", i});
            if (!f.system_include_dirs.empty()) out.push_back("/external:W0");
        }
        else
        {
            if (f.std) out.push_back("-std=c++" + std::to_string(*f.std));
            if (f.opt)
            {
                const char* o[] = {"-O0", "-Og", "-O1", "-O2", "-O3", "-Os"};
                out.push_back(o[static_cast<int>(*f.opt)]);
            }
            if (f.debug_info.value_or(false)) out.push_back("-g");
            if (f.warnings == Warnings::Off) out.push_back("-w");
            if (f.warnings == Warnings::On) out.push_back("-Wall");
            if (f.warnings == Warnings::Extra) append(out, {"-Wall", "-Wextra", "-Wpedantic"});
            if (f.werror.value_or(false)) out.push_back("-Werror");
            if (f.lto.value_or(false)) out.push_back("-flto");
            if (pic && !target_windows) out.push_back("-fPIC");
            for (const auto& d : f.defines) out.push_back("-D" + d);
            for (const auto& i : f.include_dirs) out.push_back("-I" + i);
            for (const auto& i : f.system_include_dirs) append(out, {"-isystem", i});
        }
        append(out, sanitizers(f));
        append(out, f.compile_flags);
        return dedupe_flags(out);
    }

    std::vector<std::string> compile_command(const Flags& f, const std::string& src,
                                             const std::string& obj,
                                             const std::string& depfile, bool pic) const
    {
        std::vector<std::string> cmd = cxx;
        append(cmd, compile_flags(f, pic));
        if (msvc_like())
        {
            append(cmd, {"/showIncludes", "/c", src, "/Fo" + obj});
        }
        else
        {
            // -MD instead of -MMD: -MMD would also drop headers from -isystem directories
            append(cmd, {"-MD", "-MF", depfile, "-c", src, "-o", obj});
        }
        return cmd;
    }

    // `inputs` are object files followed by libraries built by us, in link order.
    std::vector<std::string> link_command(const Flags& f, Kind kind,
                                          const std::vector<std::string>& inputs,
                                          const std::string& out,
                                          bool needs_rpath) const
    {
        std::vector<std::string> cmd = cxx;
        if (msvc_like())
        {
            cmd.push_back("/nologo");
            append(cmd, inputs);
            for (const auto& lib : f.libs) cmd.push_back(lib + ".lib");
            if (kind == Kind::SharedLib) cmd.push_back("/LD");
            append(cmd, sanitizers(f));
            if (f.lto.value_or(false) && family == Family::ClangCl)
                append(cmd, {"-flto", "-fuse-ld=lld"});
            cmd.push_back("/Fe" + out);
            cmd.push_back("/link");
            if (f.debug_info.value_or(false)) cmd.push_back("/DEBUG");
            if (f.lto.value_or(false) && family == Family::Msvc) cmd.push_back("/LTCG");
            append(cmd, f.link_flags);
            return cmd;
        }
        append(cmd, inputs);
        if (kind == Kind::SharedLib)
        {
            cmd.push_back(target_apple ? "-dynamiclib" : "-shared");
            std::string file = fs::path(out).filename().string();
            if (target_apple)
                cmd.push_back("-Wl,-install_name,@rpath/" + file);
            else if (!target_windows)
                cmd.push_back("-Wl,-soname," + file);
        }
        if (needs_rpath && !target_windows)
        {
            cmd.push_back(target_apple ? "-Wl,-rpath,@loader_path" : "-Wl,-rpath,$ORIGIN");
        }
        if (f.lto.value_or(false))
        {
            cmd.push_back("-flto");
            if (f.opt)
            {
                const char* o[] = {"-O0", "-Og", "-O1", "-O2", "-O3", "-Os"};
                cmd.push_back(o[static_cast<int>(*f.opt)]);
            }
        }
        append(cmd, sanitizers(f));
        // -fopenmp, -pthread, ... given as compile flags are needed when linking too
        for (const auto& flag : f.compile_flags)
        {
            if (compile_and_link_flag(flag)) cmd.push_back(flag);
        }
        append(cmd, f.link_flags);
        for (const auto& lib : f.libs) cmd.push_back("-l" + lib);
        append(cmd, {"-o", out});
        return dedupe_flags(cmd);
    }

    std::vector<std::string> archive_command(const std::vector<std::string>& objs,
                                             const std::string& out) const
    {
        std::vector<std::string> cmd;
        if (msvc_like())
        {
            cmd = {ar, "/nologo", "/OUT:" + out};
        }
        else
        {
            cmd = {ar, "rcs", out};
        }
        append(cmd, objs);
        return cmd;
    }
};

// `spec` is the compiler command, optionally prefixed by a launcher ("ccache g++").
// The family is found by running `<compiler> --version` instead of guessing from the
// name, so g++-14, x86_64-w64-mingw32-g++, c++ or icpx all work.
inline std::optional<Toolchain> detect_toolchain(const std::string& spec)
{
    Toolchain tc;
    tc.cxx = split(spec, ' ');
    if (tc.cxx.empty())
    {
        console().error("no compiler given");
        return std::nullopt;
    }
    std::string stem = str_lower(fs::path(tc.cxx.back()).stem().string());

    ProcessResult probe = run_capture({tc.cxx.back(), "--version"});
    if (!probe.started)
    {
        console().error("compiler '" + tc.cxx.back() + "' not found (" + probe.output + ")");
        return std::nullopt;
    }
    std::string text = str_lower(probe.output);
    tc.version = probe.output.substr(0, probe.output.find_first_of("\r\n"));

    if (stem.find("clang-cl") != std::string::npos)
        tc.family = Family::ClangCl;
    else if (text.find("microsoft") != std::string::npos || stem == "cl")
        tc.family = Family::Msvc;
    else if (text.find("clang") != std::string::npos)
        tc.family = Family::Clang;
    else
    {
        tc.family = Family::Gcc;
        if (text.find("free software foundation") == std::string::npos &&
            text.find("gcc") == std::string::npos && text.find("g++") == std::string::npos)
        {
            console().warning("unknown compiler '" + tc.cxx.back() +
                              "', assuming a gcc compatible command line");
        }
    }
    if (tc.family == Family::Msvc)
    {
        // cl prints its banner on stderr when called without sources
        tc.version = probe.output.substr(0, probe.output.find_first_of("\r\n"));
    }

    if (const char* ar = std::getenv("AR"); ar && *ar)
        tc.ar = ar;
    else if (tc.family == Family::Msvc)
        tc.ar = "lib";
    else if (tc.family == Family::ClangCl)
        tc.ar = "llvm-lib";
    else
    {
        // cross compilers: x86_64-w64-mingw32-g++ -> x86_64-w64-mingw32-ar
        std::string name = fs::path(tc.cxx.back()).filename().string();
        size_t dash = name.rfind('-');
        bool triple = dash != std::string::npos && name.find('-') < dash &&
                      (name.ends_with("g++") || name.ends_with("gcc") || name.ends_with("c++"));
        tc.ar = triple ? name.substr(0, dash + 1) + "ar" : "ar";
    }
    return tc;
}

inline std::string default_compiler()
{
    if (const char* cxx = std::getenv("CXX"); cxx && *cxx)
    {
        return cxx;
    }
#if defined(_MSC_VER) && defined(__clang__)
    return "clang-cl";
#elif defined(_MSC_VER)
    return "cl";
#else
    return "c++";
#endif
}

// ----------------------------------------------------------------------------------
// Targets
// ----------------------------------------------------------------------------------

// Extensions add_sources() picks up: C++ plus those of Build::add_language().
inline std::set<std::string>& source_extensions()
{
    static std::set<std::string> exts = {".cpp", ".cc", ".cxx", ".c++"};
    return exts;
}

inline bool is_cpp_source(const fs::path& p)
{
    return source_extensions().contains(p.extension().string());
}

// A compiler for other source files, e.g. GPU code:
//     b.add_language({.extensions = {".cu"}, .compiler = {"hipcc"}, .flags = {"--offload-arch=gfx1201"}});
// Files with these extensions are picked up by add_sources() and compiled with `compiler`
// (gcc style command line: -c, -o, and -MD -MF for automatic header tracking, which
// hipcc and nvcc support), then linked like any object with the C++ compiler. The
// target's -std, -O, -g, -D, -I and -isystem are passed along unless `inherit_flags`
// is false; warnings, sanitizers and raw compile_flags are not, other compilers often
// reject them. Runtime libraries go into the target's flags.libs / link_flags.
struct Language
{
    std::vector<std::string> extensions{};
    std::vector<std::string> compiler{};
    std::vector<std::string> flags{};
    bool inherit_flags = true;
};

inline std::vector<fs::path> find_files_all(const fs::path& dir)
{
    std::vector<fs::path> found;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec))
    {
        if (e.is_regular_file()) found.push_back(e.path().lexically_normal());
    }
    std::sort(found.begin(), found.end());
    return found;
}

class Fetched;

// A lazy path inside a fetched dependency (returned by Fetched::dir() and *fetched / "sub").
// Passing it to Target/Build methods defers fetching until a build actually needs it;
// converting it to std::filesystem::path resolves (and downloads if needed) on demand.
class FetchedPath
{
  public:
    FetchedPath(std::shared_ptr<Fetched> dep, fs::path sub = {})
        : dep_(std::move(dep)), sub_(std::move(sub))
    {
    }

    FetchedPath operator/(const fs::path& sub) const
    {
        return FetchedPath(dep_, sub_.empty() ? sub : sub_ / sub);
    }

    const std::shared_ptr<Fetched>& fetched() const { return dep_; }
    const fs::path& subpath() const { return sub_; }

    fs::path resolve() const;
    operator fs::path() const;
    std::string string() const;
    std::string generic_string() const;

  private:
    std::shared_ptr<Fetched> dep_;
    fs::path sub_;
};

class Target
{
  public:
    Target(std::string name, Kind kind) : name_(std::move(name)), kind_(kind)
    {
    }

    // Applied when compiling and linking this target.
    Flags flags;
    // For imported targets: where they come from (shown by 'tree').
    std::string origin;
    // File name of the output instead of the platform default ("libfoo.so", "foo.exe").
    std::string output_name;
    // Relative paths given to this target (sources, include dirs, objects, copies) are
    // relative to this directory, see Build::scope(). Empty: the working directory.
    fs::path base;

    fs::path at(const fs::path& p) const
    {
        return base.empty() || p.is_absolute() ? p : (base / p).lexically_normal();
    }

    // An object file that becomes part of this target: archived into a static library,
    // linked into an executable / shared library, or, for a custom or header only target,
    // linked into everything that links it. If a command of any target produces it, the
    // archive / link waits for that command and reruns whenever it changes.
    Target& add_object(const fs::path& object)
    {
        objects_.push_back(display_path(at(object)));
        return *this;
    }

    const std::vector<std::string>& objects() const
    {
        return objects_;
    }

    struct Command
    {
        std::vector<std::string> args; // empty: copy inputs[0] to outputs[0]
        std::vector<std::string> inputs;
        std::vector<std::string> outputs;
    };

    struct DeferredPath
    {
        enum class Kind
        {
            Source,
            SourcesDir,
            IncludeDir,
            Copy
        };
        Kind kind;
        FetchedPath path;
        fs::path dest{};
        bool flag1 = true;
        bool flag2 = false;
    };

    // A build step nobcpp knows nothing about (shader compilers, code generators...):
    // runs when an input changed, an output is missing or the command changed. It runs
    // before this target's sources are compiled (so it can generate headers), and
    // everything linking this target waits for it.
    Target& add_command(std::vector<std::string> args, const std::vector<fs::path>& inputs,
                        const std::vector<fs::path>& outputs)
    {
        Command c{std::move(args), {}, {}};
        for (const auto& i : inputs) c.inputs.push_back(display_path(i));
        for (const auto& o : outputs) c.outputs.push_back(display_path(o));
        commands_.push_back(std::move(c));
        return *this;
    }

    // Copies a file (e.g. a resource next to the executable), whenever it changed.
    // A directory copies every file below it.
    Target& add_copy(const fs::path& from_path, const fs::path& to)
    {
        fs::path from = at(from_path);
        if (fs::is_directory(from))
        {
            for (const auto& f : find_files_all(from)) // already resolved
            {
                commands_.push_back({{}, {display_path(f)}, {display_path(to / f.lexically_relative(from))}});
            }
            return *this;
        }
        commands_.push_back({{}, {display_path(from)}, {display_path(to)}});
        return *this;
    }

    Target& add_copy(const FetchedPath& from, const fs::path& to)
    {
        deferred_paths_.push_back({DeferredPath::Kind::Copy, from, to, true, false});
        return *this;
    }

    const std::vector<Command>& commands() const
    {
        return commands_;
    }
    // Also applied to every target that (transitively) links this one:
    // include directories, defines and system libraries a user of this target needs.
    Flags public_flags;

    // Adds all C++ sources below `dir` (sorted, so the link order is deterministic) and
    // makes `dir` a public include directory.
    Target& add_sources(const fs::path& dir_path, bool recursive = true)
    {
        const fs::path dir = at(dir_path);
        std::vector<fs::path> found;
        std::error_code ec;
        if (recursive)
        {
            for (const auto& e : fs::recursive_directory_iterator(dir, ec))
                if (e.is_regular_file() && is_cpp_source(e.path())) found.push_back(e.path());
        }
        else
        {
            for (const auto& e : fs::directory_iterator(dir, ec))
                if (e.is_regular_file() && is_cpp_source(e.path())) found.push_back(e.path());
        }
        if (ec)
        {
            console().error("cannot read source directory '" + dir.string() + "': " +
                            ec.message());
        }
        std::sort(found.begin(), found.end());
        // `dir` is already resolved against base: add as is
        for (const auto& f : found)
        {
            sources_.push_back(f.lexically_normal());
        }
        add_include_dir(dir, true, false);
        return *this;
    }

    Target& add_sources(const FetchedPath& dir, bool recursive = true)
    {
        deferred_paths_.push_back({DeferredPath::Kind::SourcesDir, dir, {}, recursive, false});
        return *this;
    }

    Target& add_source(const fs::path& file)
    {
        sources_.push_back(at(file).lexically_normal());
        return *this;
    }

    Target& add_source(const FetchedPath& file)
    {
        deferred_paths_.push_back({DeferredPath::Kind::Source, file, {}, true, false});
        return *this;
    }

    Target& link(std::shared_ptr<Target> dep)
    {
        deps_.push_back(std::move(dep));
        return *this;
    }

    // `system` include dirs are meant for third party headers: the compiler does not
    // report warnings inside them (-isystem / /external:I).
    Target& include_dir(const fs::path& dir, bool is_public = true, bool system = false)
    {
        add_include_dir(at(dir), is_public, system);
        return *this;
    }

    // an already resolved directory
    void add_include_dir(const fs::path& dir, bool is_public, bool system)
    {
        std::string d = display_path(dir);
        if (!d.empty() && d.back() == '/' && d.size() > 1)
        {
            d.pop_back();
        }
        Flags& f = is_public ? public_flags : flags;
        (system ? f.system_include_dirs : f.include_dirs).push_back(d);
    }

    Target& include_dir(const FetchedPath& dir, bool is_public = true, bool system = false)
    {
        deferred_paths_.push_back({DeferredPath::Kind::IncludeDir, dir, {}, is_public, system});
        return *this;
    }

    Target& system_include_dir(const fs::path& dir, bool is_public = true)
    {
        return include_dir(dir, is_public, true);
    }

    Target& system_include_dir(const FetchedPath& dir, bool is_public = true)
    {
        return include_dir(dir, is_public, true);
    }

    Target& define(const std::string& define, bool is_public = false)
    {
        (is_public ? public_flags : flags).defines.push_back(define);
        return *this;
    }

    Target& link_lib(const std::string& lib)
    {
        public_flags.libs.push_back(lib);
        return *this;
    }

    const std::string& name() const
    {
        return name_;
    }

    Kind kind() const
    {
        return kind_;
    }

    const std::vector<fs::path>& sources() const
    {
        return sources_;
    }

    const std::vector<std::shared_ptr<Target>>& deps() const
    {
        return deps_;
    }

    const std::vector<DeferredPath>& deferred_paths() const
    {
        return deferred_paths_;
    }

    void resolve_deferred_paths()
    {
        auto pending = std::move(deferred_paths_);
        deferred_paths_.clear();
        for (const auto& dp : pending)
        {
            fs::path p = dp.path.resolve();
            switch (dp.kind)
            {
            case DeferredPath::Kind::Source: add_source(p); break;
            case DeferredPath::Kind::SourcesDir: add_sources(p, dp.flag1); break;
            case DeferredPath::Kind::IncludeDir: include_dir(p, dp.flag1, dp.flag2); break;
            case DeferredPath::Kind::Copy: add_copy(p, dp.dest); break;
            }
        }
    }

  private:
    std::string name_;
    Kind kind_;
    std::vector<fs::path> sources_;
    std::vector<std::shared_ptr<Target>> deps_;
    std::vector<Command> commands_;
    std::vector<DeferredPath> deferred_paths_;
    std::vector<std::string> objects_;
};

// Files below `dir` with one of the extensions (".vert", ".frag"), sorted.
inline std::vector<fs::path> find_files(const fs::path& dir, const std::set<std::string>& extensions,
                                        bool recursive = true)
{
    std::vector<fs::path> found;
    std::error_code ec;
    auto consider = [&](const fs::directory_entry& e) {
        if (e.is_regular_file() && extensions.contains(e.path().extension().string()))
            found.push_back(e.path().lexically_normal());
    };
    if (recursive)
        for (const auto& e : fs::recursive_directory_iterator(dir, ec)) consider(e);
    else
        for (const auto& e : fs::directory_iterator(dir, ec)) consider(e);
    std::sort(found.begin(), found.end());
    return found;
}

// ----------------------------------------------------------------------------------
// Build cache: per output file the command hash, the time the command was started,
// the last duration and the discovered header dependencies.
// ----------------------------------------------------------------------------------

struct CacheEntry
{
    uint64_t hash = 0;
    int64_t stamp = 0; // file_time_type ticks when the job started
    double seconds = 0;
    std::string display;
    std::string source; // compile jobs: the source file (its object goes when it is deleted)
    std::vector<std::string> deps;
    std::string diagnostics; // compiler output of the last successful run (warnings)
};

// Keeps multi-line text on a single cache line.
inline std::string escape_line(std::string_view s)
{
    std::string out;
    for (char c : s)
    {
        switch (c)
        {
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c;
        }
    }
    return out;
}

inline std::string unescape_line(std::string_view s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] != '\\' || i + 1 == s.size())
        {
            out += s[i];
            continue;
        }
        switch (s[++i])
        {
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        default: out += s[i];
        }
    }
    return out;
}

inline bool has_text(std::string_view output)
{
    return strip_ansi(output).find_first_not_of(" \r\n\t") != std::string::npos;
}

// Wall clock time of a build, for `stats`.
struct BuildRecord
{
    double wall = 0;
    double cpu = 0; // sum of the durations of the jobs that ran
    int jobs_run = 0;
    int jobs_total = 0;
    int threads = 0;
};

class Cache
{
  public:
    std::unordered_map<std::string, CacheEntry> entries;
    std::optional<BuildRecord> last_build;
    std::optional<BuildRecord> last_full_build; // every job ran

    void load(const fs::path& path)
    {
        std::ifstream in(path);
        std::string line;
        if (!std::getline(in, line))
        {
            return;
        }
        strip_cr(line);
        if (line != header)
        {
            return;
        }
        CacheEntry* current = nullptr;
        while (std::getline(in, line))
        {
            strip_cr(line);
            if (line.starts_with("E\t"))
            {
                std::istringstream ss(line.substr(2));
                std::string hash, stamp, seconds, output, display;
                if (!std::getline(ss, hash, '\t') || !std::getline(ss, stamp, '\t') ||
                    !std::getline(ss, seconds, '\t') || !std::getline(ss, output, '\t') ||
                    !std::getline(ss, display, '\t'))
                {
                    current = nullptr;
                    continue;
                }
                CacheEntry parsed;
                try
                {
                    parsed.hash = std::stoull(hash, nullptr, 16);
                    parsed.stamp = std::stoll(stamp);
                    parsed.seconds = std::stod(seconds);
                }
                catch (...)
                {
                    current = nullptr;
                    continue;
                }
                parsed.display = std::move(display);
                CacheEntry& e = entries[output];
                e = std::move(parsed);
                current = &e;
            }
            else if (line.starts_with("D\t") && current)
            {
                current->deps.push_back(line.substr(2));
            }
            else if (line.starts_with("S\t") && current)
            {
                current->source = line.substr(2);
            }
            else if (line.starts_with("L\t") || line.starts_with("F\t"))
            {
                BuildRecord r;
                std::istringstream ss(line.substr(2));
                if (ss >> r.wall >> r.cpu >> r.jobs_run >> r.jobs_total >> r.threads)
                {
                    (line[0] == 'L' ? last_build : last_full_build) = r;
                }
            }
            else if (line.starts_with("O\t") && current)
            {
                current->diagnostics = unescape_line(std::string_view(line).substr(2));
            }
        }
    }

    void save(const fs::path& path) const
    {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        fs::path tmp = path.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            out << header << "\n";
            auto record = [&](char kind, const std::optional<BuildRecord>& r) {
                if (r)
                    out << kind << "\t" << r->wall << " " << r->cpu << " " << r->jobs_run << " "
                        << r->jobs_total << " " << r->threads << "\n";
            };
            record('L', last_build);
            record('F', last_full_build);
            char hash[32];
            for (const auto& [output, e] : entries)
            {
                std::snprintf(hash, sizeof(hash), "%016llx",
                              static_cast<unsigned long long>(e.hash));
                out << "E\t" << hash << "\t" << e.stamp << "\t" << e.seconds << "\t"
                    << output << "\t" << e.display << "\n";
                if (!e.source.empty())
                {
                    out << "S\t" << e.source << "\n";
                }
                for (const auto& d : e.deps)
                {
                    out << "D\t" << d << "\n";
                }
                if (!e.diagnostics.empty())
                {
                    out << "O\t" << escape_line(e.diagnostics) << "\n";
                }
            }
        }
        fs::rename(tmp, path, ec); // atomic: a crash never leaves a half written cache
    }

  private:
    static constexpr const char* header = "nobcpp-cache 2";
};

// ----------------------------------------------------------------------------------
// Jobs
// ----------------------------------------------------------------------------------

enum class JobType
{
    Compile,
    Link,
    Archive,
    Command // Target::add_command
};

enum class JobState
{
    UpToDate,
    Pending,
    Running,
    Done,
    Failed,
    Skipped
};

struct Job
{
    JobType type = JobType::Compile;
    std::string display; // source file for compiles, output file otherwise
    std::string output;
    std::string source;
    std::string depfile;
    std::vector<std::string> args;  // hashed, written to compile_commands.json
    std::vector<std::string> extra; // color flags, not hashed
    std::vector<std::string> inputs;
    std::vector<std::string> extra_outputs; // commands may write several files
    std::vector<int> deps;
    std::vector<int> dependents;

    bool dirty = false;
    bool has_history = false;
    bool foreign = false; // compiled by a Language's compiler (no C++ color flags)
    double estimate = 1.0;  // seconds, or relative weight if nothing was ever timed
    uint64_t source_bytes = 0;
    double priority = 0.0; // estimate + longest chain of dependents (critical path)

    JobState state = JobState::UpToDate;
    int pending_deps = 0;
    SteadyClock::time_point started;
    double seconds = 0;
    int64_t stamp = 0;
    ProcessResult result;
    std::string diagnostics; // output with /showIncludes lines removed
    std::vector<std::string> discovered_deps;
    int errors = 0;
    int warnings = 0;
};

// Parses a make style .d file as written by -MMD.
inline std::vector<std::string> parse_depfile(const std::string& content)
{
    std::vector<std::string> deps;
    size_t start = std::string::npos;
    for (size_t i = 0; i + 1 <= content.size(); ++i)
    {
        // the target ends at the first ':' followed by whitespace (not "C:/...")
        if (content[i] == ':' &&
            (i + 1 == content.size() || content[i + 1] == ' ' || content[i + 1] == '\n' ||
             content[i + 1] == '\r' || content[i + 1] == '\t'))
        {
            start = i + 1;
            break;
        }
    }
    if (start == std::string::npos)
    {
        return deps;
    }
    std::string current;
    for (size_t i = start; i < content.size(); ++i)
    {
        char c = content[i];
        if (c == '\\' && i + 1 < content.size() &&
            (content[i + 1] == '\n' || content[i + 1] == '\r'))
        {
            ++i; // line continuation
            if (content[i] == '\r' && i + 1 < content.size() && content[i + 1] == '\n')
                ++i;
            c = ' ';
        }
        else if (c == '\\' && i + 1 < content.size() && content[i + 1] == ' ')
        {
            current += ' ';
            ++i;
            continue;
        }
        else if (c == '$' && i + 1 < content.size() && content[i + 1] == '$')
        {
            current += '$';
            ++i;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
        {
            if (!current.empty())
            {
                deps.push_back(current);
                current.clear();
            }
            if (c == '\n')
            {
                break; // -MP style phony targets follow, we don't need them
            }
            continue;
        }
        current += c;
    }
    if (!current.empty())
    {
        deps.push_back(current);
    }
    return deps;
}

// Counts diagnostics in gcc/clang ("file:1:2: error:") and msvc
// ("file(1): error C1234:", "error LNK2019:") output.
inline void count_diagnostics(const std::string& output, int& errors, int& warnings)
{
    std::istringstream ss(strip_ansi(output));
    std::string line;
    while (std::getline(ss, line))
    {
        if (line.find(": error") != std::string::npos ||
            line.find(": fatal error") != std::string::npos ||
            line.find("error LNK") != std::string::npos)
        {
            ++errors;
        }
        else if (line.find(": warning") != std::string::npos ||
                 line.find("warning LNK") != std::string::npos)
        {
            ++warnings;
        }
    }
}

// ----------------------------------------------------------------------------------
// JSON (just enough to read CMake's file API replies)
// ----------------------------------------------------------------------------------

struct Json
{
    enum class Type
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Json> array;
    // object members; std::pair<std::string, Json> can't be used while Json is incomplete
    std::vector<std::string> keys;
    std::vector<Json> values;

    // Missing keys give a null value, so lookups can be chained.
    const Json& operator[](std::string_view key) const;

    static std::optional<Json> parse(std::string_view text)
    {
        size_t pos = 0;
        Json value;
        if (!parse_value(text, pos, value, 0))
        {
            return std::nullopt;
        }
        return value;
    }

  private:
    static void skip_space(std::string_view t, size_t& p)
    {
        while (p < t.size() && std::isspace(static_cast<unsigned char>(t[p]))) ++p;
    }

    static void append_utf8(std::string& out, unsigned cp)
    {
        if (cp < 0x80)
        {
            out += static_cast<char>(cp);
        }
        else if (cp < 0x800)
        {
            out += static_cast<char>(0xc0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
        else if (cp < 0x10000)
        {
            out += static_cast<char>(0xe0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
        else
        {
            out += static_cast<char>(0xf0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
    }

    static bool parse_hex4(std::string_view t, size_t p, unsigned& out)
    {
        if (p + 4 > t.size()) return false;
        out = 0;
        for (size_t i = p; i < p + 4; ++i)
        {
            char c = t[i];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= c - '0';
            else if (c >= 'a' && c <= 'f') out |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') out |= c - 'A' + 10;
            else return false;
        }
        return true;
    }

    static bool parse_string(std::string_view t, size_t& p, std::string& out)
    {
        if (p >= t.size() || t[p] != '"') return false;
        ++p;
        while (p < t.size())
        {
            char c = t[p++];
            if (c == '"') return true;
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (p >= t.size()) return false;
            char e = t[p++];
            switch (e)
            {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u':
            {
                unsigned cp = 0;
                if (!parse_hex4(t, p, cp)) return false;
                p += 4;
                unsigned low = 0;
                if (cp >= 0xd800 && cp <= 0xdbff && p + 6 <= t.size() && t[p] == '\\' &&
                    t[p + 1] == 'u' && parse_hex4(t, p + 2, low) && low >= 0xdc00 && low <= 0xdfff)
                {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                    p += 6;
                }
                append_utf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    static bool parse_value(std::string_view t, size_t& p, Json& v, int depth)
    {
        skip_space(t, p);
        if (p >= t.size() || depth > 256) return false;
        char c = t[p];
        if (c == '{' || c == '[')
        {
            bool is_object = c == '{';
            v.type = is_object ? Type::Object : Type::Array;
            ++p;
            skip_space(t, p);
            if (p < t.size() && t[p] == (is_object ? '}' : ']'))
            {
                ++p;
                return true;
            }
            while (true)
            {
                std::string key;
                if (is_object)
                {
                    skip_space(t, p);
                    if (!parse_string(t, p, key)) return false;
                    skip_space(t, p);
                    if (p >= t.size() || t[p] != ':') return false;
                    ++p;
                }
                Json item;
                if (!parse_value(t, p, item, depth + 1)) return false;
                if (is_object)
                {
                    v.keys.push_back(std::move(key));
                    v.values.push_back(std::move(item));
                }
                else
                {
                    v.array.push_back(std::move(item));
                }
                skip_space(t, p);
                if (p < t.size() && t[p] == ',')
                {
                    ++p;
                    continue;
                }
                if (p < t.size() && t[p] == (is_object ? '}' : ']'))
                {
                    ++p;
                    return true;
                }
                return false;
            }
        }
        if (c == '"')
        {
            v.type = Type::String;
            return parse_string(t, p, v.string);
        }
        for (auto [word, type, value] : {std::tuple{"true", Type::Bool, true},
                                         std::tuple{"false", Type::Bool, false},
                                         std::tuple{"null", Type::Null, false}})
        {
            std::string_view w = word;
            if (t.substr(p, w.size()) == w)
            {
                v.type = type;
                v.boolean = value;
                p += w.size();
                return true;
            }
        }
        size_t start = p;
        while (p < t.size() && std::strchr("+-0123456789.eE", t[p])) ++p;
        if (p == start) return false;
        v.type = Type::Number;
        v.number = std::strtod(std::string(t.substr(start, p - start)).c_str(), nullptr);
        return true;
    }
};

inline const Json& Json::operator[](std::string_view key) const
{
    static const Json null;
    for (size_t i = 0; i < keys.size(); ++i)
    {
        if (keys[i] == key) return values[i];
    }
    return null;
}

inline std::optional<std::string> read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return std::nullopt;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline void set_env(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// ----------------------------------------------------------------------------------
// Dependencies: fetching sources
// ----------------------------------------------------------------------------------

// Where to get a dependency from. Give either `git` (+ `ref` or `commit`) or `url`.
struct Source
{
    std::string git{};    // repository url (or local path)
    std::string ref{};    // tag or branch, resolved once and pinned in nobcpp.lock
    std::string commit{}; // exact commit, no lookup needed at all
    std::string url{};    // alternatively an archive: .tar.gz, .tar.xz, .zip
    bool submodules = false;
};

// Fetched sources are shared by all projects of the user, they never change once
// fetched (one directory per commit). Override with $NOBCPP_CACHE.
inline fs::path source_cache_dir()
{
    if (const char* d = std::getenv("NOBCPP_CACHE"); d && *d) return fs::path(d) / "src";
#ifdef _WIN32
    if (const char* d = std::getenv("LOCALAPPDATA"); d && *d) return fs::path(d) / "nobcpp" / "src";
#else
    if (const char* d = std::getenv("XDG_CACHE_HOME"); d && *d) return fs::path(d) / "nobcpp" / "src";
    if (const char* d = std::getenv("HOME"); d && *d) return fs::path(d) / ".cache" / "nobcpp" / "src";
#endif
    return fs::temp_directory_path() / "nobcpp" / "src";
}

// Fetched files are shown as "imgui/backends/imgui_impl_sdl3.cpp" instead of their
// full path in the cache.
inline std::string source_display_path(const fs::path& p)
{
    static const std::string cache = source_cache_dir().lexically_normal().generic_string() + "/";
    std::string s = display_path(p);
    if (!s.starts_with(cache))
    {
        return s;
    }
    std::string rest = s.substr(cache.size()); // "imgui-d88f238d1d36/imgui.cpp"
    size_t slash = rest.find('/');
    std::string dir = rest.substr(0, slash);
    size_t dash = dir.rfind('-');
    if (dir.ends_with("-sm"))
    {
        dash = dir.rfind('-', dash - 1);
    }
    return dir.substr(0, dash) + (slash == std::string::npos ? "" : rest.substr(slash));
}

inline std::string unique_suffix()
{
    return std::to_string(SteadyClock::now().time_since_epoch().count());
}

inline bool is_full_commit(const std::string& s)
{
    return s.size() == 40 && std::all_of(s.begin(), s.end(), [](char c) {
               return std::isxdigit(static_cast<unsigned char>(c));
           });
}

// Moves a finished download into the cache. Another nobcpp process may have been
// faster, then its copy is used.
inline bool publish_dir(const fs::path& tmp, const fs::path& dest)
{
    std::error_code ec;
    fs::rename(tmp, dest, ec);
    if (ec)
    {
        fs::remove_all(tmp, ec);
        return fs::exists(dest);
    }
    return true;
}

// nobcpp.lock pins tags and branches to the commit they pointed to when first used.
class Lockfile
{
  public:
    struct Entry
    {
        std::string url;
        std::string ref;
        std::string commit;
    };
    std::map<std::string, Entry> entries;

    explicit Lockfile(fs::path path) : path_(std::move(path))
    {
        std::ifstream in(path_);
        std::string line;
        while (std::getline(in, line))
        {
            strip_cr(line);
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ss(line);
            std::string name;
            Entry e;
            if (std::getline(ss, name, '\t') && std::getline(ss, e.url, '\t') &&
                std::getline(ss, e.ref, '\t') && std::getline(ss, e.commit, '\t'))
            {
                entries[name] = e;
            }
        }
    }

    void save() const
    {
        fs::path tmp = path_.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            out << "# nobcpp.lock: the exact commits of fetched dependencies, commit this file.\n"
                   "# './nobcpp update' moves tags and branches to their current commit.\n";
            for (const auto& [name, e] : entries)
            {
                out << name << "\t" << e.url << "\t" << e.ref << "\t" << e.commit << "\n";
            }
        }
        std::error_code ec;
        fs::rename(tmp, path_, ec);
    }

  private:
    fs::path path_;
};

inline std::string tail_lines(const std::string& text, int count)
{
    size_t pos = text.size();
    if (pos > 0 && text[pos - 1] == '\n') --pos;
    while (count-- > 0)
    {
        size_t nl = pos == 0 ? std::string::npos : text.rfind('\n', pos - 1);
        if (nl == std::string::npos) return text;
        pos = nl;
    }
    return text.substr(pos + 1);
}

// Runs a command, exits with a readable error if it fails.
inline ProcessResult run_or_explain(const std::vector<std::string>& cmd, const std::string& what)
{
    ProcessResult r = run_capture(cmd);
    if (r.exit_code != 0)
    {
        console().error(what + " failed:\n" + console().dim("$ " + join(cmd, " ")) + "\n" +
                        tail_lines(r.output, 30));
    }
    return r;
}

// Resolves a tag or branch to a commit with `git ls-remote`.
inline std::optional<std::string> resolve_git_ref(const std::string& url, const std::string& ref)
{
    if (is_full_commit(ref))
    {
        return ref;
    }
    ProcessResult r = run_or_explain({"git", "ls-remote", url, ref, ref + "^{}"},
                                     "looking up '" + ref + "' in " + url);
    if (r.exit_code != 0)
    {
        return std::nullopt;
    }
    // annotated tags: the peeled entry (^{}) is the commit, the plain one the tag object
    std::map<std::string, std::string> found;
    std::istringstream ss(r.output);
    std::string line;
    while (std::getline(ss, line))
    {
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        found[line.substr(tab + 1)] = line.substr(0, tab);
    }
    for (const std::string& name : {"refs/tags/" + ref + "^{}", "refs/tags/" + ref,
                                    "refs/heads/" + ref, ref})
    {
        if (auto it = found.find(name); it != found.end())
        {
            return it->second;
        }
    }
    console().error("'" + ref + "' is neither a tag nor a branch of " + url);
    return std::nullopt;
}

inline bool fetch_git(const std::string& url, const std::string& commit, bool submodules,
                      const fs::path& dest)
{
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    fs::path tmp = dest.string() + ".tmp-" + unique_suffix();
    std::string t = tmp.string();
    auto git = [&](std::vector<std::string> args) {
        std::vector<std::string> cmd = {"git", "-C", t, "-c", "advice.detachedHead=false"};
        append(cmd, args);
        return run_capture(cmd);
    };
    bool ok = run_capture({"git", "init", "-q", t}).exit_code == 0;
    // a shallow fetch of exactly the commit is fast, but not every server allows it
    if (ok && git({"fetch", "-q", "--depth", "1", url, commit}).exit_code == 0)
    {
        ok = git({"checkout", "-q", "--detach", "FETCH_HEAD"}).exit_code == 0;
    }
    else
    {
        fs::remove_all(tmp, ec);
        ok = run_or_explain({"git", "clone", "-q", url, t}, "cloning " + url).exit_code == 0 &&
             run_or_explain({"git", "-C", t, "-c", "advice.detachedHead=false", "checkout",
                             "-q", "--detach", commit},
                            "checking out " + commit)
                     .exit_code == 0;
    }
    if (ok && submodules)
    {
        ok = run_or_explain({"git", "-C", t, "submodule", "update", "-q", "--init", "--recursive",
                             "--depth", "1"},
                            "fetching submodules of " + url)
                 .exit_code == 0;
    }
    if (!ok)
    {
        fs::remove_all(tmp, ec);
        return false;
    }
    return publish_dir(tmp, dest);
}

inline bool fetch_archive(const std::string& url, const fs::path& dest)
{
    std::error_code ec;
    fs::path tmp = dest.string() + ".tmp-" + unique_suffix();
    fs::create_directories(tmp / "x", ec);
    std::string file = url.substr(0, url.find_first_of("?#"));
    file = (tmp / fs::path(file).filename()).string();
    bool ok = run_or_explain({"curl", "-fsSL", "-o", file, url}, "downloading " + url).exit_code == 0;
    if (ok)
    {
        // bsdtar (Windows, macOS) also unpacks zips, GNU tar needs unzip for them
        ok = run_capture({"tar", "-xf", file, "-C", (tmp / "x").string()}).exit_code == 0 ||
             (file.ends_with(".zip") &&
              run_or_explain({"unzip", "-q", file, "-d", (tmp / "x").string()},
                             "unpacking " + file)
                      .exit_code == 0);
        if (!ok) console().error("could not unpack " + file);
    }
    if (!ok)
    {
        fs::remove_all(tmp, ec);
        return false;
    }
    // most archives contain a single top level directory, use that as the root
    fs::path root = tmp / "x";
    std::vector<fs::path> entries;
    for (const auto& e : fs::directory_iterator(root, ec)) entries.push_back(e.path());
    if (entries.size() == 1 && fs::is_directory(entries[0])) root = entries[0];
    bool published = publish_dir(root, dest);
    fs::remove_all(tmp, ec);
    return published;
}

// A fetched source tree, see Build::fetch. Resolution and download happen lazily when
// a build actually needs the dependency (or when dir()/version() is converted to a path).
class Fetched : public std::enable_shared_from_this<Fetched>
{
  public:
    Fetched(std::string name, Source source, std::function<void(Fetched&)> resolver)
        : name_(std::move(name)), source_(std::move(source)), resolver_(std::move(resolver))
    {
    }

    const std::string& name() const { return name_; }
    const Source& source() const { return source_; }
    bool resolved() const { return resolved_; }

    // commit (or a hash of the archive url)
    const std::string& version() const
    {
        ensure_resolved();
        return version_;
    }

    // Lazy path to the fetched directory; Target/Build methods defer fetching until needed.
    FetchedPath dir() const
    {
        return FetchedPath(std::const_pointer_cast<Fetched>(shared_from_this()));
    }

    FetchedPath operator/(const fs::path& sub) const
    {
        return FetchedPath(std::const_pointer_cast<Fetched>(shared_from_this()), sub);
    }

    const fs::path& resolved_dir() const
    {
        ensure_resolved();
        return dir_;
    }

    void set_resolved(std::string version, fs::path dir)
    {
        version_ = std::move(version);
        dir_ = std::move(dir);
        resolved_ = true;
    }

    void ensure_resolved() const
    {
        if (!resolved_ && resolver_)
        {
            resolver_(const_cast<Fetched&>(*this));
        }
    }

  private:
    std::string name_;
    Source source_;
    std::function<void(Fetched&)> resolver_;
    mutable bool resolved_ = false;
    mutable std::string version_;
    mutable fs::path dir_;
};

inline fs::path FetchedPath::resolve() const
{
    const fs::path& base = dep_->resolved_dir();
    return sub_.empty() ? base : base / sub_;
}

inline FetchedPath::operator fs::path() const
{
    return resolve();
}

inline std::string FetchedPath::string() const
{
    return resolve().string();
}

inline std::string FetchedPath::generic_string() const
{
    return resolve().generic_string();
}

// How to build a fetched CMake project and what to use from it, see Build::cmake.
struct CMakeOptions
{
    std::vector<std::string> targets{};  // CMake targets to link: "SDL3::SDL3-static"
    std::vector<std::string> options{};  // configure options: "SDL_SHARED=OFF"
    std::string package{};               // find_package name, default: namespace of targets[0]
    std::vector<std::string> components{};      // find_package(... COMPONENTS ...)
    std::vector<std::string> probe_variables{}; // set before find_package: "SFML_STATIC_LIBRARIES=TRUE"
    std::string build_type{};            // default: Debug/Release/... from the active profiles
    std::string source_subdir{};         // if CMakeLists.txt is not at the root
    std::vector<std::shared_ptr<Target>> depends{}; // other cmake dependencies it needs
};

// The C compiler that belongs to a C++ compiler: g++-14 -> gcc-14, clang++ -> clang.
inline std::string c_compiler_for(const std::string& cxx)
{
    if (const char* cc = std::getenv("CC"); cc && *cc)
    {
        return cc;
    }
    fs::path p(cxx);
    std::string name = p.filename().string();
    for (auto [from, to] : {std::pair{"clang++", "clang"}, std::pair{"g++", "gcc"},
                            std::pair{"c++", "cc"}, std::pair{"icpx", "icx"}})
    {
        size_t pos = name.rfind(from);
        if (pos != std::string::npos)
        {
            name.replace(pos, std::strlen(from), to);
            return p.has_parent_path() ? (p.parent_path() / name).string() : name;
        }
    }
    return cxx; // cl and clang-cl compile C as well
}

// Splits a command line fragment ("-I/a -DX", "path\ with\ space") into arguments.
inline std::vector<std::string> split_args(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    bool in_token = false;
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i)
    {
        char c = s[i];
        if (quote)
        {
            if (c == quote) quote = 0;
            else cur += c;
        }
        else if (c == '"' || c == '\'')
        {
            quote = c;
            in_token = true;
        }
        else if (c == '\\' && i + 1 < s.size() && s[i + 1] == ' ')
        {
            cur += ' ';
            ++i;
            in_token = true;
        }
        else if (std::isspace(static_cast<unsigned char>(c)))
        {
            if (in_token) out.push_back(cur);
            cur.clear();
            in_token = false;
        }
        else
        {
            cur += c;
            in_token = true;
        }
    }
    if (in_token) out.push_back(cur);
    return out;
}

// Compile and link flags a CMake target hands to its users, as reported by CMake.
struct ImportedFlags
{
    std::vector<std::string> includes;
    std::vector<std::string> defines;
    std::vector<std::string> compile;
    std::vector<std::string> link;

    void save(const fs::path& path) const
    {
        std::ofstream out(path, std::ios::trunc);
        for (const auto& [kind, values] : {std::pair{'I', &includes}, std::pair{'D', &defines},
                                           std::pair{'C', &compile}, std::pair{'L', &link}})
        {
            for (const auto& v : *values) out << kind << "\t" << v << "\n";
        }
    }

    static std::optional<ImportedFlags> load(const fs::path& path)
    {
        std::ifstream in(path);
        if (!in) return std::nullopt;
        ImportedFlags f;
        std::string line;
        while (std::getline(in, line))
        {
            strip_cr(line);
            if (line.size() < 2) continue;
            std::string v = line.substr(2);
            switch (line[0])
            {
            case 'I': f.includes.push_back(v); break;
            case 'D': f.defines.push_back(v); break;
            case 'C': f.compile.push_back(v); break;
            case 'L': f.link.push_back(v); break;
            }
        }
        return f;
    }

    // Reads the reply of CMake's file API for the probe executable.
    static std::optional<ImportedFlags> from_file_api(const fs::path& build_dir, std::string& error)
    {
        fs::path reply = build_dir / ".cmake" / "api" / "v1" / "reply";
        std::vector<fs::path> indexes;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(reply, ec))
        {
            if (e.path().filename().string().starts_with("index-")) indexes.push_back(e.path());
        }
        if (indexes.empty())
        {
            error = "CMake wrote no file API reply (CMake 3.14 or newer is needed)";
            return std::nullopt;
        }
        std::sort(indexes.begin(), indexes.end());
        auto load = [&](const fs::path& p) {
            auto text = read_file(p);
            return text ? Json::parse(*text) : std::nullopt;
        };
        auto index = load(indexes.back());
        if (!index) return std::nullopt;
        auto model = load(reply / (*index)["reply"]["codemodel-v2"]["jsonFile"].string);
        if (!model || (*model)["configurations"].array.empty())
        {
            error = "unreadable CMake codemodel";
            return std::nullopt;
        }
        std::string target_file;
        for (const auto& t : (*model)["configurations"].array[0]["targets"].array)
        {
            if (t["name"].string == "nobcpp_probe") target_file = t["jsonFile"].string;
        }
        auto target = load(reply / target_file);
        if (target_file.empty() || !target)
        {
            error = "the probe target is missing from the CMake codemodel";
            return std::nullopt;
        }

        ImportedFlags f;
        for (const auto& group : (*target)["compileGroups"].array)
        {
            if (group["language"].string != "CXX") continue;
            for (const auto& inc : group["includes"].array) f.includes.push_back(inc["path"].string);
            for (const auto& def : group["defines"].array) f.defines.push_back(def["define"].string);
            for (const auto& frag : group["compileCommandFragments"].array)
            {
                for (const auto& a : split_args(frag["fragment"].string))
                {
                    // the language standard is ours to choose
                    if (!a.starts_with("-std=") && !a.starts_with("/std:") && !a.starts_with("-std:"))
                        f.compile.push_back(a);
                }
            }
        }
        for (const auto& frag : (*target)["link"]["commandFragments"].array)
        {
            append(f.link, split_args(frag["fragment"].string));
        }
        return f;
    }
};

// Compile time as a function of the source size: t = a * bytes^k, fitted in log space.
// Measured on real projects k is small (~0.2): includes dominate, a file 10x larger
// takes far less than 10x as long, so a linear "seconds per byte" is badly wrong.
struct SizeModel
{
    double a = 0;
    double k = 0;

    bool valid() const
    {
        return a > 0;
    }

    double predict(double bytes) const
    {
        return a * std::pow(std::max(bytes, 1.0), k);
    }

    // samples: (bytes, seconds)
    static SizeModel fit(const std::vector<std::pair<double, double>>& samples)
    {
        SizeModel m;
        if (samples.empty()) return m;
        double mx = 0, my = 0;
        for (const auto& [b, t] : samples)
        {
            mx += std::log(std::max(b, 1.0));
            my += std::log(std::max(t, 1e-3));
        }
        mx /= samples.size();
        my /= samples.size();
        double cov = 0, var = 0;
        for (const auto& [b, t] : samples)
        {
            double dx = std::log(std::max(b, 1.0)) - mx;
            cov += dx * (std::log(std::max(t, 1e-3)) - my);
            var += dx * dx;
        }
        // few or similar sized samples: no trend, the (geometric) mean time per file
        m.k = samples.size() >= 5 && var > 1e-6 ? std::clamp(cov / var, 0.0, 1.0) : 0.0;
        m.a = std::exp(my - m.k * mx);
        return m;
    }
};

// ----------------------------------------------------------------------------------
// Command line
// ----------------------------------------------------------------------------------

// nobcpp's commands, for parsing and 'help'.
inline const std::vector<std::pair<std::string, std::string>>& command_help()
{
    static const std::vector<std::pair<std::string, std::string>> cmds = {
        {"build", "build all targets (default), or named ones: build [targets...]"},
        {"rebuild", "clean and build (all targets, or named ones): rebuild [targets...]"},
        {"run", "build, then run an executable: run [target] [-- args...]"},
        {"clean", "remove the active profile's build directory"},
        {"cleanall", "remove the whole build directory, including the timing history"},
        {"compdb", "write compile_commands.json without building"},
        {"stats", "show how long each translation unit took to compile"},
        {"warnings", "replay stored warnings of all files: warnings [path filter...]"},
        {"tree", "show targets, sources and dependencies"},
        {"update", "move tags/branches of fetched dependencies to their latest commit: update [names...]"},
        {"cleancache", "delete fetched sources not used for some days (default 30): cleancache [days]"},
        {"help", "show this help"},
    };
    return cmds;
}

inline bool is_command(const std::string& s)
{
    for (const auto& [name, help] : command_help())
    {
        if (name == s) return true;
    }
    return false;
}

struct Options
{
    std::vector<std::string> positionals; // commands, profiles, targets
    std::vector<std::string> profiles;    // from -p
    std::vector<std::string> run_args;    // after --
    int jobs = 0;
    bool verbose = false;
    bool fail_fast = false;
    bool offline = false;
    ColorMode color = ColorMode::Auto;
    std::string cxx;

    // --name[=value] options nobcpp doesn't know: the build script's own (Build::option),
    // an error if none claims them
    struct Custom
    {
        std::string name;                 // without "--"
        std::optional<std::string> value; // --name=value
        int next_positional = -1;         // index in positionals of the argument after it
        bool claimed = false;
    };
    std::vector<Custom> custom;
};

// An option of the build script, as listed by 'help'.
struct ScriptOption
{
    std::string name;
    std::string value; // default, "" for flags
    std::string help;
    bool is_flag;
};

// The parsed command line: nobcpp's options and the positional arguments (commands,
// profiles, targets). Options it doesn't know are kept until the build script claims
// them with option() / flag(); check_unclaimed() reports the rest.
class CommandLine
{
  public:
    CommandLine(int argc, char** argv)
    {
        // hints print the command the way it was typed ("./nobcpp"), unless that was a
        // long absolute path
        program_ = argc > 0 ? argv[0] : "nobcpp";
        if (fs::path(program_).is_absolute())
        {
            program_ = fs::path(program_).filename().string();
        }
        parse(argc, argv);
    }

    const Options& options() const
    {
        return options_;
    }

    const std::string& program() const
    {
        return program_;
    }

    const std::vector<ScriptOption>& script_options() const
    {
        return script_options_;
    }

    [[noreturn]] void usage_error(const std::string& message) const
    {
        console().error(message);
        console().println("run '" + program_ + " help' for usage");
        std::exit(2);
    }

    std::string option(const std::string& name, const std::string& default_value, const std::string& help)
    {
        check_option_name(name);
        script_options_.push_back({name, default_value, help, false});
        std::string value = default_value;
        for (auto& opt : options_.custom)
        {
            if (opt.name != name) continue;
            opt.claimed = true;
            if (opt.value)
            {
                value = *opt.value;
            }
            else if (opt.next_positional >= 0 &&
                     opt.next_positional < static_cast<int>(options_.positionals.size()))
            {
                value = options_.positionals[opt.next_positional];
                options_.positionals[opt.next_positional].clear(); // not a command or profile
            }
            else
            {
                usage_error("missing value for --" + name);
            }
        }
        return value;
    }

    bool flag(const std::string& name, bool default_value, const std::string& help)
    {
        check_option_name(name);
        script_options_.push_back({name, "", help, true});
        bool value = default_value;
        for (auto& opt : options_.custom)
        {
            bool negated = opt.name == "no-" + name;
            if (opt.name != name && !negated) continue;
            opt.claimed = true;
            if (opt.value)
            {
                std::string v = str_lower(*opt.value);
                if (v != "true" && v != "false" && v != "1" && v != "0" && v != "on" && v != "off")
                    usage_error("--" + opt.name + " expects true or false, not '" + *opt.value + "'");
                value = v == "true" || v == "1" || v == "on";
            }
            else
            {
                value = true;
            }
            if (negated) value = !value;
        }
        return value;
    }

    // Before the positional arguments are interpreted: a typo in an option makes its
    // value look like an unknown command.
    void check_unclaimed() const
    {
        for (const auto& opt : options_.custom)
        {
            if (!opt.claimed)
            {
                usage_error("unknown option '--" + opt.name + "'" +
                            (script_options_.empty() ? "" : " (the build script's options are listed by help)"));
            }
        }
    }

  private:
    std::string program_;
    Options options_;
    std::vector<ScriptOption> script_options_;

    static void check_option_name(const std::string& name)
    {
        static const std::set<std::string> builtin = {"help", "verbose", "fail-fast", "offline", "no-color",
                                                      "color", "jobs", "profile", "cxx", "cc"};
        if (builtin.contains(name))
        {
            console().error("option '--" + name + "' is reserved by nobcpp");
            std::exit(1);
        }
    }

    void parse(int argc, char** argv)
    {
        auto value = [&](int& i, const std::string& arg, const std::string& name) {
            if (arg.size() > name.size() && arg.starts_with(name))
            {
                std::string rest = arg.substr(name.size());
                return rest[0] == '=' ? rest.substr(1) : rest;
            }
            if (i + 1 >= argc)
            {
                usage_error("missing value for " + name);
            }
            return std::string(argv[++i]);
        };
        for (int i = 1; i < argc; ++i)
        {
            std::string arg = argv[i];
            if (arg == "--")
            {
                for (++i; i < argc; ++i)
                {
                    options_.run_args.push_back(argv[i]);
                }
            }
            else if (arg == "-h" || arg == "--help")
            {
                options_.positionals.push_back("help");
            }
            else if (arg == "-v" || arg == "--verbose")
            {
                options_.verbose = true;
            }
            else if (arg == "--fail-fast")
            {
                options_.fail_fast = true;
            }
            else if (arg == "--offline")
            {
                options_.offline = true;
            }
            else if (arg == "--no-color")
            {
                options_.color = ColorMode::Never;
            }
            else if (arg == "--color" || arg.starts_with("--color="))
            {
                std::string v = value(i, arg, "--color");
                if (v == "always") options_.color = ColorMode::Always;
                else if (v == "never") options_.color = ColorMode::Never;
                else if (v == "auto") options_.color = ColorMode::Auto;
                else usage_error("--color must be auto, always or never");
            }
            else if (arg == "--jobs" || arg.starts_with("--jobs=") || arg == "-j" ||
                     (arg.size() > 2 && arg.starts_with("-j") &&
                      (arg[2] == '=' || std::isdigit(static_cast<unsigned char>(arg[2])))))
            {
                std::string v = value(i, arg, arg.starts_with("-j") ? "-j" : "--jobs");
                try
                {
                    size_t pos = 0;
                    options_.jobs = std::stoi(v, &pos);
                    if (pos != v.size())
                    {
                        usage_error("invalid job count '" + v + "'");
                    }
                }
                catch (...)
                {
                    usage_error("invalid job count '" + v + "'");
                }
            }
            else if (arg == "--profile" || arg.starts_with("--profile=") || arg == "-p" ||
                     arg.starts_with("-p="))
            {
                std::string v = value(i, arg, arg.starts_with("-p") ? "-p" : "--profile");
                append(options_.profiles, split(v, ','));
            }
            else if (arg == "--cxx" || arg.starts_with("--cxx=") || arg == "--cc" ||
                     arg.starts_with("--cc="))
            {
                options_.cxx = value(i, arg, arg.starts_with("--cxx") ? "--cxx" : "--cc");
            }
            else if (arg.size() > 2 && arg.starts_with("--"))
            {
                // maybe an option of the build script, checked once it registered them
                Options::Custom opt;
                size_t eq = arg.find('=');
                opt.name = arg.substr(2, eq == std::string::npos ? std::string::npos : eq - 2);
                if (eq != std::string::npos)
                {
                    opt.value = arg.substr(eq + 1);
                }
                else if (i + 1 < argc && argv[i + 1][0] != '-')
                {
                    opt.next_positional = static_cast<int>(options_.positionals.size());
                }
                options_.custom.push_back(opt);
            }
            else if (arg.size() > 1 && arg[0] == '-')
            {
                usage_error("unknown option '" + arg + "'");
            }
            else
            {
                options_.positionals.push_back(arg);
            }
        }
    }
};

// ----------------------------------------------------------------------------------
// Profiles, targets, languages and where outputs go
// ----------------------------------------------------------------------------------

// Named sets of flags. Several can be active at once and are merged in order.
class Profiles
{
  public:
    Profiles()
    {
        set("debug", {.opt = Opt::O0, .debug_info = true});
        set("release", {.opt = Opt::O3, .debug_info = false, .defines = {"NDEBUG"}});
        set("relwithdebinfo", {.opt = Opt::O2, .debug_info = true, .defines = {"NDEBUG"}});
        set("asan", {.debug_info = true, .sanitizers = {"address", "undefined"}});
        set("tsan", {.debug_info = true, .sanitizers = {"thread"}});
    }

    void set(const std::string& name, const Profile& p)
    {
        if (!profiles_.contains(name))
        {
            order_.push_back(name);
        }
        profiles_[name] = p;
    }

    bool contains(const std::string& name) const
    {
        return profiles_.contains(name);
    }

    const std::vector<std::string>& names() const
    {
        return order_;
    }

    // The profiles selected on the command line (-p and profile names among the
    // positional arguments), in order and without repetitions, or `fallback`.
    std::vector<std::string> selected(const Options& o, const std::string& fallback) const
    {
        std::vector<std::string> out = o.profiles;
        for (const auto& p : o.positionals)
        {
            if (!is_command(p) && profiles_.contains(p))
            {
                out.push_back(p);
            }
        }
        if (out.empty())
        {
            out.push_back(fallback);
        }
        std::vector<std::string> unique;
        for (const auto& p : out)
        {
            if (std::find(unique.begin(), unique.end(), p) == unique.end())
            {
                unique.push_back(p);
            }
        }
        return unique;
    }

    Flags merged(const std::vector<std::string>& names) const
    {
        Flags f;
        for (const auto& name : names)
        {
            f.merge(profiles_.at(name));
        }
        return f;
    }

  private:
    std::map<std::string, Profile> profiles_;
    std::vector<std::string> order_;
};

// All targets of the build script.
class Targets
{
  public:
    // A library and an executable may share a name (libastra.a and astra), names only
    // have to be unique per kind of target.
    std::shared_ptr<Target> add(const std::string& name, Kind kind)
    {
        for (const auto& t : all_)
        {
            if (t->name() == name && t->kind() == kind)
            {
                console().error("duplicate target name '" + name + "'");
                std::exit(1);
            }
        }
        auto target = std::make_shared<Target>(name, kind);
        all_.push_back(target);
        return target;
    }

    const std::vector<std::shared_ptr<Target>>& all() const
    {
        return all_;
    }

    // An executable wins if a library has the same name.
    std::shared_ptr<Target> find(const std::string& name) const
    {
        std::shared_ptr<Target> found;
        for (const auto& t : all_)
        {
            if (t->name() != name) continue;
            if (t->kind() == Kind::Executable) return t;
            if (!found) found = t;
        }
        return found;
    }

    // obj/<name>, or obj/<name>.<kind> if another kind of target has the same name
    std::string object_dir_name(const Target& t) const
    {
        for (const auto& other : all_)
        {
            if (other.get() != &t && other->name() == t.name())
            {
                const char* kinds[] = {"exe", "lib", "shared", "header"};
                return t.name() + "." + kinds[static_cast<int>(t.kind())];
            }
        }
        return t.name();
    }

    // Exits with the cycle if targets link each other in a circle.
    void check_cycles() const
    {
        std::unordered_map<const Target*, int> state; // 0 = unseen, 1 = visiting, 2 = done
        std::vector<const Target*> stack;
        std::function<void(const Target&)> dfs = [&](const Target& t) {
            state[&t] = 1;
            stack.push_back(&t);
            for (const auto& dep : t.deps())
            {
                int s = state[dep.get()];
                if (s == 1)
                {
                    auto it = std::find(stack.begin(), stack.end(), dep.get());
                    std::vector<std::string> cycle;
                    for (; it != stack.end(); ++it) cycle.push_back((*it)->name());
                    cycle.push_back(dep->name());
                    console().error("cyclic target dependency: " + join(cycle, " → "));
                    std::exit(1);
                }
                if (s == 0) dfs(*dep);
            }
            stack.pop_back();
            state[&t] = 2;
        };
        for (const auto& t : all_)
        {
            if (state[t.get()] == 0) dfs(*t);
        }
    }

  private:
    std::vector<std::shared_ptr<Target>> all_;
};

// The C++ standard library and the build script's own source don't count as tracked
// headers: only files inside the project, like -MMD.
inline std::string str_lower_if_windows(std::string s)
{
    return target_windows ? str_lower(std::move(s)) : s;
}

inline bool inside_project(const std::string& path)
{
    fs::path p(path);
    if (p.is_relative() && !p.has_root_name())
    {
        return true;
    }
    static const std::string root = str_lower_if_windows(fs::current_path().generic_string() + "/");
    return str_lower_if_windows(p.lexically_normal().generic_string()).starts_with(root);
}

// build/release, or build/release-clang++ for a compiler chosen with --cxx (or always
// with Build::compiler_in_build_dir): comparing compilers never mixes their outputs.
inline std::string profile_dir_name(const std::vector<std::string>& profiles, std::string cxx)
{
    std::string name = join(profiles, "-");
    if (!cxx.empty())
    {
        std::string compiler = fs::path(split(cxx, ' ').back()).filename().string();
        for (char& ch : compiler)
        {
            if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '+' && ch != '-' && ch != '.')
                ch = '_';
        }
        name += "-" + compiler;
    }
    return name;
}

// Where the outputs of one profile go and how they are named.
struct Layout
{
    fs::path out_dir;                              // build/<profiles>[-compiler]
    std::function<const Toolchain&()> toolchain;   // detected on first use
    const Targets* targets = nullptr;

    fs::path cache_file() const
    {
        return out_dir / ".nobcpp_cache";
    }

    std::string obj_dir() const
    {
        return (out_dir / "obj").generic_string() + "/";
    }

    fs::path output(const Target& t) const
    {
        return out_dir / (t.output_name.empty() ? toolchain().output_name(t.name(), t.kind()) : t.output_name);
    }

    // Object paths only depend on the source's own path (relative to the project, or
    // "imgui/..." for fetched sources), so adding or removing other files never moves
    // objects (which would mean recompiling them and losing their timings).
    fs::path object(const Target& t, const fs::path& src) const
    {
        fs::path rel = fs::path(source_display_path(src)).lexically_normal();
        if (rel.is_absolute() || rel.has_root_name())
        {
            rel = rel.relative_path();
        }
        std::string mangled;
        for (const auto& part : rel)
        {
            std::string s = part.string();
            if (s.empty() || s == ".") continue;
            if (!mangled.empty()) mangled += "/";
            mangled += s == ".." ? "__" : s;
        }
        return out_dir / "obj" / targets->object_dir_name(t) / (mangled + toolchain().obj_ext());
    }
};

inline const Language* language_for(const std::vector<Language>& languages, const fs::path& src)
{
    const std::string ext = src.extension().string();
    for (const auto& lang : languages)
    {
        if (std::find(lang.extensions.begin(), lang.extensions.end(), ext) != lang.extensions.end())
            return &lang;
    }
    return nullptr;
}

// gcc style command line for a Language's compiler
inline std::vector<std::string> language_compile_command(const Language& lang, const Flags& f,
                                                         const std::string& src, const std::string& obj,
                                                         const std::string& depfile, bool pic)
{
    std::vector<std::string> cmd = lang.compiler;
    if (lang.inherit_flags)
    {
        if (f.std) cmd.push_back("-std=c++" + std::to_string(*f.std));
        if (f.opt)
        {
            const char* o[] = {"-O0", "-O1", "-O1", "-O2", "-O3", "-Os"}; // no -Og everywhere
            cmd.push_back(o[static_cast<int>(*f.opt)]);
        }
        if (f.debug_info.value_or(false)) cmd.push_back("-g");
        for (const auto& d : f.defines) cmd.push_back("-D" + d);
        for (const auto& i : f.include_dirs) cmd.push_back("-I" + i);
        for (const auto& i : f.system_include_dirs) append(cmd, {"-isystem", i});
    }
    if (pic && !target_windows)
    {
        bool nvcc = fs::path(lang.compiler.back()).stem().string() == "nvcc";
        cmd.push_back(nvcc ? "-Xcompiler=-fPIC" : "-fPIC");
    }
    append(cmd, lang.flags);
    cmd = dedupe_flags(cmd);
    append(cmd, {"-MD", "-MF", depfile, "-c", src, "-o", obj});
    return cmd;
}

inline std::string short_name(const Job& job)
{
    return fs::path(job.display).filename().string();
}

inline std::string counts_text(int errors, int warnings)
{
    auto& c = console();
    std::vector<std::string> parts;
    if (errors)
        parts.push_back(c.red(std::to_string(errors) + (errors == 1 ? " error" : " errors")));
    if (warnings)
        parts.push_back(c.yellow(std::to_string(warnings) + (warnings == 1 ? " warning" : " warnings")));
    return join(parts, ", ");
}

// ----------------------------------------------------------------------------------
// Planning: targets -> jobs
// ----------------------------------------------------------------------------------

// The jobs of one build and their dependencies, made by Planner::plan(). Jobs come in
// dependency order: a job's dependencies have smaller indices.
struct JobGraph
{
    std::vector<Job> jobs;
    std::unordered_map<std::string, int> by_output;
    bool timing_known = false; // estimates are seconds (else relative file size weights)
};

// Turns targets into jobs: their command lines, which job waits for which, whether a
// job has to run (command hash and file times against the Cache) and how long it
// probably takes. Reads the file system and the cache, changes nothing.
class Planner
{
  public:
    struct Settings
    {
        const Toolchain& toolchain;
        const Flags& global;  // Build::flags
        const Flags& profile; // the active profiles, merged
        const Layout& layout;
        const Targets& targets;
        const std::vector<Language>& languages;
        const Cache& cache;
    };

    explicit Planner(const Settings& settings) : s_(settings)
    {
    }

    // Plans `roots` and everything they link, or all targets if `roots` is empty.
    JobGraph plan(const std::vector<std::shared_ptr<Target>>& roots)
    {
        graph_ = {};
        job_by_target_.clear();
        command_jobs_.clear();
        mtimes_.clear();
        seconds_by_display_.clear();
        for (const auto& [output, e] : s_.cache.entries)
        {
            seconds_by_display_[e.display] = e.seconds;
        }
        pic_targets_.clear();
        std::function<void(const Target&)> mark_pic = [&](const Target& t) {
            if (pic_targets_.insert(&t).second)
                for (const auto& dep : t.deps()) mark_pic(*dep);
        };
        for (const auto& t : s_.targets.all())
        {
            if (t->kind() == Kind::SharedLib) mark_pic(*t);
        }
        s_.targets.check_cycles();
        for (const auto& t : roots.empty() ? s_.targets.all() : roots)
        {
            plan_target(*t);
        }

        estimate_unknown_durations();
        // jobs are created in dependency order, so walk backwards for critical paths
        for (int i = static_cast<int>(graph_.jobs.size()) - 1; i >= 0; --i)
        {
            double longest = 0;
            for (int d : graph_.jobs[i].dependents)
            {
                if (graph_.jobs[d].dirty) longest = std::max(longest, graph_.jobs[d].priority);
            }
            graph_.jobs[i].priority = graph_.jobs[i].estimate + longest;
        }
        return std::move(graph_);
    }

  private:
    Settings s_;
    JobGraph graph_;
    std::unordered_map<const Target*, int> job_by_target_;
    std::unordered_map<const Target*, std::vector<int>> command_jobs_;
    std::set<const Target*> pic_targets_; // shared libs and everything linked into them
    std::unordered_map<std::string, double> seconds_by_display_;
    std::unordered_map<std::string, std::optional<fs::file_time_type>> mtimes_;

    std::optional<fs::file_time_type> cached_mtime(const std::string& path)
    {
        auto it = mtimes_.find(path);
        if (it != mtimes_.end())
        {
            return it->second;
        }
        return mtimes_[path] = file_mtime(path);
    }

    bool is_dirty(Job& job)
    {
        for (int d : job.deps)
        {
            // commands only order jobs: their outputs are not inputs of a link, unless a
            // dirty command overwrites one of this job's inputs or tracked headers (below)
            if (graph_.jobs[d].dirty && graph_.jobs[d].type != JobType::Command) return true;
        }
        auto it = s_.cache.entries.find(job.output);
        if (it == s_.cache.entries.end() || it->second.hash != hash_args(job.args))
        {
            return true;
        }
        if (!cached_mtime(job.output) ||
            std::any_of(job.extra_outputs.begin(), job.extra_outputs.end(),
                        [&](const std::string& o) { return !cached_mtime(o); }))
        {
            return true;
        }
        const CacheEntry& entry = it->second;
        auto same_path = [](const std::string& a, const std::string& b) {
            if (a == b) return true;
            if (fs::path(a).is_absolute() == fs::path(b).is_absolute()) return false;
            std::error_code ec1, ec2;
            return fs::absolute(a, ec1).lexically_normal() ==
                   fs::absolute(b, ec2).lexically_normal();
        };
        auto overwritten_by_command = [&](const std::string& path) {
            for (int d : job.deps)
            {
                const Job& dep = graph_.jobs[d];
                if (!dep.dirty || dep.type != JobType::Command) continue;
                if (same_path(dep.output, path)) return true;
                for (const auto& eo : dep.extra_outputs)
                {
                    if (same_path(eo, path)) return true;
                }
            }
            return false;
        };
        auto newer = [&](const std::string& input) {
            if (overwritten_by_command(input)) return true;
            auto t = cached_mtime(input);
            // a missing input (deleted header) forces a recompile instead of crashing
            return !t || t->time_since_epoch().count() > entry.stamp;
        };
        return std::any_of(job.inputs.begin(), job.inputs.end(), newer) ||
               std::any_of(entry.deps.begin(), entry.deps.end(), newer);
    }

    int add_job(Job job)
    {
        if (auto it = s_.cache.entries.find(job.output); it != s_.cache.entries.end())
        {
            job.estimate = it->second.seconds;
            job.has_history = true;
        }
        else if (auto by_source = seconds_by_display_.find(job.display);
                 by_source != seconds_by_display_.end())
        {
            // same source, different output path (e.g. after nobcpp changed the object
            // layout): its duration is still the best estimate
            job.estimate = by_source->second;
            job.has_history = true;
        }
        job.dirty = is_dirty(job);
        int id = static_cast<int>(graph_.jobs.size());
        for (int d : job.deps)
        {
            graph_.jobs[d].dependents.push_back(id);
        }
        graph_.by_output[job.output] = id;
        graph_.jobs.push_back(std::move(job));
        return id;
    }

    // Targets this one links against, dependents before their dependencies.
    void collect_link_deps(const Target& t, std::vector<const Target*>& order,
                           std::set<const Target*>& seen)
    {
        for (auto it = t.deps().rbegin(); it != t.deps().rend(); ++it)
        {
            const auto& dep = *it;
            if (seen.insert(dep.get()).second)
            {
                // a shared library already contains its static dependencies
                if (dep->kind() != Kind::SharedLib)
                {
                    collect_link_deps(*dep, order, seen);
                }
                order.push_back(dep.get());
            }
        }
    }

    int plan_target(const Target& t)
    {
        if (auto it = job_by_target_.find(&t); it != job_by_target_.end())
        {
            return it->second;
        }
        const Toolchain& tc = s_.toolchain;

        std::vector<const Target*> link_deps;
        std::set<const Target*> seen;
        collect_link_deps(t, link_deps, seen);
        std::reverse(link_deps.begin(), link_deps.end());

        // global -> profile -> public flags of dependencies -> own flags: the more
        // specific setting wins, like a target_compile_options() in CMake
        Flags f = s_.global;
        f.merge(s_.profile);
        for (auto it = link_deps.rbegin(); it != link_deps.rend(); ++it)
        {
            f.merge((*it)->public_flags);
        }
        f.merge(t.public_flags);
        f.merge(t.flags);

        // Linker libraries must appear dependents before dependencies (GNU ld resolves
        // static archives left to right).
        f.link_flags.clear();
        f.libs.clear();
        auto append_link = [&](const Flags& src) {
            append(f.link_flags, src.link_flags);
            append(f.libs, src.libs);
        };
        append_link(t.flags);
        append_link(t.public_flags);
        for (const Target* dep : link_deps)
        {
            append_link(dep->public_flags);
        }
        append_link(s_.profile);
        append_link(s_.global);

        std::vector<int> dep_command_jobs;
        std::vector<int> deps;
        std::vector<std::string> inputs;
        for (const Target* dep : link_deps)
        {
            if (int id = plan_target(*dep); id >= 0)
            {
                deps.push_back(id);
            }
            append(dep_command_jobs, command_jobs_[dep]);
        }

        std::vector<int> command_jobs;
        for (const auto& cmd : t.commands())
        {
            if (cmd.outputs.empty())
            {
                console().error("a command of '" + t.name() + "' has no outputs");
                std::exit(1);
            }
            Job job;
            job.type = JobType::Command;
            job.args = cmd.args;
            job.inputs = cmd.inputs;
            job.output = cmd.outputs[0];
            job.extra_outputs.assign(cmd.outputs.begin() + 1, cmd.outputs.end());
            job.deps = dep_command_jobs;
            job.display = cmd.inputs.empty() ? job.output : source_display_path(cmd.inputs[0]);
            if (graph_.by_output.contains(job.output))
            {
                console().error("'" + job.output + "' is the output of two commands");
                std::exit(1);
            }
            command_jobs.push_back(add_job(std::move(job)));
        }
        command_jobs_[&t] = command_jobs;

        if (t.kind() == Kind::HeaderOnly)
        {
            if (!t.sources().empty())
            {
                console().warning("sources of header only target '" + t.name() +
                                  "' are ignored");
            }
            job_by_target_[&t] = -1;
            return -1;
        }

        std::vector<int> compile_deps = dep_command_jobs;
        append(compile_deps, command_jobs);
        append(deps, compile_deps);

        bool pic = pic_targets_.contains(&t);
        for (const auto& src : t.sources())
        {
            fs::path obj = s_.layout.object(t, src);
            Job job;
            job.type = JobType::Compile;
            job.display = source_display_path(src);
            job.source = src.generic_string();
            job.output = obj.generic_string();
            job.depfile = job.output + ".d";
            if (const Language* lang = language_for(s_.languages, src))
            {
                job.args = language_compile_command(*lang, f, job.source, job.output, job.depfile, pic);
                job.foreign = true;
            }
            else
            {
                job.args = tc.compile_command(f, job.source, job.output, job.depfile, pic);
            }
            job.inputs = {job.source};
            job.deps = compile_deps; // own and linked targets' generated headers
            if (graph_.by_output.contains(job.output))
            {
                console().error("source '" + job.display + "' added twice to target '" +
                                t.name() + "'");
                std::exit(1);
            }
            int id = add_job(std::move(job));
            deps.push_back(id);
            inputs.push_back(graph_.jobs[id].output);
        }
        // objects that become part of this target (made by commands, prebuilt, ...)
        auto add_objects = [&](const Target& owner) {
            for (const auto& o : owner.objects())
            {
                inputs.push_back(o);
                if (auto it = graph_.by_output.find(o); it != graph_.by_output.end()) deps.push_back(it->second);
            }
        };
        add_objects(t);
        if (t.sources().empty() && t.deferred_paths().empty() && t.objects().empty())
        {
            console().warning("target '" + t.name() + "' has no sources");
        }

        Job job;
        job.output = s_.layout.output(t).generic_string();
        job.display = job.output;
        if (t.kind() == Kind::StaticLib)
        {
            job.type = JobType::Archive;
            job.args = tc.archive_command(inputs, job.output);
        }
        else
        {
            bool needs_rpath = false;
            for (const Target* dep : link_deps)
            {
                if (dep->kind() == Kind::HeaderOnly) // nothing to link but its objects
                {
                    add_objects(*dep);
                    continue;
                }
                std::string out = s_.layout.output(*dep).generic_string();
                // msvc links against the import library of a dll
                if (dep->kind() == Kind::SharedLib && tc.msvc_like())
                    out = (s_.layout.out_dir / (dep->name() + ".lib")).generic_string();
                inputs.push_back(out);
                needs_rpath |= dep->kind() == Kind::SharedLib;
            }
            job.type = JobType::Link;
            job.args = tc.link_command(f, t.kind(), inputs, job.output, needs_rpath);
            // files among the link flags (imported libraries, objects) are inputs too:
            // when one changes, the link reruns
            for (const auto& flag : f.link_flags)
            {
                if (flag.empty() || flag[0] == '-') continue;
                if (tc.msvc_like() && flag[0] == '/' && flag.find('/', 1) == std::string::npos)
                    continue; // an msvc option like /DEBUG, not a path
                if (std::find(inputs.begin(), inputs.end(), flag) != inputs.end()) continue;
                if (auto it = graph_.by_output.find(flag); it != graph_.by_output.end())
                    deps.push_back(it->second);
                else if (!cached_mtime(flag))
                    continue;
                inputs.push_back(flag);
            }
        }
        job.inputs = inputs;
        job.deps = deps;
        int id = add_job(std::move(job));
        job_by_target_[&t] = id;
        return id;
    }

    // Files without a recorded time (new, or never compiled successfully) are estimated
    // from their size with a model fitted to all timed files (see SizeModel). With no
    // timing at all, estimates become relative weights (size^0.25) for the progress bar
    // and no time is predicted until the build itself has timed some files.
    void estimate_unknown_durations()
    {
        std::vector<std::pair<double, double>> samples;
        double weight_sum = 0;
        int compiles = 0;
        for (auto& j : graph_.jobs)
        {
            if (j.type != JobType::Compile) continue;
            std::error_code ec;
            uintmax_t size = fs::file_size(j.source, ec);
            j.source_bytes = ec ? 0 : size;
            weight_sum += std::pow(std::max<double>(j.source_bytes, 1.0), 0.25);
            ++compiles;
            if (j.has_history && j.source_bytes > 0) samples.emplace_back(j.source_bytes, j.estimate);
        }
        const SizeModel model = SizeModel::fit(samples);
        graph_.timing_known = model.valid();
        const double mean_weight = compiles ? weight_sum / compiles : 1.0;

        // links, archives and commands without history: the average of their kind
        double sums[2] = {0, 0};
        int counts[2] = {0, 0};
        for (const auto& j : graph_.jobs)
        {
            if (j.type == JobType::Compile || !j.has_history) continue;
            int k = j.type == JobType::Command ? 1 : 0;
            sums[k] += j.estimate;
            counts[k]++;
        }
        for (auto& j : graph_.jobs)
        {
            const double bytes = std::max<double>(j.source_bytes, 1.0);
            const double factor = j.type == JobType::Command ? 0.1 : 0.3; // relative to a compile
            if (!graph_.timing_known)
            {
                j.estimate = j.type == JobType::Compile ? std::pow(bytes, 0.25) : factor * mean_weight;
                j.has_history = false;
            }
            else if (!j.has_history)
            {
                int k = j.type == JobType::Command ? 1 : 0;
                if (j.type == JobType::Compile)
                    j.estimate = model.predict(bytes);
                else
                    j.estimate = counts[k] ? sums[k] / counts[k] : factor * model.a;
            }
        }
    }
};

// ----------------------------------------------------------------------------------
// Execution: runs the dirty jobs of a JobGraph
// ----------------------------------------------------------------------------------

class Executor
{
  public:
    struct Settings
    {
        const Toolchain& toolchain;
        int jobs = 0; // 0: number of cores
        bool verbose = false;
        bool fail_fast = false;
    };

    struct Result
    {
        bool ok = true;
        double wall = 0;
        int threads = 0;
        std::optional<double> forecast; // predicted wall time, if every job was timed before
    };

    Executor(JobGraph& graph, const Settings& settings) : graph_(graph), s_(settings)
    {
    }

    // Runs all dirty jobs, the longest remaining chain first. Fills each job's state,
    // output, diagnostics and duration.
    Result run()
    {
        Result result;
        auto& con = console();
        std::vector<int> todo;
        for (int i = 0; i < static_cast<int>(graph_.jobs.size()); ++i)
        {
            Job& j = graph_.jobs[i];
            j.state = j.dirty ? JobState::Pending : JobState::UpToDate;
            if (j.dirty) todo.push_back(i);
        }
        if (todo.empty())
        {
            return result;
        }

        int parallel = s_.jobs;
        if (parallel <= 0)
        {
            parallel = std::max(1u, std::thread::hardware_concurrency());
        }
        parallel = std::min<int>(parallel, static_cast<int>(todo.size()));
        result.threads = parallel;

        auto cmp = [&](int a, int b) { return graph_.jobs[a].priority < graph_.jobs[b].priority; };
        std::priority_queue<int, std::vector<int>, decltype(cmp)> ready(cmp);
        for (int i : todo)
        {
            Job& j = graph_.jobs[i];
            j.pending_deps = 0;
            for (int d : j.deps)
            {
                if (graph_.jobs[d].dirty) j.pending_deps++;
            }
            if (j.pending_deps == 0) ready.push(i);
        }

        std::mutex m;
        std::condition_variable cv;
        int outstanding = static_cast<int>(todo.size());
        int finished = 0;
        int running = 0;
        bool stop = false;
        bool any_failed = false;
        double total_estimate = 0;
        for (int i : todo)
        {
            total_estimate += graph_.jobs[i].estimate;
        }
        // seconds: time based progress and a remaining time, else file size weights
        const bool use_history = graph_.timing_known;
        if (use_history)
        {
            double critical = 0;
            for (int i : todo) critical = std::max(critical, graph_.jobs[i].priority);
            result.forecast = std::max(total_estimate / std::max(1, parallel), critical);
        }
        double done_estimate = 0;
        const int total = static_cast<int>(todo.size());
        const int width = static_cast<int>(std::to_string(total).size());
        auto start = SteadyClock::now();

        // Called with `m` held.
        std::function<void(int)> skip = [&](int id) {
            for (int d : graph_.jobs[id].dependents)
            {
                Job& dep = graph_.jobs[d];
                if (dep.state == JobState::Pending)
                {
                    dep.state = JobState::Skipped;
                    --outstanding;
                    skip(d);
                }
            }
        };

        auto progress_prefix = [&](int n) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "[%*d/%d] ", width, n, total);
            return std::string(buf);
        };

        auto worker = [&]() {
            std::unique_lock<std::mutex> lock(m);
            while (true)
            {
                cv.wait(lock, [&] { return !ready.empty() || outstanding == 0 || stop; });
                if (stop || ready.empty())
                {
                    return;
                }
                int id = ready.top();
                ready.pop();
                Job& job = graph_.jobs[id];
                job.state = JobState::Running;
                job.started = SteadyClock::now();
                ++running;
                lock.unlock();

                run_job(job);

                lock.lock();
                bool ok = job.result.exit_code == 0;
                job.state = ok ? JobState::Done : JobState::Failed;
                --outstanding;
                --running;
                ++finished;
                done_estimate += job.estimate;
                if (ok)
                {
                    for (int d : job.dependents)
                    {
                        Job& dep = graph_.jobs[d];
                        if (dep.state == JobState::Pending && --dep.pending_deps == 0)
                            ready.push(d);
                    }
                }
                else
                {
                    any_failed = true;
                    skip(id);
                    if (s_.fail_fast) stop = true;
                }

                // One line per job when not on a terminal (CI logs) or when verbose,
                // on a terminal only failures are announced, details come at the end.
                std::string line;
                if (!con.tty() || s_.verbose || !ok)
                {
                    line = con.dim(progress_prefix(finished));
                    line += ok ? "" : con.red("FAILED ");
                    line += job.display + con.dim("  " + format_duration(job.seconds));
                    if (job.errors || job.warnings)
                        line += "  " + counts_text(ok ? 0 : job.errors, job.warnings);
                    line += "\n";
                    if (s_.verbose) line += con.dim(join(job.args, " ")) + "\n";
                }
                if (!line.empty()) con.print(line);
                cv.notify_all();
            }
        };

        std::vector<std::thread> pool;
        for (int i = 0; i < parallel; ++i)
        {
            pool.emplace_back(worker);
        }

        // progress bar
        {
            std::unique_lock<std::mutex> lock(m);
            while (!(outstanding == 0 || (stop && running == 0)))
            {
                if (con.tty())
                {
                    con.set_status(render_status(todo, finished, total, done_estimate,
                                                 total_estimate, use_history, parallel,
                                                 start, any_failed));
                }
                cv.wait_for(lock, std::chrono::milliseconds(100));
            }
            stop = true;
        }
        cv.notify_all();
        for (auto& t : pool)
        {
            t.join();
        }
        con.clear_status();
        result.wall = seconds_since(start);

        for (int i : todo)
        {
            if (graph_.jobs[i].state == JobState::Pending) graph_.jobs[i].state = JobState::Skipped;
        }
        result.ok = !any_failed;
        return result;
    }

  private:
    JobGraph& graph_;
    Settings s_;

    std::vector<std::string> final_args(const Job& job) const
    {
        std::vector<std::string> args = job.args;
        // only the compiler driver understands them, `ar`/`lib` would choke on them
        if (console().color() && !job.foreign && (job.type == JobType::Compile || job.type == JobType::Link))
        {
            append(args, s_.toolchain.color_flags());
        }
#ifdef _WIN32
        // CreateProcess is limited to 32k characters, use a response file
        if (windows_command_line(args).size() > 30000)
        {
            size_t prefix = s_.toolchain.cxx.size();
            if (job.type == JobType::Archive) prefix = 1;
            std::string rsp = job.output + ".rsp";
            std::ofstream out(rsp, std::ios::trunc);
            for (size_t i = prefix; i < args.size(); ++i)
            {
                if (s_.toolchain.msvc_like())
                {
                    out << quote_arg_windows(args[i]) << "\n";
                }
                else
                {
                    std::string quoted = "\"";
                    for (char ch : args[i])
                    {
                        if (ch == '\\' || ch == '"') quoted += '\\';
                        quoted += ch;
                    }
                    out << quoted << "\"\n";
                }
            }
            args.resize(prefix);
            args.push_back("@" + rsp);
        }
#endif
        return args;
    }

    void run_job(Job& job)
    {
        std::error_code ec;
        fs::create_directories(fs::path(job.output).parent_path(), ec);
        if (job.type == JobType::Archive)
        {
            fs::remove(job.output, ec); // `ar rcs` would keep members of deleted sources
        }
        job.stamp = file_clock_now();
        auto start = SteadyClock::now();
        if (job.type == JobType::Command && job.args.empty())
        {
            fs::copy_file(job.inputs[0], job.output, fs::copy_options::overwrite_existing, ec);
            job.result = {ec ? 1 : 0, ec ? "copying " + job.inputs[0] + " failed: " + ec.message() + "\n" : "", true};
            job.seconds = seconds_since(start);
            job.diagnostics = job.result.output;
            job.errors = ec ? 1 : 0;
            return;
        }
        job.result = run_capture(final_args(job));
        job.seconds = seconds_since(start);

        if (job.type == JobType::Compile && s_.toolchain.msvc_like())
        {
            const std::string prefix = "Note: including file:";
            std::istringstream ss(job.result.output);
            std::string line;
            bool first = true;
            std::string filename = fs::path(job.source).filename().string();
            while (std::getline(ss, line))
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.starts_with(prefix))
                {
                    size_t p = line.find_first_not_of(' ', prefix.size());
                    if (p != std::string::npos) job.discovered_deps.push_back(line.substr(p));
                }
                else if (!(first && line == filename)) // cl echoes the source name
                {
                    job.diagnostics += line + "\n";
                }
                first = false;
            }
        }
        else
        {
            job.diagnostics = job.result.output;
            if (job.type == JobType::Compile && job.result.exit_code == 0)
            {
                std::ifstream in(job.depfile);
                std::stringstream content;
                content << in.rdbuf();
                job.discovered_deps = parse_depfile(content.str());
                in.close();
                fs::remove(job.depfile, ec);
            }
        }
        count_diagnostics(job.diagnostics, job.errors, job.warnings);
        if (job.result.exit_code != 0 && job.errors == 0)
        {
            job.errors = 1;
        }
    }

    // Called with the executor mutex held.
    std::string render_status(const std::vector<int>& todo, int finished, int total,
                              double done_estimate, double total_estimate,
                              bool use_history, int parallel,
                              SteadyClock::time_point start, bool any_failed) const
    {
        auto& c = console();
        auto now = SteadyClock::now();
        double done = done_estimate;
        double critical = 0;
        std::vector<std::pair<SteadyClock::time_point, std::string>> running;
        int remaining = 0;
        for (int i : todo)
        {
            const Job& j = graph_.jobs[i];
            if (j.state == JobState::Running)
            {
                double elapsed = std::chrono::duration<double>(now - j.started).count();
                if (use_history) done += std::min(elapsed, j.estimate * 0.95);
                critical = std::max(critical, j.priority - elapsed);
                running.emplace_back(j.started, short_name(j));
                ++remaining;
            }
            else if (j.state == JobState::Pending)
            {
                critical = std::max(critical, j.priority);
                ++remaining;
            }
        }
        double elapsed_total = seconds_since(start);
        std::optional<double> eta;
        bool from_sizes = false; // part of the forecast is estimated from file sizes
        if (use_history)
        {
            eta = std::max((total_estimate - done) / std::max(1, std::min(parallel, remaining)),
                           critical);
            for (int i : todo)
            {
                const Job& j = graph_.jobs[i];
                if ((j.state == JobState::Pending || j.state == JobState::Running) && !j.has_history)
                    from_sizes = true;
            }
        }
        else
        {
            // Nothing was timed before: once enough files compiled, fit the size model to
            // this build and extrapolate. The first files to finish are the fast ones, so
            // running jobs count too: one that has run for 10s takes at least that long.
            std::vector<std::pair<double, double>> samples;
            int compiles = 0;
            for (int i : todo)
            {
                const Job& j = graph_.jobs[i];
                if (j.type != JobType::Compile) continue;
                ++compiles;
                if (j.state == JobState::Done && j.source_bytes > 0)
                    samples.emplace_back(j.source_bytes, j.seconds);
            }
            if (samples.size() >= 3 && static_cast<int>(samples.size()) * 4 >= compiles)
            {
                const SizeModel model = SizeModel::fit(samples);
                double link_estimate = 0;
                for (const auto& [b, t] : samples) link_estimate += t;
                link_estimate = 0.3 * link_estimate / samples.size();
                double work = 0, longest = 0;
                for (int i : todo)
                {
                    const Job& j = graph_.jobs[i];
                    if (j.state != JobState::Pending && j.state != JobState::Running) continue;
                    double expected = j.type == JobType::Compile ? model.predict(j.source_bytes)
                                                                 : link_estimate;
                    double left = expected;
                    if (j.state == JobState::Running)
                    {
                        double ran = std::chrono::duration<double>(now - j.started).count();
                        // past its prediction: nobody knows how long it still takes, assume
                        // a part of what it already ran (long jobs tend to stay long)
                        left = expected > ran ? expected - ran : 0.3 * ran;
                    }
                    work += left;
                    longest = std::max(longest, left);
                }
                eta = std::max(work / std::max(1, std::min(parallel, remaining)), longest);
                from_sizes = true;
            }
        }
        // with a forecast: the share of the expected wall time that has passed, so the bar
        // and the remaining time agree; else the share of the (file size weighted) work
        double fraction = use_history && eta ? elapsed_total / std::max(elapsed_total + *eta, 1e-9)
                                             : done / std::max(total_estimate, 1e-9);
        fraction = std::clamp(fraction, 0.0, 1.0);

        const int bar_width = 24;
        int filled = static_cast<int>(fraction * bar_width + 0.5);
        std::string bar;
        for (int i = 0; i < bar_width; ++i) bar += i < filled ? "█" : "░";

        char pct[64];
        std::snprintf(pct, sizeof(pct), "%3d%%", static_cast<int>(fraction * 100));
        std::string status = c.dim("[" + std::to_string(finished) + "/" +
                                   std::to_string(total) + "] ") +
                             (any_failed ? c.red(bar) : c.green(bar)) + " " + pct + "  " +
                             format_duration(elapsed_total);
        if (eta)
        {
            // blue: from recorded times, yellow: (partly) estimated from file sizes
            std::string forecast = "~" + format_duration(std::max(*eta, 0.0)) + " left (≈" +
                                   format_duration(elapsed_total + std::max(*eta, 0.0)) + ")";
            status += "  " + (from_sizes ? c.paint("33", forecast) : c.blue(forecast));
        }
        std::sort(running.begin(), running.end());
        std::vector<std::string> names;
        for (const auto& r : running) names.push_back(r.second);
        status += "  " + c.cyan(join(names, " "));
        return status;
    }
};

// ----------------------------------------------------------------------------------
// History: what a build leaves in the Cache for the next one
// ----------------------------------------------------------------------------------

// Per job: the command hash, when it started, how long it took, the headers it read
// and its warnings; per build: the wall time. A failed job keeps the duration of its
// last successful run but is never current.
inline void record_build(Cache& cache, const JobGraph& graph, const Executor::Result& result, const Layout& layout,
                         bool full_build)
{
    BuildRecord record{result.wall, 0, 0, static_cast<int>(graph.jobs.size()), result.threads};
    for (const auto& j : graph.jobs)
    {
        if (j.state == JobState::Done)
        {
            record.cpu += j.seconds;
            record.jobs_run++;
        }
    }
    if (result.ok && record.jobs_run > 0)
    {
        cache.last_build = record;
        if (full_build && record.jobs_run == record.jobs_total)
        {
            cache.last_full_build = record;
        }
    }
    // Objects of deleted source files are removed (and forgotten), only ever inside this
    // profile's obj/ directory. Outputs that are merely not part of this build stay:
    // a target that is defined conditionally (--gpu none, no Python) must find its
    // objects and timings again when it comes back. Partial builds don't plan every
    // target, only full builds clean up.
    if (full_build)
    {
        const std::string obj_dir = layout.obj_dir();
        for (auto it = cache.entries.begin(); it != cache.entries.end();)
        {
            const std::string& source = it->second.source;
            if (graph.by_output.contains(it->first) || source.empty() || fs::exists(source))
            {
                ++it;
                continue;
            }
            if (it->first.starts_with(obj_dir))
            {
                std::error_code ec;
                fs::remove(it->first, ec);
            }
            it = cache.entries.erase(it);
        }
    }
    for (const auto& j : graph.jobs)
    {
        if (j.state == JobState::Done)
        {
            CacheEntry& e = cache.entries[j.output];
            e.hash = hash_args(j.args);
            e.stamp = j.stamp;
            e.seconds = j.seconds;
            e.display = j.display;
            e.diagnostics = has_text(j.diagnostics) ? j.diagnostics : "";
            if (j.type == JobType::Compile)
            {
                e.source = j.source;
                e.deps.clear();
                for (const auto& d : j.discovered_deps)
                {
                    if (inside_project(d))
                        e.deps.push_back(fs::path(d).lexically_normal().generic_string());
                }
            }
        }
        else if (j.state == JobState::Failed)
        {
            // never treat a failed output as current, but keep the duration of the
            // last successful run: a failed run says nothing about the real time
            if (auto it = cache.entries.find(j.output); it != cache.entries.end())
            {
                it->second.hash = 0;
                it->second.diagnostics.clear();
            }
        }
    }
    cache.save(layout.cache_file());

}

// ----------------------------------------------------------------------------------
// Reporting
// ----------------------------------------------------------------------------------

class Reporter
{
  public:
    // `program` as typed ("./nobcpp"), `profile` e.g. "release+asan"
    Reporter(std::string program, std::string profile, bool verbose)
        : program_(std::move(program)), profile_(std::move(profile)), verbose_(verbose)
    {
    }

    // Diagnostics of every job that ran, one atomic block per file, sorted by file.
    void diagnostics(const JobGraph& g) const
    {
        auto& c = console();
        std::vector<const Job*> noisy;
        for (const auto& j : g.jobs)
        {
            bool failed = j.state == JobState::Failed;
            if (failed || ((j.state == JobState::Done) && has_text(j.diagnostics)))
                noisy.push_back(&j);
        }
        std::sort(noisy.begin(), noisy.end(),
                  [](const Job* a, const Job* b) { return a->display < b->display; });
        for (const Job* j : noisy)
        {
            bool failed = j->state == JobState::Failed;
            // the command only helps when the output alone doesn't explain the failure
            bool show_cmd = failed && (verbose_ || j->type != JobType::Compile);
            std::string block = diagnostic_block(j->display, j->diagnostics,
                                                 failed ? j->errors : 0, j->warnings,
                                                 show_cmd ? join(j->args, " ") : "");
            if (failed && !j->result.started)
                block += c.red("could not start the command") + "\n";
            c.print(block);
        }
    }

    // Warnings of files that were not recompiled are only summarized, they are often
    // from third party headers. `warnings` replays them.
    void hidden_warnings(const JobGraph& g, const Cache& cache) const
    {
        auto& c = console();
        std::vector<std::pair<int, std::string>> files;
        int total = 0;
        for (const auto& j : g.jobs)
        {
            if (j.state != JobState::UpToDate) continue;
            auto it = cache.entries.find(j.output);
            if (it == cache.entries.end() || it->second.diagnostics.empty()) continue;
            int errors = 0, warnings = 0;
            count_diagnostics(it->second.diagnostics, errors, warnings);
            if (warnings == 0) continue;
            files.emplace_back(warnings, j.display);
            total += warnings;
        }
        if (files.empty())
        {
            return;
        }
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : a.second < b.second;
        });
        std::string s = c.yellow("! ") + std::to_string(total) +
                        (total == 1 ? " warning" : " warnings") + " hidden in " +
                        std::to_string(files.size()) +
                        (files.size() == 1 ? " unchanged file" : " unchanged files") +
                        c.dim(" (show with '" + program_ + " warnings [filter]')") + "\n";
        const size_t max_lines = 5;
        for (size_t i = 0; i < std::min(files.size(), max_lines); ++i)
        {
            char count[16];
            std::snprintf(count, sizeof(count), "%6d  ", files[i].first);
            s += c.yellow(count) + files[i].second + "\n";
        }
        if (files.size() > max_lines)
        {
            s += c.dim("        ... and " + std::to_string(files.size() - max_lines) +
                       " more files") + "\n";
        }
        c.print(s);
    }

    // The result line: what was built, how long it took, the slowest files.
    void summary(const JobGraph& g, const Executor::Result& r) const
    {
        const bool ok = r.ok;
        const double wall = r.wall;
        auto& c = console();
        int built = 0, skipped = 0, up_to_date = 0, errors = 0, warnings = 0;
        double cpu = 0;
        std::vector<const Job*> done;
        std::vector<const Job*> failed_jobs;
        for (const auto& j : g.jobs)
        {
            switch (j.state)
            {
            case JobState::Done: built++; done.push_back(&j); break;
            case JobState::Failed: failed_jobs.push_back(&j); break;
            case JobState::Skipped: skipped++; break;
            case JobState::UpToDate: up_to_date++; break;
            default: break;
            }
            if (j.state == JobState::Done || j.state == JobState::Failed)
            {
                cpu += j.seconds;
                warnings += j.warnings;
                if (j.state == JobState::Failed) errors += j.errors;
            }
        }
        const std::string& profile = profile_;
        std::string s = "\n";
        if (!ok)
        {
            std::sort(failed_jobs.begin(), failed_jobs.end(),
                      [](const Job* a, const Job* b) { return a->display < b->display; });
            s += c.red("✗ build failed") + " (" + profile + ", " + format_duration(wall) +
                 "): " + counts_text(errors, warnings) + "\n";
            for (const Job* j : failed_jobs)
            {
                s += "    " + c.red("✗ ") + j->display + "  " + counts_text(j->errors, 0) + "\n";
            }
            if (skipped)
            {
                s += c.dim("    " + std::to_string(skipped) +
                           " job(s) not run because of the failure") + "\n";
            }
            c.print(s);
            return;
        }
        if (built == 0)
        {
            c.println(c.green("✓ ") + profile + ": everything up to date (" +
                      std::to_string(up_to_date) + " jobs)");
            return;
        }
        s += c.green("✓ ") + profile + ": built " + std::to_string(built) + "/" +
             std::to_string(g.jobs.size()) + " jobs in " + c.bold(format_duration(wall));
        if (r.forecast)
        {
            s += c.dim(" (forecast " + format_duration(*r.forecast) + ")");
        }
        if (wall > 0.05)
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), " (cpu %s, %.1fx parallel)",
                          format_duration(cpu).c_str(), cpu / wall);
            s += c.dim(buf);
        }
        if (warnings) s += ", " + counts_text(0, warnings);
        s += "\n";
        std::sort(done.begin(), done.end(),
                  [](const Job* a, const Job* b) { return a->seconds > b->seconds; });
        if (done.size() > 2)
        {
            std::vector<std::string> slow;
            for (size_t i = 0; i < std::min<size_t>(3, done.size()); ++i)
                slow.push_back(done[i]->display + " " + format_duration(done[i]->seconds));
            s += c.dim("  slowest: " + join(slow, " · ") + "  (see '" + program_ + " stats')") +
                 "\n";
        }
        c.print(s);
    }

    // Replays the stored output of every file, filtered by substrings of its path.
    void stored_warnings(const Cache& cache, const std::vector<std::string>& filters) const
    {
        auto& c = console();
        std::vector<const CacheEntry*> entries;
        for (const auto& [output, e] : cache.entries)
        {
            bool match = filters.empty() ||
                         std::any_of(filters.begin(), filters.end(), [&](const auto& f) {
                             return e.display.find(f) != std::string::npos;
                         });
            if (match && !e.diagnostics.empty()) entries.push_back(&e);
        }
        std::sort(entries.begin(), entries.end(),
                  [](const auto* a, const auto* b) { return a->display < b->display; });
        int total = 0;
        for (const CacheEntry* e : entries)
        {
            int errors = 0, warnings = 0;
            count_diagnostics(e->diagnostics, errors, warnings);
            total += warnings;
            c.print(diagnostic_block(e->display, e->diagnostics, 0, warnings, ""));
        }
        const std::string& profile = profile_;
        if (entries.empty())
        {
            c.println(c.green("✓ ") + profile + ": no warnings stored" +
                      (filters.empty() ? "" : " for files matching '" + join(filters, "', '") + "'"));
            return;
        }
        c.println("\n" + c.yellow("! ") + profile + ": " + std::to_string(total) +
                  (total == 1 ? " warning" : " warnings") + " in " +
                  std::to_string(entries.size()) + (entries.size() == 1 ? " file" : " files") +
                  c.dim(" (from the last compile of each file)"));
    }

    // The table of job times and what limits the build (critical path, threads).
    void stats(const JobGraph& g, const Cache& cache, int jobs_option) const
    {
        auto& c = console();
        std::vector<const Job*> recorded;
        double sum = 0;
        for (const auto& j : g.jobs)
        {
            sum += j.estimate;
            if (j.has_history) recorded.push_back(&j);
        }
        const std::string& profile = profile_;
        if (recorded.empty())
        {
            c.println("no timing data for profile '" + profile + "' yet, build first");
            return;
        }
        std::sort(recorded.begin(), recorded.end(),
                  [](const Job* a, const Job* b) { return a->estimate > b->estimate; });

        std::string s = c.bold("Job times") +
                        c.dim(" (" + profile + ", the most recent run of each job)") + "\n";
        const int bar_width = 30;
        double max = recorded[0]->estimate;
        for (const Job* j : recorded)
        {
            int filled = static_cast<int>(j->estimate / std::max(max, 1e-9) * bar_width + 0.5);
            std::string bar;
            for (int i = 0; i < bar_width; ++i) bar += i < filled ? "█" : " ";
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%9s %5.1f%% ", format_duration(j->estimate).c_str(),
                          100.0 * j->estimate / std::max(sum, 1e-9));
            s += buf + c.cyan(bar) + " " + j->display + "\n";
        }

        // Longest chain of jobs that depend on each other (compile -> archive -> link):
        // no build can be faster than this, however many threads it uses.
        std::vector<double> chain(g.jobs.size(), 0.0);
        std::vector<int> next(g.jobs.size(), -1);
        for (int i = static_cast<int>(g.jobs.size()) - 1; i >= 0; --i)
        {
            chain[i] = g.jobs[i].estimate;
            for (int d : g.jobs[i].dependents)
            {
                if (g.jobs[i].estimate + chain[d] > chain[i])
                {
                    chain[i] = g.jobs[i].estimate + chain[d];
                    next[i] = d;
                }
            }
        }
        int first = static_cast<int>(std::max_element(chain.begin(), chain.end()) - chain.begin());
        std::vector<std::string> path;
        for (int i = first; i >= 0; i = next[i])
        {
            path.push_back(short_name(g.jobs[i]));
        }
        double critical = chain[first];

        // Best case: the build simulated with 1..N threads, never more threads than jobs.
        int cores = std::max(1u, std::thread::hardware_concurrency());
        int max_threads = std::min(jobs_option > 0 ? jobs_option : cores,
                                   static_cast<int>(g.jobs.size()));
        double fastest = simulate_full_build(g, max_threads, chain);
        int useful = max_threads;
        for (int p = 1; p < max_threads; ++p)
        {
            if (simulate_full_build(g, p, chain) <= fastest * 1.01)
            {
                useful = p;
                break;
            }
        }

        auto row = [&](const std::string& label, double seconds, const std::string& how) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%-16s %9s  ", label.c_str(),
                          format_duration(seconds).c_str());
            return c.bold(buf) + c.dim(how) + "\n";
        };
        auto threads_text = [](int n) { return std::to_string(n) + (n == 1 ? " thread" : " threads"); };
        const std::string indent(28, ' ');

        s += "\n";
        s += row("sum of job times", sum,
                 "all " + std::to_string(g.jobs.size()) +
                     " jobs one after another, what a build with -j1 takes");
        if (recorded.size() < g.jobs.size())
        {
            std::vector<std::string> names;
            for (const auto& j : g.jobs)
            {
                if (!j.has_history) names.push_back(short_name(j));
            }
            if (names.size() > 4)
            {
                names.resize(4);
                names.push_back("...");
            }
            s += indent + c.dim(std::to_string(g.jobs.size() - recorded.size()) +
                                " jobs have no recorded time yet and are estimated from their file size (" +
                                join(names, ", ") + ")") + "\n";
        }
        if (const auto& r = cache.last_full_build)
        {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "wall clock, %d jobs on %s, %.1fx parallel",
                          r->jobs_run, threads_text(r->threads).c_str(),
                          r->cpu / std::max(r->wall, 1e-9));
            s += row("last full build", r->wall, buf);
        }
        if (const auto& r = cache.last_build;
            r && (!cache.last_full_build || r->jobs_run != r->jobs_total ||
                  r->jobs_total != static_cast<int>(g.jobs.size())))
        {
            s += row("last build", r->wall,
                     "wall clock, " + std::to_string(r->jobs_run) + " of " +
                         std::to_string(r->jobs_total) + " jobs on " + threads_text(r->threads));
        }
        s += row("critical path", critical,
                 join(path, " → ") + ", these depend on each other and run in sequence");
        s += row("best case", fastest,
                 "with -j" + std::to_string(useful) +
                     ", simulated from the job times and their dependencies");
        if (fastest <= critical * 1.01)
        {
            s += indent + c.dim("reaches the critical path, more threads can't make it faster") +
                 "\n";
        }
        else if (useful < max_threads)
        {
            s += indent + c.dim("more threads don't help, the dependencies don't allow more "
                                "jobs to run at the same time") + "\n";
        }
        else if (max_threads == static_cast<int>(g.jobs.size()))
        {
            s += indent + c.dim("every job gets its own thread") + "\n";
        }
        else
        {
            s += indent + c.dim("limited by " + threads_text(max_threads) +
                                (jobs_option > 0 ? " (-j)" : " (cores)") +
                                ", more would still help") + "\n";
        }
        c.print(s);
    }

  private:
    std::string program_;
    std::string profile_;
    bool verbose_;

    // One file's diagnostics: a header line with the counts, then the compiler output.
    std::string diagnostic_block(const std::string& display, std::string text, int errors,
                                 int warnings, const std::string& command) const
    {
        auto& c = console();
        int width = std::min(c.width(), 100);
        std::string counts = counts_text(errors, warnings);
        std::string title = "── " + display + " ";
        int rule = width - static_cast<int>(title.size()) -
                   static_cast<int>(strip_ansi(counts).size()) - 2;
        std::string line;
        for (int i = 0; i < std::max(rule, 3); ++i) line += "─";
        std::string block = "\n" + (errors ? c.red(title) : c.yellow(title)) + c.dim(line) +
                            " " + counts + "\n";
        if (!command.empty())
        {
            block += c.dim("$ " + command) + "\n";
        }
        if (!c.color())
        {
            text = strip_ansi(text); // replayed output may have been captured in color
        }
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        if (!text.empty()) block += text + "\n";
        return block;
    }

    // Duration of a full build with `parallel` jobs, scheduled like execute() does
    // (longest remaining chain first), using the recorded durations.
    static double simulate_full_build(const JobGraph& g, int parallel, const std::vector<double>& priority)
    {
        const int n = static_cast<int>(g.jobs.size());
        std::vector<int> pending(n);
        auto by_priority = [&](int a, int b) { return priority[a] < priority[b]; };
        std::priority_queue<int, std::vector<int>, decltype(by_priority)> ready(by_priority);
        for (int i = 0; i < n; ++i)
        {
            pending[i] = static_cast<int>(g.jobs[i].deps.size());
            if (pending[i] == 0) ready.push(i);
        }
        using Running = std::pair<double, int>; // finish time, job
        std::priority_queue<Running, std::vector<Running>, std::greater<Running>> running;
        double now = 0;
        while (!ready.empty() || !running.empty())
        {
            while (static_cast<int>(running.size()) < parallel && !ready.empty())
            {
                int id = ready.top();
                ready.pop();
                running.emplace(now + g.jobs[id].estimate, id);
            }
            auto [finish, id] = running.top();
            running.pop();
            now = finish;
            for (int d : g.jobs[id].dependents)
            {
                if (--pending[d] == 0) ready.push(d);
            }
        }
        return now;
    }
};

// compile_commands.json for clangd, only rewritten when it changes.
inline void write_compdb(const JobGraph& g)
{
    std::string cwd = fs::current_path().generic_string();
    std::string json = "[\n";
    bool first = true;
    auto entry = [&](const std::vector<std::string>& args, const std::string& file,
                     const std::string& output) {
        json += first ? "" : ",\n";
        first = false;
        json += "  {\n    \"directory\": \"" + json_escape(cwd) + "\",\n";
        json += "    \"file\": \"" + json_escape(fs::absolute(file).generic_string()) + "\",\n";
        if (!output.empty())
            json += "    \"output\": \"" + json_escape(output) + "\",\n";
        json += "    \"arguments\": [";
        for (size_t i = 0; i < args.size(); ++i)
            json += (i ? ", \"" : "\"") + json_escape(args[i]) + "\"";
        json += "]\n  }";
    };
    for (const auto& j : g.jobs)
    {
        if (j.type == JobType::Compile) entry(j.args, j.source, j.output);
    }
    if (!self_source_path().empty())
    {
        entry(self_compile_command(self_source_path(), "nobcpp"), self_source_path(), "");
    }
    json += "\n]\n";

    // only touch the file when it changed, clangd re-indexes on every write
    std::ifstream in("compile_commands.json");
    std::stringstream old;
    old << in.rdbuf();
    in.close();
    if (old.str() != json)
    {
        std::ofstream("compile_commands.json", std::ios::trunc) << json;
    }
}

// ----------------------------------------------------------------------------------
// Dependencies: fetch(), cmake(), pkg_config()
// ----------------------------------------------------------------------------------

// The CMake build type that matches the active profiles.
inline std::string cmake_build_type(const Flags& profile)
{
    bool debug_info = profile.debug_info.value_or(false);
    switch (profile.opt.value_or(Opt::O0))
    {
    case Opt::O2:
    case Opt::O3: return debug_info ? "RelWithDebInfo" : "Release";
    case Opt::Os: return "MinSizeRel";
    default: return "Debug";
    }
}

// Fetched sources, the lockfile, CMake and pkg-config dependencies. Declared while the
// build script runs, resolved lazily: only what a command needs is downloaded, built
// or asked for.
class Dependencies
{
  public:
    struct PrepareSettings
    {
        const Toolchain& toolchain;
        int jobs = 0;            // 0: number of cores
        fs::path deps_dir;       // build/_deps
        std::string build_type;  // for cmake() dependencies without their own
    };

    Dependencies(Targets& targets, bool offline, bool verbose)
        : targets_(targets), offline_(offline), verbose_(verbose)
    {
    }

    std::shared_ptr<Fetched> fetch(const std::string& name, const Source& source);
    std::shared_ptr<Target> cmake(std::shared_ptr<Fetched> source, const CMakeOptions& options);
    std::shared_ptr<Target> pkg_config(const std::string& package, bool static_libs);

    // `roots` and everything they need, all targets if `roots` is empty.
    std::set<const Target*> needed(const std::vector<std::shared_ptr<Target>>& roots) const;

    // Resolves fetched sources, pkg-config packages and cmake() dependencies that the
    // `roots` need (independent CMake builds run in parallel).
    bool prepare(const std::vector<std::shared_ptr<Target>>& roots, const PrepareSettings& settings);

    // Without network or builds: what the lockfile and the source cache already have
    // (for 'stats' and 'cleancache').
    void resolve_cached_only()
    {
        for (const auto& f : fetched_deps_)
        {
            try_resolve_cached(*f);
        }
        for (const auto& t : targets_.all())
        {
            bool all_ready = !t->deferred_paths().empty() &&
                             std::all_of(t->deferred_paths().begin(), t->deferred_paths().end(),
                                         [](const Target::DeferredPath& dp) {
                                             return dp.path.fetched()->resolved();
                                         });
            if (all_ready)
            {
                t->resolve_deferred_paths();
            }
        }
    }

    bool is_fetched(const std::string& name) const
    {
        return fetched_names_.contains(name);
    }

    std::vector<std::string> fetched_names() const
    {
        return {fetched_names_.begin(), fetched_names_.end()};
    }

    // Moves tags and branches of fetched dependencies to their latest commit. A full
    // update also drops lockfile entries of dependencies the build script doesn't fetch
    // anymore (only then: a dependency fetched conditionally, e.g. only if Python is
    // installed, must keep its pin during normal builds on other machines).
    void update(const std::vector<std::string>& names)
    {
        auto& c = console();
        std::set<const Fetched*> updated;
        for (const auto& f : fetched_deps_)
        {
            bool match = names.empty() ||
                         std::find(names.begin(), names.end(), f->name()) != names.end();
            if (!match) continue;
            resolve_fetched(*f, true);
            updated.insert(f.get());
        }
        for (auto& d : cmake_deps_)
        {
            if (updated.contains(d->source.get()))
            {
                d->prepared = false;
                d->state = CMakeDep::State::Waiting;
                d->built = false;
            }
        }
        if (names.empty() && fs::exists("nobcpp.lock"))
        {
            if (!lockfile_) lockfile_ = std::make_unique<Lockfile>("nobcpp.lock");
            for (auto it = lockfile_->entries.begin(); it != lockfile_->entries.end();)
            {
                if (fetched_names_.contains(it->first))
                {
                    ++it;
                    continue;
                }
                c.println(c.cyan("nobcpp.lock: ") + "removed " + it->first + c.dim(" (not fetched anymore)"));
                it = lockfile_->entries.erase(it);
            }
            lockfile_->save();
        }
        c.println(c.green("✓ ") + (names.empty() ? std::string("all fetched dependencies") : join(names, ", ")) +
                  " at the latest commit of their tag or branch (nobcpp.lock)");
    }

    // Deletes fetched sources that no project used for `days` days. The cache is shared
    // by all projects; sources of this one are always kept.
    void cleancache(int days)
    {
        for (const auto& f : fetched_deps_)
        {
            try_resolve_cached(*f);
        }
        auto& c = console();
        fs::path root = source_cache_dir();
        std::error_code ec;
        const auto now = fs::file_time_type::clock::now();
        const auto max_age = std::chrono::hours(24) * days;
        uintmax_t freed = 0;
        int removed = 0, kept = 0;
        std::vector<fs::path> dirs;
        for (const auto& e : fs::directory_iterator(root, ec))
        {
            if (e.is_directory(ec)) dirs.push_back(e.path());
        }
        for (const auto& dir : dirs)
        {
            bool partial = dir.filename().string().find(".tmp-") != std::string::npos;
            auto used = file_mtime(dir.string() + ".used");
            if (!used) used = file_mtime(dir);
            bool old = used && now - *used > (partial ? std::chrono::hours(24) : max_age);
            if (used_cache_dirs_.contains(dir) || !old)
            {
                ++kept;
                continue;
            }
            freed += directory_size(dir);
            fs::remove_all(dir, ec);
            fs::remove(dir.string() + ".used", ec);
            ++removed;
            c.println(c.dim("  removed " + dir.filename().string()));
        }
        c.println(c.green("✓ ") + "removed " + std::to_string(removed) + " cached source" +
                  (removed == 1 ? "" : "s") + " (" + format_bytes(static_cast<double>(freed)) + "), kept " +
                  std::to_string(kept) + " in " + display_path(root));
    }

  private:
    struct PkgConfigDep
    {
        std::shared_ptr<Target> target;
        std::string package;
        bool static_libs = false;
        bool resolved = false;
    };

    struct CMakeDep
    {
        std::shared_ptr<Target> target;
        std::shared_ptr<Fetched> source;
        CMakeOptions options;
        // resolved by prepare()
        std::vector<CMakeDep*> depends;
        std::string build_type;
        std::vector<std::string> common; // generator and compiler settings
        std::vector<std::string> configure;
        std::string prefix_path;
        fs::path dir, build_dir, install_dir, probe_dir, import_file, log_file;
        enum class State
        {
            Waiting,
            Running,
            Done,
            Failed
        } state = State::Waiting;
        bool prepared = false;
        bool built = false; // false: everything was there from an earlier run
        std::string phase;
        SteadyClock::time_point phase_start;
        double seconds = 0;
        std::string error;
    };

    Targets& targets_;
    bool offline_;
    bool verbose_;
    std::vector<std::shared_ptr<Fetched>> fetched_deps_;
    std::vector<std::shared_ptr<PkgConfigDep>> pkg_config_deps_;
    std::vector<std::shared_ptr<CMakeDep>> cmake_deps_;
    std::set<std::string> fetched_names_;
    std::set<fs::path> used_cache_dirs_;
    std::unique_ptr<Lockfile> lockfile_;

    void resolve_fetched(Fetched& f, bool force_update);
    void try_resolve_cached(Fetched& f);
    bool resolve_pkg_config(PkgConfigDep& dep, const Toolchain& tc);
    bool build_cmake_dep(CMakeDep& d, std::mutex& m, int jobs);
    void apply_import(CMakeDep& d, const ImportedFlags& imported);

    static uintmax_t directory_size(const fs::path& dir)
    {
        uintmax_t size = 0;
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec))
        {
            if (e.is_regular_file(ec)) size += e.file_size(ec);
        }
        return size;
    }

    static std::string format_bytes(double bytes)
    {
        const char* units[] = {"B", "KB", "MB", "GB"};
        int u = 0;
        while (bytes >= 1024 && u < 3)
        {
            bytes /= 1024;
            ++u;
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.1f %s", bytes, units[u]);
        return buf;
    }
};

// ----------------------------------------------------------------------------------
// Build: what the build script talks to
// ----------------------------------------------------------------------------------

// The API of a build script. It declares targets, profiles, options and dependencies;
// run() resolves the command line into commands and carries them out with the parts
// above: Dependencies -> Planner -> Executor -> record_build() -> Reporter.
class Build
{
  public:
    // Flags for every target. Profiles are applied on top.
    Flags flags;
    std::string build_dir = "build";
    // also put the default compiler into the build directory name (build/debug-c++);
    // a compiler chosen with --cxx always is
    bool compiler_in_build_dir = false;
    std::string default_profile = "debug";

    Build(int argc, char** argv)
        : cli_(argc, argv), deps_(targets_, cli_.options().offline, cli_.options().verbose)
    {
        console().configure(cli_.options().color);
    }

    // Registers (or replaces) a profile. Several profiles can be active at once and are
    // merged in command line order: `./nobcpp release asan` or `-p release,asan`.
    void profile(const std::string& name, const Profile& p)
    {
        profiles_.set(name, p);
    }

    // True if `name` was selected on the command line (or is the default profile).
    // Usable while defining targets, before run().
    bool profile_active(const std::string& name) const
    {
        auto active = profiles_.selected(cli_.options(), default_profile);
        return std::find(active.begin(), active.end(), name) != active.end();
    }

    std::shared_ptr<Target> executable(const std::string& name, const fs::path& src_dir = {})
    {
        return add_target(name, Kind::Executable, src_dir);
    }

    std::shared_ptr<Target> executable(const std::string& name, const FetchedPath& src_dir)
    {
        auto target = add_target(name, Kind::Executable, {});
        target->add_sources(src_dir);
        return target;
    }

    std::shared_ptr<Target> static_lib(const std::string& name, const fs::path& src_dir = {})
    {
        return add_target(name, Kind::StaticLib, src_dir);
    }

    std::shared_ptr<Target> static_lib(const std::string& name, const FetchedPath& src_dir)
    {
        auto target = add_target(name, Kind::StaticLib, {});
        target->add_sources(src_dir);
        return target;
    }

    std::shared_ptr<Target> shared_lib(const std::string& name, const fs::path& src_dir = {})
    {
        return add_target(name, Kind::SharedLib, src_dir);
    }

    std::shared_ptr<Target> shared_lib(const std::string& name, const FetchedPath& src_dir)
    {
        auto target = add_target(name, Kind::SharedLib, {});
        target->add_sources(src_dir);
        return target;
    }

    // A header only library, e.g. third party code: linking it only adds its include
    // directory (and any public flags you give it). With `system` its warnings are hidden.
    std::shared_ptr<Target> header_lib(const std::string& name, const fs::path& include_dir = {},
                                       bool system = false)
    {
        auto target = add_target(name, Kind::HeaderOnly, {});
        if (!include_dir.empty())
        {
            target->include_dir(include_dir, true, system);
        }
        return target;
    }

    std::shared_ptr<Target> header_lib(const std::string& name, const FetchedPath& include_dir,
                                       bool system = false)
    {
        auto target = add_target(name, Kind::HeaderOnly, {});
        target->include_dir(include_dir, true, system);
        return target;
    }

    // A target that only runs commands (see Target::add_command), e.g. shaders.
    std::shared_ptr<Target> custom(const std::string& name)
    {
        auto target = add_target(name, Kind::HeaderOnly, {});
        target->origin = "custom commands";
        return target;
    }

    // Downloads sources into a cache shared by all projects (once per commit) and
    // returns where they are. Tags and branches are pinned to a commit in nobcpp.lock,
    // './nobcpp update' moves them. Use the sources directly (header only libraries,
    // or compile them with static_lib()) or build them with cmake().
    std::shared_ptr<Fetched> fetch(const std::string& name, const Source& source)
    {
        return deps_.fetch(name, source);
    }

    // Builds and installs a fetched CMake project into build/_deps (once per build
    // type and set of options), then imports `options.targets` with everything they
    // need: include directories (as system headers), defines and transitive libraries.
    std::shared_ptr<Target> cmake(std::shared_ptr<Fetched> source, const CMakeOptions& options)
    {
        return deps_.cmake(std::move(source), options);
    }

    // Imports a library installed on the system via pkg-config (gcc/clang only).
    std::shared_ptr<Target> pkg_config(const std::string& package, bool static_libs = false)
    {
        return deps_.pkg_config(package, static_libs);
    }

    // An option of the build script: --name=value or --name value. Returns the value
    // from the command line or `default_value`. Shown by 'help'.
    std::string option(const std::string& name, const std::string& default_value, const std::string& help)
    {
        return cli_.option(name, default_value, help);
    }

    // A switch of the build script: --name or --name=true / --no-name or --name=false.
    bool flag(const std::string& name, bool default_value, const std::string& help)
    {
        return cli_.flag(name, default_value, help);
    }

    // Whether the compiler accepts `source` with `flags` (and links it, unless `link` is
    // false), like CMake's check_cxx_source_compiles: e.g. does OpenMP work with this
    // clang. The language standard of b.flags applies. Results are cached in
    // build/probes (keyed by compiler, flags and source), asking again costs nothing.
    bool try_compile(const std::string& source, const std::vector<std::string>& extra_flags = {}, bool link = true);

    // Targets of a project in another directory, see Scope.
    class Scope scope(const fs::path& root);

    // Another compiler for source files with other extensions (CUDA, HIP, ...), see
    // Language. Register it before adding sources, add_sources() then picks them up.
    void add_language(const Language& language)
    {
        if (language.extensions.empty() || language.compiler.empty())
        {
            console().error("add_language(): needs .extensions and .compiler");
            std::exit(1);
        }
        for (const auto& ext : language.extensions)
        {
            source_extensions().insert(ext);
        }
        languages_.push_back(language);
    }

    // Where executables and libraries of the selected profiles end up, e.g. build/debug.
    fs::path output_dir() const
    {
        return fs::path(build_dir) /
               profile_dir_name(profiles_.selected(cli_.options(), default_profile), compiler_for_dir());
    }

    // The compiler selected via --cxx, $CXX or the platform default. Exits if it
    // cannot be found.
    const Toolchain& toolchain()
    {
        if (!toolchain_)
        {
            toolchain_ = detect_toolchain(cli_.options().cxx.empty() ? default_compiler() : cli_.options().cxx);
            if (!toolchain_)
            {
                std::exit(1);
            }
        }
        return *toolchain_;
    }

    // Executes the commands given on the command line. Returns the exit code for main.
    int run();

  private:
    struct Action
    {
        std::string command;
        std::string target{};               // for run, cleancache
        std::vector<std::string> targets{}; // for build, rebuild
        std::vector<std::string> filters{}; // for warnings, update
    };

    // The selected profiles and where their outputs go, fixed when run() starts.
    struct Session
    {
        std::vector<std::string> profiles;
        Flags profile_flags;
        Layout layout;

        std::string label() const
        {
            return join(profiles, "+");
        }
    };

    CommandLine cli_;
    Profiles profiles_;
    Targets targets_;
    Dependencies deps_;
    std::vector<Language> languages_;
    std::optional<Toolchain> toolchain_;

    std::string compiler_for_dir() const
    {
        std::string cxx = cli_.options().cxx;
        return cxx.empty() && compiler_in_build_dir ? default_compiler() : cxx;
    }

    std::shared_ptr<Target> add_target(const std::string& name, Kind kind, const fs::path& src_dir)
    {
        auto target = targets_.add(name, kind);
        if (!src_dir.empty())
        {
            target->add_sources(src_dir);
        }
        return target;
    }

    Session session()
    {
        Session s;
        s.profiles = profiles_.selected(cli_.options(), default_profile);
        s.profile_flags = profiles_.merged(s.profiles);
        s.layout.out_dir = fs::path(build_dir) / profile_dir_name(s.profiles, compiler_for_dir());
        s.layout.toolchain = [this]() -> const Toolchain& { return toolchain(); };
        s.layout.targets = &targets_;
        return s;
    }

    Reporter reporter(const Session& s) const
    {
        return Reporter(cli_.program(), s.label(), cli_.options().verbose);
    }

    bool prepare(const Session& s, const std::vector<std::shared_ptr<Target>>& roots)
    {
        return deps_.prepare(roots, {toolchain(), cli_.options().jobs, fs::path(build_dir) / "_deps",
                                     cmake_build_type(s.profile_flags)});
    }

    JobGraph plan(const Session& s, const Cache& cache, const std::vector<std::shared_ptr<Target>>& roots)
    {
        Planner planner({toolchain(), flags, s.profile_flags, s.layout, targets_, languages_, cache});
        return planner.plan(roots);
    }

    std::vector<std::shared_ptr<Target>> lookup_targets(const std::vector<std::string>& names) const
    {
        std::vector<std::shared_ptr<Target>> roots;
        for (const auto& name : names)
        {
            if (auto t = targets_.find(name))
            {
                roots.push_back(std::move(t));
            }
        }
        return roots;
    }

    std::vector<Action> resolve();
    void print_help() const;

    bool cmd_build(const Session& s, const std::vector<std::shared_ptr<Target>>& roots);
    bool cmd_rebuild(const Session& s, const std::vector<std::shared_ptr<Target>>& roots);
    bool cmd_clean(const Session& s, bool all);
    int cmd_run(const Session& s, const std::string& name);
    bool cmd_compdb(const Session& s);
    void cmd_stats(const Session& s);
    void cmd_warnings(const Session& s, const std::vector<std::string>& filters);
    void cmd_tree(const Session& s);
};

inline std::vector<Build::Action> Build::resolve()
{
    const Options& o = cli_.options();
    cli_.check_unclaimed();
    std::vector<Action> actions;
    for (const std::string& p : o.positionals)
    {
        if (p.empty())
        {
            continue; // the value of a build script option
        }
        if (is_command(p))
        {
            actions.push_back({p});
        }
        else if (profiles_.contains(p))
        {
            continue;
        }
        else if (!actions.empty() && actions.back().command == "run" && actions.back().target.empty() &&
                 targets_.find(p))
        {
            actions.back().target = p;
        }
        else if (!actions.empty() && (actions.back().command == "build" || actions.back().command == "rebuild") &&
                 targets_.find(p))
        {
            actions.back().targets.push_back(p);
        }
        else if (!actions.empty() && actions.back().command == "warnings")
        {
            actions.back().filters.push_back(p);
        }
        else if (!actions.empty() && actions.back().command == "update")
        {
            if (!deps_.is_fetched(p))
            {
                cli_.usage_error("'" + p + "' is not a fetched dependency (fetched: " +
                                 join(deps_.fetched_names(), ", ") + ")");
            }
            actions.back().filters.push_back(p);
        }
        else if (!actions.empty() && actions.back().command == "cleancache" && actions.back().target.empty() &&
                 std::all_of(p.begin(), p.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)); }))
        {
            actions.back().target = p;
        }
        else if (targets_.find(p))
        {
            actions.push_back({"build", "", {p}, {}});
        }
        else
        {
            cli_.usage_error("unknown command, profile or target '" + p + "'");
        }
    }
    for (const auto& p : o.profiles)
    {
        if (!profiles_.contains(p))
        {
            cli_.usage_error("unknown profile '" + p + "' (available: " + join(profiles_.names(), ", ") + ")");
        }
    }
    if (!o.run_args.empty() &&
        std::none_of(actions.begin(), actions.end(), [](const Action& a) { return a.command == "run"; }))
    {
        cli_.usage_error("arguments after '--' are only used by 'run'");
    }
    if (actions.empty())
    {
        actions.push_back({"build"});
    }
    if (!profiles_.contains(default_profile))
    {
        cli_.usage_error("default profile '" + default_profile + "' does not exist");
    }
    return actions;
}

inline void Build::print_help() const
{
    auto& c = console();
    std::string s;
    s += c.bold("usage: ") + cli_.program() + " [commands...] [profiles...] [options] [-- run args]\n\n";
    s += c.bold("commands:\n");
    for (const auto& [name, help] : command_help())
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "  %-10s %s\n", name.c_str(), help.c_str());
        s += buf;
    }
    s += c.bold("\nprofiles:") + " (combine them: '" + cli_.program() + " release asan', default: " +
         default_profile + ")\n  ";
    s += join(profiles_.names(), ", ") + "\n";
    s += c.bold("\noptions:\n");
    s += "  -p, --profile a,b  select profiles (same as naming them)\n";
    s += "  -j, --jobs N       parallel jobs (default: number of cores)\n";
    s += "  -v, --verbose      print every command\n";
    s += "      --fail-fast    stop starting new jobs after the first failure\n";
    s += "      --offline      never download, use fetched dependencies from the cache\n";
    s += "      --cxx CMD      compiler to use (default: $CXX or " + default_compiler() + ")\n";
    s += "      --color WHEN   auto, always or never (also: --no-color, $NO_COLOR)\n";
    if (!cli_.script_options().empty())
    {
        s += c.bold("\nbuild script options:\n");
        for (const auto& o : cli_.script_options())
        {
            std::string left = o.is_flag ? "--" + o.name + ", --no-" + o.name : "--" + o.name + " VALUE";
            char buf[512];
            std::snprintf(buf, sizeof(buf), "  %-22s %s%s\n", left.c_str(), o.help.c_str(),
                          o.is_flag || o.value.empty() ? "" : (" (default: " + o.value + ")").c_str());
            s += buf;
        }
    }
    s += c.bold("\nexamples:\n");
    s += "  " + cli_.program() + "                    debug build\n";
    s += "  " + cli_.program() + " release run -- x   release build, run with argument x\n";
    s += "  " + cli_.program() + " -p release,asan    release build with sanitizers\n";
    s += "  " + cli_.program() + " --cxx clang++ -j8  build with clang using 8 jobs\n";
    c.print(s);
}

inline bool Build::cmd_build(const Session& s, const std::vector<std::shared_ptr<Target>>& roots)
{
    const Options& o = cli_.options();
    if (!prepare(s, roots))
    {
        return false;
    }
    const bool full_build = roots.empty();
    Cache cache;
    cache.load(s.layout.cache_file());
    JobGraph graph = plan(s, cache, roots);
    if (full_build || !fs::exists("compile_commands.json"))
    {
        write_compdb(graph);
    }
    if (o.verbose)
    {
        const Toolchain& tc = toolchain();
        console().println(console().dim("compiler: " + join(tc.cxx, " ") + " (" + family_name(tc.family) + ", " +
                                        tc.version + ")"));
    }
    const Executor::Result result = Executor(graph, {toolchain(), o.jobs, o.verbose, o.fail_fast}).run();
    record_build(cache, graph, result, s.layout, full_build);
    const Reporter report = reporter(s);
    report.diagnostics(graph);
    report.summary(graph, result);
    report.hidden_warnings(graph, cache);
    return result.ok;
}

inline bool Build::cmd_rebuild(const Session& s, const std::vector<std::shared_ptr<Target>>& roots)
{
    if (roots.empty())
    {
        return cmd_clean(s, false) && cmd_build(s, {});
    }
    // only the named targets: delete their outputs and forget that they are current
    if (!prepare(s, roots))
    {
        return false;
    }
    Cache cache;
    cache.load(s.layout.cache_file());
    JobGraph graph = plan(s, cache, roots);
    std::error_code ec;
    for (const auto& j : graph.jobs)
    {
        fs::remove(j.output, ec);
        for (const auto& eo : j.extra_outputs) fs::remove(eo, ec);
        if (auto it = cache.entries.find(j.output); it != cache.entries.end())
        {
            it->second.hash = 0;
            it->second.diagnostics.clear();
        }
    }
    cache.save(s.layout.cache_file());
    return cmd_build(s, roots);
}

inline bool Build::cmd_clean(const Session& s, bool all)
{
    fs::path dir = all ? fs::path(build_dir) : s.layout.out_dir;
    // 'clean' and 'rebuild' keep the timing history: exactly then every time is known
    Cache timings;
    if (!all)
    {
        timings.load(s.layout.cache_file());
        for (auto& [output, e] : timings.entries)
        {
            e.hash = 0; // outdated, only the duration is still useful
            e.diagnostics.clear();
        }
    }
    std::error_code ec;
    auto removed = fs::remove_all(dir, ec);
    if (!ec && !timings.entries.empty())
    {
        timings.save(s.layout.cache_file());
    }
    if (ec)
    {
        console().error("could not remove '" + display_path(dir) + "': " + ec.message());
        return false;
    }
    console().println(console().green("✓ ") + "removed " + display_path(dir) + " (" + std::to_string(removed) +
                      " files)");
    return true;
}

inline int Build::cmd_run(const Session& s, const std::string& name)
{
    std::shared_ptr<Target> target;
    if (!name.empty())
    {
        target = targets_.find(name);
    }
    else
    {
        std::vector<std::shared_ptr<Target>> exes;
        for (const auto& t : targets_.all())
            if (t->kind() == Kind::Executable) exes.push_back(t);
        if (exes.size() != 1)
        {
            std::vector<std::string> names;
            for (const auto& t : exes) names.push_back(t->name());
            console().error(exes.empty() ? "no executable target to run"
                                         : "several executables, choose one: run " + join(names, "|"));
            return 2;
        }
        target = exes[0];
    }
    if (!target || target->kind() != Kind::Executable)
    {
        console().error("'" + name + "' is not an executable target");
        return 2;
    }
    if (!cmd_build(s, {target}))
    {
        return 1;
    }
    const std::vector<std::string>& run_args = cli_.options().run_args;
    std::vector<std::string> args = {fs::absolute(s.layout.output(*target)).string()};
    append(args, run_args);
    console().println(console().dim("› " + display_path(s.layout.output(*target)) +
                                    (run_args.empty() ? "" : " " + join(run_args, " "))));
    std::fflush(stdout);
    return run_interactive(args);
}

inline bool Build::cmd_compdb(const Session& s)
{
    if (!prepare(s, {}))
    {
        return false;
    }
    Cache cache;
    cache.load(s.layout.cache_file());
    write_compdb(plan(s, cache, {}));
    console().println(console().green("✓ ") + "wrote compile_commands.json");
    return true;
}

inline void Build::cmd_stats(const Session& s)
{
    deps_.resolve_cached_only();
    Cache cache;
    cache.load(s.layout.cache_file());
    // the dependency graph, with the recorded durations as estimates
    reporter(s).stats(plan(s, cache, {}), cache, cli_.options().jobs);
}

inline void Build::cmd_warnings(const Session& s, const std::vector<std::string>& filters)
{
    Cache cache;
    cache.load(s.layout.cache_file());
    reporter(s).stored_warnings(cache, filters);
}

inline void Build::cmd_tree(const Session& session)
{
    auto& c = console();
    std::string s;
    std::set<const Target*> active;
    std::set<const Target*> printed;
    std::function<void(const Target&, int)> print = [&](const Target& t, int depth) {
        const char* kinds[] = {"executable", "static lib", "shared lib",
                               t.origin.empty() ? "header only" : "imported"};
        std::string indent(depth * 2, ' ');
        if (!active.insert(&t).second)
        {
            s += indent + c.red(t.name() + " (cycle)") + "\n";
            return;
        }
        printed.insert(&t);
        std::string out = t.kind() == Kind::HeaderOnly
                              ? join(t.public_flags.system_include_dirs, ", ")
                              : display_path(session.layout.output(t));
        if (!t.origin.empty())
        {
            out = t.origin;
        }
        if (t.kind() == Kind::HeaderOnly && !t.public_flags.include_dirs.empty())
        {
            out += (out.empty() ? "" : ", ") + join(t.public_flags.include_dirs, ", ");
        }
        s += indent + c.bold(t.name()) +
             c.dim(" (" + std::string(kinds[(int)t.kind()]) + " → " + out + ")") + "\n";
        for (const auto& src : t.sources())
        {
            s += indent + "  " + display_path(src) + "\n";
        }
        for (const auto& dp : t.deferred_paths())
        {
            if (dp.kind == Target::DeferredPath::Kind::Source ||
                dp.kind == Target::DeferredPath::Kind::SourcesDir)
            {
                std::string sub = dp.path.subpath().empty() ? "" : "/" + display_path(dp.path.subpath());
                s += indent + "  " + c.dim(dp.path.fetched()->name() + sub) + "\n";
            }
        }
        for (const auto& dep : t.deps())
        {
            print(*dep, depth + 1);
        }
        active.erase(&t);
    };
    std::set<const Target*> used;
    for (const auto& t : targets_.all())
        for (const auto& d : t->deps()) used.insert(d.get());
    for (const auto& t : targets_.all())
        if (!used.contains(t.get())) print(*t, 0);
    for (const auto& t : targets_.all())
        if (!printed.contains(t.get())) print(*t, 0);
    c.print(s);
}

inline bool Build::try_compile(const std::string& source, const std::vector<std::string>& extra_flags, bool link)
{
    const Toolchain& tc = toolchain();
    std::vector<std::string> key = tc.cxx;
    append(key, {tc.version, link ? "link" : "compile", flags.std ? std::to_string(*flags.std) : "", source});
    append(key, extra_flags);
    char id[32];
    std::snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(hash_args(key)));
    fs::path dir = fs::path(build_dir) / "probes";
    fs::path result_file = dir / (std::string(id) + ".result");
    if (auto cached = read_file(result_file); cached && !cached->empty())
    {
        return (*cached)[0] == '1';
    }

    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path src = dir / (std::string(id) + ".cpp");
    fs::path out = dir / (std::string(id) + (link ? (target_windows ? ".exe" : ".bin") : tc.obj_ext()));
    std::ofstream(src, std::ios::trunc) << source;

    std::vector<std::string> cmd = tc.cxx;
    if (tc.msvc_like())
    {
        append(cmd, {"/nologo", "/EHsc"});
        if (flags.std) cmd.push_back(*flags.std > 20 ? "/std:c++latest" : "/std:c++" + std::to_string(*flags.std));
        append(cmd, extra_flags);
        append(cmd, link ? std::vector<std::string>{src.string(), "/Fe" + out.string(), "/Fo" + (dir / id).string() + ".obj"}
                         : std::vector<std::string>{"/c", src.string(), "/Fo" + out.string()});
    }
    else
    {
        if (flags.std) cmd.push_back("-std=c++" + std::to_string(*flags.std));
        append(cmd, extra_flags);
        if (!link) cmd.push_back("-c");
        append(cmd, {src.string(), "-o", out.string()});
    }
    ProcessResult r = run_capture(cmd);
    bool ok = r.started && r.exit_code == 0;
    std::ofstream(dir / (std::string(id) + ".log"), std::ios::trunc) << "$ " << join(cmd, " ") << "\n" << r.output;
    std::ofstream(result_file, std::ios::trunc) << (ok ? "1" : "0") << "\n";
    fs::remove(out, ec);
    if (cli_.options().verbose)
    {
        console().println(console().dim("probe " + std::string(id) + ": " + (ok ? "works" : "fails") +
                                         " (" + display_path(dir / (std::string(id) + ".log")) + ")"));
    }
    return ok;
}

// Targets of a project that lives in another directory, e.g. a library used by several
// projects together with its own build logic. Relative paths given to its targets
// (sources, include directories, objects, copies) are relative to `root()`; command
// arguments are passed unchanged, use path() for them. The other project offers a
// function in a header, for example astra_build.hpp:
//
//     inline std::shared_ptr<nob::Target> add_astra(nob::Scope s)
//     {
//         auto lib = s.static_lib("astra", "src");
//         if (s.build().flag("openmp", true, "use OpenMP")) lib->flags.compile_flags.push_back("-fopenmp");
//         return lib;
//     }
//
// and calls add_astra(b.scope(".")) in its own nobcpp.cpp, while another project calls
// add_astra(b.scope("../ASTra")) (and lists "../ASTra/astra_build.hpp" in the deps of
// rebuild_self). Target names are shared by all scopes.
class Scope
{
  public:
    Scope(Build& build, fs::path root) : build_(build), root_(std::move(root).lexically_normal())
    {
    }

    Build& build() const
    {
        return build_;
    }

    const fs::path& root() const
    {
        return root_;
    }

    // A path inside the project, for command arguments, inputs and outputs.
    fs::path path(const fs::path& p) const
    {
        return p.is_absolute() ? p : (root_ / p).lexically_normal();
    }

    std::shared_ptr<Target> executable(const std::string& name, const fs::path& src_dir = {}) const
    {
        return adopt(build_.executable(name), src_dir);
    }

    std::shared_ptr<Target> static_lib(const std::string& name, const fs::path& src_dir = {}) const
    {
        return adopt(build_.static_lib(name), src_dir);
    }

    std::shared_ptr<Target> shared_lib(const std::string& name, const fs::path& src_dir = {}) const
    {
        return adopt(build_.shared_lib(name), src_dir);
    }

    std::shared_ptr<Target> header_lib(const std::string& name, const fs::path& include_dir = {},
                                       bool system = false) const
    {
        auto t = adopt(build_.header_lib(name), {});
        if (!include_dir.empty())
        {
            t->include_dir(include_dir, true, system);
        }
        return t;
    }

    std::shared_ptr<Target> custom(const std::string& name) const
    {
        return adopt(build_.custom(name), {});
    }

  private:
    std::shared_ptr<Target> adopt(std::shared_ptr<Target> t, const fs::path& src_dir) const
    {
        t->base = root_;
        if (!src_dir.empty())
        {
            t->add_sources(src_dir);
        }
        return t;
    }

    Build& build_;
    fs::path root_;
};

inline Scope Build::scope(const fs::path& root)
{
    return Scope(*this, root);
}

inline int Build::run()
{
    const std::vector<Action> actions = resolve();
    const Session s = session();
    for (const auto& action : actions)
    {
        const std::string& cmd = action.command;
        bool ok = true;
        if (cmd == "help")
            print_help();
        else if (cmd == "build")
            ok = cmd_build(s, lookup_targets(action.targets));
        else if (cmd == "rebuild")
            ok = cmd_rebuild(s, lookup_targets(action.targets));
        else if (cmd == "clean")
            ok = cmd_clean(s, false);
        else if (cmd == "cleanall")
            ok = cmd_clean(s, true);
        else if (cmd == "compdb")
            ok = cmd_compdb(s);
        else if (cmd == "stats")
            cmd_stats(s);
        else if (cmd == "tree")
            cmd_tree(s);
        else if (cmd == "warnings")
            cmd_warnings(s, action.filters);
        else if (cmd == "update")
            deps_.update(action.filters);
        else if (cmd == "cleancache")
            deps_.cleancache(action.target.empty() ? 30 : std::stoi(action.target));
        else if (cmd == "run")
            return cmd_run(s, action.target);
        if (!ok)
        {
            return 1;
        }
    }
    return 0;
}

inline std::shared_ptr<Fetched> Dependencies::fetch(const std::string& name, const Source& source)
{
    if (source.git.empty() == source.url.empty())
    {
        console().error("fetch(\"" + name + "\"): give either .git or .url");
        std::exit(1);
    }
    auto dep = std::make_shared<Fetched>(name, source, [this](Fetched& f) { resolve_fetched(f, false); });
    fetched_deps_.push_back(dep);
    fetched_names_.insert(name);
    return dep;
}

inline void Dependencies::resolve_fetched(Fetched& f, bool force_update)
{
    if (f.resolved() && !force_update)
    {
        return;
    }
    auto& c = console();
    const std::string& name = f.name();
    const Source& source = f.source();
    set_env("GIT_TERMINAL_PROMPT", "0"); // fail instead of hanging on a password prompt
    std::string version;
    std::string label;
    if (!source.url.empty())
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%016llx",
                      static_cast<unsigned long long>(hash_args({source.url})));
        version = buf;
        label = source.url;
    }
    else
    {
        std::string commit = source.commit;
        std::string ref = source.ref.empty() ? "HEAD" : source.ref;
        if (commit.empty())
        {
            if (!lockfile_)
            {
                lockfile_ = std::make_unique<Lockfile>("nobcpp.lock");
            }
            auto it = lockfile_->entries.find(name);
            bool locked = it != lockfile_->entries.end() && it->second.url == source.git &&
                          it->second.ref == ref;
            if (locked && !force_update)
            {
                commit = it->second.commit;
            }
            else
            {
                if (offline_)
                {
                    c.error("'" + name + "' is not in nobcpp.lock yet, '" + ref +
                            "' can't be looked up with --offline");
                    std::exit(1);
                }
                auto resolved = resolve_git_ref(source.git, ref);
                if (!resolved)
                {
                    std::exit(1);
                }
                commit = *resolved;
                if (!locked || it->second.commit != commit)
                {
                    std::string was = locked ? " (was " + it->second.commit.substr(0, 12) + ")" : "";
                    c.println(c.cyan("nobcpp.lock: ") + name + " " + ref + " → " +
                              commit.substr(0, 12) + c.dim(was));
                    lockfile_->entries[name] = {source.git, ref, commit};
                    lockfile_->save();
                }
            }
        }
        version = commit;
        label = (source.ref.empty() ? "" : source.ref + " ") + commit.substr(0, 12);
    }

    fs::path dir = source_cache_dir() /
                   (name + "-" + version.substr(0, 12) + (source.submodules ? "-sm" : ""));
    if (!fs::exists(dir))
    {
        if (offline_)
        {
            c.error("'" + name + "' (" + label + ") is not downloaded yet and --offline is set");
            std::exit(1);
        }
        c.println(c.cyan("fetching ") + name + " " + c.dim(label));
        auto start = SteadyClock::now();
        bool ok = source.url.empty() ? fetch_git(source.git, version, source.submodules, dir)
                                     : fetch_archive(source.url, dir);
        if (!ok)
        {
            c.error("fetching '" + name + "' failed");
            std::exit(1);
        }
        c.println(c.green("✓ ") + "fetched " + name +
                  c.dim(" in " + format_duration(seconds_since(start)) + " → " +
                        display_path(dir)));
    }
    // remember the use, 'cleancache' only deletes sources no project used for a while
    used_cache_dirs_.insert(dir);
    std::ofstream(dir.string() + ".used", std::ios::trunc) << fs::current_path().generic_string() << "\n";
    f.set_resolved(std::move(version), std::move(dir));
}

// Resolves a fetched dependency from the lockfile and local cache only, without network.
inline void Dependencies::try_resolve_cached(Fetched& f)
{
    if (f.resolved())
    {
        used_cache_dirs_.insert(f.resolved_dir());
        return;
    }
    const std::string& name = f.name();
    const Source& source = f.source();
    std::string version;
    if (!source.url.empty())
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%016llx",
                      static_cast<unsigned long long>(hash_args({source.url})));
        version = buf;
    }
    else if (!source.commit.empty())
    {
        version = source.commit;
    }
    else
    {
        if (!lockfile_)
        {
            lockfile_ = std::make_unique<Lockfile>("nobcpp.lock");
        }
        std::string ref = source.ref.empty() ? "HEAD" : source.ref;
        auto it = lockfile_->entries.find(name);
        if (it == lockfile_->entries.end() || it->second.url != source.git || it->second.ref != ref)
        {
            return;
        }
        version = it->second.commit;
    }
    fs::path dir = source_cache_dir() /
                   (name + "-" + version.substr(0, 12) + (source.submodules ? "-sm" : ""));
    used_cache_dirs_.insert(dir);
    if (fs::exists(dir))
    {
        f.set_resolved(std::move(version), std::move(dir));
    }
}

inline std::shared_ptr<Target> Dependencies::cmake(std::shared_ptr<Fetched> source,
                                            const CMakeOptions& options)
{
    if (options.targets.empty())
    {
        console().error("cmake(" + source->name() +
                        "): name the CMake targets to link, e.g. .targets = {\"SDL3::SDL3\"}");
        std::exit(1);
    }
    if (options.package.empty() && options.targets[0].find("::") == std::string::npos)
    {
        console().error("cmake(" + source->name() + "): can't guess the find_package() name from '" +
                        options.targets[0] + "', set .package (e.g. .package = \"SFML\")");
        std::exit(1);
    }
    auto target = targets_.add(source->name(), Kind::HeaderOnly);
    const Source& s = source->source();
    std::string label = !s.ref.empty()    ? s.ref
                        : !s.commit.empty() ? s.commit.substr(0, 12)
                                            : s.url;
    target->origin = "cmake" + (label.empty() ? "" : ", " + label);
    auto dep = std::make_shared<CMakeDep>();
    dep->target = target;
    dep->source = std::move(source);
    dep->options = options;
    cmake_deps_.push_back(dep);
    return target;
}

inline std::shared_ptr<Target> Dependencies::pkg_config(const std::string& package, bool static_libs)
{
    auto target = targets_.add(package, Kind::HeaderOnly);
    target->origin = "pkg-config";
    auto dep = std::make_shared<PkgConfigDep>();
    dep->target = target;
    dep->package = package;
    dep->static_libs = static_libs;
    pkg_config_deps_.push_back(std::move(dep));
    return target;
}

inline bool Dependencies::resolve_pkg_config(PkgConfigDep& dep, const Toolchain& tc)
{
    if (dep.resolved)
    {
        return true;
    }
    if (tc.msvc_like())
    {
        console().error("pkg_config(\"" + dep.package +
                        "\") needs a gcc or clang style compiler, use cmake() with msvc");
        return false;
    }
    const char* env = std::getenv("PKG_CONFIG");
    std::string exe = env && *env ? env : "pkg-config";
    // --cflags and --libs separately: what a flag is for depends on where it came from
    auto query = [&](const char* what) -> std::optional<std::vector<std::string>> {
        std::vector<std::string> cmd = {exe, what};
        if (dep.static_libs) cmd.push_back("--static");
        cmd.push_back(dep.package);
        ProcessResult r = run_or_explain(cmd, "pkg-config " + dep.package);
        if (r.exit_code != 0) return std::nullopt;
        return split_args(r.output);
    };
    auto cflags = query("--cflags");
    auto libs = cflags ? query("--libs") : std::nullopt;
    if (!cflags || !libs)
    {
        return false;
    }
    std::string version = run_capture({exe, "--modversion", dep.package}).output;
    version = version.substr(0, version.find_first_of("\r\n"));

    dep.target->origin = "pkg-config" + (version.empty() ? "" : ", " + version);
    Flags& f = dep.target->public_flags;
    const std::vector<std::string>& args = *cflags;
    for (size_t i = 0; i < args.size(); ++i)
    {
        const std::string& a = args[i];
        // "-Ipath" or "-I path"
        auto value = [&](size_t prefix) {
            return a.size() > prefix ? a.substr(prefix)
                                     : (i + 1 < args.size() ? args[++i] : std::string());
        };
        if (a.starts_with("-I"))
            f.system_include_dirs.push_back(value(2)); // third party: no warnings
        else if (a == "-isystem")
            f.system_include_dirs.push_back(value(8));
        else if (a.starts_with("-D"))
            f.defines.push_back(value(2));
        else
            f.compile_flags.push_back(a); // -fopenmp, -pthread: the link gets them too
    }
    const std::vector<std::string>& lib_args = *libs;
    for (size_t i = 0; i < lib_args.size(); ++i)
    {
        const std::string& a = lib_args[i];
        if ((a == "-L" || a == "-l") && i + 1 < lib_args.size())
        {
            f.link_flags.push_back(a + lib_args[++i]); // "-l m" -> "-lm"
        }
        else if (a == "-framework" && i + 1 < lib_args.size())
        {
            append(f.link_flags, {a, lib_args[i + 1]});
            ++i;
        }
        else
        {
            f.link_flags.push_back(a); // -l, -L, -Wl, files and link only flags
        }
    }
    dep.resolved = true;
    return true;
}

inline std::set<const Target*> Dependencies::needed(
    const std::vector<std::shared_ptr<Target>>& roots) const
{
    std::set<const Target*> reached;
    if (roots.empty())
    {
        for (const auto& t : targets_.all()) reached.insert(t.get());
        return reached;
    }
    std::unordered_map<const Target*, const CMakeDep*> cmake_by_target;
    for (const auto& d : cmake_deps_) cmake_by_target[d->target.get()] = d.get();
    std::function<void(const Target&)> visit = [&](const Target& t) {
        if (!reached.insert(&t).second) return;
        for (const auto& dep : t.deps()) visit(*dep);
        if (auto it = cmake_by_target.find(&t); it != cmake_by_target.end())
        {
            for (const auto& dep : it->second->options.depends) visit(*dep);
        }
    };
    for (const auto& r : roots) visit(*r);
    return reached;
}

// Configures, builds and installs a CMake dependency (unless an earlier run did), then
// asks CMake which flags its targets need. Runs on its own thread.
inline bool Dependencies::build_cmake_dep(CMakeDep& d, std::mutex& m, int jobs)
{
    if (auto imported = ImportedFlags::load(d.import_file))
    {
        apply_import(d, *imported);
        return true;
    }
    auto& c = console();
    {
        std::lock_guard<std::mutex> lock(m);
        d.built = true;
    }
    std::error_code ec;
    fs::create_directories(d.dir, ec);
    std::ofstream log(d.log_file, std::ios::app);
    auto step = [&](const std::string& phase, const std::vector<std::string>& cmd) {
        {
            std::lock_guard<std::mutex> lock(m);
            d.phase = phase;
            d.phase_start = SteadyClock::now();
        }
        if (!c.tty() || verbose_)
        {
            c.println(c.dim("  " + d.target->name() + ": " + phase));
        }
        log << "$ " << join(cmd, " ") << "\n";
        ProcessResult r = run_capture(cmd);
        log << r.output << "\n";
        log.flush();
        if (r.exit_code != 0)
        {
            d.error = "cmake " + phase + " failed\n" + c.dim("$ " + join(cmd, " ")) + "\n" +
                      tail_lines(r.output, 25) + c.dim("full log: " + display_path(d.log_file));
            return false;
        }
        return true;
    };

    fs::path installed = d.dir / "installed";
    if (!fs::exists(installed))
    {
        std::string jobs_str = std::to_string(std::max(1, jobs));
        if (!step("configure", d.configure) ||
            !step("build", {"cmake", "--build", d.build_dir.string(), "--config", d.build_type,
                            "--parallel", jobs_str}) ||
            !step("install", {"cmake", "--install", d.build_dir.string(), "--config", d.build_type}))
        {
            return false;
        }
        std::ofstream(installed) << "ok\n";
    }

    // A tiny project that links the requested targets: CMake resolves everything they
    // need (includes, defines, transitive libraries) and reports it via its file API.
    const CMakeOptions& o = d.options;
    std::string package = o.package;
    if (package.empty())
    {
        package = o.targets[0].substr(0, o.targets[0].find("::"));
    }
    fs::remove_all(d.probe_dir, ec);
    fs::path probe_build = d.probe_dir / "build";
    fs::create_directories(probe_build / ".cmake" / "api" / "v1" / "query", ec);
    std::ofstream(probe_build / ".cmake" / "api" / "v1" / "query" / "codemodel-v2");
    std::ofstream(d.probe_dir / "probe.cpp") << "int main() { return 0; }\n";
    {
        std::ofstream cm(d.probe_dir / "CMakeLists.txt");
        cm << "cmake_minimum_required(VERSION 3.16)\nproject(nobcpp_probe C CXX)\n";
        for (const auto& v : o.probe_variables)
        {
            size_t eq = v.find('=');
            cm << "set(" << v.substr(0, eq) << " \"" << (eq == std::string::npos ? "" : v.substr(eq + 1))
               << "\")\n";
        }
        cm << "find_package(" << package << " REQUIRED";
        if (!o.components.empty()) cm << " COMPONENTS " << join(o.components, " ");
        cm << ")\nadd_executable(nobcpp_probe probe.cpp)\n";
        cm << "target_link_libraries(nobcpp_probe PRIVATE " << join(o.targets, " ") << ")\n";
    }
    std::vector<std::string> probe = {"cmake", "-S", d.probe_dir.string(), "-B", probe_build.string(),
                                      "-DCMAKE_BUILD_TYPE=" + d.build_type,
                                      "-DCMAKE_PREFIX_PATH=" + d.prefix_path};
    append(probe, d.common);
    // without this the probe would report the default flags of its build type
    for (const char* var : {"CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS", "CMAKE_EXE_LINKER_FLAGS"})
    {
        for (const char* config : {"", "_DEBUG", "_RELEASE", "_RELWITHDEBINFO", "_MINSIZEREL"})
        {
            probe.push_back("-D" + std::string(var) + config + "=");
        }
    }
    if (!step("import", probe))
    {
        return false;
    }
    std::string error;
    auto imported = ImportedFlags::from_file_api(probe_build, error);
    if (!imported)
    {
        d.error = "reading what CMake reported failed: " + error;
        return false;
    }
    imported->save(d.import_file);
    apply_import(d, *imported);
    return true;
}

inline void Dependencies::apply_import(CMakeDep& d, const ImportedFlags& imported)
{
    Flags& f = d.target->public_flags;
    append(f.system_include_dirs, imported.includes); // third party: no warnings
    append(f.defines, imported.defines);
    append(f.compile_flags, imported.compile);
    append(f.link_flags, imported.link);
}

// Resolves needed fetched sources, pkg-config packages, and cmake() dependencies
// (independent CMake builds run in parallel).
inline bool Dependencies::prepare(const std::vector<std::shared_ptr<Target>>& roots, const PrepareSettings& settings)
{
    const std::set<const Target*> needed_targets = needed(roots);

    // 1. Resolve fetched sources referenced by needed targets (deferred paths and cmake deps).
    for (const auto& t : targets_.all())
    {
        if (!needed_targets.contains(t.get())) continue;
        for (const auto& dp : t->deferred_paths())
        {
            resolve_fetched(*dp.path.fetched(), false);
        }
        t->resolve_deferred_paths();
    }
    std::vector<CMakeDep*> active_cmake;
    for (auto& d : cmake_deps_)
    {
        if (!needed_targets.contains(d->target.get()) || d->prepared) continue;
        resolve_fetched(*d->source, false);
        const Source& s = d->source->source();
        d->target->origin = "cmake, " + (s.ref.empty() ? "" : s.ref + " ") +
                            (s.url.empty() ? d->source->version().substr(0, 12) : s.url);
        active_cmake.push_back(d.get());
    }

    // 2. Resolve needed pkg-config packages.
    for (auto& dep : pkg_config_deps_)
    {
        if (!needed_targets.contains(dep->target.get()) || dep->resolved) continue;
        if (!resolve_pkg_config(*dep, settings.toolchain))
        {
            return false;
        }
    }

    if (active_cmake.empty())
    {
        return true;
    }

    auto& c = console();
    const Toolchain& tc = settings.toolchain;

    std::vector<std::string> common;
    if (run_capture({"ninja", "--version"}).exit_code == 0)
    {
        append(common, {"-G", "Ninja"});
    }
    const std::string& cxx = tc.cxx.back();
    common.push_back("-DCMAKE_CXX_COMPILER=" + cxx);
    common.push_back("-DCMAKE_C_COMPILER=" + c_compiler_for(cxx));
    if (tc.cxx.size() > 1)
    {
        std::string launcher = join(std::vector<std::string>(tc.cxx.begin(), tc.cxx.end() - 1), ";");
        common.push_back("-DCMAKE_CXX_COMPILER_LAUNCHER=" + launcher);
        common.push_back("-DCMAKE_C_COMPILER_LAUNCHER=" + launcher);
    }
    if (tc.msvc_like())
    {
        // the static runtime (/MT), what cl uses by default, mixing them fails to link
        append(common, {"-DCMAKE_POLICY_DEFAULT_CMP0091=NEW",
                        "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"});
    }

    auto hex = [](uint64_t h) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
        return std::string(buf).substr(0, 10);
    };
    fs::path root = fs::absolute(settings.deps_dir);
    for (CMakeDep* d : active_cmake)
    {
        const CMakeOptions& o = d->options;
        d->build_type = o.build_type.empty() ? settings.build_type : o.build_type;
        d->common = common;
        d->depends.clear();
        std::vector<std::string> prefix;
        for (const auto& dep_target : o.depends)
        {
            auto it = std::find_if(cmake_deps_.begin(), cmake_deps_.end(),
                                   [&](const auto& x) { return x->target == dep_target; });
            if (it == cmake_deps_.end() || it->get() == d || (*it)->install_dir.empty())
            {
                c.error("cmake(" + d->target->name() + "): .depends must name cmake() targets "
                        "declared before it");
                return false;
            }
            d->depends.push_back(it->get());
            prefix.push_back((*it)->install_dir.generic_string());
        }
        // everything that influences the build decides its directory, so changing an
        // option or the profile never reuses a wrong build
        std::vector<std::string> key = {d->source->version(), o.source_subdir, d->build_type};
        append(key, common);
        append(key, prefix);
        append(key, o.options);
        uint64_t build_hash = hash_args(key);
        d->dir = root / (d->target->name() + "-" + str_lower(d->build_type) + "-" + hex(build_hash));
        d->build_dir = d->dir / "build";
        d->install_dir = d->dir / "install";
        d->log_file = d->dir / "nobcpp.log";
        prefix.insert(prefix.begin(), d->install_dir.generic_string());
        d->prefix_path = join(prefix, ";");

        std::vector<std::string> probe_key = {std::to_string(build_hash), o.package};
        append(probe_key, o.targets);
        append(probe_key, o.components);
        append(probe_key, o.probe_variables);
        std::string probe_id = hex(hash_args(probe_key));
        d->probe_dir = d->dir / ("probe-" + probe_id);
        d->import_file = d->dir / ("import-" + probe_id + ".txt");

        d->configure = {"cmake", "-S", (d->source->resolved_dir() / o.source_subdir).string(),
                        "-B", d->build_dir.string(),
                        "-DCMAKE_BUILD_TYPE=" + d->build_type,
                        "-DCMAKE_INSTALL_PREFIX=" + d->install_dir.string(),
                        "-DCMAKE_PREFIX_PATH=" + d->prefix_path,
                        "-DBUILD_SHARED_LIBS=OFF",
                        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"};
        append(d->configure, common);
        for (const auto& opt : o.options)
        {
            d->configure.push_back("-D" + opt); // after the defaults, so they can be overridden
        }
    }

    int total_jobs = settings.jobs > 0 ? settings.jobs
                                       : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    int to_build = 0;
    for (CMakeDep* d : active_cmake)
    {
        if (!fs::exists(d->import_file)) ++to_build;
    }
    int per_dep_jobs = std::max(1, total_jobs / std::max(1, to_build));

    std::mutex m;
    std::condition_variable cv;
    auto run = [&](CMakeDep& d) {
        {
            std::unique_lock<std::mutex> lock(m);
            cv.wait(lock, [&] {
                return std::all_of(d.depends.begin(), d.depends.end(), [](const CMakeDep* x) {
                    return x->state == CMakeDep::State::Done || x->state == CMakeDep::State::Failed;
                });
            });
            if (std::any_of(d.depends.begin(), d.depends.end(),
                            [](const CMakeDep* x) { return x->state == CMakeDep::State::Failed; }))
            {
                d.error = "not built because a dependency it needs failed";
                d.state = CMakeDep::State::Failed;
                cv.notify_all();
                return;
            }
            d.state = CMakeDep::State::Running;
        }
        auto start = SteadyClock::now();
        bool ok = build_cmake_dep(d, m, per_dep_jobs);
        std::lock_guard<std::mutex> lock(m);
        d.seconds = seconds_since(start);
        d.state = ok ? CMakeDep::State::Done : CMakeDep::State::Failed;
        d.prepared = ok;
        cv.notify_all();
    };

    auto start = SteadyClock::now();
    std::vector<std::thread> threads;
    for (CMakeDep* d : active_cmake)
    {
        threads.emplace_back(run, std::ref(*d));
    }
    {
        std::unique_lock<std::mutex> lock(m);
        auto finished = [&] {
            return std::all_of(active_cmake.begin(), active_cmake.end(), [](const CMakeDep* d) {
                return d->state == CMakeDep::State::Done || d->state == CMakeDep::State::Failed;
            });
        };
        while (!finished())
        {
            if (c.tty())
            {
                std::vector<std::string> parts;
                for (const CMakeDep* d : active_cmake)
                {
                    if (d->state == CMakeDep::State::Running && d->built)
                    {
                        parts.push_back(c.cyan(d->target->name()) + " " + d->phase + " " +
                                        c.dim(format_duration(seconds_since(d->phase_start))));
                    }
                }
                if (!parts.empty())
                {
                    c.set_status(c.dim("dependencies (" + format_duration(seconds_since(start)) +
                                       ")  ") + join(parts, c.dim(" · ")));
                }
            }
            cv.wait_for(lock, std::chrono::milliseconds(150));
        }
    }
    for (auto& t : threads)
    {
        t.join();
    }
    c.clear_status();

    bool ok = true;
    for (const CMakeDep* d : active_cmake)
    {
        if (d->state == CMakeDep::State::Failed)
        {
            ok = false;
            c.print("\n" + c.red("── dependency " + d->target->name() + " ") + "\n" + d->error + "\n");
        }
        else if (d->built)
        {
            c.println(c.green("✓ ") + d->target->name() +
                      c.dim(" (" + d->target->origin + ", " + d->build_type + ") ready in " +
                            format_duration(d->seconds)));
        }
    }
    if (!ok)
    {
        c.println("\n" + c.red("✗ ") + "a dependency failed to build");
    }
    return ok;
}

} // namespace nob
