#include <components/render/backend/vsg/statictexturequirks.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>

namespace
{
    [[noreturn]] void fail(const char* message)
    {
        std::cerr << "CP3B3 static texture quirk smoke failure: " << message << '\n';
        std::exit(1);
    }
}

int main()
{
    std::array<std::uint8_t, 18> tga{};
    tga[2] = 2u;
    tga[16] = 16u;
    tga[17] = 1u;
    if (!RenderVsg::openMwDiscard16BitTgaAlpha(tga))
        fail("16bpp true-color TGA one-bit alpha was not discarded");

    tga[16] = 24u;
    if (RenderVsg::openMwDiscard16BitTgaAlpha(tga))
        fail("24bpp TGA was incorrectly classified as legacy one-bit-alpha case");

    std::array<std::uint8_t, 18> paletteTga{};
    paletteTga[2] = 1u;
    paletteTga[7] = 16u;
    paletteTga[17] = 1u;
    if (!RenderVsg::openMwDiscard16BitTgaAlpha(paletteTga))
        fail("16bpp palette TGA did not use the OpenMW palette-depth byte");

    // One opaque BC1 block. color0 <= color1 enables the three-color/alpha
    // encoding, but no selector 3 is present, so OpenMW's detect mode treats it
    // as RGB DXT1.
    std::array<std::uint8_t, 8> opaqueBc1{ 0x00u, 0x00u, 0xffu, 0xffu, 0x00u, 0x00u, 0x00u, 0x00u };
    if (RenderVsg::openMwBc1UsesOneBitAlpha(opaqueBc1))
        fail("opaque BC1 block was incorrectly classified as one-bit alpha");

    // Same endpoint ordering with selector 3 in texel zero: OpenMW promotes the
    // DDS from RGB DXT1 to RGBA DXT1.
    std::array<std::uint8_t, 8> alphaBc1{ 0x00u, 0x00u, 0xffu, 0xffu, 0x03u, 0x00u, 0x00u, 0x00u };
    if (!RenderVsg::openMwBc1UsesOneBitAlpha(alphaBc1))
        fail("BC1 selector-3 transparency was not detected");

    // In four-color mode (color0 > color1), selector 3 is an interpolated opaque
    // color and must not be classified as alpha.
    std::array<std::uint8_t, 8> fourColorBc1{ 0xffu, 0xffu, 0x00u, 0x00u, 0x03u, 0x00u, 0x00u, 0x00u };
    if (RenderVsg::openMwBc1UsesOneBitAlpha(fourColorBc1))
        fail("four-color BC1 selector 3 was incorrectly treated as transparency");

    std::array<std::uint8_t, 16> mixedBc1{};
    for (std::size_t i = 0; i < opaqueBc1.size(); ++i)
        mixedBc1[i] = opaqueBc1[i];
    for (std::size_t i = 0; i < alphaBc1.size(); ++i)
        mixedBc1[8u + i] = alphaBc1[i];
    if (!RenderVsg::openMwBc1UsesOneBitAlpha(mixedBc1))
        fail("multi-block BC1 scan stopped before later transparent block");

    const std::array<std::uint8_t, 7> malformedBc1{};
    if (RenderVsg::openMwBc1UsesOneBitAlpha(std::span<const std::uint8_t>(malformedBc1)))
        fail("malformed BC1 byte count was classified as transparent");

    std::cout << "CP3B3 static texture quirk smoke: PASS\n";
    return 0;
}
