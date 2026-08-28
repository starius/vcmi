/*
 * ClassicBattleAITest.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "../StdInc.h"

#include "../../AI/BattleAI/BattleAI.h"
#include "../../AI/BattleAI/Classic/ClassicAttackEvaluator.h"
#include "../../AI/BattleAI/Classic/ClassicBattleController.h"
#include "../../AI/BattleAI/Classic/ClassicBattleDecision.h"
#include "../../AI/BattleAI/Classic/ClassicBattleRng.h"
#include "../../AI/BattleAI/Classic/ClassicBattleStateView.h"
#include "../../AI/BattleAI/Classic/ClassicCombatValue.h"
#include "../../AI/BattleAI/Classic/ClassicDecisionTrace.h"
#include "../../AI/BattleAI/Classic/ClassicRetreatEvaluator.h"
#include "../../AI/BattleAI/Classic/ClassicRulesAdapter.h"
#include "../../AI/BattleAI/Classic/ClassicSpellEvaluator.h"
#include "../../lib/bonuses/Bonus.h"
#include "../../lib/BattleFieldHandler.h"
#include "../../lib/battle/BattleLayout.h"
#include "../../lib/callback/CBattleCallback.h"
#include "../../lib/callback/IClient.h"
#include "../../lib/entities/artifact/CArtifactInstance.h"
#include "../../lib/entities/hero/CHeroHandler.h"
#include "../../lib/gameState/CGameState.h"
#include "../../lib/json/JsonNode.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/mapObjects/CGTownInstance.h"
#include "../../lib/modding/IdentifierStorage.h"
#include "../../lib/modding/ModScope.h"
#include "../../lib/networkPacks/PacksForClientBattle.h"
#include "../../lib/ObstacleHandler.h"
#include "../../lib/battle/CObstacleInstance.h"
#include "../../lib/CPlayerState.h"
#include "../../lib/spells/AbilityCaster.h"
#include "../../lib/spells/ISpellMechanics.h"
#include "../../server/CGameHandler.h"
#include "../mock/TinyH3MBuilder.h"
#include "../server/battles/BattleTestFixture.h"

namespace
{
std::string jsonEscape(std::string_view input)
{
	std::ostringstream escaped;
	for(const unsigned char character : input)
	{
		switch(character)
		{
			case '"':
				escaped << "\\\"";
				break;
			case '\\':
				escaped << "\\\\";
				break;
			case '\b':
				escaped << "\\b";
				break;
			case '\f':
				escaped << "\\f";
				break;
			case '\n':
				escaped << "\\n";
				break;
			case '\r':
				escaped << "\\r";
				break;
			case '\t':
				escaped << "\\t";
				break;
			default:
				if(character < 0x20)
				escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
						<< static_cast<int32_t>(character) << std::dec;
				else
					escaped << character;
		}
	}
	return escaped.str();
}

class UpperBoundClassicBattleAIRng final : public IClassicBattleAIRng
{
public:
	int32_t nextIntInclusive(int32_t lower, int32_t upper) override
	{
		return std::max(lower, upper);
	}
};

class RecordingBattleAIClient final : public IClient
{
public:
	int requests = 0;

	std::optional<BattleAction> makeSurrenderRetreatDecision(
		PlayerColor player,
		const BattleID & battleID,
		const BattleStateInfoForRetreat & battleState) override
	{
		return std::nullopt;
	}

	int sendRequest(const CPackForServer & request, PlayerColor player, bool waitTillRealize) override
	{
		return ++requests;
	}
};

class ClassicBattleAIIntegrationTest : public BattleTestFixture
{
protected:
	void SetUp() override
	{
		BattleTestFixture::SetUp();
		startGame();
		startBattle();
		battle()->tacticDistance = 0;
	}

	std::shared_ptr<CBattleInfoCallback> battleCallback()
	{
		return std::shared_ptr<CBattleInfoCallback>(
			battle(),
			[](CBattleInfoCallback *)
			{
			}
		);
	}

	void removeAllStacks()
	{
		BattleUnitsChanged removal;
		removal.battleID = BattleID(0);
		for(const CStack * stack : battle()->battleGetAllStacks(false))
			removal.changedStacks.emplace_back(stack->unitId(), UnitChanges::EOperation::REMOVE);
		gameHandler->sendAndApply(removal);
	}
};

class ClassicBattleAISiegeIntegrationTest : public TinyMapGameTest
{

protected:
	std::shared_ptr<CGameHandler> gameHandler;
	RecordingGameServer server;
	Services * gameServices() override
	{
		return LIBRARY;
	}

	void configurePlayer(PlayerSettings & settings) const override
	{
		settings.bonus = PlayerStartingBonus::GOLD;
	}

	void SetUp() override
	{
		TinyMapGameTest::SetUp();
		const CreatureID token(0);
		TinyH3M::TinyH3MBuilder builder(EMapFormat::SOD);
		builder
			.size(36, false)
			.name("ClassicBattleAISiege")
			.playerActive(PlayerColor(0))
			.playerActive(PlayerColor(1))
			.hero({5, 5, 0}, HeroTypeID(0), PlayerColor(0)).heroGarrison({{token, 1}})
			.hero({12, 12, 0}, HeroTypeID(1), PlayerColor(1)).heroGarrison({{token, 1}})
			.town({10, 10, 0}, FactionID::CASTLE, PlayerColor(1))
			.townFortification(3);
		startWithMap(std::move(builder));

		server.gameState = gameState();
		gameHandler = std::make_shared<CGameHandler>(server, gameState());
		gameHandler->randomizer->setSeed(BattleTestFixture::seed);
		CGHeroInstance * attacker = findHeroByOwner(PlayerColor(0));
		CGHeroInstance * defender = findHeroByOwner(PlayerColor(1));
		CGTownInstance * town = findFirst<CGTownInstance>();
		ASSERT_NE(attacker, nullptr);
		ASSERT_NE(defender, nullptr);
		ASSERT_NE(town, nullptr);

		BattleSideArray<const CGHeroInstance *> heroes = {attacker, defender};
		BattleSideArray<const CArmedInstance *> armies = {attacker, defender};
		const int3 tile(10, 10, 0);
		const BattleLayout layout = BattleLayout::createDefaultLayout(*gameState(), attacker, defender);
		const std::string battlefieldName = "core:sand_shore";
		const BattleField battlefield(
			*LIBRARY->identifiers()->getIdentifier(ModScope::scopeGame(), "battlefield", battlefieldName));
		BattleStart start;
		start.info = BattleInfo::setupBattle(
			gameState().get(),
			tile,
			gameState()->getTile(tile)->getTerrainID(),
			battlefield,
			armies,
			heroes,
			layout,
			nullptr
		);
		start.battleID = BattleID(0);
		gameHandler->sendAndApply(start);
		battle()->townID = town->id;
		battle()->tacticDistance = 0;
		battle()->obstacles.clear();
	}

	void TearDown() override
	{
		gameHandler.reset();
		TinyMapGameTest::TearDown();
	}

	BattleInfo * battle() const
	{
		return gameState()->currentBattles.front().get();
	}

	CStack * addStack(BattleSide side, CreatureID creature, BattleHex position, int32_t count)
	{
		battle::UnitInfo info;
		info.id = battle()->battleNextUnitId();
		info.count = count;
		info.type = creature;
		info.side = side;
		info.position = position;
		BattleUnitsChanged change;
		change.battleID = BattleID(0);
		change.changedStacks.emplace_back(info.id, UnitChanges::EOperation::ADD);
		info.save(change.changedStacks.back().data);
		gameHandler->sendAndApply(change);
		return battle()->getStack(info.id);
	}

	std::shared_ptr<CBattleInfoCallback> battleCallback()
	{
		return std::shared_ptr<CBattleInfoCallback>(battle(), [](CBattleInfoCallback *) {});
	}
};

class ClassicBattleAIProbe : public BattleTestFixture
{
	std::map<uint32_t, std::string> stackKeys;
	ClassicDecisionContext decisionContext;
	bool hasCanonicalHero = false;

	CStack * addCanonicalStack(
		BattleSide side,
		CreatureID creature,
		BattleHex position,
		int32_t count,
		SlotID slot)
	{
		const uint32_t id = battle()->battleNextUnitId();
		const CStackBasicDescriptor descriptor(creature, count);
		battle()->generateNewStack(id, descriptor, side, slot, position);
		CStack * result = battle()->getStack(id, false);
		if(!result)
			throw std::runtime_error("probe importer could not create a canonical stack");
		result->localInit(battle());
		return result;
	}

	std::shared_ptr<CBattleInfoCallback> probeBattleCallback() const
	{
		return std::shared_ptr<CBattleInfoCallback>(battle(), [](CBattleInfoCallback *) {});
	}

	static std::string key(const JsonNode & node)
	{
		if(node.isString())
			return node.String();
		return std::to_string(node["side"].Integer()) + ":"
			+ std::to_string(node["slot"].Integer()) + ":"
			+ std::to_string(node["summon_ordinal"].Integer());
	}

	static const char * actionName(EActionType type)
	{
		switch(type)
		{
			case EActionType::END_TACTIC_PHASE:
				return "END_TACTICS";
			case EActionType::RETREAT:
				return "RETREAT";
			case EActionType::HERO_SPELL:
				return "HERO_SPELL";
			case EActionType::WALK:
				return "MOVE";
			case EActionType::WAIT:
				return "WAIT";
			case EActionType::DEFEND:
				return "DEFEND";
			case EActionType::WALK_AND_ATTACK:
				return "MELEE";
			case EActionType::SHOOT:
				return "SHOOT";
			case EActionType::CATAPULT:
				return "CATAPULT";
			case EActionType::MONSTER_SPELL:
				return "CREATURE_SPELL";
			case EActionType::STACK_HEAL:
				return "HEAL";
			default:
				return "SKIP";
		}
	}

	static const char * wallPartName(const BattleAction & action)
	{
		if(action.actionType != EActionType::CATAPULT || action.target.empty())
			return nullptr;
		switch(action.target.front().hexValue.toInt())
		{
			case 50: return "keep";
			case 183: return "bottom_tower";
			case 182: return "bottom_wall";
			case 130: return "below_gate";
			case 78: return "over_gate";
			case 29: return "upper_wall";
			case 12: return "upper_tower";
			case 96: return "gate";
			default: return nullptr;
		}
	}

	static std::string backgroundStem(std::string value)
	{
		const size_t slash = value.find_last_of("/\\");
		if(slash != std::string::npos)
			value.erase(0, slash + 1);
		const size_t dot = value.find_last_of('.');
		if(dot != std::string::npos)
			value.erase(dot);
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
		{
			return static_cast<char>(std::tolower(character));
		});
		return value;
	}

	static BattleField canonicalBattlefield(const JsonNode & input, std::optional<int32_t> townFaction)
	{
		if(!input.isString())
			return BattleField(input.Integer());
		const std::string expected = backgroundStem(input.String());
		if(townFaction)
		{
			// Siege backgrounds are selected by the defended faction and are not
			// entries in VCMI's ordinary battlefield registry. Siege geometry is
			// carried by BattleInfo::SiegeInfo, so NONE is the exact battlefield
			// identity after validating the original PCX/faction pairing.
			static const std::array<const char *, 9> siegeBackgrounds = {{
				"sgcsback", "sgrmback", "sgtwback", "sginback", "sgncback",
				"sgdnback", "sgstback", "sgfrback", "sgelback"
			}};
			if(*townFaction < 0 || *townFaction >= static_cast<int32_t>(siegeBackgrounds.size())
			   || expected != siegeBackgrounds[*townFaction])
			{
				throw std::runtime_error(
					"canonical Heroes III siege background does not match the defended faction: "
					+ input.String());
			}
			return BattleField::NONE;
		}
		BattleField result = BattleField::NONE;
		LIBRARY->battlefields()->forEach(
			[&](const BattleFieldInfo * info, bool & stop)
			{
				if(backgroundStem(info->graphics.getName()) == expected)
				{
					result = info->battlefield;
					stop = true;
				}
			});
		if(result == BattleField::NONE)
			throw std::runtime_error("canonical Heroes III battle background has no VCMI mapping: " + input.String());
		return result;
	}

	void giveCanonicalArtifact(CGHeroInstance * hero, const JsonNode & input, ArtifactPosition position)
	{
		const ArtifactID artifact(input["artifact_id"].Integer());
		if(!artifact.hasValue() || !artifact.toArtifact())
			throw std::runtime_error("canonical hero contains an invalid artifact ID");

		NewArtifact request;
		request.artHolder = hero->id;
		request.artId = artifact;
		request.spellId = SpellID(input["spell_id"].Integer());
		request.pos = position;
		gameHandler->sendAndApply(request);

		const CArtifactInstance * actual = hero->getArt(position, false);
		if(!actual || actual->getTypeId() != artifact)
			throw std::runtime_error("probe importer could not reproduce a canonical artifact");
		if(actual->isScroll() && actual->getScrollSpellID() != request.spellId)
			throw std::runtime_error("probe importer could not reproduce a canonical spell scroll");
	}

	void configureHero(CGHeroInstance * hero, const JsonNode & input)
	{
		while(!hero->artifactsWorn.empty())
			hero->removeArtifact(hero->artifactsWorn.begin()->first);
		while(!hero->artifactsInBackpack.empty())
			hero->removeArtifact(ArtifactPosition::BACKPACK_START);
		hero->removeAllSpells();

		const HeroTypeID heroType(input["hero_id"].Integer());
		if(!heroType.hasValue() || heroType.getNum() >= static_cast<int32_t>(LIBRARY->heroh->size()))
			throw std::runtime_error("canonical hero contains an invalid SoD hero ID");
		hero->setHeroType(heroType);
		hero->level = input["level"].Integer();
		hero->setExperience(input["experience"].Integer(), ChangeValueMode::ABSOLUTE);

		const auto & secondary = input["secondary"].Vector();
		if(secondary.size() != 28)
			throw std::runtime_error("canonical hero must contain all 28 secondary-skill levels");
		for(size_t skill = 0; skill < secondary.size(); ++skill)
			hero->setSecSkillLevel(SecondarySkill(skill), secondary[skill].Integer(), ChangeValueMode::ABSOLUTE);

		const auto & primary = input["primary"].Vector();
		if(primary.size() != 4)
			throw std::runtime_error("canonical hero must contain all four primary skills");
		for(size_t skill = 0; skill < primary.size(); ++skill)
			hero->setPrimarySkill(PrimarySkill(skill), primary[skill].Integer(), ChangeValueMode::ABSOLUTE);

		for(const auto & bonus : hero->getHeroType()->specialty)
			hero->addNewBonus(bonus);

		for(const JsonNode & artifact : input["equipped"].Vector())
		{
			const ArtifactPosition position(artifact["slot"].Integer());
			if(position < ArtifactPosition(0) || position >= ArtifactPosition::BACKPACK_START)
				throw std::runtime_error("canonical equipped artifact contains an invalid slot");
			giveCanonicalArtifact(hero, artifact, position);
		}
		for(size_t index = 0; index < input["backpack"].Vector().size(); ++index)
			giveCanonicalArtifact(
				hero,
				input["backpack"][index],
				ArtifactPosition::BACKPACK_START + static_cast<int32_t>(index)
			);

		for(const JsonNode & spell : input["spellbook"].Vector())
		{
			const SpellID spellID(spell.Integer());
			if(!spellID.hasValue() || !spellID.toSpell())
				throw std::runtime_error("canonical hero contains an invalid spell ID");
			hero->addSpellToSpellbook(spellID);
		}
		hero->mana = input["mana"].Integer();

		if(hero->getHeroTypeID() != heroType
		   || hero->level != input["level"].Integer()
		   || hero->exp != input["experience"].Integer()
		   || hero->mana != input["mana"].Integer())
		{
			throw std::runtime_error("probe importer could not reproduce canonical hero identity or resources");
		}
		for(size_t skill = 0; skill < secondary.size(); ++skill)
			if(hero->getSecSkillLevel(SecondarySkill(skill)) != secondary[skill].Integer())
				throw std::runtime_error("probe importer could not reproduce canonical secondary skills");
		for(size_t skill = 0; skill < primary.size(); ++skill)
			if(hero->getBasePrimarySkillValue(PrimarySkill(skill)) != primary[skill].Integer())
				throw std::runtime_error("probe importer could not reproduce canonical primary skills");
		std::set<int32_t> expectedSpells;
		for(const JsonNode & spell : input["spellbook"].Vector())
			expectedSpells.insert(spell.Integer());
		std::set<int32_t> actualSpells;
		for(SpellID spell : hero->getSpellsInSpellbook())
			actualSpells.insert(spell.getNum());
		if(actualSpells != expectedSpells)
			throw std::runtime_error("probe importer could not reproduce the canonical spellbook");
	}

	void applyCanonicalEffect(CStack * target, const JsonNode & input)
	{
		const SpellID spellID(input["spell_id"].Integer());
		const CSpell * spell = spellID.toSpell();
		if(!spell)
			throw std::runtime_error("canonical stack effect has no SoD spell mapping");
		const int32_t mastery = input["mastery"].isNull() ? 0 : input["mastery"].Integer();
		const int32_t duration = input["duration"].Integer();
		spells::AbilityCaster caster(target, mastery);
		spells::BattleCast cast(battle(), &caster, spells::Mode::PASSIVE, spell);
		cast.setSpellLevel(mastery);
		cast.setEffectDuration(duration);
		spells::Target destination;
		destination.emplace_back(target);
		cast.applyEffects(gameHandler->spellcastEnvironment(), destination, false, true);
	}

	static EWallState canonicalWallState(const JsonNode & input)
	{
		if(input.isString())
		{
			const std::string value = input.String();
			if(value == "none") return EWallState::NONE;
			if(value == "destroyed") return EWallState::DESTROYED;
			if(value == "damaged") return EWallState::DAMAGED;
			if(value == "intact") return EWallState::INTACT;
			if(value == "reinforced") return EWallState::REINFORCED;
			throw std::runtime_error("canonical wall has an unknown state");
		}
		const int32_t value = input.Integer();
		if(value < static_cast<int32_t>(EWallState::NONE)
		   || value > static_cast<int32_t>(EWallState::REINFORCED))
			throw std::runtime_error("canonical wall state is outside the SoD range");
		return static_cast<EWallState>(value);
	}

	static EGateState canonicalGateState(const std::string & value)
	{
		if(value == "none") return EGateState::NONE;
		if(value == "closed") return EGateState::CLOSED;
		if(value == "opened") return EGateState::OPENED;
		if(value == "destroyed") return EGateState::DESTROYED;
		throw std::runtime_error("canonical gate has an unknown state");
	}

	void resetBattle(const JsonNode & request)
	{
		BattleTestFixture::TearDown();
		BattleTestFixture::SetUp();
		const JsonNode & stateInput = request["state"];
		if(request["metadata"]["hook"].String() == "AICheckRetreat")
		{
			const std::string activeKey = key(request["active_stack"]);
			const auto & stacks = stateInput["stacks"].Vector();
			const auto active = std::ranges::find_if(stacks, [&](const JsonNode & stack)
			{
				return key(stack["key"]) == activeKey;
			});
			if(active == stacks.end())
				throw std::runtime_error("canonical active stack does not exist");
			const JsonNode & side = stateInput["sides"][(*active)["effective_side"].Integer()];
			if(!side["hero"].isNull() && side["gold"].isNull())
				throw std::runtime_error("AICheckRetreat requires canonical player gold");
		}
		const JsonNode & battlefieldInput = stateInput["battlefield"];
		const bool hasTown = !battlefieldInput["town"].isNull();
		if(!hasTown && (!battlefieldInput["walls"].Struct().empty()
		   || battlefieldInput["gate"].String() != "none"
		   || battlefieldInput["moat"].Bool()))
		{
			throw std::runtime_error("canonical siege state has no defended town");
		}
		const FactionID townFaction = hasTown
			? FactionID(battlefieldInput["town"]["faction"].Integer())
			: FactionID::CASTLE;
		startGame(hasTown, townFaction);
		CGTownInstance * defendedTown = hasTown ? findFirst<CGTownInstance>() : nullptr;
		if(hasTown && !defendedTown)
			throw std::runtime_error("probe importer could not create the canonical defended town");
		if(defendedTown)
		{
			const int32_t fortification = battlefieldInput["town"]["fortification"].Integer();
			defendedTown->removeAllBuildings();
			if(fortification >= 1) defendedTown->addBuilding(BuildingID::FORT);
			if(fortification >= 2) defendedTown->addBuilding(BuildingID::CITADEL);
			if(fortification >= 3) defendedTown->addBuilding(BuildingID::CASTLE);
			if(defendedTown->getFactionID() != townFaction
			   || static_cast<int32_t>(defendedTown->fortLevel()) != fortification)
				throw std::runtime_error("probe importer could not reproduce canonical town identity or fortification");
		}

		hasCanonicalHero = false;
		for(int32_t sideIndex = 0; sideIndex < 2; ++sideIndex)
		{
			if(stateInput["sides"][sideIndex]["physical_side"].Integer() != sideIndex)
				throw std::runtime_error("canonical sides must be ordered by physical_side");
			const JsonNode & heroInput = stateInput["sides"][sideIndex]["hero"];
			if(heroInput.isNull())
				continue;
			hasCanonicalHero = true;
			configureHero(sideIndex == 0 ? attackerSideHero : defenderSideHero, heroInput);
		}

		startBattle();
		if(defendedTown)
			battle()->townID = defendedTown->id;
		static const std::array<std::pair<const char *, EWallPart>, 8> wallParts = {{
			{"keep", EWallPart::KEEP},
			{"bottom_tower", EWallPart::BOTTOM_TOWER},
			{"bottom_wall", EWallPart::BOTTOM_WALL},
			{"below_gate", EWallPart::BELOW_GATE},
			{"over_gate", EWallPart::OVER_GATE},
			{"upper_wall", EWallPart::UPPER_WALL},
			{"upper_tower", EWallPart::UPPER_TOWER},
			{"gate", EWallPart::GATE}
		}};
		for(const auto & [name, part] : wallParts)
		{
			const JsonNode & wall = battlefieldInput["walls"][name];
			if(!wall.isNull())
			{
				const EWallState expected = canonicalWallState(wall);
				battle()->si.wallState[part] = expected;
				if(battle()->si.wallState[part] != expected)
					throw std::runtime_error("probe importer could not reproduce a canonical wall state");
			}
		}
		const EGateState expectedGate = canonicalGateState(battlefieldInput["gate"].String());
		battle()->si.gateState = expectedGate;
		if(battle()->si.gateState != expectedGate)
			throw std::runtime_error("probe importer could not reproduce canonical gate state");
		if(probeBattleCallback()->hasMoat() != battlefieldInput["moat"].Bool())
			throw std::runtime_error("probe importer could not reproduce canonical moat state");
		const bool tacticsActive = stateInput["tactics"]["active"].Bool();
		const int64_t tacticsMastery = stateInput["tactics"]["distance"].Integer();
		const bool hasTacticsSide = !stateInput["tactics"]["side"].isNull();
		if(tacticsActive != (hasTacticsSide && tacticsMastery > 0))
			throw std::runtime_error("canonical tactics active, side, and distance fields are inconsistent");
		// Heroes III stores the mastery advantage (Basic=1, Advanced=2,
		// Expert=3); the legal deployment depth is 2*mastery+1 columns.
		// VCMI stores that already-expanded deployment depth.
		const int64_t tacticsDistance = tacticsActive ? 2 * tacticsMastery + 1 : 0;
		battle()->tacticDistance = tacticsDistance;
		battle()->tacticsSide = stateInput["tactics"]["side"].isNull()
			? BattleSide::NONE
			: static_cast<BattleSide>(stateInput["tactics"]["side"].Integer());
		if(battle()->getTacticDist() != tacticsDistance
		   || battle()->getTacticsSide() != (tacticsActive
			   ? static_cast<BattleSide>(stateInput["tactics"]["side"].Integer())
			   : BattleSide::NONE))
		{
			throw std::runtime_error("probe importer could not reproduce canonical tactics state");
		}
		battle()->terrainType = TerrainId(battlefieldInput["terrain"].Integer());
		const BattleField expectedBattlefield = canonicalBattlefield(
			battlefieldInput["battlefield_id"],
			hasTown ? std::optional<int32_t>(townFaction.getNum()) : std::nullopt);
		battle()->battlefieldType = expectedBattlefield;
		battle()->nodeHasChanged();
		battle()->obstacles.clear();
		for(const JsonNode & input : battlefieldInput["obstacles"].Vector())
		{
			const int32_t obstacleID = input["obstacle_id"].Integer();
			if(obstacleID >= 0)
			{
				if(obstacleID > 90)
					throw std::runtime_error("canonical obstacle contains an invalid obstacle ID");
				const ObstacleInfo * info = LIBRARY->obstacleHandler->getByName(std::to_string(obstacleID));
				if(!info || info->isAbsoluteObstacle
				   || input["owner"].Integer() != -1
				   || !input["visible"].Bool()
				   || input["damage"].Integer() != 0
				   || input["duration"].Integer() != 0
				   || input["effect_id"].Integer() != -1)
				{
					throw std::runtime_error("canonical static obstacle has spell-created state");
				}
				auto obstacle = std::make_shared<CObstacleInstance>();
				obstacle->ID = info->obstacle.getNum();
				obstacle->pos = BattleHex(input["hex"].Integer());
				obstacle->obstacleType = CObstacleInstance::USUAL;
				obstacle->uniqueID = static_cast<int32_t>(battle()->obstacles.size());
				battle()->obstacles.push_back(std::move(obstacle));
				continue;
			}

			const int32_t effect = input["effect_id"].Integer();
			if(effect < 10 || effect > 13)
				throw std::runtime_error("canonical spell-created obstacle has an unknown SoD effect");
			auto obstacle = std::make_shared<SpellCreatedObstacle>();
			obstacle->ID = effect;
			obstacle->pos = BattleHex(input["hex"].Integer());
			obstacle->uniqueID = static_cast<int32_t>(battle()->obstacles.size());
			obstacle->turnsRemaining = input["duration"].Integer();
			obstacle->casterSide = static_cast<BattleSide>(input["owner"].Integer());
			obstacle->minimalDamage = input["damage"].Integer();
			obstacle->hidden = !input["visible"].Bool();
			obstacle->revealed = input["visible"].Bool();
			obstacle->customSize.insert(obstacle->pos);
			switch(effect)
			{
				case 10: // Quicksand
					obstacle->passable = true;
					obstacle->trap = true;
					break;
				case 11: // Land Mine
					obstacle->passable = true;
					obstacle->removeOnTrigger = true;
					obstacle->trigger = SpellID(effect);
					break;
				case 12: // Force Field
					obstacle->passable = false;
					break;
				case 13: // Fire Wall
					obstacle->passable = true;
					obstacle->trigger = SpellID(effect);
					break;
			}
			battle()->obstacles.push_back(std::move(obstacle));
		}
		if(battlefieldInput["moat"].Bool())
		{
			// The lightweight test battle creates the town/fortification bonus but
			// does not run the scripted moat ability used by a live server. Rebuild
			// the canonical SoD obstacle after importing the explicit obstacle list
			// so danger-map and stopping-tile queries see the live-battle state.
			static const std::array<int32_t, 9> moatDamage = {{70, 70, 150, 90, 70, 90, 70, 90, 70}};
			static const std::array<int32_t, 10> ordinaryMoat = {{11, 28, 44, 61, 77, 111, 129, 146, 164, 181}};
			static const std::array<int32_t, 21> fortressMoat = {{
				10, 11, 27, 28, 43, 44, 60, 61, 76, 77, 94,
				110, 111, 128, 129, 145, 146, 163, 164, 180, 181}};
			const int32_t faction = townFaction.getNum();
			if(faction < 0 || faction >= static_cast<int32_t>(moatDamage.size()))
				throw std::runtime_error("canonical SoD moat has an unsupported faction");
			auto moat = std::make_shared<SpellCreatedObstacle>();
			moat->obstacleType = CObstacleInstance::MOAT;
			moat->uniqueID = static_cast<int32_t>(battle()->obstacles.size());
			moat->casterSide = BattleSide::DEFENDER;
			moat->minimalDamage = moatDamage[faction];
			moat->passable = true;
			if(townFaction == FactionID::FORTRESS)
			{
				for(const int32_t hex : fortressMoat)
					moat->customSize.insert(BattleHex(hex));
			}
			else
			{
				for(const int32_t hex : ordinaryMoat)
					moat->customSize.insert(BattleHex(hex));
			}
			battle()->obstacles.push_back(std::move(moat));
			battle()->nodeHasChanged();
			const auto obstacles = probeBattleCallback()->battleGetAllObstacles(BattleSide::DEFENDER);
			const bool imported = std::ranges::any_of(obstacles, [](const auto & obstacle)
			{
				const auto * importedMoat = dynamic_cast<const SpellCreatedObstacle *>(obstacle.get());
				return importedMoat
					&& importedMoat->obstacleType == CObstacleInstance::MOAT
					&& importedMoat->minimalDamage > 0
					&& importedMoat->getAffectedTiles().contains(BattleHex(111));
			});
			if(!imported)
				throw std::runtime_error("probe importer could not reproduce canonical moat geometry or damage");
		}
		if(battle()->terrainType.getNum() != battlefieldInput["terrain"].Integer()
		   || battle()->battlefieldType != expectedBattlefield)
		{
			throw std::runtime_error("probe importer could not reproduce canonical battlefield identity");
		}
		for(int32_t sideIndex = 0; sideIndex < 2; ++sideIndex)
		{
			const JsonNode & sideInput = stateInput["sides"][sideIndex];
			const int32_t expectedPlayer = sideInput["player_color"].Integer();
			if(expectedPlayer < PlayerColor::NEUTRAL.getNum()
			   || expectedPlayer >= PlayerColor::PLAYER_LIMIT_I)
				throw std::runtime_error("canonical side has an invalid SoD player color");
			battle()->getSide(static_cast<BattleSide>(sideIndex)).color = PlayerColor(expectedPlayer);
			if(battle()->getSidePlayer(static_cast<BattleSide>(sideIndex)).getNum() != expectedPlayer)
				throw std::runtime_error("probe importer could not reproduce canonical side ownership");
			if(sideInput["human"].Bool() != sideInput["local_human"].Bool())
				throw std::runtime_error("probe importer cannot represent a remote human as local auto-combat");
			if(PlayerColor(expectedPlayer).isValidPlayer())
			{
				PlayerState * playerState = gameState()->getPlayerState(PlayerColor(expectedPlayer));
				playerState->human = sideInput["human"].Bool();
				if(playerState->isHuman() != sideInput["human"].Bool())
					throw std::runtime_error("probe importer could not reproduce canonical human control");
			}
			else if(sideInput["human"].Bool() || sideInput["local_human"].Bool())
				throw std::runtime_error("a neutral side cannot be human controlled");
			if(!sideInput["gold"].isNull() && PlayerColor(expectedPlayer).isValidPlayer())
			{
				SetResources resources;
				resources.player = PlayerColor(expectedPlayer);
				resources.mode = ChangeValueMode::ABSOLUTE;
				resources.res[GameResID::GOLD] = sideInput["gold"].Integer();
				gameHandler->sendAndApply(resources);
				if(gameState()->getPlayerState(resources.player)->resources[GameResID::GOLD]
				   != sideInput["gold"].Integer())
				{
					throw std::runtime_error("probe importer could not reproduce canonical player gold");
				}
			}
			if(sideInput["retreated"].Bool() || sideInput["surrendered"].Bool())
				throw std::runtime_error("a canonical pre-decision state cannot contain a finished side");
			const int32_t summonedElemental = sideInput["summoned_elemental"].Integer();
			if(summonedElemental != -1
			   && summonedElemental != CreatureID::AIR_ELEMENTAL
			   && summonedElemental != 113
			   && summonedElemental != CreatureID::FIRE_ELEMENTAL
			   && summonedElemental != 115)
			{
				throw std::runtime_error("canonical summoned-elemental marker is outside the SoD elemental family");
			}
			const JsonNode & heroInput = stateInput["sides"][sideIndex]["hero"];
			if(heroInput.isNull())
				battle()->getSide(static_cast<BattleSide>(sideIndex)).heroID = ObjectInstanceID::NONE;
			else
				battle()->getSide(static_cast<BattleSide>(sideIndex)).castSpellsCount = heroInput["spell_cast"].Bool() ? 1 : 0;
			if(!stateInput["sides"][sideIndex]["enchanter_cooldown"].isNull())
				battle()->getSide(static_cast<BattleSide>(sideIndex)).enchanterCounter
					= stateInput["sides"][sideIndex]["enchanter_cooldown"].Integer();
		}

		const int64_t requestedRound = stateInput["round"].Integer();
		if(requestedRound < 0 || requestedRound > 10000)
			throw std::runtime_error("canonical round is outside the supported range");
		while(battle()->getRound() < requestedRound)
			battle()->nextRound();
		if(battle()->getRound() != requestedRound)
			throw std::runtime_error("probe importer could not reproduce the canonical round");

		BattleUnitsChanged removal;
		removal.battleID = BattleID(0);
		for(const CStack * existing : battle()->battleGetAllStacks(false))
			removal.changedStacks.emplace_back(existing->unitId(), UnitChanges::EOperation::REMOVE);
		gameHandler->sendAndApply(removal);

		stackKeys.clear();
		decisionContext.selectedCreatureSpells.clear();
		decisionContext.targetRecords.clear();
		decisionContext.secondPhase = !stateInput["second_phase"].isNull()
			&& stateInput["second_phase"].Bool();
		for(const JsonNode & input : request["state"]["stacks"].Vector())
		{
			const int64_t count = input["count"].Integer();
			if(count <= 0)
				throw std::runtime_error("probe importer requires a positive stack count");
			const bool hasNativeState = !input["native"].isNull();
			const int64_t originalCount = hasNativeState ? input["native"]["original_count"].Integer() : count;
			if(originalCount < count)
				throw std::runtime_error("canonical original stack count is smaller than its live count");
			const std::string canonicalKey = key(input["key"]);
			if(std::ranges::any_of(stackKeys, [&](const auto & entry) { return entry.second == canonicalKey; }))
				throw std::runtime_error("probe importer requires unique canonical stack keys");
			const int32_t creatureID = input["creature_id"].Integer();
			const size_t firstColon = canonicalKey.find(':');
			const size_t secondColon = canonicalKey.find(':', firstColon + 1);
			const int32_t keySlot = std::stoi(canonicalKey.substr(firstColon + 1, secondColon - firstColon - 1));
			const int32_t summonOrdinal = std::stoi(canonicalKey.substr(secondColon + 1));
			SlotID unitSlot;
			if(input["summoned"].Bool() || input["clone"].Bool())
				unitSlot = SlotID::SUMMONED_SLOT_PLACEHOLDER;
			else if(creatureID == CreatureID::ARROW_TOWERS)
				unitSlot = SlotID::ARROW_TOWERS_SLOT;
			else if(creatureID >= CreatureID::CATAPULT
				&& creatureID <= CreatureID::AMMO_CART)
				unitSlot = SlotID::WAR_MACHINES_SLOT;
			else
			{
				if(keySlot < 0 || keySlot >= GameConstants::ARMY_SIZE || summonOrdinal != 0)
					throw std::runtime_error("canonical ordinary stack has an invalid army slot or summon ordinal");
				unitSlot = SlotID(keySlot);
			}
			// SoD stores its three Arrow Tower pseudo-positions as drawing-grid
			// sentinels. VCMI deliberately uses a separate negative sentinel range;
			// translate at the differential boundary instead of leaking either
			// engine's representation into the canonical protocol.
			BattleHex importedPosition(input["position"].Integer());
			if(creatureID == CreatureID::ARROW_TOWERS)
			{
				switch(input["position"].Integer())
				{
					case 251: importedPosition = BattleHex(BattleHex::CASTLE_BOTTOM_TOWER); break;
					case 254: importedPosition = BattleHex(BattleHex::CASTLE_CENTRAL_TOWER); break;
					case 255: importedPosition = BattleHex(BattleHex::CASTLE_UPPER_TOWER); break;
					default: throw std::runtime_error("canonical Arrow Tower has an unknown SoD position sentinel");
				}
			}
			CStack * added = addCanonicalStack(
				static_cast<BattleSide>(input["physical_side"].Integer()),
				CreatureID(creatureID),
				importedPosition,
				originalCount,
				unitSlot);
			// BattleUnitsChanged stacks are created after BattleInfo::localInit, so
			// the fixture's inherited native-terrain bonuses are not propagated to
			// them. Reapply the three SoD native-terrain bonuses to the synthetic
			// stack when the canonical state proves that rule is active.
			if(hasNativeState
			   && added->isOnNativeTerrain()
			   && input["native"]["original_speed"].Integer() == added->getMovementRange() + 1)
			{
				added->addNewBonus(std::make_shared<Bonus>(
					BonusDuration::ONE_BATTLE, BonusType::STACKS_SPEED, BonusSource::TERRAIN_NATIVE, 1,
					BonusSourceID()));
				added->addNewBonus(std::make_shared<Bonus>(
					BonusDuration::ONE_BATTLE, BonusType::PRIMARY_SKILL, BonusSource::TERRAIN_NATIVE, 1,
					BonusSourceID(), BonusSubtypeID(PrimarySkill::ATTACK)));
				added->addNewBonus(std::make_shared<Bonus>(
					BonusDuration::ONE_BATTLE, BonusType::PRIMARY_SKILL, BonusSource::TERRAIN_NATIVE, 1,
					BonusSourceID(), BonusSubtypeID(PrimarySkill::DEFENSE)));
			}

			JsonNode state = added->save();
			JsonNode & unit = state["state"];
			const int64_t maxHealth = added->getMaxHealth();
			const int64_t topHealth = input["top_hp"].Integer();
			const int64_t shots = input["shots"].Integer();
			const int64_t retaliations = input["retaliations"].Integer();
			if(input["facing"].Integer() != 1 - input["physical_side"].Integer())
				throw std::runtime_error("probe importer cannot reproduce a nonstandard stack facing");
			if(topHealth < 1 || topHealth > maxHealth)
				throw std::runtime_error("top_hp is outside the selected creature's health range");
			if(shots < 0 || shots > added->shots.total())
				throw std::runtime_error("shots is outside the selected creature's ammunition range");
			if(retaliations < 0 || retaliations > added->counterAttacks.total())
				throw std::runtime_error("retaliations is outside the selected creature's range");
			unit["health"]["firstHPleft"].Integer() = topHealth;
			unit["health"]["fullUnits"].Integer() = count - 1;
			unit["health"]["resurrected"].Integer() = hasNativeState
				? input["native"]["resurrected"].Integer()
				: 0;
			unit["shots"]["used"].Integer() = added->shots.total() - shots;
			unit["counterAttacks"]["used"].Integer() = added->counterAttacks.total() - retaliations;
			unit["waiting"].Bool() = input["waited"].Bool();
			unit["waitedThisTurn"].Bool() = input["waited"].Bool();
			unit["moved"].Bool() = input["done"].Bool();
			const int64_t defendModifier = hasNativeState
				? input["native"]["modifiers"]["defend"].Integer()
				: 0;
			unit["defending"].Bool() = defendModifier > 0;
			unit["cloned"].Bool() = input["clone"].Bool();
			unit["summoned"].Bool() = input["summoned"].Bool();
			added->load(state);
			for(const JsonNode & effect : input["effects"].Vector())
				applyCanonicalEffect(added, effect);
			if(defendModifier > 0)
			{
				added->addNewBonus(std::make_shared<Bonus>(
					BonusDuration::STACK_GETS_TURN,
					BonusType::PRIMARY_SKILL,
					BonusSource::OTHER,
					defendModifier,
					BonusSourceID(),
					BonusSubtypeID(PrimarySkill::DEFENSE)));
				added->addNewBonus(std::make_shared<Bonus>(
					BonusDuration::STACK_GETS_TURN,
					BonusType::UNIT_DEFENDING,
					BonusSource::OTHER,
					0,
					BonusSourceID()));
			}
			if(input["disabled"].Bool() && !added->hasBonusOfType(BonusType::NOT_ACTIVE))
			{
				added->addNewBonus(std::make_shared<Bonus>(
					BonusDuration::ONE_BATTLE,
					BonusType::NOT_ACTIVE,
					BonusSource::OTHER,
					0,
					BonusSourceID()));
			}
			const int32_t actualEffectiveSide = added->isHypnotized()
				? 1 - static_cast<int32_t>(added->unitSide())
				: static_cast<int32_t>(added->unitSide());
			if(added->getCount() != count
			   || added->getFirstHPleft() != topHealth
			   || added->getPosition() != importedPosition
			   || added->shots.available() != shots
			   || added->counterAttacks.available() != retaliations
			   || added->waited() != input["waited"].Bool()
			   || added->moved() != input["done"].Bool()
			   || added->isClone() != input["clone"].Bool()
			   || added->summoned != input["summoned"].Bool()
			   || added->isSummoned() != (input["summoned"].Bool() || input["clone"].Bool())
			   || actualEffectiveSide != input["effective_side"].Integer()
			   || added->hasBonusOfType(BonusType::NOT_ACTIVE) != input["disabled"].Bool())
			{
				throw std::runtime_error(
					"probe importer could not reproduce the canonical stack state exactly: key=" + canonicalKey
					+ " count=" + std::to_string(added->getCount()) + "/" + std::to_string(count)
					+ " hp=" + std::to_string(added->getFirstHPleft()) + "/" + std::to_string(topHealth)
					+ " pos=" + std::to_string(added->getPosition().toInt()) + "/" + std::to_string(input["position"].Integer())
					+ " shots=" + std::to_string(added->shots.available()) + "/" + std::to_string(shots)
					+ " retal=" + std::to_string(added->counterAttacks.available()) + "/" + std::to_string(retaliations)
					+ " waited=" + std::to_string(added->waited()) + "/" + std::to_string(input["waited"].Bool())
					+ " moved=" + std::to_string(added->moved()) + "/" + std::to_string(input["done"].Bool())
					+ " clone=" + std::to_string(added->isClone()) + "/" + std::to_string(input["clone"].Bool())
					+ " summoned=" + std::to_string(added->summoned) + "/" + std::to_string(input["summoned"].Bool())
					+ " side=" + std::to_string(actualEffectiveSide) + "/" + std::to_string(input["effective_side"].Integer())
					+ " disabled=" + std::to_string(added->hasBonusOfType(BonusType::NOT_ACTIVE)) + "/" + std::to_string(input["disabled"].Bool()));
			}
			if(hasNativeState)
			{
				const JsonNode & native = input["native"];
				const int32_t selectedCreatureSpell = native["faerie_dragon_spell"].Integer();
				if(creatureID == 134 && selectedCreatureSpell >= 0)
				{
					const SpellID selectedSpell(selectedCreatureSpell);
					if(!selectedSpell.hasValue() || !selectedSpell.toSpell())
						throw std::runtime_error("canonical selected creature spell is invalid");
					decisionContext.selectedCreatureSpells.emplace(added->unitId(), selectedSpell);
				}
				if(added->isOnNativeTerrain() != native["on_native_terrain"].Bool())
					throw std::runtime_error("canonical native-terrain state differs");
				auto requireValue = [&](const char * field, int64_t expected, int64_t actual)
				{
					if(actual != expected)
						throw std::runtime_error(
							"canonical native stack field differs: " + std::string(field)
							+ " expected=" + std::to_string(expected) + " actual=" + std::to_string(actual)
							+ " nativeTerrain=" + std::to_string(added->isOnNativeTerrain())
							+ " terrain=" + std::to_string(battle()->terrainType.getNum()));
				};
				auto requireEqual = [&](const char * field, int64_t actual)
				{
					requireValue(field, native[field].Integer(), actual);
				};
				requireEqual("fight_value", added->unitType()->getFightValue());
				requireEqual("max_hp", added->getMaxHealth());
				// In SoD army::speed is the raw speed field. Slow is held in a
				// separate floating-point multiplier and is applied by
				// army::get_speed (0x4489F0). VCMI exposes that latter value as
				// getMovementRange(), so compare equivalent effective speeds.
				int64_t expectedSpeed = native["speed"].Integer();
				for(const JsonNode & effect : input["effects"].Vector())
				{
					if(effect["spell_id"].Integer() == SpellID::SLOW)
					{
						const int64_t slowPercent = effect["mastery"].Integer() >= 2 ? 50 : 75;
						expectedSpeed = std::max<int64_t>(1, expectedSpeed * slowPercent / 100);
						break;
					}
				}
				requireValue("effective_speed", expectedSpeed, added->getMovementRange());
				requireEqual("attack", ClassicRulesAdapter::attack(added, false));
				requireEqual("defense", ClassicRulesAdapter::defense(added));
				requireEqual("min_damage", ClassicRulesAdapter::minDamage(added, false));
				requireEqual("max_damage", ClassicRulesAdapter::maxDamage(added, false));
			}
			stackKeys[added->unitId()] = canonicalKey;
		}
		// AI_target is persistent native scratch state and may point to a stack
		// later in physical serialization order (or to a dead physical slot that
		// is intentionally absent). Resolve it only after all live unit IDs exist.
		for(const JsonNode & input : request["state"]["stacks"].Vector())
		{
			if(input["native"].isNull())
				continue;
			const std::string canonicalKey = key(input["key"]);
			const auto actor = std::ranges::find_if(
				stackKeys,
				[&](const auto & entry) { return entry.second == canonicalKey; });
			if(actor == stackKeys.end())
				throw std::runtime_error("canonical AI target record owner was not imported");
			const JsonNode & ai = input["native"]["ai"];
			ClassicAITargetRecord record;
			record.targetValue = ai["target_value"].Integer();
			record.targetDistance = ai["target_distance"].Integer();
			record.possibleTargets = static_cast<uint32_t>(ai["possible_targets"].Integer());
			if(!ai["target"].isNull())
			{
				record.hasTarget = true;
				const std::string targetKey = key(ai["target"]);
				const auto target = std::ranges::find_if(
					stackKeys,
					[&](const auto & entry) { return entry.second == targetKey; });
				if(target != stackKeys.end())
					record.targetUnitId = target->first;
			}
			decisionContext.targetRecords.emplace(actor->first, record);
		}
		for(int32_t sideIndex = 0; sideIndex < 2; ++sideIndex)
		{
			const int32_t summonedElemental = stateInput["sides"][sideIndex]["summoned_elemental"].Integer();
			if(summonedElemental == -1)
				continue;

			// Heroes III remembers the one permitted elemental family even after
			// its last summoned stack dies.  VCMI derives the same restriction by
			// scanning summoned units, including ghosts, so an unkeyed ghost is an
			// exact state representation and is invisible to tactical stack scans.
			CStack * marker = addCanonicalStack(
				static_cast<BattleSide>(sideIndex),
				CreatureID(summonedElemental),
				BattleHex::INVALID,
				1,
				SlotID::SUMMONED_SLOT_PLACEHOLDER);
			marker->summoned = true;
			marker->onRemoved();
			if(!marker->isGhost() || !marker->isSummoned())
				throw std::runtime_error("probe importer could not reproduce the summoned-elemental family marker");
		}
		if(!stateInput["current_side"].isNull())
		{
			const std::string activeKey = key(request["active_stack"]);
			const auto active = std::ranges::find_if(
				stackKeys,
				[&](const auto & entry) { return entry.second == activeKey; });
			if(active == stackKeys.end())
				throw std::runtime_error("canonical active stack was not imported");
			const CStack * activeStack = battle()->getStack(active->first, false);
			const int32_t actualSide = static_cast<int32_t>(ClassicBattleStateView(probeBattleCallback()).controllingSide(activeStack));
			if(actualSide != stateInput["current_side"].Integer())
				throw std::runtime_error("canonical current_side differs from active stack control");
		}
	}

	std::string normalize(
		const BattleAction & action,
		const ReplayClassicBattleAIRng & rng,
		const ClassicDecisionTrace & trace) const
	{
		auto keyFor = [&](uint32_t id) -> std::string
		{
			const auto found = stackKeys.find(id);
			return found == stackKeys.end() ? std::string() : found->second;
		};
		int32_t targetHex = -1;
		int32_t attackFrom = -1;
		std::string targetKey;
		std::string secondaryTargetKey;
		if(!action.target.empty())
		{
			size_t targetIndex = 0;
			if(action.actionType == EActionType::WALK_AND_ATTACK && action.target.size() > 1)
			{
				attackFrom = action.target.front().hexValue.toInt();
				targetIndex = 1;
			}
			targetHex = action.target[targetIndex].hexValue.toInt();
			if(action.target[targetIndex].unitValue >= 0)
				targetKey = keyFor(action.target[targetIndex].unitValue);
			if(action.target.size() > targetIndex + 1 && action.target[targetIndex + 1].unitValue >= 0)
				secondaryTargetKey = keyFor(action.target[targetIndex + 1].unitValue);
		}

		std::ostringstream result;
		result << "{\"record_type\":\"decision\",\"import_verified\":true,\"action\":{\"type\":\""
			   << actionName(action.actionType) << "\",\"actor\":";
		const std::string actor = keyFor(action.stackNumber);
		result << (actor.empty() ? "null" : "\"" + actor + "\"") << ",\"spell_id\":";
		if(action.spell.hasValue())
			result << action.spell.getNum();
		else
			result << "null";
		result << ",\"target\":" << (targetKey.empty() ? "null" : "\"" + targetKey + "\"")
			   << ",\"target_hex\":" << targetHex
			   << ",\"attack_from_hex\":" << attackFrom
			   << ",\"secondary_target\":"
			   << (secondaryTargetKey.empty() ? "null" : "\"" + secondaryTargetKey + "\"")
			   << ",\"wall_part\":";
		const char * wallPart = wallPartName(action);
		result << (wallPart ? "\"" + std::string(wallPart) + "\"" : "null") << "},\"rng_tape\":[";
		for(size_t index = 0; index < rng.getRequests().size(); ++index)
		{
			if(index)
				result << ',';
			const auto & request = rng.getRequests()[index];
			result << "{\"lower\":" << request.lower << ",\"upper\":" << request.upper << ",\"value\":" << request.value << "}";
		}
		result << "],\"trace\":[";
		for(size_t index = 0; index < trace.getEntries().size(); ++index)
		{
			if(index)
				result << ',';
			const auto & entry = trace.getEntries()[index];
			result << "{\"stage\":\"" << entry.stage << "\",\"key\":\"" << entry.key << "\",\"value\":" << entry.value << "}";
		}
		result << "]}";
		return result.str();
	}

public:
	void replay(const std::string & line)
	{
		JsonParsingSettings settings;
		settings.mode = JsonParsingSettings::JsonFormatMode::JSON;
		settings.strict = true;
		const JsonNode request(line.data(), line.size(), settings, "classic-ai-probe");
		resetBattle(request);

		std::vector<int32_t> tape;
		for(const JsonNode & item : request["rng_tape"].Vector())
			tape.push_back(item["value"].Integer());
		auto rng = std::make_shared<ReplayClassicBattleAIRng>(std::move(tape));
		auto trace = std::make_shared<ClassicDecisionTrace>();
		const std::string activeKey = key(request["active_stack"]);
		const CStack * active = nullptr;
		for(const auto & [id, canonicalKey] : stackKeys)
			if(canonicalKey == activeKey)
				active = battle()->battleGetStackByID(id);
		if(!active)
			throw std::runtime_error("active_stack does not exist");

		AutocombatPreferences preferences;
		preferences.enableSpellsUsage = hasCanonicalHero;
		preferences.enableTacticsUsage = false;
		ClassicDecisionEntryPoint entryPoint = ClassicDecisionEntryPoint::FULL_PIPELINE;
		const std::string hook = request["metadata"]["hook"].String();
		if(hook == "DoCompAI")
			entryPoint = ClassicDecisionEntryPoint::DO_COMP_AI;
		else if(hook == "BerserkAttack")
			entryPoint = ClassicDecisionEntryPoint::DO_COMP_AI;
		else if(hook == "DoSpellAI")
			entryPoint = ClassicDecisionEntryPoint::DO_SPELL_AI;
		else if(hook == "AICheckRetreat")
			entryPoint = ClassicDecisionEntryPoint::CHECK_RETREAT;
		auto callback = std::shared_ptr<CBattleInfoCallback>(
			battle(),
			[](CBattleInfoCallback *)
			{
			}
		);
		const BattleAction action = ClassicBattleDecision::decide(
			gameHandler.get(),
			callback,
			ClassicBattleStateView(callback).controllingSide(active),
			active,
			request["state"]["difficulty"].Integer(),
			preferences,
			rng,
			trace,
			entryPoint,
			&decisionContext
		);
		std::cout << normalize(action, *rng, *trace) << std::endl;
	}
};
}

TEST(ClassicBattleAIRngTest, ReplaysInclusiveValuesInOrder)
{
	ReplayClassicBattleAIRng rng({75, 100, 1});
	EXPECT_EQ(rng.nextIntInclusive(75, 100), 75);
	EXPECT_EQ(rng.nextIntInclusive(75, 100), 100);
	EXPECT_EQ(rng.nextIntInclusive(1, 100), 1);
	EXPECT_EQ(rng.consumed(), 3);
	EXPECT_TRUE(rng.exhausted());
}

TEST(ClassicBattleAIRngTest, RejectsExhaustedTape)
{
	ReplayClassicBattleAIRng rng({});
	EXPECT_THROW(rng.nextIntInclusive(1, 100), std::runtime_error);
}

TEST(ClassicBattleAIRngTest, RejectsValueOutsideRequestedRange)
{
	ReplayClassicBattleAIRng rng({74});
	EXPECT_THROW(rng.nextIntInclusive(75, 100), std::runtime_error);
}

TEST(ClassicBattleAIRngTest, ProductionGeneratorHonorsInclusiveBounds)
{
	ClassicBattleAIRng rng;
	for(int iteration = 0; iteration < 32; ++iteration)
	{
		const int32_t value = rng.nextIntInclusive(-3, 7);
		EXPECT_GE(value, -3);
		EXPECT_LE(value, 7);
	}
}

TEST(ClassicBattleAIRngTest, VstdAdapterUsesTheInjectedInclusiveStream)
{
	UpperBoundClassicBattleAIRng source;
	ClassicVstdRngAdapter rng(source);
	EXPECT_EQ(rng.nextInt(-2, 7), 7);
	EXPECT_EQ(rng.nextInt64(-2, 9), 9);
	EXPECT_DOUBLE_EQ(rng.nextDouble(-2.0, 6.0), 6.0);
	EXPECT_EQ(rng.nextInt(11), 11);
	EXPECT_EQ(rng.nextInt64(12), 12);
	EXPECT_DOUBLE_EQ(rng.nextDouble(13.0), 13.0);
	EXPECT_EQ(rng.nextInt(), std::numeric_limits<int32_t>::max());
	EXPECT_EQ(rng.nextBinomialInt(3, 1.0), 3);
	EXPECT_THROW(
		rng.nextInt64(static_cast<int64_t>(std::numeric_limits<int32_t>::min()) - 1, 0),
		std::out_of_range
	);
}

TEST(ClassicBattleAIRngTest, DestroysGeneratorThroughInterface)
{
	std::unique_ptr<IClassicBattleAIRng> rng =
		std::make_unique<ReplayClassicBattleAIRng>(std::vector<int32_t>{7});
	EXPECT_EQ(rng->nextIntInclusive(0, 7), 7);
	rng.reset();
	EXPECT_EQ(rng, nullptr);
}

TEST(ClassicCombatValueTest, UsesExecutableStyleTruncationTowardZero)
{
	EXPECT_EQ(ClassicCombatValue::truncateTowardZero(2.49), 2);
	EXPECT_EQ(ClassicCombatValue::truncateTowardZero(2.51), 2);
	EXPECT_EQ(ClassicCombatValue::truncateTowardZero(-2.49), -2);
}

TEST(ClassicAttackEvaluatorTest, ReproducesShooterDisabledTargetAndEqualityOrdering)
{
	EXPECT_FALSE(ClassicAttackEvaluator::shouldReplaceShooterTarget(100, true, 1, true));
	EXPECT_TRUE(ClassicAttackEvaluator::shouldReplaceShooterTarget(1, false, 100, true));
	EXPECT_TRUE(ClassicAttackEvaluator::shouldReplaceShooterTarget(100, true, 100, false));
	EXPECT_FALSE(ClassicAttackEvaluator::shouldReplaceShooterTarget(99, true, 100, false));
	EXPECT_TRUE(ClassicAttackEvaluator::shouldReplaceShooterTarget(100, false, 100, false));
}

TEST(ClassicSpellEvaluatorTest, AppliesManaConservationBoundaries)
{
	EXPECT_EQ(ClassicSpellEvaluator::applyManaConservation(100, 0), 0);
	EXPECT_EQ(ClassicSpellEvaluator::applyManaConservation(100, 1), 100);
	EXPECT_EQ(ClassicSpellEvaluator::applyManaConservation(100, 4), 200);
	EXPECT_EQ(ClassicSpellEvaluator::applyManaConservation(100, 6), 245);
	EXPECT_EQ(ClassicSpellEvaluator::applyManaConservation(100, 7), 250);
	EXPECT_EQ(ClassicSpellEvaluator::applyManaConservation(100, 20), 250);
}

TEST(ClassicSpellEvaluatorTest, AppliesSlowToCurrentEffectiveSpeedWithIntegerTruncation)
{
	EXPECT_EQ(ClassicSpellEvaluator::classicSlowSpeed(5, 0), 3);
	EXPECT_EQ(ClassicSpellEvaluator::classicSlowSpeed(5, 1), 3);
	EXPECT_EQ(ClassicSpellEvaluator::classicSlowSpeed(5, 2), 2);
	EXPECT_EQ(ClassicSpellEvaluator::classicSlowSpeed(11, 3), 5);
	EXPECT_EQ(ClassicSpellEvaluator::classicSlowSpeed(1, 3), 1);
	// Haste is deliberately already present in the input speed. The SoD AI
	// scores expert Slow as 50% of ten, not 50% of the un-Hasted base five.
	EXPECT_EQ(ClassicSpellEvaluator::classicSlowSpeed(10, 3), 5);
}

TEST(ClassicRetreatEvaluatorTest, AppliesBiasWithConversionAfterEachStep)
{
	EXPECT_EQ(ClassicRetreatEvaluator::applyTenPercentBias(10), 11);
	EXPECT_EQ(ClassicRetreatEvaluator::applyTenPercentBias(15), 17);
	EXPECT_EQ(ClassicRetreatEvaluator::applyTenPercentBias(ClassicRetreatEvaluator::applyTenPercentBias(15)), 19);
}

TEST(ClassicDecisionTraceTest, PreservesInsertionOrder)
{
	ClassicDecisionTrace trace;
	trace.record("one", "a", 1);
	trace.record("two", "b", 2);
	ASSERT_EQ(trace.getEntries().size(), 2);
	EXPECT_EQ(trace.getEntries()[0].stage, "one");
	EXPECT_EQ(trace.getEntries()[1].stage, "two");
	trace.clear();
	EXPECT_TRUE(trace.getEntries().empty());
}

TEST(ClassicBattleAIModeTest, DefaultsToModernAndCanBeEnabledExplicitly)
{
	CBattleAI modern;
	EXPECT_FALSE(modern.isClassicMode());

	BattleAISettings settings;
	settings.mode = BattleAIMode::CLASSIC;
	CBattleAI classic(settings);
	EXPECT_TRUE(classic.isClassicMode());
}

TEST_F(ClassicBattleAIIntegrationTest, BuildsStableSidesAndCombatParameters)
{
	auto callback = battleCallback();
	ClassicBattleStateView view(callback);
	const auto stacks = view.orderedStacks();
	ASSERT_GE(stacks.size(), 2);
	EXPECT_EQ(stacks.front()->unitSide(), BattleSide::ATTACKER);
	EXPECT_EQ(stacks.back()->unitSide(), BattleSide::DEFENDER);
	EXPECT_TRUE(view.isEnemy(stacks.front(), stacks.back()));
	EXPECT_FALSE(view.isEnemy(stacks.front(), stacks.front()));

	ClassicCombatValue values(callback);
	const auto parameters = values.buildParameters(BattleSide::ATTACKER, 2);
	int32_t expectedLowestAttack = std::numeric_limits<int32_t>::max();
	int32_t expectedLowestDefense = std::numeric_limits<int32_t>::max();
	for(const CStack * stack : callback->battleGetAllStacks(false))
	{
		if(!stack->alive() || stack->isTurret())
			continue;
		expectedLowestAttack = std::min(
			expectedLowestAttack,
			stack->getAttack(stack->isShooter()) - stack->unitType()->getBaseAttack());
		expectedLowestDefense = std::min(
			expectedLowestDefense,
			stack->getDefense(false) - stack->unitType()->getBaseDefense());
	}
	EXPECT_EQ(parameters.lowestAttack, expectedLowestAttack);
	EXPECT_EQ(parameters.lowestDefense, expectedLowestDefense);
	EXPECT_GT(parameters.friendlyCombatValue, 0);
	EXPECT_GT(parameters.enemyCombatValue, 0);
	EXPECT_GE(parameters.roundsLeft, 1);
	EXPECT_LE(parameters.roundsLeft, 7);
}

TEST_F(ClassicBattleAIIntegrationTest, ReproducesExecutableMoveOrderBandsAndSideAlternation)
{
	CStack * attacker = addStack(BattleSide::ATTACKER, CreatureID(0), BattleHex(74), 1);
	CStack * defender = addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(112), 1);
	auto callback = battleCallback();
	ClassicBattleStateView view(callback);

	auto entryFor = [](const std::vector<ClassicMoveOrderEntry> & order, const CStack * stack)
		-> const ClassicMoveOrderEntry &
	{
		const auto found = std::ranges::find_if(order, [stack](const auto & entry)
		{
			return entry.stack == stack;
		});
		if(found == order.end())
			throw std::runtime_error("test stack missing from classic move order");
		return *found;
	};
	auto firstSideForKey = [](const std::vector<ClassicMoveOrderEntry> & order, int32_t key)
	{
		const auto found = std::ranges::find_if(order, [key](const auto & entry)
		{
			return entry.key == key;
		});
		return found == order.end() ? BattleSide::NONE : found->stack->unitSide();
	};

	const int32_t speed = attacker->getInitiative(0);
	ASSERT_EQ(speed, defender->getInitiative(0));
	auto order = view.moveOrder(BattleSide::ATTACKER, false);
	EXPECT_EQ(entryFor(order, attacker).key, speed);
	EXPECT_EQ(firstSideForKey(order, speed), BattleSide::ATTACKER);
	order = view.moveOrder(BattleSide::DEFENDER, false);
	EXPECT_EQ(firstSideForKey(order, speed), BattleSide::DEFENDER);

	order = view.moveOrder(BattleSide::ATTACKER, true);
	EXPECT_EQ(entryFor(order, attacker).key, -speed);

	JsonNode state = attacker->save();
	state["state"]["waiting"].Bool() = false;
	state["state"]["waitedThisTurn"].Bool() = false;
	state["state"]["moved"].Bool() = true;
	attacker->load(state);
	order = view.moveOrder(BattleSide::ATTACKER, false);
	EXPECT_EQ(entryFor(order, attacker).key, speed - 1000);

	auto blind = std::make_shared<Bonus>(
		BonusDuration::N_TURNS,
		BonusType::NOT_ACTIVE,
		BonusSource::SPELL_EFFECT,
		0,
		BonusSourceID(SpellID(SpellID::BLIND)));
	blind->turnsRemain = 2;
	attacker->addNewBonus(blind);
	order = view.moveOrder(BattleSide::ATTACKER, false);
	EXPECT_EQ(entryFor(order, attacker).key, -10000);

	CStack * tent = addStack(BattleSide::ATTACKER, CreatureID(CreatureID::FIRST_AID_TENT), BattleHex(72), 1);
	order = view.moveOrder(BattleSide::ATTACKER, false);
	EXPECT_EQ(entryFor(order, tent).key, -100000);
	EXPECT_GT(order.front().order, order.back().order);
}

TEST_F(ClassicBattleAIIntegrationTest, LossValueConvertsTheDirectHitPointProductOnce)
{
	auto callback = battleCallback();
	const CStack * candidate = nullptr;
	for(const CStack * stack : callback->battleGetAllStacks(false))
	{
		if(stack->alive() && !stack->isTurret() && !stack->isShooter()
		   && stack->canMove() && stack->getTotalAttacks(false) == 1)
		{
			candidate = stack;
			break;
		}
	}
	ASSERT_NE(candidate, nullptr);

	ClassicCombatParameters parameters;
	parameters.lowestAttack = candidate->getAttack(false) - candidate->unitType()->getBaseAttack();
	parameters.lowestDefense = candidate->getDefense(false) - candidate->unitType()->getBaseDefense();
	const int64_t hitPoints = candidate->getMaxHealth();
	const int64_t lostHealth = std::max<int64_t>(1, hitPoints / 3);
	const int64_t before = candidate->getAvailableHealth();
	const int64_t expected = ClassicCombatValue::truncateTowardZero(
		static_cast<double>(candidate->unitType()->getFightValue()) * lostHealth / hitPoints);
	ClassicCombatValue values(callback);
	EXPECT_EQ(values.lossValue(candidate, before, before - lostHealth, parameters), expected);
}

TEST_F(ClassicBattleAIIntegrationTest, BlockedShooterPenaltyAndRangedExtraStrikeStayInsideSquareRoot)
{
	CStack * shooter = addStack(BattleSide::ATTACKER, CreatureID(3), BattleHex(77), 1);
	auto callback = battleCallback();
	ClassicCombatParameters parameters;
	parameters.lowestAttack = shooter->getAttack(true) - shooter->unitType()->getBaseAttack();
	parameters.lowestDefense = shooter->getDefense(false) - shooter->unitType()->getBaseDefense();
	ClassicCombatValue values(callback);
	const int64_t fightValue = shooter->unitType()->getFightValue();
	ASSERT_TRUE(callback->battleCanShoot(shooter));
	EXPECT_EQ(values.unitValue(shooter, parameters), ClassicCombatValue::truncateTowardZero(fightValue * std::sqrt(2.0)));

	addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(78), 1);
	ASSERT_FALSE(callback->battleCanShoot(shooter));
	parameters.lowestAttack = shooter->getAttack(false) - shooter->unitType()->getBaseAttack();
	EXPECT_EQ(values.unitValue(shooter, parameters), ClassicCombatValue::truncateTowardZero(fightValue * std::sqrt(0.5)));
}

TEST_F(ClassicBattleAIIntegrationTest, CloneAndSummonUseOriginalSpecialStackScaling)
{
	CStack * clone = addStack(BattleSide::ATTACKER, CreatureID(0), BattleHex(74), 10);
	JsonNode cloneState = clone->save();
	cloneState["state"]["cloned"].Bool() = true;
	clone->load(cloneState);

	ClassicCombatParameters cloneParameters;
	cloneParameters.lowestAttack = clone->getAttack(false) - clone->unitType()->getBaseAttack();
	cloneParameters.lowestDefense = clone->getDefense(false) - clone->unitType()->getBaseDefense();
	auto callback = battleCallback();
	ClassicCombatValue values(callback);
	EXPECT_EQ(
		values.stackValue(clone, cloneParameters),
		ClassicCombatValue::truncateTowardZero(clone->unitType()->getFightValue() * clone->getCount() / 5.0));

	CStack * summoned = addStack(BattleSide::ATTACKER, CreatureID(0), BattleHex(75), 1);
	JsonNode summonState = summoned->save();
	summonState["state"]["summoned"].Bool() = true;
	summoned->load(summonState);
	ClassicCombatParameters summonParameters;
	summonParameters.lowestAttack = summoned->getAttack(false) - summoned->unitType()->getBaseAttack();
	summonParameters.lowestDefense = summoned->getDefense(false) - summoned->unitType()->getBaseDefense();
	int64_t ordinaryHealth = 0;
	for(const CStack * stack : callback->battleGetAllStacks(false))
	{
		if(stack->unitSide() == summoned->unitSide()
		   && !stack->hasBonusOfType(BonusType::SIEGE_WEAPON)
		   && !stack->summoned
		   && !stack->isClone())
			ordinaryHealth += stack->getAvailableHealth();
	}
	const double expected = summoned->unitType()->getFightValue()
		* static_cast<double>(ordinaryHealth) / (summoned->getAvailableHealth() + ordinaryHealth);
	EXPECT_EQ(values.stackValue(summoned, summonParameters), ClassicCombatValue::truncateTowardZero(expected));
}

TEST_F(ClassicBattleAIIntegrationTest, ProducesDeterministicLegalStackAction)
{
	auto callback = battleCallback();
	ClassicBattleStateView view(callback);
	const CStack * attacker = nullptr;
	for(const CStack * stack : view.orderedStacks())
	{
		if(stack->unitSide() == BattleSide::ATTACKER)
		{
			attacker = stack;
			break;
		}
	}
	ASSERT_NE(attacker, nullptr);

	ClassicCombatValue values(callback);
	const auto parameters = values.buildParameters(BattleSide::ATTACKER, 2);
	auto rng = std::make_shared<ReplayClassicBattleAIRng>(std::vector<int32_t>(32, 100));
	auto trace = std::make_shared<ClassicDecisionTrace>();
	ClassicAttackEvaluator evaluator(callback, rng, trace);
	const ClassicScoredAction result = evaluator.chooseAction(attacker, parameters);

	EXPECT_TRUE(result.valid);
	EXPECT_EQ(result.action.stackNumber, attacker->unitId());
	EXPECT_NE(result.action.actionType, EActionType::NO_ACTION);
	EXPECT_FALSE(trace->getEntries().empty());
}

TEST_F(ClassicBattleAIIntegrationTest, MultiTurnMeleeTargetAdvancesBeforeConsideringWait)
{
	removeAllStacks();
	battle()->obstacles.clear();
	battle()->terrainType = TerrainId(2); // Grass: native Castle units gain one speed.
	battle()->nodeHasChanged();
	CStack * pikemen = addStack(BattleSide::ATTACKER, CreatureID(0), BattleHex(86), 95);
	pikemen->addNewBonus(std::make_shared<Bonus>(
		BonusDuration::ONE_BATTLE,
		BonusType::STACKS_SPEED,
		BonusSource::TERRAIN_NATIVE,
		1,
		BonusSourceID()));
	addStack(BattleSide::DEFENDER, CreatureID(41), BattleHex(100), 1);
	auto callback = battleCallback();
	ASSERT_EQ(pikemen->getMovementRange(), 5);
	ClassicCombatValue values(callback);
	const ClassicCombatParameters parameters = values.buildParameters(BattleSide::ATTACKER, 2);
	auto rng = std::make_shared<ReplayClassicBattleAIRng>(std::vector<int32_t>{92});
	ClassicAttackEvaluator evaluator(callback, rng, nullptr);

	const ClassicScoredAction result = evaluator.chooseAction(pikemen, parameters);
	ASSERT_TRUE(result.valid);
	EXPECT_EQ(result.action.actionType, EActionType::WALK);
	ASSERT_FALSE(result.action.target.empty());
	EXPECT_EQ(result.action.target.front().hexValue, BattleHex(91));
	EXPECT_TRUE(rng->exhausted());
}

TEST_F(ClassicBattleAIIntegrationTest, DangerProjectionMarksVisitedCellsThroughSpeedPlusOne)
{
	removeAllStacks();
	battle()->obstacles.clear();
	battle()->terrainType = TerrainId(2);

	auto addStaticObstacle = [&](int32_t obstacleID, int32_t hex)
	{
		const ObstacleInfo * info = LIBRARY->obstacleHandler->getByName(std::to_string(obstacleID));
		ASSERT_NE(info, nullptr);
		auto obstacle = std::make_shared<CObstacleInstance>();
		obstacle->ID = info->obstacle.getNum();
		obstacle->pos = BattleHex(hex);
		obstacle->obstacleType = CObstacleInstance::USUAL;
		obstacle->uniqueID = static_cast<int32_t>(battle()->obstacles.size());
		battle()->obstacles.push_back(std::move(obstacle));
	};
	addStaticObstacle(23, 41);
	addStaticObstacle(21, 131);
	addStaticObstacle(19, 26);
	addStaticObstacle(20, 62);

	CStack * masterGenie = addStack(BattleSide::ATTACKER, CreatureID(37), BattleHex(35), 20);
	CStack * archMage = addStack(BattleSide::ATTACKER, CreatureID(35), BattleHex(137), 20);
	const std::array<CStack *, 6> enemies = {
		addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(10), 167),
		addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(44), 167),
		addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(113), 167),
		addStack(BattleSide::DEFENDER, CreatureID(1), BattleHex(129), 167),
		addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(146), 166),
		addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(180), 154),
	};
	auto markDone = [](CStack * stack)
	{
		JsonNode state = stack->save();
		state["state"]["moved"].Bool() = true;
		stack->load(state);
	};
	markDone(archMage);
	for(CStack * enemy : enemies)
		markDone(enemy);
	battle()->nodeHasChanged();

	auto callback = battleCallback();
	ClassicCombatValue values(callback);
	const ClassicCombatParameters parameters = values.buildParameters(BattleSide::ATTACKER, 2);
	auto rng = std::make_shared<ReplayClassicBattleAIRng>(std::vector<int32_t>{
		82, 95, 92, 98, 89, 83, 97, 89, 75, 79, 91, 77});
	auto trace = std::make_shared<ClassicDecisionTrace>();
	ClassicAttackEvaluator evaluator(callback, rng, trace, true, true);

	const ClassicScoredAction result = evaluator.chooseAction(masterGenie, parameters);
	ASSERT_TRUE(result.valid);
	EXPECT_EQ(result.action.actionType, EActionType::WALK_AND_ATTACK);
	auto firstDanger = [&](int32_t hex) -> std::optional<int64_t>
	{
		const auto found = std::ranges::find_if(
			trace->getEntries(),
			[hex](const ClassicDecisionTraceEntry & entry)
			{
				return entry.stage == "attack_hex.danger"
					&& entry.key == std::to_string(hex);
			});
		return found == trace->getEntries().end()
			? std::nullopt
			: std::optional<int64_t>(found->value);
	};
	const auto danger9 = firstDanger(9);
	const auto danger11 = firstDanger(11);
	const auto danger28 = firstDanger(28);
	const auto danger45 = firstDanger(45);
	ASSERT_TRUE(danger9.has_value());
	ASSERT_TRUE(danger11.has_value());
	ASSERT_TRUE(danger28.has_value());
	ASSERT_TRUE(danger45.has_value());
	EXPECT_LT(*danger11, *danger9);
	EXPECT_EQ(*danger28, *danger11);
	EXPECT_LT(*danger45, *danger11);
}

TEST_F(ClassicBattleAIIntegrationTest, DangerProjectionFiltersObstaclesAndValuesHostileFireWall)
{
	removeAllStacks();
	battle()->obstacles.clear();
	CStack * actor = addStack(BattleSide::ATTACKER, CreatureID(0), BattleHex(35), 100);
	addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(151), 100);

	auto addSpellObstacle = [&](SpellID trigger, BattleSide caster, int32_t damage, BattleHex hex)
	{
		auto obstacle = std::make_shared<SpellCreatedObstacle>();
		obstacle->ID = trigger.getNum();
		obstacle->pos = hex;
		obstacle->uniqueID = static_cast<int32_t>(battle()->obstacles.size());
		obstacle->turnsRemaining = 2;
		obstacle->casterSide = caster;
		obstacle->minimalDamage = damage;
		obstacle->passable = true;
		obstacle->trigger = trigger;
		obstacle->customSize.insert(hex);
		battle()->obstacles.push_back(std::move(obstacle));
	};
	// Exercise every original Fire Wall filter before the one contributing
	// hostile, positive damage record.
	const ObstacleInfo * ordinaryInfo = LIBRARY->obstacleHandler->getByName("23");
	ASSERT_NE(ordinaryInfo, nullptr);
	auto ordinary = std::make_shared<CObstacleInstance>();
	ordinary->ID = ordinaryInfo->obstacle.getNum();
	ordinary->obstacleType = CObstacleInstance::USUAL;
	ordinary->pos = BattleHex(54);
	ordinary->uniqueID = static_cast<int32_t>(battle()->obstacles.size());
	battle()->obstacles.push_back(std::move(ordinary));
	addSpellObstacle(SpellID(SpellID::LAND_MINE), BattleSide::DEFENDER, 100, BattleHex(55));
	addSpellObstacle(SpellID(SpellID::FIRE_WALL), BattleSide::ATTACKER, 100, BattleHex(56));
	addSpellObstacle(SpellID(SpellID::FIRE_WALL), BattleSide::DEFENDER, 0, BattleHex(57));
	addSpellObstacle(SpellID(SpellID::FIRE_WALL), BattleSide::DEFENDER, 1000, BattleHex(150));
	battle()->nodeHasChanged();

	auto callback = battleCallback();
	ClassicCombatValue values(callback);
	const ClassicCombatParameters parameters = values.buildParameters(BattleSide::ATTACKER, 2);
	auto rng = std::make_shared<UpperBoundClassicBattleAIRng>();
	auto trace = std::make_shared<ClassicDecisionTrace>();
	ClassicAttackEvaluator evaluator(callback, rng, trace);
	const ClassicScoredAction result = evaluator.chooseAction(actor, parameters);

	ASSERT_TRUE(result.valid);
	const auto danger = std::ranges::find_if(
		trace->getEntries(),
		[](const ClassicDecisionTraceEntry & entry)
		{
			return entry.stage == "attack_hex.danger" && entry.key == "150";
		});
	ASSERT_NE(danger, trace->getEntries().end());
	EXPECT_LT(danger->value, 0);
}

TEST_F(ClassicBattleAIIntegrationTest, RunSelectorEscapesNegativeDangerWithOriginalTieOrder)
{
	removeAllStacks();
	battle()->obstacles.clear();
	CStack * actor = addStack(BattleSide::ATTACKER, CreatureID(0), BattleHex(56), 100);
	addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(151), 100);

	auto fireWall = std::make_shared<SpellCreatedObstacle>();
	fireWall->ID = SpellID(SpellID::FIRE_WALL).getNum();
	fireWall->pos = actor->getPosition();
	fireWall->uniqueID = 0;
	fireWall->turnsRemaining = 2;
	fireWall->casterSide = BattleSide::DEFENDER;
	fireWall->minimalDamage = 1000;
	fireWall->passable = true;
	fireWall->trigger = SpellID(SpellID::FIRE_WALL);
	fireWall->customSize.insert(actor->getPosition());
	battle()->obstacles.push_back(std::move(fireWall));
	battle()->nodeHasChanged();

	auto callback = battleCallback();
	ClassicCombatValue values(callback);
	const ClassicCombatParameters parameters = values.buildParameters(BattleSide::ATTACKER, 2);
	auto trace = std::make_shared<ClassicDecisionTrace>();
	ClassicAttackEvaluator evaluator(callback, std::make_shared<UpperBoundClassicBattleAIRng>(), trace, true);
	const ClassicScoredAction result = evaluator.chooseRunAction(actor, parameters);

	ASSERT_TRUE(result.valid);
	EXPECT_EQ(result.action.actionType, EActionType::WALK);
	ASSERT_FALSE(result.action.target.empty());
	// All six adjacent safe cells have danger zero and distance one. The
	// executable's ascending scan replaces on a complete tie, retaining 73.
	EXPECT_EQ(result.action.target.front().hexValue, BattleHex(73));
	EXPECT_EQ(result.score, 0);
	EXPECT_EQ(result.attackTime, 1);
	const auto current = std::ranges::find_if(
		trace->getEntries(),
		[](const auto & entry)
		{
			return entry.stage == "run.current";
		}
	);
	ASSERT_NE(current, trace->getEntries().end());
	EXPECT_LT(current->value, 0);
	const auto selected = std::ranges::find_if(
		trace->getEntries(),
		[](const auto & entry)
		{
			return entry.stage == "run.final_hex";
		}
	);
	ASSERT_NE(selected, trace->getEntries().end());
	EXPECT_EQ(selected->value, 73);
}

TEST_F(ClassicBattleAIIntegrationTest, RunSelectorRequiresClassicHardGateAndNegativeCurrentDanger)
{
	removeAllStacks();
	battle()->obstacles.clear();
	CStack * actor = addStack(BattleSide::ATTACKER, CreatureID(0), BattleHex(56), 100);
	addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(151), 100);
	auto callback = battleCallback();
	ClassicCombatValue values(callback);
	const ClassicCombatParameters parameters = values.buildParameters(BattleSide::ATTACKER, 2);

	ClassicAttackEvaluator hardEvaluator(callback, std::make_shared<UpperBoundClassicBattleAIRng>(), nullptr, true);
	EXPECT_FALSE(hardEvaluator.chooseRunAction(actor, parameters).valid);

	auto fireWall = std::make_shared<SpellCreatedObstacle>();
	fireWall->ID = SpellID(SpellID::FIRE_WALL).getNum();
	fireWall->pos = actor->getPosition();
	fireWall->uniqueID = 0;
	fireWall->turnsRemaining = 2;
	fireWall->casterSide = BattleSide::DEFENDER;
	fireWall->minimalDamage = 1000;
	fireWall->passable = true;
	fireWall->trigger = SpellID(SpellID::FIRE_WALL);
	fireWall->customSize.insert(actor->getPosition());
	battle()->obstacles.push_back(std::move(fireWall));
	battle()->nodeHasChanged();

	ClassicAttackEvaluator normalEvaluator(callback, std::make_shared<UpperBoundClassicBattleAIRng>(), nullptr, false);
	EXPECT_FALSE(normalEvaluator.chooseRunAction(actor, parameters).valid);
}

TEST_F(ClassicBattleAIIntegrationTest, RunSelectorRejectsEveryOriginalIncapacitatingEffect)
{
	const std::array<SpellID, 3> disablingSpells = {
		SpellID(SpellID::BLIND),
		SpellID(SpellID::STONE_GAZE),
		SpellID(SpellID::PARALYZE),
	};
	for(const SpellID spell : disablingSpells)
	{
		SCOPED_TRACE(spell.getNum());
		removeAllStacks();
		battle()->obstacles.clear();
		CStack * actor = addStack(BattleSide::ATTACKER, CreatureID(0), BattleHex(56), 100);
		addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(151), 100);
		auto fireWall = std::make_shared<SpellCreatedObstacle>();
		fireWall->ID = SpellID(SpellID::FIRE_WALL).getNum();
		fireWall->pos = actor->getPosition();
		fireWall->uniqueID = 0;
		fireWall->turnsRemaining = 2;
		fireWall->casterSide = BattleSide::DEFENDER;
		fireWall->minimalDamage = 1000;
		fireWall->passable = true;
		fireWall->trigger = SpellID(SpellID::FIRE_WALL);
		fireWall->customSize.insert(actor->getPosition());
		battle()->obstacles.push_back(std::move(fireWall));
		auto disabled = std::make_shared<Bonus>(BonusDuration::N_TURNS, BonusType::NOT_ACTIVE, BonusSource::SPELL_EFFECT, 0, BonusSourceID(spell));
		disabled->turnsRemain = 1;
		actor->addNewBonus(disabled);
		battle()->nodeHasChanged();

		auto callback = battleCallback();
		ClassicCombatValue values(callback);
		const ClassicCombatParameters parameters = values.buildParameters(BattleSide::ATTACKER, 2);
		ClassicAttackEvaluator evaluator(callback, std::make_shared<UpperBoundClassicBattleAIRng>(), nullptr, true);
		EXPECT_FALSE(evaluator.chooseRunAction(actor, parameters).valid);
	}

	removeAllStacks();
	battle()->obstacles.clear();
	CStack * bound = addStack(BattleSide::ATTACKER, CreatureID(0), BattleHex(56), 100);
	addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(151), 100);
	auto fireWall = std::make_shared<SpellCreatedObstacle>();
	fireWall->ID = SpellID(SpellID::FIRE_WALL).getNum();
	fireWall->pos = bound->getPosition();
	fireWall->uniqueID = 0;
	fireWall->turnsRemaining = 2;
	fireWall->casterSide = BattleSide::DEFENDER;
	fireWall->minimalDamage = 1000;
	fireWall->passable = true;
	fireWall->trigger = SpellID(SpellID::FIRE_WALL);
	fireWall->customSize.insert(bound->getPosition());
	battle()->obstacles.push_back(std::move(fireWall));
	bound->addNewBonus(std::make_shared<Bonus>(BonusDuration::ONE_BATTLE, BonusType::BIND_EFFECT, BonusSource::CREATURE_ABILITY, 0, BonusSourceID()));
	battle()->nodeHasChanged();
	auto callback = battleCallback();
	ClassicCombatValue values(callback);
	const ClassicCombatParameters parameters = values.buildParameters(BattleSide::ATTACKER, 2);
	ClassicAttackEvaluator evaluator(callback, std::make_shared<UpperBoundClassicBattleAIRng>(), nullptr, true);
	EXPECT_FALSE(evaluator.chooseRunAction(bound, parameters).valid);
}

TEST_F(ClassicBattleAIIntegrationTest, NormalDifficultyMoveStillUsesDeduplicatedDangerProjection)
{
	removeAllStacks();
	battle()->obstacles.clear();
	battle()->terrainType = TerrainId(2);

	auto addStaticObstacle = [&](int32_t obstacleID, int32_t hex)
	{
		const ObstacleInfo * info = LIBRARY->obstacleHandler->getByName(std::to_string(obstacleID));
		ASSERT_NE(info, nullptr);
		auto obstacle = std::make_shared<CObstacleInstance>();
		obstacle->ID = info->obstacle.getNum();
		obstacle->pos = BattleHex(hex);
		obstacle->obstacleType = CObstacleInstance::USUAL;
		obstacle->uniqueID = static_cast<int32_t>(battle()->obstacles.size());
		battle()->obstacles.push_back(std::move(obstacle));
	};
	addStaticObstacle(23, 41);
	addStaticObstacle(21, 131);
	addStaticObstacle(19, 26);
	addStaticObstacle(20, 62);

	addStack(BattleSide::ATTACKER, CreatureID(0), BattleHex(1), 10);
	addStack(BattleSide::ATTACKER, CreatureID(6), BattleHex(35), 10);
	addStack(BattleSide::ATTACKER, CreatureID(24), BattleHex(70), 10);
	addStack(BattleSide::ATTACKER, CreatureID(39), BattleHex(104), 10);
	addStack(BattleSide::ATTACKER, CreatureID(90), BattleHex(137), 10);
	addStack(BattleSide::ATTACKER, CreatureID(96), BattleHex(172), 10);
	addStack(BattleSide::DEFENDER, CreatureID(96), BattleHex(14), 167);
	addStack(BattleSide::DEFENDER, CreatureID(96), BattleHex(48), 167);
	addStack(BattleSide::DEFENDER, CreatureID(96), BattleHex(82), 167);
	CStack * actor = addStack(BattleSide::DEFENDER, CreatureID(97), BattleHex(116), 167);
	addStack(BattleSide::DEFENDER, CreatureID(96), BattleHex(150), 166);
	addStack(BattleSide::DEFENDER, CreatureID(96), BattleHex(184), 166);
	battle()->nodeHasChanged();

	auto callback = battleCallback();
	ClassicCombatValue values(callback);
	const ClassicCombatParameters parameters = values.buildParameters(BattleSide::DEFENDER, 1);
	auto rng = std::make_shared<ReplayClassicBattleAIRng>(
		std::vector<int32_t>{76, 78, 88, 90, 92, 86});
	auto trace = std::make_shared<ClassicDecisionTrace>();
	// false models the Normal-difficulty computer-side long-wait policy. It
	// must not disable danger-map construction.
	ClassicAttackEvaluator evaluator(callback, rng, trace, false, false);

	const ClassicScoredAction result = evaluator.chooseAction(actor, parameters);
	ASSERT_TRUE(result.valid);
	EXPECT_EQ(result.action.actionType, EActionType::WALK);
	ASSERT_FALSE(result.action.target.empty());
	EXPECT_EQ(result.action.target.front().hexValue, BattleHex(163));
	EXPECT_TRUE(rng->exhausted());

	auto traceValue = [&](std::string_view stage, int32_t hex) -> std::optional<int64_t>
	{
		const auto found = std::ranges::find_if(
			trace->getEntries(),
			[&](const ClassicDecisionTraceEntry & entry)
			{
				return entry.stage == stage && entry.key == std::to_string(hex);
			});
		return found == trace->getEntries().end()
			? std::nullopt
			: std::optional<int64_t>(found->value);
	};
	const auto danger162 = traceValue("move_toward.candidate_head", 162);
	const auto danger161 = traceValue("move_toward.candidate_head", 161);
	const auto danger160 = traceValue("move_toward.candidate_head", 160);
	ASSERT_TRUE(danger162.has_value());
	ASSERT_TRUE(danger161.has_value());
	ASSERT_TRUE(danger160.has_value());
	EXPECT_LT(*danger162, 0);
	EXPECT_EQ(*danger161, *danger162);
	EXPECT_EQ(*danger160, *danger162);
	EXPECT_EQ(traceValue("move_toward.result", 2), 163);
}

TEST_F(ClassicBattleAIIntegrationTest, MeleeTargetSelectionPrefersEnabledStackOverHigherScoringBlindStack)
{
	removeAllStacks();
	battle()->obstacles.clear();
	CStack * swordsmen = addStack(BattleSide::ATTACKER, CreatureID(6), BattleHex(86), 1000);
	CStack * enabled = addStack(BattleSide::DEFENDER, CreatureID(96), BattleHex(87), 167);
	CStack * blinded = addStack(BattleSide::DEFENDER, CreatureID(96), BattleHex(70), 165);
	auto blind = std::make_shared<Bonus>(
		BonusDuration::N_TURNS,
		BonusType::NOT_ACTIVE,
		BonusSource::SPELL_EFFECT,
		0,
		BonusSourceID(SpellID(SpellID::BLIND)));
	blind->turnsRemain = 3;
	blinded->addNewBonus(blind);
	auto blindRetaliation = std::make_shared<Bonus>(
		BonusDuration::N_TURNS,
		BonusType::GENERAL_ATTACK_REDUCTION,
		BonusSource::SPELL_EFFECT,
		50,
		BonusSourceID(SpellID(SpellID::BLIND)));
	blindRetaliation->turnsRemain = 3;
	blinded->addNewBonus(blindRetaliation);
	JsonNode blindedState = blinded->save();
	blindedState["counterAttacks"]["used"].Integer() = blinded->counterAttacks.total();
	blinded->load(blindedState);
	battle()->nodeHasChanged();

	auto callback = battleCallback();
	ClassicCombatValue values(callback);
	const ClassicCombatParameters parameters = values.buildParameters(BattleSide::ATTACKER, 1);
	auto rng = std::make_shared<ReplayClassicBattleAIRng>(std::vector<int32_t>{100, 100});
	auto trace = std::make_shared<ClassicDecisionTrace>();
	ClassicAttackEvaluator evaluator(callback, rng, trace);

	const ClassicScoredAction result = evaluator.chooseAction(swordsmen, parameters);
	ASSERT_TRUE(result.valid);
	EXPECT_EQ(result.action.actionType, EActionType::WALK_AND_ATTACK);
	ASSERT_FALSE(result.action.target.empty());
	ASSERT_GT(result.action.target.size(), 1);
	EXPECT_EQ(result.action.target[1].unitValue, enabled->unitId());
	EXPECT_TRUE(rng->exhausted());

	auto randomizedScore = [&](const CStack * target) -> std::optional<int64_t>
	{
		const auto found = std::ranges::find_if(
			trace->getEntries(),
			[target](const ClassicDecisionTraceEntry & entry)
			{
				return entry.stage == "melee.randomized"
					&& entry.key == std::to_string(target->getPosition().toInt());
			});
		return found == trace->getEntries().end()
			? std::nullopt
			: std::optional<int64_t>(found->value);
	};
	const auto enabledScore = randomizedScore(enabled);
	const auto blindedScore = randomizedScore(blinded);
	ASSERT_TRUE(enabledScore.has_value());
	ASSERT_TRUE(blindedScore.has_value());
	EXPECT_GT(*blindedScore, *enabledScore);
	for(const CStack * target : {enabled, blinded})
	{
		const auto time = std::ranges::find_if(
			trace->getEntries(),
			[target](const ClassicDecisionTraceEntry & entry)
			{
				return entry.stage == "melee.time"
					&& entry.key == std::to_string(target->unitId());
			});
		ASSERT_NE(time, trace->getEntries().end());
		EXPECT_EQ(time->value, 1);
	}
}

TEST_F(ClassicBattleAIIntegrationTest, StackEntryPointConsumesOnlyDoCompAIRandomness)
{
	auto callback = battleCallback();
	ClassicBattleStateView view(callback);
	const CStack * attacker = view.orderedStacks().front();
	auto rng = std::make_shared<ReplayClassicBattleAIRng>(std::vector<int32_t>{75});
	auto trace = std::make_shared<ClassicDecisionTrace>();
	AutocombatPreferences preferences;
	preferences.enableSpellsUsage = true;
	const BattleAction action = ClassicBattleDecision::decide(
		gameHandler.get(), callback, attacker->unitSide(), attacker, 2, preferences, rng, trace,
		ClassicDecisionEntryPoint::DO_COMP_AI);
	EXPECT_NE(action.actionType, EActionType::NO_ACTION);
	EXPECT_EQ(rng->consumed(), 1);
	EXPECT_TRUE(rng->exhausted());
}

TEST_F(ClassicBattleAIIntegrationTest, RejectsModdedCreaturesOutsideConformanceDomain)
{
	CStack * modded = addStack(
		BattleSide::ATTACKER,
		creatureByName("vcmi-test:testSoulStealer"),
		BattleHex(leftHex),
		1
	);
	AutocombatPreferences preferences;
	preferences.enableSpellsUsage = false;
	preferences.enableTacticsUsage = false;
	EXPECT_THROW(
		ClassicBattleDecision::decide(
			gameHandler.get(),
			battleCallback(),
			BattleSide::ATTACKER,
			modded,
			4,
			preferences,
			std::make_shared<UpperBoundClassicBattleAIRng>(),
			nullptr
		),
		std::domain_error
	);
}

TEST_F(ClassicBattleAIIntegrationTest, EvaluatesHeroSpellAndRetreatWithRealHeroState)
{
	auto callback = battleCallback();
	giveArtifact(attackerSideHero, ArtifactID::SPELLBOOK, ArtifactPosition::SPELLBOOK);
	attackerSideHero->addSpellToSpellbook(SpellID::MAGIC_ARROW);
	attackerSideHero->setPrimarySkill(PrimarySkill::SPELL_POWER, 10, ChangeValueMode::ABSOLUTE);
	attackerSideHero->mana = 100;
	attackerSideHero->exp = 5000;

	auto rng = std::make_shared<UpperBoundClassicBattleAIRng>();
	auto trace = std::make_shared<ClassicDecisionTrace>();
	ClassicSpellEvaluator spells(gameHandler.get(), callback, rng, trace);
	EXPECT_TRUE(spells.canChooseHeroSpell(BattleSide::ATTACKER));
	ClassicCombatParameters parameters;
	const ClassicScoredSpell spell = spells.chooseHeroSpell(
		BattleSide::ATTACKER, false, parameters);
	EXPECT_TRUE(spell.valid);
	EXPECT_EQ(spell.action.actionType, EActionType::HERO_SPELL);

	parameters.friendlyCombatValue = 1;
	parameters.enemyCombatValue = 100000;
	addStack(BattleSide::DEFENDER, CreatureID(13), BattleHex(120), 10000);
	ClassicRetreatEvaluator retreat(callback, rng, trace);
	EXPECT_TRUE(retreat.shouldRetreat(BattleSide::ATTACKER, 4, parameters));
}

TEST_F(ClassicBattleAIIntegrationTest, EvaluatesSingleTargetBasicHaste)
{
	removeAllStacks();
	CStack * pikemen = addStack(BattleSide::ATTACKER, CreatureID(0), BattleHex(35), 100);
	addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(151), 100);
	giveArtifact(attackerSideHero, ArtifactID::SPELLBOOK, ArtifactPosition::SPELLBOOK);
	attackerSideHero->addSpellToSpellbook(SpellID::HASTE);
	attackerSideHero->setSecSkillLevel(
		SecondarySkill::AIR_MAGIC, 1, ChangeValueMode::ABSOLUTE);
	attackerSideHero->setPrimarySkill(
		PrimarySkill::SPELL_POWER, 10, ChangeValueMode::ABSOLUTE);
	attackerSideHero->mana = 100;

	auto callback = battleCallback();
	auto rng = std::make_shared<UpperBoundClassicBattleAIRng>();
	auto trace = std::make_shared<ClassicDecisionTrace>();
	ClassicCombatValue values(callback);
	const ClassicCombatParameters parameters = values.buildParameters(BattleSide::ATTACKER, 4);
	ClassicAttackEvaluator attacks(callback, rng, trace);
	ClassicSpellEvaluator spells(
		gameHandler.get(), callback, rng, trace, nullptr, nullptr, &attacks);
	const ClassicScoredSpell spell = spells.chooseHeroSpell(
		BattleSide::ATTACKER, false, parameters);

	ASSERT_TRUE(spell.valid);
	EXPECT_EQ(spell.action.actionType, EActionType::HERO_SPELL);
	EXPECT_EQ(spell.action.spell, SpellID::HASTE);
	ASSERT_FALSE(spell.action.target.empty());
	EXPECT_EQ(spell.action.target.front().unitValue, pikemen->unitId());
	EXPECT_TRUE(std::ranges::any_of(
		trace->getEntries(),
		[pikemen](const ClassicDecisionTraceEntry & entry)
		{
			return entry.stage == "spell.haste.stack"
				&& entry.key == std::to_string(pikemen->unitId());
		}));
}

TEST_F(ClassicBattleAIIntegrationTest, EvaluatesSingleTargetBasicSlow)
{
	removeAllStacks();
	addStack(BattleSide::ATTACKER, CreatureID(0), BattleHex(35), 100);
	CStack * pikemen = addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(151), 100);
	giveArtifact(attackerSideHero, ArtifactID::SPELLBOOK, ArtifactPosition::SPELLBOOK);
	attackerSideHero->addSpellToSpellbook(SpellID::SLOW);
	attackerSideHero->setSecSkillLevel(
		SecondarySkill::EARTH_MAGIC, 1, ChangeValueMode::ABSOLUTE);
	attackerSideHero->setPrimarySkill(
		PrimarySkill::SPELL_POWER, 10, ChangeValueMode::ABSOLUTE);
	attackerSideHero->mana = 100;

	auto callback = battleCallback();
	auto rng = std::make_shared<UpperBoundClassicBattleAIRng>();
	auto trace = std::make_shared<ClassicDecisionTrace>();
	ClassicCombatValue values(callback);
	const ClassicCombatParameters parameters = values.buildParameters(BattleSide::ATTACKER, 4);
	ClassicAttackEvaluator attacks(callback, rng, trace);
	ClassicSpellEvaluator spells(
		gameHandler.get(), callback, rng, trace, nullptr, nullptr, &attacks);
	const ClassicScoredSpell spell = spells.chooseHeroSpell(
		BattleSide::ATTACKER, false, parameters);

	ASSERT_TRUE(spell.valid);
	EXPECT_EQ(spell.action.actionType, EActionType::HERO_SPELL);
	EXPECT_EQ(spell.action.spell, SpellID::SLOW);
	ASSERT_FALSE(spell.action.target.empty());
	EXPECT_EQ(spell.action.target.front().unitValue, pikemen->unitId());
	EXPECT_TRUE(std::ranges::any_of(
		trace->getEntries(),
		[pikemen](const ClassicDecisionTraceEntry & entry)
		{
			return entry.stage == "spell.slow.stack"
				&& entry.key == std::to_string(pikemen->unitId());
		}));
}

TEST_F(ClassicBattleAIIntegrationTest, HeroSpellPipelineProjectsBeforeRetreatEligibilityGate)
{
	removeAllStacks();
	battle()->obstacles.clear();
	CStack * pikemen = addStack(BattleSide::ATTACKER, CreatureID(0), BattleHex(86), 100);
	addStack(BattleSide::DEFENDER, CreatureID(94), BattleHex(15), 2);
	addStack(BattleSide::DEFENDER, CreatureID(94), BattleHex(83), 2);
	addStack(BattleSide::DEFENDER, CreatureID(95), BattleHex(117), 2);
	addStack(BattleSide::DEFENDER, CreatureID(94), BattleHex(185), 1);
	giveArtifact(attackerSideHero, ArtifactID::SPELLBOOK, ArtifactPosition::SPELLBOOK);
	attackerSideHero->addSpellToSpellbook(SpellID::MAGIC_ARROW);
	attackerSideHero->setPrimarySkill(PrimarySkill::SPELL_POWER, 10, ChangeValueMode::ABSOLUTE);
	attackerSideHero->mana = 100;
	attackerSideHero->exp = 0;

	auto rng = std::make_shared<ReplayClassicBattleAIRng>(
		std::vector<int32_t>{92});
	auto trace = std::make_shared<ClassicDecisionTrace>();
	AutocombatPreferences preferences;
	preferences.enableSpellsUsage = true;
	const BattleAction action = ClassicBattleDecision::decide(
		gameHandler.get(), battleCallback(), BattleSide::ATTACKER, pikemen, 1,
		preferences, rng, trace, ClassicDecisionEntryPoint::DO_SPELL_AI);

	EXPECT_EQ(action.actionType, EActionType::HERO_SPELL);
	EXPECT_EQ(action.spell, SpellID::MAGIC_ARROW);
	ASSERT_GE(trace->getEntries().size(), 2);
	EXPECT_TRUE(std::ranges::any_of(trace->getEntries(), [](const auto & entry)
	{
		return entry.stage == "spell.prepass" && entry.key == "begin";
	}));
	EXPECT_TRUE(std::ranges::any_of(trace->getEntries(), [](const auto & entry)
	{
		return entry.stage == "spell.prepass" && entry.key == "end";
	}));
	EXPECT_EQ(rng->consumed(), 1);
	EXPECT_TRUE(rng->exhausted());
}

TEST_F(ClassicBattleAIIntegrationTest, RetreatRequiresSurrenderCostPlusTwentyFiveHundredGold)
{
	attackerSideHero->exp = 5000;
	// Keep the side alive through the one-action projection while retaining a
	// clearly losing raw fight-value share. Otherwise projected extinction is
	// an earlier unconditional retreat gate and the treasury boundary is never
	// reached.
	addStack(BattleSide::ATTACKER, CreatureID(56), BattleHex(52), 100000);
	addStack(BattleSide::DEFENDER, CreatureID(13), BattleHex(120), 10000);
	auto callback = battleCallback();
	auto rng = std::make_shared<UpperBoundClassicBattleAIRng>();
	ClassicRetreatEvaluator retreat(callback, rng, nullptr, gameHandler.get());
	ClassicCombatParameters parameters;

	auto setGold = [&](int32_t amount)
	{
		SetResources resources;
		resources.player = attackerSideHero->getOwner();
		resources.mode = ChangeValueMode::ABSOLUTE;
		resources.res[GameResID::GOLD] = amount;
		gameHandler->sendAndApply(resources);
	};
	const int32_t required = callback->battleGetSurrenderCost(attackerSideHero->getOwner()) + 2500;
	ASSERT_GT(required, 0);
	setGold(required - 1);
	EXPECT_FALSE(retreat.shouldRetreat(BattleSide::ATTACKER, 4, parameters));
	setGold(required);
	EXPECT_TRUE(retreat.shouldRetreat(BattleSide::ATTACKER, 4, parameters));
}

TEST_F(ClassicBattleAIIntegrationTest, LocalHumanAutocombatSkipsTheLowDifficultyRoll)
{
	attackerSideHero->exp = 0;
	while(!attackerSideHero->artifactsWorn.empty())
		attackerSideHero->removeArtifact(attackerSideHero->artifactsWorn.begin()->first);
	while(!attackerSideHero->artifactsInBackpack.empty())
		attackerSideHero->removeArtifact(ArtifactPosition::BACKPACK_START);
	PlayerState * player = gameState()->getPlayerState(attackerSideHero->getOwner());
	player->human = true;
	auto callback = battleCallback();
	auto humanRng = std::make_shared<ReplayClassicBattleAIRng>(std::vector<int32_t>{});
	ClassicRetreatEvaluator humanRetreat(callback, humanRng, nullptr, gameHandler.get());
	ClassicCombatParameters parameters;
	EXPECT_FALSE(humanRetreat.shouldRetreat(BattleSide::ATTACKER, 1, parameters));
	EXPECT_EQ(humanRng->consumed(), 0);

	player->human = false;
	auto aiRng = std::make_shared<ReplayClassicBattleAIRng>(std::vector<int32_t>{50});
	ClassicRetreatEvaluator aiRetreat(callback, aiRng, nullptr, gameHandler.get());
	EXPECT_FALSE(aiRetreat.shouldRetreat(BattleSide::ATTACKER, 1, parameters));
	EXPECT_EQ(aiRng->consumed(), 1);
}

TEST_F(ClassicBattleAIIntegrationTest, NormalDifficultyRollFiftyStopsAndFiftyOneContinues)
{
	removeAllStacks();
	CStack * pikemen = addStack(BattleSide::ATTACKER, CreatureID(0), BattleHex(35), 1);
	addStack(BattleSide::DEFENDER, CreatureID(13), BattleHex(151), 100);
	attackerSideHero->exp = 5000;
	gameState()->getPlayerState(attackerSideHero->getOwner())->human = false;
	const std::map<uint32_t, int64_t> projectedDamage = {
		{pikemen->unitId(), pikemen->getAvailableHealth()}
	};
	ClassicCombatParameters parameters;

	auto stoppedRng = std::make_shared<ReplayClassicBattleAIRng>(std::vector<int32_t>{50});
	ClassicRetreatEvaluator stopped(battleCallback(), stoppedRng, nullptr, gameHandler.get());
	EXPECT_FALSE(stopped.shouldRetreat(
		BattleSide::ATTACKER, 1, parameters, &projectedDamage));
	EXPECT_TRUE(stoppedRng->exhausted());

	auto continuedRng = std::make_shared<ReplayClassicBattleAIRng>(std::vector<int32_t>{51});
	ClassicRetreatEvaluator continued(battleCallback(), continuedRng, nullptr, gameHandler.get());
	EXPECT_TRUE(continued.shouldRetreat(
		BattleSide::ATTACKER, 1, parameters, &projectedDamage));
	EXPECT_TRUE(continuedRng->exhausted());
}

TEST_F(ClassicBattleAIIntegrationTest, RetreatUsesProjectedExtinctionBeforeTheGoldGate)
{
	removeAllStacks();
	CStack * shooter = addStack(BattleSide::ATTACKER, CreatureID(41), BattleHex(35), 100);
	CStack * victim = addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(151), 1);
	attackerSideHero->exp = 0;
	defenderSideHero->exp = 5000;
	auto callback = battleCallback();
	ASSERT_TRUE(callback->battleCanShoot(shooter, victim->getPosition()));

	SetResources resources;
	resources.player = defenderSideHero->getOwner();
	resources.mode = ChangeValueMode::ABSOLUTE;
	resources.res[GameResID::GOLD] = 0;
	gameHandler->sendAndApply(resources);

	auto rng = std::make_shared<UpperBoundClassicBattleAIRng>();
	auto trace = std::make_shared<ClassicDecisionTrace>();
	ClassicCombatValue values(callback);
	const ClassicCombatParameters parameters = values.buildParameters(BattleSide::DEFENDER, 4);
	ClassicAttackEvaluator projection(callback, rng, trace);
	const ClassicExpectedDamage expected = projection.projectExpectedDamage(
		BattleSide::DEFENDER, true, parameters);
	ASSERT_TRUE(expected.contains(victim->unitId()));
	EXPECT_EQ(expected.at(victim->unitId()), victim->getAvailableHealth());

	ClassicRetreatEvaluator retreat(callback, rng, trace, gameHandler.get());
	EXPECT_TRUE(retreat.shouldRetreat(BattleSide::DEFENDER, 4, parameters));
}

TEST_F(ClassicBattleAIIntegrationTest, BerserkStackUsesForcedActionPipeline)
{
	auto callback = battleCallback();
	ClassicBattleStateView view(callback);
	const CStack * attacker = view.orderedStacks().front();
	auto berserk = std::make_shared<Bonus>(
		BonusDuration::N_TURNS,
		BonusType::ATTACKS_NEAREST_CREATURE,
		BonusSource::SPELL_EFFECT,
		1,
		BonusSourceID(SpellID(SpellID::BERSERK))
	);
	berserk->turnsRemain = 1;
	const_cast<CStack *>(attacker)->addNewBonus(berserk);

	ClassicCombatValue values(callback);
	const auto parameters = values.buildParameters(BattleSide::ATTACKER, 4);
	auto rng = std::make_shared<UpperBoundClassicBattleAIRng>();
	ClassicAttackEvaluator evaluator(callback, rng, nullptr);
	const ClassicScoredAction result = evaluator.chooseAction(attacker, parameters);
	EXPECT_TRUE(result.valid);
	EXPECT_NE(result.action.actionType, EActionType::NO_ACTION);
}

TEST_F(ClassicBattleAIIntegrationTest, CreatureCasterEvaluatesLegalSpellTargets)
{
	CStack * caster = addStack(BattleSide::ATTACKER, CreatureID(13), BattleHex(leftHex), 10);
	CStack * wounded = addStack(BattleSide::ATTACKER, CreatureID(0), BattleHex(rightHex), 10);
	JsonNode state = wounded->save();
	state["state"]["health"]["firstHPleft"].Integer() = 1;
	state["state"]["health"]["fullUnits"].Integer() = 8;
	wounded->load(state);

	auto rng = std::make_shared<UpperBoundClassicBattleAIRng>();
	auto trace = std::make_shared<ClassicDecisionTrace>();
	ClassicSpellEvaluator spells(gameHandler.get(), battleCallback(), rng, trace);
	ClassicCombatParameters parameters;
	const ClassicScoredSpell spell = spells.chooseCreatureSpell(caster, 0, parameters);
	EXPECT_TRUE(spell.valid);
	EXPECT_EQ(spell.action.actionType, EActionType::MONSTER_SPELL);
}

TEST_F(ClassicBattleAIIntegrationTest, ChoosesRangedAttackWhenShooterHasALegalTarget)
{
	CStack * shooter = addStack(BattleSide::ATTACKER, CreatureID(2), BattleHex(leftHex), 20);
	addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(rightHex + 17), 20);
	auto callback = battleCallback();
	ClassicCombatValue values(callback);
	const auto parameters = values.buildParameters(BattleSide::ATTACKER, 4);
	auto rng = std::make_shared<UpperBoundClassicBattleAIRng>();
	ClassicAttackEvaluator evaluator(callback, rng, nullptr);

	const ClassicScoredAction result = evaluator.chooseAction(shooter, parameters);
	ASSERT_TRUE(result.valid);
	EXPECT_EQ(result.action.actionType, EActionType::SHOOT);
}

TEST_F(ClassicBattleAIIntegrationTest, SimulatedStrikeIsCappedAtDefenderHealth)
{
	removeAllStacks();
	CStack * shooter = addStack(BattleSide::ATTACKER, CreatureID(2), BattleHex(35), 100);
	CStack * victim = addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(rightHex + 17), 1);
	auto callback = battleCallback();
	ClassicCombatValue values(callback);
	const auto parameters = values.buildParameters(BattleSide::ATTACKER, 4);
	auto trace = std::make_shared<ClassicDecisionTrace>();
	ClassicAttackEvaluator evaluator(
		callback, std::make_shared<UpperBoundClassicBattleAIRng>(), trace);

	const ClassicScoredAction result = evaluator.chooseAction(shooter, parameters);
	ASSERT_TRUE(result.valid);
	ASSERT_EQ(result.action.actionType, EActionType::SHOOT);
	const std::string key = std::to_string(shooter->unitId()) + ":" + std::to_string(victim->unitId());
	const auto strike = std::ranges::find_if(
		trace->getEntries(),
		[&](const ClassicDecisionTraceEntry & entry)
		{
			return entry.stage == "attack_sim.first" && entry.key == key;
		});
	ASSERT_NE(strike, trace->getEntries().end());
	EXPECT_EQ(strike->value, victim->getAvailableHealth());
}

TEST_F(ClassicBattleAIIntegrationTest, DamageMidpointIsTakenAcrossTheWholeStack)
{
	removeAllStacks();
	// Archers deal 2..3 damage. For three creatures the executable takes
	// (2 + 3) * 3 / 2 = 7 before modifiers, rather than (2 + 3) / 2 * 3 = 6.
	CStack * shooter = addStack(BattleSide::ATTACKER, CreatureID(2), BattleHex(35), 3);
	CStack * victim = addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(43), 100);
	auto callback = battleCallback();
	ClassicCombatValue values(callback);
	const auto parameters = values.buildParameters(BattleSide::ATTACKER, 4);
	auto trace = std::make_shared<ClassicDecisionTrace>();
	ClassicAttackEvaluator evaluator(
		callback, std::make_shared<UpperBoundClassicBattleAIRng>(), trace);

	const ClassicScoredAction result = evaluator.chooseAction(shooter, parameters);
	ASSERT_TRUE(result.valid);
	ASSERT_EQ(result.action.actionType, EActionType::SHOOT);
	const std::string key = std::to_string(shooter->unitId()) + ":" + std::to_string(victim->unitId());
	const auto strike = std::ranges::find_if(
		trace->getEntries(),
		[&](const ClassicDecisionTraceEntry & entry)
		{
			return entry.stage == "attack_sim.first" && entry.key == key;
		});
	ASSERT_NE(strike, trace->getEntries().end());
	EXPECT_EQ(strike->value, 7);
}

TEST_F(ClassicBattleAIIntegrationTest, AreaShooterEvaluatesBothCellsOfDoubleWideTarget)
{
	removeAllStacks();
	CStack * magog = addStack(BattleSide::ATTACKER, CreatureID(45), BattleHex(35), 100);
	CStack * griffin = addStack(BattleSide::DEFENDER, CreatureID(4), BattleHex(112), 20);
	ASSERT_TRUE(griffin->doubleWide());

	auto callback = battleCallback();
	ClassicCombatValue values(callback);
	const auto parameters = values.buildParameters(BattleSide::ATTACKER, 4);
	auto rng = std::make_shared<UpperBoundClassicBattleAIRng>();
	auto trace = std::make_shared<ClassicDecisionTrace>();
	ClassicAttackEvaluator evaluator(callback, rng, trace);
	const ClassicScoredAction result = evaluator.chooseAction(magog, parameters);

	ASSERT_TRUE(result.valid);
	EXPECT_EQ(result.action.actionType, EActionType::SHOOT);
	ASSERT_FALSE(result.action.target.empty());
	EXPECT_EQ(result.action.target.front().unitValue, griffin->unitId());
	EXPECT_TRUE(griffin->getHexes().contains(result.action.target.front().hexValue));
	std::ostringstream projectionTrace;
	for(const auto & entry : trace->getEntries())
		projectionTrace << entry.stage << ':' << entry.key << '=' << entry.value << ' ';
	EXPECT_TRUE(std::ranges::any_of(
		trace->getEntries(),
		[&](const ClassicDecisionTraceEntry & entry)
		{
			return entry.stage == "shoot.center"
				&& entry.key == std::to_string(griffin->unitId())
				&& griffin->getHexes().contains(BattleHex(entry.value));
		}));
	EXPECT_TRUE(std::ranges::any_of(
		trace->getEntries(),
		[&](const ClassicDecisionTraceEntry & entry)
		{
			return entry.stage == "shoot.target_time"
				&& entry.key == std::to_string(griffin->unitId())
				&& entry.value > 0;
		})) << projectionTrace.str();
}

TEST_F(ClassicBattleAIIntegrationTest, AreaShooterSubtractsFriendlySplashDamage)
{
	removeAllStacks();
	CStack * magog = addStack(BattleSide::ATTACKER, CreatureID(45), BattleHex(35), 100);
	CStack * friendly = addStack(BattleSide::ATTACKER, CreatureID(0), BattleHex(111), 1);
	// Keep the defender's projected attack time identical after removing the
	// splash victim, so the score delta isolates friendly-fire valuation.
	addStack(BattleSide::ATTACKER, CreatureID(0), BattleHex(116), 1);
	CStack * enemy = addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(112), 100);
	auto callback = battleCallback();
	ClassicCombatValue values(callback);
	const ClassicCombatParameters parameters = values.buildParameters(BattleSide::ATTACKER, 4);
	auto scoreFor = [&](const std::shared_ptr<ClassicDecisionTrace> & trace)
	{
		const auto found = std::ranges::find_if(
			trace->getEntries(),
			[enemy](const ClassicDecisionTraceEntry & entry)
			{
				return entry.stage == "shoot"
					&& entry.key == std::to_string(enemy->unitId());
			});
		EXPECT_NE(found, trace->getEntries().end());
		return found == trace->getEntries().end() ? int64_t(0) : found->value;
	};

	auto rng = std::make_shared<UpperBoundClassicBattleAIRng>();
	auto withFriendlyTrace = std::make_shared<ClassicDecisionTrace>();
	ClassicAttackEvaluator withFriendly(callback, rng, withFriendlyTrace);
	const ClassicScoredAction withFriendlyResult = withFriendly.chooseAction(magog, parameters);
	ASSERT_TRUE(withFriendlyResult.valid);
	ASSERT_EQ(withFriendlyResult.action.actionType, EActionType::SHOOT);
	const int64_t withFriendlyScore = scoreFor(withFriendlyTrace);

	BattleUnitsChanged removal;
	removal.battleID = BattleID(0);
	removal.changedStacks.emplace_back(friendly->unitId(), UnitChanges::EOperation::REMOVE);
	gameHandler->sendAndApply(removal);
	auto withoutFriendlyTrace = std::make_shared<ClassicDecisionTrace>();
	ClassicAttackEvaluator withoutFriendly(callback, rng, withoutFriendlyTrace);
	const ClassicScoredAction withoutFriendlyResult = withoutFriendly.chooseAction(magog, parameters);
	ASSERT_TRUE(withoutFriendlyResult.valid);
	ASSERT_EQ(withoutFriendlyResult.action.actionType, EActionType::SHOOT);
	EXPECT_LT(withFriendlyScore, scoreFor(withoutFriendlyTrace));
}

TEST_F(ClassicBattleAIIntegrationTest, AreaShooterUsesRangedDamageForFriendlySplash)
{
	removeAllStacks();
	CStack * magog = addStack(BattleSide::ATTACKER, CreatureID(45), BattleHex(35), 100);
	CStack * friendly = addStack(BattleSide::ATTACKER, CreatureID(0), BattleHex(111), 30);
	addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(112), 100);
	auto callback = battleCallback();
	ClassicCombatValue values(callback);
	const ClassicCombatParameters parameters = values.buildParameters(BattleSide::ATTACKER, 4);
	auto trace = std::make_shared<ClassicDecisionTrace>();
	ClassicAttackEvaluator evaluator(
		callback, std::make_shared<UpperBoundClassicBattleAIRng>(), trace);
	ASSERT_TRUE(evaluator.chooseAction(magog, parameters).valid);

	const std::string friendlyKey =
		std::to_string(magog->unitId()) + ":" + std::to_string(friendly->unitId());
	const auto friendlyStrike = std::ranges::find_if(
		trace->getEntries(),
		[&](const ClassicDecisionTraceEntry & entry)
		{
			return entry.stage == "attack_sim.first" && entry.key == friendlyKey;
		});
	ASSERT_NE(friendlyStrike, trace->getEntries().end());
	BattleAttackInfo rangedAttack(magog, friendly, 0, true);
	const DamageEstimation rangedEstimate = callback->battleEstimateDamage(rangedAttack);
	const int64_t expectedRangedDamage = std::min<int64_t>(
		friendly->getAvailableHealth(),
		(rangedEstimate.damage.min + rangedEstimate.damage.max) / 2);
	BattleAttackInfo meleeAttack(magog, friendly, 0, false);
	const DamageEstimation meleeEstimate = callback->battleEstimateDamage(meleeAttack);
	const int64_t expectedMeleeDamage = std::min<int64_t>(
		friendly->getAvailableHealth(),
		(meleeEstimate.damage.min + meleeEstimate.damage.max) / 2);
	ASSERT_NE(expectedRangedDamage, expectedMeleeDamage);
	EXPECT_EQ(friendlyStrike->value, expectedRangedDamage);
}

TEST_F(ClassicBattleAIIntegrationTest, DeathCloudSkipsUndeadSplashTargets)
{
	removeAllStacks();
	CStack * lich = addStack(BattleSide::ATTACKER, CreatureID(64), BattleHex(35), 100);
	CStack * primary = addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(112), 100);
	CStack * secondary = addStack(BattleSide::DEFENDER, CreatureID(56), BattleHex(111), 100);
	// Preserve the defender side's projection when the adjacent stack is
	// replaced, isolating Death Cloud receptiveness from target-time scoring.
	addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(116), 1);
	auto callback = battleCallback();
	ClassicCombatValue values(callback);
	const ClassicCombatParameters parameters = values.buildParameters(BattleSide::ATTACKER, 4);
	auto rng = std::make_shared<UpperBoundClassicBattleAIRng>();
	auto scoreForPrimary = [&](const std::shared_ptr<ClassicDecisionTrace> & trace)
	{
		ClassicAttackEvaluator evaluator(callback, rng, trace);
		const ClassicScoredAction result = evaluator.chooseAction(lich, parameters);
		EXPECT_TRUE(result.valid);
		const auto found = std::ranges::find_if(
			trace->getEntries(),
			[primary](const ClassicDecisionTraceEntry & entry)
			{
				return entry.stage == "shoot"
					&& entry.key == std::to_string(primary->unitId());
			});
		EXPECT_NE(found, trace->getEntries().end());
		return found == trace->getEntries().end() ? int64_t(0) : found->value;
	};

	auto undeadTrace = std::make_shared<ClassicDecisionTrace>();
	const int64_t undeadScore = scoreForPrimary(undeadTrace);
	BattleUnitsChanged replacement;
	replacement.battleID = BattleID(0);
	replacement.changedStacks.emplace_back(secondary->unitId(), UnitChanges::EOperation::REMOVE);
	gameHandler->sendAndApply(replacement);
	addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(111), 100);
	auto livingTrace = std::make_shared<ClassicDecisionTrace>();
	EXPECT_GT(scoreForPrimary(livingTrace), undeadScore);
}

TEST_F(ClassicBattleAIIntegrationTest, MeleeSimulationRecalculatesSecondStrikeAfterRetaliation)
{
	CStack * crusaders = addStack(BattleSide::ATTACKER, CreatureID(7), BattleHex(77), 40);
	CStack * pikemen = addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(78), 120);
	auto callback = battleCallback();
	ClassicCombatValue values(callback);
	const auto parameters = values.buildParameters(BattleSide::ATTACKER, 4);
	auto rng = std::make_shared<ReplayClassicBattleAIRng>(std::vector<int32_t>(64, 100));
	auto trace = std::make_shared<ClassicDecisionTrace>();
	ClassicAttackEvaluator evaluator(callback, rng, trace);
	evaluator.chooseAction(crusaders, parameters);

	const std::string key = std::to_string(crusaders->unitId()) + ":" + std::to_string(pikemen->unitId());
	auto traceValue = [&](std::string_view stage) -> int64_t
	{
		const auto found = std::ranges::find_if(
			trace->getEntries(),
			[&](const ClassicDecisionTraceEntry & entry)
			{
				return entry.stage == stage && entry.key == key;
			});
		EXPECT_NE(found, trace->getEntries().end()) << stage;
		return found == trace->getEntries().end() ? 0 : found->value;
	};

	const int64_t first = traceValue("attack_sim.first");
	const int64_t retaliation = traceValue("attack_sim.retaliation");
	const int64_t second = traceValue("attack_sim.second");
	EXPECT_GT(first, 0);
	EXPECT_GT(retaliation, 0);
	EXPECT_GT(second, 0);
	EXPECT_LT(second, first);
	EXPECT_EQ(traceValue("attack_sim.fire_first"), 0);
	EXPECT_EQ(traceValue("attack_sim.fire_retaliation"), 0);
	EXPECT_EQ(traceValue("attack_sim.fire_second"), 0);
}

TEST_F(ClassicBattleAIIntegrationTest, MeleeSimulationAppliesInnateFireShieldBeforeEveryStrike)
{
	CStack * crusaders = addStack(BattleSide::ATTACKER, CreatureID(7), BattleHex(77), 1000);
	CStack * sultans = addStack(BattleSide::DEFENDER, CreatureID(53), BattleHex(78), 1000);
	auto callback = battleCallback();
	ClassicCombatValue values(callback);
	const auto parameters = values.buildParameters(BattleSide::ATTACKER, 4);
	auto rng = std::make_shared<ReplayClassicBattleAIRng>(std::vector<int32_t>(64, 100));
	auto trace = std::make_shared<ClassicDecisionTrace>();
	ClassicAttackEvaluator evaluator(callback, rng, trace);
	evaluator.chooseAction(crusaders, parameters);

	const std::string key = std::to_string(crusaders->unitId()) + ":" + std::to_string(sultans->unitId());
	auto traceValue = [&](std::string_view stage) -> int64_t
	{
		const auto found = std::ranges::find_if(
			trace->getEntries(),
			[&](const ClassicDecisionTraceEntry & entry)
			{
				return entry.stage == stage && entry.key == key;
			});
		EXPECT_NE(found, trace->getEntries().end()) << stage;
		return found == trace->getEntries().end() ? 0 : found->value;
	};

	EXPECT_GT(traceValue("attack_sim.fire_first"), 0);
	EXPECT_GT(traceValue("attack_sim.fire_second"), 0);
	EXPECT_LT(traceValue("attack_sim.attacker_after"), crusaders->getAvailableHealth());
}

TEST_F(ClassicBattleAISiegeIntegrationTest, CatapultTargetsGateThenFallsBackToWalls)
{
	auto callback = battleCallback();
	ClassicBattleStateView view(callback);
	const CStack * actor = view.orderedStacks().front();
	auto rng = std::make_shared<UpperBoundClassicBattleAIRng>();
	ClassicAttackEvaluator evaluator(callback, rng, nullptr);

	battle()->si.gateState = EGateState::CLOSED;
	BattleAction action = evaluator.chooseCatapultAction(actor);
	EXPECT_EQ(action.actionType, EActionType::CATAPULT);
	ASSERT_FALSE(action.target.empty());
	EXPECT_EQ(action.target.front().hexValue, callback->wallPartToBattleHex(EWallPart::GATE));

	battle()->si.gateState = EGateState::DESTROYED;
	for(auto & [part, state] : battle()->si.wallState)
		state = EWallState::NONE;
	battle()->si.wallState[EWallPart::KEEP] = EWallState::INTACT;
	action = evaluator.chooseCatapultAction(actor);
	EXPECT_EQ(action.actionType, EActionType::CATAPULT);
	EXPECT_EQ(action.target.front().hexValue, callback->wallPartToBattleHex(EWallPart::KEEP));

	battle()->si.wallState[EWallPart::KEEP] = EWallState::DESTROYED;
	EXPECT_EQ(evaluator.chooseCatapultAction(actor).actionType, EActionType::DEFEND);
}

TEST_F(ClassicBattleAISiegeIntegrationTest, GroundAttackerAdvancesToOriginalOutsideGateContour)
{
	BattleUnitsChanged removal;
	removal.battleID = BattleID(0);
	for(const CStack * stack : battle()->battleGetAllStacks(false))
		removal.changedStacks.emplace_back(stack->unitId(), UnitChanges::EOperation::REMOVE);
	gameHandler->sendAndApply(removal);

	for(auto & [part, state] : battle()->si.wallState)
		state = EWallState::REINFORCED;
	battle()->si.gateState = EGateState::CLOSED;
	CStack * attacker = addStack(BattleSide::ATTACKER, CreatureID(0), BattleHex(86), 97);
	attacker->addNewBonus(std::make_shared<Bonus>(
		BonusDuration::ONE_BATTLE,
		BonusType::STACKS_SPEED,
		BonusSource::TERRAIN_NATIVE,
		1,
		BonusSourceID()));
	addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(100), 100);

	auto callback = battleCallback();
	ClassicCombatValue values(callback);
	const auto parameters = values.buildParameters(BattleSide::ATTACKER, 4);
	auto rng = std::make_shared<ReplayClassicBattleAIRng>(std::vector<int32_t>{});
	auto trace = std::make_shared<ClassicDecisionTrace>();
	ClassicAttackEvaluator evaluator(callback, rng, trace);
	const ClassicScoredAction result = evaluator.chooseAction(attacker, parameters);

	ASSERT_TRUE(result.valid);
	EXPECT_EQ(result.action.actionType, EActionType::WALK);
	ASSERT_FALSE(result.action.target.empty());
	EXPECT_EQ(result.action.target.front().hexValue, BattleHex(91));
	EXPECT_EQ(rng->consumed(), 0u);
	const auto advance = std::ranges::find_if(trace->getEntries(), [](const ClassicDecisionTraceEntry & entry)
	{
		return entry.stage == "siege.advance";
	});
	ASSERT_NE(advance, trace->getEntries().end());
	EXPECT_EQ(advance->value, BattleHex::GATE_BRIDGE);
}

TEST_F(ClassicBattleAISiegeIntegrationTest, MoatDangerChargesDefendingStack)
{
	BattleUnitsChanged removal;
	removal.battleID = BattleID(0);
	for(const CStack * stack : battle()->battleGetAllStacks(false))
		removal.changedStacks.emplace_back(stack->unitId(), UnitChanges::EOperation::REMOVE);
	gameHandler->sendAndApply(removal);

	addStack(BattleSide::ATTACKER, CreatureID(94), BattleHex(86), 100);
	CStack * defender = addStack(BattleSide::DEFENDER, CreatureID(58), BattleHex(97), 409);
	auto moat = std::make_shared<SpellCreatedObstacle>();
	moat->obstacleType = CObstacleInstance::MOAT;
	moat->uniqueID = 0;
	moat->casterSide = BattleSide::DEFENDER;
	moat->minimalDamage = 70;
	moat->passable = true;
	moat->customSize.insert(defender->getPosition());
	battle()->obstacles.push_back(std::move(moat));
	battle()->nodeHasChanged();

	auto callback = battleCallback();
	ClassicCombatValue values(callback);
	const auto parameters = values.buildParameters(BattleSide::DEFENDER, 4);
	auto trace = std::make_shared<ClassicDecisionTrace>();
	ClassicAttackEvaluator evaluator(
		callback,
		std::make_shared<UpperBoundClassicBattleAIRng>(),
		trace,
		true,
		false);
	evaluator.chooseRunAction(defender, parameters);
	const auto moatDanger = std::ranges::find_if(trace->getEntries(), [](const auto & entry)
	{
		return entry.stage == "run.current";
	});
	ASSERT_NE(moatDanger, trace->getEntries().end());
	const int64_t health = defender->getAvailableHealth();
	const int64_t loss = values.lossValue(
		defender,
		health,
		std::max<int64_t>(0, health - 70),
		parameters,
		false,
		parameters.killsOnly);
	EXPECT_EQ(moatDanger->value, -loss);
}

TEST_F(ClassicBattleAISiegeIntegrationTest, ClosedGateRejectsDefenderPathBeforeMeleeRng)
{
	BattleUnitsChanged removal;
	removal.battleID = BattleID(0);
	for(const CStack * stack : battle()->battleGetAllStacks(false))
		removal.changedStacks.emplace_back(stack->unitId(), UnitChanges::EOperation::REMOVE);
	gameHandler->sendAndApply(removal);

	for(auto & [part, state] : battle()->si.wallState)
		state = EWallState::REINFORCED;
	battle()->si.gateState = EGateState::CLOSED;
	CStack * attacker = addStack(BattleSide::ATTACKER, CreatureID(0), BattleHex::GATE_BRIDGE, 94);
	CStack * defender = addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(100), 100);

	auto callback = battleCallback();
	ClassicCombatValue values(callback);
	const auto parameters = values.buildParameters(BattleSide::DEFENDER, 1);
	auto rng = std::make_shared<ReplayClassicBattleAIRng>(std::vector<int32_t>{});
	ClassicAttackEvaluator evaluator(callback, rng, nullptr, false, false);
	const ClassicScoredAction result = evaluator.chooseAction(defender, parameters);

	ASSERT_TRUE(result.valid);
	EXPECT_EQ(result.action.actionType, EActionType::DEFEND);
	ASSERT_EQ(result.action.target.size(), 1u);
	EXPECT_EQ(result.action.target.front().unitValue, attacker->unitId());
	EXPECT_EQ(result.action.target.front().hexValue, BattleHex::INVALID);
	EXPECT_EQ(rng->consumed(), 0u);
}

TEST_F(ClassicBattleAISiegeIntegrationTest, RunSelectorPrecedesWaitFallbackForThreatenedGarrison)
{
	BattleUnitsChanged removal;
	removal.battleID = BattleID(0);
	for(const CStack * stack : battle()->battleGetAllStacks(false))
		removal.changedStacks.emplace_back(stack->unitId(), UnitChanges::EOperation::REMOVE);
	gameHandler->sendAndApply(removal);

	for(auto & [part, state] : battle()->si.wallState)
		state = EWallState::REINFORCED;
	battle()->si.gateState = EGateState::CLOSED;
	addStack(BattleSide::ATTACKER, CreatureID(0), BattleHex::GATE_BRIDGE, 94);
	CStack * defender = addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(100), 100);
	auto fireWall = std::make_shared<SpellCreatedObstacle>();
	fireWall->ID = SpellID(SpellID::FIRE_WALL).getNum();
	fireWall->pos = defender->getPosition();
	fireWall->uniqueID = 0;
	fireWall->turnsRemaining = 2;
	fireWall->casterSide = BattleSide::ATTACKER;
	fireWall->minimalDamage = 1000;
	fireWall->passable = true;
	fireWall->trigger = SpellID(SpellID::FIRE_WALL);
	fireWall->customSize.insert(defender->getPosition());
	battle()->obstacles.push_back(std::move(fireWall));
	battle()->nodeHasChanged();

	auto callback = battleCallback();
	ClassicCombatValue values(callback);
	const auto parameters = values.buildParameters(BattleSide::DEFENDER, 4);
	auto trace = std::make_shared<ClassicDecisionTrace>();
	ClassicAttackEvaluator evaluator(callback, std::make_shared<UpperBoundClassicBattleAIRng>(), trace, true, false);
	const ClassicScoredAction result = evaluator.chooseAction(defender, parameters);

	ASSERT_TRUE(result.valid);
	EXPECT_EQ(result.action.actionType, EActionType::WALK);
	const auto current = std::ranges::find_if(
		trace->getEntries(),
		[](const auto & entry)
		{
			return entry.stage == "run.current";
		}
	);
	ASSERT_NE(current, trace->getEntries().end());
	EXPECT_LT(current->value, 0);
}

TEST_F(ClassicBattleAISiegeIntegrationTest, FailedSiegeIgnoresCatapultAndDefendsInsteadOfWaiting)
{
	BattleUnitsChanged removal;
	removal.battleID = BattleID(0);
	for(const CStack * stack : battle()->battleGetAllStacks(false))
		removal.changedStacks.emplace_back(stack->unitId(), UnitChanges::EOperation::REMOVE);
	gameHandler->sendAndApply(removal);

	for(auto & [part, state] : battle()->si.wallState)
		state = EWallState::REINFORCED;
	battle()->si.gateState = EGateState::CLOSED;
	CStack * attacker = addStack(BattleSide::ATTACKER, CreatureID(0), BattleHex::GATE_BRIDGE, 94);
	addStack(BattleSide::ATTACKER, CreatureID(CreatureID::CATAPULT), BattleHex(120), 1);
	addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(100), 100);

	auto callback = battleCallback();
	ClassicCombatValue values(callback);
	const auto parameters = values.buildParameters(BattleSide::ATTACKER, 4);
	auto rng = std::make_shared<ReplayClassicBattleAIRng>(std::vector<int32_t>{});
	// With the long-move policy enabled, an ordinary no-action fallback waits.
	// failed_siege is the condition that changes this exact case to DEFEND.
	ClassicAttackEvaluator evaluator(callback, rng, nullptr, true, false);
	const ClassicScoredAction result = evaluator.chooseAction(attacker, parameters);

	ASSERT_TRUE(result.valid);
	EXPECT_EQ(result.action.actionType, EActionType::DEFEND);
	EXPECT_EQ(rng->consumed(), 0u);
}

TEST_F(ClassicBattleAIIntegrationTest, HealingTentSelectsMostWoundedFriendly)
{
	CStack * lightlyWounded = addStack(BattleSide::ATTACKER, CreatureID(3), BattleHex(leftHex), 5);
	CStack * heavilyWounded = addStack(BattleSide::ATTACKER, CreatureID(3), BattleHex(rightHex), 5);
	JsonNode lightState = lightlyWounded->save();
	lightState["state"]["health"]["firstHPleft"].Integer() = lightlyWounded->getMaxHealth() - 1;
	lightlyWounded->load(lightState);
	JsonNode heavyState = heavilyWounded->save();
	heavyState["state"]["health"]["firstHPleft"].Integer() = 1;
	heavilyWounded->load(heavyState);

	auto callback = battleCallback();
	ClassicBattleStateView view(callback);
	const CStack * actor = view.orderedStacks().front();
	auto rng = std::make_shared<UpperBoundClassicBattleAIRng>();
	ClassicAttackEvaluator evaluator(callback, rng, nullptr);
	const BattleAction action = evaluator.chooseHealingTentAction(actor);
	EXPECT_EQ(action.actionType, EActionType::STACK_HEAL);
	ASSERT_FALSE(action.target.empty());
	EXPECT_EQ(action.target.front().unitValue, heavilyWounded->unitId());
}

TEST_F(ClassicBattleAISiegeIntegrationTest, CyclopsPrefersWeakestWallForStrandedArmy)
{
	BattleUnitsChanged removal;
	removal.battleID = BattleID(0);
	for(const CStack * stack : battle()->battleGetAllStacks(false))
		if(stack->unitSide() == BattleSide::DEFENDER)
			removal.changedStacks.emplace_back(stack->unitId(), UnitChanges::EOperation::REMOVE);
	gameHandler->sendAndApply(removal);

	CStack * cyclops = addStack(BattleSide::ATTACKER, CreatureID(94), BattleHex(35), 1);
	addStack(BattleSide::ATTACKER, CreatureID(0), BattleHex(52), 1000);
	battle()->si.gateState = EGateState::CLOSED;
	battle()->si.wallState[EWallPart::BELOW_GATE] = EWallState::DAMAGED;
	battle()->si.wallState[EWallPart::OVER_GATE] = EWallState::INTACT;
	battle()->si.wallState[EWallPart::BOTTOM_WALL] = EWallState::INTACT;
	battle()->si.wallState[EWallPart::UPPER_WALL] = EWallState::INTACT;

	auto callback = battleCallback();
	ClassicCombatValue values(callback);
	const auto parameters = values.buildParameters(BattleSide::ATTACKER, 4);
	auto rng = std::make_shared<ReplayClassicBattleAIRng>(std::vector<int32_t>{1});
	ClassicAttackEvaluator evaluator(callback, rng, nullptr);
	const ClassicScoredAction result = evaluator.chooseAction(cyclops, parameters);
	ASSERT_TRUE(result.valid);
	EXPECT_EQ(result.action.actionType, EActionType::CATAPULT);
	ASSERT_FALSE(result.action.target.empty());
	EXPECT_EQ(result.action.target.front().hexValue, callback->wallPartToBattleHex(EWallPart::BELOW_GATE));
	ASSERT_EQ(rng->getRequests().size(), 1u);
	EXPECT_EQ(rng->getRequests().front().lower, 1);
	EXPECT_EQ(rng->getRequests().front().upper, 1);
	EXPECT_EQ(rng->getRequests().front().value, 1);
}

TEST_F(ClassicBattleAISiegeIntegrationTest, CyclopsWallTieUsesOriginalOneBasedOrder)
{
	BattleUnitsChanged removal;
	removal.battleID = BattleID(0);
	for(const CStack * stack : battle()->battleGetAllStacks(false))
		if(stack->unitSide() == BattleSide::DEFENDER)
			removal.changedStacks.emplace_back(stack->unitId(), UnitChanges::EOperation::REMOVE);
	gameHandler->sendAndApply(removal);

	CStack * cyclops = addStack(BattleSide::ATTACKER, CreatureID(94), BattleHex(35), 1);
	addStack(BattleSide::ATTACKER, CreatureID(0), BattleHex(52), 1000);
	for(EWallPart part : {
		EWallPart::BELOW_GATE,
		EWallPart::OVER_GATE,
		EWallPart::BOTTOM_WALL,
		EWallPart::UPPER_WALL})
	{
		battle()->si.wallState[part] = EWallState::INTACT;
	}

	auto callback = battleCallback();
	ClassicCombatValue values(callback);
	const auto parameters = values.buildParameters(BattleSide::ATTACKER, 4);
	auto rng = std::make_shared<ReplayClassicBattleAIRng>(std::vector<int32_t>{3});
	ClassicAttackEvaluator evaluator(callback, rng, nullptr);
	const ClassicScoredAction result = evaluator.chooseAction(cyclops, parameters);

	ASSERT_TRUE(result.valid);
	EXPECT_EQ(result.action.actionType, EActionType::CATAPULT);
	ASSERT_FALSE(result.action.target.empty());
	EXPECT_EQ(result.action.target.front().hexValue, callback->wallPartToBattleHex(EWallPart::BOTTOM_WALL));
	ASSERT_EQ(rng->getRequests().size(), 1u);
	EXPECT_EQ(rng->getRequests().front().lower, 1);
	EXPECT_EQ(rng->getRequests().front().upper, 4);
	EXPECT_EQ(rng->getRequests().front().value, 3);
}

TEST_F(ClassicBattleAIIntegrationTest, ControllerCoversLifecycleDecisionAndTacticsDispatch)
{
	RecordingBattleAIClient client;
	auto callback = std::make_shared<CBattleCallback>(PlayerColor(0), &client);
	callback->onBattleStarted(battle());
	auto environment = std::static_pointer_cast<Environment>(gameHandler);
	auto rng = std::make_shared<UpperBoundClassicBattleAIRng>();
	auto trace = std::make_shared<ClassicDecisionTrace>();
	ClassicBattleController controller(environment, callback, PlayerColor(0), rng, trace);
	controller.battleStart(BattleID(0), BattleSide::ATTACKER);

	ClassicBattleStateView view(battleCallback());
	const CStack * attacker = view.orderedStacks().front();
	AutocombatPreferences preferences;
	preferences.enableSpellsUsage = false;
	preferences.enableTacticsUsage = false;
	const BattleAction action = controller.decideStackAction(BattleID(0), attacker, preferences);
	EXPECT_NE(action.actionType, EActionType::NO_ACTION);
	controller.activeStack(BattleID(0), attacker, preferences);
	controller.yourTacticPhase(BattleID(0), 0, preferences);

	preferences.enableTacticsUsage = true;
	battle()->tacticDistance = 2;
	battle()->tacticsSide = BattleSide::ATTACKER;
	controller.yourTacticPhase(BattleID(0), 2, preferences);
	controller.actionFinished(BattleID(1), BattleAction::makeDefend(attacker));
	controller.actionFinished(BattleID(0), BattleAction::makeMove(attacker, attacker->getPosition()));
	EXPECT_GT(client.requests, 0);

	controller.battleEnd(BattleID(0));
	callback->onBattleEnded(BattleID(0));
}

TEST(ClassicBattleAIOracleMap, WritesDeterministicSmokeScenario)
{
	const CreatureID pikeman(0);
	const CreatureID archangel(13);
	const auto bytes = TinyH3M::TinyH3MBuilder(EMapFormat::SOD)
		.size(36, false)
		.name("Classic BattleAI Oracle Smoke")
		.description("Move the red hero one tile east to start the deterministic oracle battle.")
		.difficulty(EMapDifficulty::NORMAL)
		.playerActive(PlayerColor(0))
		.hero({5, 5, 0}, HeroTypeID(0), PlayerColor(0))
		.heroGarrison({{pikeman, 100}})
		.heroExperience(0)
		.heroPrimary(2, 2, 1, 1)
		.heroSecondarySkills({{SecondarySkill::ARCHERY, 1}, {SecondarySkill::LEADERSHIP, 1}})
		.heroEquipped({{ArtifactPosition::MACH4, ArtifactID::CATAPULT}})
		.heroSpells({})
		.town({12, 12, 0}, FactionID::CASTLE, PlayerColor(0))
		.monster({6, 5, 0}, archangel, 1, 4)
		.buildAndDump("ClassicBattleAIOracleSmoke");

	EXPECT_FALSE(bytes.empty());
}

TEST(ClassicBattleAIOracleMap, WritesDeterministicShooterScenario)
{
	const CreatureID pikeman(0);
	const CreatureID titan(41);
	const auto bytes = TinyH3M::TinyH3MBuilder(EMapFormat::SOD)
		.size(36, false)
		.name("Classic BattleAI Oracle Shooter")
		.description("Move the red hero one tile east to start the deterministic ranged-AI oracle battle.")
		.difficulty(EMapDifficulty::NORMAL)
		.playerActive(PlayerColor(0))
		.hero({5, 5, 0}, HeroTypeID(0), PlayerColor(0))
		.heroGarrison({{pikeman, 100}})
		.heroExperience(0)
		.heroPrimary(2, 2, 1, 1)
		.heroSecondarySkills({{SecondarySkill::ARCHERY, 1}, {SecondarySkill::LEADERSHIP, 1}})
		.heroEquipped({{ArtifactPosition::MACH4, ArtifactID::CATAPULT}})
		.heroSpells({})
		.town({12, 12, 0}, FactionID::CASTLE, PlayerColor(0))
		.monster({6, 5, 0}, titan, 1, 4)
		.buildAndDump("ClassicBattleAIOracleShooter");

	EXPECT_FALSE(bytes.empty());
}

TEST(ClassicBattleAIOracleMap, WritesDeterministicAreaShooterScenario)
{
	const CreatureID pikeman(0);
	const CreatureID archer(2);
	const CreatureID magog(45);
	const auto bytes = TinyH3M::TinyH3MBuilder(EMapFormat::SOD)
		.size(36, false)
		.name("Classic BattleAI Oracle Area Shooter")
		.description("Move the red hero one tile east to start the deterministic Magog oracle battle.")
		.difficulty(EMapDifficulty::NORMAL)
		.playerActive(PlayerColor(0))
		.hero({5, 5, 0}, HeroTypeID(0), PlayerColor(0))
		.heroGarrison({{pikeman, 100}, {archer, 100}})
		.heroExperience(0)
		.heroPrimary(2, 2, 1, 1)
		.heroSecondarySkills({})
		.heroEquipped({})
		.heroSpells({})
		.town({12, 12, 0}, FactionID::CASTLE, PlayerColor(0))
		.monster({6, 5, 0}, magog, 20, 4)
		.buildAndDump("ClassicBattleAIOracleAreaShooter");

	EXPECT_FALSE(bytes.empty());
}

TEST(ClassicBattleAIOracleMap, WritesDeterministicMagogFriendlySplashScenario)
{
	const CreatureID archangel(13);
	const CreatureID magog(45);
	const CreatureID pikeman(0);
	const auto bytes = TinyH3M::TinyH3MBuilder(EMapFormat::SOD)
		.size(36, false)
		.name("Classic BattleAI Oracle Magog Friendly Splash")
		.description("Move the Archangel beside the blue Pikemen, then enable auto-combat for the Magog decision.")
		.difficulty(EMapDifficulty::NORMAL)
		.playerActive(PlayerColor(0))
		.playerActive(PlayerColor(1))
		.hero({5, 5, 0}, HeroTypeID(0), PlayerColor(0))
		.heroGarrison({{archangel, 1}, {magog, 100}})
		.heroExperience(0)
		.heroPrimary(2, 2, 1, 1)
		.heroSecondarySkills({})
		.heroEquipped({})
		.heroSpells({})
		.hero({6, 5, 0}, HeroTypeID(1), PlayerColor(1))
		.heroGarrison({{pikeman, 100}})
		.heroExperience(0)
		.heroPrimary(2, 2, 1, 1)
		.heroSecondarySkills({})
		.heroEquipped({})
		.heroSpells({})
		.buildAndDump("ClassicBattleAIOracleMagogFriendlySplash");

	EXPECT_FALSE(bytes.empty());
}

TEST(ClassicBattleAIOracleMap, WritesDeterministicLichLivingSplashScenario)
{
	const CreatureID archangel(13);
	const CreatureID lich(64);
	const CreatureID pikeman(0);
	const auto bytes = TinyH3M::TinyH3MBuilder(EMapFormat::SOD)
		.size(36, false)
		.name("Classic BattleAI Oracle Lich Living Splash")
		.description("Move the Archangel beside the blue Pikemen, then enable auto-combat for the Lich decision.")
		.difficulty(EMapDifficulty::NORMAL)
		.playerActive(PlayerColor(0))
		.playerActive(PlayerColor(1))
		.hero({5, 5, 0}, HeroTypeID(0), PlayerColor(0))
		.heroGarrison({{archangel, 1}, {lich, 100}})
		.heroExperience(0)
		.heroPrimary(2, 2, 1, 1)
		.heroSecondarySkills({})
		.heroEquipped({})
		.heroSpells({})
		.hero({6, 5, 0}, HeroTypeID(1), PlayerColor(1))
		.heroGarrison({{pikeman, 100}})
		.heroExperience(0)
		.heroPrimary(2, 2, 1, 1)
		.heroSecondarySkills({})
		.heroEquipped({})
		.heroSpells({})
		.buildAndDump("ClassicBattleAIOracleLichLivingSplash");

	EXPECT_FALSE(bytes.empty());
}

TEST(ClassicBattleAIOracleMap, WritesDeterministicLichUndeadSplashScenario)
{
	const CreatureID ghostDragon(69);
	const CreatureID lich(64);
	const CreatureID pikeman(0);
	const auto bytes = TinyH3M::TinyH3MBuilder(EMapFormat::SOD)
		.size(36, false)
		.name("Classic BattleAI Oracle Lich Undead Splash")
		.description("Move the Ghost Dragon beside the blue Pikemen, then enable auto-combat for the Lich decision.")
		.difficulty(EMapDifficulty::NORMAL)
		.playerActive(PlayerColor(0))
		.playerActive(PlayerColor(1))
		.hero({5, 5, 0}, HeroTypeID(0), PlayerColor(0))
		.heroGarrison({{ghostDragon, 1}, {lich, 100}})
		.heroExperience(0)
		.heroPrimary(2, 2, 1, 1)
		.heroSecondarySkills({})
		.heroEquipped({})
		.heroSpells({})
		.hero({6, 5, 0}, HeroTypeID(1), PlayerColor(1))
		.heroGarrison({{pikeman, 100}})
		.heroExperience(0)
		.heroPrimary(2, 2, 1, 1)
		.heroSecondarySkills({})
		.heroEquipped({})
		.heroSpells({})
		.buildAndDump("ClassicBattleAIOracleLichUndeadSplash");

	EXPECT_FALSE(bytes.empty());
}

TEST(ClassicBattleAIOracleMap, WritesDeterministicAreaShotTraceInvarianceScenario)
{
	const CreatureID archangel(13);
	const CreatureID lich(64);
	const CreatureID dendroidGuard(22);
	const auto bytes = TinyH3M::TinyH3MBuilder(EMapFormat::SOD)
		.size(36, false)
		.name("Classic BattleAI Area-Shot Trace Invariance")
		.description("Move the Archangel beside the neutral Dendroids; compare traced and untraced Lich decisions.")
		.difficulty(EMapDifficulty::NORMAL)
		.playerActive(PlayerColor(0))
		.hero({5, 5, 0}, HeroTypeID(0), PlayerColor(0))
		.heroGarrison({{archangel, 1}, {lich, 100}})
		.heroExperience(0)
		.heroPrimary(2, 2, 1, 1)
		.heroSecondarySkills({})
		.heroEquipped({})
		.heroSpells({})
		.monster({6, 5, 0}, dendroidGuard, 1000, 4)
		.buildAndDump("ClassicBattleAIOracleAreaShotTraceInvariance");

	EXPECT_FALSE(bytes.empty());
}

TEST(ClassicBattleAIOracleMap, WritesDeterministicHeroSpellScenario)
{
	const CreatureID pikeman(0);
	const CreatureID cyclops(94);
	const auto bytes = TinyH3M::TinyH3MBuilder(EMapFormat::SOD)
		.size(36, false)
		.name("Classic BattleAI Oracle Hero Spell")
		.description("Move the red hero one tile east, then enable auto-combat for the spell-AI oracle battle.")
		.difficulty(EMapDifficulty::NORMAL)
		.playerActive(PlayerColor(0))
		.hero({5, 5, 0}, HeroTypeID(0), PlayerColor(0))
		.heroGarrison({{pikeman, 100}})
		.heroExperience(0)
		.heroPrimary(2, 2, 10, 10)
		.heroSecondarySkills({})
		.heroEquipped({
			{ArtifactPosition::SPELLBOOK, ArtifactID::SPELLBOOK},
			{ArtifactPosition::MACH4, ArtifactID::CATAPULT}})
		.heroSpells({SpellID::MAGIC_ARROW})
		.town({12, 12, 0}, FactionID::CASTLE, PlayerColor(0))
		.monster({6, 5, 0}, cyclops, 7, 4)
		.buildAndDump("ClassicBattleAIOracleHeroSpell");

	EXPECT_FALSE(bytes.empty());
}

TEST(ClassicBattleAIOracleMap, WritesDeterministicCreatureSpellScenario)
{
	const CreatureID faerieDragon(134);
	const CreatureID pikeman(0);
	const auto bytes = TinyH3M::TinyH3MBuilder(EMapFormat::SOD)
		.size(36, false)
		.name("Classic BattleAI Oracle Creature Spell")
		.description("Move the red hero one tile east, then enable auto-combat for the creature-spell oracle battle.")
		.difficulty(EMapDifficulty::NORMAL)
		.playerActive(PlayerColor(0))
		.hero({5, 5, 0}, HeroTypeID(0), PlayerColor(0))
		.heroGarrison({{faerieDragon, 4}})
		.heroExperience(0)
		.heroPrimary(2, 2, 1, 1)
		.heroSecondarySkills({})
		.heroEquipped({{ArtifactPosition::MACH4, ArtifactID::CATAPULT}})
		.heroSpells({})
		.town({12, 12, 0}, FactionID::CASTLE, PlayerColor(0))
		.monster({6, 5, 0}, pikeman, 1000, 4)
		.buildAndDump("ClassicBattleAIOracleCreatureSpell");

	EXPECT_FALSE(bytes.empty());
}

TEST(ClassicBattleAIOracleMap, WritesDeterministicBerserkScenario)
{
	const CreatureID titan(41);
	const CreatureID behemoth(96);
	const auto bytes = TinyH3M::TinyH3MBuilder(EMapFormat::SOD)
		.size(36, false)
		.name("Classic BattleAI Oracle Berserk")
		.description("Move the red hero one tile east, then enable auto-combat for the Berserk oracle battle.")
		.difficulty(EMapDifficulty::NORMAL)
		.playerActive(PlayerColor(0))
		.hero({5, 5, 0}, HeroTypeID(0), PlayerColor(0))
		// The fast shooter survives its first automatic action without a
		// retaliation; the split non-shooters then exercise forced Berserk AI.
		.heroGarrison({{titan, 100}})
		.heroExperience(0)
		.heroPrimary(2, 2, 10, 10)
		.heroSecondarySkills({{SecondarySkill::FIRE_MAGIC, 3}})
		.heroEquipped({
			{ArtifactPosition::SPELLBOOK, ArtifactID::SPELLBOOK},
			{ArtifactPosition::MACH4, ArtifactID::CATAPULT}})
		.heroSpells({SpellID::BERSERK})
		.town({12, 12, 0}, FactionID::CASTLE, PlayerColor(0))
		.monster({6, 5, 0}, behemoth, 1000, 1)
		.buildAndDump("ClassicBattleAIOracleBerserk");

	EXPECT_FALSE(bytes.empty());
}

TEST(ClassicBattleAIOracleMap, WritesDeterministicShooterDefenseScenario)
{
	const CreatureID grandElf(19);
	const CreatureID zombie(59);
	const auto bytes = TinyH3M::TinyH3MBuilder(EMapFormat::SOD)
		.size(36, false)
		.name("Classic BattleAI Oracle Shooter Defense")
		.description("Move the red hero one tile east, then enable auto-combat for the defensive-placement oracle battle.")
		.difficulty(EMapDifficulty::NORMAL)
		.playerActive(PlayerColor(0))
		.hero({5, 5, 0}, HeroTypeID(0), PlayerColor(0))
		.heroGarrison({
			{grandElf, 20}, {grandElf, 20}, {grandElf, 20},
			{grandElf, 20}, {grandElf, 20}, {grandElf, 20}})
		.heroExperience(0)
		.heroPrimary(2, 2, 1, 1)
		.heroSecondarySkills({})
		.heroEquipped({{ArtifactPosition::MACH4, ArtifactID::CATAPULT}})
		.heroSpells({})
		.town({12, 12, 0}, FactionID::CASTLE, PlayerColor(0))
		.monster({6, 5, 0}, zombie, 10000, 1)
		.buildAndDump("ClassicBattleAIOracleShooterDefense");

	EXPECT_FALSE(bytes.empty());
}

TEST(ClassicBattleAIOracleMap, WritesDeterministicTacticsScenario)
{
	const CreatureID swordsman(6);
	const CreatureID pikeman(0);
	const auto bytes = TinyH3M::TinyH3MBuilder(EMapFormat::SOD)
		.size(36, false)
		.name("Classic BattleAI Oracle Tactics")
		.description("Move the red hero one tile east, then enable auto-combat during tactics placement.")
		.difficulty(EMapDifficulty::NORMAL)
		.playerActive(PlayerColor(0))
		.hero({5, 5, 0}, HeroTypeID(0), PlayerColor(0))
		.heroGarrison({{swordsman, 100}})
		.heroExperience(0)
		.heroPrimary(2, 2, 1, 1)
		.heroSecondarySkills({{SecondarySkill::TACTICS, 3}})
		.heroEquipped({{ArtifactPosition::MACH4, ArtifactID::CATAPULT}})
		.heroSpells({})
		.town({12, 12, 0}, FactionID::CASTLE, PlayerColor(0))
		.monster({6, 5, 0}, pikeman, 100, 4)
		.buildAndDump("ClassicBattleAIOracleTactics");

	EXPECT_FALSE(bytes.empty());
}

TEST(ClassicBattleAIOracleMap, WritesDeterministicTacticsShooterScenario)
{
	const CreatureID grandElf(19);
	const CreatureID pikeman(0);
	const auto bytes = TinyH3M::TinyH3MBuilder(EMapFormat::SOD)
		.size(36, false)
		.name("Classic BattleAI Oracle Tactics Shooter")
		.description("Enable auto-combat during tactics placement to exercise shooter deployment.")
		.difficulty(EMapDifficulty::NORMAL)
		.playerActive(PlayerColor(0))
		.hero({5, 5, 0}, HeroTypeID(0), PlayerColor(0))
		.heroGarrison({{grandElf, 100}})
		.heroExperience(0)
		.heroPrimary(2, 2, 1, 1)
		.heroSecondarySkills({{SecondarySkill::TACTICS, 3}})
		.heroEquipped({{ArtifactPosition::MACH4, ArtifactID::CATAPULT}})
		.heroSpells({})
		.town({12, 12, 0}, FactionID::CASTLE, PlayerColor(0))
		.monster({6, 5, 0}, pikeman, 1000, 4)
		.buildAndDump("ClassicBattleAIOracleTacticsShooter");

	EXPECT_FALSE(bytes.empty());
}

TEST(ClassicBattleAIOracleMap, WritesDeterministicAdvancedSpellScenario)
{
	const CreatureID archangel(13);
	const CreatureID cyclops(94);
	const auto bytes = TinyH3M::TinyH3MBuilder(EMapFormat::SOD)
		.size(36, false)
		.name("Classic BattleAI Oracle Advanced Spells")
		.description("Enable auto-combat to evaluate chain and battlefield-wide damage spells.")
		.difficulty(EMapDifficulty::NORMAL)
		.playerActive(PlayerColor(0))
		.hero({5, 5, 0}, HeroTypeID(0), PlayerColor(0))
		.heroGarrison({{archangel, 2}})
		.heroExperience(0)
		.heroPrimary(2, 2, 10, 20)
		.heroSecondarySkills({
			{SecondarySkill::AIR_MAGIC, 3},
			{SecondarySkill::EARTH_MAGIC, 3},
			{SecondarySkill::FIRE_MAGIC, 3}})
		.heroEquipped({
			{ArtifactPosition::SPELLBOOK, ArtifactID::SPELLBOOK},
			{ArtifactPosition::MACH4, ArtifactID::CATAPULT}})
		.heroSpells({
			SpellID::CHAIN_LIGHTNING,
			SpellID::DEATH_RIPPLE,
			SpellID::DESTROY_UNDEAD,
			SpellID::ARMAGEDDON})
		.town({12, 12, 0}, FactionID::CASTLE, PlayerColor(0))
		.monster({6, 5, 0}, cyclops, 1000, 1)
		.buildAndDump("ClassicBattleAIOracleAdvancedSpells");

	EXPECT_FALSE(bytes.empty());
}

TEST(ClassicBattleAIOracleMap, WritesDeterministicGenieSpellScenario)
{
	const CreatureID masterGenie(37);
	const CreatureID nagaQueen(35);
	const CreatureID pikeman(0);
	const auto bytes = TinyH3M::TinyH3MBuilder(EMapFormat::SOD)
		.size(36, false)
		.name("Classic BattleAI Oracle Genie Spell")
		.description("Enable auto-combat to exercise the Genie creature-spell chooser.")
		.difficulty(EMapDifficulty::NORMAL)
		.playerActive(PlayerColor(0))
		.hero({5, 5, 0}, HeroTypeID(0), PlayerColor(0))
		.heroGarrison({{masterGenie, 20}, {nagaQueen, 20}})
		.heroExperience(0)
		.heroPrimary(2, 2, 1, 1)
		.heroSecondarySkills({})
		.heroEquipped({{ArtifactPosition::MACH4, ArtifactID::CATAPULT}})
		.heroSpells({})
		.town({12, 12, 0}, FactionID::CASTLE, PlayerColor(0))
		.monster({6, 5, 0}, pikeman, 1000, 4)
		.buildAndDump("ClassicBattleAIOracleGenieSpell");

	EXPECT_FALSE(bytes.empty());
}

TEST(ClassicBattleAIOracleMap, WritesDeterministicExchangeSpellScenario)
{
	const CreatureID pikeman(0);
	const CreatureID swordsman(6);
	const CreatureID unicorn(24);
	const CreatureID nagaQueen(39);
	const CreatureID ogre(90);
	const CreatureID behemoth(96);
	const CreatureID archangel(13);
	const auto bytes = TinyH3M::TinyH3MBuilder(EMapFormat::SOD)
		.size(36, false)
		.name("Classic BattleAI Oracle Exchange Spells")
		.description("Enable auto-combat to exercise exchange-based enchantment values.")
		.difficulty(EMapDifficulty::NORMAL)
		.playerActive(PlayerColor(0))
		.hero({5, 5, 0}, HeroTypeID(0), PlayerColor(0))
		.heroGarrison({
			{pikeman, 10},
			{swordsman, 10},
			{unicorn, 10},
			{nagaQueen, 10},
			{ogre, 10},
			{behemoth, 10},
			{archangel, 1}})
		.heroExperience(0)
		.heroPrimary(2, 2, 10, 20)
		.heroSecondarySkills({
			{SecondarySkill::AIR_MAGIC, 3},
			{SecondarySkill::EARTH_MAGIC, 3}})
		.heroEquipped({
			{ArtifactPosition::SPELLBOOK, ArtifactID::SPELLBOOK},
			{ArtifactPosition::MACH4, ArtifactID::CATAPULT}})
		.heroSpells({SpellID::HASTE, SpellID::SLOW})
		.town({12, 12, 0}, FactionID::CASTLE, PlayerColor(0))
		.monster({6, 5, 0}, behemoth, 1000, 1)
		.buildAndDump("ClassicBattleAIOracleExchangeSpells");

	EXPECT_FALSE(bytes.empty());
}

TEST(ClassicBattleAIOracleMap, WritesDeterministicSixStackExchangeSpellScenario)
{
	const CreatureID pikeman(0);
	const CreatureID swordsman(6);
	const CreatureID unicorn(24);
	const CreatureID nagaQueen(39);
	const CreatureID ogre(90);
	const CreatureID behemoth(96);
	const auto bytes = TinyH3M::TinyH3MBuilder(EMapFormat::SOD)
		.size(36, false)
		.name("Classic BattleAI Oracle Six-Stack Exchange Spells")
		.description("Exercise the original six-stack post-Haste movement scenario.")
		.difficulty(EMapDifficulty::NORMAL)
		.playerActive(PlayerColor(0))
		.hero({5, 5, 0}, HeroTypeID(0), PlayerColor(0))
		.heroGarrison({
			{pikeman, 10},
			{swordsman, 10},
			{unicorn, 10},
			{nagaQueen, 10},
			{ogre, 10},
			{behemoth, 10}})
		.heroExperience(0)
		.heroPrimary(2, 2, 10, 20)
		.heroSecondarySkills({
			{SecondarySkill::AIR_MAGIC, 3},
			{SecondarySkill::EARTH_MAGIC, 3}})
		.heroEquipped({
			{ArtifactPosition::SPELLBOOK, ArtifactID::SPELLBOOK},
			{ArtifactPosition::MACH4, ArtifactID::CATAPULT}})
		.heroSpells({SpellID::HASTE, SpellID::SLOW})
		.town({12, 12, 0}, FactionID::CASTLE, PlayerColor(0))
		.monster({6, 5, 0}, behemoth, 1000, 1)
		.buildAndDump("ClassicBattleAIOracleSixStackExchangeSpells");

	EXPECT_FALSE(bytes.empty());
}

TEST(ClassicBattleAIOracleMap, WritesDeterministicPersistentSlowScenario)
{
	const CreatureID pikeman(0);
	const CreatureID swordsman(6);
	const CreatureID unicorn(24);
	const CreatureID nagaQueen(39);
	const CreatureID ogre(90);
	const CreatureID behemoth(96);
	const CreatureID titan(41);
	const CreatureID masterGenie(37);
	const auto bytes = TinyH3M::TinyH3MBuilder(EMapFormat::SOD)
		.size(36, false)
		.name("Classic BattleAI Oracle Persistent Slow")
		.description("Enable auto-combat to exercise Slow against persistent AI target records.")
		.difficulty(EMapDifficulty::NORMAL)
		.playerActive(PlayerColor(0))
		.playerActive(PlayerColor(1))
		.hero({5, 5, 0}, HeroTypeID(0), PlayerColor(0))
		.heroGarrison({
			{pikeman, 20},
			{swordsman, 20},
			{unicorn, 20},
			{nagaQueen, 20},
			{ogre, 20},
			{behemoth, 20},
			{titan, 10}})
		.heroExperience(0)
		.heroPrimary(2, 2, 10, 20)
		.heroSecondarySkills({{SecondarySkill::AIR_MAGIC, 3}})
		.heroEquipped({
			{ArtifactPosition::SPELLBOOK, ArtifactID::SPELLBOOK},
			{ArtifactPosition::MACH4, ArtifactID::CATAPULT}})
		.heroSpells({SpellID::HASTE})
		.hero({6, 5, 0}, HeroTypeID(1), PlayerColor(1))
		.heroGarrison({{masterGenie, 250}})
		.heroExperience(0)
		.heroPrimary(2, 2, 10, 20)
		.heroSecondarySkills({{SecondarySkill::EARTH_MAGIC, 3}})
		.heroEquipped({
			{ArtifactPosition::SPELLBOOK, ArtifactID::SPELLBOOK},
			{ArtifactPosition::MACH4, ArtifactID::CATAPULT}})
		.heroSpells({SpellID::SLOW})
		.buildAndDump("ClassicBattleAIOraclePersistentSlow");

	EXPECT_FALSE(bytes.empty());
}

TEST(ClassicBattleAIOracleMap, WritesDeterministicSiegeScenario)
{
	const CreatureID pikeman(0);
	const auto bytes = TinyH3M::TinyH3MBuilder(EMapFormat::SOD)
		.size(36, false)
		.name("Classic BattleAI Oracle Siege")
		.description("A ground attacker exercises closed-fort movement and retreat logic.")
		.difficulty(EMapDifficulty::NORMAL)
		.playerActive(PlayerColor(0))
		.playerActive(PlayerColor(1))
		.hero({3, 5, 0}, HeroTypeID(0), PlayerColor(0))
		.heroGarrison({{pikeman, 100}})
		.heroExperience(0)
		.heroPrimary(2, 2, 1, 1)
		.heroSecondarySkills({})
		.heroEquipped({{ArtifactPosition::MACH4, ArtifactID::CATAPULT}})
		.heroSpells({})
		.town({6, 5, 0}, FactionID::CASTLE, PlayerColor(1))
		.townGarrison({{pikeman, 100}})
		.townFortification(3)
		.buildAndDump("ClassicBattleAIOracleSiege");

	EXPECT_FALSE(bytes.empty());
}

TEST_F(ClassicBattleAIProbe, ReplayNDJSON)
{
	if(!std::getenv("VCMI_CLASSIC_AI_PROBE"))
		GTEST_SKIP() << "persistent probe is enabled with VCMI_CLASSIC_AI_PROBE=1";
	std::string line;
	while(std::getline(std::cin, line))
	{
		if(line.empty())
			continue;
		try
		{
			replay(line);
		}
		catch(const std::exception & error)
		{
			std::cout << "{\"record_type\":\"decision\",\"error\":\"" << jsonEscape(error.what()) << "\"}"
					  << std::endl;
		}
	}
}
