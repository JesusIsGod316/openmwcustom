#include "text.hpp"

#include "alignment.hpp"

#include <components/debug/hitchtrace.hpp>

namespace LuaUi
{
    LuaText::LuaText()
        : mAutoSized(true)
    {
    }

    void LuaText::initialiseOverride()
    {
        mReflowPropertiesApplied = false;
        MyGUI::EditBox::initialiseOverride();
    }

    void LuaText::initialize()
    {
        mReflowPropertiesApplied = false;
        changeWidgetSkin("LuaText");
        setEditStatic(true);
        setVisibleHScroll(false);
        setVisibleVScroll(false);

        WidgetExtension::initialize();
    }

    void LuaText::updateProperties()
    {
        Debug::HitchTrace::Scope properties("lua.ui.text.properties", Debug::HitchTrace::CpuTiming::WallEnvelope,
            getTypeName(), getName());
        mAutoSized = propertyValue("autoSize", true);

        setCaption(propertyValue("text", std::string()));
        const int fontHeight = propertyValue("textSize", 10);
        const bool applyFont = !reflowReuseEnabled() || !mReflowPropertiesApplied || fontHeight == 0
            || getFontHeight() != fontHeight;
        if (applyFont)
        {
            Debug::HitchTrace::Scope font("lua.ui.text.font_apply", Debug::HitchTrace::CpuTiming::WallEnvelope);
            setFontHeight(fontHeight);
        }
        setTextColour(propertyValue("textColor", MyGUI::Colour(0, 0, 0, 1)));
        setEditMultiLine(propertyValue("multiline", false));
        const bool wordWrap = propertyValue("wordWrap", false);
        const bool applyWrap = !reflowReuseEnabled() || !mReflowPropertiesApplied || getEditWordWrap() != wordWrap;
        if (applyWrap)
        {
            Debug::HitchTrace::Scope wrap("lua.ui.text.wrap_apply", Debug::HitchTrace::CpuTiming::WallEnvelope);
            setEditWordWrap(wordWrap);
        }
        properties.setMetric(0, "font_applied", applyFont);
        properties.setMetric(1, "font_skipped", !applyFont);
        properties.setMetric(2, "wrap_applied", applyWrap);
        properties.setMetric(3, "wrap_skipped", !applyWrap);
        mReflowPropertiesApplied = true;

        Alignment horizontal(propertyValue("textAlignH", Alignment::Start));
        Alignment vertical(propertyValue("textAlignV", Alignment::Start));
        setTextAlign(alignmentToMyGui(horizontal, vertical));

        setTextShadow(propertyValue("textShadow", false));
        setTextShadowColour(propertyValue("textShadowColor", MyGUI::Colour(0, 0, 0, 1)));

        WidgetExtension::updateProperties();
    }

    void LuaText::setCaption(const MyGUI::UString& caption)
    {
        Debug::HitchTrace::Scope apply("lua.ui.text.caption_apply", Debug::HitchTrace::CpuTiming::WallEnvelope);
        MyGUI::TextBox::setCaption(caption);
        if (mAutoSized)
            updateCoord();
    }

    const std::vector<std::string_view>& LuaText::allUsedProperties() const
    {
        static std::vector<std::string_view> usedProps = std::invoke([this] {
            std::vector<std::string_view> props = { "text", "textSize", "textColor", "wordWrap", "textAlignH",
                "textAlignV", "multiline", "readOnly", "autoSize", "textShadow", "textShadowColor" };
            auto baseProps = WidgetExtension::allUsedProperties();
            props.insert(props.end(), baseProps.begin(), baseProps.end());
            return props;
        });
        return usedProps;
    }

    MyGUI::IntSize LuaText::calculateSize() const
    {
        if (mAutoSized)
        {
            Debug::HitchTrace::Scope measure("lua.ui.text.measure_text", Debug::HitchTrace::CpuTiming::WallEnvelope,
                getTypeName(), getName());
            return getTextSize();
        }
        else
            return WidgetExtension::calculateSize();
    }

    MyGUI::IntCoord LuaText::getTextRegion() const
    {
        const MyGUI::IntPoint offset = WidgetExtension::getContentOffset();
        const MyGUI::IntSize size = WidgetExtension::getContentSize();
        return MyGUI::IntCoord(offset, size);
    }

    MyGUI::IntSize LuaText::getViewSize() const
    {
        return WidgetExtension::getContentSize();
    }
}
