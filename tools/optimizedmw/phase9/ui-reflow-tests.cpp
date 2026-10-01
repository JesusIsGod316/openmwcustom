// Real MyGUI/LuaUi layout and editing fixture. Only the render device, asset
// transport, and font atlas are headless; widget/edit/text layout code is the
// production components library and installed MyGUI engine.
#include <components/debug/hitchtrace.hpp>
#include <components/lua/asyncpackage.hpp>
#include <components/lua/luastate.hpp>
#include <components/lua_ui/content.hpp>
#include <components/lua_ui/element.hpp>
#include <components/lua_ui/text.hpp>
#include <components/lua_ui/textedit.hpp>
#include <components/lua_ui/util.hpp>
#include <components/lua_ui/widget.hpp>

#include <MyGUI_DataManager.h>
#include <MyGUI_FontManager.h>
#include <MyGUI_Gui.h>
#include <MyGUI_IFont.h>
#include <MyGUI_InputManager.h>
#include <MyGUI_LogManager.h>
#include <MyGUI_RenderManager.h>
#include <MyGUI_ResourceManager.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    void require(bool condition, std::string_view message)
    {
        if (!condition)
            throw std::runtime_error(std::string(message));
    }

    class MemoryTexture final : public MyGUI::ITexture
    {
    public:
        explicit MemoryTexture(std::string name)
            : mName(std::move(name))
        {
        }
        const std::string& getName() const override { return mName; }
        void createManual(int width, int height, MyGUI::TextureUsage usage, MyGUI::PixelFormat format) override
        {
            mWidth = width;
            mHeight = height;
            mUsage = usage;
            mFormat = format;
            mBytes.resize(static_cast<std::size_t>(width) * height * format.getBytesPerPixel());
        }
        void loadFromFile(const std::string&) override
        {
            createManual(64, 64, MyGUI::TextureUsage::Static, MyGUI::PixelFormat::R8G8B8A8);
        }
        void saveToFile(const std::string&) override {}
        void setShader(const std::string&) override {}
        void destroy() override { mBytes.clear(); }
        void* lock(MyGUI::TextureUsage) override
        {
            mLocked = true;
            return mBytes.data();
        }
        void unlock() override { mLocked = false; }
        bool isLocked() const override { return mLocked; }
        int getWidth() const override { return mWidth; }
        int getHeight() const override { return mHeight; }
        MyGUI::PixelFormat getFormat() const override { return mFormat; }
        MyGUI::TextureUsage getUsage() const override { return mUsage; }
        std::size_t getNumElemBytes() const override { return mFormat.getBytesPerPixel(); }

    private:
        std::string mName;
        int mWidth{ 64 };
        int mHeight{ 64 };
        bool mLocked{ false };
        MyGUI::TextureUsage mUsage{ MyGUI::TextureUsage::Static };
        MyGUI::PixelFormat mFormat{ MyGUI::PixelFormat::R8G8B8A8 };
        std::vector<std::uint8_t> mBytes;
    };

    class MemoryVertices final : public MyGUI::IVertexBuffer
    {
    public:
        void setVertexCount(std::size_t count) override { mVertices.resize(count); }
        std::size_t getVertexCount() const override { return mVertices.size(); }
        MyGUI::Vertex* lock() override { return mVertices.data(); }
        void unlock() override {}

    private:
        std::vector<MyGUI::Vertex> mVertices;
    };

    class HeadlessRender final : public MyGUI::RenderManager
    {
    public:
        MyGUI::IVertexBuffer* createVertexBuffer() override { return new MemoryVertices; }
        void destroyVertexBuffer(MyGUI::IVertexBuffer* buffer) override { delete buffer; }
        MyGUI::ITexture* createTexture(const std::string& name) override
        {
            auto texture = std::make_unique<MemoryTexture>(name);
            auto* result = texture.get();
            require(mTextures.emplace(name, std::move(texture)).second, "Duplicate fixture texture");
            return result;
        }
        void destroyTexture(MyGUI::ITexture* texture) override { mTextures.erase(texture->getName()); }
        MyGUI::ITexture* getTexture(const std::string& name) override
        {
            const auto found = mTextures.find(name);
            return found == mTextures.end() ? nullptr : found->second.get();
        }
        const MyGUI::IntSize& getViewSize() const override { return mViewSize; }
        MyGUI::VertexColourType getVertexFormat() const override { return MyGUI::VertexColourType::ColourABGR; }
        void setViewSize(int width, int height) override
        {
            mViewSize = MyGUI::IntSize(width, height);
            if (MyGUI::Gui::getInstancePtr() != nullptr)
                onResizeView(mViewSize);
        }
        void registerShader(const std::string&, const std::string&, const std::string&) override {}

    private:
        MyGUI::IntSize mViewSize{ 1024, 768 };
        std::map<std::string, std::unique_ptr<MemoryTexture>> mTextures;
    };

    class BytesStream final : public MyGUI::IDataStream
    {
    public:
        explicit BytesStream(std::string bytes)
            : mBytes(std::move(bytes))
        {
        }
        bool eof() override { return mOffset == mBytes.size(); }
        std::size_t size() override { return mBytes.size(); }
        void readline(std::string& line, MyGUI::Char delimiter) override
        {
            const auto end = mBytes.find(static_cast<char>(delimiter), mOffset);
            const auto length = (end == std::string::npos ? mBytes.size() : end) - mOffset;
            line.assign(mBytes, mOffset, length);
            mOffset += length + (end == std::string::npos ? 0 : 1);
        }
        std::size_t read(void* output, std::size_t count) override
        {
            count = std::min(count, mBytes.size() - mOffset);
            std::memcpy(output, mBytes.data() + mOffset, count);
            mOffset += count;
            return count;
        }

    private:
        std::string mBytes;
        std::size_t mOffset{ 0 };
    };

    class FixtureData final : public MyGUI::DataManager
    {
    public:
        explicit FixtureData(std::filesystem::path directory)
            : mDirectory(std::move(directory))
        {
        }
        MyGUI::IDataStream* getData(const std::string& name) const override
        {
            if (name == "fixture-skin.xml")
                return new BytesStream(R"(<?xml version="1.0" encoding="UTF-8"?>
<MyGUI type="Resource" version="1.1">
  <Resource type="ResourceSkin" name="Default" size="16 16"/>
</MyGUI>)");
            if (name == "fixture-layer.xml")
                return new BytesStream(R"(<?xml version="1.0" encoding="UTF-8"?>
<MyGUI type="Layer" version="1.0">
  <Layer name="Fixture" overlapped="true" pick="true"/>
</MyGUI>)");
            std::ifstream input(mDirectory / name, std::ios::binary);
            if (!input)
                return nullptr;
            return new BytesStream(std::string(std::istreambuf_iterator<char>(input), {}));
        }
        void freeData(MyGUI::IDataStream* stream) override { delete stream; }
        bool isDataExist(const std::string& name) const override
        {
            return name == "fixture-skin.xml" || name == "fixture-layer.xml"
                || std::filesystem::is_regular_file(mDirectory / name);
        }
        const MyGUI::VectorString& getDataListNames(const std::string&) const override { return mNames; }
        std::string getDataPath(const std::string& name) const override { return (mDirectory / name).string(); }

    private:
        std::filesystem::path mDirectory;
        MyGUI::VectorString mNames;
    };

    // Counts actual glyph visits made by MyGUI's production text measurement.
    // It does not implement measurement or duplicate the candidate logic.
    class CountingFont final : public MyGUI::IFont
    {
        MYGUI_RTTI_DERIVED(CountingFont)
    public:
        explicit CountingFont(MyGUI::ITexture* texture, std::string_view name = "Default", int defaultHeight = 10)
            : mTexture(texture)
            , mDefaultHeight(defaultHeight)
        {
            setResourceName(name);
            for (MyGUI::Char code = 0; code != 128; ++code)
                mGlyphs.emplace(code, MyGUI::GlyphInfo(code, 5, 10, 5, 0, 0, MyGUI::FloatRect(0, 0, .1f, .1f)));
            for (const auto code : { MyGUI::FontCodeType::Cursor, MyGUI::FontCodeType::Selected,
                     MyGUI::FontCodeType::SelectedBack, MyGUI::FontCodeType::NotDefined })
                mGlyphs.emplace(code, MyGUI::GlyphInfo(code, 1, 10, 1, 0, 0, MyGUI::FloatRect(0, 0, .1f, .1f)));
        }
        const MyGUI::GlyphInfo* getGlyphInfo(MyGUI::Char code) const override
        {
            ++mGlyphVisits;
            const auto found = mGlyphs.find(code);
            return found == mGlyphs.end() ? &mGlyphs.at(MyGUI::FontCodeType::NotDefined) : &found->second;
        }
        MyGUI::ITexture* getTextureFont() const override { return mTexture; }
        int getDefaultHeight() const override { return mDefaultHeight; }
        std::uint64_t visits() const { return mGlyphVisits; }

    private:
        MyGUI::ITexture* mTexture;
        int mDefaultHeight;
        std::map<MyGUI::Char, MyGUI::GlyphInfo> mGlyphs;
        mutable std::uint64_t mGlyphVisits{ 0 };
    };

    std::string jsonString(std::string_view text)
    {
        std::string result = "\"";
        for (const unsigned char ch : text)
        {
            switch (ch)
            {
                case '"':
                    result += "\\\"";
                    break;
                case '\\':
                    result += "\\\\";
                    break;
                case '\n':
                    result += "\\n";
                    break;
                case '\r':
                    result += "\\r";
                    break;
                case '\t':
                    result += "\\t";
                    break;
                default:
                    require(ch >= 32, "Unexpected control character in fixture text");
                    result.push_back(static_cast<char>(ch));
            }
        }
        return result + '"';
    }

    void writeCoord(std::ostream& output, const MyGUI::IntCoord& coord)
    {
        output << '[' << coord.left << ',' << coord.top << ',' << coord.width << ',' << coord.height << ']';
    }

    MyGUI::EditBox* findEditBox(MyGUI::Widget* widget)
    {
        if (auto* edit = dynamic_cast<MyGUI::EditBox*>(widget))
            return edit;
        for (std::size_t i = 0; i < widget->getChildCount(); ++i)
            if (auto* result = findEditBox(widget->getChildAt(i)))
                return result;
        return nullptr;
    }

    void writeWidget(std::ostream& output, MyGUI::Widget* widget)
    {
        // The candidate's private EditBox child adds a skin-generation hook.
        // Compare its public EditBox behavior, not that internal C++ type tag.
        const auto* reflowEdit = dynamic_cast<LuaUi::LuaReflowEditBox*>(widget);
        const auto type = reflowEdit != nullptr ? reflowEdit->MyGUI::EditBox::getTypeName() : widget->getTypeName();
        output << "{\"type\":" << jsonString(type) << ",\"coord\":";
        writeCoord(output, widget->getCoord());
        output << ",\"visible\":" << (widget->getVisible() ? "true" : "false");
        if (auto* edit = dynamic_cast<MyGUI::EditBox*>(widget))
        {
            const auto size = edit->getTextSize();
            output << ",\"caption\":" << jsonString(edit->getCaption().asUTF8()) << ",\"text_size\":[" << size.width
                   << ',' << size.height << ']' << ",\"font_height\":" << edit->getFontHeight()
                   << ",\"wrap\":" << (edit->getEditWordWrap() ? "true" : "false")
                   << ",\"multiline\":" << (edit->getEditMultiLine() ? "true" : "false")
                   << ",\"static\":" << (edit->getEditStatic() ? "true" : "false")
                   << ",\"cursor\":" << edit->getTextCursor() << ",\"selection\":[" << edit->getTextSelectionStart()
                   << ',' << edit->getTextSelectionEnd() << ']' << ",\"scroll\":[" << edit->getHScrollPosition() << ','
                   << edit->getVScrollPosition() << ']' << ",\"focus\":"
                   << (MyGUI::InputManager::getInstance().getKeyFocusWidget() == edit ? "true" : "false");
        }
        output << ",\"children\":[";
        for (std::size_t i = 0; i < widget->getChildCount(); ++i)
        {
            if (i != 0)
                output << ',';
            writeWidget(output, widget->getChildAt(i));
        }
        output << "]}";
    }

    void replaceEnvironment(std::string_view value)
    {
#ifdef _WIN32
        require(_putenv_s("OPENMW_HITCH_UI_REFLOW", std::string(value).c_str()) == 0, "Failed fixture env mutation");
#else
        require(setenv("OPENMW_HITCH_UI_REFLOW", std::string(value).c_str(), 1) == 0, "Failed fixture env mutation");
#endif
    }

    void runFixture(HeadlessRender& render, CountingFont& font, const std::filesystem::path& resultPath, bool expected)
    {
        LuaUtil::LuaState lua(nullptr, nullptr);
        lua.addInternalLibSearchPath(std::filesystem::path(P9_SOURCE) / "components/lua_ui");
        std::ofstream output(resultPath);
        require(static_cast<bool>(output), "Cannot open fixture result");
        output << "{\"policy_enabled\":" << (expected ? "true" : "false") << ",\"snapshots\":[";
        std::uint64_t stableGlyphVisits = 0;
        std::size_t snapshotCount = 0;
        lua.protectedCall([&](LuaUtil::LuaView& view) {
            auto& state = view.sol();
            auto newContent = LuaUi::loadContentConstructor(&lua);
            auto makeTable = [&] { return sol::table(state, sol::create); };
            auto makeContent = [&](const sol::table& items) { return newContent(items).get<sol::table>(); };
            sol::table hidden = makeTable();
            hidden[LuaUtil::ScriptsContainer::sScriptIdKey] = LuaUtil::ScriptId{};
            state["async"] = LuaUtil::getAsyncPackageInitializer(state, [] { return 0.; }, [] { return 0.; })(hidden);
            state.safe_script(R"(
                fixtureChanges = 0
                fixtureLastText = ''
                fixtureEvent = async:callback(function(text)
                    fixtureChanges = fixtureChanges + 1
                    fixtureLastText = text
                end)
            )");
            sol::table editProps = makeTable();
            editProps["text"]
                = "Alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi omicron pi rho sigma tau.";
            editProps["size"] = osg::Vec2f(180, 400);
            editProps["textSize"] = 10;
            editProps["wordWrap"] = true;
            editProps["multiline"] = true;
            editProps["readOnly"] = true;
            editProps["autoSize"] = true;
            sol::table editEvents = makeTable();
            editEvents["textChanged"] = state["fixtureEvent"];
            sol::table editLayout = makeTable();
            editLayout["type"] = "LuaTextEdit";
            editLayout["props"] = editProps;
            editLayout["events"] = editEvents;
            sol::table templateProps = makeTable();
            templateProps["textSize"] = 13;
            sol::table editTemplate = makeTable();
            editTemplate["type"] = "LuaTextEdit";
            editTemplate["props"] = templateProps;
            editLayout["template"] = editTemplate;
            sol::table items = makeTable();
            items[1] = editLayout;
            sol::table container = makeTable();
            container["type"] = "LuaContainer";
            container["content"] = makeContent(items);
            sol::table textProps = makeTable();
            textProps["text"] = "Status #FF0000red#000000 black";
            textProps["textSize"] = 10;
            textProps["wordWrap"] = false;
            sol::table textLayout = makeTable();
            textLayout["type"] = "LuaText";
            textLayout["props"] = textProps;
            sol::table rootItems = makeTable();
            rootItems[1] = container;
            rootItems[2] = textLayout;
            sol::table rootProps = makeTable();
            rootProps["size"] = osg::Vec2f(300, 600);
            rootProps["autoSize"] = false;
            rootProps["gap"] = 7;
            sol::table rootLayout = makeTable();
            rootLayout["type"] = "LuaFlex";
            rootLayout["layer"] = "Fixture";
            rootLayout["props"] = rootProps;
            rootLayout["content"] = makeContent(rootItems);
            sol::table options = makeTable();
            options["noWarnUnused"] = true;
            auto element = LuaUi::Element::make(rootLayout, false, options);
            element->create();
            require(element->mRoot->reflowReuseEnabled() == expected, "Startup UI policy mismatch");
            auto* editExtension = element->mRoot->children().at(0)->children().at(0);
            auto* edit = findEditBox(editExtension->widget());
            require(edit != nullptr, "Production LuaTextEdit was not created");
            require(edit->getTextSize().height > edit->getFontHeight(), "Fixture did not exercise real word wrapping");
            auto snapshot = [&](std::string_view name) {
                if (snapshotCount++ != 0)
                    output << ',';
                output << "{\"name\":" << jsonString(name)
                       << ",\"lua_change_events\":" << state["fixtureChanges"].get<int>()
                       << ",\"last_change_text\":" << jsonString(state["fixtureLastText"].get<std::string>())
                       << ",\"widget\":";
                writeWidget(output, element->mRoot->widget());
                output << '}';
            };
            auto update = [&] {
                element->mState = LuaUi::Element::Update;
                element->update();
            };
            snapshot("created_nested_wrapped_autosize");
            element->mRoot->updateCoord();
            const auto stableStart = font.visits();
            for (int i = 0; i < 24; ++i)
                element->mRoot->updateCoord();
            stableGlyphVisits = font.visits() - stableStart;
            snapshot("repeated_same_layout");
            // Mutate the existing Lua table; table identity is intentionally unchanged.
            editProps["size"] = osg::Vec2f(95, 0);
            update();
            snapshot("narrow_auto_height");
            editProps["text"] = "Short replacement";
            update();
            snapshot("caption_changed");
            editProps["textSize"] = sol::nil;
            update();
            require(edit->getFontHeight() == 13, "Template font fallback not applied");
            snapshot("template_property_fallback");
            templateProps["textSize"] = 19;
            update();
            require(edit->getFontHeight() == 19, "Mutable template property not applied");
            snapshot("template_mutated");
            editProps["textSize"] = 0;
            update();
            require(edit->getFontHeight() == 10, "Default font height semantics changed");
            snapshot("zero_default_font_height");
            edit->setFontName("FixtureAlternate");
            update();
            require(edit->getFontHeight() == 17, "Default height did not follow changed font");
            snapshot("changed_font_default_height");
            editProps["wordWrap"] = false;
            editProps["multiline"] = false;
            editProps["readOnly"] = false;
            editProps["autoSize"] = false;
            editProps["size"] = osg::Vec2f(140, 30);
            editProps["text"] = "editable";
            editProps["textSize"] = 10;
            update();
            auto& input = MyGUI::InputManager::getInstance();
            input.setKeyFocusWidget(edit);
            edit->setTextCursor(edit->getCaption().length());
            input.injectKeyPress(MyGUI::KeyCode::X, 'x');
            input.injectKeyRelease(MyGUI::KeyCode::X);
            require(state["fixtureChanges"].get<int>() > 0, "Production Lua textChanged callback was not invoked");
            snapshot("interactive_edit");
            input.injectKeyPress(MyGUI::KeyCode::LeftControl);
            input.injectKeyPress(MyGUI::KeyCode::Z, 0);
            input.injectKeyRelease(MyGUI::KeyCode::Z);
            input.injectKeyRelease(MyGUI::KeyCode::LeftControl);
            snapshot("interactive_undo");
            edit->setTextSelection(1, 4);
            snapshot("selection_before_explicit_update");
            update();
            snapshot("explicit_update_edit_semantics");
            editProps["wordWrap"] = true;
            editProps["multiline"] = true;
            editProps["readOnly"] = true;
            editProps["autoSize"] = true;
            editProps["size"] = osg::Vec2f(100, 0);
            editProps["text"] = "one two three four five six seven eight nine ten";
            update();
            snapshot("wrap_transition");
            render.setViewSize(800, 600);
            element->mRoot->updateCoord();
            snapshot("resize_and_wrap_transition");
            edit->changeWidgetSkin("LuaTextEdit");
            update();
            snapshot("skin_recreated_and_properties_reapplied");
            edit->setFontName("FixtureAlternate");
            require(edit->getFontHeight() == 10, "Skin replacement lost explicit custom font height");
            element->mRoot->updateCoord();
            snapshot("font_changed_after_skin_recreation");
            // Check the gate remains frozen for subsequent widgets, too.
            replaceEnvironment(expected ? "0" : "1");
            auto fresh = LuaUi::Element::make(editLayout, false, options);
            fresh->create();
            require(fresh->mRoot->reflowReuseEnabled() == expected, "UI policy changed after environment mutation");
            LuaUi::Element::erase(fresh.get());
            LuaUi::Element::erase(element.get());
            require(input.getKeyFocusWidget() == nullptr, "Destroyed editor retained key focus");
        });
        output << "],\"stable_glyph_visits\":" << stableGlyphVisits << ",\"total_glyph_visits\":" << font.visits()
               << ",\"snapshot_count\":" << snapshotCount << "}\n";
        output.close();
        require(static_cast<bool>(output), "Fixture result write failed");
        LuaUi::clearGameInterface();
        LuaUi::clearMenuInterface();
    }
}

int main(int argc, char** argv)
{
    try
    {
        require(argc == 3, "Usage: p9-ui-reflow RESULT.json expected-policy-0-or-1");
        const bool expected = std::string_view(argv[2]) == "1";
        MyGUI::LogManager log;
        log.setSTDOutputEnabled(false);
        FixtureData data(std::filesystem::path(P9_SOURCE) / "files/data/mygui");
        HeadlessRender render;
        MyGUI::Gui gui;
        gui.initialise("");
        LuaUi::registerAllWidgets();
        auto* texture = render.createTexture("fixture-font-atlas");
        texture->createManual(64, 64, MyGUI::TextureUsage::Static, MyGUI::PixelFormat::R8G8B8A8);
        auto* font = new CountingFont(texture);
        MyGUI::ResourceManager::getInstance().addResource(font);
        MyGUI::ResourceManager::getInstance().addResource(new CountingFont(texture, "FixtureAlternate", 17));
        MyGUI::FontManager::getInstance().setDefaultFont("Default");
        require(MyGUI::ResourceManager::getInstance().load("fixture-skin.xml"), "Fixture base skin load failed");
        require(MyGUI::ResourceManager::getInstance().load("openmw_lua.xml"), "Production Lua skin load failed");
        require(MyGUI::ResourceManager::getInstance().load("fixture-layer.xml"), "Fixture layer load failed");
        runFixture(render, *font, std::filesystem::path(argv[1]), expected);
        gui.shutdown();
        Debug::HitchTrace::Capture::instance().finish();
        std::cout << "PASS: production MyGUI UI reflow fixture\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
