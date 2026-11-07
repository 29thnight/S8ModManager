using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using S8.Scripting;

namespace S8.LuaHost;

// Runs main.lua from the mod folder over the SDK's flat layer. Lua gets only
// base (without file loading), string, table, math and bit: no io, os, package,
// ffi or debug, so a script cannot reach files or process memory.
public sealed unsafe class LuaMod : ScriptMod
{
    private static LuaApi? api;
    private nint state;
    private GCHandle self;
    private int officerMeta, cityMeta, effectCount;
    private ScriptedTraitComponent? currentTrait;
    private string logPath = "";

    [StructLayout(LayoutKind.Sequential)]
    private struct EntityRef
    {
        public ulong Session;
        public ushort Id;
    }

    private static LuaApi Api => api ?? throw new InvalidOperationException("LuaJIT is not loaded.");

    protected override void OnLoad()
    {
        string directory = Context.ModDirectory;
        logPath = Path.Combine(directory, "lua.log");
        api ??= new LuaApi(Path.Combine(directory, "lua51.dll"));
        state = Api.NewState();
        if (state == 0)
            throw new InvalidOperationException("Lua 상태를 만들지 못했습니다.");
        self = GCHandle.Alloc(this);

        OpenLibraries();
        Run(Encoding.UTF8.GetBytes(Prelude.Source), "=prelude", pushRaw: true);
        Run(File.ReadAllBytes(Path.Combine(directory, "main.lua")), "@main.lua", pushRaw: false);
    }

    protected override void OnUnload()
    {
        if (state != 0)
            Api.Close(state);
        state = 0;
        currentTrait = null;
        if (self.IsAllocated)
            self.Free();
    }

    private void OpenLibraries()
    {
        var a = Api;
        (nint Open, string Name)[] libraries =
            [((nint)a.OpenBase, ""), ((nint)a.OpenString, "string"), ((nint)a.OpenTable, "table"),
             ((nint)a.OpenMath, "math"), ((nint)a.OpenBit, "bit")];

        foreach (var library in libraries)
        {
            a.PushCClosure(state, (delegate* unmanaged[Cdecl]<nint, int>)library.Open, 0);
            a.PushString(state, library.Name);
            a.Call(state, 1, 0);
        }

        foreach (string name in new[] { "dofile", "loadfile", "load", "loadstring", "newproxy", "getfenv", "setfenv" })
        {
            a.PushNil(state);
            a.SetNamed(state, LuaApi.GlobalsIndex, name);
        }
    }

    private void Run(byte[] code, string name, bool pushRaw)
    {
        var a = Api;
        byte[] chunk = Encoding.UTF8.GetBytes(name + "\0");
        int top = a.GetTop(state);
        int status;

        fixed (byte* data = code, label = chunk)
            status = a.LoadBuffer(state, data, (nuint)code.Length, label);
        if (status == 0)
        {
            if (pushRaw)
                PushRawTable();
            status = a.PCall(state, pushRaw ? 1 : 0, 0, 0);
        }

        string? error = status == 0 ? null : a.ReadString(state, -1);
        a.SetTop(state, top);
        if (error != null)
            throw new InvalidOperationException($"Lua 오류: {error}");
    }

    private void PushRawTable()
    {
        var a = Api;
        (string Name, nint Function)[] functions =
        [
            ("set_meta", (nint)(delegate* unmanaged[Cdecl]<nint, int>)&RawSetMeta), ("menu_add", (nint)(delegate* unmanaged[Cdecl]<nint, int>)&RawMenuAdd), ("player", (nint)(delegate* unmanaged[Cdecl]<nint, int>)&RawPlayer),
            ("officer", (nint)(delegate* unmanaged[Cdecl]<nint, int>)&RawOfficer), ("city", (nint)(delegate* unmanaged[Cdecl]<nint, int>)&RawCity), ("get", (nint)(delegate* unmanaged[Cdecl]<nint, int>)&RawGet), ("set", (nint)(delegate* unmanaged[Cdecl]<nint, int>)&RawSet),
            ("events_on", (nint)(delegate* unmanaged[Cdecl]<nint, int>)&RawEventsOn), ("battle_on", (nint)(delegate* unmanaged[Cdecl]<nint, int>)&RawBattleOn), ("trait_show", (nint)(delegate* unmanaged[Cdecl]<nint, int>)&RawTraitShow),
            ("trait_get", (nint)(delegate* unmanaged[Cdecl]<nint, int>)&RawTraitGet), ("unit_get", (nint)(delegate* unmanaged[Cdecl]<nint, int>)&RawUnitGet), ("unit_restore", (nint)(delegate* unmanaged[Cdecl]<nint, int>)&RawUnitRestore),
            ("log", (nint)(delegate* unmanaged[Cdecl]<nint, int>)&RawLog),
        ];

        a.CreateTable(state, 0, functions.Length);
        foreach (var function in functions)
        {
            a.PushLightUserdata(state, (void*)GCHandle.ToIntPtr(self));
            a.PushCClosure(state, (delegate* unmanaged[Cdecl]<nint, int>)function.Function, 1);
            a.SetNamed(state, -2, function.Name);
        }
    }

    // Every raw entry: find the mod from the closure's upvalue, run the body, and
    // report failure as values. Exceptions never cross into Lua.
    private static int Guard(nint L, Func<LuaMod, nint, int> body)
    {
        var a = Api;
        int top = a.GetTop(L);
        try
        {
            var mod = (LuaMod)GCHandle.FromIntPtr((nint)a.ToUserdata(L, LuaApi.FirstUpvalue)).Target!;
            int count = body(mod, L);
            a.PushBoolean(L, 1);
            a.Insert(L, -(count + 1));
            return count + 1;
        }
        catch (Exception exception)
        {
            a.SetTop(L, top);
            a.PushBoolean(L, 0);
            a.PushString(L, exception.Message);
            return 2;
        }
    }

    private static string String(nint L, int index, string what) =>
        Api.Type(L, index) == LuaApi.TypeString ? Api.ReadString(L, index)! : throw new ArgumentException($"{what}: 문자열이 필요합니다.");

    private static double Number(nint L, int index, string what) =>
        Api.Type(L, index) == LuaApi.TypeNumber ? Api.ToNumber(L, index) : throw new ArgumentException($"{what}: 숫자가 필요합니다.");

    private static T Whole<T>(double value, string what) where T : struct, System.Numerics.INumber<T>
    {
        if (value != Math.Floor(value))
            throw new ArgumentException($"{what}: 정수가 필요합니다.");
        try { return T.CreateChecked(value); }
        catch (OverflowException) { throw new ArgumentException($"{what}: 범위를 벗어났습니다 ({value})."); }
    }

    private static int Function(nint L, int index)
    {
        if (Api.Type(L, index) != LuaApi.TypeFunction)
            throw new ArgumentException("함수가 필요합니다.");
        Api.PushValue(L, index);
        return Api.Ref(L, LuaApi.RegistryIndex);
    }

    private void Invoke(int function, Action<nint>? pushArgument = null)
    {
        if (state == 0)
            return;
        var a = Api;
        int top = a.GetTop(state);
        a.RawGetI(state, LuaApi.RegistryIndex, function);
        pushArgument?.Invoke(state);
        if (a.PCall(state, pushArgument == null ? 0 : 1, 0, 0) != 0)
            Log($"오류: {a.ReadString(state, -1)}");
        a.SetTop(state, top);
    }

    private void Log(string text) =>
        File.AppendAllText(logPath, $"{DateTimeOffset.Now:yyyy-MM-dd HH:mm:ss} {text}{Environment.NewLine}");

    // The Lua stack of a raw call is the caller's thread, which may be a coroutine.
    private (bool IsOfficer, ushort Id) Entity(nint L, int index)
    {
        var a = Api;
        if (a.Type(L, index) != LuaApi.TypeUserdata || a.GetMetatable(L, index) == 0)
            throw new ArgumentException("무장이나 도시가 필요합니다.");
        a.RawGetI(L, LuaApi.RegistryIndex, officerMeta);
        bool officer = a.RawEqual(L, -1, -2) != 0;
        a.RawGetI(L, LuaApi.RegistryIndex, cityMeta);
        bool city = a.RawEqual(L, -1, -3) != 0;
        a.SetTop(L, -4);
        if (!officer && !city)
            throw new ArgumentException("무장이나 도시가 필요합니다.");

        var entity = (EntityRef*)a.ToUserdata(L, index);
        if (entity->Session != World.SessionId)
            throw new InvalidOperationException("이전 세션에서 얻은 값입니다. 다시 얻으십시오.");
        return (officer, entity->Id);
    }

    // Pushing on L, not state: the caller may be a coroutine.
    private static void PushEntityOn(LuaMod mod, nint L, int meta, ushort id)
    {
        var a = Api;
        var entity = (EntityRef*)a.NewUserdata(L, (nuint)sizeof(EntityRef));
        *entity = new EntityRef { Session = mod.World.SessionId, Id = id };
        a.RawGetI(L, LuaApi.RegistryIndex, meta);
        a.SetMetatable(L, -2);
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int RawSetMeta(nint L) => Guard(L, (mod, l) =>
    {
        Api.PushValue(l, 1);
        mod.officerMeta = Api.Ref(l, LuaApi.RegistryIndex);
        Api.PushValue(l, 2);
        mod.cityMeta = Api.Ref(l, LuaApi.RegistryIndex);
        return 0;
    });

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int RawMenuAdd(nint L) => Guard(L, (mod, l) =>
    {
        var surface = String(l, 1, "메뉴 위치") switch
        {
            "title_edit" => MenuSurface.TitleEdit,
            "in_game_edit" => MenuSurface.InGameEdit,
            "function" => MenuSurface.Function,
            "palace" => MenuSurface.Palace,
            var other => throw new ArgumentException($"알 수 없는 메뉴 위치: {other} (title_edit, in_game_edit, function, palace)")
        };
        string label = String(l, 2, "메뉴 이름");
        int function = Function(l, 3);
        mod.AddMenu(surface, label, () => mod.Invoke(function));
        return 0;
    });

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int RawPlayer(nint L) => Guard(L, (mod, l) =>
    {
        PushEntityOn(mod, l, mod.officerMeta, mod.World.PlayerOfficer.Id);
        return 1;
    });

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int RawOfficer(nint L) => Guard(L, (mod, l) =>
    {
        PushEntityOn(mod, l, mod.officerMeta, mod.World.Officer(Whole<ushort>(Number(l, 1, "무장 번호"), "무장 번호")).Id);
        return 1;
    });

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int RawCity(nint L) => Guard(L, (mod, l) =>
    {
        PushEntityOn(mod, l, mod.cityMeta, mod.World.City(Whole<ushort>(Number(l, 1, "도시 번호"), "도시 번호")).Id);
        return 1;
    });

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int RawGet(nint L) => Guard(L, (mod, l) =>
    {
        var (isOfficer, id) = mod.Entity(l, 1);
        string key = String(l, 2, "이름");
        if (key == "id")
        {
            Api.PushNumber(l, id);
            return 1;
        }
        if (!isOfficer && key == "name")
        {
            Api.PushString(l, mod.World.City(id).Name);
            return 1;
        }
        Api.PushNumber(l, isOfficer ? EntityFields.Get(mod.World.Officer(id), key) : EntityFields.Get(mod.World.City(id), key));
        return 1;
    });

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int RawSet(nint L) => Guard(L, (mod, l) =>
    {
        var (isOfficer, id) = mod.Entity(l, 1);
        string key = String(l, 2, "이름");
        double value = Number(l, 3, key);
        if (isOfficer)
            EntityFields.Set(mod.World.Officer(id), key, value);
        else
            EntityFields.Set(mod.World.City(id), key, value);
        return 0;
    });

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int RawEventsOn(nint L) => Guard(L, (mod, l) =>
    {
        string name = String(l, 1, "사건 이름");
        int function = Function(l, 2);
        static void PushDate(nint s, string prefix, GameDate date)
        {
            Api.PushNumber(s, date.Year);
            Api.SetNamed(s, -2, prefix + "year");
            Api.PushNumber(s, date.Month);
            Api.SetNamed(s, -2, prefix + "month");
        }

        switch (name)
        {
            case "month_changed":
                mod.World.MonthChanged += month => mod.Invoke(function, s =>
                {
                    Api.CreateTable(s, 0, 4);
                    PushDate(s, "", month.Current);
                    PushDate(s, "previous_", month.Previous);
                });
                break;
            case "council_entered":
                mod.World.CouncilEntered += council => mod.Invoke(function, s =>
                {
                    Api.CreateTable(s, 0, 2);
                    PushDate(s, "", council.Date);
                });
                break;
            default:
                throw new ArgumentException($"알 수 없는 사건: {name} (month_changed, council_entered)");
        }
        return 0;
    });

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int RawBattleOn(nint L) => Guard(L, (mod, l) =>
    {
        ushort trait = Whole<ushort>(Number(l, 1, "기재 번호"), "기재 번호");
        if (trait is < 1 or > 254)
            throw new ArgumentException($"기재 번호는 1~254입니다 ({trait}).");
        string moment = String(l, 2, "시점");
        int function = Function(l, 3);
        string id = $"lua.{++mod.effectCount}.{moment}";
        Action<ScriptedTraitComponent> run = component =>
        {
            mod.currentTrait = component;
            try { mod.Invoke(function); }
            finally { mod.currentTrait = null; }
        };
        var definition = new ScriptEffectDefinition(id, $"Lua {moment}", "Lua 스크립트 효과");

        switch (moment)
        {
            case "turn_start": mod.Effects.Register(definition, onTurnStart: run); break;
            case "unit_action_end": mod.Effects.Register(definition, onUnitActionEnd: run); break;
            case "turn_end": mod.Effects.Register(definition, onTurnEnd: run); break;
            default: throw new ArgumentException($"알 수 없는 시점: {moment}");
        }
        mod.Effects.Bind(trait, id);
        return 0;
    });

    private ScriptedTraitComponent Trait => currentTrait
        ?? throw new InvalidOperationException("전투 효과가 실행되는 동안에만 쓸 수 있습니다.");

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int RawTraitShow(nint L) => Guard(L, (mod, l) =>
    {
        mod.Trait.Show(String(l, 1, "대사"));
        return 0;
    });

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int RawTraitGet(nint L) => Guard(L, (mod, l) =>
    {
        string key = String(l, 1, "이름");
        if (key != "id")
            throw new ArgumentException($"기재에 없는 값: {key} (id, unit, show)");
        Api.PushNumber(l, mod.Trait.PersonalityId);
        return 1;
    });

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int RawUnitGet(nint L) => Guard(L, (mod, l) =>
    {
        var unit = mod.Trait.Unit;
        Api.PushNumber(l, String(l, 1, "이름") switch
        {
            "soldiers" => unit.Soldiers,
            "capacity" => unit.Capacity,
            var other => throw new ArgumentException($"부대에 없는 값: {other} (soldiers, capacity, restore_soldiers)")
        });
        return 1;
    });

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int RawUnitRestore(nint L) => Guard(L, (mod, l) =>
    {
        Api.PushNumber(l, mod.Trait.Unit.RestoreSoldiers(Whole<uint>(Number(l, 1, "회복량"), "회복량")));
        return 1;
    });

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int RawLog(nint L) => Guard(L, (mod, l) =>
    {
        mod.Log(Api.ReadString(l, 1) ?? Api.Type(l, 1).ToString());
        return 0;
    });
}
