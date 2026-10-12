#include "textedit.hpp"

#include "alignment.hpp"

#include <components/debug/hitchtrace.hpp>

namespace LuaUi
{
    void LuaTextEdit::initialize()
    {
        mReflowPropertiesApplied = false;
        mReflowPropertySkinGeneration = 0;
        mReflowSizeSkinGeneration = 0;
        if (reflowReuseEnabled())
        {
            mReflowEditBox = createWidget<LuaReflowEditBox>("LuaTextEdit", MyGUI::IntCoord(0, 0, 0, 0), MyGUI::Align::Default);
            mEditBox = mReflowEditBox;
        }
        else
            mEditBox = createWidget<MyGUI::EditBox>("LuaTextEdit", MyGUI::IntCoord(0, 0, 0, 0), MyGUI::Align::Default);
        mEditBox->eventEditTextChange += MyGUI::newDelegate(this, &LuaTextEdit::textChange);
        mEditBox->setMaxTextLength(std::numeric_limits<std::size_t>::max());
        registerEvents(mEditBox);
        WidgetExtension::initialize();
    }

    void LuaTextEdit::deinitialize()
    {
        mEditBox->eventEditTextChange -= MyGUI::newDelegate(this, &LuaTextEdit::textChange);
        clearEvents(mEditBox);
        WidgetExtension::deinitialize();
    }

    void LuaTextEdit::updateProperties()
    {
        Debug::HitchTrace::Scope properties("lua.ui.edit.properties", Debug::HitchTrace::CpuTiming::WallEnvelope,
            widget()->getTypeName(), widget()->getName());
        const int fontHeight = propertyValue("textSize", 10);
        const bool reuse = mReflowEditBox != nullptr && mReflowPropertiesApplied
            && mReflowPropertySkinGeneration == mReflowEditBox->skinGeneration();
        // The initial call establishes MyGUI's custom-font-height semantics.
        // Zero means "use the font default" and must still be applied explicitly.
        const bool applyFont = !reuse || fontHeight == 0 || mEditBox->getFontHeight() != fontHeight;
        if (applyFont)
        {
            Debug::HitchTrace::Scope font("lua.ui.edit.font_apply", Debug::HitchTrace::CpuTiming::WallEnvelope);
            mEditBox->setFontHeight(fontHeight);
        }
        mEditBox->setTextColour(propertyValue("textColor", MyGUI::Colour(0, 0, 0, 1)));
        const bool wordWrap = propertyValue("wordWrap", false);
        const bool applyWrap = !reuse || mEditBox->getEditWordWrap() != wordWrap;
        if (applyWrap)
        {
            Debug::HitchTrace::Scope wrap("lua.ui.edit.wrap_apply", Debug::HitchTrace::CpuTiming::WallEnvelope);
            mEditBox->setEditWordWrap(wordWrap);
        }
        properties.setMetric(0, "font_applied", applyFont);
        properties.setMetric(1, "font_skipped", !applyFont);
        properties.setMetric(2, "wrap_applied", applyWrap);
        properties.setMetric(3, "wrap_skipped", !applyWrap);
        mReflowPropertiesApplied = true;
        if (mReflowEditBox != nullptr)
            mReflowPropertySkinGeneration = mReflowEditBox->skinGeneration();

        Alignment horizontal(propertyValue("textAlignH", Alignment::Start));
        Alignment vertical(propertyValue("textAlignV", Alignment::Start));
        mEditBox->setTextAlign(alignmentToMyGui(horizontal, vertical));

        {
            Debug::HitchTrace::Scope modes("lua.ui.edit.modes_apply", Debug::HitchTrace::CpuTiming::WallEnvelope);
            mMultiline = propertyValue("multiline", false);
            mEditBox->setEditMultiLine(mMultiline);

            bool readOnly = propertyValue("readOnly", false);
            mEditBox->setEditStatic(readOnly);

            mAutoSize = (readOnly || !mMultiline) && propertyValue("autoSize", false);
        }

        // change caption last, for multiline and wordwrap to apply
        {
            Debug::HitchTrace::Scope caption("lua.ui.edit.caption_apply", Debug::HitchTrace::CpuTiming::WallEnvelope);
            mEditBox->setCaption(propertyValue("text", std::string()));
        }

        WidgetExtension::updateProperties();
    }

    void LuaTextEdit::textChange(MyGUI::EditBox*)
    {
        protectedCall([this](LuaUtil::LuaView& view) {
            triggerEvent("textChanged", sol::make_object(view.sol(), mEditBox->getCaption().asUTF8()));
        });
    }

    void LuaTextEdit::updateCoord()
    {
        Debug::HitchTrace::Scope layout("lua.ui.edit.layout", Debug::HitchTrace::CpuTiming::WallEnvelope,
            widget()->getTypeName(), widget()->getName());
        WidgetExtension::updateCoord();
        mEditBox->setPosition(getContentOffset());
        setEditBoxSize(getContentSize());
    }

    void LuaTextEdit::setEditBoxSize(const MyGUI::IntSize& size) const
    {
        Debug::HitchTrace::Scope resize("lua.ui.edit.size_apply", Debug::HitchTrace::CpuTiming::WallEnvelope);
        // EditBox::setSize invalidates wrapped text even when its size is
        // unchanged. Preserve MyGUI's own measurement cache; actual text/font/
        // skin/viewport changes continue to invalidate it through MyGUI.
        const bool applySize = mReflowEditBox == nullptr
            || mReflowSizeSkinGeneration != mReflowEditBox->skinGeneration() || mEditBox->getSize() != size;
        resize.setMetric(0, "size_applied", applySize);
        resize.setMetric(1, "size_skipped", !applySize);
        if (applySize)
            mEditBox->setSize(size);
        if (mReflowEditBox != nullptr)
            mReflowSizeSkinGeneration = mReflowEditBox->skinGeneration();
    }

    void LuaTextEdit::updateChildren()
    {
        WidgetExtension::updateChildren();
        // otherwise it won't be focusable
        mEditBox->detachFromWidget();
        mEditBox->attachToWidget(this);
    }

    MyGUI::IntSize LuaTextEdit::calculateSize() const
    {
        Debug::HitchTrace::Scope measure("lua.ui.edit.measure", Debug::HitchTrace::CpuTiming::WallEnvelope,
            widget()->getTypeName(), widget()->getName());
        MyGUI::IntSize normalSize = WidgetExtension::calculateSize();
        if (mAutoSize)
        {
            const MyGUI::IntSize contentSize(std::max(0, normalSize.width - (mPadding.mLeft + mPadding.mRight)),
                std::max(0, normalSize.height - (mPadding.mTop + mPadding.mBottom)));
            setEditBoxSize(contentSize);
            int targetHeight;
            if (mMultiline)
            {
                Debug::HitchTrace::Scope text("lua.ui.edit.measure_text", Debug::HitchTrace::CpuTiming::WallEnvelope);
                targetHeight = mEditBox->getTextSize().height;
            }
            else
                targetHeight = mEditBox->getFontHeight();
            normalSize.height = std::max(normalSize.height, targetHeight + mPadding.mTop + mPadding.mBottom);
        }
        return normalSize;
    }

    const std::vector<std::string_view>& LuaTextEdit::allUsedProperties() const
    {
        static std::vector<std::string_view> usedProps = std::invoke([this] {
            std::vector<std::string_view> props = { "text", "textSize", "textColor", "wordWrap", "textAlignH",
                "textAlignV", "multiline", "readOnly", "autoSize" };
            auto baseProps = WidgetExtension::allUsedProperties();
            props.insert(props.end(), baseProps.begin(), baseProps.end());
            return props;
        });
        return usedProps;
    }
}
