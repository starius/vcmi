local ResourceTrader = require("Engine.ResourceTrader")

local function market(offers)
	return {
		id = 700,
		offers = offers
	}
end

local trades = {}
local cc = {
	trade = function(_, marketID, mode, sold, bought, amount)
		table.insert(trades, {
			marketID = marketID,
			mode = mode,
			sold = sold,
			bought = bought,
			amount = amount
		})
	end
}

local ok = ResourceTrader.tradeHelper(
	0.5,
	market({
		["0:6"] = { given = 1, received = 100 },
		["0:2"] = { given = 5, received = 1 }
	}),
	{ [6] = 1000 },
	{ [6] = 0 },
	{ [0] = 20 },
	{ goldPressureOverMax = false },
	cc)

assert(ok == true)
assert(#trades == 1)
assert(trades[1].marketID == 700)
assert(trades[1].mode == ResourceTrader.RESOURCE_RESOURCE)
assert(trades[1].sold == 0)
assert(trades[1].bought == 6)
assert(trades[1].amount == 10)

trades = {}
ok = ResourceTrader.tradeHelper(
	0.5,
	market({
		["6:2"] = { given = 100, received = 1 }
	}),
	{ [2] = 2 },
	{ [6] = 10 },
	{ [6] = 1000 },
	{ goldPressureOverMax = true },
	cc)

assert(ok == false)
assert(#trades == 0)

trades = {}
ok = ResourceTrader.tradeHelper(
	0.5,
	market({
		["6:2"] = { given = 100, received = 1 }
	}),
	{ [2] = 2 },
	{ [6] = 10 },
	{ [6] = 1000 },
	{ goldPressureOverMax = false },
	cc)

assert(ok == true)
assert(trades[1].sold == 6)
assert(trades[1].bought == 2)
assert(trades[1].amount == 200)

trades = {}
ok = ResourceTrader.tradeHelper(
	0.5,
	market({
		["0:6"] = { given = 50, received = 100 }
	}),
	{ [6] = 100 },
	{ [6] = 0 },
	{ [0] = 20 },
	{},
	cc)

assert(ok == false)
assert(#trades == 0)
