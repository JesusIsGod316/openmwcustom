#include <components/render/backend/vsg/populationbindings.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace
{
    void require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }
}

int main()
{
    try
    {
        std::vector<RenderCore::PopulationInstanceRecord> rows(4096);
        for (std::size_t slot = 0; slot < rows.size(); ++slot)
        {
            rows[slot].sourceIdentity = "reference:" + std::to_string(slot);
            rows[slot].transform.translation.x = static_cast<double>(slot);
        }
        RenderVsg::PopulationBindings bindings(rows);
        require(bindings.valid(), "unique authored references must bind");
        auto unchanged = bindings.prepare(rows);
        require(unchanged && unchanged->changes.empty(), "unchanged population must produce zero row writes");
        std::reverse(rows.begin(), rows.end());
        auto reordered = bindings.prepare(rows);
        require(reordered && reordered->changes.empty(), "reordering cannot reassign stable slots");
        rows[7].transform.translation.z = 41;
        auto moved = bindings.prepare(rows);
        require(moved && moved->changes.size() == 1 && moved->changes[0].slot == 4088,
            "movement must name exactly its original row");
        require(bindings.rows()[4088].transform.translation.z == 0, "preparation must not mutate live rows");
        auto superseded = bindings.prepare(rows);
        require(bindings.publish(*moved), "valid row update must publish");
        require(!bindings.publish(*superseded), "stale prepared serial must be rejected");
        require(bindings.rows()[4088].transform.translation.z == 41, "movement lost at publication");
        require(bindings.prepare(rows)->changes.empty(), "settled placement must stop producing writes");
        RenderVsg::PopulationBindings other(rows);
        require(!other.publish(*bindings.prepare(rows)), "cross-resident update must be rejected");
        auto invalid = rows;
        invalid.pop_back();
        require(!bindings.prepare(invalid), "removal must request GPU-safe resident replacement");
        invalid = rows;
        invalid.front().sourceIdentity = "new reference";
        require(!bindings.prepare(invalid), "same-count identity replacement must not reuse another object's slot");
        invalid = rows;
        invalid.front().sourceIdentity = invalid.back().sourceIdentity;
        require(!bindings.prepare(invalid), "duplicate identities must fall back");
        require(!RenderVsg::PopulationBindings(invalid).valid(), "duplicate initial identity must fall back");
        invalid = rows;
        invalid.front().sourceIdentity.clear();
        require(!RenderVsg::PopulationBindings(invalid).valid(), "anonymous assets must fall back");
        invalid = rows;
        invalid.front().semanticFlags ^= RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::ShadowCaster);
        require(!bindings.prepare(invalid), "shadow traversal changes must rebuild the graph");
        invalid = rows;
        invalid.front().lightingEnabled = false;
        require(!bindings.prepare(invalid), "lighting changes must not masquerade as transforms");
        rows[9].lod.maximumDistance = 250;
        auto lod = bindings.prepare(rows);
        require(lod && lod->changes.size() == 1 && bindings.publish(*lod), "LOD changes must update their bound row");
        require(bindings.prepare(rows)->changes.empty(), "LOD publication must settle");
        auto forged = *bindings.prepare(rows);
        forged.changes.push_back({rows.size(), rows.front()});
        require(!bindings.publish(forged), "out-of-range prepared update accepted");
        forged = *bindings.prepare(rows);
        forged.changes.push_back({0, rows.front()});
        require(!bindings.publish(forged), "identity-forged prepared update accepted");
        auto several = rows;
        several.front().transform.translation.y += 1;
        several.back().transform.translation.y += 1;
        auto multi = bindings.prepare(several);
        require(multi && multi->changes.size() == 2 && multi->changes[0].slot < multi->changes[1].slot,
            "reordered input must produce deterministic unique row writes");
        forged = *multi;
        forged.changes.push_back(forged.changes.back());
        require(!bindings.publish(forged) && bindings.publish(*multi), "duplicate row write accepted or valid update corrupted");
        std::cout << "PASS persistent population identities, targeted updates, transactions and conservative fallbacks\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& e)
    {
        std::cerr << "FAIL " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
