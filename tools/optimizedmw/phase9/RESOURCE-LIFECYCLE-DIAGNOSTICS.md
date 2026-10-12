# Resource lifecycle and model stages

These receipts run only in the existing Deep HitchTrace capture. They introduce
no resource-loading switch, cache key, additional payload reference, wait or
cache winner policy. Normal uses the original SceneUtil::CopyOp dynamic type;
diagnostic retirement scans and payload reference-count sampling are gated.
Cache and prepared-pool items carry one integer token, so retrieval does not
scan the registry. The registry allocates at diagnostic startup, retains at
most 65,536 immutable publication identities, and never reuses a token.

`resource.preload_snapshot` records the main-thread immutable model snapshot.
`resource.preparation_origin` is an explicit optional PreloadItem worker guard,
linked to that snapshot. Request contexts name the exact expected cache owner,
VFS object and captured VFS index generation. Nested image or unrelated cache
operations cannot acquire the parent's template/NIF/keyframe identity.

| Receipt | Meaning |
| --- | --- |
| resource.lifecycle_publish | Exact cache-held object or prepared node was published; monotonic lifecycle_id, resource_ptr, VFS generation and source_flags. |
| resource.lifecycle_first_demand | First required manager API retrieval of this publication, once; cache_hit distinguishes retrieval from constructing publication. Links to the publication span. |
| resource.lifecycle_prepare_hit | Explicit optional worker retrieved this exact existing cache publication, once. Links to the preparation guard; publication_span and resource_ptr prove the publication used. |
| resource.lifecycle_invalidation | VFS identity/generation no longer matches. No qualified demand or preparation reuse is credited afterward. Links to the publication. |
| resource.lifecycle_retire | Actual cache/pool owner relinquishment at replacement, expiry, pressure, explicit removal/clear, destruction or prepared-pool consumption. Links to the publication. |
| resource.lifecycle_status | Publications and capacity, allocation and metadata failures; emitted after workers join, before capture finish, even for zero resources. |
| resource.lifecycle_coverage | Still-active publications and explicit unknown origins, generations and observation attempts. |

`source_flags & 3` is publication origin: 0 unclassified speculation, 1 required
API retrieval, 2 explicit optional preparation. Bit 4 means generation unknown
or invalid. Bit 16 means a later exact optional preparation hit was observed;
it does not retroactively identify an unknown creation origin. Invalidation
reason bits are 1 missing VFS, 2 different VFS object, 4 different publication
generation, and 8 request generation changed during retrieval. Status capacity,
allocation or metadata failures make this family's completeness false. Unknown
coverage is reported separately and must never become a guessed expiry/miss.

The identity is cache/pool owner plus exact publication token and resource
pointer, with generation qualification. A filename alone is insufficient.
First demand means API retrieval, not first visible draw, GPU readiness or
first animation playback. Retirement means the named owner released its
reference; external references can keep the payload alive. PoolConsumed
transfers a live node to its demand caller. remaining_refs is sampled before
that ownership change. retained_estimate is the existing speculative charge,
not physical RAM/VRAM freed. Registry entries own no resource references.

Thresholded child scopes split actual original model work into model_graph,
model_geometry_data / model_bs_geometry, model_skin_weights, model_particles,
model_controller_targets, model_lod_cleanup and model_skeleton. Aggregate
model_convert/model_graph counts are successful nodes, geometries, vertices
and primitive indices from the original traversal; no extra graph walk runs.
Template stages split filtering, shared-state handling and bounds. Clone stages
split original copy, controller target remapping, template pin and particle
initialization. Copy workload counts are forwarded node/drawable/callback
requests, including shallow drawable requests, not allocations or copied bytes.

Maintenance scopes separate expiry/pressure/bounded scans, final cache payload
release, prepared-owner release, shared-state pruning, deferred-owner release,
per-manager update and trim, and cell-preload holder release. Owner/entry/work
counts and estimated charges remain category-specific logical units. Timer
cost stays in the measured diagnostic workload. `diagnostic.resource_lifecycle`
includes registry locking and receipt creation; it is never subtracted to
fabricate a gameplay result. Thresholded missing child scopes remain unknown.

Production-linked component fixture filter:
`ResourceLifecycleRegistry.*:ResourceLifecycleProduction.*:ResourceLifecycleFinalStatus.*`.
Launch a separate process with OPENMW_P9_HITCH_SPANS_FILE pointing to a fresh
resource-lifecycle-spans.csv. The final test emits status after all previous
fixture workers and owners end. Small isolated registries suppress receipts
only to keep their test-local tokens/failure injection out of the production
singleton's capture; production cache hooks always use the singleton.
The fixtures test bounded capacity/allocation/metadata failure, pointer reuse,
retired/owner/generation rejection, exact prep-to-demand cache use, concurrent
canonical publication, nested cache isolation, original expiry/pressure/clear
rules, model hierarchy/transform/hash and controller-remapped shallow clones.
The undeclared-speculation fixture intentionally reports one unknown origin;
this is explicit coverage, not transport loss. Run the same filter in Normal
to verify contract behavior (five Deep policy tests skip outside Deep).

Revision 4 also writes `p9-hitch-spans.csv.lifecycle-status.txt` after joined
workers, outside the recorder queue. It preserves exact 64-bit status/coverage
receipt IDs, registry counters, unknown provenance and final output state.
The validator compares it with retained receipts when the CPU timeline is
complete. A valid final registry snapshot survives timeline loss but cannot
restore missing lifetime events or establish exact first-use joins. Normal
emits neither this file nor the timeline. Additional fixtures cover IDs above
2^53, zero resources and unwritable output.
