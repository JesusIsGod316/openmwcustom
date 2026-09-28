OptimizedMW P8G4 - experimental switchable build

START-OptimizedMW-Test.bat is the only test BAT to use.
Extract this complete build into a NEW folder. Extracting over an old build
cannot remove that old folder's BAT files. No user files are automatically deleted.

First compare 1 REFERENCE with 2 COMBINED on the same save and route.
Advanced modes 3 CULL-CPU, 4 LOD2, 5 SHADOW-BATCH isolate each change.
Mode 6 LEGACY-LOGGING renders the REFERENCE configuration with the old logging.
It tests measurement contamination; it is not a different renderer or a new winner.

REFERENCE: P8G3 HIER-CULL with clean capture.
CULL-CPU: traversal-scoped reuse of view transforms and conservative wind inputs.
LOD2: four immutable prefix tiers, shared bounded radius-policy states, near
protection, projected-size protection, full fallback and shader fade agreement.
SHADOW-BATCH: optional prepared opaque static proxies in eligible distant AND
active chunks. Normal visible geometry remains unchanged. Alpha, dynamic,
unsupported state and far cascades with small-feature pruning use original geometry.
Keep 3 cascades, 2048 resolution, 4096 distance, far mode 3 and 5px caster pruning.
No FAST-WIND, order-only, reduced shadow quality or temporal shadow reuse.

The launcher restores settings.cfg and inherited OPENMW environment variables.
New representation settings are startup-only. Do not change LOD/view settings
mid-benchmark. Quit normally; do not kill the process.

Clean capture buffers raw frames and delayed CPU-side OSG statistics in fixed
memory, then formats/writes them after gameplay. It suppresses synchronous OSG
text dumps and rolling summaries. Resource sampling occurs every 60 frames.
Other existing asynchronous diagnostics remain enabled: observer cost is not zero.
An abrupt crash may lose the in-memory frame/render records. Existing crash/log
files are still collected. Missing completion status, dropped records, allocation
failure, or write failure makes a capture incomplete; do not promote from it.
GPU results retain their origin frame and explicit availability. Missing data is
NaN, not zero. The final eight render frames are intentionally not synchronously
queried. Proxy counters measure cull visits, NOT proven driver draw calls.

After exit, the report retains full-capture statistics, separate ordinary <100ms
statistics, >=100ms events, spike clusters and matched-frame auxiliary context.
It does not select the historical matched late-exterior route automatically.
Nested CPU scopes and GPU time overlap: NEVER add them as an exclusive budget.
Periodic other_ms spikes remain an unproven logging hypothesis until compared.

This package is NOT runtime/performance promoted. Visual checks: wind, camera
turns, distant LOD transitions, large plants, cell returns, shadow silhouettes,
cutout foliage, moving actors, water/alternate views and first/third person.
