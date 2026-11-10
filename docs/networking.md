# Networking

## Networking: the deterministic simulation

Networking is being built on one deterministic core. The plan: client prediction against an
authoritative server by default, full-world input rollback as an option, QUIC and Steam
transports, and thousands of players per server. The first part is the core: given the same
inputs, the game simulates **the same bits** on every x64 PC, with any number of threads, in the
editor's JIT and in a shipped NativeAOT game.

**Ticks.** The simulation runs in ticks, the 60 Hz physics steps. Each tick runs in this order:

1. The tick's input.
2. `OnFixedUpdate` on systems, then components.
3. The physics step.
4. That step's `OnCollisionEnter` / `OnTriggerEnter` / `OnTriggerExit` (now delivered per tick, no
   longer once per frame in the after-physics phase).

Game logic that must agree across replays and the network belongs there. `OnUpdate` /
`OnLateUpdate` are presentation: cameras, UI, effects.

Inside a tick:

- `Time.InTick` is true.
- `Time.Tick` counts ticks since the game started.
- `Time.Delta` is `Time.TickDelta` (1/60 s).
- `Time.Total` is `Tick × TickDelta`.
- Input is the tick's:
  - `Input.Pressed` / `Released` and actions' edges compare against the last tick, so a press counts once whatever the frame rate.
  - A tap shorter than a tick still shows as down for the next tick.
  - `Input.MouseDelta` is the motion since the last tick.
- Dynamic bodies' entities hold the step's pose, not the render's interpolated one, from the tick's start until the frame's ticks are done.
- Water buoyancy samples the tick's time (`WaterSystem::setSimTime`), so floating things don't depend on frame times.

**Deterministic math.** These are the float hazards and what the engine does about each:

| Hazard | What the engine does |
|---|---|
| `MathF`/`Math`'s `Sin`, `Cos`, `Atan2`, `Exp`, `Pow`… call the C runtime, whose results depend on the CPU (FMA paths) and the Windows version. | `DMath` (C#) and `fx::dmath` (C++) compute them from `+ - * / sqrt` in double, within ~2e-16 of the C runtime. Engine code on the simulation's path uses `fx::dmath`: animation's slerps and IK, the character mover's slope, water and ocean sampling. `falcon dmath-hash` and the Sandbox's log both give `38d7ca740fd695b0`, the same for MSVC, the JIT and NativeAOT. |
| `System.Numerics` code gives different bits with AVX on (measured: the same `Vector3` loop hashes differently). | The engine starts the JIT with `DOTNET_EnableAVX=0`, and NativeAOT games build with `IlcInstructionSet=x86-64-v2` (`Falcon.Game.targets`). The editor and shipped games then compute alike. |
| Floating-point control state. | The C runtime's FMA3 paths are off, and the main and job threads' SSE control word is reset (rounding, no flush-to-zero). |
| Randomness. | `Sim.Random` (`Float`, `Range`, `Chance`, `InsideUnitSphere`…) depends only on `Sim.Seed`, the tick and how many numbers were drawn before in it. A tick run again draws the same numbers. |
| Non-deterministic calls in game code. | Analyzer **FALCON100** warns about `MathF`/`Math` transcendentals, `Quaternion.Slerp`/`CreateFrom*`, `System.Random` and the clocks (`DateTime.Now`, `Stopwatch`, `Environment.TickCount`) inside `OnFixedUpdate` and the collision callbacks. It suggests the replacement. |

**Recording and replaying.** `--sim-record run.fxsim` saves:

- each frame's length;
- the input the scripts saw (keys, mouse, gamepads, text);
- each tick's hashes: Box3D's body state, and every entity's transform;
- each frame's transform hash.

`--sim-replay run.fxsim` plays those frames and that input back, and compares every tick. The first
difference is logged with its tick, frame and what differed (physics bodies, entity transforms, or
only after the frame: animation, `OnUpdate`), and the run exits with code 1.
`--sim-input-demo <seed>` plays a synthetic player instead of the real input: keys, mouse and
frame times from 4 to 40 ms. Tests use it.

| Test | What it checks |
|---|---|
| `sim_record` | The Sandbox with 256 falling bodies, 600 frames recorded (~790 ticks). |
| `sim_replay_jit`, `sim_replay_1thread`, `sim_replay_aot` | The recording replays identically with the default threads, one thread and the NativeAOT build. |
| `sim_replay_detects` | A different run (300 bodies) fails at tick 1. |
| `sim_dmath_native`, `sim_dmath_jit`, `sim_dmath_aot` | The math's bits. |
| `editor_script_ticks` | Per-tick input edges (including a one-frame tap), `Time` in ticks, collisions inside their tick. |

**Networked objects and rollback (API v37).** The networked simulation is made of
`NetworkObject`s (Add Component > Network Object in the editor, or `AddComponent<NetworkObject>()`).

- **What an object's state is:**
  - its transform (unless `SyncTransform` is off);
  - its components' **`[Sync]`** fields and properties. Supported types: bool, byte, int, uint, long, ulong, float, double, enums, string, Vector2/3/4, Quaternion, Entity. The source generator writes the code, with no reflection. **FALCON003** warns about members it can't sync: private, readonly, or another type.
- **Each object has a `NetId`** that names it on every machine. Ids are given in the order objects wake: the scene's in scene order, then in the order the simulation spawns them. `Network.Find(id)`, `Network.Of(entity)` and `Network.Objects` (in NetId order) look them up.
- **`Network.Destroy(entity)`** only puts an object to sleep (`Dormant`, disabled) while a rollback could still need it, and destroys it two windows later.
- **`CharacterMover`** is a built-in component: a character moved in ticks. `Move(wishVelocity, jump)` once per tick accelerates (with air control), applies gravity and jumping, then slides, climbs steps and keeps to slopes through Box3D's mover. Its `Velocity` and `Grounded` are `[Sync]`, so a rollback restores them.
- **Ticks are self-contained,** so a tick runs the same whatever frames it falls in:
  - components queued since the last tick (a spawned prefab's) are made first;
  - whether one runs is its own and its entity's enabled state at that moment;
  - one that hasn't started starts there (`OnStart` before its first `OnFixedUpdate`).
- **The history:** when the simulation keeps one (`SimHistory`; FxManagedApi `simControl`), each tick's state is saved after its step and events. A rollback restores the state after an earlier tick:
  - objects spawned since are destroyed;
  - dormant ones are woken;
  - transforms and `[Sync]` members are put back.

  It then simulates the ticks after it again, with the input each tick had (`ScriptHost::rollback`). Only the networked objects' components run then, and the physics world isn't stepped. That is the authoritative mode's prediction: predicted logic moves with movers and queries, and doesn't rely on collision callbacks.
- **Checking a re-run tick:** each tick simulated again is compared with the first time's. The first difference is logged with the object, its name, and which component's `[Sync]` members or transform differed.
- **Not restored:** state a component keeps outside `[Sync]` members is its own.
- **`--sim-rollback K`:** every frame, the game is rolled back K ticks and simulated forward again. It exits with code 1 if any tick comes out differently.
- **Arena** (`managed/Arena`, `bin/scripts/arena/{jit,aot}/Arena.dll`) is the networked sample game:
  - six players with CharacterMovers (player 0 on the keyboard and mouse, the others bots choosing with `Sim.Random`);
  - `[Sync]` health, score, aim and cooldowns;
  - projectiles spawned and destroyed in ticks, hitting players.
- **Tests:**

| Test | What it checks |
|---|---|
| `sim_rollback_arena` | 1,200 frames of the synthetic player, rolled back 8 ticks every frame: about 9,500 ticks simulated again, all identical. |
| `sim_rollback_arena_aot` | That recording replays identically on NativeAOT while rolling back 12 ticks every frame. |

**Transports (`engine/net`, the `falcon_net` library).** A transport carries connections, each with:

- reliable messages, on 4 channels, each channel ordered, up to 64 MB each;
- unreliable datagrams, about 1,200 bytes each.

Everything is polled from the game's thread. Addresses name the transport:

| Address | Transport |
|---|---|
| `quic://host:port[#sha256]` | **QUIC**, through Microsoft's MsQuic 2.6.3 (MIT, its OpenSSL build). |
| `loopback://port` | In process: a listen server's own player, tests. |

**QUIC** (MsQuic is fetched by `scripts/fetch-msquic.ps1` into `external/msquic`):

- `msquic.dll` is loaded at run time from `bin/net`. A build or machine without it runs without `quic://`.
- Reliable channels are one unidirectional stream per channel and direction. Unreliable messages are QUIC datagrams (RFC 9221).
- ALPN `falcon/1`. Keep-alive every 2 s, idle timeout 10 s.
- **The server's certificate:** the server makes itself a self-signed one at start, a fresh ECDSA P-256 key.
  - It is made with BCrypt alone, the X.509 structure written by the engine. No Windows key store or certificate store is touched, and nothing prompts (Windows' certificate APIs can ask for a smart card).
  - Its SHA-256 is logged. A client can pin it (`quic://1.2.3.4:27015#<sha256>`): another certificate is then refused.
  - Unpinned clients rely on the session's own authentication.

**The conditioner** simulates network conditions around any transport: one-way latency, jitter, datagram loss and duplication, and a bandwidth cap (`"latency,jitter,loss[,duplicate[,kbps]]"`).

- Reliable messages are only delayed, in order per channel.

**Tests** (`falcon_net_test`, two endpoints in one process):

| Test | What it checks |
|---|---|
| `net_loopback` | Ordered channels, a 4 MB message, datagrams, disconnects. |
| `net_conditioner` | 40 ms latency, 10% loss, 5% doubles. |
| `net_quic` | On localhost: a 10 MB message beside 2,000 small ones on three channels, 500 datagrams, the server answering. |
| `net_quic_pinned` | The pinned certificate accepted, a wrong pin refused. |
| `net_quic_loss` | 20 ms and 10% loss: about 900 of 1,000 datagrams, every reliable message. |
| `net_quic_reconnect` | Connecting again after a disconnect. |
| `net_quic_idle` | A silent connection times out; keep-alive holds one open. |

0-RTT reconnects are not done yet.

**Sessions and replication (API v38, `Network/Session.cs`).**

**Starting a session:**

- In code: `Network.Host(port)` or `Network.Connect("quic://host:port[#sha256]")`.
- On the command line: `--host [[address:]port]` (default 27015, 0: any; with an address such as `127.0.0.1:0` it listens there only, so a local test raises no firewall prompt) or `--connect <address>`, read before the game's `OnStart`. In code: `Network.Host(port, scheme, address)`.
- `Network.Role`, `IsHost`, `IsClient`, `Connections`, `Local`, `HostConnection`.
- The `Connected` and `Disconnected` events, and `INetworkListener` on the Game, systems and components.

**The handshake:** the client sends the protocol and a hash of the game's component classes. A client of another build is refused with the reason. The host welcomes it with its connection id and tick.

**What the host sends each client:**

- **Spawns:** each NetworkObject once, reliably (channel 1), in NetId order: its recipe and its first state.
  - The recipe is the prefab it came from (`Network.Spawn(prefab, position, rotation, owner)`), or its components by class with their `[Property]` values (objects built in code).
  - The client builds the same object under the host's NetId.
  - The scene's own objects already exist there with the same NetIds, so they only take the state.
  - Late joiners get everything that exists.
- **Despawns:** when an object goes, reliably.
- **Snapshots:**
  - At `SendRate` (30/s by default), the state of each object that changed since the client last acknowledged it, in datagrams that fit the path.
  - An object's state is encoded once a send for all clients (`ObjectState`: its transform and its `[Sync]` members). Each datagram is acknowledged; what a lost datagram carried goes again.
  - A state too big for a datagram goes reliably.
  - Entity references (in `[Sync]` members and RPC arguments) travel as NetIds.

**On clients, the host's objects are proxies:**

- Their components don't run `OnFixedUpdate`; the host's simulation is the one.
- Their `[Sync]` members take the host's values as they come.
- Their transforms are interpolated `InterpolationDelay` (0.1 s) behind the newest host tick heard of.
- A client's own objects get NetIds of their own (from 2^62) and aren't sent.

**RPCs:** `[Rpc.Broadcast]`, `[Rpc.Host]` and `[Rpc.Owner]` on a component's methods (`Reliable`, `AnyCaller`).

- The source generator intercepts every call to one (C# interceptors, enabled for `Falcon.Generated`), so it is called like any method.
- The arguments are sent to where it runs, then it runs here too when it should:
  - a broadcast runs everywhere, a client's going through the host;
  - a host RPC runs on the host;
  - an owner RPC runs on the object's owner.
- The host only takes a client's call when the client owns the object, unless `AnyCaller`.
- While one runs from the network, `Rpc.Caller` is who called it.
- **FALCON004** warns about methods that can't be RPCs: static, non-void, private, ref parameters, or unsendable types.

**Testing many machines on one:**

- `--net-clients N`: the host starts N processes of the same game connecting to it (hidden, with `--net-child`: each ends when the host goes), and its run fails with theirs. The tests host on `127.0.0.1:0`, as does the editor's Play with clients.
- `--net-sim latency,jitter,loss` applies to every machine.
- `--net-verify`: each client reports hashes of the states it holds, and the host checks them against its own history of changes.

| Test | What it checks |
|---|---|
| `net_session_jit`, `net_session_aot` | An Arena host with two late-joining clients, everyone at 40 ms ±10 and 10% loss. About 1,000 states checked, all identical. |
| `net_session_rpc` | The host's unreliable Hit broadcast heard on a client. |

**Prediction (API v39, `Network/Prediction.cs`).** A component driven by a player's input implements
`IPredicted<TInput>` (TInput: any unmanaged struct).

- **`BuildInput(ref input)`** runs each tick where the object's owner is (`NetworkObject.Owner`: a client, or the host for -1). It reads the keyboard, the mouse or an AI's choice.
- **`Simulate(in input)`** runs with that input:
  - on the host;
  - on the owning client, as a prediction (`Network.IsPredicting`: don't spawn networked objects or send RPCs there).
- Without a session it simply runs BuildInput, then Simulate.

**How a client predicts:**

- **Inputs:** it sends each tick's inputs to the host in datagrams, every one also carrying the 7 before it, so lost packets cost nothing. The host keeps them by tick and repeats a client's last input when one is missing.
- **Keeping ahead:** the client measures its round trip and the host's tick with pings the host echoes. It keeps its own ticks half a round trip plus 3 ticks ahead of the host's, so its inputs arrive in time.
  - **Late inputs:** each pong also says how early the client's inputs came lately (the fewest ticks ahead of the tick that used them). Busy frames on either side (a host running several ticks a frame, a client sending several ticks' inputs at once) eat into that, so the client's lead grows at once when they came with fewer than 2 ticks to spare, up to 20 more, and shrinks slowly while there is room (`Network.InputLead`).
  - **Small drift:** its simulation runs a little faster or slower (time dilation, 0.9 to 1.1×; `simSetRate`).
  - **Large gaps:** at the start, after a stall, or whenever its ticks fall behind the point where its inputs would come late, its tick number jumps once (`simSetTick`). Its inputs for the ticks from there on start again: the host drops those it had for the old ones (input datagrams carry the jump count, protocol 7).
- **Checking:** the host's state for the client's own object at tick T is compared with the state the client predicted for T.
  - Deterministic code predicts it exactly.
  - Otherwise (another player's hit, a lost input) the client takes the host's state and simulates the later ticks again with their inputs.
  - The first correction is logged with what differed.
- **The numbers:** `Network.PredictedTicks`, `PredictionChecks`, `Corrections`, `TickJumps`, `RoundTrip`; on the host, `InputsUsed` and `InputsMissed`.

**Lag compensation:**

- A built-in **`Hitbox`** component (a capsule) is recorded on the host every tick for a second.
- **`LagCompensation.Raycast`**, while the host simulates a client's input (`Network.SimulatingFor`), tests the hitboxes where that client saw them: the host tick it was showing, sent with its inputs. Elsewhere it tests them where they are.
- Arena's right mouse button fires a lag-compensated rail.

**Arena:** each client that joins gets a player of its own, which it predicts. Its synthetic player then drives it; in a session the synthetic input keeps real frame times.

| Test | What it checks |
|---|---|
| `net_predict` | Two predicting clients at a 150 ms round trip with jitter and 5% loss. About 440 predictions checked per client, about 4% corrected (the start, hits by others). A client fails with fewer than 50 checks or more than 1 in 10 corrected. |

**The dedicated server (`--server`).** The game with no window, GPU device, browser or audio device.

**What runs:**

- the scene's entities, from the pak, read on the CPU and never uploaded;
- physics, water, terrain, animation and bones;
- particles, decals, prefabs;
- the game's scripts.

**How it runs:**

- **Pacing:** 60 ticks a second, sleeping between frames.
- **Hosting:** it hosts a session on port 27015 unless `--host` gives another.
- **Terrain:** tiles and their collision load around every networked character (a NetworkObject with a CharacterMover). The game reports them each frame (`simFocus`, API v40), and clients' and single-player views stream around them too. A server has no camera to stream around.
- **Limits:** `--max-players N` refuses clients beyond N. `--password` makes clients give the same one (clients pass `--password` too; `Network.MaxConnections`, `Network.Password`).
- **Console:** lines typed on its standard input are commands:
  - `status`: the players with their ping, loss and traffic;
  - `kick <id> [why]`;
  - `say <text>`: shown on every client, `Network.Message`;
  - `quit`;
  - anything else goes to the game: `Network.Command`.
- **Build Game** also writes `<Name> Server.cmd` beside the game: the same executable with `--server`.

| Test | What it checks |
|---|---|
| `net_server` | A server with two verified client processes. |
| `net_server_full` | A one-player server refusing the second. |

**Thousands of players.** One server holds 1,000 players in one world. The engine's part of a send costs about 30 ms at 1,000 players.

**What a client is sent:**

- **Interest:** an object with a `RelevanceRadius` reaches a client within that distance of one of the client's own objects. It stays until it is 1.2 times that far away. The host finds them through a grid of 32 m cells, rebuilt once a send. Objects without a radius go to everyone, and a client always gets its own.
- **Budget:** `Network.ClientBandwidth` (bits a second, default 512,000) covers each client's spawns and snapshots:
  - Spawns go nearest cells first, up to three quarters of a send's share. The rest arrive at later sends.
  - Changed objects go by priority: the client's own first, then the ones that have waited longest. Objects that don't fit wait, and their priority grows.
  - Everyone's objects, a client's own objects, despawns and RPCs always go.
- **Encoded once:** each object's state is encoded once a send, for every client. Spawns refer to recipes, a prefab or components with their properties, sent to each client once and then named by hash (protocol 5).
- **Fewer messages:** a send's reliable messages (spawns, despawns) go to each client as one batch.
- **Parallel:** with 8 clients or more, each client's packets are built on the thread pool. Sending stays on the game thread.

**For game code:**

- **`Network.Overlap(centre, radius, results)`** finds the networked objects near a point, in NetId order. It is cheap enough for thousands of queries a tick, because it uses a grid built at the tick's first query. An object that moved more than a metre since then, or was spawned since, is found from the next tick on.
- **Arena's projectiles** look for players this way. Before, each one checked every object every tick: about 300 ms a tick at 1,000 players. Now it takes about 6 ms.

**QUIC threading:** MsQuic is called only on the game thread. Its callbacks only set flags and queue events, and a connection's handle is closed when the game polls its disconnect. (A lock shared with the callbacks deadlocked a server when 200 bots left at once.)

**Bots: `--bots N --connect <address>`.** One headless process runs N clients over QUIC:

- each does the full handshake;
- the game's own `BuildInput` gives them input;
- they acknowledge snapshots but don't simulate.

Several bot processes can join one server. `--bots N --net-clients 1` on a server starts one bot process itself.

**Server statistics:** the server logs its stats every 5 seconds; `Network.ServerStats` holds the latest. They show:

- how long encoding, clients' packets and sending took;
- traffic out, split into spawns, snapshots and other;
- objects per player;
- the game code's time per tick.

**Measured on the development machine,** with Arena in Sponza: a 60 Hz server on QUIC, and bots in 5 processes of 200 on the same machine, all within the courtyard.

| Players | Objects | A send (encode / clients / send) | Out a player | Objects a player | Game code a tick |
|---|---|---|---|---|---|
| 150 (`net_load`) | ~650 | 2 / 1 / 0.4 ms | ~450–515 kbit/s | ~270 | ~5–10 ms |
| 1,000 | ~4,300 | 12–25 / 5–18 / 5–14 ms | ~330–515 kbit/s (spawns ~half) | ~250–400 | ~5–10 ms |

Arena is a stress test, not a typical game. Every bot fires five shots a second inside a ~60 m courtyard, so a few hundred objects are within reach of every player. A game spread over a larger map sends far less. Lower `ClientBandwidth` to trade update rate for traffic.

| Test | What it checks |
|---|---|
| `net_load` | A server with 150 bots in one process: the server's stats show all 150 playing and being sent objects. |

**Not on a server yet:** scatter collision (it comes from the GPU), scene changes for every machine, and a Linux build.

**Not done yet:**

- Owner-written `[Sync(FromOwner)]` members, `[Change]` callbacks, smoothing of visual corrections.
- The Desert soldier as a networked player.
- Scene changes for every machine.
- Sharding across several servers (NetIds and connections leave room for it).
- `[Sync]` members marked in the Inspector.

**In the editor.**

**Project Settings > Network**, saved with the project (`"network"` in `project.falcon`):

- A preset sets the rest:

  | Preset | Players | Snapshots a second | Budget a client | Other |
  |---|---|---|---|---|
  | Competitive | 64 | 30 | 512 kbit/s | |
  | Large battle | 1,000 | 20 | 256 kbit/s | |
  | MMO / survival | 2,000 | 10 | 128 kbit/s | |
  | Rollback | 8 | | | the whole world from inputs |

  Changing any value makes it Custom.
- **Fields:** the mode, snapshots a second, bandwidth a client, max players, interpolation delay, rollback mode's input delay and window, and the default port.
- **Where they apply:** the game gets them at its start, in the launch JSON's `"project"`. That holds in the editor and in a built game alike. Command-line flags win over them.

**Play with clients:**

- The Play bar's **Clients** (alone, or +1 to +4) has Play start client processes of the scene as saved. The editor's own game hosts in the project's mode, and the clients join.
- The clients are windowed, or hidden when the editor is. They play under the chosen **Conditions**: no lag, LAN (5 ms), internet (50 ms, 1% loss) or bad (125 ms, 5% loss), each way.
- Stop ends them.

**The Network tab** (beside the Profiler) shows the running game's session four times a second:

- its role, mode, tick, port and objects;
- each player's ping, loss, traffic and SteamID;
- the host's send timings and traffic;
- a client's predictions and corrections;
- rollback's rollbacks, waits, late inputs, hash checks and desyncs.

The game reports it through `Network.Status()`, a JSON string, which the API v45 export `netStatus` hands to the editor.

| Test | What it checks |
|---|---|
| `editor_network` | A preset sets the rest and a change makes it Custom; Play with a client: it joins, and the Network tab shows the host with one player; Stop ends it. |

**Rollback mode: the whole world from inputs (`Network.Mode = Rollback`, `--net-mode rollback`).** Every machine simulates everything, physics included, from every player's input. This is the GGPO-style mode, for 2 to 8 players: fighting games, RTS, physics puzzles. Bandwidth is tiny, only inputs are sent, and there is nothing to replicate.

**Inputs:**

- A player's input for a tick is built `Network.InputDelay` ticks earlier (default 2) and sent to the host, which passes it on to everyone. Each datagram also repeats the last 8 ticks' inputs.
- A tick whose remote inputs haven't arrived runs with each player's last input repeated: a guess.
- When the real input differs from the guess, the world goes back to the tick before it and is simulated again to now. That restores the scripts' kept state and physics', and re-runs each tick with its inputs.
- A frame rolls back at most once a tick, to the earliest tick that needs it.

**Never too late:**

- A machine never gets further ahead of the inputs it has than a rollback can reach (`RollbackWindow`, default 32 ticks). It runs fewer ticks that frame, or none while waiting (`RollbackStalled`): a hitch, never a desync.
- Clients keep to the host's tick by time dilation only. They never jump, because every tick must be simulated with its inputs.
- They aim using the lowest recent round trip: slow frames would inflate it.

**Desyncs:**

- Every 30 ticks each machine hashes a tick everyone's inputs are in for: the scripts' live state plus physics' state.
- The host compares the others' hashes with its own and logs a desync (`RollbackStats`).
- Dormant objects aren't in the hash: whether one is still listed depends on when it went.

**Starting:** everyone waits, paused, until the host has its players. Those are `--net-clients N`, or `MaxConnections`. Then all start at tick 0 together.

- Each player's objects are made on every machine (`INetworkListener.OnConnected`, in connection order). No objects get client-local ids, and there are no proxies.
- There is no late join yet.

**The rule for game code:** `BuildInput` only reads input. It runs on its player's machine alone, so changing simulation state there desyncs everyone else. Arena's bots now choose in `Simulate`, from `Sim.Random`.

**Physics rollback (`engine/physics/rollback/physics_snapshot.c`):**

- It is a fork of Box3D's world snapshot, built into Box3D's library (with its flags, so its struct layouts) without touching the submodule.
- Geometry is kept by pointer instead of copied and hashed. A snapshot holds a reference on each hull it uses.
- Bodies', shapes' and joints' user data are kept.
- The engine's own body records are kept with it: entity bodies, pools, joints, touching contacts, water.
- **When:** each step's state is kept after the step and its events, where the scripts keep theirs.
- **Restoring** puts both back, then each later step is taken again with its tick's kept input inside it (API v43's flag 2). Re-simulated steps' events are dropped, since they were heard already, and the render's in-between pose is put back.
- **Static collision:** terrain or scatter streaming in drops what was kept, since the kept worlds point at it.
- **Cost:** in Arena (Sponza plus a pile of crates) a kept state is 160 KB, kept in 0.03 ms and restored in 0.06 ms. `--sim-rollback K` now rolls physics back too: 9,544 steps came out identical.
- **API v44:** `simWindow`, `simRollback`, `simPhysicsHash`, `simMaxTicks`. `simSetRate` takes 0 (no ticks).

**Measured:** 3 players (a host and two client processes) at a 100 ms round trip with 2% loss, over 3,000 frames:

- about 850 rollbacks each (about 7 ticks back on average);
- a few waits for inputs;
- no input too late;
- about 100 hashes checked: no desync.

**Cost of game code under rollback:** every rolled-back tick runs the game's code again. In Arena that cost is the character mover:

- A move in Sponza's atrium costs about 0.45 ms: its capsule queries against the scene's mesh take 25–85 µs each.
- The mover now skips a slide pass once a cast covers the whole move, and reuses its last ground check.
- A rollback session's ticks need cheap simulation code.

| Test | What it checks |
|---|---|
| `net_rollback` | Three processes in rollback mode at a 100 ms round trip with 2% loss: rollbacks, no input too late, the host's hash checks with no desync. |
| `sim_rollback_arena`, `sim_rollback_arena_aot` | Now with physics rolled back too (and a pile of crates the shots knock about). |

**Not deterministic yet:**

- Terrain and scatter colliders stream around the view camera. Scatter colliders come from the GPU, a few frames late. Both will follow the players' simulated positions, computed on the CPU, with the dedicated server.
- Particles that push bodies run on frame time. They are applied in entity order, so replays match, but they are presentation and won't push networked bodies.
- The engine's fly camera (no main camera) isn't replayed.
