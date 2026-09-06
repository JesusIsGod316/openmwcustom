#ifndef OPENMW_TOOLS_V4_CP3B4_CONFORMANCE_REPORT_H
#define OPENMW_TOOLS_V4_CP3B4_CONFORMANCE_REPORT_H

#include <components/render/backend/vsg/staticnifconformance.hpp>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ostream>
#include <stdexcept>
#include <string_view>

namespace Cp3b4
{
    inline constexpr std::string_view AssetReportSchema = "openmw-v4-cp3b4-asset-report-v1";

    inline void writeJsonString(std::ostream& out, std::string_view value)
    {
        out << '"';
        for (const unsigned char ch : value)
        {
            switch (ch)
            {
                case '"':
                    out << "\\\"";
                    break;
                case '\\':
                    out << "\\\\";
                    break;
                case '\b':
                    out << "\\b";
                    break;
                case '\f':
                    out << "\\f";
                    break;
                case '\n':
                    out << "\\n";
                    break;
                case '\r':
                    out << "\\r";
                    break;
                case '\t':
                    out << "\\t";
                    break;
                default:
                    if (ch < 0x20)
                    {
                        const std::ios_base::fmtflags flags = out.flags();
                        const char fill = out.fill();
                        out << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
                            << static_cast<unsigned int>(ch);
                        out.flags(flags);
                        out.fill(fill);
                    }
                    else
                        out << static_cast<char>(ch);
                    break;
            }
        }
        out << '"';
    }

    [[nodiscard]] inline std::string_view stageName(RenderVsg::StaticNifConformanceStage stage) noexcept
    {
        switch (stage)
        {
            case RenderVsg::StaticNifConformanceStage::Translate:
                return "translate";
            case RenderVsg::StaticNifConformanceStage::Publish:
                return "publish";
            case RenderVsg::StaticNifConformanceStage::Plan:
                return "plan";
            case RenderVsg::StaticNifConformanceStage::Realize:
                return "realize";
            case RenderVsg::StaticNifConformanceStage::Complete:
                return "complete";
        }
        return "unknown";
    }

    [[nodiscard]] inline std::string_view publishStatusName(NifRender::TranslationPublishStatus status) noexcept
    {
        switch (status)
        {
            case NifRender::TranslationPublishStatus::Applied:
                return "applied";
            case NifRender::TranslationPublishStatus::InvalidBundle:
                return "invalid-bundle";
            case NifRender::TranslationPublishStatus::TranslationErrors:
                return "translation-errors";
            case NifRender::TranslationPublishStatus::ReservationFailed:
                return "reservation-failed";
            case NifRender::TranslationPublishStatus::BatchBuildFailed:
                return "batch-build-failed";
            case NifRender::TranslationPublishStatus::PublishRejected:
                return "publish-rejected";
        }
        return "unknown";
    }

    [[nodiscard]] inline std::string_view severityName(NifRender::DiagnosticSeverity severity) noexcept
    {
        switch (severity)
        {
            case NifRender::DiagnosticSeverity::Info:
                return "info";
            case NifRender::DiagnosticSeverity::Warning:
                return "warning";
            case NifRender::DiagnosticSeverity::Error:
                return "error";
        }
        return "unknown";
    }

    inline void writeAssetReport(
        const std::filesystem::path& path, std::string_view nifPath, const RenderVsg::StaticNifConformanceResult& result)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error("unable to open CP3B4 report for writing: " + path.string());

        const NifRender::TranslationSummary& summary = result.translationSummary;
        const RenderVsg::StaticRealizationStats& stats = result.realization.stats;

        out << "{\n  \"schema\":";
        writeJsonString(out, AssetReportSchema);
        out << ",\n  \"nif\":";
        writeJsonString(out, nifPath);
        out << ",\n  \"complete\":" << (result.complete() ? "true" : "false") << ",\n  \"stage\":";
        writeJsonString(out, stageName(result.stage));
        out << ",\n  \"publishStatus\":";
        writeJsonString(out, publishStatusName(result.publishStatus));

        out << ",\n  \"translation\":{"
            << "\"rendered\":" << summary.rendered << ','
            << "\"collisionOnly\":" << summary.collisionOnly << ','
            << "\"hidden\":" << summary.hidden << ','
            << "\"deferred\":" << summary.deferred << ','
            << "\"unsupported\":" << summary.unsupported << ','
            << "\"ignored\":" << summary.ignored << "},\n";

        out << "  \"realization\":{"
            << "\"draws\":" << stats.drawCount << ','
            << "\"sortedDraws\":" << stats.sortedDrawCount << ','
            << "\"billboardDraws\":" << stats.billboardDraws << ','
            << "\"pipelines\":" << stats.pipelineKeys << ','
            << "\"materials\":" << stats.materialKeys << ','
            << "\"textureViews\":" << stats.textureViewKeys << ','
            << "\"samplers\":" << stats.samplerKeys << ','
            << "\"textureLoads\":" << stats.textureLoads << ','
            << "\"textureCacheHits\":" << stats.textureCacheHits << ','
            << "\"unsupportedTextureBindings\":" << stats.unsupportedTextureBindings << ','
            << "\"runtimeContextEffects\":" << stats.runtimeContextEffects << "},\n";

        out << "  \"translationDiagnostics\":[";
        for (std::size_t i = 0; i < result.translationDiagnostics.size(); ++i)
        {
            const NifRender::TranslationDiagnostic& diagnostic = result.translationDiagnostics[i];
            if (i != 0)
                out << ',';
            out << "\n    {\"severity\":";
            writeJsonString(out, severityName(diagnostic.severity));
            out << ",\"code\":";
            writeJsonString(out, diagnostic.code);
            out << ",\"record\":";
            if (diagnostic.sourceRecordId)
                out << *diagnostic.sourceRecordId;
            else
                out << "null";
            out << ",\"type\":";
            writeJsonString(out, diagnostic.sourceRecordType);
            out << ",\"message\":";
            writeJsonString(out, diagnostic.message);
            out << '}';
        }
        if (!result.translationDiagnostics.empty())
            out << '\n' << "  ";
        out << "],\n";

        out << "  \"realizationDiagnostics\":[";
        for (std::size_t i = 0; i < result.realization.diagnostics.size(); ++i)
        {
            if (i != 0)
                out << ',';
            out << '\n' << "    ";
            writeJsonString(out, result.realization.diagnostics[i]);
        }
        if (!result.realization.diagnostics.empty())
            out << '\n' << "  ";
        out << "]\n}\n";

        out.flush();
        if (!out)
            throw std::runtime_error("failed while writing CP3B4 report: " + path.string());
    }
}

#endif
