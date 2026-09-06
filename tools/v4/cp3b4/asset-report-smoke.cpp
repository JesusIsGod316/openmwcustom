#include "asset-report.hpp"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace
{
    void require(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }
}

int main()
{
    Cp3b4::AssetReport report;
    report.nif = "meshes/test/quoted-\"name\".nif";
    report.complete = true;
    report.stage = "complete";
    report.publishStatus = "applied";
    report.translation.rendered = 3;
    report.translation.collisionOnly = 1;
    report.translation.hidden = 2;
    report.realization.draws = 4;
    report.realization.sortedDraws = 1;
    report.realization.billboardDraws = 1;
    report.realization.pipelines = 2;
    report.realization.materials = 3;

    Cp3b4::TranslationDiagnostic diagnostic;
    diagnostic.severity = "warning";
    diagnostic.code = "cp3b4-test";
    diagnostic.record = 42;
    diagnostic.type = "NiTriShape";
    diagnostic.message = "line one\nline two with \\ and \"quotes\"";
    report.translationDiagnostics.push_back(diagnostic);
    report.realizationDiagnostics.emplace_back("realizer diagnostic");

    std::ostringstream encoded;
    Cp3b4::writeAssetReport(encoded, report);
    const std::string json = encoded.str();
    require(json.find("\"schema\":\"openmw-v4-cp3b4-asset-report-v1\"") != std::string::npos,
        "asset report schema missing");
    require(json.find("\"rendered\":3") != std::string::npos, "translation count missing");
    require(json.find("\"draws\":4") != std::string::npos, "realization count missing");
    require(json.find("quoted-\\\"name\\\"") != std::string::npos, "JSON quote escaping missing");
    require(json.find("line one\\nline two with \\\\ and \\\"quotes\\\"") != std::string::npos,
        "JSON diagnostic escaping missing");

    std::cout << json;
    return 0;
}
