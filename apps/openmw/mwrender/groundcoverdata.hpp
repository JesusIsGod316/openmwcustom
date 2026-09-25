#ifndef OPENMW_MWRENDER_GROUNDCOVERDATA_H
#define OPENMW_MWRENDER_GROUNDCOVERDATA_H

#include <components/esm3/loadcell.hpp>
#include <components/vfs/pathutil.hpp>

#include <map>
#include <vector>

namespace MWRender
{
    struct GroundcoverEntry
    {
        ESM::RefNum mRefNum;
        ESM::Position mPos;
        float mScale = 1.0f;

        GroundcoverEntry() = default;
        explicit GroundcoverEntry(const ESM::CellRef& ref)
            : mRefNum(ref.mRefNum)
            , mPos(ref.mPos)
            , mScale(ref.mScale)
        {
        }
    };

    using GroundcoverInstanceMap = std::map<VFS::Path::Normalized, std::vector<GroundcoverEntry>, std::less<>>;
}

#endif
