-- Mirrors AI/Nullkiller2/Engine/ResourceTrader.{h,cpp}: ResourceTrader.

local ResourceTrader = {}

ResourceTrader.ARMY_GOLD_RATIO_PER_MAKE_TURN_PASS = 0.1
ResourceTrader.EXPENDABLE_BULK_RATIO = 0.5
ResourceTrader.EMPTY = -1
ResourceTrader.GOLD = 6
ResourceTrader.RESOURCE_RESOURCE = "RESOURCE_RESOURCE"
ResourceTrader.RESOURCE_COUNT = 7

local function getResource(resources, resourceID)
	if not resources then
		return 0
	end
	if resources[0] == nil and resources[7] ~= nil then
		return resources[resourceID + 1] or 0
	end
	return resources[resourceID] or 0
end

local function isEmpty(resources)
	for resourceID = 0, ResourceTrader.RESOURCE_COUNT - 1 do
		if getResource(resources, resourceID) ~= 0 then
			return false
		end
	end
	return true
end

local function callMethod(object, methodName, ...)
	if object and type(object[methodName]) == "function" then
		return object[methodName](object, ...)
	end
	return nil
end

local function getOffer(market, fromResource, toResource)
	if market and type(market.getOffer) == "function" then
		local givenPerUnit, receivedPerUnit = market:getOffer(fromResource, toResource, ResourceTrader.RESOURCE_RESOURCE)
		return givenPerUnit or 0, receivedPerUnit or 0
	end

	local key = tostring(fromResource) .. ":" .. tostring(toResource)
	local offer = market and market.offers and market.offers[key]
	if offer then
		return offer.givenPerUnit or offer.given or 0, offer.receivedPerUnit or offer.received or 0
	end

	return 0, 0
end

local function marketObjectID(market)
	if not market then
		return nil
	end
	if type(market.getObjInstanceID) == "function" then
		return market:getObjInstanceID()
	end
	return market.id or market.objectID or market.objectId
end

local function isGoldPressureOverMax(buildAnalyzer)
	if buildAnalyzer and type(buildAnalyzer.isGoldPressureOverMax) == "function" then
		return buildAnalyzer:isGoldPressureOverMax()
	end
	return buildAnalyzer and buildAnalyzer.goldPressureOverMax or false
end

function ResourceTrader.trade(buildAnalyzer, cc, freeResources)
	local haveTraded = false
	local marketID = nil

	for _, town in ipairs(callMethod(cc, "getTownsInfo") or {}) do
		local hasMarketplace = false
		if type(town.hasBuiltResourceMarketplace) == "function" then
			hasMarketplace = town:hasBuiltResourceMarketplace()
		else
			hasMarketplace = town.hasBuiltResourceMarketplace == true
		end

		if hasMarketplace then
			marketID = town.id
			break
		end
	end

	if not marketID then
		return false
	end

	local market = callMethod(cc, "getObj", marketID, false)
	if not market then
		return false
	end

	local shouldTryToTrade = true
	while shouldTryToTrade do
		shouldTryToTrade = false
		callMethod(buildAnalyzer, "update")

		local missingNow = callMethod(buildAnalyzer, "getMissingResourcesNow", ResourceTrader.ARMY_GOLD_RATIO_PER_MAKE_TURN_PASS) or {}
		if isEmpty(missingNow) then
			break
		end

		local income = callMethod(buildAnalyzer, "getDailyIncome") or {}
		local freeAfterMissingTotal = callMethod(
			buildAnalyzer,
			"getFreeResourcesAfterMissingTotal",
			ResourceTrader.ARMY_GOLD_RATIO_PER_MAKE_TURN_PASS) or freeResources or {}

		if ResourceTrader.tradeHelper(
			ResourceTrader.EXPENDABLE_BULK_RATIO,
			market,
			missingNow,
			income,
			freeAfterMissingTotal,
			buildAnalyzer,
			cc) then
			haveTraded = true
			shouldTryToTrade = true
		end
	end

	return haveTraded
end

function ResourceTrader.tradeHelper(expendableBulkRatio, market, missingNow, income, freeAfterMissingTotal, buildAnalyzer, cc)
	local mostWanted = ResourceTrader.EMPTY
	local mostWantedScoreNeg = math.huge
	local mostExpendable = ResourceTrader.EMPTY
	local mostExpendableAmountPos = 0

	for resourceID = 0, ResourceTrader.RESOURCE_COUNT - 1 do
		if getResource(missingNow, resourceID) ~= 0 then
			local score = getResource(income, resourceID) - getResource(missingNow, resourceID)
			if resourceID ~= ResourceTrader.GOLD then
				local _, receivedPerUnit = getOffer(market, resourceID, ResourceTrader.GOLD)
				score = score * receivedPerUnit
			end

			if score < mostWantedScoreNeg then
				mostWanted = resourceID
				mostWantedScoreNeg = score
			end
		end
	end

	for resourceID = 0, ResourceTrader.RESOURCE_COUNT - 1 do
		local amountToSell = getResource(freeAfterMissingTotal, resourceID)
		if amountToSell ~= 0 then
			local okToSell = false
			if resourceID == ResourceTrader.GOLD then
				if getResource(income, ResourceTrader.GOLD) > 0 and not isGoldPressureOverMax(buildAnalyzer) then
					okToSell = true
				end
			else
				okToSell = true
			end

			if okToSell and amountToSell > mostExpendableAmountPos then
				mostExpendable = resourceID
				mostExpendableAmountPos = amountToSell
			end
		end
	end

	if mostExpendable == mostWanted or mostWanted == ResourceTrader.EMPTY or mostExpendable == ResourceTrader.EMPTY then
		return false
	end

	local givenPerUnit, receivedPerUnit = getOffer(market, mostExpendable, mostWanted)
	if givenPerUnit == 0 or receivedPerUnit == 0 then
		return false
	end

	if givenPerUnit > mostExpendableAmountPos then
		return false
	end

	local multiplier = math.min(
		math.floor(mostExpendableAmountPos * expendableBulkRatio / givenPerUnit),
		math.floor(getResource(missingNow, mostWanted) / receivedPerUnit))

	if multiplier == 0 then
		multiplier = 1
	end

	local givenMultiplied = givenPerUnit * multiplier
	if givenMultiplied > getResource(freeAfterMissingTotal, mostExpendable) then
		return false
	end

	if cc and type(cc.trade) == "function" then
		cc:trade(marketObjectID(market), ResourceTrader.RESOURCE_RESOURCE, mostExpendable, mostWanted, givenMultiplied)
	end

	return true
end

return ResourceTrader
