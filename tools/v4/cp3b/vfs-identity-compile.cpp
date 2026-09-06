#include <components/nifrender/vfsidentity.hpp>

#include <array>
#include <cassert>

int main()
{
    const std::array<std::uint64_t, 2> hash{ 0x1u, 0x2u };
    const std::string encoded = NifRender::encodeOpenMwContentHash(hash);
    assert(encoded == "openmw128:00000000000000010000000000000002");
    return 0;
}
