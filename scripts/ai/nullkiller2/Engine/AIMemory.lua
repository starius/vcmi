-- Mirrors AI/Nullkiller2/Engine/AIMemory.{h,cpp} on object-id sets.

local AIMemory = {}
AIMemory.__index = AIMemory

local function objectID(value)
	if type(value) == "number" then
		return value
	end
	if type(value) == "table" then
		return value.id or value.objectID or value.objectId or value.num or value[1]
	end
	return value
end

local function objectType(value)
	if type(value) ~= "table" then
		return nil
	end
	return value.ID or value.objectType or value.type or value.typeName
end

local function visitMode(value)
	if type(value) ~= "table" then
		return nil
	end
	return value.visitMode
		or value.rewardable and value.rewardable.visitMode
		or value.configuration and value.configuration.visitMode
		or value.configuration and value.configuration.visit and value.configuration.visit.mode
end

local function ensureSet(state, key)
	state[key] = state[key] or {}
	return state[key]
end

local function setValue(set, id, value)
	if id ~= nil then
		set[tostring(id)] = value
	end
end

local function hasValue(set, id)
	return id ~= nil and set[tostring(id)] == true
end

local function sortedIDs(set)
	local result = {}
	for id, enabled in pairs(set or {}) do
		if enabled then
			table.insert(result, tonumber(id) or id)
		end
	end
	table.sort(result, function(lhs, rhs)
		if type(lhs) == type(rhs) then
			return lhs < rhs
		end
		return tostring(lhs) < tostring(rhs)
	end)
	return result
end

local function callbackObject(callback, id)
	if not callback then
		return nil
	end
	if type(callback.getObjInstance) == "function" then
		local result = callback:getObjInstance(id)
		if result ~= nil then
			return result
		end
	end
	if type(callback.getObj) == "function" then
		return callback:getObj(id, false)
	end
	return nil
end

function AIMemory.new(state)
	state = state or {}
	ensureSet(state, "visitableObjs")
	ensureSet(state, "alreadyVisited")
	state.knownTeleportChannels = state.knownTeleportChannels or {}
	state.knownSubterraneanGates = state.knownSubterraneanGates or {}
	return setmetatable({
		state = state,
		visitableObjs = state.visitableObjs,
		alreadyVisited = state.alreadyVisited,
		knownTeleportChannels = state.knownTeleportChannels,
		knownSubterraneanGates = state.knownSubterraneanGates
	}, AIMemory)
end

function AIMemory:export()
	return self.state
end

function AIMemory:removeFromMemory(obj)
	local id = objectID(obj)
	setValue(self.visitableObjs, id, nil)
	setValue(self.alreadyVisited, id, nil)

	local boat = type(obj) == "table" and (obj.boat or obj.currentBoat) or nil
	if boat then
		local boatID = objectID(boat)
		setValue(self.visitableObjs, boatID, nil)
		setValue(self.alreadyVisited, boatID, nil)
	end
end

function AIMemory:addVisitableObject(obj)
	local id = objectID(obj)
	setValue(self.visitableObjs, id, true)
end

function AIMemory:markObjectVisited(obj)
	local id = objectID(obj)
	if id == nil then
		return
	end

	local mode = visitMode(obj)
	if mode == "VISIT_HERO" or mode == "hero" or mode == "VISIT_BONUS" or mode == "bonus" then
		return
	end

	local kind = objectType(obj)
	if obj.isMonster or kind == "MONSTER" or kind == "monster" then
		return
	end

	setValue(self.alreadyVisited, id, true)
end

function AIMemory:markObjectUnvisited(obj)
	setValue(self.alreadyVisited, objectID(obj), nil)
end

function AIMemory:wasVisited(obj)
	return hasValue(self.alreadyVisited, objectID(obj))
end

function AIMemory:removeInvisibleOrDeletedObjects(callback)
	for _, id in ipairs(sortedIDs(self.visitableObjs)) do
		if callbackObject(callback, id) == nil then
			setValue(self.visitableObjs, id, nil)
			setValue(self.alreadyVisited, id, nil)
		end
	end
end

function AIMemory:visitableIdsToObjsVector(callback)
	local result = {}
	for _, id in ipairs(sortedIDs(self.visitableObjs)) do
		local obj = callbackObject(callback, id)
		if obj ~= nil then
			table.insert(result, obj)
		end
	end
	return result
end

function AIMemory:visitableIdsToObjsSet(callback)
	local result = {}
	for _, obj in ipairs(self:visitableIdsToObjsVector(callback)) do
		result[obj] = true
	end
	return result
end

return AIMemory
