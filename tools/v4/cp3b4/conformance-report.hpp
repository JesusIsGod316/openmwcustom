#ifndef OPENMW_TOOLS_V4_CP3B4_CONFORMANCE_REPORT_H
#define OPENMW_TOOLS_V4_CP3B4_CONFORMANCE_REPORT_H

#include "asset-report.hpp"

#include <components/render/backend/vsg/staticnifconformance.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace Cp3b4
{
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

    [[nodiscard]] inline AssetReport makeAssetReport(
        std::string_view nifPath, const RenderVsg::StaticNifConformanceResult& result)
    {
        AssetReport report;
        report.nif = std::string(nifPath);
        report.complete = result.complete();
        report.stage = std::string(stageName(result.stage));
        report.publishStatus = std::string(publishStatusName(result.publishStatus));

        const NifRender::TranslationSummary& summary = result.translationSummary;
        report.translation.rendered = summary.rendered;
        report.translation.collisionOnly = summary.collisionOnly;
        report.translation.hidden = summary.hidden;
        report.translation.deferred = summary.deferred;
        report.translation.unsupported = summary.unsupported;
        report.translation.ignored = summary.ignored;

        const RenderVsg::StaticRealizationStats& stats = result.realization.stats;
        report.realization.draws = stats.drawCount;
        report.realization.sortedDraws = stats.sortedDrawCount;
        report.realization.billboardDraws = stats.billboardDraws;
        report.realization.pipelines = stats.pipelineKeys;
        report.realization.materials = stats.materialKeys;
        report.realization.textureViews = stats.textureViewKeys;
        report.realization.samplers = stats.samplerKeys;
        report.realization.textureLoads = stats.textureLoads;
        report.realization.textureCacheHits = stats.textureCacheHits;
        report.realization.unsupportedTextureBindings = stats.unsupportedTextureBindings;
        report.realization.runtimeContextEffects = stats.runtimeContextEffects;

        report.translationDiagnostics.reserve(result.translationDiagnostics.size());
        for (const NifRender::TranslationDiagnostic& source : result.translationDiagnostics)
        {
            TranslationDiagnostic diagnostic;
            diagnostic.severity = std::string(severityName(source.severity));
            diagnostic.code = source.code;
            diagnostic.record = source.sourceRecordId;
            diagnostic.type = source.sourceRecordType;
            diagnostic.message = source.message;
            report.translationDiagnostics.push_back(std::move(diagnostic));
        }
        report.realizationDiagnostics = result.realization.diagnostics;
        return report;
    }

    inline void writeAssetReport(
        const std::filesystem::path& path, std::string_view nifPath, const RenderVsg::StaticNifConformanceResult& result)
    {
        writeAssetReport(path, makeAssetReport(nifPath, result));
    }
}

#endif
