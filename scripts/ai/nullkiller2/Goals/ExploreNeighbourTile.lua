-- Mirrors AI/Nullkiller2/Goals/ExploreNeighbourTile.{h,cpp}: neighbour exploration policy.

local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")

local ExploreNeighbourTile = CGoal.derive("ExploreNeighbourTile", AbstractGoal.EGoals.EXPLORE_NEIGHBOUR_TILE, { elementar = true })

function ExploreNeighbourTile.evaluateTileScore(tilesDiscovered, movementCost)
	return tilesDiscovered / math.max(0.1, movementCost)
end

function ExploreNeighbourTile.evaluateNeighbourExplorationCandidate(candidate)
	local result = {
		accepted = false,
		value = 0.0
	}

	if not candidate.sameDay
		or not candidate.accessible
		or not candidate.safe
		or candidate.tilesDiscovered <= 0 then
		return result
	end

	result.accepted = true
	result.value = candidate.tilesDiscovered * candidate.tilesDiscovered / math.max(0.1, candidate.movementCost)
	return result
end

function ExploreNeighbourTile:init(hero, amount)
	self.tilesToExplore = amount
	self:sethero(hero)
end

function ExploreNeighbourTile:equalsTyped(_other)
	return false
end

function ExploreNeighbourTile.findTarget(hero, aiNk)
	local result = nil
	local candidates = {}

	if aiNk and type(aiNk.getNeighbourExplorationCandidates) == "function" then
		candidates = aiNk:getNeighbourExplorationCandidates(hero)
	elseif hero and hero.neighbourExplorationCandidates then
		candidates = hero.neighbourExplorationCandidates
	end

	for _, candidate in ipairs(candidates or {}) do
		local evaluation = ExploreNeighbourTile.evaluateNeighbourExplorationCandidate(candidate)
		if evaluation.accepted then
			local target = {
				tile = candidate.tile,
				tilesDiscovered = candidate.tilesDiscovered,
				movementCost = candidate.movementCost,
				value = evaluation.value
			}

			if not result or target.value > result.value then
				result = target
			end
		end
	end

	return result
end

function ExploreNeighbourTile:accept(aiGw)
	for _ = 1, self.tilesToExplore do
		if self.hero and self.hero.movementPointsRemaining ~= nil and self.hero.movementPointsRemaining <= 0 then
			return nil
		end

		local target = ExploreNeighbourTile.findTarget(self.hero, aiGw)
		if not target then
			return nil
		end

		if aiGw and type(aiGw.moveHeroToTile) == "function" then
			if not aiGw:moveHeroToTile(target.tile, self.hero) then
				return nil
			end
		else
			return {
				action = "moveHeroToTile",
				hero = self.hero,
				tile = target.tile
			}
		end
	end

	return nil
end

function ExploreNeighbourTile:toString()
	return "Explore neighbour tiles by " .. AbstractGoal._helpers.translatedName(self.hero, tostring(CGoal.objectID(self.hero)))
end

return ExploreNeighbourTile
