using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using S8.LuaHost;
using S8.Scripting;

// Runs real LuaJIT over a fake engine: main.lua reaches officers and cities
// through the SDK handles, the sandbox hides files, and errors stay in Lua.
internal static unsafe class LuaChecks
{
    [StructLayout(LayoutKind.Sequential)]
    private struct Request
    {
        public ulong Session;
        public uint Kind, Index;
        public long Value;
    }

    private static ulong session = 40;
    private static readonly Dictionary<(ulong Id, uint Kind, uint Index), long> values = [];

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static long Invoke(int operation, ulong target, long argument, void* data, uint bytes)
    {
        if (operation == 29)
        {
            new Span<byte>(data, (int)bytes).Clear();
            *(ulong*)data = session;
            *(ushort*)((byte*)data + 8) = 565;
            return 0;
        }
        if (operation is 30 or 31)
        {
            new Span<byte>(data, (int)bytes).Clear();
            *(ulong*)data = 0x100000 + target * 0x1000;
            *(ulong*)((byte*)data + 8) = session;
            *(ushort*)((byte*)data + 16) = (ushort)target;
            return 1;
        }
        if (operation is 33 or 34)
        {
            var request = *(Request*)data;
            if (request.Session != session)
                return (int)ConnectionStatus.StaleSession;
            var key = (target, request.Kind, request.Index);
            if (operation == 34)
                values[key] = request.Value;
            return values.GetValueOrDefault(key);
        }
        return (int)ConnectionStatus.InvalidApi;
    }

    private const string Script = """
        local me = game.player
        me.gold = 500
        me.gold = me.gold + 25
        me.intelligence = 99
        log("gold=" .. me.gold .. " int=" .. me.intelligence .. " id=" .. me.id)
        log("sandbox=" .. tostring(io) .. tostring(os) .. tostring(require) .. tostring(ffi)
          .. tostring(loadstring) .. tostring(dofile) .. tostring(debug))
        local ok, err = pcall(function() me.gold = -1 end)
        log("negative=" .. tostring(ok) .. " " .. err)
        log("unknown=" .. tostring((pcall(function() return me.nope end))))
        local city = game.city(1)
        city.grain = 300
        log("grain=" .. city.grain .. " limit_write=" .. tostring((pcall(function() city.technology_limit = 3 end))))
        keep = me
        events.on("month_changed", function(d)
          log("month=" .. d.year .. "/" .. d.month .. " from " .. d.previous_month
            .. " keep=" .. tostring((pcall(function() return keep.gold end))))
        end)
        """;

    public static int Run(string luaJit)
    {
        int checks = 0;
        void Check(bool value, string message)
        {
            if (!value)
                throw new Exception(message);
            ++checks;
        }

        if (!File.Exists(luaJit))
            throw new FileNotFoundException("Build S8.LuaJIT first; the Lua checks use the real LuaJIT.", luaJit);

        string directory = Path.Combine(Path.GetTempPath(), "s8-lua-checks-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        File.Copy(luaJit, Path.Combine(directory, "lua51.dll"));
        string log = Path.Combine(directory, "lua.log");

        NativeApi api = new()
        {
            Version = NativeContract.Version,
            Size = (uint)sizeof(NativeApi),
            Connection = new() { Capabilities = NativeCapability.GameData },
            Invoke = &Invoke
        };
        nint previous = Native.Enter(&api);
        try
        {
            File.WriteAllText(Path.Combine(directory, "main.lua"), Script);
            var mod = new LuaMod();
            IScriptModule module = mod;
            module.Initialize(new ModContext("fixture", directory));
            string text = File.ReadAllText(log);

            Check(text.Contains("gold=525 int=99 id=565"), "Lua reads and writes officer values through the handle: " + text);
            Check(text.Contains("sandbox=nilnilnilnilnilnilnil"), "The sandbox exposes no file, module, ffi or debug access: " + text);
            Check(text.Contains("negative=false main.lua:8:") && text.Contains("gold"),
                "An out-of-range value raises a Lua error at the script line: " + text);
            Check(text.Contains("unknown=false"), "An unknown field is an error, not nil");
            Check(text.Contains("grain=300 limit_write=false"), "City fields write, read-only fields refuse");

            mod.World.DispatchActivity(ScriptEvent.MonthChanged, 200 | (1 << 16), 200 | (2 << 16));
            Check(File.ReadAllText(log).Contains("month=200/2 from 1 keep=true"), "Lua receives world events with dates");

            ++session;
            mod.World.RefreshSession();
            mod.World.DispatchActivity(ScriptEvent.MonthChanged, 200 | (2 << 16), 200 | (3 << 16));
            Check(File.ReadAllText(log).Contains("month=200/3 from 2 keep=false"),
                "An officer kept from an earlier session is refused");
            module.Dispose();

            File.WriteAllText(Path.Combine(directory, "main.lua"), "local x = = 1");
            bool rejected = false;
            try { ((IScriptModule)new LuaMod()).Initialize(new ModContext("fixture", directory)); }
            catch (InvalidOperationException error) { rejected = error.Message.Contains("main.lua:1"); }
            Check(rejected, "A syntax error fails the load with the script position");
        }
        finally
        {
            Native.Leave(previous);
        }

        return checks;
    }
}
