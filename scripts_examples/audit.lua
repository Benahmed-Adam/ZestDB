-- Audit trail and statistics with "after" triggers.
--
-- An after trigger runs once the write is stored and logged, so it sees the
-- final value (after any before trigger). It cannot cancel the write: if it
-- fails, the command returns an error but the original write stays.
-- Writes made by triggers are logged in the WAL like any other write, and the
-- triggers are not run again when the WAL is replayed after a crash.
--
--   s order:1001 "2 pizzas"   -> also writes stats:orders:written, stats:orders:last, audit:order:<n>
--   d order:1001              -> increments stats:orders:deleted and logs a warning
--   call order_stats          -> "written=1 deleted=1 live=0"
--   call order_history "5"    -> the 5 most recent audit entries, newest first

local WEEK = 7 * 24 * 3600

zest.register_trigger{
    name = "order_written", phase = "after", event = "set", mode = "sw", pattern = "order:",
    fn = function(key, value)
        local n = zest.incr("stats:orders:written")
        zest.set("stats:orders:last", key)
        -- who made the change: the user name, or the role for a client without one
        local caller = zest.caller()
        local who = caller and (caller.user or caller.role) or "server"
        -- one entry per write, kept a week: the TTL cleans the trail without a job
        zest.set("audit:order:" .. n, key .. " = " .. value .. " by " .. who, WEEK)
    end,
}

zest.register_trigger{
    name = "order_deleted", phase = "after", event = "del", mode = "sw", pattern = "order:",
    fn = function(key)
        zest.incr("stats:orders:deleted")
        zest.log("order deleted: " .. key, "warning")
    end,
}

zest.register_function("order_stats", function()
    local written = tonumber(zest.get("stats:orders:written") or "0")
    local deleted = tonumber(zest.get("stats:orders:deleted") or "0")
    return string.format("written=%d deleted=%d live=%d", written, deleted, zest.count("sw", "order:"))
end)

zest.register_function("order_history", function(limit)
    local last = tonumber(zest.get("stats:orders:written") or "0")
    local wanted = math.tointeger(tonumber(limit or "10")) or 10
    local lines = {}
    -- audit keys are numbered, so walk the numbers down instead of sorting keys ("10" < "9" as text)
    for n = last, math.max(1, last - wanted + 1), -1 do
        local entry = zest.get("audit:order:" .. n)
        if entry then
            lines[#lines + 1] = n .. ": " .. entry
        end
    end
    return table.concat(lines, "\n")
end)
