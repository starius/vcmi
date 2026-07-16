# VGT Event Transcript Plan

This document restarts the readable VCMI Game Transcript work from a text-first direction. It keeps the useful parts of the previous VGT readable format, but rejects binary packet dumps, save snapshots, state checkpoint hashes, record counters, and raw compatibility blobs. The map-file hash is still required because the external map file is part of the transcript input.

The target is a compact YAML transcript that can be read by a human, parsed by normal YAML tooling, and used by a future recorded-game player to reconstruct a game timeline, inspect decisions, scroll through game history, and branch into playable state for any side after replaying the text events.

## Principles

- The file is YAML 1.2, not a custom DSL.
- The file is append-friendly. New turns or world phases can be appended to the end.
- The file contains no binary payloads, no save snapshots, and no state checkpoint hashes.
- The header contains a required hash of the referenced map file, so replay can fail early if the map changed.
- The map-file hash is an input-integrity check, not a replay checkpoint. It is the only required hash in the first version.
- Records use identifiers instead of enum numbers.
- Records use full readable field names. Avoid VGT-specific abbreviations.
- Only material events are recorded. Do not record acknowledgements, duplicate network delivery, timer ticks, internal implementation noise, or random draws that never realize into game state or a decision.
- Consecutive successful one-tile movement requests are written as one `moveHero`
  route. Replay expands the route back into the original one-tile server requests.
  A blocking visit or any other non-success result ends the route and remains explicit.
- Turn documents and `endTurn` actions already express ordinary turn boundaries, so
  duplicate turn-start/end effects are omitted unless they carry a query or timer
  state. Visit-end sentinels, duplicate town-visit notifications, visitor bookkeeping,
  and internal reward-selection flags are also derived implementation state and are
  omitted from the readable transcript.
- Every actor decision is recorded: human, AI, neutral/world, battle AI, query answer, retreat/surrender choice, and scripted choice where applicable.
- Every authoritative material effect is recorded, including effects nobody could see yet, such as neutral growth or week-start spawned monsters in fog.
- In the material event stream, randomness is recorded only as realized facts near the event that consumed it. Internal RNG state is not a timeline event. Replay advances RNG by feeding recorded decisions through normal server logic.
- Countdown timer ticks are not recorded. Timer state is recorded only when a timer forces a gameplay action, or as a compact start/end snapshot on the turn end record for analysis.

## Self-Sufficiency Boundary

The transcript names the map, content set, settings, map game-setting overrides, players, and all events. It does not embed the map, but it must identify the exact map file with a cryptographic hash.

A replay/player tool starts from the declared map and content, verifies the map hash, initializes the game from the declared settings, restores small deterministic initialization state such as the map object-name counter, starts the normal server flow, then compiles VGT decision records back into server requests. Effect records are audit/readability records; they are checked against recomputed server results when a checker supports that comparison, but they are not the replay source of truth. For generated random maps, the writer must save the generated map as a normal map file, hash that saved file, and reference it from the transcript. The transcript may also record the random-map generator options preserved by normal save files, but replay still loads the saved map file instead of generating a new one.

This keeps the transcript readable while detecting the most dangerous external input drift: the map file. Mod/content hashing can be added later if needed, but it is not required for the first implementation.

## YAML Stream Shape

Use a YAML multi-document stream. The header is the first document. Each following document is a turn, world phase, or battle-only phase. This is easier to append than a single large top-level list.

```yaml
vgt: 4
format: VCMI readable event transcript
engine: { version: "1.8.0", build: "develop" }
map:
  uri: "Maps/RandomMaps/trace_giant_8ai.vmap"
  name: "Coldshadow's Fantasy"
  source: generated-map-file
  hash: { algorithm: sha256, value: "8db3480a8b6f7e7a8c3d2a17f8c1b0c77a2b8b7c4f7f4ce0f1b5b83e7a3e0000" }
  objectNameCounter: 5576
  generator: { width: 252, height: 252, levels: 2, humanOrComputerPlayers: 8, teams: 0, computerOnlyPlayers: 0, computerOnlyTeams: 0, water: normal, monsters: normal, template: "8XM12", roads: ["core:dirtRoad", "core:gravelRoad", "core:cobblestoneRoad"], players: { red: { type: ai, faction: random, hero: random, team: 0 }, blue: { type: ai, faction: random, hero: random, team: 1 } } }
  initialGenerator: { width: 252, height: 252, levels: 2, humanOrComputerPlayers: 8, teams: 0, computerOnlyPlayers: 0, computerOnlyTeams: 0, water: normal, monsters: normal, template: "8XM12", roads: ["core:dirtRoad", "core:gravelRoad", "core:cobblestoneRoad"], players: { red: { type: ai, faction: random, hero: random, team: none }, blue: { type: ai, faction: random, hero: random, team: none } } }
content:
  ruleset: sod
  mods: [{ id: vcmi, version: "1.8.0" }, { id: core, version: "1.8.0" }]
settings:
  start: newGame
  difficulty: normal
  timer: none
  gameSettingsOverrides: { spells: { tomesGrantBannedSpells: true } }
players:
  red:  { controller: ai, adventureAI: Nullkiller2, battleAI: BattleAI, team: none }
  blue: { controller: ai, adventureAI: Nullkiller2, battleAI: BattleAI, team: none }
initialPlayers:
  red:  { controller: ai, faction: random, hero: random, startingBonus: random }
  blue: { controller: ai, faction: random, hero: random, startingBonus: random }
aliases:
  hero/red/orrin: { type: "core:orrin", start: [10, 10, 0] }
  town/red/castle: { type: "core:castle", start: [8, 10, 0] }
---
turn: { day: 1, player: red }
actions:
  - buildStructure: { actor: ai/red/Nullkiller2, town: town/red/castle, building: "core:townHall" }
  - town: { id: town/red/castle, build: "core:townHall", cost: [{ resource: "core:gold", amount: 2500 }] }
  - resources: { player: red, change: [{ resource: "core:gold", from: 5000, to: 2500 }] }
  - moveHero: { actor: ai/red/Nullkiller2, hero: hero/red/orrin, destination: [12, 10, 0], reason: visit }
  - hero: { id: hero/red/orrin, path: [[10, 10, 0], [11, 10, 0], [12, 10, 0]], movement: [1560, 1360] }
  - visit: { hero: hero/red/orrin, object: object/resource/gold/at-12-10-0 }
  - resources: { player: red, change: [{ resource: "core:gold", from: 2500, to: 3000 }], source: object/resource/gold/at-12-10-0 }
  - remove: { object: object/resource/gold/at-12-10-0, reason: collected }
  - endTurn: { actor: ai/red/Nullkiller2 }
  - turnEnd: { player: red, timer: { start: { turn: 120000 }, end: { turn: 91784 } } }
---
world: { day: 8, phase: weekStart }
events:
  - random: { consumer: weekCreature, result: "core:imp" }
  - week: { kind: creature, creature: "core:imp" }
  - spawn: { id: object/monster/imp/at-44-19-0, type: "core:imp", count: 34, position: [44, 19, 0], visibleTo: [] }
```

`players` is the resolved setup after lobby and map random choices have been realized. It is the primary human-readable roster. `initialPlayers` is the original setup passed into game initialization. It preserves choices such as `random` so a replay can rebuild VCMI's `initialOpts` and traditional save files exactly.

## Identifiers

Use slash aliases for game objects because these are hierarchy-like references, not domain names:

- `hero/red/orrin`
- `town/blue/capitol`
- `object/mine/ore/at-18-42-0`
- `object/monster/imp/at-44-19-0`
- `battle/dragonUtopia/at-70-33-0`
- `stack/attacker/0`
- `query/red/levelUp/orrin/day12`

Use mod-qualified identifiers for game content:

- `"core:gold"`
- `"core:orrin"`
- `"core:pikeman"`
- `"core:earthMagic"`
- `"core:townHall"`

Use numeric values only for naturally numeric facts: coordinates, battle hexes, stack counts, resource amounts, damage, experience, movement points, and slot numbers where no stable name exists.

## Record Style

Most actions are one-key mappings in flow style:

```yaml
- resources: { player: red, change: [{ resource: "core:gold", from: 5000, to: 2500 }] }
```

Use block style for nested structures:

```yaml
- battle:
    id: battle/dragonUtopia/at-70-33-0
    ...
```

Do not include packet type IDs, record counters, connection IDs, request serials, or raw payloads in the normal format. If an implementation needs those for debugging, they belong in a separate debug trace, not in VGT.

## Decisions And Effects

The transcript records both decisions and effects. Decisions are authoritative for replay. Effects explain and audit what became true.

Action records answer "what did an actor choose?" VGT 4 uses the action verb as the
record key so readers do not have to scan through a generic `decision` wrapper. The
replayer continues to accept the VGT 3 `decision: { kind: ... }` spelling.

```yaml
- moveHero: { actor: ai/red/Nullkiller2, hero: hero/red/orrin, destination: [12, 10, 0], reason: visit }
- queryAnswer: { actor: human/blue, query: query/blue/levelUp/valeska/day4, answer: "core:archery" }
- melee: { actor: battleAI/red/BattleAI, stack: stack/attacker/0, target: stack/defender/2 }
```

Effect records answer "what became true in game state?"

```yaml
- hero: { id: hero/red/orrin, path: [[10, 10, 0], [11, 10, 0], [12, 10, 0]], movement: [1560, 1360] }
- resources: { player: red, change: [{ resource: "core:gold", from: 2500, to: 3000 }] }
- army: { owner: hero/red/orrin, slot: 0, creature: "core:pikeman", count: [41, 37], reason: casualties }
```

A decision may be followed by zero, one, or many effects. Some world effects have no player decision. Some rejected decisions have no material effect and should be recorded only if useful for analysis:

```yaml
- recruitHero: { actor: ai/red/Nullkiller2, town: town/red/castle, hero: "core:sorsha" }
- rejected: { decision: recruitHero, reason: insufficientGold }
```

Rejected or illegal decisions are not required for state replay, but are valuable for AI debugging. They can be enabled as an analysis option.

Timer-forced choices are decisions whose actor is the clock, not a player or AI:

```yaml
- endTurn: { actor: timer/red }
- battle:
    id: 2
    events:
      - battleAction: { actor: timer/red, battle: 2, action: { side: attacker, stack: stack/1, creature: core/pikeman, action: defend } }
```

Timer updates used only for UI display are not transcript records. At turn end, the recorder may include a compact timer snapshot for analysis:

```yaml
- turnEnd: { player: red, timer: { start: { turn: 120000, base: 300000 }, end: { base: 276442, ended: true } } }
```

## Event Coverage

The first complete text format must cover these material effects:

- turn start, turn end, day/week/month transition
- player status, victory, loss, defeat, retreat, surrender
- resources and income
- hero movement, teleport, embark/disembark, digging
- object visit start/end when it changes future legal state
- object ownership, creation, removal, position, visitors
- town construction, razing, name, researched spells, available creatures, garrison heroes
- hero primary skills, secondary skills, experience, level-up choices, mana, movement
- spells learned/removed, adventure spell casts and their effects
- army stack type, count, slot changes, upgrades, garrison swaps, casualties, summons
- artifact creation, pickup, equip, unequip, move, assemble/disassemble, discharge, destroy
- fog-of-war changes, compacted as tile runs or rectangles rather than one tile per line
- quests and query state that affects future legal choices
- all battle setup, decisions, actions, damage, deaths, spell effects, morale/luck outcomes, obstacle changes, round changes, active stack changes, and battle result
- world events, timed events, creature growth, spawned monsters, generated dwellings/resources, and any hidden effect that changes future state

The first implementation may support a subset, but the writer must know when the transcript is incomplete. Strict mode should stop on an unmodelled material state change instead of emitting opaque data.

## Randomness

Record realized random facts near the material event they affect. Do not record RNG state or unused draws.

Examples:

```yaml
- random: { consumer: weekCreature, result: "core:imp" }
- random: { consumer: townSpellResearch, town: town/red/castle, level: 3, offered: ["core:fireball", "core:forceField"] }
- hit: { attacker: stack/attacker/0, defender: stack/defender/2, damage: 173, killed: 8, roll: { kind: damage, range: [12, 24], value: 19 }, luck: good }
```

If the random result is fully described by the effect, a separate `random` record is optional. For analysis, include it when it explains luck versus decision quality.

## Battles

Battles are nested under the action or world event that caused them. Battle decisions and effects stay visually inside the battle block.

```yaml
- battle:
    id: battle/monster/at-12-10-0
    position: [12, 10, 0]
    field: "core:grass"
    attacker: hero/red/orrin
    defender: object/monster/pikeman/at-12-10-0
    setup:
      - side: { name: attacker, player: red, hero: hero/red/orrin }
      - stack: { id: stack/attacker/0, side: attacker, creature: "core:marksman", count: 42, at: 42 }
      - stack: { id: stack/defender/0, side: defender, creature: "core:pikeman", count: 38, at: 87 }
    rounds:
      - round: 1
        actions:
          - shoot: { actor: battleAI/red/BattleAI, stack: stack/attacker/0, target: stack/defender/0 }
          - shot: { stack: stack/attacker/0, target: stack/defender/0 }
          - hit: { attacker: stack/attacker/0, defender: stack/defender/0, damage: 94, killed: 7, remaining: 31, luck: none }
          - active: { stack: stack/defender/0 }
          - wait: { actor: neutralAI, stack: stack/defender/0 }
          - wait: { stack: stack/defender/0 }
    result:
      winner: red
      outcome: win
      experience: [{ hero: hero/red/orrin, amount: 390 }]
      losses:
        attacker: []
        defender: [{ creature: "core:pikeman", count: 38 }]
```

Inside battle blocks, use `from`, `to`, and `at` for hexes. Do not write `fromHex` or `toHex`.
Battle decisions follow the same verb-keyed style as adventure actions: `wait`,
`defend`, `shoot`, `walkAndAttack`, `heroSpell`, and the other battle action names
are direct record keys. The enclosing battle supplies the battle identifier.

## Fog And Hidden State

Fog changes are relevant because a recorded-game player must reproduce what each party knew. They can be large, so they need a compact textual encoding.

Preferred forms:

```yaml
- visibility: { player: red, reveal: { rectangles: [[[10, 10, 0], [16, 16, 0]]], reason: hero/red/orrin } }
- visibility: { player: blue, reveal: { runs: [{ y: 44, z: 0, x: [12, 18] }, { y: 45, z: 0, x: [13, 17] }] } }
```

Do not emit one event per tile. A single visibility action may contain many rectangles or runs.

## Implementation Plan

### 1. Writer Skeleton

Add a text writer separate from the binary trace recorder. It should write a YAML stream with:

- header document
- turn/world documents
- action lists
- no binary fields
- map file hash in the header
- no state checkpoint hashes

Enable it with an environment variable or local debug option at first, for example `VCMI_VGT_TEXT=/path/game.vgt`.

For deterministic replay diagnostics, `VCMI_VGT_TURN_STATE_DIR=/path/game.turn-states`
writes a full server save immediately after every applied player turn end. Files use a
stable chronological name such as `turn-000001-day-0001-red.vsgm1`. These saves are
an external debug oracle and are not part of the portable VGT format. Pass the archive
to `scripts/vgt_replay.py replay --expected-turn-states game.turn-states` to compare
every reconstructed save byte-for-byte. `--output-turn-states` optionally keeps the
reconstructed saves for diagnosis.

### 2. Alias Registry

Build a stable alias registry during game initialization:

- heroes by owner and type/name
- towns by owner/type/position
- repeated map objects by type and position
- battle stacks by battle-local side/slot
- generated objects when they appear

The registry is an implementation helper; aliases are the public VGT references.

### 3. Decision Capture

Capture decisions from server request handling:

- `CPackForServer` requests in `CGameHandler::handleReceivedPack`
- query answers in `QueryReply`
- battle actions in `MakeAction`
- AI-originated requests are captured the same way as human requests because they enter the server as requests

Do not record low-level socket acknowledgements.

### 4. Effect Capture

Use `CVCMIServer::applyPack(CPackForClient & pack)` as the main source of authoritative material effects. This is the cleanest implementation seam discovered during the trace work: server-generated packs are applied to `CGameState` there exactly once.

Do not write `CPackForClient` as a packet. Instead, add text codecs that convert each relevant pack into VGT effects.

Initial high-value codecs:

- `NewTurn`, `PlayerEndsTurn`, `PlayerStartsTurn`
- `TryMoveHero`, `HeroVisit`, `RemoveObject`, `NewObject`, `SetObjectProperty`, `ChangeObjectVisitors`
- `SetResources`, `NewStructures`, `RazeStructures`, `SetAvailableCreatures`
- army and artifact operation packs
- `HeroLevelUp`, `SetSecSkill`, `SetPrimarySkill`, `SetHeroExperience`, `SetMana`, `SetMovePoints`
- `FoWChange` with compact tile encoding
- all battle packs required for battle playback
- `InfoWindow` and dialogs only when they create choices or explain material results
- victory/loss/player status packs

### 5. World And Hidden Effects

World events are not a special replay mode. They are the same applied effects grouped under:

```yaml
world: { day: 8, phase: weekStart }
```

This captures invisible spawns and growth because they still produce material state effects.

### 6. Completeness Mode

The writer must not silently omit material changes.

- Strict mode: stop recording and report the first unmodelled material pack.
- Exploratory mode: write `unmodelled: { pack: PackName, material: true }` and mark the document `complete: false` in the next header/update document.

No raw fallback is allowed in either mode.

### 7. Replay/Player Tool Shape

The future player tool should:

1. Load map/content/settings from the header.
2. Start the normal server flow.
3. Compile each VGT decision into the corresponding server request and feed it to the server.
4. Compare recorded effects with recomputed effects where an audit checker supports that record type.
5. Build its own external cache for fast seeking. The cache is not part of VGT.
6. Render the timeline and battle blocks.
7. Allow branching into live play from a reconstructed state.

Because VGT contains no snapshots, jumping to the middle requires replay from the start or using a cache built by the tool. That is acceptable for the text format.

Decision replay through normal mechanics is the authoritative reconstruction path. Effects state what actually became true, including realized random outcomes such as a wandering monster joining instead of starting a battle, but they are not used to advance server state during normal replay.

## Open Questions

- How much initial alias data should be written versus derived from the map at replay time?
- Which `InfoWindow`/dialog records are material and which are only UI narration?
- Should rejected AI decisions be enabled by default or only in debug transcripts?
- How should modded packs register their text codecs?
- How aggressive should fog compaction be before it becomes hard to read?
- Which world/random events need explicit `random` records when the material effect already contains the result?

## First Milestone

The first useful milestone is not full game coverage. It is a small complete game where every material event has a text codec:

- two AI players
- normal adventure movement
- resource pickup
- town building
- one neutral battle
- one level-up choice
- one week transition with a realized random result
- victory/loss if it happens

The acceptance test is a generated VGT file that contains no raw/binary fields and can be replayed by a prototype interpreter into the same visible game timeline.
