# V4.0 CP1A — RenderWorld relationship invariants

Status: **DESIGN ADDENDUM — LOCKED BEFORE UPDATE-BATCH PUBLICATION**

Authority: `V4-CP1-NEUTRAL-RENDER-CONTRACT.md`, `V4-CP1A-ARCHITECTURE-AUDIT.md`, and `V4-CP1A-SOURCE-SEAM-INVENTORY.md` remain authoritative. This addendum narrows internal consistency rules discovered while auditing the first header/test-only RenderWorld foundation.

## 1. Finding

The initial foundation represents both directions of individual object/chunk association:

- `InstanceRecord::chunk` stores the instance's owning `ChunkHandle`;
- `ChunkRecord::members` stores `InstanceHandle` membership.

If both are exposed as independently writable persistent state, they can diverge. A committed instance can point at a chunk without entering `ChunkRecord::members`; retiring an instance can leave a stale member; retiring a chunk can leave a live instance pointing at a retired handle.

The same general issue exists for resource references: an instance may hold a live `MeshHandle`, `MaterialHandle`, or `SkeletonHandle`, so retiring one of those resources independently can leave a dangling logical reference even though the instance itself remains live.

That state shape is acceptable only as temporary scaffolding. It must not become the public batch/publisher contract.

## 2. Single semantic ownership rule

For individually addressable `InstanceRecord` objects, `InstanceRecord::chunk` is the canonical semantic association.

`ChunkRecord::members` is a derived ordered index/read convenience for individually addressable instances, not an independently writable source of truth.

Consequences:

1. producer operations express create/reparent/retire of the instance association once;
2. the RenderWorld publisher maintains any stored chunk membership index from that same operation;
3. no public update operation may separately mutate the instance's chunk and the chunk's member list;
4. read views must never expose a revision where the two representations disagree.

This is the same "one semantic write" rule already locked for neutral state versus the legacy OSG adapter, applied inside RenderWorld itself.

## 3. Publication atomicity

When update batches arrive, relationship changes are validated against the pre-batch world and then committed as one publication revision.

Required invariants at the published boundary:

- every live `InstanceRecord::chunk`, when present, resolves to a live matching-generation `ChunkHandle`;
- every individually addressable instance listed in a chunk-derived membership index resolves live and points back to that chunk;
- one individually addressable instance appears in at most one chunk membership index;
- reparenting removes the old derived membership and establishes the new one in the same publication;
- retiring an instance removes its derived membership before the revision becomes visible;
- retiring a chunk is rejected while live individually addressable members remain unless the same ordered batch first reparents or retires them;
- stale-generation relationship operations fail closed before mutation.

No renderer read view may observe a half-applied reparent or retirement.

## 4. ObjectPaging distinction

This rule does not force future ObjectPaging/static chunk payloads to become thousands of independently addressable `InstanceRecord`s.

The later ObjectPaging producer lane may place compact immutable/static items directly under logical chunk ownership, as allowed by the locked contract. Those compact chunk-local items are a different representation from individually addressable instances and must not create a second writable ownership path for the same logical object.

Therefore:

- first-slice non-paged objects: canonical `InstanceRecord::chunk`, derived chunk membership index;
- future paged/static compact population: chunk-owned compact items or handles, but never simultaneously represented as the same individual logical instance unless a deliberate migration operation changes representation.

## 5. Resource referential integrity

A published RenderWorld revision must not contain a live logical record that refers to a retired or wrong-generation logical resource.

For the current foundation this means at minimum:

- every live `InstanceRecord::mesh` resolves to a live matching-generation `MeshHandle`;
- every `InstanceRecord::materials` entry resolves to a live matching-generation `MaterialHandle`;
- every present `InstanceRecord::skeleton` resolves to a live matching-generation `SkeletonHandle`;
- every present `InstanceRecord::chunk` resolves as specified above.

Resource retirement is therefore an ordered semantic operation, not an unconstrained table erase. A mesh/material/skeleton/chunk that is still referenced by a live instance cannot retire by itself. The batch must first retire or rebind every dependent logical record, then retire the resource in deterministic operation order.

Later relationships follow the same rule when they become explicit: materials to textures, instances to pose/morph state, chunks to compact static resources, and light/environment records to any referenced logical resources.

Backend residency remains independent. Evicting a Vulkan/VSG/OpenGL resident object does **not** retire the logical resource handle and therefore does not participate in this dependency rule.

## 6. API direction before Increment C

Before `RenderWorldUpdateBatch` becomes authoritative, narrow the foundation API so callers cannot bypass relationship invariants.

Preferred direction:

- low-level slot reservation/commit remains implementation-private or test-support only;
- batch/publisher operations own create/reparent/rebind/retire semantics;
- read access remains const/versioned;
- resource-table creation may retain simpler internal helpers, but resource retirement must still obey dependency validation once live references exist.

Do not carry the current direct `commit(InstanceHandle, InstanceRecord)` / unconstrained resource-retire freedom into the public semantic renderer service.

## 7. Required tests added to the Increment C gate

In addition to the existing handle/generation/epoch tests, update-batch publication must prove:

1. instance create establishes chunk-derived membership;
2. instance reparent moves membership exactly once;
3. instance retire removes membership;
4. chunk retire with live member rejects without partial mutation;
5. same batch `reparent-or-retire members -> retire old chunk` succeeds deterministically;
6. stale instance/chunk generations cannot alter membership;
7. referenced mesh/material/skeleton retirement rejects without partial mutation;
8. same ordered batch can rebind/retire dependents and then retire the old resource;
9. stale resource generations cannot satisfy dependency validation;
10. failed relationship or dependency validation leaves the previously published world/revision unchanged;
11. two identical ordered relationship batches produce identical handles, membership order and published revision sequence.

These are correctness gates, not performance mechanisms.
