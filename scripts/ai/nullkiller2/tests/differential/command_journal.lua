local BuildThis = require("Goals.BuildThis")
local HostCommands = require("Actions.HostCommands")
local RecruitHero = require("Goals.RecruitHero")

local host = HostCommands.new({})
local town = {
	id = 10,
	name = "Castle",
	buildings = {
		[5] = { name = "Tavern" }
	},
	availableHeroes = {
		{ id = 21, name = "Weak", totalStrength = 1 },
		{ id = 22, name = "Strong", totalStrength = 5 }
	}
}

BuildThis.new(5, town):accept(host)
RecruitHero.new(town):accept(host)
host:endTurn()

for index, command in ipairs(host:getJournal()) do
	if command.name == "buildBuilding" then
		print(string.format("%d:%s town=%s bid=%s", index, command.name, command.payload.town, command.payload.bid))
	elseif command.name == "recruitHero" then
		print(string.format("%d:%s town=%s hero=%s", index, command.name, command.payload.town, command.payload.hero))
	else
		print(string.format("%d:%s", index, command.name))
	end
end
