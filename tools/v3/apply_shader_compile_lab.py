from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_once(rel, old, new):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"shader-compile lab patched {rel}")


replace_once(
    "components/shader/shadermanager.cpp",
    '''#include <fstream>
#include <regex>''',
    '''#include <fstream>
#include <iomanip>
#include <regex>''',
)

replace_once(
    "components/shader/shadermanager.cpp",
    '''#include <components/debug/debuglog.hpp>''',
    '''#include <components/debug/debuglog.hpp>
#include <components/debug/v3diagnostics.hpp>''',
)

# File/include parsing is separate from define/variant generation, so a trace
# can distinguish VFS/source preparation from the later GL compile/link stall.
replace_once(
    "components/shader/shadermanager.cpp",
    '''        if (templateIt == mShaderTemplates.end())
        {
            std::filesystem::path path = mPath / templateName;''',
    '''        if (templateIt == mShaderTemplates.end())
        {
            Debug::V3Diagnostics::TraceScope trace("render", "shader_template_load", templateName, 0.1);
            Debug::V3Diagnostics::ScopedCsvTimer timer(
                Debug::V3Diagnostics::renderWriter(), "shader_template_load", templateName, 0.1);
            std::filesystem::path path = mPath / templateName;''',
)

# Shader-variant generation occurs only on a shader-cache miss. Quick cache
# hits stay effectively free because the timer is only installed after the miss.
replace_once(
    "components/shader/shadermanager.cpp",
    '''        ShaderMap::iterator shaderIt = mShaders.find(std::make_pair(templateName, defines));
        if (shaderIt == mShaders.end())
        {
            std::string shaderSource = templateIt->second;''',
    '''        ShaderMap::iterator shaderIt = mShaders.find(std::make_pair(templateName, defines));
        if (shaderIt == mShaders.end())
        {
            Debug::V3Diagnostics::TraceScope trace("render", "shader_source_create", templateName, 0.1);
            Debug::V3Diagnostics::ScopedCsvTimer timer(
                Debug::V3Diagnostics::renderWriter(), "shader_source_create", templateName, 0.1);
            std::string shaderSource = templateIt->second;''',
)

# OSG performs GL shader compilation/program linking lazily in Program::apply.
# Measure only relink events. Build the diagnostic label locally from whatever
# shader stages are actually attached; do not mutate osg::Program and do not
# assume vertex/fragment pointers are present in the low-level creation path.
replace_once(
    "components/shader/shadermanager.cpp",
    '''    void SamplerProgram::apply(osg::State& state) const
    {
        const PerContextProgram* pcp = getPCP(state);
        const bool relink = pcp->needsLink();

        osg::Program::apply(state);

        if (state.getLastAppliedProgramObject() != pcp)''',
    '''    void SamplerProgram::apply(osg::State& state) const
    {
        const PerContextProgram* pcp = getPCP(state);
        const bool relink = pcp->needsLink();

        auto& v3Writer = Debug::V3Diagnostics::renderWriter();
        const bool v3Profile = relink && v3Writer.enabled();
        std::string v3ProgramDetail;
        if (v3Profile)
        {
            for (unsigned int i = 0; i < getNumShaders(); ++i)
            {
                if (const osg::Shader* shader = getShader(i))
                {
                    if (!v3ProgramDetail.empty())
                        v3ProgramDetail += " + ";
                    v3ProgramDetail += shader->getName();
                }
            }
            if (v3ProgramDetail.empty())
                v3ProgramDetail = "unnamed_program";
        }

        const auto v3Start
            = v3Profile ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        osg::Program::apply(state);
        if (v3Profile)
        {
            const double v3Ms = Debug::V3Diagnostics::elapsedMs(v3Start);
            std::ostringstream row;
            row << Debug::V3HitchTelemetry::currentFrame() << ',' << Debug::V3Diagnostics::epochMs() << ','
                << Debug::V3Diagnostics::csvQuote("program_link_apply") << ','
                << Debug::V3Diagnostics::csvQuote(v3ProgramDetail) << ',' << std::fixed << std::setprecision(3) << v3Ms;
            v3Writer.writeLine(row.str());
        }

        if (state.getLastAppliedProgramObject() != pcp)''',
)

print("V3 Shader/Program Compile Lab source patch completed successfully.")
