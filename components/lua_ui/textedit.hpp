#ifndef OPENMW_LUAUI_TEXTEDIT
#define OPENMW_LUAUI_TEXTEDIT

#include <cstdint>
#include <vector>

#include <MyGUI_EditBox.h>

#include "widget.hpp"

namespace LuaUi
{
    // Candidate-only private child. The base edit implementation remains in
    // MyGUI; the generation makes reuse safe after its skin/client is replaced.
    class LuaReflowEditBox : public MyGUI::EditBox
    {
        MYGUI_RTTI_DERIVED(LuaReflowEditBox)

    public:
        std::uint64_t skinGeneration() const { return mSkinGeneration; }

    protected:
        void initialiseOverride() override
        {
            ++mSkinGeneration;
            MyGUI::EditBox::initialiseOverride();
        }

    private:
        std::uint64_t mSkinGeneration{ 0 };
    };

    class LuaTextEdit : public MyGUI::Widget, public WidgetExtension
    {
        MYGUI_RTTI_DERIVED(LuaTextEdit)

    public:
        bool isTextInput() override { return mEditBox->getEditStatic(); }

    protected:
        void initialize() override;
        void deinitialize() override;
        void updateProperties() override;
        void updateCoord() override;
        void updateChildren() override;
        MyGUI::IntSize calculateSize() const override;

        const std::vector<std::string_view>& allUsedProperties() const override;

    private:
        void textChange(MyGUI::EditBox*);
        void setEditBoxSize(const MyGUI::IntSize&) const;

        MyGUI::EditBox* mEditBox = nullptr;
        LuaReflowEditBox* mReflowEditBox = nullptr;
        bool mMultiline{ false };
        bool mAutoSize{ false };
        bool mReflowPropertiesApplied{ false };
        std::uint64_t mReflowPropertySkinGeneration{ 0 };
        mutable std::uint64_t mReflowSizeSkinGeneration{ 0 };
    };
}

#endif // OPENMW_LUAUI_TEXTEDIT
