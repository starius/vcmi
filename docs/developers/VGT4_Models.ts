/*
 * Static-type prototype for config/schemas/vgt-4.schema.json.
 *
 * This file is documentation, not runtime parser code. It demonstrates that the
 * refined YAML grammar maps to closed discriminated unions without packet-style
 * catch-all records. A generator may produce equivalent interfaces from the JSON
 * Schema descriptions.
 */

export type Identifier = string;
export type Actor = string;
export type PlayerColor = string;
/** [x,y] is surface; the three-item form requires a nonzero z. */
export type Position = readonly [x: number, y: number] | readonly [x: number, y: number, nonzeroZ: number];
export type ResourceMap = Readonly<Record<Identifier, number>>;

export interface Header {
    readonly vgt: 4;
    readonly format: "VCMI readable event transcript";
    readonly engine: { readonly version: string };
    readonly map: {
        readonly uri: string;
        readonly name?: string;
        readonly source?: "generated-map-file";
        readonly textEncoding:
            | { readonly source: "utf-8"; readonly stored: "utf-8" }
            | { readonly source: "h3m-auto"; readonly fallback: string; readonly stored: "utf-8" };
        readonly hash: { readonly algorithm: "sha256"; readonly value: string };
        readonly objectNameCounter: number;
        readonly generator?: MapGenerator;
        readonly initialGenerator?: MapGenerator;
    };
    readonly settings: {
        readonly start: string;
        readonly startTime: number;
        readonly difficulty: string | number;
        readonly randomSeed: number;
        readonly simturns?: Readonly<Record<string, unknown>>;
        readonly timer?: Readonly<Record<string, unknown>>;
        readonly extraOptions?: Readonly<Record<string, unknown>>;
        readonly gameSettingsOverrides?: Readonly<Record<string, unknown>>;
    };
    readonly players: Readonly<Record<PlayerColor, PlayerSettings>>;
    readonly initialPlayers: Readonly<Record<PlayerColor, PlayerSettings>>;
    readonly initialState: {
        readonly heroes: Readonly<Record<Identifier, InitialHero>>;
    };
}

/** Delta from one-level/no-water/normal-monsters and sequential AI/random players. */
export interface MapGenerator {
    readonly width: number;
    readonly height: number;
    readonly humanOrComputerPlayers: number;
    readonly levels?: number;
    readonly teams?: number;
    readonly computerOnlyPlayers?: number;
    readonly computerOnlyTeams?: number;
    readonly water?: "random" | "none" | "normal" | "islands" | number;
    readonly monsters?: "random" | "weak" | "normal" | "strong" | number;
    readonly template?: string;
    readonly roads?: readonly Identifier[];
    /** Present colors when the slot set differs from sequential active-player defaults. */
    readonly slots?: readonly PlayerColor[];
    readonly players?: Readonly<Record<PlayerColor, {
        readonly type: "human" | "ai" | "computerOnly" | "unknown";
        readonly faction: Identifier;
        readonly hero: Identifier;
        readonly team: number | "none";
    }>>;
}

export interface PlayerSettings {
    readonly controller?: "human" | "ai";
    readonly faction?: Identifier;
    readonly hero?: Identifier;
    readonly heroPortrait?: Identifier;
    readonly heroNameTextId?: string;
    readonly startingBonus?: string;
    readonly handicap?: {
        readonly resources?: ResourceMap;
        readonly incomePercent?: number;
        readonly growthPercent?: number;
    };
    readonly name?: string;
    readonly connections?: readonly number[];
    readonly computerOnly?: boolean;
}

export interface InitialHero {
    readonly position: Position;
    readonly experience: number;
    readonly mana: number;
    readonly movement: number;
    readonly artifacts: readonly unknown[];
    readonly army: readonly {
        readonly slot: number;
        readonly creature: Identifier;
        readonly count: number;
    }[];
}

export interface TurnDocument {
    readonly turn: { readonly date: string | 0; readonly player: PlayerColor };
    readonly actions: readonly TranscriptRecord[];
}

export interface WorldDocument {
    readonly world: {
        readonly date: string | 0;
        readonly phase: "startup" | "newDay";
    };
    readonly events: readonly TranscriptRecord[];
}

export type Transcript = readonly [Header, ...(TurnDocument | WorldDocument)[]];

export interface ActorContext {
    /** Omitted when equal to the enclosing turn player. */
    readonly actor?: Actor;
}

export type MovementBody = { readonly transit?: boolean } & (
    | { readonly to: Position; readonly steps?: never }
    | { readonly to: Position; /** Compass runs such as "NE E*7". */ readonly steps: string }
);

export type Move = ActorContext & { readonly hero: Identifier } & MovementBody;
export type Approach = MovementBody;

export interface Build extends ActorContext {
    readonly town: Identifier;
    readonly building: Identifier;
    /** Positive resources paid; derived resource effects are regenerated. */
    readonly cost: ResourceMap;
}

export interface RecruitUnit {
    readonly creature: Identifier;
    readonly count: number;
    readonly slot?: number;
    readonly pool?: Identifier;
}

export interface Recruit extends ActorContext {
    readonly at: Identifier;
    readonly to?: Identifier;
    readonly units: Readonly<Record<Identifier, number>> | readonly RecruitUnit[];
    readonly pool?: Identifier;
    readonly paid?: ResourceMap;
    readonly remaining?: Readonly<Record<Identifier, number>>;
}

export interface Choice {
    readonly name: string;
    /** Numeric server answer, including null when cancellation has no value. */
    readonly value: number | null;
}

export interface Encounter extends ActorContext {
    readonly hero: Identifier;
    readonly with: Identifier;
    readonly approach?: Approach;
    /** A semantic quest request; one artifact stays scalar for readability. */
    readonly quest?: {
        readonly bring: Identifier | readonly [Identifier, Identifier, ...Identifier[]];
    };
    readonly choice?: Choice;
    /** Custom map-authored text only; generated UI messages are omitted. */
    readonly text?: string;
    readonly outcome?: readonly TranscriptRecord[];
}

export interface SharedCreaturePool {
    readonly creatures: readonly [Identifier, Identifier, ...Identifier[]];
    readonly count: number;
}

export interface CreaturePool {
    readonly [creatureOrShared: string]: number | readonly SharedCreaturePool[] | undefined;
    readonly shared?: readonly SharedCreaturePool[];
}

export type BattleUnitName = string;

export interface BattleRosterEntry {
    /** Raw stack IDs occur only here. */
    readonly stack: number;
    readonly owner: PlayerColor;
    readonly count: number;
}

export interface BattleTarget {
    readonly unit?: BattleUnitName;
    readonly hex?: number;
}

export interface BattleAction {
    /** Omitted together when unit is attacker/... or defender/.... */
    readonly actor?: Actor;
    readonly side?: "attacker" | "defender" | "none" | "invalid" | "allKnowing" | "unknown";
    readonly unit: BattleUnitName;
    readonly spell?: Identifier;
    readonly target?: readonly BattleTarget[];
}

export type BattleActionRecord =
    | { readonly none: BattleAction }
    | { readonly endTactics: BattleAction }
    | { readonly retreat: BattleAction }
    | { readonly surrender: BattleAction }
    | { readonly wait: BattleAction | BattleUnitName | readonly BattleUnitName[] }
    | { readonly defend: BattleAction | BattleUnitName | readonly BattleUnitName[] }
    | { readonly catapult: BattleAction }
    | { readonly badMorale: BattleAction }
    | { readonly stackHeal: BattleAction }
    | { readonly walkAndCast: BattleAction };

export interface BattleMove {
    /** Both are omitted when unit carries the battle-local side prefix. */
    readonly actor?: Actor;
    readonly side?: "attacker" | "defender";
    readonly unit: BattleUnitName;
    readonly to: number;
    readonly path?: readonly number[];
    readonly teleport?: boolean;
    /** Siege gate state change folded into this uninterrupted applied path. */
    readonly gate?: string;
}

export interface Retaliation {
    readonly damage?: number;
    readonly killed?: number;
    /** Ordered outcomes when a breath or area retaliation affects multiple stacks. */
    readonly hits?: readonly [BattleHit, BattleHit, ...BattleHit[]];
    readonly luck?: "good" | "bad";
    readonly deathBlow?: boolean;
    readonly spellLike?: boolean;
    readonly lifeDrain?: boolean;
}

/** One target affected by a breath, area, or other multi-target creature attack. */
export interface BattleHit {
    readonly target: BattleUnitName;
    readonly damage: number;
    readonly killed?: number;
    readonly spell?: Identifier;
}

/** Secondary damage resolved within an attack exchange. */
export interface BattleAttackEffect {
    readonly effect: Identifier;
    readonly target: BattleUnitName;
    readonly damage: number;
    readonly killed?: number;
}

export interface AttackExchange {
    readonly actor?: Actor;
    readonly side?: "attacker" | "defender";
    readonly by: BattleUnitName;
    readonly target?: BattleUnitName;
    /** Exact original tactical targets; outcomes are expressed by the named fields. */
    readonly aim: readonly BattleTarget[];
    readonly approach?: readonly number[];
    readonly returnsTo?: number;
    readonly returnPath?: readonly [number, number, ...number[]];
    readonly ranged?: boolean;
    /** The engine selected this action; replay must not submit it again. */
    readonly automatic?: boolean;
    /** Present for double shots or other deterministic multi-strike attacks. */
    readonly strikes?: number;
    readonly damage?: number;
    readonly killed?: number;
    /** Ordered logical outcomes when one attack affects multiple stacks. */
    readonly hits?: readonly [BattleHit, BattleHit, ...BattleHit[]];
    /** Named spell effects applied by a creature attack. */
    readonly applies?: readonly Identifier[];
    /** Secondary damage such as death stare or reflected fire-shield damage. */
    readonly effects?: readonly [BattleAttackEffect, ...BattleAttackEffect[]];
    readonly retaliation?: Retaliation;
    readonly luck?: "good" | "bad";
    readonly deathBlow?: boolean;
    readonly spellLike?: boolean;
    readonly lifeDrain?: boolean;
}

export interface BattleCast {
    readonly actor?: Actor;
    /** Retained for hero casts; creature aliases already carry their side. */
    readonly side?: "attacker" | "defender";
    readonly caster: Identifier;
    readonly spell: Identifier;
    readonly aim: readonly BattleTarget[];
    readonly target?: BattleUnitName;
    readonly mana?: number;
    readonly damage?: number;
    readonly killed?: number;
    /** Total hit points restored by this cast. */
    readonly healed?: number;
}

export interface NamedBattleEffect {
    readonly event: string;
    readonly unit?: BattleUnitName;
    readonly path?: readonly number[];
    readonly distance?: number;
    readonly teleporting?: boolean;
    readonly side?: string;
    readonly spell?: Identifier;
    readonly caster?: BattleUnitName | "spell";
    readonly at?: number;
    readonly hero?: boolean;
    readonly changes?: readonly unknown[] | number;
    readonly action?: Readonly<Record<string, unknown>>;
}

/** A bonus whose target is the enclosing battle. */
export interface BattleBonusEffect {
    readonly type: Identifier;
    readonly value: number;
    readonly duration?: Identifier | readonly Identifier[];
    readonly source?: Identifier;
    readonly sourceKind?: Identifier;
    readonly subtype?: string;
    readonly turns?: number;
    readonly valueKind?: Identifier;
    readonly stacking?: string;
    readonly text?: string;
}

export type BattleEvent =
    | BattleActionRecord
    | { readonly move: BattleMove }
    | { readonly attack: AttackExchange }
    | { readonly cast: BattleCast }
    | { readonly bonus: BattleBonusEffect }
    | NamedBattleEffect
    | SemanticEffectRecord;

export interface BattleRound {
    readonly round: number;
    readonly events: readonly BattleEvent[];
}

export interface BattleOutcome {
    readonly result: string;
    readonly winnerSide: string;
    readonly winner: PlayerColor;
    readonly loser: PlayerColor;
    readonly experience?: Readonly<Record<Identifier, number>>;
    readonly casualties: {
        readonly attacker?: Readonly<Record<Identifier, number>>;
        readonly defender?: Readonly<Record<Identifier, number>>;
    };
	/** Permanent surviving count by battle unit name; dead units are omitted. */
	readonly survivors: Readonly<Record<BattleUnitName, number>>;
	/** Permanent stacks created during combat; count is the initially created amount. */
	readonly createdUnits?: Readonly<Record<BattleUnitName, {
		readonly creature: Identifier;
		readonly count: number;
		readonly hex: number;
	}>>;
	/** Compact RNG continuation at the tactical/strategic boundary. */
	readonly continuation: BattleRandomizerContinuation;
    /** Net persistent hero mana changes caused by the battle. */
    readonly mana?: Readonly<Record<Identifier, number>>;
    readonly artifactMoves?: readonly unknown[];
    readonly learnedSpells?: { readonly hero: Identifier; readonly spells: readonly Identifier[] };
    readonly grownArtifacts?: number;
    readonly dischargedArtifacts?: number;
    readonly raised?: { readonly creature: Identifier; readonly count: number };
    readonly removeDefender?: true;
    readonly aftermath?: readonly TranscriptRecord[];
}

export interface BattleRandomizerContinuation {
	readonly global: string;
	readonly goodMorale?: readonly BattleRandomizerStream[];
	readonly badMorale?: readonly BattleRandomizerStream[];
	readonly goodLuck?: readonly BattleRandomizerStream[];
	readonly badLuck?: readonly BattleRandomizerStream[];
	readonly combatAbility?: readonly BattleRandomizerStream[];
}

export interface BattleRandomizerStream {
	readonly object: number;
	readonly state: { readonly generator: string; readonly bias?: number };
}

export interface BattleScene {
    readonly id: number;
    readonly attacker?: Identifier;
    readonly defender?: Identifier;
    readonly units?: Readonly<Record<BattleUnitName, BattleRosterEntry>>;
    readonly events: readonly (BattleEvent | BattleRound)[];
    /** Required semantic boundary for replay modes that skip tactical events. */
    readonly outcome: BattleOutcome;
}

/** One capture record serves both the action and its semantic ownership outcome. */
export interface CaptureRecord extends ActorContext {
    readonly hero?: Identifier;
    readonly object: Identifier;
    readonly owner?: PlayerColor;
    readonly approach?: Approach;
    readonly opened?: OpenedActivity;
    readonly income?: ResourceMap;
}

export type SemanticEffectRecord =
    | { readonly resources: ResourceMap | Readonly<Record<PlayerColor, ResourceMap>> }
    | { readonly setResources: ResourceMap | Readonly<Record<PlayerColor, ResourceMap>> }
    | { readonly skills: Readonly<Record<Identifier, Readonly<Record<Identifier, number>>>> }
    | { readonly setSkills: Readonly<Record<Identifier, Readonly<Record<Identifier, number>>>> }
    | { readonly experience: Readonly<Record<Identifier, number>> }
    | { readonly setExperience: Readonly<Record<Identifier, number>> }
    | { readonly mana: Readonly<Record<Identifier, number>> }
    | { readonly setMana: Readonly<Record<Identifier, number>> }
    | { readonly setMovement: Readonly<Record<Identifier, number>> }
    | { readonly available: Readonly<Record<Identifier, CreaturePool>> }
    | { readonly availability: { readonly towns?: Readonly<Record<Identifier, CreaturePool>>; readonly dwellings?: Readonly<Record<Identifier, CreaturePool>> } }
    | { readonly growth: Readonly<Record<Identifier, CreaturePool>> }
    | { readonly income: Readonly<Record<PlayerColor, ResourceMap>> }
    | { readonly refresh: Readonly<Record<Identifier, { readonly movement?: number; readonly mana?: number }>> }
    | { readonly refresh: { readonly object: Identifier; readonly reward?: ResourceMap; readonly text?: string } }
    | { readonly week: { readonly type: string; readonly creature?: Identifier } }
    | { readonly townless: { readonly player?: PlayerColor; readonly days?: number; readonly cleared?: true } }
    | { readonly capture: CaptureRecord }
    | { readonly bonus: {
        readonly targetKind: "object" | "player" | "battle" | "heroCommander";
        readonly target: Identifier;
        readonly type: Identifier;
        readonly value: number;
        readonly duration?: Identifier | readonly Identifier[];
        readonly source?: Identifier;
        readonly sourceKind?: Identifier;
        readonly subtype?: string;
        readonly turns?: number;
        readonly valueKind?: Identifier;
        readonly stacking?: string;
        /** Genuine map-authored description only. */
        readonly text?: string;
    } }
    | { readonly story: { readonly hero?: Identifier; readonly at?: Identifier; readonly text: string; readonly choice?: Choice } }
    | { readonly collects: { readonly resources: ResourceMap } }
    | { readonly joins: { readonly hero: Identifier; readonly units: Readonly<Record<Identifier, number>> } }
    | { readonly finds: { readonly artifact: Identifier; readonly instance?: number; readonly slot?: number | string } }
    | { readonly reveal: { readonly by: Identifier; readonly player?: PlayerColor } }
    | { readonly appears: { readonly object: Identifier | null; readonly at?: Position; readonly owner?: PlayerColor; readonly units?: readonly unknown[] } }
    | { readonly opened: OpenedActivity }
    | { readonly visit: { readonly hero: Identifier; readonly object: Identifier } };

export interface OpenedActivity {
    readonly activity: string;
    readonly with?: Identifier;
    readonly object?: Identifier;
    readonly hero?: Identifier;
    readonly at?: Identifier;
}

export type OtherDecisionRecord =
    | "endTurn"
    | "ready"
    | { readonly endTurn: ActorContext }
    | { readonly dismissHero: ActorContext & { readonly hero: Identifier } }
    | { readonly dig: ActorContext & { readonly hero: Identifier } }
    | { readonly pauseTimer: ActorContext }
    | { readonly castleTeleportHero: ActorContext & { readonly hero: Identifier; readonly destination: Identifier; readonly source: number } }
    | { readonly visitTownBuilding: ActorContext & { readonly town: Identifier; readonly building: Identifier } }
    | { readonly razeStructure: ActorContext & { readonly town: Identifier; readonly building: Identifier } }
    | { readonly spellResearch: ActorContext & { readonly town: Identifier; readonly spell: Identifier; readonly accepted: boolean } }
    | { readonly hire: ActorContext & { readonly at: Identifier; readonly hero: Identifier; readonly paid: ResourceMap; readonly replacement?: Identifier; readonly arrival?: Position; readonly boat?: Identifier } }
    | { readonly answer: ActorContext & { readonly name?: Identifier; readonly value: number | null } }
    | { readonly finish: ActorContext & { readonly activity: string } }
    | { readonly chooseSkill: ActorContext & { readonly hero: Identifier; readonly skill: Identifier } }
    | { readonly swapStacks: ActorContext & { readonly from: ArmySlot; readonly to: ArmySlot } }
    | { readonly mergeStacks: ActorContext & { readonly from: ArmySlot; readonly into: ArmySlot } }
    | { readonly splitStack: ActorContext & { readonly from: ArmySlot; readonly to: ArmySlot; readonly count: number } }
    | { readonly castAdventureSpell: ActorContext & { readonly hero: Identifier; readonly spell: Identifier; readonly position: Position } };

export interface ArmySlot { readonly army: Identifier; readonly slot: number }
export type SingleResourceAmount = Readonly<Record<Identifier, number>>;

export type MarketDecisionRecord =
    | { readonly trade: ActorContext & { readonly at: Identifier; readonly exchanges: readonly { readonly sold: SingleResourceAmount; readonly received: SingleResourceAmount }[] } }
    | { readonly sendResources: ActorContext & { readonly at: Identifier; readonly to: PlayerColor; readonly resources: ResourceMap } }
    | { readonly sellCreatures: ActorContext & { readonly at: Identifier; readonly hero: Identifier; readonly sales: readonly { readonly slot: number; readonly creature: Identifier; readonly count: number; readonly received: SingleResourceAmount }[] } }
    | { readonly buyArtifacts: ActorContext & { readonly at: Identifier; readonly hero: Identifier; readonly purchases: readonly { readonly artifact: Identifier; readonly paid: SingleResourceAmount }[] } }
    | { readonly sellArtifacts: ActorContext & { readonly at: Identifier; readonly hero: Identifier; readonly sales: readonly { readonly artifact: Identifier; readonly instance: number; readonly received: SingleResourceAmount }[] } }
    | { readonly transformUndead: ActorContext & { readonly at: Identifier; readonly hero?: Identifier; readonly stacks: readonly { readonly slot: number; readonly creature: Identifier }[] } }
    | { readonly learnSkills: ActorContext & { readonly at: Identifier; readonly hero: Identifier; readonly skills: readonly Identifier[] } }
    | { readonly sacrificeCreatures: ActorContext & { readonly at: Identifier; readonly hero: Identifier; readonly stacks: readonly { readonly slot: number; readonly creature: Identifier; readonly count: number }[] } }
    | { readonly sacrificeArtifacts: ActorContext & { readonly at: Identifier; readonly hero: Identifier; readonly artifacts: readonly { readonly artifact: Identifier; readonly instance: number }[] } };

export type TravelDecisionRecord =
    | { readonly visit: ActorContext & { readonly hero: Identifier; readonly object: Identifier; readonly approach?: Approach } }
    | { readonly capture: CaptureRecord }
    | { readonly teleport: ActorContext & { readonly hero: Identifier; readonly via: Identifier; readonly exit?: Identifier; readonly to?: Position; readonly blocked?: true; readonly random?: true; readonly approach?: Approach; readonly outcome?: readonly TranscriptRecord[] } };

export interface RemoveEffect {
    readonly object: Identifier;
    readonly initiator?: PlayerColor;
}

export interface HeroRecruitedEffect {
    readonly player: PlayerColor;
    readonly hero: Identifier;
    readonly town: Identifier;
    readonly tile: Position;
    readonly boat?: Identifier;
}

export interface LevelUpEffect {
    readonly hero: Identifier;
    readonly primary: Identifier;
    readonly choices?: readonly Identifier[];
}

export interface VisibilityRun {
    readonly y: number;
    /** Absent means surface; an explicit value is nonzero. */
    readonly z?: number;
    /** Inclusive first and last x coordinate. */
    readonly x: readonly [first: number, last: number];
}

export type AuditEffectRecord =
    | { readonly turnEnd: { readonly player: PlayerColor; readonly timer: { readonly start: unknown; readonly end: unknown } } }
    | { readonly playerEnd: { readonly player: PlayerColor; readonly result: "victory" | "loss" | "ingame"; readonly silent?: boolean } }
    | { readonly stackExperience: { readonly army: Identifier; readonly values: readonly { readonly slot: number; readonly amount: number }[] } }
    | { readonly spells: { readonly hero: Identifier; readonly mode: "learn" | "forget"; readonly spells: readonly Identifier[] } }
    | { readonly visibility: { readonly player?: PlayerColor; readonly mode: "hidden" | "revealed" | "unknown"; readonly runs: readonly VisibilityRun[] } }
    | { readonly objectPosition: { readonly object: Identifier; readonly to: Position; readonly initiator?: PlayerColor } }
    | { readonly remove: RemoveEffect }
    | { readonly townHeroes: { readonly town: Identifier; readonly visiting: Identifier; readonly garrison: Identifier } }
    | { readonly heroRecruited: HeroRecruitedEffect }
    | { readonly heroOwner: { readonly hero: Identifier; readonly player: PlayerColor; readonly boat?: Identifier } }
    | { readonly quest: { readonly player?: PlayerColor; readonly object: Identifier } }
    | { readonly availableArtifacts: { readonly object: Identifier; readonly artifacts: readonly Identifier[] } }
    | { readonly levelUp: LevelUpEffect }
    /* Lower-frequency audit operations are closed in the JSON Schema; their nested
       engine-shaped payloads remain intentionally opaque in this hand-written prototype. */
    | { readonly localState: Readonly<Record<string, unknown>> }
    | { readonly availableHero: Readonly<Record<string, unknown>> }
    | { readonly town: Readonly<Record<string, unknown>> }
    | { readonly artifact: Readonly<Record<string, unknown>> }
    | { readonly artifacts: Readonly<Record<string, unknown>> }
    | { readonly army: Readonly<Record<string, unknown>> }
    | { readonly adventureSpell: Readonly<Record<string, unknown>> };

export type HeroSceneAction =
    | { readonly move: MovementBody }
    | { readonly encounter: Omit<Encounter, "actor" | "hero"> }
    | { readonly visit: { readonly object: Identifier; readonly approach?: Approach } }
    | { readonly capture: { readonly object: Identifier; readonly owner?: PlayerColor; readonly opened?: OpenedActivity; readonly approach?: Approach } }
    | { readonly teleport: { readonly via: Identifier; readonly exit?: Identifier; readonly to?: Position; readonly blocked?: true; readonly random?: true; readonly approach?: Approach } }
    | { readonly levelUp: Omit<LevelUpEffect, "hero"> }
    | { readonly chooseSkill: { readonly skill: Identifier } }
    | { readonly dig: Readonly<Record<never, never>> }
    | { readonly dismissHero: Readonly<Record<never, never>> };

export interface HeroScene {
    readonly with: Identifier;
    /** The writer only creates a scene for two or more consecutive actions. */
    readonly actions: readonly [HeroSceneAction, HeroSceneAction, ...HeroSceneAction[]];
}

export type TranscriptRecord =
    | { readonly move: Move }
    | { readonly build: Build }
    | { readonly recruit: Recruit }
    | { readonly encounter: Encounter }
    | { readonly battle: BattleScene }
    | SemanticEffectRecord
    | OtherDecisionRecord
    | MarketDecisionRecord
    | TravelDecisionRecord
    | AuditEffectRecord
    | { readonly unmodelled: { readonly stream: "decision" | "effect"; readonly pack: string; readonly material: boolean } }
    | HeroScene;
