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
            return check(raw.unit_get(k))
          end,
          __newindex = function() error("부대 값은 읽기 전용입니다. restore_soldiers 를 쓰십시오.", 2) end,
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
        battle = {}
        for _, moment in ipairs({ "turn_start", "unit_action_end", "turn_end" }) do
          battle["on_" .. moment] = function(trait_id, fn)
            if type(fn) ~= "function" then error("함수가 필요합니다.", 2) end
            check(raw.battle_on(trait_id, moment, function() return fn(trait) end))
          end
        end
        log = wrap(raw.log)
        """;
}
