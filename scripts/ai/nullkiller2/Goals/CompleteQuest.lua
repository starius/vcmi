-- Mirrors AI/Nullkiller2/Goals/CompleteQuest.{h,cpp}: quest mission decomposition.

local AbstractGoal = require("Goals.AbstractGoal")
local CaptureObjectsBehavior = require("Behaviors.CaptureObjectsBehavior")
local CGoal = require("Goals.CGoal")

local CompleteQuest = CGoal.derive("CompleteQuest", AbstractGoal.EGoals.COMPLETE_QUEST)

local Obj = {
	ARTIFACT = 5,
	BORDERGUARD = 9,
	KEYMASTER = 10,
	PRISON = 62,
	BORDER_GATE = 212
}

local function call(object, name, ...)
	if object and type(object[name]) == "function" then
		local ok, result = pcall(object[name], object, ...)
		if ok then
			return result
		end
	end
	return nil
end

local function objectID(value)
	return CGoal.objectID(value)
end

local function objectType(object)
	return object and (object.ID or object.typeID or object.idType or object.type)
end

local function objectSubType(object)
	return object and (object.subID or object.subId or object.subType)
end

local function typeIs(object, numericID, name)
	local value = objectType(object)
	return value == numericID or value == name
end

local function artifactID(value)
	if type(value) == "table" then
		return value.id or value.num or value.artifactID or value.artifactId or value[1]
	end
	return value
end

local function visitablePos(object)
	return call(object, "visitablePos") or object and (object.visitablePos or object.tile)
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

local function questMission(quest)
	return quest and (quest.mission or quest) or {}
end

local function hasItems(values)
	return type(values) == "table" and next(values) ~= nil
end

local function resourcesNonZero(resources)
	local result = call(resources, "nonZero")
	if result ~= nil then
		return result == true
	end

	for _, value in pairs(resources or {}) do
		if type(value) == "number" and value ~= 0 then
			return true
		end
	end
	return false
end

local function primaryStatMission(primary)
	for _, value in pairs(primary or {}) do
		if value and value ~= 0 then
			return true
		end
	end
	return false
end

local function killTarget(quest)
	local value = quest and (quest.killTarget or quest.killTargetObject)
	if value == nil or value == -1 or value == "NONE" then
		return nil
	end
	return value
end

local function pathList(self, aiNk)
	local object = questObject(self.q, aiNk)
	local paths = call(aiNk and aiNk.pathfinder, "getPathInfo", visitablePos(object))
	if paths ~= nil then
		return paths
	end
	return object and object.paths or self.q.paths or {}
end

local function checkQuest(quest, path)
	if path.questAllowed ~= nil then
		return path.questAllowed == true
	end
	if path.canCompleteQuest ~= nil then
		return path.canCompleteQuest == true
	end
	if path.targetHero and path.targetHero.canCompleteQuest ~= nil then
		return path.targetHero.canCompleteQuest == true
	end
	local result = call(quest, "checkQuest", path.targetHero)
	if result ~= nil then
		return result == true
	end
	return true
end

local function checkMissionArmy(path)
	if path.missionArmyAllowed ~= nil then
		return path.missionArmyAllowed == true
	end
	if path.heroArmy and path.heroArmy.missionArmyAllowed ~= nil then
		return path.heroArmy.missionArmyAllowed == true
	end
	return true
end

local function isObjectPassable(aiNk, object)
	local result = call(aiNk, "isObjectPassable", object)
	if result ~= nil then
		return result == true
	end
	return object and object.passable == true
end

local function objectByID(aiNk, id)
	if type(id) == "table" and objectType(id) ~= nil then
		return id
	end
	local result = call(aiNk and aiNk.cc, "getObj", objectID(id))
	if result ~= nil then
		return result
	end
	for _, object in ipairs(aiNk and aiNk.objects or {}) do
		if objectID(object) == objectID(id) then
			return object
		end
	end
	return nil
end

local function relations(aiNk, object)
	return object and (object.relationsName or object.relations or object.relation)
		or call(aiNk and aiNk.cc, "getPlayerRelations", aiNk and aiNk.playerID, object and object.tempOwner)
		or "NEUTRAL"
end

function CompleteQuest:init(quest, cc)
	self.q = quest or {}
	self.cc = cc
	self.objid = objectID(questObject(self.q, { cc = cc })) or -1
end

function CompleteQuest:isKeyMaster(aiNk)
	local object = questObject(self.q, aiNk)
	return typeIs(object, Obj.BORDER_GATE, "BORDER_GATE")
		or typeIs(object, Obj.BORDERGUARD, "BORDERGUARD")
end

function CompleteQuest:toString()
	return "Complete quest " .. self:questToString()
end

function CompleteQuest:equalsTyped(other)
	if self:isKeyMaster() then
		return other:isKeyMaster()
			and objectSubType(questObject(self.q)) == objectSubType(questObject(other.q))
	elseif other:isKeyMaster() then
		return false
	end

	local lhs = questRecord(self.q)
	local rhs = questRecord(other.q)
	return lhs == rhs
		or objectID(lhs) ~= nil and objectID(lhs) == objectID(rhs)
		or self.objid ~= -1 and self.objid == other.objid
end

function CompleteQuest:hasHash()
	return true
end

function CompleteQuest:getHash()
	if self:isKeyMaster() then
		return objectSubType(questObject(self.q)) or -1
	end
	return objectID(questObject(self.q)) or self.objid
end

function CompleteQuest:questToString()
	local object = questObject(self.q)
	if self:isKeyMaster() then
		local color = object and (object.colorName or object.tentColor or object.color or objectSubType(object)) or "unknown"
		return "find " .. tostring(color) .. " keymaster tent"
	end

	local quest = questRecord(self.q)
	local name = quest and (quest.questName or quest.missionName)
	if name == "NONE" or quest and quest.inactive == true then
		return "inactive quest"
	end

	return quest and (quest.rolloverText or quest.text or quest.description or quest.questName)
		or "quest"
end

function CompleteQuest:tryCompleteQuest(aiNk)
	local object = questObject(self.q, aiNk)
	local quest = questRecord(self.q, aiNk)
	local paths = {}

	for _, path in ipairs(pathList(self, aiNk)) do
		if checkQuest(quest, path) then
			table.insert(paths, path)
		end
	end

	return CaptureObjectsBehavior.getVisitGoals(paths, aiNk, object)
end

function CompleteQuest:missionArt(aiNk)
	local solutions = self:tryCompleteQuest(aiNk)
	if #solutions > 0 then
		return solutions
	end

	local result = {}
	for _, artifact in ipairs(questMission(questRecord(self.q, aiNk)).artifacts or {}) do
		table.insert(result, CaptureObjectsBehavior.new():ofType(Obj.ARTIFACT, artifactID(artifact)))
	end
	return result
end

function CompleteQuest:missionHero(aiNk)
	local solutions = self:tryCompleteQuest(aiNk)
	if #solutions == 0 then
		table.insert(solutions, CaptureObjectsBehavior.new():ofType(Obj.PRISON))
	end
	return solutions
end

function CompleteQuest:missionArmy(aiNk)
	local object = questObject(self.q, aiNk)
	local paths = {}
	for _, path in ipairs(pathList(self, aiNk)) do
		if checkMissionArmy(path) then
			table.insert(paths, path)
		end
	end
	return CaptureObjectsBehavior.getVisitGoals(paths, aiNk, object)
end

function CompleteQuest:missionIncreasePrimaryStat(aiNk)
	return self:tryCompleteQuest(aiNk)
end

function CompleteQuest:missionLevel(aiNk)
	return self:tryCompleteQuest(aiNk)
end

function CompleteQuest:missionKeymaster(aiNk)
	local object = questObject(self.q, aiNk)
	if isObjectPassable(aiNk, object) then
		return CaptureObjectsBehavior.new(object):decompose(aiNk)
	end
	return CaptureObjectsBehavior.new():ofType(Obj.KEYMASTER, objectSubType(object)):decompose(aiNk)
end

function CompleteQuest:missionResources(aiNk)
	return self:tryCompleteQuest(aiNk)
end

function CompleteQuest:missionDestroyObj(aiNk)
	local object = questObject(self.q, aiNk)
	local target = objectByID(aiNk, killTarget(questRecord(self.q, aiNk)))
	if not target then
		return CaptureObjectsBehavior.new(object):decompose(aiNk)
	end
	if relations(aiNk, target) == "ENEMIES" then
		return CaptureObjectsBehavior.new(target):decompose(aiNk)
	end
	return {}
end

function CompleteQuest:decompose(aiNk)
	if self:isKeyMaster(aiNk) then
		return self:missionKeymaster(aiNk)
	end

	local quest = questRecord(self.q, aiNk)
	local mission = questMission(quest)

	if hasItems(mission.artifacts) then
		return self:missionArt(aiNk)
	end
	if hasItems(mission.heroes) then
		return self:missionHero(aiNk)
	end
	if hasItems(mission.creatures) then
		return self:missionArmy(aiNk)
	end
	if resourcesNonZero(mission.resources) then
		return self:missionResources(aiNk)
	end
	if killTarget(quest) ~= nil then
		return self:missionDestroyObj(aiNk)
	end
	if primaryStatMission(mission.primary) then
		return self:missionIncreasePrimaryStat(aiNk)
	end
	if (mission.heroLevel or quest.heroLevel or 0) > 0 then
		return self:missionLevel(aiNk)
	end

	return {}
end

CompleteQuest.Obj = Obj

return CompleteQuest
