# VGT Event Transcript Plan

This document restarts the readable VCMI Game Transcript work from a text-first direction. It keeps the useful parts of the previous VGT readable format, but rejects binary packet dumps, save snapshots, state checkpoint hashes, record counters, and raw compatibility blobs. The map-file hash is still required because the external map file is part of the transcript input.

The result is a compact YAML transcript that can be read by a human, parsed by
normal YAML tooling, and replayed through the normal server request path to
reconstruct game state. It is also suitable for timeline inspection and later UI
work such as scrolling through history or branching into live play.

## Current implementation

VGT 4 is the current writer format. It contains a header followed by turn and world
documents made from semantic actions and material outcomes. It has no continuation
documents, embedded saves, checkpoints, packet payloads, or state hashes. VGT 4 is
the only supported transcript version while the format is being stabilized.

The implementation is an optional static server component under `server/vgt`.
Configure VCMI with `-DENABLE_VGT=ON` to include recording and replay support; it is
disabled by default. Disabled builds compile the narrow server integration hooks as
inline no-ops and do not expose the VGT command-line options.

The reviewed readability and compression changes are implemented. Their rationale,
exact expansion rules, and validation record are kept in
[`VGT_Readability_Compression_Refinement_Plan.md`](VGT_Readability_Compression_Refinement_Plan.md).

Enabled builds write readable transcripts automatically below the user data
`Transcripts` directory. Enable only the optional exact-replay debug oracle with:

```bash
VCMI_VGT_TURN_STATE_DIR=/tmp/game.turn-states \
vcmiclient [game options]
```

Main files use `<UTC-start>_<map-slug>_g<game-id>.vgt`; tactical events use the
adjacent `.vgt.battles.yaml` companion. Successful saves append ignored savepoint
comments used to resume the same pair. Loading an older save forks at that savepoint
instead of destroying either line of history. A load with no matching savepoint
starts a marked partial transcript so subsequent history is still retained.

`game.turn-states` contains a full server save after every applied player turn, named
in chronological order, for example
`turn-000001-day-0001-red.vsgm1`. It is deliberately separate from the portable
transcript and should not be distributed as part of a VGT recording.

Validate the YAML, replay its semantic actions, and compare every reconstructed turn
save byte-for-byte with client-local UI preferences canonicalized away:

```bash
python3 scripts/vgt_replay.py replay /tmp/game.vgt \
  --engine-binary build/bin/vcmiserver \
  --output-save /tmp/replayed.vsgm1 \
  --resource-root /path/to/vcmi-data \
  --expected-turn-states /tmp/game.turn-states \
  --output-turn-states /tmp/replayed.turn-states \
  --strict
```

The first mismatch stops replay and reports both sizes, the first differing byte
offset, and the expected and reconstructed byte values. The output archive is optional
and is useful only for diagnosing a mismatch.

To turn the first 25 completed player turns into a normal loadable save, omit the
debug comparison options and select the replay extent:

```bash
python3 scripts/vgt_replay.py replay /tmp/game.vgt \
  --engine-binary build/bin/vcmiserver \
  --output-save /tmp/after-turn-25.vsgm1 \
  --resource-root /path/to/vcmi-data \
  --through-turn 25 \
  --strict
```

World events immediately following the selected turn are included, so the produced
save represents the stable state before the next player's actions. It can then be
loaded through VCMI's normal saved-game interface.

## Principles

- The file is YAML 1.2, not a custom DSL.
- The file is append-friendly. New turns or world phases can be appended to the end.
- The YAML data contains no binary payloads, save snapshots, or state checkpoint hashes. Ignored append-only comments associate exact save files with transcript offsets for safe resume and branching.
- The header contains a required hash of the referenced map file, so replay can fail early if the map changed.
- The map-file hash is an input-integrity check, not a replay checkpoint. It is the only required hash in the first version.
- Records use identifiers instead of enum numbers.
- Records use full readable field names. Avoid VGT-specific abbreviations.
- Only material events are recorded. Do not record acknowledgements, duplicate network delivery, timer ticks, internal implementation noise, or random draws that never realize into game state or a decision.
- Consecutive successful one-tile movement requests are written as one `move`
  transaction. One tile uses only `to`; every multi-tile ordinary move uses
  run-length compass `steps` plus its exact destination. Replay expands this sole
  grammar into the original one-tile server requests. A blocking visit, teleport,
  embark/disembark, or other semantic transition ends ordinary movement.
- Turn documents and `endTurn` actions already express ordinary turn boundaries, so
  duplicate turn-start/end effects are omitted unless they carry a semantic decision
  prompt or timer state. Visit-end sentinels, duplicate town-visit notifications, visitor bookkeeping,
  and internal reward-selection flags are also derived implementation state and are
  omitted from the readable transcript.
- Every gameplay decision is recorded: human, AI, neutral/world, battle AI,
  decision-bearing query answer, retreat/surrender choice, and scripted choice where
  applicable. Interface readiness, pause toggles, and context-free UI answers are
  session protocol rather than game history and are omitted.
- Every authoritative material effect that cannot be regenerated exactly from the
  referenced inputs is recorded, including effects nobody could see yet, such as a
  randomized neutral refresh or week-start spawned monsters in fog. Ordinary fixed
  map-dwelling growth is derived and omitted after its day-one inventory is stated.
- In the material event stream, randomness is recorded only as realized facts near the event that consumed it. Internal RNG state is not a timeline event. Replay advances RNG by feeding recorded decisions through normal server logic.
- Countdown timer ticks are not recorded. Timer state is recorded only when a timer forces a gameplay action, or as a compact start/end snapshot on the turn end record for analysis.

## Self-Sufficiency Boundary

The transcript names the map, content set, settings, map game-setting overrides, players, and all events. It does not embed the map, but it must identify the exact map file with a cryptographic hash.

A replay/player tool starts from the declared map and content, verifies the map hash, initializes the game from the declared settings, restores small deterministic initialization state such as the map object-name counter, starts the normal server flow, then compiles VGT decision records back into server requests. Effect records are audit/readability records; they are checked against recomputed server results when a checker supports that comparison, but they are not the replay source of truth. For generated random maps, the writer must save the generated map as a normal map file, hash that saved file, and reference it from the transcript. The transcript may also record the random-map generator options preserved by normal save files, but replay still loads the saved map file instead of generating a new one.

This keeps the transcript readable while detecting the most dangerous external input drift: the map file. Mod/content hashing can be added later if needed, but it is not required for the first implementation.

## YAML Stream Shape

Use a YAML multi-document stream. The header is the first document. Each following document is a turn, world phase, or battle-only phase. This is easier to append than a single large top-level list.

The machine-readable definition is
[`config/schemas/vgt-4.schema.json`](../../config/schemas/vgt-4.schema.json). It is a
JSON Schema Draft 2020-12 document for the parsed YAML stream: load all YAML
documents and present them to the schema as an array. This is the same normalized
shape produced by `scripts/vgt_replay.py check --normalized-json`. JSON Schema
`description` fields are the format's inline machine-readable documentation.

```yaml
vgt: 4
format: VCMI readable event transcript
engine: { version: "1.8.0" }
map:
  uri: "Maps/Arrogance.h3m"
  name: "Arrogance"
  hash: { algorithm: sha256, value: "8db3480a8b6f7e7a8c3d2a17f8c1b0c77a2b8b7c4f7f4ce0f1b5b83e7a3e0000" }
  objectNameCounter: 128
settings:
  start: newGame
  startTime: 1775000000
  difficulty: normal
  randomSeed: 504122489
players:
  red: { controller: ai, faction: conflux, hero: grindan }
initialPlayers: {}
initialState:
  heroes:
    red/orrin: { position: [10, 10], experience: 0, mana: 12, movement: 1560, artifacts: [], army: [] }
---
turn: { date: 1/1/1, player: red }
actions:
  - build: { town: town/castle@8.10, building: townHall, cost: { gold: 2500 } }
  - encounter:
      hero: orrin
      with: resource/gold@12.10
      approach: { to: [11, 10], steps: "E" }
      outcome:
        - resources: { gold: +500 }
        - remove: { object: resource/gold@12.10 }
  - endTurn
---
world: { date: 1/1/2, phase: newDay }
events:
  - income: { red: { gold: 500 } }
  - refresh: { red/orrin: { movement: 1560, mana: 12 } }
  - week: { type: firstWeek }
```

`players` is the resolved setup after lobby and map random choices have been realized. It is the primary human-readable roster. `initialPlayers` is the original setup passed into game initialization. It preserves choices such as `random` so a replay can rebuild VCMI's `initialOpts` and traditional save files exactly.

## Identifiers

Use compact slash aliases for game objects because these are hierarchy-like
references, not domain names. The core content namespace is implicit, heroes omit
the redundant `hero/` kind, and surface object locations omit `z = 0`:

- `red/orrin`
- `town/blue/capitol-castle@8.10`
- `mine/ore-pit@18.42`
- `monster/imps@44.19`
- `monster/imps@44.19.1` (underground)
- `attacker/marksmen` (battle-local)

Server query IDs are internal replay bookkeeping and are not public transcript
references. A query answer is nested under the action that caused it; the readable
choice name and any replay-significant numeric choice value remain.

Core content names are bare. Non-core content keeps its mod namespace:

- `gold`
- `orrin`
- `pikeman`
- `earthMagic`
- `townHall`
- `myMod/customCreature`

Use numeric values only for naturally numeric facts: coordinates, battle hexes, stack counts, resource amounts, damage, experience, movement points, and slot numbers where no stable name exists.

## Record Style

Most actions are one-key mappings in flow style:

```yaml
- resources: { gold: -2500 }
```

Use block style for nested structures:

```yaml
- battle:
    id: 12
    attacker: red/orrin
    defender: dragonUtopia/dragon-utopia@70.33
    ...
```

Do not include packet type IDs, record counters, connection IDs, request serials, or raw payloads in the normal format. If an implementation needs those for debugging, they belong in a separate debug trace, not in VGT.

## Decisions And Effects

The transcript records both decisions and effects. Decisions are authoritative for replay. Effects explain and audit what became true.

Action records answer "what did an actor choose?" The action verb is the record key
so readers do not have to scan through a generic `decision` wrapper.

```yaml
- move: { hero: orrin, to: [12, 10] }
- chooseSkill: { actor: blue, hero: gurnisson, skill: logistics }
- build: { town: town/castle@8.10, building: townHall, cost: { gold: 2500 } }
```

The enclosing turn supplies the actor and reference owner, so both are omitted for
the active player. One adjacent tile uses only its destination:

```yaml
- move: { hero: yog, to: [32, 4] }
```

Every multi-tile ordinary move uses run-length compass directions, with its exact
destination as a check:

```yaml
- move: { hero: yog, to: [41, 8], steps: "SE E SE E*3 NE N NE" }
```

Ordinary movement cannot change map levels. Gates, monoliths, whirlpools, and
teleport spells end it and become semantic `teleport` actions. `[x, y]` means the
surface; a nonzero level is one destination tuple such as `[x, y, 1]`.
Coordinate-list movement is not a format variant.

Effect records answer "what became true in game state?"

```yaml
- resources: { red: { gold: +500 } }
- skills: { red/orrin: { attack: +1 } }
- setMovement: { red/orrin: 1360 }
```

A decision may be followed by zero, one, or many effects. Some world effects have no
player decision. Rejected or illegal requests are not part of the current portable
format because they do not change replay state; a separate AI debug trace may record
them if needed.

Timer-forced choices are decisions whose actor is the clock, not a player or AI:

```yaml
- endTurn: { actor: timer/red }
- battle:
    id: 2
    events:
      - defend: { actor: timer/red, side: attacker, unit: attacker/pikemen }
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
- quests and semantic decision prompts that affect future legal choices
- all battle setup, decisions, actions, damage, deaths, spell effects, morale/luck outcomes, obstacle changes, round changes, active stack changes, and battle result
- world events, timed events, non-derived creature availability, spawned monsters,
  generated dwellings/resources, and any hidden effect that changes future state

The writer must know when the transcript is incomplete. Strict validation rejects an
`unmodelled` material record instead of accepting opaque data; the review corpus is
accepted only when it contains none.

## Randomness

Record realized random facts in the semantic event they affect. Do not record RNG
state, unused draws, or a generic `random` record. For example, a week record names
the selected creature, a spell-research decision names the offered/selected spell,
and an attack exchange carries `luck: good` when luck materially changed it.

## Battles

Battles are nested under the action or world event that caused them. Battle decisions and effects stay visually inside the battle block.

```yaml
- battle:
    id: 3
    attacker: red/orrin
    defender: monster/pikemen@12.10
    units:
      attacker/marksmen: { stack: 0, owner: red, count: 42 }
      defender/pikemen: { stack: 4, owner: neutral, count: 38 }
    events:
      - { event: start }
      - { event: nextRound }
      - wait: [defender/pikemen]
      - attack:
          actor: red
          side: attacker
          by: attacker/marksmen
          target: defender/pikemen
          aim: [{ unit: defender/pikemen, hex: 87 }]
          ranged: true
          damage: 94
          killed: 7
    outcome:
      result: normal
      winnerSide: attacker
      winner: red
      loser: neutral
      experience: { red/orrin: 390 }
      casualties: { defender: { pikeman: 38 } }
      removeDefender: true
```

Inside battle blocks, tactical walking is `move` with a hex `path`; it is unrelated
to adventure-map compass `steps`. Walking into an attack is folded into that
`attack` as `approach`. Ranged attacks are also `attack`, with `ranged: true`, and
spells are `cast`; damage, healing, and mana are folded into that cast. A siege gate
opening inside an uninterrupted walk is folded into its complete `path`. The
enclosing battle supplies the battle identifier. Raw stack
IDs appear only once in `units`; duplicate creature stacks receive stable final
ordinal segments such as `defender/pikemen/1` and `defender/pikemen/2`. Exactly
redundant accepted-action echoes are omitted, immediate retaliation is nested in the
initiating `attack`, and all resolution/cleanup facts are consolidated under one
`outcome`. A server-selected war-machine shot remains visible as an attack with
`automatic: true`; it is an observed part of the battle story, not a decision that
replay submits a second time.

## Fog And Hidden State

Fog changes are relevant because a recorded-game player must reproduce what each party knew. They can be large, so they need a compact textual encoding.

Preferred forms:

```yaml
- visibility: { mode: revealed, runs: [{ y: 44, x: [12, 18] }, { y: 45, x: [13, 17] }] }
- visibility: { player: blue, mode: hidden, runs: [{ y: 9, z: 1, x: [4, 11] }] }
```

The current turn player is implicit; another player is explicit. Surface runs omit
`z`, and underground runs retain `z: 1`. Do not emit one event per tile.

## Implementation Architecture

The following sections are the implemented architecture and retained design
checklist. The writer, aliases, decision/effect capture, strict checker, semantic
replayer, and byte-exact turn oracle all exist in the referenced source files.

### 1. Writer Skeleton

Add a text writer separate from the binary trace recorder. It should write a YAML stream with:

- header document
- turn/world documents
- action lists
- no binary fields
- map file hash in the header
- no state checkpoint hashes

An enabled build writes automatically to `VCMIDirs::get().userDataPath() / "Transcripts"`.
Recording follows game lifecycle rather than process lifetime, so starting or loading
a game selects the correct transcript without an output-path environment variable.

For deterministic replay diagnostics, `VCMI_VGT_TURN_STATE_DIR=/path/game.turn-states`
writes a full server save immediately after every applied player turn end. Files use a
stable chronological name such as `turn-000001-day-0001-red.vsgm1`. These saves are
an external debug oracle and are not part of the portable VGT format. Pass the archive
to `scripts/vgt_replay.py replay --expected-turn-states game.turn-states` to compare
every reconstructed save byte-for-byte after clearing `playerLocalSettings` on both
sides. That field contains only client selection, planned paths, list ordering, and
spellbook UI preferences; it is not authoritative game state. `--output-turn-states`
optionally keeps the reconstructed saves for diagnosis.

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
world: { date: 1/2/1, phase: newDay }
```

This captures invisible spawns and non-derived growth because they still produce
material state effects. Fixed weekly map-dwelling growth is regenerated from the
map/content inputs and therefore does not need a repeated event.

### 6. Completeness Mode

The writer must not silently omit material changes.

- The recorder writes `unmodelled: { stream: effect, pack: PackName, material: true }`
  when it encounters an unknown material pack, making incompleteness visible.
- `scripts/vgt_replay.py check --strict` and `replay --strict` reject the first such
  record. Release/acceptance recordings always use strict mode.

No raw fallback is allowed.

### 7. Replay/Player Tool Shape

The replayer does the following; timeline rendering and interactive branching are
separate UI work:

1. Load map/content/settings from the header.
2. Start the normal server flow.
3. Compile each VGT decision into the corresponding server request and feed it to the server.
4. Compare recorded effects with recomputed effects where an audit checker supports that record type.
5. Build its own external cache for fast seeking. The cache is not part of VGT.
6. Render the timeline and battle blocks.
7. Leave the reconstructed save available as the basis for later interactive
   branching.

Because VGT contains no snapshots, jumping to the middle requires replay from the start or using a cache built by the tool. That is acceptable for the text format.

Decision replay through normal mechanics is the authoritative reconstruction path. Effects state what actually became true, including realized random outcomes such as a wandering monster joining instead of starting a battle, but they are not used to advance server state during normal replay.

## Stabilization Boundary

- VGT 4 is the only accepted text grammar; the checker rejects legacy record names,
  identifier prefixes, coordinate-list movement, and redundant surface z values.
- Standard generated `InfoWindow` prose is omitted. Literal/non-core authored text
  is retained only as encounter or story narrative; semantic outcomes stay even
  when their UI message is dropped.
- Unknown material decisions/effects are fatal in strict validation. There is no raw
  compatibility fallback.
- The portable file remains events-only. Full saves belong in the sibling
  `game.turn-states/` debug directory and are used only as a byte-exact oracle.
- The schema and TypeScript model are the source for parser/code-generation work;
  future format additions must extend their closed record unions at the same time as
  the writer and replayer.
