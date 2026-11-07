namespace S8.LuaHost;

// The Lua face of the SDK. Raw C# functions return (true, ...) or (false, message);
// error() is raised here, in Lua, so no Lua error ever unwinds through managed frames.
internal static class Prelude
{
    public const string Source = """
        local raw = ...

        local function check(ok, ...)
          if not ok then error((...), 3) end
          return ...
        end

        local function wrap(f)
          return function(...) return check(f(...)) end
        end

        local function entity(kind)
          return {
            __index = function(u, k) return check(raw.get(u, k)) end,
            __newindex = function(u, k, v) check(raw.set(u, k, v)) end,
            __tostring = function(u) return kind .. " " .. u.id end,
          }
        end
        raw.set_meta(entity("officer"), entity("city"))

        -- Valid only while a battle effect runs.
        local unit = setmetatable({}, {
          __index = function(_, k)
            if k == "restore_soldiers" then
              return function(_, amount) return check(raw.unit_restore(amount)) end
            end
            -- heal(amount[, cap]): drops the fraction, applies the cap, skips zero.
            if k == "heal" then
              return function(_, amount, cap)
                amount = math.floor(amount)
                if cap and amount > cap then amount = cap end
                if amount <= 0 then return 0 end
                return check(raw.unit_restore(amount))
              end
            end
            -- A destroyed unit has no wounded to bring back.
            if k == "wounded" then
              local soldiers = check(raw.unit_get("soldiers"))
              if soldiers == 0 then return 0 end
              return math.max(check(raw.unit_get("capacity")) - soldiers, 0)
            end
            return check(raw.unit_get(k))
          end,
          __newindex = function() error("부대 값은 읽기 전용입니다. heal 을 쓰십시오.", 2) end,
        })
        local trait = setmetatable({}, {
          __index = function(_, k)
            if k == "unit" then return unit end
            if k == "show" then return function(_, text) check(raw.trait_show(text)) end end
            return check(raw.trait_get(k))
          end,
        })

        menu = { add = wrap(raw.menu_add) }
        game = setmetatable({ officer = wrap(raw.officer), city = wrap(raw.city) }, {
          __index = function(_, k)
            if k == "player" then return check(raw.player()) end
          end,
        })
        events = { on = wrap(raw.events_on) }
        -- effect "이름" { description = ..., turn_end = function(unit, value, e) ... end }
        -- Defined once; the trait editor puts it into trait slots with a value.
        -- A handler that returns text shows it as the unit's line.
        local moments = { "turn_start", "unit_action_end", "turn_end" }
        effect = function(name)
          if type(name) ~= "string" then error("효과 이름이 필요합니다.", 2) end
          return function(spec)
            if type(spec) ~= "table" then error("효과 정의 표가 필요합니다.", 2) end
            local handlers = {}
            for i, moment in ipairs(moments) do
              local fn = spec[moment]
              if fn ~= nil and type(fn) ~= "function" then error(moment .. ": 함수가 필요합니다.", 2) end
              handlers[i] = fn and function()
                local line = fn(unit, trait.value, trait)
                if line ~= nil then check(raw.trait_show(tostring(line))) end
              end
            end
            check(raw.effect_define(spec.id, name, spec.description, handlers[1], handlers[2], handlers[3]))
          end
        end
        log = wrap(raw.log)
        """;
}
