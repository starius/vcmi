local AIMemory = require("Engine.AIMemory")

local state = {}
local memory = AIMemory.new(state)

memory:addVisitableObject({ id = 10 })
memory:addVisitableObject({ id = 11 })
memory:markObjectVisited({ id = 10 })
memory:markObjectVisited({ id = 12, ID = "MONSTER" })
memory:markObjectVisited({ id = 13, visitMode = "VISIT_HERO" })

assert(memory:wasVisited({ id = 10 }) == true)
assert(memory:wasVisited({ id = 12 }) == false)
assert(memory:wasVisited({ id = 13 }) == false)
assert(state.visitableObjs["10"] == true)
assert(state.visitableObjs["11"] == true)
assert(state.alreadyVisited["10"] == true)

local callback = {
	objects = {
		[10] = { id = 10, name = "Chest" }
	},
	getObj = function(self, id)
		return self.objects[id]
	end
}

memory:removeInvisibleOrDeletedObjects(callback)
assert(state.visitableObjs["10"] == true)
assert(state.visitableObjs["11"] == nil)
assert(state.alreadyVisited["10"] == true)

local objects = memory:visitableIdsToObjsVector(callback)
assert(#objects == 1)
assert(objects[1].id == 10)

memory:markObjectUnvisited({ id = 10 })
assert(memory:wasVisited({ id = 10 }) == false)

memory:removeFromMemory({ id = 10 })
assert(state.visitableObjs["10"] == nil)
