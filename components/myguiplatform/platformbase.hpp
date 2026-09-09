#ifndef OPENMW_COMPONENTS_MYGUIPLATFORM_PLATFORMBASE_H
#define OPENMW_COMPONENTS_MYGUIPLATFORM_PLATFORMBASE_H

namespace Shader
{
    class ShaderManager;
}

namespace MyGUIPlatform
{
    /// Renderer-agnostic base for the MyGUI platform, so MWGui::WindowManager can hold the platform without knowing
    /// which backend built it (the OSG MyGUIPlatform::Platform or the VSG VsgMyGui::Platform). Only the members
    /// WindowManager needs post-construction are exposed here; each concrete platform adds its own
    /// getRenderManagerPtr()/getDataManagerPtr() used at its construction site.
    class PlatformBase
    {
    public:
        virtual ~PlatformBase() = default;

        virtual void shutdown() = 0;

        /// OSG: switch the MyGUI render manager to the shader-based path using the given ShaderManager.
        /// VSG: no-op (shaders are baked into the UI pipeline).
        virtual void enableShaders(Shader::ShaderManager& shaderManager) = 0;

        /// Notify the concrete render manager of a drawable pixel-size change.
        virtual void setViewSize(int width, int height) = 0;
    };
}

#endif
