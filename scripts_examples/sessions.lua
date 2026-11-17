-- Login sessions that expire on their own.
--
-- session:<token> = <user>, with a TTL. setnx refuses a token that is already
-- in use, expire implements a sliding expiration. Keys removed by their TTL do
-- not fire "del" triggers: only d / delby / zest.del do.
--
--   call session_open "tok-123" "alice" "1800"   -> "session:tok-123"
--   call session_user "tok-123"                  -> "alice"
--   call session_touch "tok-123" "1800"          -> "extended"
--   call session_close "tok-123"                 -> "closed"
--   call sessions_active                         -> "0"

local PREFIX = "session:"

local function seconds(text, default)
    local n = math.tointeger(tonumber(text or default))
    if n == nil or n <= 0 then
        error("the duration must be a positive number of seconds")
    end
    return n
end

zest.register_function("session_open", function(token, user, ttl)
    local key = PREFIX .. token
    if not zest.setnx(key, user) then
        error("this token is already in use")
    end
    zest.expire(key, seconds(ttl, "3600"))
    zest.incr("stats:sessions:opened")
    return key
end)

zest.register_function("session_user", function(token)
    return zest.get(PREFIX .. token)
end)

zest.register_function("session_touch", function(token, ttl)
    return zest.expire(PREFIX .. token, seconds(ttl, "3600")) and "extended" or "expired"
end)

zest.register_function("session_close", function(token)
    return zest.del(PREFIX .. token) and "closed" or "unknown"
end)

zest.register_function("sessions_active", function()
    return zest.count("sw", PREFIX)
end)
