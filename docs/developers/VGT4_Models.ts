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
export type Position2D = readonly [x: number, y: number];
export type Position3D = readonly [x: number, y: number, z: number];
export type ResourceMap = Readonly<Record<Identifier, number>>;

export interface Header {
    readonly vgt: 4;
    readonly format: "VCMI readable event transcript";
    readonly engine: { readonly version: string };
    readonly map: {
        readonly uri: string;
        readonly name: string;
        readonly source?: "generated-map-file";
        readonly hash: { readonly algorithm: "sha256"; readonly value: string };
        readonly objectNameCounter: number;
        readonly generator?: unknown;
        readonly initialGenerator?: unknown;
    };
    readonly settings: Readonly<Record<string, unknown>>;
    readonly players: Readonly<Record<PlayerColor, PlayerSettings>>;
    readonly initialPlayers: Readonly<Record<PlayerColor, PlayerSettings>>;
    readonly initialState: {
        readonly heroes: readonly InitialHero[];
    };
}

export interface PlayerSettings {
    readonly controller: "human" | "ai";
    readonly faction: Identifier;
    readonly hero: Identifier;
    readonly heroPortrait: Identifier;
    readonly heroNameTextId: string;
    readonly startingBonus: string;
    readonly handicap: {
        readonly resources: ResourceMap;
        readonly incomePercent: number;
        readonly growthPercent: number;
    };
    readonly name: string;
    readonly connections: readonly number[];
    readonly computerOnly: boolean;
}

export interface InitialHero {
    readonly id: Identifier;
    readonly type: Identifier;
    readonly owner: PlayerColor;
    readonly position: Position3D;
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

export type Move = ActorContext & { readonly hero: Identifier; readonly transit?: boolean } & (
    | { readonly to: Position3D }
    | { readonly route: readonly [Position2D, Position2D, ...Position2D[]]; readonly z: number }
    | { readonly to: Position3D; readonly steps: string }
);

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
}

export interface Recruit extends ActorContext {
    readonly at: Identifier;
    readonly to?: Identifier;
    readonly units: Readonly<Record<Identifier, number>> | readonly RecruitUnit[];
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
    readonly actor: Actor;
    readonly side: "attacker" | "defender" | "none" | "invalid" | "allKnowing" | "unknown";
    readonly unit: BattleUnitName;
    readonly spell?: Identifier;
    readonly target?: readonly BattleTarget[];
}

export type BattleActionRecord =
    | { readonly none: BattleAction }
    | { readonly endTactics: BattleAction }
    | { readonly retreat: BattleAction }
    | { readonly surrender: BattleAction }
    | { readonly heroSpell: BattleAction }
    | { readonly walk: BattleAction }
    | { readonly wait: BattleAction | BattleUnitName | readonly BattleUnitName[] }
    | { readonly defend: BattleAction }
    | { readonly walkAndAttack: BattleAction }
    | { readonly shoot: BattleAction }
    | { readonly catapult: BattleAction }
    | { readonly monsterSpell: BattleAction }
    | { readonly badMorale: BattleAction }
    | { readonly stackHeal: BattleAction }
    | { readonly walkAndCast: BattleAction };

export interface UnitStateAfterAttack {
    readonly units?: number;
    readonly hp?: number;
    readonly at?: number;
}

export interface Retaliation {
    readonly damage: number;
    readonly killed: number;
    readonly left?: UnitStateAfterAttack;
    readonly ranged?: boolean;
    readonly luck?: "good" | "bad";
    readonly deathBlow?: boolean;
    readonly spellLike?: boolean;
    readonly lifeDrain?: boolean;
}

export interface AttackExchange extends Retaliation {
    readonly by: BattleUnitName;
    readonly target: BattleUnitName;
    readonly retaliation?: Retaliation;
    /** A non-adjacent counterattack that could not be folded into its initiating exchange. */
    readonly counterattack?: true;
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

export type BattleEvent =
    | BattleActionRecord
    | { readonly attack: AttackExchange }
    | NamedBattleEffect
    | SemanticEffectRecord;

export interface BattleOutcome {
    readonly result?: string;
    readonly winnerSide?: string;
    readonly winner?: PlayerColor;
    readonly loser?: PlayerColor;
    readonly experience?: Readonly<Record<Identifier, number>>;
    readonly casualties?: {
        readonly attacker?: Readonly<Record<Identifier, number>>;
        readonly defender?: Readonly<Record<Identifier, number>>;
    };
    readonly artifactMoves?: readonly unknown[];
    readonly learnedSpells?: { readonly hero: Identifier; readonly spells: readonly Identifier[] };
    readonly grownArtifacts?: number;
    readonly dischargedArtifacts?: number;
    readonly raised?: { readonly creature: Identifier; readonly count: number };
    readonly removeDefender?: true;
    readonly aftermath?: readonly TranscriptRecord[];
}

export interface BattleScene {
    readonly id: number;
    readonly attacker?: Identifier;
    readonly defender?: Identifier;
    readonly units?: Readonly<Record<BattleUnitName, BattleRosterEntry>>;
    readonly events: readonly BattleEvent[];
    readonly outcome?: BattleOutcome;
}

export type SemanticEffectRecord =
    | { readonly resources: Readonly<Record<PlayerColor, ResourceMap>> }
    | { readonly setResources: Readonly<Record<PlayerColor, ResourceMap>> }
    | { readonly skills: Readonly<Record<Identifier, Readonly<Record<Identifier, number>>>> }
    | { readonly setSkills: Readonly<Record<Identifier, Readonly<Record<Identifier, number>>>> }
    | { readonly experience: Readonly<Record<Identifier, number>> }
    | { readonly setExperience: Readonly<Record<Identifier, number>> }
    | { readonly mana: Readonly<Record<Identifier, number>> }
    | { readonly setMana: Readonly<Record<Identifier, number>> }
    | { readonly setMovement: Readonly<Record<Identifier, number>> }
    | { readonly available: Readonly<Record<Identifier, CreaturePool>> }
    | { readonly growth: Readonly<Record<Identifier, CreaturePool>> }
    | { readonly income: Readonly<Record<PlayerColor, ResourceMap>> }
    | { readonly refresh: Readonly<Record<Identifier, { readonly movement?: number; readonly mana?: number }>> }
    | { readonly refresh: { readonly object: Identifier; readonly reward?: ResourceMap; readonly text?: string } }
    | { readonly week: { readonly type: string; readonly creature?: Identifier } }
    | { readonly capture: { readonly hero?: Identifier; readonly object: Identifier; readonly owner: PlayerColor; readonly income?: ResourceMap } }
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
    | { readonly visit: { readonly hero: Identifier; readonly object: Identifier } };

export type OtherDecisionRecord =
    | { readonly endTurn: ActorContext }
    | { readonly dismissHero: ActorContext & { readonly hero: Identifier } }
    | { readonly dig: ActorContext & { readonly hero: Identifier } }
    | { readonly pauseTimer: ActorContext }
    | { readonly ready: ActorContext }
    | { readonly castleTeleportHero: ActorContext & { readonly hero: Identifier; readonly destination: Identifier; readonly source: number } }
    | { readonly visitTownBuilding: ActorContext & { readonly town: Identifier; readonly building: Identifier } }
    | { readonly razeStructure: ActorContext & { readonly town: Identifier; readonly building: Identifier } }
    | { readonly spellResearch: ActorContext & { readonly town: Identifier; readonly spell: Identifier; readonly accepted: boolean } }
    | { readonly hireHero: ActorContext & { readonly town: Identifier; readonly hero: Identifier; readonly nextHero: Identifier } }
    | { readonly queryAnswer: ActorContext & { readonly query: number | "none" | "client"; readonly answer: number | null } }
    | { readonly castAdventureSpell: ActorContext & { readonly hero: Identifier; readonly spell: Identifier; readonly position: Position3D } };

export interface RemoveEffect {
    readonly object: Identifier;
    readonly initiator: PlayerColor;
}

export interface HeroRecruitedEffect {
    readonly player: PlayerColor;
    readonly hero: Identifier;
    readonly town: Identifier;
    readonly tile: Position3D;
    readonly boat: Identifier;
}

export interface LevelUpEffect {
    readonly player: PlayerColor;
    readonly hero: Identifier;
    readonly primary: Identifier;
    readonly choices: readonly Identifier[];
    readonly query: number | "none" | "client";
}

export interface VisibilityRun {
    readonly y: number;
    readonly z: number;
    /** Inclusive first and last x coordinate. */
    readonly x: readonly [first: number, last: number];
}

export interface QueryEffect {
    readonly kind: "blockingDialog" | "exchangeDialog" | "openWindow" | "garrisonDialog" | "teleportDialog";
    readonly query: number | "none" | "client";
    readonly player?: PlayerColor;
    readonly selection?: boolean;
    readonly cancel?: boolean;
    readonly choices?: number;
    readonly hero1?: Identifier;
    readonly hero2?: Identifier;
    readonly window?: string;
    readonly object?: Identifier;
    readonly visitor?: Identifier;
    readonly hero?: Identifier;
    readonly removableUnits?: boolean;
    readonly title?: string;
    readonly firstExit?: Identifier;
    readonly exits?: number;
    readonly impassable?: boolean;
}

export type AuditEffectRecord =
    | { readonly turnStart: { readonly player: PlayerColor; readonly query: number | "none" | "client" } }
    | { readonly turnEnd: { readonly player: PlayerColor; readonly timer: { readonly start: unknown; readonly end: unknown } } }
    | { readonly playerEnd: { readonly player: PlayerColor; readonly result: "victory" | "loss" | "ingame"; readonly silent: boolean } }
    | { readonly stackExperience: { readonly army: Identifier; readonly values: readonly { readonly slot: number; readonly amount: number }[] } }
    | { readonly spells: { readonly hero: Identifier; readonly mode: "learn" | "forget"; readonly spells: readonly Identifier[] } }
    | { readonly visibility: { readonly player: PlayerColor; readonly mode: "hidden" | "revealed" | "unknown"; readonly runs: readonly VisibilityRun[] } }
    | { readonly objectPosition: { readonly object: Identifier; readonly to: Position3D; readonly initiator: PlayerColor } }
    | { readonly remove: RemoveEffect }
    | { readonly townHeroes: { readonly town: Identifier; readonly visiting: Identifier; readonly garrison: Identifier } }
    | { readonly heroRecruited: HeroRecruitedEffect }
    | { readonly heroOwner: { readonly hero: Identifier; readonly player: PlayerColor; readonly boat: Identifier } }
    | { readonly quest: { readonly player: PlayerColor; readonly object: Identifier } }
    | { readonly availableArtifacts: { readonly object: Identifier; readonly artifacts: readonly Identifier[] } }
    | { readonly levelUp: LevelUpEffect }
    | { readonly query: QueryEffect }
    /* Lower-frequency audit operations are closed in the JSON Schema; their nested
       engine-shaped payloads remain intentionally opaque in this hand-written prototype. */
    | { readonly localState: Readonly<Record<string, unknown>> }
    | { readonly newObject: Readonly<Record<string, unknown>> }
    | { readonly availableHero: Readonly<Record<string, unknown>> }
    | { readonly town: Readonly<Record<string, unknown>> }
    | { readonly artifact: Readonly<Record<string, unknown>> }
    | { readonly artifacts: Readonly<Record<string, unknown>> }
    | { readonly army: Readonly<Record<string, unknown>> }
    | { readonly adventureSpell: Readonly<Record<string, unknown>> };

export type TranscriptRecord =
    | { readonly move: Move }
    | { readonly build: Build }
    | { readonly recruit: Recruit }
    | { readonly encounter: Encounter }
    | { readonly battle: BattleScene }
    | SemanticEffectRecord
    | OtherDecisionRecord
    | AuditEffectRecord
    | { readonly unmodelled: { readonly stream: "decision" | "effect"; readonly pack: string; readonly material: boolean } }
    | { readonly at: Identifier; readonly build: Identifier; readonly cost: ResourceMap; readonly actor?: Actor }
    | { readonly with: Identifier; readonly move: Omit<Move, "hero" | "actor">; readonly actor?: Actor };
