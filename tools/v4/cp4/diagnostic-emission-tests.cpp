#include <components/debug/gameplaydiagnostics.hpp>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace
{
    void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
    void environment(const char* key, const char* value)
    {
#ifdef _WIN32
        check(_putenv_s(key, value) == 0, "environment setup failed");
#else
        check(setenv(key, value, 1) == 0, "environment setup failed");
#endif
    }
}
int main(int argc, char** argv)
{
    try
    {
        using namespace Debug::GameplayDiagnostics;
        const bool active = argc == 2 && std::string_view(argv[1]) == "on";
        const auto path = std::filesystem::temp_directory_path()
            / ("openmw-diagnostic-test-" + std::to_string(Clock::now().time_since_epoch().count()) + ".jsonl");
        check(!std::filesystem::exists(path), "test path collision");
        environment("OPENMW_GAMEPLAY_DIAGNOSTICS", active ? "1" : "0");
        environment("OPENMW_GAMEPLAY_DIAGNOSTICS_FILE", path.string().c_str());
        context.sample = false;
        {
            Operation outer("transition", "exterior");
            try
            {
                Operation inner("load_cell", "quoted \"cell\"\nname");
                emit("failure", {{"message", "attachment failed"}}, true);
                throw std::runtime_error("fixture");
            }
            catch (const std::runtime_error&) {}
        }
        check(!sampling(), "loading diagnostics altered frame sampling");
        if (!active)
            check(!std::filesystem::exists(path), "disabled diagnostics wrote a file");
        else
        {
            std::ifstream input(path);
            const std::string rows((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            check(std::count(rows.begin(), rows.end(), '\n') == 5, "missing unsampled loading/failure events");
            check(rows.find("\"unwinding\":\"1\"") != std::string::npos, "lost unwinding record");
            check(rows.find("\"unwinding\":\"0\"") != std::string::npos, "lost normal completion record");
            check(rows.find("\\\"cell\\\"\\u000aname") != std::string::npos, "invalid JSON escaping");
            check(rows.find("\"type\":\"failure\"") != std::string::npos, "missing failure record");
            // The process owns this uniquely generated diagnostic fixture. The
            // sink remains open until exit, so leave the tiny file on Windows.
            std::cout << "Fixture evidence: " << path << '\n';
        }
        std::cout << "Diagnostic emission " << (active ? "on" : "off") << ": PASS\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
