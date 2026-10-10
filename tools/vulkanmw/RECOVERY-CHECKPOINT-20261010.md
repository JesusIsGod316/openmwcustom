# VulkanMW renderer source recovery checkpoint

This branch preserves the October 10 local worktree, based on
`3993b098613b131ba431d7b2f57320a2c8a2ce60`. It is an archival source checkpoint,
not a release, feature promotion, or claim of newly passing builds/tests.

The original local overlay contains 37 modified tracked files and 16 untracked
source/checkpoint files. No unrelated immersion-mod directory was added.
The existing control paths and original feature defaults are preserved.

The September 30 tested Stage 1 executable has SHA256
`fc5be60b61092400fa70bdb196a62134642252ce0be51b2abb3de946cca92ecc`.
Its historical source manifest does not describe all source on this recovery
branch: nine listed source files changed subsequently, and additional native
GPU-deformation/profile-isolation work is present. The later local executable
has SHA256 `f847a212fbac4a450c2edfd507b12f44db8777e13cdcca93a97882c55981de0a`
and has not been treated as the matched benchmark executable.

Read `STAGE1-PERSISTENT-SCENE-20260930.md` and
`STAGE1-OBJECT-UPDATES-20260930.md` as historical checkpoints. Their source,
correctness and benchmark results must not be transferred to the later work.
The tested matched Stage 1 pair was not a meaningful performance promotion.

Continue the donor integration on common retained interfaces:

1. Precise engine-owned scene/controller publications that remove supported
   objects from recurring compatibility capture, with bounded resource lifetime
   and narrow unsupported-content fallbacks.
2. Shared GPU draw structure, batching, view-specific visibility/LOD/occlusion
   and safely partitioned substantial VSG recording.
3. Persistent actor pose/deformation/attachments that progressively replace
   evaluated OSG actor capture. Validate the later deformation work separately.

Keep OpenMW mod/save behavior, secondary-view visibility and gameplay updates
intact. Do not wholesale-merge another authoritative scene or remove OSG-native
asset support. OptimizedMW OpenGL-specific work is a separate track, not an
optimization layer to carry indiscriminately into Vulkan. Postprocessing is
deferred; two public launchers remain the desired interface.

A private resume kit preserves byte-exact overlays, raw benchmark evidence,
build receipts, profiles, archive/ledger snapshots, and a self-contained Git
history bundle. Original worktrees, game settings and saves were not modified
by creating this recovery branch. Build/GPU/gameplay validation is required
before installing or promoting this current source.
