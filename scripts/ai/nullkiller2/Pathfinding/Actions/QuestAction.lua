-- Mirrors AI/Nullkiller2/Pathfinding/Actions/QuestAction.{h,cpp}: quest path special action.

local CompleteQuest = require("Goals.CompleteQuest")

local QuestAction = {}
QuestAction.__index = QuestAction

local function call(object, name, ...)
	if object and type(object[name]) == "function" then
		local ok, result = pcall(object[name], object, ...)
		if ok then
			return result
		end
	end
	return nil
end

local function normalizedActionName(action)
	if type(action) ~= "table" then
		return ""
	end
	local name = action.type or action.kind or action.name or action.class or action.ID or ""
	return string.gsub(string.lower(tostring(name)), "[%s_%-]", "")
end

local function objectID(value)
	if type(value) == "number" then
		return value
	end
	if type(value) == "table" then
		return value.id or value.objectID or value.objectId or value.num or value[1]
	end
	return value
end

local function objectType(object)
	return object and (object.ID or object.typeID or object.idType or object.type)
end

local function objectIsBorderGate(object)
	local value = objectType(object)
	return value == "BORDER_GATE" or value == "BORDERGUARD" or value == 212 or value == 9
end

local function visitablePos(object)
	return call(object, "visitablePos") or object and (object.visitablePos or object.tile)
end

local function questInfoFromDescriptor(action)
	return action and (action.questInfo or action.quest or action.q) or {}
end

local function questObject(questInfo, aiNk)
	local callback = aiNk and aiNk.cc
	return call(questInfo, "getObject", callback)
		or questInfo.object
		or questInfo.obj
		or questInfo.questObject
		or questInfo.targetObject
end

local function questRecord(questInfo, aiNk)
	local callback = aiNk and aiNk.cc
	return call(questInfo, "getQuest", callback)
		or questInfo.quest
		or questInfo
end

local function checkQuest(quest, hero)
	local result = call(quest, "checkQuest", hero)
	if result ~= nil then
		return result == true
	end
	if hero and hero.canCompleteQuest ~= nil then
		return hero.canCompleteQuest == true
	end
	if quest and quest.canComplete ~= nil then
		return quest.canComplete == true
	end
	return true
end

local function heroOwner(hero)
	return hero and (hero.owner or hero.tempOwner)
end

local function activeForPlayer(quest, owner)
	local active = quest and (quest.activeForPlayers or quest.activePlayers)
	if type(active) ~= "table" then
		return false
	end
	return active[owner] == true or active[tostring(owner)] == true
end

local function wasVisited(object, player)
	local result = call(object, "wasVisited", player)
	if result ~= nil then
		return result == true
	end
	return object and object.wasVisited == true
end

local function questTile(questInfo, aiNk)
	local object = questObject(questInfo, aiNk)
	return visitablePos(object)
end

function QuestAction.new(questInfo)
	return setmetatable({
		questInfo = questInfo or {}
	}, QuestAction)
end

function QuestAction.isQuestAction(action)
	if getmetatable(action) == QuestAction then
		return true
	end
	return normalizedActionName(action) == "questaction"
end

function QuestAction.fromDescriptor(action)
	if getmetatable(action) == QuestAction then
		return action
	end
	return QuestAction.new(questInfoFromDescriptor(action))
end

function QuestAction:canAct(aiNk, hero)
	local object = questObject(self.questInfo, aiNk)
	local quest = questRecord(self.questInfo, aiNk)
	if objectIsBorderGate(object) then
		return checkQuest(quest, hero)
	end

	local notActivated = not wasVisited(object, aiNk and aiNk.playerID)
		and not activeForPlayer(quest, heroOwner(hero))
	return notActivated or checkQuest(quest, hero)
end

function QuestAction:decompose(aiNk, _hero)
	return CompleteQuest.new(self.questInfo, aiNk and aiNk.cc)
end

function QuestAction:execute(aiGw, hero, fallbackTile)
	local tile = questTile(self.questInfo, aiGw) or fallbackTile
	if tile == nil then
		error("Quest special action is missing quest object tile", 2)
	end
	return aiGw:moveHeroToTile(tile, hero)
end

function QuestAction:toString()
	return "Complete Quest"
end

function QuestAction.decomposeAction(action, aiNk, hero)
	return QuestAction.fromDescriptor(action):decompose(aiNk, hero)
end

function QuestAction.executeAction(aiGw, hero, fallbackTile, action)
	return QuestAction.fromDescriptor(action):execute(aiGw, hero, fallbackTile)
end

function QuestAction.questTile(action, aiNk)
	return questTile(questInfoFromDescriptor(action), aiNk)
end

function QuestAction.questObjectID(action, aiNk)
	return objectID(questObject(questInfoFromDescriptor(action), aiNk))
end

return QuestAction
