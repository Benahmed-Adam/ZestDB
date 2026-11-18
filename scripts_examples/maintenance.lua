-- Administration helpers built on keys / count / ttl / expire / persist.
--
-- Filters take the same modes as getby: sw (starts with), ct (contains),
-- ew (ends with), re (regex on the whole key). Each call is bounded by the
-- instruction limit: process large sets in batches with the max argument.
--
--   call inventory "user:" "order:" "session:"   -> "user:=120 order:=3400 session:=58"
--   call purge "sw" "tmp:" "1000"                -> number of keys deleted (call again until 0)
--   call expire_matching "sw" "cache:" "600"     -> number of keys given a 10 minute TTL
--   call rename_key "old:name" "new:name"        -> "renamed" (the TTL is kept)
--   call copy_key "template:invoice" "invoice:42"

local function limit(text)
    local n = math.tointeger(tonumber(text or "1000"))
    if n == nil or n <= 0 then
        error("max must be a positive number")
    end
    return n
end

zest.register_function("inventory", function(...)
    local parts = {}
    for _, prefix in ipairs({ ... }) do
        parts[#parts + 1] = prefix .. "=" .. zest.count("sw", prefix)
    end
    if #parts == 0 then
        return "total=" .. zest.count()
    end
    return table.concat(parts, " ")
end)

zest.register_function("purge", function(mode, pattern, max)
    local deleted = 0
    for _, key in ipairs(zest.keys(mode, pattern, limit(max))) do
        if zest.del(key) then
            deleted = deleted + 1
        end
    end
    zest.log(string.format("purge %s %s: %d keys deleted", mode, pattern, deleted))
    return deleted
end)

zest.register_function("expire_matching", function(mode, pattern, seconds, max)
    local ttl = math.tointeger(tonumber(seconds))
    if ttl == nil or ttl <= 0 then
        error("seconds must be a positive number")
    end
    local changed = 0
    for _, key in ipairs(zest.keys(mode, pattern, limit(max))) do
        if zest.expire(key, ttl) then
            changed = changed + 1
        end
    end
    return changed
end)

zest.register_function("persist_matching", function(mode, pattern, max)
    local changed = 0
    for _, key in ipairs(zest.keys(mode, pattern, limit(max))) do
        if zest.ttl(key) >= 0 and zest.persist(key) then
            changed = changed + 1
        end
    end
    return changed
end)

local function copy(from, to)
    local value = zest.get(from)
    if value == nil then
        error(from .. " does not exist")
    end
    local ttl = zest.ttl(from)
    zest.set(to, value, ttl > 0 and ttl or nil)
end

zest.register_function("copy_key", function(from, to)
    if zest.exists(to) then
        error(to .. " already exists")
    end
    copy(from, to)
    return "copied"
end)

zest.register_function("rename_key", function(from, to)
    if zest.exists(to) then
        error(to .. " already exists")
    end
    copy(from, to)
    zest.del(from)
    return "renamed"
end)
