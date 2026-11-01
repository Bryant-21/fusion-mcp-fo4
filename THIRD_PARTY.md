# Source provenance

New standalone provider API/registry, typed schema checks, bootstrap and fixtures
are original project-owned GPL-3.0 code.

`src/core/RequestBroker.*`, `src/core/GameTaskDispatcher.*`,
`src/transport/Protocol.*`, `src/transport/Server.*`, `src/runtime/LegacyQuery.h`,
`res/commonlibf4-plugin.cpp.in`, and original broker/task fixtures were extracted
from original project-owned F4FS source at tested commit
`5ef4c26` in the local `f4fs-settings-bloom-20261006` checkout (docs HEAD `3257ad1`).
Those input files were verified unchanged between the tested commit and docs HEAD
before copying. Namespaces and include paths changed; broker adds independent UI
and VM owner slots; transport uses the independent copied catalog rather than an
F4FS-specific provider or allowlist. Existing owner/lifecycle regressions remain.
No North feature implementation, shaders, assets, SDKs, F4FS settings module,
DevTools implementation, or Tales implementation is copied.

`vendor/nlohmann/json.hpp` and `vendor/nlohmann/LICENSE.MIT` are the approved
unchanged nlohmann/json v3.12.0 dependency from commit
`55f93686c01528224f448c19128836e7df245f72`:

- Header SHA256 `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`.
- License SHA256 `46a65cffd1ea955132d95a8dd921640714a8d6b537d2e4e482d31145ae95b603`.

CommonLibF4 is an explicit external read-only build dependency. This project does
not clone, modify, update, repin or publish any CommonLib submodule. The local
approved source snapshot is supplied with `--commonlib_root`; its license remains
in that checkout. CommonLib supplies only F4SE interfaces here; no RE engine
structures or addresses are accessed by this initial runtime bootstrap.

The Library checkpoint named in `docs/COMMAND_TRANSFER.md` has not been copied
into implementation; its Windows metadata materialization failed. Preserve that
failure evidence and do not claim it is incorporated.

`src/core/FrameCapture.*` and `src/runtime/PresentCapture.*` (native present-time
screenshot) are new project-owned code that follows the capture technique and
format handling of F4FS's Screenshot tool (Bryant-21/fusion-shaders-fo4
PR #88, `src/Screenshot/*`): a single GPU copy into a staging texture at the
present seam, DO_NOT_WAIT Map polling on later frames, row packing (8-bit
RGBA/BGRA to 24-bit, R10G10B10A2 to 48-bit PNG) and WIC encoding on a worker
thread. That tool is derived from Skyrim Community Shaders
(https://github.com/community-shaders/skyrim-community-shaders,
`src/Features/ScreenshotFeature.{h,cpp}` at
`3a6f91a1a9e28ec9bae266b1b1a5cc0c0ed5e589`), licensed GPL-3.0-or-later WITH
Modding Exception AND GPL-3.0 Linking Exception, which this GPL-3.0 project may
incorporate. No F4FS source file is linked or required at runtime.
