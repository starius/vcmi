# VGT Readability And Compression Refinement Plan

## Status And Scope

This document records the reviewed follow-up plan for making VGT more compact and
more like a game story written by a human. It refines, but does not yet implement,
the current VGT 4 format described in
[`VGT_Event_Transcript_Plan.md`](VGT_Event_Transcript_Plan.md).

All eighteen reviewed proposals are included. The examples in this document are
proposed syntax, not syntax accepted by the current schema or replayer.

The default requirement is semantic losslessness: a refined record must expand into
the same ordered decisions and independent material outcomes, or provide everything
needed to issue the same server requests. Proposal 18 deliberately uses the narrower
definition of replay losslessness: deterministic effects may be regenerated, but
important human-readable outcomes remain in the transcript.

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
2. Inherit the turn player and support a focused hero or location scene.
3. Shorten identifiers reversibly without a glossary.
4. Encode long ordinary movement routes as run-length directions.
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

Current shape:

```yaml
- buildStructure: { actor: red, town: town/red/froisan-conflux/at-7-5-0, building: core/cityHall }
- resources: { player: red, mode: relative, values: [{ gold: -5000 }] }
- town: { id: town/red/froisan-conflux/at-7-5-0, build: [core/cityHall, core/extraTownHall, core/extraCityHall], builtThisTurn: 1 }
```

Proposed shape after applying the identifier rules from proposal 3:

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

The player in the turn header is the default actor and affected player. A nested
scene may establish a default location, town, or hero. Exceptions such as `world`, a
timer, or another player remain explicit.

```yaml
turn: { date: 1/1/1, player: red }
actions:
  - at: town/red/froisan-conflux@7.5
    build: cityHall
    cost: { gold: 5000 }
  - with: red/grindan
    move: { to: [8, 5, 0] }
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

For a long uninterrupted ordinary route, use run-length compass directions from the
hero's known starting position. Retain the exact destination as a readable check.

```yaml
- move:
    hero: red/grindan
    to: [18, 7, 0]
    steps: "SE E SE E*3 NE N NE"
```

This expands exactly to the original tile sequence. Short routes may keep coordinate
lists. A gate, monolith, whirlpool, spell teleport, embark, disembark, blocking visit,
or level change ends the ordinary route and remains a separate semantic event.

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
- available:
    town/red/froisan-conflux@7.5:
      pixie: 18
      airElemental: 1
      waterElemental: 0
```

The content definition supplies absent, unavailable dwelling levels. A pool that
permits base and upgraded creatures must retain that shared-pool relationship in a
compact explicit form; it must not be flattened into two independent counts.

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
    remaining: { waterElemental: 0, airElemental: 1, pixie: 18 }
```

The batch expands to the same ordered server requests. It must split when another
decision, query, failure, or material side effect interrupts recruitment.

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
          hero: red/grindan
          spell: lightningBolt
          target: defender/stone-gargoyles
          mana: -10
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
- walk: { unit: attacker/water-elementals, to: 58 }
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
- move:
    unit: attacker/air-elementals
    path: [2, 3, 4, 5, 6, 7, 8]
- attack:
    by: attacker/air-elementals
    target: defender/stone-gargoyles
```

When one side has multiple stacks of the same creature, assign stable ordinals for
the lifetime of the battle:

```text
defender/stone-gargoyles#1
defender/stone-gargoyles#2
```

Raw stack IDs appear only in the roster. Summoned, cloned, transformed, and newly
created stacks receive equally descriptive stable names when they appear.

### 14. Combat Exchanges

Combine an initiating attack, its material target state, and an immediate retaliation
into one combat exchange. Do not repeat attacker and creature identity inside nested
attack results.

```yaml
- attack:
    by: attacker/air-elementals
    target: defender/stone-gargoyles
    damage: 48
    killed: 3
    left: { units: 14, hp: 11, at: 44 }
    retaliation:
      damage: 16
      killed: 0
      left: { units: 7, hp: 9, at: 27 }
```

Fields such as `healthDelta: -48` and `operation: update` are derived and omitted.
Named exceptional facts such as luck, morale, ranged fire, no retaliation, or a
special effect remain explicit. Unknown flags must not be silently discarded.

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
  removeDefender: true
```

Omit empty artifact moves and repeated winner/loser spellings. Preserve surrender,
retreat, hero defeat, artifacts, necromancy, creature transformation, and any other
non-default outcome explicitly.

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

Proposed compromise:

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

## Implementation Sequence

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
2. Implement direction-route expansion and verify every reconstructed coordinate.
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
6. Expand compact routes and compare every tile.
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
