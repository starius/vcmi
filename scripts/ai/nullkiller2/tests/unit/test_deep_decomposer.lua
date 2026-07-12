local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")
local Composition = require("Goals.Composition")
local DeepDecomposer = require("Engine.DeepDecomposer")

local Leaf = CGoal.derive("Leaf", AbstractGoal.EGoals.BUY_ARMY, { elementar = true })

function Leaf:init(name, objectID)
	self.name = name
	self.objid = objectID or -1
	self.priority = 1
end

function Leaf:equalsTyped(other)
	return self.name == other.name
end

function Leaf:toString()
	return self.name
end

local Branch = CGoal.derive("Branch", AbstractGoal.EGoals.CAPTURE_OBJECTS)

function Branch:init(name, children)
	self.name = name
	self.children = children or {}
end

function Branch:equalsTyped(other)
	return self.name == other.name
end

function Branch:toString()
	return self.name
end

function Branch:decompose(_aiNk)
	return self.children
end

function Branch:hasHash()
	return self.hash ~= nil
end

function Branch:getHash()
	return self.hash or 0
end

local root = Branch.new("root")
local middle = Branch.new("middle")
local leaf = Leaf.new("leaf", 90)
root.children = { middle }
middle.children = { leaf }

local decomposer = DeepDecomposer.new({})
local results = {}
decomposer:decompose(results, root, 3)
assert(#results == 1)
assert(results[1].goalType == AbstractGoal.EGoals.COMPOSITION)
assert(results[1]:isElementar() == true)
assert(results[1]:isObjectAffected(90) == true)

local flattened = results[1]:decompose({})
assert(#flattened == 3)
assert(flattened[1] == root)
assert(flattened[2] == middle)
assert(flattened[3] == leaf)
assert(results[1]:toString() == "Composition[root, ] [middle, ] [leaf => ] ")

local directResults = {}
DeepDecomposer.new({}):decompose(directResults, Branch.new("direct", { Leaf.new("direct-leaf", 91) }), 2)
assert(#directResults == 1)
assert(directResults[1]:toString() == "direct-leaf")

local loop = Branch.new("loop")
loop.children = { loop }
local loopResults = {}
DeepDecomposer.new({}):decompose(loopResults, loop, 3)
assert(#loopResults == 0)

local composition = Composition.new()
composition:addNext(root):addNext(middle):addNext(leaf)
assert(#composition:getAffectedObjects() == 1)
assert(composition:getAffectedObjects()[1] == 90)
