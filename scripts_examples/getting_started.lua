-- Getting started: the shape of every ZestDB script.
--
-- A script runs once when it is loaded. At that point it may only call
-- zest.register_function and zest.register_trigger: reading or writing data is
-- refused. The functions it registers run later, on `call` or on a write, and
-- have the whole zest API.
--
--   script load getting_started "<this file>"   (or copy it to DbPath/scripts/ and `script reload getting_started`)
--   call hello "Ada"                             -> "Hello, Ada!"
--   call visits "home"                           -> "1", then "2", ...

-- Arguments always arrive as strings; the return value becomes the response
-- (converted with tostring, nil gives an empty response).
zest.register_function("hello", function(name)
    if name == nil or name == "" then
        error("usage: call hello \"<name>\"")
    end
    return "Hello, " .. name .. "!"
end)

-- A page counter: zest.incr creates the key at 0 and returns the new value.
zest.register_function("visits", function(page)
    local count = zest.incr("visits:" .. page)
    if count % 1000 == 0 then
        zest.log(string.format("page %s reached %d visits", page, count))
    end
    return count
end)

-- print() also goes to the server log, at the info level.
print("getting_started loaded")
