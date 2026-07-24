# VGT Readability And Compression Refinement Plan

## Status And Scope

This document records the reviewed and implemented refinement that makes VGT more
compact and more like a game story written by a human. It defines the current VGT 4
format together with
[`VGT_Event_Transcript_Plan.md`](VGT_Event_Transcript_Plan.md).

The original eighteen reviewed proposals and the sixteen-proposal stabilization
pass are included. The examples are accepted current syntax unless explicitly
labelled as the removed verbose shape.

The default requirement is semantic losslessness: a compact record must expand into
the same ordered decisions and independent material outcomes, or provide everything
needed to issue the same server requests. Proposal 18 deliberately uses the narrower
definition of replay losslessness: deterministic effects may be regenerated, but
important human-readable outcomes remain in the transcript. The strict checker,
semantic replayer, and byte-exact turn-save oracle are the acceptance mechanisms.

## Agreed General Rules

- Keep YAML 1.2 and a machine-readable schema.
- Keep the external map and its hash as replay inputs.
- Prefer game concepts over network packets and serialized implementation state.
- Keep exact numeric values needed to reproduce decisions, even when a readable name
  is also present.
- Omit facts that are deterministic properties of the referenced map or content,
  unless they materially improve the human story.
- Omit standard UI boilerplate. Preserve genuine map-authored narrative.
- Preserve exceptional or ambiguous state explicitly; defaults may be omitted only
  when the format defines an unambiguous expansion.
- Continue verifying replay with full per-turn saves compared byte-for-byte.
- Do not add compatibility forms while the format is still being stabilized.

## Proposal Checklist

1. Combine a decision and its effects into a semantic transaction; omit derived
   automatic buildings.
2. Inherit the turn player and support a bounded focused hero scene.
3. Shorten identifiers reversibly without a glossary.
4. Encode every multi-tile ordinary movement with run-length directions.
5. Use signs and verbs instead of repeated `mode` fields.
6. Make creature availability sparse and creature-keyed.
7. Batch ordered recruitment into one purchase.
8. Represent each new day as one global world chapter.
9. Combine visits, prompts, choices, and results into encounters.
10. Omit generated UI messages but preserve map-authored narrative and meaningful
    choices.
11. Keep each battle in one continuous scene.
12. Remove redundant battle `startAction` records.
13. Use descriptive battle-local unit names containing side and creature type.
14. Combine an attack and its retaliation into one combat exchange.
15. Batch consecutive passive battle choices while preserving order.
16. Consolidate battle resolution and post-battle cleanup.
17. Record rewardable refreshes as sparse mechanical changes only.
18. Regenerate low-level deterministic effects while retaining concise story
    outcomes such as paid resources.

## Detailed Refinements

### 1. Semantic Transactions

Group a player decision with the independent material results that explain it. Do
not present a player action and its server effects as unrelated adjacent records.

Removed verbose shape:

```yaml
- buildStructure: { actor: red, town: town/red/froisan-conflux/at-7-5-0, building: core/cityHall }
- resources: { player: red, mode: relative, values: [{ gold: -5000 }] }
- town: { id: town/red/froisan-conflux/at-7-5-0, build: [core/cityHall, core/extraTownHall, core/extraCityHall], builtThisTurn: 1 }
```

Current shape after applying the identifier rules from proposal 3:

```yaml
- build:
    town: town/red/froisan-conflux@7.5
    building: cityHall
    cost: { gold: 5000 }
```

`extraTownHall` and `extraCityHall` are automatic buildings derived from the
referenced content definition. They are not future construction and are not player
decisions, so the normal transcript omits them. An exceptional non-derived automatic
effect would remain explicit under a specifically named result field, not a vague
field such as `completed`.

### 2. Turn And Scene Context

The player in the turn header is the default actor and affected player. References
owned by that player omit the owner. A bounded nested scene may establish a hero
for two or more consecutive actions. Exceptions such as `world`, a timer, or another
player remain explicit.

```yaml
turn: { date: 1/1/1, player: red }
actions:
  - build: { town: town/froisan-conflux@7.5, building: cityHall, cost: { gold: 5000 } }
  - with: grindan
    actions:
      - move: { to: [8, 5] }
      - encounter: { with: resource/wood@9.5, outcome: [{ resources: { wood: +6 } }] }
```

Context is lexical and limited to the containing mapping. It must never depend on an
earlier unrelated document or require guessing from prose.

### 3. Reversible Short Identifiers Without A Glossary

Do not add an entity definition list merely to shorten ordinary references. Apply
deterministic shortening rules instead:

- `core/` is the implicit default content namespace.
- Non-core mod namespaces remain explicit.
- Remove the generic `object/` prefix.
- Remove the `hero/` prefix; the owner and hero name remain.
- Retain meaningful kinds such as `town/`, `monster/`, and `mine/`.
- Replace `/at-x-y-z` with `@x.y` on the surface and `@x.y.z` on other levels.
- `z = 0` is the surface and is omitted from object locations.
- `z = 1` is the underground and remains explicit.

Examples:

```text
hero/red/core/grindan
-> red/grindan

town/red/froisan-conflux/at-7-5-0
-> town/red/froisan-conflux@7.5

object/core/monster/stone-gargoyles/at-9-9-0
-> monster/stone-gargoyles@9.9

object/core/monster/stone-gargoyles/at-9-9-1
-> monster/stone-gargoyles@9.9.1

core/lightningBolt
-> lightningBolt
```

Capitalization of hero and town display names is deferred. Canonical identifiers
remain mechanically reversible and case-correct; a viewer may capitalize them for
display later.

### 4. Direction-Encoded Movement

One adjacent tile uses only `to`. Every multi-tile uninterrupted ordinary move uses
run-length compass directions from the hero's known starting position. Retain the
exact destination as a readable check. There is no coordinate-list alternative.

```yaml
- move:
    hero: grindan
    to: [18, 7]
    steps: "SE E SE E*3 NE N NE"
```

This expands exactly to the original tile sequence. `[x, y]` means the surface;
`[x, y, z]` is allowed only for a nonzero level. A gate, monolith, whirlpool, spell
teleport, embark, disembark, blocking visit, or level change ends ordinary movement
and remains a separate semantic event.

### 5. Signed Changes And Explicit Assignment Verbs

Use the sign of a value for relative changes. Use a distinct verb for absolute
assignment rather than repeating `mode`.

```yaml
- mana: { red/grindan: -10 }
- skills: { red/grindan: { attack: +1 } }
- spend: { gold: 5000 }
- setMana: { red/grindan: 30 }
```

Resource collections use mappings instead of arrays of one-key mappings. The schema
must distinguish signed deltas from absolute values without relying on field order.

### 6. Sparse Creature Availability

Represent only actual creature pools. Do not write seven positional town levels
filled with empty lists.

```yaml
- availability:
    towns:
      town/red/froisan-conflux@7.5: { pixie: 18, airElemental: 1 }
    dwellings:
      fire-lake@105.6.1: { efreet: 2 }
```

The content definition supplies absent, unavailable dwelling levels. A pool that
permits base and upgraded creatures must retain that shared-pool relationship in a
compact explicit form; it must not be flattened into two independent counts.

The day-one inventories are initial state, so collect all town and map-dwelling
inventories into this single world record. On later weeks, retain town `growth` and
any nonstandard or data-dependent dwelling result. Omit the ordinary fixed weekly
refresh of a map dwelling when its exact result follows from the referenced content
definition; replay regenerates it. Refugee Camps and scripted or dynamically changed
dwellings are examples whose realized availability must remain explicit.

### 7. Ordered Recruitment Batches

Combine consecutive recruitment requests at the same source into one ordered
purchase. Preserve order and destination slots where they affect the resulting army.

```yaml
- recruit:
    at: town/red/froisan-conflux@7.5
    units:
      - { creature: waterElemental, count: 6, slot: 0 }
      - { creature: airElemental, count: 5, slot: 1 }
      - { creature: pixie, count: 2, slot: 2 }
    paid: { gold: 3100 }
```

The batch expands to the same ordered server requests. It must split when another
decision, query, failure, or material side effect interrupts recruitment. The
remaining pool is the deterministic result of those requests and is not repeated.

### 8. Global Day Chapters

Normal daily income, hero movement and mana refresh, and weekly creature growth are
applied globally before any player starts a turn. Represent them in one world
document, not as portions attached to individual player turns.

```yaml
world: { date: 1/2/1, phase: newDay }
events:
  - income:
      red:  { wood: 2, gold: 1000 }
      blue: { ore: 2, gold: 1500 }
  - growth:
      town/red/froisan-conflux@7.5: { pixie: +10, airElemental: +3 }
      town/blue/slau-stronghold@4.29: { goblin: +15, goblinWolfRider: +9 }
  - refresh:
      red/grindan: { movement: 1500, mana: 12 }
      blue/gurnisson: { movement: 1630, mana: 20 }
---
turn: { date: 1/2/1, player: red }
```

Creature growth appears only on a week boundary. Player-specific timed map or town
events are the exception: they run immediately before that player's turn and remain
inside that turn rather than the global day chapter.

### 9. Semantic Encounters

Combine an object visit, its decision-bearing prompt, the selected answer, and its
material result into one encounter. Preserve the raw answer value needed for exact
replay alongside a readable choice name.

```yaml
- encounter:
    hero: red/grindan
    with: monster/gnoll-marauders@5.8
    choice: { name: acceptJoin, value: 1 }
    outcome:
      joins: { gnollMarauder: 21, slot: 3 }
      removeEncounter: true
```

An encounter must retain the order of any nested server requests and must expose a
rejected, cancelled, or failed outcome when that distinction affects subsequent
legal state.

### 10. Narrative Text Policy

Do not record rendered text merely because the server opened an information window.
Apply this policy:

- Omit standard generated UI messages for mines, resources, skill objects,
  recruitment, and similar mechanics.
- Preserve semantic outcomes such as ownership, income, rewards, and learned skills.
- Preserve custom map-authored narrative, dialogue, riddles, and choice text when it
  contributes to the story.
- Preserve the meaning of a decision-bearing prompt, preferably as a semantic choice
  rather than rendered UI text.
- Retain the raw numeric choice value when exact replay requires it.

Standard UI example:

```text
"You gain control of a Sawmill. It will provide you with two units of wood per day."
```

Replace it with the mechanical story:

```yaml
- capture:
    hero: blue/gurnisson
    object: mine/sawmill@2.34
    income: { wood: 2 }
```

Keep genuine map-authored narrative:

```yaml
- story:
    hero: red/grindan
    at: event@12.8
    text: "The dying guard entrusts Grindan with the northern gate key."
    choice: { name: acceptKey, value: 1 }
```

Do not expose `MetaString` instruction arrays, numeric local-string encodings, or
rendered core localization as portable VGT records.

### 11. Continuous Battle Scenes

Keep a battle in one block from start through final cleanup. Do not split it merely
because a battle action produces an adventure-level effect such as hero mana loss,
experience, army casualties, artifact movement, or object removal.

```yaml
- battle:
    id: 0
    attacker: red/grindan
    defender: monster/stone-gargoyles@9.9
    events:
      - cast:
          caster: red/grindan
          spell: lightningBolt
          target: defender/stone-gargoyles
          mana: 10
          damage: 85
          killed: 5
      - ...
    outcome: ...
```

The event order inside the block remains authoritative. A player viewing the story
should not need to join several blocks with the same numeric battle ID.

### 12. Remove Redundant `startAction`

When a battle decision is accepted exactly as written, do not repeat it as a
`startAction` effect.

```yaml
- move: { actor: red, side: attacker, unit: attacker/water-elementals, to: 58, path: [54, 55, 56, 57, 58] }
```

If the accepted action differs from the request, preserve that exceptional fact with
an explicit field such as `acceptedAs`, or with a separate rejection/transformation
record. The writer must test equivalence rather than assuming it.

### 13. Descriptive Battle-Local Unit Names

Declare each battle stack once using a name that includes both its side and creature
type. Do not use opaque names such as `A0` and `D0` in the human event stream.

```yaml
units:
  attacker/air-elementals:
    stack: 0
    owner: red
    count: 8
  defender/stone-gargoyles:
    stack: 4
    owner: neutral
    count: 22
```

Events then use the descriptive name:

```yaml
- wait: attacker/air-elementals
- move: { actor: red, side: attacker, unit: attacker/air-elementals, to: 8, path: [2, 3, 4, 5, 6, 7, 8] }
- attack:
    by: attacker/air-elementals
    target: defender/stone-gargoyles
```

When one side has multiple stacks of the same creature, assign stable ordinals for
the lifetime of the battle:

```text
defender/stone-gargoyles/1
defender/stone-gargoyles/2
```

Raw stack IDs appear only in the roster. Summoned, cloned, transformed, and newly
created stacks receive equally descriptive stable names when they appear.

### 14. Combat Exchanges

Combine an initiating attack, its material target state, and an immediate retaliation
into one combat exchange. Do not repeat attacker and creature identity inside nested
attack results.

```yaml
- attack:
    actor: red
    side: attacker
    by: attacker/air-elementals
    target: defender/stone-gargoyles
    via: [9]
    from: 27
    targetAt: 44
    damage: 48
    killed: 3
    retaliation:
      damage: 16
```

Fields such as `healthDelta: -48` and `operation: update` are derived and omitted.
Named exceptional facts such as luck, morale, ranged fire, no retaliation, or a
special effect remain explicit. Unknown flags must not be silently discarded.
Deterministic double shots are one exchange with aggregate `damage`/`killed` and
`strikes: 2`; named creature spell effects use `applies: [curse]`.

### 15. Ordered Passive Battle Batches

Batch consecutive passive choices such as `wait` when no intervening event needs to
be shown. Preserve exact order.

```yaml
- wait:
    - attacker/air-elementals
    - attacker/pixies
    - defender/stone-gargoyles
    - attacker/water-elementals
```

Split the batch at an effect, query, timer-forced action, rejection, or any other
event that is meaningful in the story. Apply the same technique to another passive
action only if its semantics and ordering are equally unambiguous.

### 16. Consolidated Battle Outcomes

Replace repeated `result`, `resultAccepted`, `resultsApplied`, and `ended` records,
plus adjacent post-battle mutations, with one outcome containing each independent
fact once.

```yaml
outcome:
  winner: red/grindan
  defeated: monster/stone-gargoyles@9.9
  experience:
    hero: 368
    stacks:
      attacker/air-elementals: 368
      attacker/water-elementals: 368
      attacker/pixies: 368
      attacker/gnoll-marauders: 368
  survivors:
    attacker/pixies: 14
  armies:
    red/grindan: [{ slot: 0, creature: pixie, count: 14, experience: 12880 }]
  removeDefender: true
```

Omit empty artifact moves and repeated winner/loser spellings. Preserve surrender,
retreat, hero defeat, artifacts, necromancy, creature transformation, and any other
non-default outcome explicitly. Record each surviving strategic participant army as
absolute slots after battle resolution, including total stack experience. This keeps
the strategic result sufficient for both tactical replay and
battle fast-forwarding, including retreat outcomes that consolidate battle stacks.

### 17. Sparse Rewardable Refreshes

Do not serialize the full rewardable-object configuration or its default-filled
`MetaString`, limiter, reward, tooltip, and reset structures. Record only the newly
realized mechanical change relative to the referenced object definition.

```yaml
- refresh: { object: windmill@1.26, reward: { sulfur: 5 } }
```

For a standard Windmill, the referenced content definition already says that it is
available once per week, resets its visitors weekly, rerolls its reward weekly, and
shows a standard message after it has been visited. Therefore:

- Do not write `otherwise: {}`. The already-visited state is not an alternative
  reward branch.
- Do not write numeric `localStrings` values or message-operation codes.
- Do not write the standard already-visited message.
- Do not restate weekly reset and once-per-week behavior.
- Do write a sparse semantic patch when a custom object dynamically changes a
  non-default mechanical rule that cannot be derived from the map and content inputs.
- Apply proposal 10 when a custom reward contains genuine map-authored narrative.

### 18. Replay-Lossless Derived Effect Elision

Allow deterministic replay to regenerate low-level effect records, but retain concise
outcomes that make the human story understandable. This is the agreed compromise,
not a decisions-only transcript.

Verbose shape:

```yaml
- recruitCreatures: { actor: red, source: town/red/froisan-conflux@7.5, destination: town/red/froisan-conflux@7.5, creature: pixie, amount: 2, level: 0 }
- resources: { player: red, mode: relative, values: [{ gold: -50 }] }
- availableCreatures: { object: town/red/froisan-conflux@7.5, levels: [...] }
- army: { owner: town/red/froisan-conflux@7.5, slot: 2, insert: { creature: pixie, count: 2 } }
```

Current replay-lossless compromise:

```yaml
- recruit:
    at: town/red/froisan-conflux@7.5
    units: { pixie: 2 }
    paid: { gold: 50 }
```

Replay regenerates the exact availability and army mutations. `paid` remains because
it explains the action to a human and provides a useful audit check. Apply the same
rule elsewhere: retain compact costs, rewards, casualties, ownership, learned skills,
and other meaningful outcomes even when the server can recompute them.

This proposal is replay-lossless but intentionally does not preserve a one-to-one
copy of every original effect packet. Exact per-turn save comparison remains the
required proof that no game state was lost.

## Stabilization Pass

The corpus review after the first implementation accepted these additional rules.
They supersede any older example above when the two differ.

1. Resource trades use named, outcome-bearing exchanges and batch adjacent trades
   at one market. Other market modes use distinct verbs such as `sendResources`,
   `sellCreatures`, `buyArtifacts`, `sellArtifacts`, `learnSkills`,
   `transformUndead`, `sacrificeCreatures`, and `sacrificeArtifacts`.

   ```yaml
   - trade:
       at: town/bocc-stronghold@25.4.1
       exchanges:
         - { sold: { ore: 1 }, received: { gold: 25 } }
   ```

2. Omit a neutral field only when its record variant defines one exact default.
   This includes false `silent`, zero kills, empty injury lists, empty reverse
   artifact moves, absent boats, and scalar `endTurn`/`ready`. A Magic Well's zero
   bonus is semantic and becomes `usedToday`, not a dropped no-op.

3. Hero recruitment is one transaction with a readable cost. `replacement`,
   `arrival`, and `boat` appear only for nonstandard cases.

   ```yaml
   - hire: { at: town/froisan-conflux@7.5, hero: ciele, paid: { gold: 2500 } }
   ```

4. Army arrangement uses `swapStacks`, `mergeStacks` with `into`, or `splitStack`
   with a positive `count`; there is no numeric mode or zero whole-stack sentinel.

5. Recruitment `remaining` is sparse and contains only positive pools.

6. Movement has one grammar: a one-tile move has `to`; every multi-tile ordinary
   move has `to` plus direction `steps`. Surface positions omit z. There is no
   coordinate-list movement form.

7. Battle walking and applied paths are one `move`; approach, attack, result, and
   immediate retaliation are one `attack`. An uninterrupted siege-gate walk keeps
   one complete path and folds the gate state into that move.

8. A battle spell request, cast notification, status changes, mana payment, and
   damage or healing are one `cast`. Stable names such as `poison`, `regenerate`,
   `fear`, `unbind`, and `cloned` replace numeric effect/property codes where
   applicable. The automatic `catapultShot` notification is represented by the
   semantic `catapult` result instead of being repeated. Other engine-selected
   war-machine attacks remain readable attacks marked `automatic: true`, so replay
   observes them without submitting a duplicate decision.

   Automatic healing is similarly one frozen semantic event rather than separate
   `spellCast` and `unitsChanged` implementation packets:

   ```yaml
   - heal:
       by: attacker/first-aid-tents
       target: attacker/cavaliers
       amount: 10
       after: { count: 2, topHp: 48, at: 98 }
   ```

   `after` prevents a reader or future engine from having to reconstruct hidden
   battle state in order to know exactly what survived the heal.

9. Monoliths, whirlpools, and teleporters are one `teleport` with `via`, semantic
   exit selection, final position or `blocked`, and optional folded `approach`.

10. Header player data is sparse. `initialPlayers` is a delta from resolved
    `players`; disabled/default settings are absent; `initialState.heroes` is a
    readable mapping keyed by hero rather than one huge flow line.

11. Generic constructor prefixes such as `creatureGeneratorCommon` and
    `shrineOfMagicLevel1` are omitted when the specific object name resolves
    uniquely.

12. Query IDs are not public transcript data. A choice is nested under the action
   that caused it; level-up answers use `chooseSkill`, and closing a deterministic
   activity uses `finish`. A meaningful numeric answer, including zero, remains.
   When a guarded-dwelling prompt follows a battle, its aftermath carries a readable
   named `answer` and not the server query number.

13. Authored H3M text is normalized at import: a complete non-ASCII UTF-8 source
    string is preserved; otherwise the configured legacy map encoding is converted
    to UTF-8. VGT never performs independent mojibake repair.

14. The turn player is the lexical owner/actor default. Current-player hero and
    object references omit that player; cross-player references remain explicit.

15. Two or more consecutive actions by one hero form `with: <hero>` plus a nested
    `actions` list. A scene never leaks beyond its mapping.

16. An adjacent same-hero ordinary move that directly causes an encounter, visit,
   capture, or teleport is nested as `approach`. Standalone travel remains `move`.

Automatic notifications that the next player's resident hero is visiting their own
town are omitted between `endTurn` and the next turn header. They are repeated server
bookkeeping, not new visits in the story.

## Final Corpus Pass

The long-game space profile produced one further accepted iteration. These rules
supersede older examples in this document where they differ.

1. A turn begins with a compact absolute snapshot for its current player. Routine
   all-player income and refresh packet summaries are omitted; deterministic replay
   regenerates them.

   ```yaml
   turn:
     date: 2/3/4
     player: red
     resources: { wood: 17, ore: 9, gold: 12350 }
     heroes: { grindan: { movement: 1710, mana: 42 } }
   ```

2. Weekly availability is absolute, not a growth delta. Town pools and all
   non-deterministic dwelling pools, notably Refugee Camps, are grouped. Ordinary
   map-dwelling growth remains omitted only when it is fixed by content and game
   settings.

   ```yaml
   - weeklyAvailability:
       towns:
         town/froisan-conflux@7.5: { pixie: 38, airElemental: 12 }
       dwellings:
         refugee-camp@10.18: { angel: 1 }
   ```

3. Realized stochastic facts are frozen transcript authority. They are never
   replaced with a request to reroll using a future engine's RNG. Identical weekly
   rewards and wandering-monster spawns are grouped without hiding their exact
   locations, amounts, or counts.

   ```yaml
   - weeklyRewards:
       - { at: [mystical-garden@184.4, mystical-garden@249.8], reward: { gold: 500 } }
   - spawns:
       pegasus:
         - { at: [227, 84, 1], count: 26 }
         - { at: [115, 174], count: 22 }
   ```

4. Resource trades use natural named quantities. Repeated resource pairs in one
   uninterrupted market transaction are aggregated.

   ```yaml
   - trade:
       at: town/bocc-stronghold@25.4.1
       exchanges: ["5 ore for 100 gold", "2 sulfur for 1 crystal"]
   ```

5. Consecutive artifact transfers name the artifacts and share their holder
   context. Exact slots remain because they are required to reproduce equipment and
   backpack order.

   ```yaml
   - moveArtifacts:
       from: ignissa
       to: tyris
       artifacts:
         - { artifact: spellBook, from: 17, to: 17 }
         - { artifact: badgeOfCourage, from: 9, to: 10 }
   ```

6. An uninterrupted stack-management session records its strategically meaningful
   final armies rather than a packet-like series of swaps, merges, and one-creature
   splits. The result is absolute and replay applies it through server-owned state
   changes.

   ```yaml
   - arrangeArmies:
       armies:
         thunar: [{ slot: 0, creature: troglodyte, count: 1 }]
         dace: [{ slot: 0, creature: troglodyte, count: 46 }]
   ```

   If undead stacks temporarily leave and re-enter an army during the collapsed
   session, the recorder adds `refreshUndeadMorale: [hero]`. This rare replay
   detail preserves byte-identical bonus ordering without exposing the discarded
   low-level moves.

7. The main transcript contains a complete strategic battle boundary and is
   independently replayable without its adjacent `*.battles.yaml` companion.
   `battle.id` joins the optional tactical detail; it is not described as
   `tactics`, because it is simply a stable battle index. The main outcome owns
   the explicit result and winning side, winner and loser, casualties, survivors,
   permanently created units, experience, mana, rewards, absolute post-battle
   armies, ordered aftermath, and the RNG state required to continue the game.

   The append-only tactical companion depends on the main transcript, declares it
   with `main: game.vgt`, and stores only `id`, `randomBefore`, and `events`. It
   does not repeat the initial unit roster or any strategic outcome field. Full
   tactical replay reconstructs battle inputs from the main game state and merges
   these fields by id. Main-only fast-forward replay never opens the companion.

   The main RNG boundary uses `random.beforeContinuation` after tactics and before
   strategic cleanup. A complete `random.atContinuation` is present only when
   cleanup changes it; otherwise the former is also the continuation state. The
   tactical-only `randomBefore` freezes participant RNG after battle setup and
   before the first tactical decision. Participant-scoped biased streams use the
   compact object-id mapping:

   ```yaml
   combatAbility:
     "11180": 471476864
     "18652": { generator: 2140859999, bias: -2000 }
   ```

   These states include level-up RNG only for participating hero types. A recorded
   `levelUp` also freezes the rolled primary skill and offered secondary skills;
   replay corrects regenerated values before applying `chooseSkill`. This keeps
   action-by-action replay independent of battle-AI calculations that may otherwise
   advance random streams between setup and the first decision.

   Turn-relative terminal results omit their redundant player (`playerEnd: {
   result: loss }`). Derived town-visit notifications for a different player during
   elimination or between-turn cleanup are not part of the acting player's story
   and are omitted.

8. Every tactical attack freezes its exact geometry. `from` is always the cell from
   which the attack was made; `via` is the exact preceding route and is absent only
   without movement; `targetAt` is the target cell. Return routes remain explicit.

   ```yaml
   - attack:
       by: attacker/air-elementals
       via: [182, 183, 167, 149]
       from: 133
       target: defender/stone-gargoyles
       targetAt: 150
       damage: 48
   ```

   Tactical replay must not rely on a future pathfinder selecting the same canonical
   route. The recorded route and final attacker cell are the frozen facts.

   Each hit also carries its compact frozen post-state. `damage` and `killed` are
   the readable story; `after` prevents poison, death-blow, rebirth, or later rule
   changes from making the surviving stack ambiguous:

   ```yaml
   - attack:
       by: attacker/wyvern-monarches
       from: 157
       target: defender/royal-griffins
       targetAt: 172
       damage: 278
       killed: 11
       after: { count: 11, topHp: 17, at: 172 }
   ```

9. A zero strategic stack-experience value is the defined default and is absent.
   Recruitment pool `remaining`, derived artifact effects, derived chosen-skill
   effects, and `removeDefender: true` are also absent because each duplicates one
   unambiguous semantic action or outcome.

10. Movement names only first sightings with durable strategic value: towns,
    creature banks, Libraries of Enlightenment, quest objects, major/relic
    artifacts, level 5+ dwellings, and enemy-owned armies. Routine resources,
    decoration, roads, and ordinary pickups remain implicit in the visibility
    change, so exploration stays readable.

    ```yaml
    - move:
        hero: grindan
        to: [48, 31]
        steps: "NE*3 E"
        discovers: [town/black-quarter@50.30, dragon-utopia@52.27]
    ```

    `discovers` is an assertion, never a state-changing replay input. Normal FoW
    logic reveals the map first; replay then requires the independently derived
    first-sighting set to match the transcript exactly. An absent field asserts an
    empty set, so replay also rejects an omitted sighting before the next decision
    or document boundary. Deterministic effect records may occur between the action
    and its assertion. The same field is available on a teleport. A displacement
    that has no enclosing travel action uses `discovers: { hero, objects }` as a
    checked observation.

## Implementation Sequence

All phases below are implemented. The numbered items are retained as the dependency
order and review checklist, not as pending work.

### Phase 1: Grammar And Expansion Model

1. Specify how every refined record expands into decisions and independent effects.
2. Define lexical context inheritance and ensure it never crosses a document.
3. Define the shortened identifier grammar, including mod namespaces and ambiguity
   errors.
4. Define signed delta versus absolute assignment rules.
5. Update the JSON Schema only after these rules are unambiguous.

### Phase 2: Adventure Transactions And World Chapters

1. Implement build, recruitment, resource, availability, movement, and encounter
   transaction codecs.
2. Implement the sole direction grammar and verify every reconstructed coordinate.
3. Group global income, refresh, and weekly growth before player turns.
4. Keep per-player timed events in the appropriate player turn.
5. Apply the narrative text filtering policy and preserve custom story text.
6. Replace full rewardable configurations with sparse mechanical refreshes.

### Phase 3: Continuous Battle Stories

1. Buffer a battle from start through cleanup without changing event order.
2. Build the descriptive battle-local roster.
3. Remove exactly redundant `startAction` records.
4. Group passive choices and attack/retaliation exchanges.
5. Consolidate battle results and adjacent post-battle effects.
6. Preserve uncommon battle mechanics explicitly and fail strict recording if a
   material mechanic has no semantic representation.

### Phase 4: Derived Effect Elision

1. Classify each current effect as independent, human-relevant but derived, or wholly
   derived implementation detail.
2. Retain concise costs and outcomes according to proposal 18.
3. Remove only effects that normal deterministic replay regenerates.
4. Compare every turn save byte-for-byte before accepting an effect classification.

### Phase 5: Static-Language Tooling

1. Update the full JSON Schema with closed discriminated record variants.
2. Keep unknown record keys and unknown fields invalid.
3. Add schema examples for all eighteen refinements.
4. Generate or prototype typed models for at least one statically typed language to
   verify that context, unions, and compact forms remain practical.

## Validation Matrix

Run AI-versus-AI recordings across:

- small, medium, large, and extra-large maps;
- surface-only and surface-plus-underground maps;
- no water, normal water, and islands;
- two through eight players, including teams and simultaneous turns;
- human-style timers and no timers;
- towns with base and upgraded creature pools;
- standard and custom rewardable objects;
- custom map narrative, riddles, and decision-bearing prompts;
- battles with duplicate creature stacks, summons, clones, transformations, spells,
  war machines, sieges, retreat, surrender, and hero defeat;
- gates, monoliths, whirlpools, boats, and adventure spell teleports.

For every accepted transcript:

1. Validate all YAML documents against the current schema.
2. Reject every unmodelled material decision or effect in strict mode.
3. Replay through normal server requests.
4. Compare every reconstructed per-turn save byte-for-byte.
5. Verify that compact identifiers resolve uniquely.
6. Expand compact direction runs and compare every tile.
7. Confirm that standard UI boilerplate is absent.
8. Confirm that custom narrative and meaningful choices remain readable.
9. Confirm that each battle has one continuous block and stable descriptive unit
   names.
10. Measure raw text size, gzip size, record count, and longest line against current
    VGT 4.

## Completion Criteria

The refinement is complete when all eighteen proposals have a closed schema and
writer/replayer coverage, strict mode reports no unmodelled material changes across
the validation matrix, every saved turn matches byte-for-byte, and a reader can
follow the main decisions and outcomes without understanding VCMI network packets or
internal serialization structures.

## Implementation Evidence

The completed implementation was exercised with eight AI-versus-AI recordings: six
generated maps plus Arrogance and Pandora's Box. They cover small through extra-large
sizes, two through eight player slots, computer-only slots, teams, surface-only and
two-level terrain, no water through islands, normal and strong monsters, and 20
through 40 player turns per game. All eight streams passed strict and JSON Schema
validation. Semantic replay reconstructed all 250 recorded per-turn saves
byte-for-byte. The final text corpus is 610,335 bytes, 96,106 words, and 7,308 lines;
deterministic `gzip -9` is 90,534 bytes.

A separate 36-turn regression containing seven automatic ballista shots also matched
every saved turn after those engine-selected attacks were marked `automatic: true`.
Against the retained pre-refinement recordings for the same authored maps and turn
counts, Arrogance is 32.20% smaller and Pandora's Box is 29.03% smaller as raw text.

An exact four-turn before/after recording on the same map, setup, and random seed
changed from 102,700 to 44,892 bytes (56.29% smaller), 14,833 to 6,565 words (55.74%
fewer), and 776 to 492 lines (36.60% fewer). Deterministic `gzip -9` size changed
from 9,514 to 6,273 bytes (34.07% smaller). The refined member matched all four
turn-state saves exactly.
