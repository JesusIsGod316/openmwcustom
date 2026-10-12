#include "container.hpp"

#include <algorithm>
#include <components/debug/hitchtrace.hpp>

namespace LuaUi
{
    void LuaContainer::updateChildren()
    {
        WidgetExtension::updateChildren();
        updateSizeToFit();
    }

    MyGUI::IntSize LuaContainer::childScalingSize() const
    {
        return MyGUI::IntSize();
    }

    MyGUI::IntSize LuaContainer::templateScalingSize() const
    {
        return mInnerSize;
    }

    void LuaContainer::updateSizeToFit()
    {
        Debug::HitchTrace::Scope measure("lua.ui.container.measure", Debug::HitchTrace::CpuTiming::WallEnvelope);
        measure.setMetric(0, "content_children", children().size());
        measure.setMetric(1, "template_children", templateChildren().size());
        MyGUI::IntSize innerSize = MyGUI::IntSize();
        for (const auto w : children())
        {
            MyGUI::IntCoord coord = w->calculateCoord();
            innerSize.width = std::max(innerSize.width, coord.left + coord.width);
            innerSize.height = std::max(innerSize.height, coord.top + coord.height);
        }
        MyGUI::IntSize outerSize = innerSize;
        for (const auto w : templateChildren())
        {
            MyGUI::IntCoord coord = w->calculateCoord();
            outerSize.width = std::max(outerSize.width, coord.left + coord.width);
            outerSize.height = std::max(outerSize.height, coord.top + coord.height);
        }
        mInnerSize = innerSize;
        mOuterSize = outerSize;
    }

    MyGUI::IntSize LuaContainer::calculateSize() const
    {
        return mOuterSize;
    }

    void LuaContainer::updateCoord()
    {
        Debug::HitchTrace::Scope layout("lua.ui.container.coord", Debug::HitchTrace::CpuTiming::WallEnvelope);
        updateSizeToFit();
        WidgetExtension::updateCoord();
    }
}
