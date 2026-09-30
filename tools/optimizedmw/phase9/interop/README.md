# Phase 9 standalone GL/Vulkan interop fixture

This opt-in developer executable verifies a bounded same-device shared-image
round trip. It does not enable DLSS, DLAA, scene jitter or any game renderer.
It neither depends on nor modifies the separate VulkanMW world-renderer track.

Configure the Phase 9 native harness with
`-DOPENMW_PHASE9_INTEROP_FIXTURE=ON` and build `p9-gl-vulkan-interop` and
`p9-interop-contract`. On Windows provide Vulkan 1.1-or-newer headers through
`P9_VULKAN_INCLUDE_DIR`; if absent, CMake downloads checksum-pinned official
Vulkan-Headers v1.3.290 into the private build directory. No Vulkan SDK
installation or import library is needed:
the fixture loads the installed `System32/vulkan-1.dll`. OSG 3.6.5 creates the
real GL context; its DLLs must already be on `PATH`, as for the native harness.

Run `Run-InteropFixture.ps1 -Executable <absolute exe path> -RequireSupport` on
the intended game GPU. The wrapper works in PowerShell 5.1 and 7, captures the
executable SHA256 and JSONL evidence, and terminates the child process after
30 seconds by default. Native Vulkan and GL completion waits have their own
1500 ms deadline. A driver API itself cannot be preempted by a native deadline,
so the isolated process is the outer bound. A timeout or pixel mismatch is a
failure. Unsupported configurations exit 77 and CTest marks them skipped;
`-RequireSupport` makes a skip fail the user's hardware gate.

The fixture checks:

- Exactly one GL device, nonzero matching Vulkan device UUID and driver UUID,
  valid matching Windows LUID and a matching single-bit device node mask.
- The exact RGBA8_UNORM, 2D, optimal-tiling, single-sample image with sampled,
  color-attachment and transfer-source/destination usage. Both APIs must support
  the requested optimal tiling; Vulkan must advertise exportability and a
  compatible opaque Win32 memory/semaphore handle type.
- Dedicated allocation on both APIs, offset zero, exported NT handle reference
  ownership, and a complete GL framebuffer backed by the imported image.
- GL writes a sequence-specific, four-quadrant pattern and signals its imported binary
  semaphore. Vulkan waits, acquires external queue ownership, copies every pixel
  to coherent host memory, uploads a different spatial pattern, releases ownership/layout
  and signals the return semaphore. GL waits and verifies every returned pixel,
  including quadrant boundaries in the non-square extent.
- Two independent slots are submitted before either is consumed. A slot cannot
  be reused until the Vulkan fence and GL consumer completion prove retirement.
  Every sequence has a generation token to reject stale consumers. Three
  generations cover 16x16 to 37x19 to 16x16 recreation; all old resources retire
  before replacement. There is no `glFinish`, `vkDeviceWaitIdle`, or production
  synchronization change.

`p9-interop-contract` tests premature reuse, missing GL completion, duplicate
consumers, stale sequence/generation tokens, resize while in flight, retired
resource reuse, all-zero IDs, mismatched UUIDs and invalid node masks. Native
hardware evidence is required in addition to this contract test. Linux compiles
the common contract and a clean unsupported gate, because this fixture currently
implements opaque Win32 handles. A Mesa skip does not prove Windows sharing.

On an unexpected runtime failure, in-flight objects are left to standalone
process reclamation; their destructors never wait indefinitely or destroy GPU
resources without retirement proof. Successful runs delete GL references before
Vulkan allocation/semaphore ownership and delete the device after slot retirement.

This is a narrow interop qualification, not a production bridge: it covers one
format/usage contract and a transfer workload, not NGX, production color/depth/
motion formats, multi-device or context sharing, transparency or dynamic actor
motion. Later consumers must validate their own resource contracts and maintain
the same ownership rules. Pixel success alone carries no FPS claim.

The qualified route explicitly enables the advertised KHR external-memory and
external-semaphore capability extensions on the Vulkan instance and the KHR
external-memory/semaphore plus Win32 extensions on its device. It retains the
exported NT handles until both API consumers retire. Earlier revisions that
closed memory handles immediately after import intermittently returned GL memory
import error 1285; the current route passed the recorded pixel tests. Changes to
extension activation and handle lifetime were not measured in a controlled
driver study, so these observations do not establish their individual causes.
The fixture checks those advertised extensions and skips configurations outside
the qualified route. Evidence is scoped to the recorded driver and exact
resource contract.

The protocol follows the Khronos
[external-object GL extension](https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/main/extensions/EXT/EXT_external_objects.txt),
[Win32 import extension](https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/main/extensions/EXT/EXT_external_objects_win32.txt),
[physical-device identity contract](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceIDProperties.html)
and [Vulkan ownership/synchronization rules](https://docs.vulkan.org/spec/latest/chapters/synchronization.html).
