-- Mirrors AI/Nullkiller2/Goals/DigAtTile.{h,cpp}: DigAtTile.

local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")

local DigAtTile = CGoal.derive("DigAtTile", AbstractGoal.EGoals.DIG_AT_TILE)

local function sameTile(lhs, rhs)
	lhs = lhs or {}
	rhs = rhs or {}
	return lhs.x == rhs.x and lhs.y == rhs.y and lhs.z == rhs.z
end

function DigAtTile:init(Tile)
	if Tile ~= nil then
		self.tile = Tile
	end
end

function DigAtTile:equalsTyped(other)
	return other.hero == self.hero and sameTile(other.tile, self.tile)
end

return DigAtTile
