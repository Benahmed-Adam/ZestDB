-- Rate limiting with a counter and a TTL (fixed window).
--
-- The first request of a window creates the counter and gives it the window as
-- TTL; when it expires, the next request starts a new window. Scripts have no
-- clock (os is not available): the TTL is the clock.
--
--   call allow "api-key-42" "100" "60"   -> "allowed" for the first 100 calls of the minute,
--                                           then "blocked:<seconds before the window resets>"

zest.register_function("allow", function(client, max, window)
    local limit = math.tointeger(tonumber(max))
    local seconds = math.tointeger(tonumber(window))
    if client == nil or limit == nil or seconds == nil or limit <= 0 or seconds <= 0 then
        error("usage: call allow \"<client>\" \"<max requests>\" \"<window in seconds>\"")
    end

    local key = "ratelimit:" .. client
    local hits = zest.incr(key)
    if hits == 1 or zest.ttl(key) == -1 then
        zest.expire(key, seconds)
    end
    if hits > limit then
        return "blocked:" .. zest.ttl(key)
    end
    return "allowed"
end)

zest.register_function("reset_limit", function(client)
    zest.del("ratelimit:" .. client)
    return "reset"
end)
