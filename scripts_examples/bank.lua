-- Procedures: several operations in one round trip, with checks and rollback.
--
-- Balances are integers in cents under balance:<account>. Each step uses
-- zest.incr, which is atomic per key, and a failed transfer undoes what it did
-- (compensation). A procedure is not a transaction: another client can see the
-- intermediate state between two steps, and a crash between them leaves it.
--
--   call deposit "alice" "10000"            -> "10000"
--   call transfer "alice" "bob" "2500"      -> "1" (the ledger entry id)
--   call balance "alice"                    -> "7500"
--   call transfer "alice" "bob" "999999"    -> 600, nothing changed
--   s balance:alice "-5"                    -> 603, a balance is never negative

local BALANCE = "balance:"

local function cents(text)
    local n = math.tointeger(tonumber(text))
    if n == nil or n <= 0 then
        error("the amount must be a positive whole number of cents")
    end
    return n
end

-- Guards direct writes (s / setnx / cas). incr is not covered: the procedures check it.
zest.register_trigger{
    name = "balance_not_negative", phase = "before", event = "set", mode = "sw", pattern = BALANCE,
    fn = function(key, value)
        local n = math.tointeger(tonumber(value))
        if n == nil or n < 0 then
            error(key .. " must be a whole number >= 0")
        end
        return tostring(n)
    end,
}

zest.register_function("deposit", function(account, amount)
    return zest.incr(BALANCE .. account, cents(amount))
end)

zest.register_function("withdraw", function(account, amount)
    local n = cents(amount)
    local left = zest.incr(BALANCE .. account, -n)
    if left < 0 then
        zest.incr(BALANCE .. account, n)
        error("insufficient funds")
    end
    return left
end)

zest.register_function("transfer", function(from, to, amount)
    local n = cents(amount)
    if from == to then
        error("cannot transfer to the same account")
    end
    if zest.incr(BALANCE .. from, -n) < 0 then
        zest.incr(BALANCE .. from, n)
        error("insufficient funds")
    end
    zest.incr(BALANCE .. to, n)

    local id = zest.incr("ledger:next")
    zest.set("ledger:" .. id, string.format("%s>%s:%d", from, to, n))
    return id
end)

zest.register_function("balance", function(account)
    return zest.get(BALANCE .. account) or "0"
end)
