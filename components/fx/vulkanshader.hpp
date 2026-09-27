#ifndef OPENMW_COMPONENTS_FX_VULKANSHADER_HPP
#define OPENMW_COMPONENTS_FX_VULKANSHADER_HPP

#include <algorithm>
#include <cctype>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Fx
{
    struct VulkanShaderSources
    {
        std::string vertex;
        std::string fragment;
        // Identical descriptor bindings in both stages. Bindings 0, 1 and 2
        // are frame state, point lights and technique parameters respectively.
        std::vector<std::string> samplers;
    };

    namespace VulkanShaderDetail
    {
        struct Token { std::string_view text; std::size_t begin; std::size_t end; };

        // This is an interface-declaration lexer, not a replacement for the
        // OMWFX parser or GLSL compiler. Preserve comments and #line directives
        // verbatim, and let the real compiler diagnose shader expressions.
        inline std::vector<Token> tokenize(std::string_view source)
        {
            std::vector<Token> result;
            for (std::size_t i = 0; i < source.size();)
            {
                if (std::isspace(static_cast<unsigned char>(source[i]))) { ++i; continue; }
                if (source.substr(i, 2) == "//" || source[i] == '#')
                {
                    do
                    {
                        auto end = source.find('\n', i);
                        if (end == std::string_view::npos) { i = source.size(); break; }
                        const bool continued = end > 0 && source[end - 1] == '\\';
                        i = end + 1;
                        if (!continued) break;
                    } while (i < source.size());
                    continue;
                }
                if (source.substr(i, 2) == "/*")
                {
                    const auto end = source.find("*/", i + 2);
                    if (end == std::string_view::npos) throw std::runtime_error("Unterminated GLSL comment");
                    i = end + 2;
                    continue;
                }
                const auto begin = i++;
                if (std::isalnum(static_cast<unsigned char>(source[begin])) || source[begin] == '_')
                    while (i < source.size() && (std::isalnum(static_cast<unsigned char>(source[i])) || source[i] == '_')) ++i;
                result.push_back({source.substr(begin, i - begin), begin, i});
            }
            return result;
        }

        struct Interface
        {
            std::string name;
            std::string type;
            unsigned locations = 1;
            std::size_t begin = 0;
            std::size_t end = 0;
            bool vertexAttribute = false;
        };

        inline std::vector<Interface> interfaces(std::string_view body, bool vertex)
        {
            const auto tokens = tokenize(body);
            std::vector<Interface> result;
            int braces = 0;
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                if (tokens[i].text == "{") { ++braces; continue; }
                if (tokens[i].text == "}") { --braces; continue; }
                if (braces != 0) continue;
                const auto direction = tokens[i].text;
                if (direction != "omw_In" && direction != "omw_Out") continue;
                if (i + 3 >= tokens.size()) throw std::runtime_error("Incomplete OMWFX shader interface");
                Interface item;
                item.begin = tokens[i].begin;
                item.type = tokens[++i].text;
                item.name = tokens[++i].text;
                item.vertexAttribute = vertex && direction == "omw_In";
                if (item.vertexAttribute && item.name != "omw_Vertex")
                    throw std::runtime_error("Unsupported post-processing vertex attribute: " + item.name);
                if (vertex && direction != "omw_Out" && !item.vertexAttribute)
                    throw std::runtime_error("Invalid post-processing vertex interface");
                if (!vertex && direction != "omw_In")
                    throw std::runtime_error("Use omw_FragColor for the post-processing output");
                // Matrix columns consume separate locations, including Rafael's
                // floatNxM aliases. Reserve by declared type, not byte size.
                if (item.type.starts_with("mat") && item.type.size() >= 4)
                    item.locations = static_cast<unsigned>(item.type[3] - '0');
                else if (item.type.starts_with("float") && item.type.size() >= 8 && item.type[6] == 'x')
                    item.locations = static_cast<unsigned>(item.type[5] - '0');
                if (++i < tokens.size() && tokens[i].text == "[")
                {
                    if (i + 2 >= tokens.size() || tokens[i + 2].text != "]")
                        throw std::runtime_error("Unsupported OMWFX interface array declaration");
                    const std::string count(tokens[i + 1].text);
                    if (count.empty() || count.find_first_not_of("0123456789") != std::string::npos)
                        throw std::runtime_error("OMWFX interface arrays require a literal size");
                    const auto size = std::stoul(count);
                    if (size == 0 || size > 64) throw std::runtime_error("Invalid OMWFX interface array size");
                    item.locations *= static_cast<unsigned>(size);
                    i += 3;
                }
                if (i >= tokens.size() || tokens[i].text != ";")
                    throw std::runtime_error("Unsupported OMWFX interface declaration for " + item.name);
                item.end = tokens[i].end;
                result.push_back(std::move(item));
            }
            return result;
        }
    }

    inline void qualifyVulkanInterfaces(std::string& vertex, std::string& fragment)
    {
        using namespace VulkanShaderDetail;
        const auto outputs = interfaces(vertex, true);
        const auto inputs = interfaces(fragment, false);
        std::map<std::string, std::pair<unsigned, unsigned>> locations;
        for (const auto& output : outputs)
            if (!output.vertexAttribute) locations.try_emplace(output.name, 0u, output.locations);
        for (const auto& input : inputs)
        {
            const auto found = locations.find(input.name);
            if (found == locations.end() || found->second.second != input.locations)
                throw std::runtime_error("Unmatched OMWFX varying: " + input.name);
        }
        unsigned next = 0;
        for (auto& [name, location] : locations) { location.first = next; next += location.second; }
        const auto rewrite = [&](std::string& source, const auto& declarations)
        {
            for (auto it = declarations.rbegin(); it != declarations.rend(); ++it)
                if (it->vertexAttribute)
                    source.replace(it->begin, it->end - it->begin, "/* generated fullscreen vertex */");
                else
                    source.insert(it->begin, "layout(location=" + std::to_string(locations.at(it->name).first) + ") ");
        };
        rewrite(vertex, outputs);
        rewrite(fragment, inputs);
    }
}
#endif
