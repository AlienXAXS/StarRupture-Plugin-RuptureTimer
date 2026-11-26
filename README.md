# RuptureTimer

A [StarRupture ModLoader](https://github.com/AlienXAXS/StarRupture-ModLoader) plugin that shows what
the Rupture Wave is doing right now, how long until it changes, and what comes next — as a stylish,
configurable in-game overlay.

- **Current state** — Pre-Wave, Moving, Fadeout, Growback or Calm, with the active substage
  (Fire Wave, Burning, Regrowth, …) and the wave type (Heat / Cold).
- **Time until the next state** — a countdown with a progress ring, and a stage timeline showing
  where you are in the cycle.
- **Next state** — what the wave transitions into.
- **Multiplayer aware** — the server reads the wave and syncs it to every connected client.

When the plugin does not have the data, it says so. It never fills a gap with a guessed number —
unknown countdowns render as `--:--` with the reason spelled out.

## Why it needs a server-side plugin

The game only replicates a coarse slice of the wave state to clients (`ACrWaveTimerActor`, which is
what the vanilla HUD counter reads). That has no Pre-Wave phase, no substages and no sub-stage
progress. The full picture lives on `UCrEnviroWaveSubsystem`, a world subsystem that does not
replicate — and its companion `UCrEnviroWaveTimerSubsystem` is not even created on a client.

So RuptureTimer reads the wave on whichever side has authority and forwards a compact snapshot over
the ModLoader's plugin network channel.

| You are running | Install | Detected as | What the plugin does |
|---|---|---|---|
| Solo / offline | Client DLL | `SOLO` | Reads the wave locally, no networking |
| Listen server (hosting) | Client DLL | `HOST` | Reads the wave **and** broadcasts to clients |
| Joined a server | Client DLL | `CLIENT` | Displays whatever the server sends |
| Dedicated server | Server DLL | `SERVER` | Reads and broadcasts; no UI |

A client connected to a server **without** the server DLL installed will sit in its waiting state.
That is deliberate — see above.

> Both DLLs must be from the **same release**. The wire format is versioned, and a mismatched pair is
> dropped with a warning in the log rather than decoded into nonsense — which looks exactly like "no
> data" on the client.

## How the wave clock works

Worth knowing, because two of its quirks look like plugin bugs and are not.

On the host (single player, or the server) the countdown is
`UCrEnviroWaveTimerSubsystem::NextWaveTimer`: the game sets it to `WaitingDuration` (40 minutes on the
stock config) when a calm window starts, its `Tick` counts it down while `bWaitingForNextWave` is set
and waves are not paused, and the wave starts when it reaches zero. It is saved with the world, so the
countdown carries over a save/load, and it stands still while waves are paused. It is the same value
the wave timer in BetterCheats shows.

`ACrWaveTimerActor::NextTime`, the replicated value the vanilla `WaveTimeCounter` HUD element reads, is
an **absolute world time**, armed as `WaitingDuration + TimeSeconds`. **World time is persisted with
the save** — a long-running world reports `TimeSeconds` in the hundreds of hours — but the calm
deadline is armed at world start, before that time is restored. On a loaded save `NextTime` therefore
sits hours in the past until the first wave event of the session rearms it. The host does not use it
for the calm countdown; a client whose server has no RuptureTimer has nothing else to read and falls
back to it.

**If no wave is running and no countdown is running** (the host is not waiting for a next wave, or on
such a client `NextTime` has passed), the overlay shows **"Overdue · no wave started yet"** rather than
a negative or invented number.

> `NextPhase` has no value for Pre-Wave — `OnWaveStarted` only rearms for Moving, Fadeout and
> Growback. A wave entering Pre-Wave therefore leaves the replicated timer untouched, which is why
> the plugin reads stage and substage from `UCrEnviroWaveSubsystem` instead of inferring them from
> `NextPhase`.

## Installation

Requires [StarRupture-ModLoader](https://github.com/AlienXAXS/StarRupture-ModLoader) v1.23.0 or later.

Copy the matching DLL into the game's `Plugins/` directory next to `dwmapi.dll`:

- Players and listen-server hosts: `RuptureTimer-Client.dll` → rename to `RuptureTimer.dll`
- Dedicated servers: `RuptureTimer-Server.dll` → rename to `RuptureTimer.dll`

The release ZIPs already contain a correctly named `Plugins\RuptureTimer.dll` plus the sidecar that
wires up auto-updates, so installing from the ZIP is easier.

On first run `AxRuptureTimer.ini` is generated in `<game_dir>/Plugins/config/`.

## Configuration

Edit `Plugins/config/AxRuptureTimer.ini`, or use the ModLoader config tab — changes apply live, no
restart needed. Ranged values render as sliders.

| Section | Key | Default | Purpose |
|---|---|---|---|
| General | `Enabled` | `true` | Master switch |
| General | `ShowOverlay` | `true` | Show the on-screen readout |
| General | `ToggleKey` | `F7` | Show/hide the overlay |
| Display | `ShowStageTimeline` | `true` | The Pre-Wave → Calm track |
| Display | `ShowProgressRing` | `true` | Countdown ring |
| Display | `ShowNextState` | `true` | The `NEXT ▸` line |
| Display | `ShowSubstage` | `true` | Fire Wave / Burning / Regrowth / … |
| Display | `ShowWavePosition` | `false` | Wave's map position while moving |
| Display | `ShowNetworkStatus` | `true` | Role badge and freshness dot |
| Display | `ShowTimeSinceWave` | `false` | How long the current wave has run |
| Display | `CompactMode` | `false` | Countdown only |
| Display | `AccentByWaveType` | `true` | Ember for Heat, ice for Cold |
| Display | `Scale` | `1.0` | Size multiplier (0.6 – 2.0) |
| Display | `Opacity` | `0.88` | Background opacity (0.15 – 1.0) |
| Display | `AccentPreset` | `0` | 0 Signal, 1 Amber, 2 Violet, 3 Mono |
| Network | `BroadcastIntervalSeconds` | `1.0` | Server snapshot rate (0.25 – 10) |
| Network | `StaleAfterSeconds` | `5.0` | Mark data stale after this quiet period (2 – 30) |
| Network | `CountdownInterpolation` | `true` | Tick the countdown locally between packets |
| Advanced | `VerboseLogging` | `false` | Log wave transitions and packet traffic |
| Advanced | `ShowDiagnosticsWindow` | `false` | Show the raw diagnostics window |

Turning `CountdownInterpolation` off is the strictest mode: the countdown then only ever shows what
the server actually sent, so it visibly steps once per broadcast interval.

The overlay is draggable. A mouse cursor only exists while the ModLoader window is open, so open it
(`F2`), drag the overlay where you want it, then close it again. The position is remembered between
sessions.

## Diagnostics window

The `ShowDiagnosticsWindow` checkbox opens a window showing the raw snapshot, both countdown sources
side by side, the replicated `NextTime` / `NextPhase` values, packet counters and data age. If the
two sources ever disagree it says so, and the replicated value wins.

It is a **widget**, not a ModLoader config-tab panel: an open panel counts towards the loader's
exclusive input capture, which freezes the game underneath. As a widget it stays on screen while you
keep playing, so you can watch the numbers against a live wave. To click its buttons or scroll it,
open the ModLoader window (`F2`) — that is what puts a cursor on screen.

On a client the raw values shown are the ones the **server** read, not local defaults — they are
carried in the packet specifically so a remote session can be diagnosed.

### Debug builds only

A **Dump wave objects to log** button appears in Debug builds. It walks all of GObjects and logs
every wave-related object with its reflected properties, offsets and live values — the tool used to
confirm that the vanilla HUD widget reads the same `NextTime` this plugin does. It costs a ~100 MB
transient allocation and a visible hitch, so it is compiled out of Release entirely (see
`RUPTURETIMER_DEBUG_TOOLS` in [`src/debug/object_dumper.h`](src/debug/object_dumper.h)).

## Building

```bat
msbuild RuptureTimer.sln /p:Configuration="Client Release" /p:Platform=x64
```

```bat
msbuild RuptureTimer.sln /p:Configuration="Server Release" /p:Platform=x64
```

Output lands at `build/<Configuration>/Plugins/RuptureTimer.dll`.

### Requirements

- Visual Studio 2022 (MSVC v143, C++20)
- [StarRupture-Game-SDK](https://github.com/AlienXAXS/StarRupture-Game-SDK)
- [StarRupture-Plugin-SDK](https://github.com/AlienXAXS/StarRupture-Plugin-SDK)

Both SDKs are consumed from **sibling checkouts** — they are not vendored into this repo:

```
GitHub/
├── StarRupture-Game-SDK/
├── StarRupture-Plugin-SDK/
└── StarRupture-Plugin-RuptureTimer/   <- this repo
```

Adjust [`Shared.props`](Shared.props) if your layout differs, or override on the command line with
`/p:GameSDKRoot=... /p:PluginSDKInclude=...`.

## Layout

| Path | Role |
|---|---|
| [`src/plugin.cpp`](src/plugin.cpp) | Exports, lifecycle, world activation, keybind, config wiring |
| [`src/config/`](src/config) | Config schema and double-buffered typed accessors |
| [`src/wave/`](src/wave) | Snapshot type, stage/duration model, authority-side game reads |
| [`src/net/`](src/net) | Wire format, role detection, broadcast and staleness tracking |
| [`src/ui/`](src/ui) | Theme, draw-list primitives, overlay widget, diagnostics window |
| [`src/debug/`](src/debug) | GObjects/reflection dumper — **Debug builds only**, stubbed out of Release |
| [`Shared.props`](Shared.props) | SDK paths, per-configuration defines, CI build tag |
| [`.github/workflows/release.yml`](.github/workflows/release.yml) | Builds both variants into one release |

The game is only ever touched from the game thread; the overlay renders from a snapshot copy, so the
render thread never calls into `ProcessEvent`.

## Releasing

Run the **Build and Release** workflow (`workflow_dispatch`). It builds both `Client Release` and
`Server Release`, tags the next minor version, and publishes both DLLs, both update manifests and
both ZIPs in a single release. A `version` file at the repo root overrides the auto-increment when it
contains a strictly higher semver.

## API reference

The full plugin API — hooks, config system, pattern scanner, ImGui, networking — is documented in
[PluginDevelopment.md](https://github.com/AlienXAXS/StarRupture-Plugin-SDK/blob/main/PluginDevelopment.md)
in the Plugin SDK repo, which is the source of truth for `plugin_interface.h`.

## Reporting Bugs

Found a bug? [Report it on GitHub](https://github.com/AlienXAXS/StarRupture-Plugin-RuptureTimer/issues) and attach your `ModLoader.log` (in `<game>\StarRupture\Binaries\Win64\ModLoader\Logs\`).

## Credits

Original mod by [@AlienXAXS](https://github.com/AlienXAXS).

Maintained by [@msantoro12](https://github.com/msantoro12).
