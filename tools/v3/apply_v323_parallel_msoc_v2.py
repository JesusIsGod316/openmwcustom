from pathlib import Path

HERE = Path(__file__).resolve().parent
base = HERE / "apply_v323_parallel_msoc.py"
source = base.read_text(encoding="utf-8")

start_marker = "# Replace the terrain/full raster functions together. This preserves the exact legacy path when V3.23 is off."
end_marker = "# App-layer mode plumbing and stronger exact-active paged coverage."
start = source.find(start_marker)
end = source.find(end_marker, start)
if start < 0 or end < 0:
    raise RuntimeError("V3.23 repair shim could not locate the original component-raster replacement block")

replacement = r"""# Robust V3.23 component-raster patch.
# Preserve inherited V3 instrumentation by editing the existing terrain function
# in place and inserting new helper/batch functions instead of replacing the
# complete inherited raster function bodies.
component_path = ROOT / "components/sceneutil/occlusionculling.cpp"
component = component_path.read_text(encoding="utf-8")
terrain_start = component.find("    void OcclusionCuller::rasterizeTerrainOccluder(")
aabb_start = component.find("    void OcclusionCuller::rasterizeAABBOccluder(", terrain_start)
if terrain_start < 0 or aabb_start < 0:
    raise RuntimeError("V3.23 component raster function boundaries were not found")
terrain_block = component[terrain_start:aabb_start]
terrain_call = "        rasterizeOccluder(worldPositions, indices);"
if terrain_block.count(terrain_call) != 1:
    raise RuntimeError(
        f"V3.23 terrain full-buffer call expected once, found {terrain_block.count(terrain_call)}"
    )
terrain_parallel = r'''        if (mV323ParallelMsocMode > 0 && mV323Worker && mMOCTerrainOnly)
        {
            const bool submitted = mV323Worker->trySubmit(
                [this, &worldPositions, &indices] { renderOccluderTo(mMOCTerrainOnly, worldPositions, indices); });
            // Main thread performs the exact existing full-buffer raster concurrently.
            rasterizeOccluder(worldPositions, indices);
            if (submitted)
            {
                // Bounded same-frame join on a dedicated worker. No shared preload queue participates.
                mV323Worker->wait();
                return;
            }
            // Busy-inline fallback: fall through to the inherited terrain-only raster body.
        }
        else
            rasterizeOccluder(worldPositions, indices);'''
terrain_block = terrain_block.replace(terrain_call, terrain_parallel, 1)
component = component[:terrain_start] + terrain_block + component[aabb_start:]

aabb_start = component.find("    void OcclusionCuller::rasterizeAABBOccluder(", terrain_start)
helper_and_batch = r'''    bool OcclusionCuller::renderOccluderTo(MaskedOcclusionCulling* moc,
        const std::vector<osg::Vec3f>& worldPositions, const std::vector<unsigned int>& indices) const
    {
        if (!mFrameActive || !moc || worldPositions.empty() || indices.empty())
            return false;

        const int numTris = static_cast<int>(indices.size()) / 3;
        if (numTris <= 0)
            return false;

        const unsigned int vertexCount = static_cast<unsigned int>(worldPositions.size());
        for (const auto idx : indices)
            if (idx >= vertexCount)
                return false;

        for (const auto& v : worldPositions)
        {
            const float w = mVPFloat[3] * v.x() + mVPFloat[7] * v.y() + mVPFloat[11] * v.z() + mVPFloat[15];
            if (!std::isfinite(w) || std::abs(w) < 1e-6f)
                return false;
        }

        MaskedOcclusionCulling::VertexLayout vtxLayout(12, 4, 8);
        moc->RenderTriangles(reinterpret_cast<const float*>(worldPositions.data()), indices.data(), numTris, mVPFloat,
            MaskedOcclusionCulling::BACKFACE_NONE, MaskedOcclusionCulling::CLIP_PLANE_ALL, vtxLayout);
        return true;
    }

    void OcclusionCuller::rasterizeOccluderBatch(const std::vector<V323OccluderBatchView>& meshes)
    {
        if (meshes.empty())
            return;

        if (mV323ParallelMsocMode < 2 || !mV323Worker || !mV323WorkerMOC || meshes.size() < 2)
        {
            for (const auto& mesh : meshes)
                if (mesh.vertices && mesh.indices)
                    rasterizeOccluder(*mesh.vertices, *mesh.indices);
            return;
        }

        const bool submitted = mV323Worker->trySubmit([this, &meshes] {
            mV323WorkerMOC->ClearBuffer();
            for (std::size_t i = 1; i < meshes.size(); i += 2)
            {
                const auto& mesh = meshes[i];
                if (mesh.vertices && mesh.indices)
                    renderOccluderTo(mV323WorkerMOC, *mesh.vertices, *mesh.indices);
            }
        });
        if (!submitted)
        {
            // Busy-inline fallback: preserve exact current-frame visibility without queueing.
            for (const auto& mesh : meshes)
                if (mesh.vertices && mesh.indices)
                    rasterizeOccluder(*mesh.vertices, *mesh.indices);
            return;
        }

        for (std::size_t i = 0; i < meshes.size(); i += 2)
        {
            const auto& mesh = meshes[i];
            if (mesh.vertices && mesh.indices)
                rasterizeOccluder(*mesh.vertices, *mesh.indices);
        }
        mV323Worker->wait();
        mMOC->MergeBuffer(mV323WorkerMOC);
    }

'''
component = component[:aabb_start] + helper_and_batch + component[aabb_start:]
component_path.write_text(component, encoding="utf-8", newline="\n")
print("V3.23 patched inherited terrain raster in-place + inserted parallel paged batch")

"""

source = source[:start] + replacement + source[end:]
exec(
    compile(source, str(base), "exec"),
    {"__file__": str(base), "__name__": "__main__"},
)
