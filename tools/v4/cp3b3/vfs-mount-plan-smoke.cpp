#include "vfs-mount-plan.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <vector>

namespace
{
    [[noreturn]] void fail(const char* message)
    {
        std::cerr << "CP3B3 VFS mount-plan smoke failure: " << message << '\n';
        std::exit(1);
    }
}

int main()
{
    using Cp3b3::VfsMountEntry;
    using Cp3b3::VfsMountKind;

    const std::filesystem::path archiveA("Morrowind.bsa");
    const std::filesystem::path archiveB("Tribunal.bsa");
    const std::filesystem::path dataA("Data Files");
    const std::filesystem::path dataB("Project Cyrodiil");

    const std::vector<VfsMountEntry> plan = Cp3b3::buildVfsMountPlan(
        { archiveA, archiveB }, { dataA, dataB, dataA });

    if (plan.size() != 4)
        fail("duplicate loose data root was not ignored");

    if (plan[0].kind != VfsMountKind::Archive || plan[0].path != archiveA)
        fail("first archive order changed");
    if (plan[1].kind != VfsMountKind::Archive || plan[1].path != archiveB)
        fail("later archive did not retain later/higher-priority position");
    if (plan[2].kind != VfsMountKind::DataRoot || plan[2].path != dataA)
        fail("loose data roots are not mounted after archives");
    if (plan[3].kind != VfsMountKind::DataRoot || plan[3].path != dataB)
        fail("later loose root did not retain later/higher-priority position");

    std::cout << "CP3B3 VFS mount-plan smoke: PASS\n";
    return 0;
}
