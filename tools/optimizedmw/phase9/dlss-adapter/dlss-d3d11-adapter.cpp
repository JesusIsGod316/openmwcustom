#include "shared-d3d11-textures.hpp"
#include "gpu-transfers.hpp"
#include <nvsdk_ngx_helpers.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <psapi.h>
#include <mutex>

namespace
{
    using namespace P9Dlss;
    constexpr GLenum Red = 0x1903, Rg = 0x8227;
    struct Evidence
    {
        std::ofstream file;
        explicit Evidence(const std::filesystem::path& path) : file(path)
        { require(file.is_open(), "Fresh writable adapter evidence"); }
        void row(const char* check, const std::string& fields)
        {
            const std::string value = std::string("{\"schema\":1,\"check\":\"") + check + "\"," + fields + "}";
            file << value << std::endl; std::cout << value << std::endl;
            require(bool(file), "Adapter evidence output completed");
        }
    };
    void ngx(NVSDK_NGX_Result result, const char* call)
    {
        if (NVSDK_NGX_FAILED(result))
            throw std::runtime_error(std::string(call) + ": NGX=" + std::to_string(static_cast<unsigned>(result)));
    }
    void NVSDK_CONV sdkLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
    {
        static std::mutex lock;
        const std::lock_guard guard(lock);
        std::cerr << "NGX: " << (message ? message : "") << std::endl;
    }
    std::string jsonPath(const std::filesystem::path& path)
    {
        const auto value = path.u8string();
        std::string result = "\"";
        for (const auto byte : value)
        {
            const char c = static_cast<char>(byte);
            require(static_cast<unsigned char>(c) >= 32, "Runtime path contains no JSON control characters");
            if (c == '\\' || c == '"') result += '\\';
            result += c;
        }
        return result + '"';
    }
    float half(unsigned value)
    {
        const float sign = value & 0x8000 ? -1.f : 1.f;
        const unsigned exponent = (value >> 10) & 31, mantissa = value & 1023;
        if (exponent == 31) return mantissa ? NAN : sign * INFINITY;
        if (!exponent) return sign * std::ldexp(static_cast<float>(mantissa), -24);
        return sign * std::ldexp(1.f + static_cast<float>(mantissa) / 1024.f, static_cast<int>(exponent) - 15);
    }
    std::array<float, 4> color(unsigned quadrant, bool changed)
    {
        const unsigned index = (quadrant + (changed ? 1 : 0)) % 4;
        constexpr std::array<std::array<float, 4>, 4> values{{
            {.75f, .125f, .125f, 1.f}, {.125f, .75f, .125f, 1.f},
            {.125f, .125f, .75f, 1.f}, {.75f, .75f, .125f, 1.f}}};
        return values[index];
    }
    void write(const Texture& texture, GLenum format, const float* data)
    {
        glBindTexture(GL_TEXTURE_2D, texture.gl);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texture.width, texture.height, format, GL_FLOAT, data);
        glCheck("GL producer writes shared typed texture");
    }
    std::vector<float> read(const Texture& texture, GLenum format, unsigned channels)
    {
        std::vector<float> values(static_cast<std::size_t>(texture.width) * texture.height * channels, NAN);
        glBindTexture(GL_TEXTURE_2D, texture.gl);
        glGetTexImage(GL_TEXTURE_2D, 0, format, GL_FLOAT, values.data());
        glCheck("GL consumes completed shared typed texture");
        return values;
    }

    struct Fixture
    {
        HiddenContext context;
        Device device;
        Bridge bridge;
        GpuTransfers gpuTransfers;
        // Retain all SDK configuration strings and descriptors through its
        // shutdown; do not rely on an undocumented copy of caller storage.
        std::filesystem::path applicationDataPath;
        std::wstring runtimeDirectory;
        std::array<const wchar_t*, 1> runtimePaths{};
        NVSDK_NGX_FeatureCommonInfo commonInfo{};
        NVSDK_NGX_Parameter* capabilities = nullptr;
        NVSDK_NGX_Handle* feature = nullptr;
        std::array<Texture, 5> textures; // color, depth, low-resolution motion, exposure, output
        std::uint64_t generation = 0, sequence = 0;
        bool retired = true;
        bool negativeNoEvaluation = false;
        bool gpuTransfersEnabled = false;
        explicit Fixture(const std::filesystem::path& logDirectory)
            : applicationDataPath(logDirectory)
        {
            bridge.device = bridge.openDevice(device.device.Get());
            require(bridge.device != nullptr, "Open exact shared D3D11 device");
            wchar_t exe[32768]{};
            const DWORD length = GetModuleFileNameW(nullptr, exe, 32768);
            require(length && length < 32768, "Fixture executable path available");
            runtimeDirectory = std::filesystem::path(exe).parent_path().wstring();
            runtimePaths[0] = runtimeDirectory.c_str();
            commonInfo.PathListInfo = {runtimePaths.data(), 1};
            commonInfo.LoggingInfo = {sdkLog, NVSDK_NGX_LOGGING_LEVEL_VERBOSE, true};
            // This is our project identifier; no registered identifier from another game.
            ngx(NVSDK_NGX_D3D11_Init_with_ProjectID("54e0e39d-bf8f-44e0-b523-8ac7b0e39cc9",
                NVSDK_NGX_ENGINE_TYPE_CUSTOM, "OptimizedMW-standalone-adapter-v1",
                applicationDataPath.c_str(), device.device.Get(), &commonInfo), "Initialize real NGX D3D11 backend");
            ngx(NVSDK_NGX_D3D11_GetCapabilityParameters(&capabilities), "Get actual NGX capabilities");
            int available = 0, needsDriver = 0;
            ngx(capabilities->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available), "Query DLSS support");
            capabilities->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
            require(available && !needsDriver, "Actual DLSS super-resolution support on current driver");
        }
        void create(unsigned width, unsigned height, Evidence& evidence)
        {
            require(retired && !feature, "Generation recreation requires complete retirement");
            unsigned iw = 0, ih = 0, maxw = 0, maxh = 0, minw = 0, minh = 0;
            float sharpness = 0;
            ngx(NGX_DLSS_GET_OPTIMAL_SETTINGS(capabilities, width, height, NVSDK_NGX_PerfQuality_Value_MaxQuality,
                &iw, &ih, &maxw, &maxh, &minw, &minh, &sharpness), "Get real DLSS input dimensions");
            require(iw && ih && iw < width && ih < height, "Real super-resolution scale");
            textures[0] = device.make(bridge, iw, ih, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
            textures[1] = device.make(bridge, iw, ih, DXGI_FORMAT_R32_FLOAT, false);
            textures[2] = device.make(bridge, iw, ih, DXGI_FORMAT_R16G16_FLOAT, false);
            textures[3] = device.make(bridge, 1, 1, DXGI_FORMAT_R32_FLOAT, false);
            textures[4] = device.make(bridge, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
            if (gpuTransfersEnabled) gpuTransfers.makeConsumer(width, height);
            NVSDK_NGX_DLSS_Create_Params params{};
            params.Feature.InWidth = iw; params.Feature.InHeight = ih;
            params.Feature.InTargetWidth = width; params.Feature.InTargetHeight = height;
            params.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_MaxQuality;
            params.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
            ngx(NGX_D3D11_CREATE_DLSS_EXT(device.commands.Get(), &feature, capabilities, &params),
                "Create real NVIDIA DLSS feature");
            device.completion();
            wchar_t modulePath[32768]{}, executablePath[32768]{};
            const auto coreModule = GetModuleHandleW(L"_nvngx.dll");
            const DWORD moduleLength = GetModuleFileNameW(coreModule, modulePath, 32768);
            const DWORD executableLength = GetModuleFileNameW(nullptr, executablePath, 32768);
            require(coreModule && moduleLength && moduleLength < 32768 && executableLength && executableLength < 32768,
                "Actual installed NVIDIA NGX core identity available");
            {
                HMODULE modules[1024]{}; DWORD needed = 0;
                require(K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)
                    && needed <= sizeof(modules), "Enumerate actual runtime module identities");
                for (unsigned i = 0; i < needed / sizeof(HMODULE); ++i)
                {
                    wchar_t path[32768]{};
                    if (GetModuleFileNameW(modules[i], path, 32768))
                    {
                        const std::filesystem::path actual(path);
                        const auto name = actual.filename().wstring();
                        if (name.find(L"ngx") != std::wstring::npos || name.find(L"dlss") != std::wstring::npos)
                            evidence.row("runtime_module_observed", "\"status\":\"observing\",\"path\":" + jsonPath(actual));
                    }
                }
            }
            ++generation;
            std::ostringstream fields;
            fields << "\"status\":\"pass\",\"generation\":" << generation
                << ",\"input_width\":" << iw << ",\"input_height\":" << ih
                << ",\"output_width\":" << width << ",\"output_height\":" << height
                << ",\"input_color\":\"RGBA16F\",\"depth\":\"R32F\",\"motion\":\"RG16F\",\"output\":\"RGBA16F\""
                << ",\"actual_ngx_core\":" << jsonPath(modulePath)
                << ",\"configured_dlss_runtime\":" << jsonPath(
                    std::filesystem::path(executablePath).parent_path() / "nvngx_dlss.dll")
                << ",\"snippet_provenance\":\"inspect_sdk_callback_log_not_standard_module_enumeration\"";
            evidence.row("real_feature_created", fields.str());
        }
        void evaluate(unsigned frame, Evidence& evidence)
        {
            require(feature && retired, "No early reuse of in-flight shared resources");
            retired = false; ++sequence;
            const auto& colorTexture = textures[0]; const auto& output = textures[4];
            std::vector<float> colors(static_cast<std::size_t>(colorTexture.width) * colorTexture.height * 4);
            const bool changed = frame >= 2;
            for (unsigned y = 0; y < colorTexture.height; ++y)
                for (unsigned x = 0; x < colorTexture.width; ++x)
                {
                    const auto pixel = color((x >= colorTexture.width / 2) + 2 * (y >= colorTexture.height / 2), changed);
                    std::copy(pixel.begin(), pixel.end(), colors.begin() + 4 * (y * colorTexture.width + x));
                }
            std::vector<float> depths(static_cast<std::size_t>(colorTexture.width) * colorTexture.height, .5f);
            std::vector<float> motion(static_cast<std::size_t>(colorTexture.width) * colorTexture.height * 2, 0.f);
            std::vector<float> sentinel(static_cast<std::size_t>(output.width) * output.height * 4, -9.f);
            const float exposure = 1.f;
            std::vector<HANDLE> handles; for (const auto& texture : textures) handles.push_back(texture.registration);
            const auto produceStart = Clock::now();
            bridge.lock(handles);
            if (gpuTransfersEnabled)
            {
                gpuTransfers.quadrants(textures[0], [changed](unsigned q) { return color(q, changed); });
                gpuTransfers.fill(textures[1], {.5f, 0.f, 0.f, 1.f});
                gpuTransfers.fill(textures[2], {0.f, 0.f, 0.f, 1.f});
                gpuTransfers.fill(textures[3], {exposure, 0.f, 0.f, 1.f});
                gpuTransfers.fill(textures[4], {-9.f, -9.f, -9.f, -9.f});
            }
            else
            {
                write(textures[0], GL_RGBA, colors.data()); write(textures[1], Red, depths.data());
                write(textures[2], Rg, motion.data()); write(textures[3], Red, &exposure);
                write(textures[4], GL_RGBA, sentinel.data());
            }
            bridge.unlock(handles);
            const double produceMs = milliseconds(produceStart);
            NVSDK_NGX_D3D11_DLSS_Eval_Params params{};
            params.Feature.pInColor = textures[0].dx.Get(); params.Feature.pInOutput = textures[4].dx.Get();
            params.pInDepth = textures[1].dx.Get(); params.pInMotionVectors = textures[2].dx.Get();
            params.pInExposureTexture = textures[3].dx.Get();
            params.InRenderSubrectDimensions = {colorTexture.width, colorTexture.height};
            params.InMVScaleX = 1.f; params.InMVScaleY = 1.f;
            params.InReset = (frame == 0 || frame == 2); params.InPreExposure = 1.f; params.InExposureScale = 1.f;
            params.InFrameTimeDeltaInMsec = 16.6667f;
            const auto evaluateStart = Clock::now();
            if (!negativeNoEvaluation)
                ngx(NGX_D3D11_EVALUATE_DLSS_EXT(device.commands.Get(), feature, capabilities, &params),
                    "Evaluate actual DLSS on GL-produced color/depth/motion");
            const double evaluateMs = milliseconds(evaluateStart);
            // A bounded native completion observation, not Evaluate return as a fence.
            const auto completeStart = Clock::now();
            if (!negativeNoEvaluation) device.completion();
            const double completeMs = milliseconds(completeStart);
            const auto consumeStart = Clock::now();
            bridge.lock(handles);
            if (gpuTransfersEnabled) gpuTransfers.consume(textures[4]);
            bridge.unlock(handles);
            const double graphicsConsumeMs = milliseconds(consumeStart);
            const auto verifyStart = Clock::now();
            bridge.lock(handles);
            auto pixels = read(textures[4], GL_RGBA, 4);
            if (gpuTransfersEnabled)
                require(read(gpuTransfers.consumerImage, GL_RGBA, 4) == pixels,
                    "Every actual graphics consumer pixel equals the shared DLSS output");
            // Verify input identity too: NGX cannot quietly consume an unrelated
            // texture on another device or a stale generation of our buffers.
            const auto actualColors = read(textures[0], GL_RGBA, 4);
            const auto actualDepth = read(textures[1], Red, 1);
            const auto actualMotion = read(textures[2], Rg, 2);
            const auto actualExposure = read(textures[3], Red, 1);
            require(actualColors == colors && actualDepth == depths && actualMotion == motion
                && actualExposure == std::vector<float>{exposure},
                "Exact typed input images survived actual shared evaluation");
            glBindTexture(GL_TEXTURE_2D, 0);
            bridge.unlock(handles);
            const double consumeMs = milliseconds(consumeStart);
            const double verificationMs = milliseconds(verifyStart);
            for (float value : pixels) require(std::isfinite(value) && value > -.5f && value < 2.f,
                "Every actual DLSS output component initialized and finite");
            float maximumError = 0.f;
            for (unsigned q = 0; q < 4; ++q)
            {
                const auto expected = color(q, changed);
                const unsigned cx = output.width * (q & 1 ? 3 : 1) / 4;
                const unsigned cy = output.height * (q & 2 ? 3 : 1) / 4;
                for (unsigned y = cy - 4; y < cy + 4; ++y)
                    for (unsigned x = cx - 4; x < cx + 4; ++x)
                        for (unsigned c = 0; c < 3; ++c)
                            maximumError = std::max(maximumError,
                                std::abs(pixels[4 * (y * output.width + x) + c] - expected[c]));
            }
            require(maximumError < .15f, "Spatial DLSS output follows this generation's changed GL producer");

            // An independent D3D readback must match every GL output pixel exactly.
            ComPtr<ID3D11Texture2D> staging;
            D3D11_TEXTURE2D_DESC desc{}; output.dx->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            dx(device.device->CreateTexture2D(&desc, nullptr, &staging), "Create bounded verification readback");
            device.commands->CopyResource(staging.Get(), output.dx.Get()); device.completion();
            D3D11_MAPPED_SUBRESOURCE mapped{};
            dx(device.commands->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map completed verification image");
            bool exact = true;
            for (unsigned y = 0; y < output.height; ++y)
            {
                const auto* row = reinterpret_cast<const std::uint16_t*>(
                    static_cast<const char*>(mapped.pData) + y * mapped.RowPitch);
                for (unsigned x = 0; x < output.width * 4; ++x)
                    exact = exact && half(row[x]) == pixels[y * output.width * 4 + x];
            }
            device.commands->Unmap(staging.Get(), 0);
            require(exact, "Every GL-consumed pixel equals completed D3D11 DLSS output");
            retired = true;
            std::ostringstream fields;
            fields << "\"status\":\"pass\",\"generation\":" << generation << ",\"sequence\":" << sequence
                << ",\"reset\":" << (params.InReset ? "true" : "false")
                << ",\"exact_input_pixels\":true,\"exact_cross_api_output\":true,\"maximum_patch_error\":" << maximumError
                << ",\"exact_exposure_pixel\":true"
                << ",\"gpu_produced_inputs\":" << (gpuTransfersEnabled ? "true" : "false")
                << ",\"exact_graphics_consumer_pixels\":" << (gpuTransfersEnabled ? "true" : "false")
                << ",\"gl_production_lock_write_unlock_ms\":" << produceMs
                << ",\"ngx_evaluate_cpu_ms\":" << evaluateMs << ",\"d3d_completion_observation_ms\":" << completeMs
                << ",\"gl_graphics_consumer_lock_copy_unlock_ms\":" << graphicsConsumeMs
                << ",\"gl_verification_readbacks_unlock_ms\":" << verificationMs
                << ",\"gl_consume_lock_readback_unlock_ms\":" << consumeMs
                << ",\"scope\":\"synthetic_adapter_cost_with_readback_not_game_performance\""
                << ",\"graphics_consumer_scope\":\"interop_lock_gpu_copy_unlock_without_verification_readback\"";
            evidence.row("real_dlss_evaluated", fields.str());
        }
        void destroy(Evidence& evidence)
        {
            require(retired && feature, "Feature retirement only after actual consumer completion");
            evidence.row("feature_retirement_begin", "\"status\":\"observing\"");
            device.commands->ClearState();
            device.completion("before ReleaseFeature");
            evidence.row("feature_retirement_gpu_complete", "\"status\":\"pass\"");
            ngx(NVSDK_NGX_D3D11_ReleaseFeature(feature), "Release completed DLSS feature"); feature = nullptr;
            evidence.row("feature_retirement_sdk_released", "\"status\":\"pass\"");
            // All evaluation/copy/readback work was completed above. The SDK's
            // ReleaseFeature contract frees its feature memory; it is not a
            // new application GPU workload requiring an empty-context marker.
            // Restore our immediate context before creating the next feature.
            device.commands->ClearState();
            if (gpuTransfersEnabled)
            {
                std::vector<HANDLE> handles; for (const auto& texture : textures) handles.push_back(texture.registration);
                bridge.lock(handles); gpuTransfers.retireConsumer(); bridge.unlock(handles);
            }
            for (auto& texture : textures) retire(bridge, texture);
            evidence.row("feature_retirement_resources_released", "\"status\":\"pass\"");
        }
        void close()
        {
            require(retired && !feature, "No outstanding feature at adapter shutdown");
            ngx(NVSDK_NGX_D3D11_DestroyParameters(capabilities), "Retire capability parameter owner"); capabilities = nullptr;
            ngx(NVSDK_NGX_D3D11_Shutdown1(device.device.Get()), "Shutdown actual NGX device");
            // Shutdown follows completed evaluation and feature retirement;
            // no application GPU resources remain in flight or are reused.
            require(bridge.closeDevice(bridge.device), "Close completed interop device"); bridge.device = nullptr;
            if (gpuTransfersEnabled) gpuTransfers.close();
            device.commands.Reset(); device.device.Reset(); device.adapter.Reset(); context.close();
        }
    };
}

int main(int argc, char** argv)
{
    bool negative = false, gpu = false;
    try
    {
        require(argc >= 3 && argc <= 5 && std::string(argv[1]) == "--output",
            "Usage: fixture --output fresh-evidence.jsonl [--negative-no-evaluation] [--gpu-transfers]");
        for (int i = 3; i < argc; ++i)
        {
            const std::string option(argv[i]);
            if (option == "--negative-no-evaluation" && !negative) negative = true;
            else if (option == "--gpu-transfers" && !gpu) gpu = true;
            else throw std::runtime_error("Unknown or duplicated fixture option");
        }
        const auto output = std::filesystem::absolute(argv[2]);
        require(!std::filesystem::exists(output), "Preserve previous fixture output");
        std::filesystem::create_directories(output.parent_path() / "NgxLogs");
        Evidence evidence(output);
        // Keep the native owners out of exception unwinding: on unexpected
        // driver failure, the standalone process reclaims them. Unregistering
        // possibly in-flight resources in destructors would be unsafe.
        auto* fixture = new Fixture(output.parent_path() / "NgxLogs");
        fixture->negativeNoEvaluation = negative;
        fixture->gpuTransfersEnabled = gpu;
        if (gpu) fixture->gpuTransfers.initialize();
        std::ostringstream identity;
        identity << "\"status\":\"pass\",\"device_luid_low\":" << fixture->device.glLuid.LowPart
            << ",\"device_luid_high\":" << fixture->device.glLuid.HighPart
            << ",\"device_node_mask\":" << fixture->device.nodeMask
            << ",\"vendor_id\":" << fixture->device.description.VendorId
            << ",\"device_id\":" << fixture->device.description.DeviceId
            << ",\"same_device\":true,\"window_hidden\":true,\"actual_ngx_backend\":\"D3D11\""
            << ",\"gpu_transfers\":" << (gpu ? "true" : "false");
        evidence.row("same_device_real_ngx", identity.str());
        const auto dimensions = gpu
            ? std::array{std::pair{1920u,1080u}, std::pair{1600u,900u}, std::pair{1920u,1080u}}
            : std::array{std::pair{640u,360u}, std::pair{800u,448u}, std::pair{640u,360u}};
        for (const auto extent : dimensions)
        {
            fixture->create(extent.first, extent.second, evidence);
            for (unsigned frame = 0; frame < 4; ++frame) fixture->evaluate(frame, evidence);
            fixture->destroy(evidence);
        }
        fixture->close(); delete fixture;
        evidence.row("adapter_complete", std::string("\"status\":\"pass\",\"actual_evaluations\":12,\"retired_generations\":3,")
            + "\"gpu_transfers\":" + (gpu ? "true" : "false")
            + ",\"game_dlss_enabled\":false,\"dense_game_motion_proved\":false,\"performance_promoted\":false");
        return 0;
    }
    catch (const std::exception& exception)
    {
        if (negative && std::string(exception.what()) == "Every actual DLSS output component initialized and finite")
        {
            std::cout << "{\"schema\":1,\"check\":\"negative_no_evaluation\",\"status\":\"pass\","
                "\"untouched_output_rejected\":true,\"actual_dlss_evaluations\":0}" << std::endl;
            return 0;
        }
        std::cerr << "DLSS adapter fixture failed: " << exception.what() << std::endl;
        return 1;
    }
}
