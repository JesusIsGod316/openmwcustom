#ifndef OPENMW_COMPONENTS_OCCLUSIONCULLING_OCCLUSIONSTORAGE_H
#define OPENMW_COMPONENTS_OCCLUSIONCULLING_OCCLUSIONSTORAGE_H

#include "occludermesh.hpp"
#include "telemetry.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct sqlite3;

namespace OcclusionCulling
{
    /// SQLite-backed cache of precomputed occluder meshes.
    /// Keyed by (model_path, mesh_res, shrink_key).
    /// Thread-safe for concurrent get() calls; put() is also guarded by a mutex.
    class OcclusionStorage
    {
    public:
        explicit OcclusionStorage(const std::string& databasePath);
        ~OcclusionStorage();

        OcclusionStorage(const OcclusionStorage&) = delete;
        OcclusionStorage& operator=(const OcclusionStorage&) = delete;

        bool isOpen() const;
        bool get(std::string_view modelPath, int meshRes, int shrinkKey, OccluderMesh& out) const;
        void put(std::string_view modelPath, int meshRes, int shrinkKey, const OccluderMesh& mesh);

        struct Stats
        {
            int memHits = 0; // served from in-memory map
            int dbHits  = 0; // loaded from SQLite
            int misses  = 0; // not in cache — buildSimplifiedMesh was called
            int writes  = 0; // written to SQLite
        };
        /// Returns accumulated stats since last call and resets counters.
        Stats getAndResetStats();

        void recordMiss() { ++mMisses; }

        static int makeShrinkKey(float shrinkFactor);

    private:
        bool deserialize(const void* data, int bytes, OccluderMesh& out) const;
        std::vector<std::byte> serialize(const OccluderMesh& mesh) const;
        std::string makeKey(std::string_view modelPath, int meshRes, int shrinkKey) const;

        sqlite3* mDb = nullptr;
        mutable std::mutex mMutex;
        mutable std::unordered_map<std::string, std::shared_ptr<OccluderMesh>> mCache;

        // Interval counters preserve V2's existing 300-frame debug statistics while
        // Telemetry::CacheCounter also tracks lifetime totals for V3 telemetry.
        mutable Telemetry::CacheCounter<Telemetry::CacheCounterId::MemHits> mMemHits;
        mutable Telemetry::CacheCounter<Telemetry::CacheCounterId::DbHits> mDbHits;
        mutable Telemetry::CacheCounter<Telemetry::CacheCounterId::Misses> mMisses;
        mutable Telemetry::CacheCounter<Telemetry::CacheCounterId::Writes> mWrites;
    };
}

#endif
