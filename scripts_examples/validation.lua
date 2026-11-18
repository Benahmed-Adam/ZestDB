-- Validation and normalisation with "before" triggers.
--
-- A before-set trigger receives (key, value) and returns the value to store:
-- return it unchanged, return a cleaned version, or raise an error to reject the
-- write (code 603 TriggerRejected, nothing is written).
-- A before-del trigger receives (key) and can only accept or reject.
--
-- They run for s, setex, setnx, cas, setby and for zest.set from other scripts.
-- incr / decr only run "after" triggers: their value is computed under the shard lock.

local function trim(s)
    return (s:gsub("^%s+", ""):gsub("%s+$", ""))
end

-- email:<id> -> stored trimmed and lowercased, refused if it does not look like an address.
--   s email:42 "  Ada@Example.COM "   -> stored as "ada@example.com"
--   s email:42 "not an email"         -> 603
zest.register_trigger{
    name = "normalize_email", phase = "before", event = "set", mode = "sw", pattern = "email:",
    fn = function(key, value)
        local email = trim(value):lower()
        if not email:match("^[%w%._%+%-]+@[%w%-]+%.[%w%.%-]+$") then
            error("invalid email for " .. key)
        end
        return email
    end,
}

-- stock:<sku> -> a whole number, never negative ("12.0" is stored as "12").
zest.register_trigger{
    name = "stock_is_natural", phase = "before", event = "set", mode = "sw", pattern = "stock:",
    fn = function(key, value)
        local n = math.tointeger(tonumber(value))
        if n == nil or n < 0 then
            error("stock must be a whole number >= 0, got '" .. value .. "'")
        end
        return tostring(n)
    end,
}

-- note:<id> -> at most 280 characters (counted as UTF-8 characters, not bytes).
zest.register_trigger{
    name = "note_max_length", phase = "before", event = "set", mode = "sw", pattern = "note:",
    fn = function(key, value)
        local length = utf8.len(value)
        if length == nil then
            error("a note must be valid UTF-8")
        end
        if length > 280 then
            error(string.format("a note is limited to 280 characters, got %d", length))
        end
        return value
    end,
}

-- Permissions per key: config:* can only be written by an admin.
-- zest.caller() is the client that sent the command ({ user = "bob", role = "write" }),
-- or nil for writes the server makes itself.
--   s config:app "v2"   as bob (write)  -> 603
zest.register_trigger{
    name = "config_admin_only", phase = "before", event = "set", mode = "sw", pattern = "config:",
    fn = function(key, value)
        local caller = zest.caller()
        if caller ~= nil and caller.role ~= "admin" then
            error(key .. " can only be changed by an admin")
        end
        return value
    end,
}

-- A regex filter (mode "re" matches the whole key): these keys cannot be deleted.
--   d config:app   -> 603
zest.register_trigger{
    name = "protect_config", phase = "before", event = "del", mode = "re", pattern = "config:(app|billing)",
    fn = function(key)
        error(key .. " is protected")
    end,
}
