#include "imagetosurface.hpp"

#include <stdexcept>

#include <SDL3/SDL.h>
#include <osg/Image>

namespace SDLUtil
{

    SurfaceUniquePtr imageToSurface(osg::Image* image, bool flip)
    {
        const int width = image->s();
        const int height = image->t();
        const SDL_PixelFormat format
            = SDL_GetPixelFormatForMasks(32, 0xFF000000, 0x00FF0000, 0x0000FF00, 0x000000FF);
        if (format == SDL_PIXELFORMAT_UNKNOWN)
            throw std::runtime_error("Failed to select SDL3 RGBA surface format: " + std::string(SDL_GetError()));

        SDL_Surface* rawSurface = SDL_CreateSurface(width, height, format);
        if (!rawSurface)
            throw std::runtime_error("Failed to create SDL3 surface: " + std::string(SDL_GetError()));

        SurfaceUniquePtr surface(rawSurface, SDL_DestroySurface);
        for (int x = 0; x < width; ++x)
            for (int y = 0; y < height; ++y)
            {
                const osg::Vec4f clr = image->getColor(x, flip ? ((height - 1) - y) : y);
                auto* p = static_cast<Uint8*>(surface->pixels) + y * surface->pitch + x * 4;
                *reinterpret_cast<Uint32*>(p)
                    = SDL_MapSurfaceRGBA(surface.get(), static_cast<Uint8>(clr.r() * 255),
                        static_cast<Uint8>(clr.g() * 255), static_cast<Uint8>(clr.b() * 255),
                        static_cast<Uint8>(clr.a() * 255));
            }

        return surface;
    }

}
