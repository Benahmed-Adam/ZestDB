-- A secondary index: find a user by email without scanning every user.
--
-- Users are stored as   user:<id> = "name=<name>;email=<email>;city=<city>"
-- and the triggers keep  idx:user:email:<email> = user:<id>  in step:
--   * before set : the email is required and must not belong to another user;
--                  if the user changes email, the old index entry is removed
--   * after set  : the index points to the user
--   * before del : the index entry is removed with the user (a before trigger
--                  still sees the record, an after-del trigger would not)
--
--   s user:1 "name=Ada;email=Ada@Example.com;city=London"
--   call find_user_by_email "ada@example.com"   -> "user:1 name=Ada;email=Ada@Example.com;city=London"
--   s user:2 "name=Bob;email=ada@example.com"   -> 603, the email is taken
--   call users_in_city "London"                 -> "user:1"

local INDEX = "idx:user:email:"

local function parse(record)
    local fields = {}
    for name, value in record:gmatch("([^;=]+)=([^;]*)") do
        fields[name] = value
    end
    return fields
end

local function email_of(record)
    local email = parse(record).email
    return email and email ~= "" and email:lower() or nil
end

zest.register_trigger{
    name = "user_email_unique", phase = "before", event = "set", mode = "sw", pattern = "user:",
    fn = function(key, value)
        local email = email_of(value)
        if email == nil then
            error(key .. ": a user record needs an email= field")
        end
        local owner = zest.get(INDEX .. email)
        if owner ~= nil and owner ~= key then
            error(email .. " is already used by " .. owner)
        end
        local previous = zest.get(key)
        local old = previous and email_of(previous)
        if old ~= nil and old ~= email then
            zest.del(INDEX .. old)
        end
        return value
    end,
}

zest.register_trigger{
    name = "user_email_index", phase = "after", event = "set", mode = "sw", pattern = "user:",
    fn = function(key, value)
        zest.set(INDEX .. email_of(value), key)
    end,
}

zest.register_trigger{
    name = "user_email_cleanup", phase = "before", event = "del", mode = "sw", pattern = "user:",
    fn = function(key)
        local record = zest.get(key)
        local email = record and email_of(record)
        if email ~= nil then
            zest.del(INDEX .. email)
        end
    end,
}

zest.register_function("find_user_by_email", function(email)
    local key = zest.get(INDEX .. email:lower())
    if key == nil then
        return nil
    end
    return key .. " " .. (zest.get(key) or "")
end)

-- Without an index, a query scans the keys: fine for a few thousand users,
-- add another index (idx:user:city:...) when it grows.
zest.register_function("users_in_city", function(city)
    local found = {}
    for _, key in ipairs(zest.keys("sw", "user:", 10000)) do
        local record = zest.get(key)
        if record ~= nil and parse(record).city == city then
            found[#found + 1] = key
        end
    end
    return table.concat(found, ",")
end)
