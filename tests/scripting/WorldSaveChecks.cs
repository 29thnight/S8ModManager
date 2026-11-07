using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using S8.Scripting;

// Each mod has its own GameWorld, so its own WorldSaveData, over one save blob per game.
internal static unsafe class WorldSaveChecks
{
    private static byte[] saved = [];

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static long Invoke(int operation, ulong target, long argument, void* data, uint bytes)
    {
        if (operation == 44) // ReadWorldSaveData
        {
            if (data == null)
                return saved.Length;
            saved.CopyTo(new Span<byte>(data, (int)bytes));
            return 0;
        }
        if (operation == 45) // StageWorldSaveData
        {
            saved = new ReadOnlySpan<byte>(data, (int)bytes).ToArray();
            return 0;
        }
        return (int)ConnectionStatus.InvalidApi;
    }

    public static int Run()
    {
        int checks = 0;
        void Check(bool value, string message)
        {
            if (!value)
                throw new Exception(message);
            ++checks;
        }

        saved = [];
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
            // No setup types in the core library, so the worlds only hold save data.
            using var first = new GameWorld(scriptAssembly: typeof(int).Assembly);
            using var second = new GameWorld(scriptAssembly: typeof(int).Assembly);
            first.BeginSession(90);
            second.BeginSession(90);

            var firstData = first.SaveData;
            var secondData = second.SaveData;
            firstData.Set("first.value", 1);
            secondData.Set("second.value", 2);
            Check(secondData.Get<int>("first.value") == 1, "A mod reads what another mod saved after it loaded");
            firstData.Set("first.value", 3);

            using var reloaded = new GameWorld(scriptAssembly: typeof(int).Assembly);
            reloaded.BeginSession(90);
            Check(reloaded.SaveData.Get<int>("first.value") == 3 && reloaded.SaveData.Get<int>("second.value") == 2,
                "Saving one mod's value keeps every other mod's value");
        }
        finally
        {
            Native.Leave(previous);
        }
        return checks;
    }
}
