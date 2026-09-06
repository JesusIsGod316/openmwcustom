#ifndef OPENMW_TOOLS_V4_CP3B4_ASSET_REPORT_H
#define OPENMW_TOOLS_V4_CP3B4_ASSET_REPORT_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Cp3b4
{
    inline constexpr std::string_view AssetReportSchema = "openmw-v4-cp3b4-asset-report-v1";

    struct TranslationCounts
    {
        std::uint32_t rendered = 0;
        std::uint32_t collisionOnly = 0;
        std::uint32_t hidden = 0;
        std::uint32_t deferred = 0;
        std::uint32_t unsupported = 0;
        std::uint32_t ignored = 0;
    };

    struct RealizationCounts
    {
        std::uint32_t draws = 0;
        std::uint32_t sortedDraws = 0;
        std::uint32_t billboardDraws = 0;
        std::uint32_t pipelines = 0;
        std::uint32_t materials = 0;
        std::uint32_t textureViews = 0;
        std::uint32_t samplers = 0;
        std::uint32_t textureLoads = 0;
        std::uint32_t textureCacheHits = 0;
        std::uint32_t unsupportedTextureBindings = 0;
        std::uint32_t runtimeContextEffects = 0;
    };

    struct TextureDecodeCounts
    {
        std::uint32_t warningFallbacks = 0;
        std::vector<std::string> diagnostics;
    };

    struct TranslationDiagnostic
    {
        std::string severity;
        std::string code;
        std::optional<std::uint32_t> record;
        std::string type;
        std::string message;
    };

    struct AssetReport
    {
        std::string nif;
        bool complete = false;
        std::string stage;
        std::string publishStatus;
        TranslationCounts translation;
        RealizationCounts realization;
        TextureDecodeCounts textureDecode;
        std::vector<TranslationDiagnostic> translationDiagnostics;
        std::vector<std::string> realizationDiagnostics;
    };

    inline void writeJsonString(std::ostream& out, std::string_view value)
    {
        out << '"';
        for (const unsigned char ch : value)
        {
            switch (ch)
            {
                case '"': out << "\\\""; break;
                case '\\': out << "\\\\"; break;
                case '\b': out << "\\b"; break;
                case '\f': out << "\\f"; break;
                case '\n': out << "\\n"; break;
                case '\r': out << "\\r"; break;
                case '\t': out << "\\t"; break;
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

    inline void writeStringArray(std::ostream& out, const std::vector<std::string>& values, std::string_view indent)
    {
        out << '[';
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            if (i != 0)
                out << ',';
            out << '\n' << indent;
            writeJsonString(out, values[i]);
        }
        if (!values.empty())
            out << '\n';
        out << ']';
    }

    inline void writeAssetReport(std::ostream& out, const AssetReport& report)
    {
        out << "{\n  \"schema\":";
        writeJsonString(out, AssetReportSchema);
        out << ",\n  \"nif\":";
        writeJsonString(out, report.nif);
        out << ",\n  \"complete\":" << (report.complete ? "true" : "false") << ",\n  \"stage\":";
        writeJsonString(out, report.stage);
        out << ",\n  \"publishStatus\":";
        writeJsonString(out, report.publishStatus);

        out << ",\n  \"translation\":{"
            << "\"rendered\":" << report.translation.rendered << ','
            << "\"collisionOnly\":" << report.translation.collisionOnly << ','
            << "\"hidden\":" << report.translation.hidden << ','
            << "\"deferred\":" << report.translation.deferred << ','
            << "\"unsupported\":" << report.translation.unsupported << ','
            << "\"ignored\":" << report.translation.ignored << "},\n";

        out << "  \"realization\":{"
            << "\"draws\":" << report.realization.draws << ','
            << "\"sortedDraws\":" << report.realization.sortedDraws << ','
            << "\"billboardDraws\":" << report.realization.billboardDraws << ','
            << "\"pipelines\":" << report.realization.pipelines << ','
            << "\"materials\":" << report.realization.materials << ','
            << "\"textureViews\":" << report.realization.textureViews << ','
            << "\"samplers\":" << report.realization.samplers << ','
            << "\"textureLoads\":" << report.realization.textureLoads << ','
            << "\"textureCacheHits\":" << report.realization.textureCacheHits << ','
            << "\"unsupportedTextureBindings\":" << report.realization.unsupportedTextureBindings << ','
            << "\"runtimeContextEffects\":" << report.realization.runtimeContextEffects << "},\n";

        out << "  \"textureDecode\":{\"warningFallbacks\":" << report.textureDecode.warningFallbacks
            << ",\"diagnostics\":";
        writeStringArray(out, report.textureDecode.diagnostics, "    ");
        out << "},\n";

        out << "  \"translationDiagnostics\":[";
        for (std::size_t i = 0; i < report.translationDiagnostics.size(); ++i)
        {
            const TranslationDiagnostic& diagnostic = report.translationDiagnostics[i];
            if (i != 0)
                out << ',';
            out << "\n    {\"severity\":";
            writeJsonString(out, diagnostic.severity);
            out << ",\"code\":";
            writeJsonString(out, diagnostic.code);
            out << ",\"record\":";
            if (diagnostic.record)
                out << *diagnostic.record;
            else
                out << "null";
            out << ",\"type\":";
            writeJsonString(out, diagnostic.type);
            out << ",\"message\":";
            writeJsonString(out, diagnostic.message);
            out << '}';
        }
        if (!report.translationDiagnostics.empty())
            out << '\n' << "  ";
        out << "],\n";

        out << "  \"realizationDiagnostics\":";
        writeStringArray(out, report.realizationDiagnostics, "    ");
        out << "\n}\n";
    }

    inline void writeAssetReport(const std::filesystem::path& path, const AssetReport& report)
    {
        if (const std::filesystem::path parent = path.parent_path(); !parent.empty())
            std::filesystem::create_directories(parent);

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error("unable to open CP3B4 report for writing: " + path.string());
        writeAssetReport(out, report);
        out.flush();
        if (!out)
            throw std::runtime_error("failed while writing CP3B4 report: " + path.string());
    }
}

#endif
