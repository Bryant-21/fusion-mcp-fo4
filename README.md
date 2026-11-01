# Fusion MCP

![Fusion MCP logo](logo.png)

F4SE plugin that lets local tools drive Fallout 4 over the Model Context Protocol (MCP): console
lines, saves and load barriers, screenshots, references, actors, camera, input and logs. Other mods,
such as Fusion Shaders, can add their own commands through the optional versioned C ABI; none is
required by the host. Transport is off by default and listens on IPv4 loopback only.

| Tool | Behaviour |
| --- | --- |
| `fusionmcp.console.commands` | Read-only native console inventory; flags ambiguous (duplicate) rows and lifecycle commands. |
| `fusionmcp.console.run` | Mutating, async. `{line, waitMs?}`: one line exactly as typed into the console (script statements such as `set gamehour to 12`, `;`-separated statements, `player.`/`<refid>.` prefixes, `load`/`save`/`qqq`/`bat`, every command). The engine runs it on the Game owner before the result is written (`completion: "executed"`); `output` is everything the console printed for that line, engine echo removed. A load line naming a listed save returns `load.transactionId` for `fusionmcp.wait_ready` (`completion: "queued"`: the engine runs it next frame). `waitMs` keeps recording later prints (default 3000 for `bat`). `semanticSuccess: null`. |
| `fusionmcp.console.execute` | Typed form of `console.run`: `{command, target?, arguments?, waitMs?}` assembled into one quoted statement. |
| `fusionmcp.global.get` / `fusionmcp.global.set` | `{global}` / `{global, value}`: a GLOB by EditorID, runtime FormID or FormKey, read or written directly (no script compiler). Constant globals are refused, as the console refuses them. |
| `fusionmcp.time.set` | `{hour}` 0..<24: the Calendar's GameHour global, directly. |
| `fusionmcp.game.readiness` | Read-only single sample of menu/player/cell/3D state. Not a load barrier. |
| `fusionmcp.saves.list` | Saves newest first. |
| `fusionmcp.saves.load` | Mutating. Exact save stem; returns a transaction in phase `dispatched`. |
| `fusionmcp.wait_ready` | Nonblocking barrier: `ready` only after correlated PreLoad/PostLoad success and stable readiness samples in the new epoch; otherwise `failed`/`superseded`/`uncorrelated`/`timeout`/`cancelled`. |
| `fusionmcp.capture.screenshot` | Mutating, async. Default `method:"native"`: D3D11 readback of the frame the game presents, written to `Documents\My Games\Fallout4\FusionMCP\Screenshots`; fails with `frames_not_presented` when no frame is presented (minimized, display off). `method:"engine"`: the game's ScreenShot command (`ScreenShotN.png` in the game folder). Result is the verified PNG (size, dimensions, SHA-256). SDR, not HDR. |
| `fusionmcp.capture.image` | Read-only. Returns a screenshot (default newest native or engine) as an MCP `image` content block: JPEG up to `maxWidth` (default 1280), fitted to ~240 KB base64. `capture:true` takes a fresh native frame first and also returns its path, bytes and SHA-256. |
| `fusionmcp.batch.execute` / `get` / `cancel` | Up to 32 ordered steps, validated at admission; each starts after the previous is terminal. |
| `fusionmcp.forms.resolve`, `fusionmcp.ref.inspect`, `fusionmcp.actor.state`, `fusionmcp.scene.nearby` | Read-only copies of form, reference, actor (health, combat, package, inventory) and nearby-reference state. |
| `fusionmcp.world.spawn` | Mutating, async. Engine `PlaceAtMe`; returns the new references found near the anchor. |
| `fusionmcp.ui.menus`, `fusionmcp.ui.messagebox.answer` | Open menus and the visible message box; answer it by button index or label. |
| `fusionmcp.camera.state` / `mode` / `look` | Camera mode, player yaw/pitch, look-at-reference. |
| `fusionmcp.input.hold` | Mutating, async. Synthetic button events prepended to the engine's own per-frame `PlayerControls` input pass. |
| `fusionmcp.wait.until` | Async condition barrier (world ready, menus, message box, 3D, combat, death, distance). |
| `fusionmcp.anim.variables`, `fusionmcp.anim.watch` | Animation graph variables and recorded graph events of a reference. |
| `fusionmcp.anim.active_clips` | Read-only. Clip generators on the active branch of each behaviour graph of a reference, plus the equipped weapon's 1P/3P graphs: animation file, behaviour file, playback speed, local time. |
| `fusionmcp.node.transform`, `fusionmcp.node.children` | Read-only. One named node of the 1P or 3P 3D (local/world transform, parent, flags, culling, world bound), or a depth-limited subtree. Rotation is NiMatrix3 as stored; FO4 renders it transposed relative to CommonLib's `operator*`. |
| `fusionmcp.actor.equipped_weapon_3d` | Read-only. The equipped weapon's biped 3D in 1P and 3P: biped slot, loaded, attached, parent node, world transform and bound, culled, graph manager. |
| `fusionmcp.actor.weapon_cull_state` | Read-only. Weapon cull counter (raw and decoded), weapon and gun state, AI process weapon-loading queries, `Weapon` / `RArm_Hand` culling in 1P and 3P. |
| `fusionmcp.logs.list` / `read` | Papyrus and F4SE logs; offset reads return only new text. |
| `fusionmcp.renderdoc.status` / `capture` / `captures` | RenderDoc in-application API (needs `renderdoc.dll` loaded before the device: `[RenderDoc] Load = true` here, or F4FS's own loader). `capture` (mutating) triggers a whole-frame capture and is refused while any capture is in progress; `captures` lists every `.rdc` in the process, including F4FS-scoped ones from `f4fs.capture` `action=renderdoc`. |
| `f4fs.feature`, `f4fs.inspect`, `f4fs.shadercache`, `f4fs.capture`, `f4fs.settings` | F4FS remote-control families, routed to the loaded `F4FS.dll`'s versioned `F4FS_RemoteControl_GetAPI` (ABI v1, header in `vendor/f4fs`). Read actions always; `capture` queue/renderdoc, `settings` set/save and `inspect` cancel need `AllowMutations = true`. Without F4FS (or with an older build, or `[RemoteControl] bEnabled = false` in F4FS.toml) they stay listed with `_meta.fusionmcp.available=false` and a `fusionmcp.unavailableReason`. F4FS queues work on its render thread; poll `f4fs.inspect` `kind=request`. |

The native adapter is enabled only on runtimes 1.10.163, 1.11.221 and 1.11.240,
whose ids/RVAs are confirmed in the RE database. The input hook installs only when the
live `PlayerControls` vtable slot matches the expected engine function. A save needing a
missing Creation reports `failed` while the engine shows its prompt.

### Console lines

`fusionmcp.console.run` calls the engine's own console path, never a parser of ours: `Console::ExecuteCommand`
(echo, macros/hotkeys, `;` statement split, `ForEachRef[`, queue) and then `Console::ExecuteQueuedCommand`,
which compiles and runs the line with the console's script compiler (`Script::CompileAndRun`,
`SYSTEM_WINDOW_COMPILER`) against the console's selected reference. Output comes from a patch of the
`ConsoleLog::Print -> ConsoleLog::AddString` call(s), the only path console text takes, active only for the
line being run (and for `waitMs` afterwards). The engine's pending log buffer is not used: the console menu
empties it and it stops growing at 64 KB. A second patch prints `ExecuteCommand`'s echo of the line through
`"%s"`: the engine passes the typed line as a printf format, so `%` used to crash or garble; it is now
ordinary text, for console users too. Each patched call site is checked to hold the expected engine call
before it is changed (log line `FusionMCP console: ...`); without the print patch the log buffer is the
fallback (`outputSource: "log_buffer"`), and without the echo patch lines containing `%` are refused.

| Runtime | ExecuteCommand | ExecuteQueuedCommand | ConsoleLog::AddString | Print call sites patched | Echo call site |
| --- | --- | --- | --- | --- | --- |
| 1.10.163 | og 1061864 (0x125B4A0) | og 675475 (0x125B780) | og 764 (0x1262F50) | 0x1262F1F | 0x125B516 -> PrintLine og 1065300 |
| 1.11.221 | ng 2248537 (0x1035EC0) | ng 2248538 (0x10364F0) | ng 2248593 (0x103BEC0) | 0x103BDD0, 0x103BE8E | 0x1035F49 -> Print ng 2248591 |
| 1.11.240 | ng 2248537 (0x1036250) | ng 2248538 (0x1036880) | ng 2248593 (0x103C250) | 0x103C160, 0x103C21E | 0x10362D9 -> Print ng 2248591 |

Verified in game on 1.11.240 (2026-10-09): `set gamehour to 12` then `GetGlobalValue GameHour` -> `GetGlobalValue >> 12.00`;
`player.GetPos X`, `player.GetPos Y ; player.GetPos Z`, `player.GetAV Health`, `player.GetItemCount 0000000F`, `help iron 4`,
`help "100% [x]" 4`, `bat <missing>` all returned their console text; `save <throwaway>`, then `load <throwaway>` reached
`wait_ready` `ready` with the restored GameHour; `qqq` quit. 1.10.163 and 1.11.221 use the same code with their own call sites,
checked against the binaries in redb but not yet run in game.

What remains refused, and why:

- Control characters (newline, etc.) and lines over 1000 bytes: the console input box cannot hold them and
  the engine copies lines into 1024-byte buffers (with room for macro expansion). Use `;` for statements.
- In `console.execute` only: string arguments containing `"` or `;`, since the console grammar has no escape
  for `"` and `;` would start another statement. `console.run` accepts both.
- `%` only when the echo patch could not be installed (unrecognised build).

Lines that load a save (`load`/`LoadGame`, alone or within `;` statements) are left in the engine's queue
for its own drain on the next frame: F4SE's PreLoad/PostLoad messages, raised inside the engine call,
change the load epoch, which would retire the request reporting the transaction. `qqq` quits the process,
so its request may never report back.

## Connecting an MCP client

For multiple game instances, set `FUSIONMCP_PORT` in each game's launch environment
to a different decimal TCP port (1–65535). It overrides `[MCP] Port` without
editing the shared TOML and is inherited through the F4SE loader. Missing override
keeps the TOML port; an invalid override disables transport and logs the reason.
The override does not enable MCP or change mutation permissions.

Set `FUSIONMCP_DISABLED=1` in the launch environment to keep transport off for that
process, even when the TOML has `Enabled = true`. It is checked before the TOML or port
override. Any present value other than exactly `0` disables transport; `0` leaves
the TOML's enabled setting in control.

Transport is MCP Streamable HTTP (protocol `2025-11-25`; `2025-06-18` and
`2025-03-26` accepted) at `POST /mcp` on `127.0.0.1:<Port>` (plain HTTP). Responses are single
JSON bodies; there is no SSE stream, resources or pagination. The in-game listener
needs `[MCP] Enabled = true`.

There is no token. The listener binds 127.0.0.1 only and guards against browser
attacks without a secret: a `Host` other than `127.0.0.1:<Port>` or
`localhost:<Port>` is refused with 403 (DNS rebinding), any `Origin` other than
`null` is refused with 403 (cross-site pages), and a POST whose `Content-Type` is
not `application/json` is refused with 415 (form/text posts that skip the CORS
preflight). Any local process can still connect, as with any loopback service.

```python
from mcp import ClientSession
from mcp.client.streamable_http import streamablehttp_client

host, port = "127.0.0.1", 8922
async with streamablehttp_client(f"http://{host}:{port}/mcp") as (read, write, _):
    async with ClientSession(read, write) as session:
        await session.initialize()
        admitted = (await session.call_tool("fusionmcp.console.execute",
            {"command": "GetPos", "target": "player", "arguments": ["X"]})).structuredContent
        # Admission is not success: poll the request until a terminal state.
        state = await session.call_tool("fusionmcp.request.get", {"requestId": admitted["result"]["requestId"]})
```

The runtime installs only the F4SE game task owner. Render/UI/VM provider commands
remain unavailable until trusted adapters for those owners exist. Async progress
uses bounded trusted owner wakes independent of client traffic; no owner callback
waits for another thread. Provider disable and unregister can return BUSY until
an owner pump acknowledges pending resource retirement.

The default-off owner wake service starts only alongside explicitly enabled
transport. Disconnects do not stop admitted operations from progressing or timing
out. Shutdown disables admission, marks outstanding work terminal, and keeps owner
retirement wakes alive until provider cancellation acknowledges cleanup. A stalled
provider remains leased and BUSY; stopping the listener never grants DLL unload.

## More

- [Third-party code and credits](THIRD_PARTY.md)
- [AI contributions](AI_CONTRIBUTIONS.md)
