#include <components/fx/vulkanshader.hpp>
#include <iostream>
#include <stdexcept>

namespace
{
    void require(bool value) { if (!value) throw std::runtime_error("FX interface contract failed"); }
    void rejects(std::string vertex, std::string fragment)
    {
        bool rejected = false;
        try { Fx::qualifyVulkanInterfaces(vertex, fragment); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected);
    }
}
int main()
{
    std::string vertex = R"(
// omw_Out vec4 notADeclaration;
#define DECLARATION omw_Out vec2 alsoIgnored;
#if OMW_USE_BINDINGS
omw_In vec2 omw_Vertex;
#endif
omw_Out vec2 uv;
omw_Out mat4 PreviousProjection;
omw_Out float3x3 Stars;
omw_Out vec4 weights[2];
void main() { omw_Position = vec4(omw_Vertex,0,1); }
)";
    std::string fragment = R"(
omw_In vec4 weights[2];
/* omw_In vec3 comment; */
omw_In float3x3 Stars;
omw_In vec2 uv;
omw_In mat4 PreviousProjection;
)";
    Fx::qualifyVulkanInterfaces(vertex, fragment);
    require(vertex.find("omw_In vec2 omw_Vertex;") == std::string::npos);
    require(vertex.find("layout(location=0) omw_Out mat4 PreviousProjection;") != std::string::npos);
    require(fragment.find("layout(location=4) omw_In float3x3 Stars;") != std::string::npos);
    require(fragment.find("layout(location=7) omw_In vec2 uv;") != std::string::npos);
    require(fragment.find("layout(location=8) omw_In vec4 weights[2];") != std::string::npos);
    require(vertex.find("// omw_Out vec4 notADeclaration;") != std::string::npos);
    rejects("omw_Out vec2 uv;", "omw_In vec2 missing;");
    rejects("omw_Out mat4 uv;", "omw_In mat3 uv;");
    rejects("omw_Out vec2 uv[COUNT];", "omw_In vec2 uv[COUNT];");
    rejects("omw_In vec3 normal;", "");
    rejects("omw_Out vec2 a,b;", "");
    std::cout << "Vulkan OMWFX interface contracts passed\n";
}
