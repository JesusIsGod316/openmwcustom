#include <components/debug/benchmarkcontext.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace
{
    std::string read(const std::filesystem::path& path)
    {
        std::ifstream input(path);
        if (!input) throw std::runtime_error("Missing context output");
        return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
    }
    void need(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
}

int main(int argc, char** argv)
{
    try
    {
        if (argc != 2) throw std::runtime_error("Expected owned fixture directory");
        const auto root = std::filesystem::path(argv[1]);
        std::filesystem::create_directories(root);
        using namespace Debug::BenchmarkContext;
        const auto path = root / "context.csv";
        Capture capture;
        capture.prepare(path.string());
        capture.begin(10, 0);
        need(capture.snapshotDue(), "Initial context snapshot absent");
        Snapshot sample;
        sample.frame = 10;
        sample.valid = true;
        sample.player = { 1, 2, 3 };
        sample.setCell("cell \"one\"");
        capture.snapshot(sample);
        capture.end(10, 0);
        capture.begin(11, 0);
        need(!capture.snapshotDue(), "Unchanged context sampled every frame");
        capture.observe(Paused | Gui);
        need(capture.snapshotDue(), "Transient pause did not request snapshot");
        capture.observe(0);
        capture.end(11, 0);
        capture.begin(12, 0);
        capture.end(42, 0);
        capture.begin(43, NotRunning | MainMenu);
        capture.end(43, NotRunning | MainMenu);
        capture.finish(true);
        const auto rows = read(path);
        need(rows.find("10,10,0,0,0\n") != std::string::npos, "Active frame changed");
        need(rows.find("11,11,0,0,3\n") != std::string::npos, "Transient pause was lost at final observation");
        need(rows.find("12,42,0,0,32\n") != std::string::npos, "Loading viewer advance hidden");
        need(rows.find("43,43,20,20,20\n") != std::string::npos, "Main menu frame misclassified");
        need(read(std::filesystem::path(path) += ".snapshots.csv").find("\"cell \"\"one\"\"\"") != std::string::npos,
            "Owned snapshot CSV text not escaped");
        const auto status = read(std::filesystem::path(path) += ".capture-status.txt");
        need(status.find("rows=4\n") != std::string::npos && status.find("dropped=0\n") != std::string::npos
            && status.find("unfinished_frames=0\n") != std::string::npos, "Context completion wrong");

        const auto partial = root / "partial.csv";
        Capture overflow;
        overflow.prepare(partial.string());
        for (unsigned frame = 0; frame < 131073; ++frame)
        {
            overflow.begin(frame, 0);
            overflow.end(frame, 0);
        }
        overflow.begin(131074, Paused);
        overflow.finish(false);
        const auto incomplete = read(std::filesystem::path(partial) += ".capture-status.txt");
        need(incomplete.find("normal_finish=0\n") != std::string::npos
            && incomplete.find("dropped=1\n") != std::string::npos
            && incomplete.find("unfinished_frames=1\n") != std::string::npos, "Incomplete context accepted");
        Snapshot longCell;
        longCell.setCell(std::string(500, 'x'));
        need(longCell.cellTruncated && longCell.cell.back() == '\0', "Cell overflow hidden");
        std::cout << "PASS context flags, transient states, loading advance, snapshots, bounds and failure receipts\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
