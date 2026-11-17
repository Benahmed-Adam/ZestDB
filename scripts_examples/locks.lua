-- A lock shared by several workers, with an expiry so a crashed worker
-- cannot hold it forever.
--
--   call lock_acquire "nightly-report" "worker-1" "300"   -> "acquired"
--   call lock_acquire "nightly-report" "worker-2" "300"   -> "busy:worker-1"
--   call lock_release "nightly-report" "worker-2"         -> "not owner"
--   call lock_release "nightly-report" "worker-1"         -> "released"
--
-- Limits: setnx then expire are two steps, so a crash between them leaves a
-- lock without expiry (lock_release or `d lock:<name>` frees it). Release reads
-- then deletes: if the lock expires and another worker takes it in between, the
-- delete removes the new lock. Keep the TTL well above the work duration.

local PREFIX = "lock:"

zest.register_function("lock_acquire", function(name, owner, ttl)
    local seconds = math.tointeger(tonumber(ttl or "60"))
    if seconds == nil or seconds <= 0 then
        error("the ttl must be a positive number of seconds")
    end
    local key = PREFIX .. name
    if zest.setnx(key, owner) then
        zest.expire(key, seconds)
        return "acquired"
    end
    return "busy:" .. (zest.get(key) or "?")
end)

zest.register_function("lock_release", function(name, owner)
    local key = PREFIX .. name
    local holder = zest.get(key)
    if holder == nil then
        return "free"
    end
    if holder ~= owner then
        return "not owner"
    end
    zest.del(key)
    return "released"
end)

zest.register_function("lock_owner", function(name)
    return zest.get(PREFIX .. name)
end)
