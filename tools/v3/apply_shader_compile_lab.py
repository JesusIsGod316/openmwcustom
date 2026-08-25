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
    '''#include <components/debug/debuglog.hpp>''',
    '''#include <components/debug/debuglog.hpp>
#include <components/debug/v3diagnostics.hpp>''',
)

# Source/template creation occurs on a shader-cache miss. Quick cache hits stay
# effectively free because the timer is only installed after the miss is known.
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

# Give programs stable diagnostic names derived from their shader variants.
replace_once(
    "components/shader/shadermanager.cpp",
    '''            program->addShader(vertexShader);
            program->addShader(fragmentShader);
            addLinkedShaders(vertexShader, program);''',
    '''            program->addShader(vertexShader);
            program->addShader(fragmentShader);
            program->setName(vertexShader->getName() + " + " + fragmentShader->getName());
            addLinkedShaders(vertexShader, program);''',
)

# OSG performs GL shader compilation/program linking lazily in Program::apply.
# Measure only relink events, so normal already-linked draws do not generate a
# row. This catches real draw-thread shader/link stalls rather than just source
# preprocessing time.
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
        const auto v3Start
            = v3Profile ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        osg::Program::apply(state);
        if (v3Profile)
        {
            const double v3Ms = Debug::V3Diagnostics::elapsedMs(v3Start);
            std::ostringstream row;
            row << Debug::V3HitchTelemetry::currentFrame() << ',' << Debug::V3Diagnostics::epochMs()
                << ",\"program_link_apply\"," << Debug::V3Diagnostics::csvQuote(getName()) << ','
                << std::fixed << std::setprecision(3) << v3Ms;
            v3Writer.writeLine(row.str());
        }

        if (state.getLastAppliedProgramObject() != pcp)''',
)

print("V3 Shader/Program Compile Lab source patch completed successfully.")
