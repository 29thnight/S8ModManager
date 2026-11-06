namespace S8.Scripting;

// The C ABI guard shared by the JIT host and AOT mod exports.
public static unsafe class ModDispatch
{
    public static bool IsLoadEvent(int kind) => (ScriptEvent)kind is ScriptEvent.Open or ScriptEvent.Configure;

    // resolve runs with the engine API entered, so module initialization may call the engine.
    public static int Run(NativeApi* api, Func<IScriptModule?> resolve, int kind, int owner, int value)
    {
        if (api == null)
            return (int)ConnectionStatus.InvalidApi;

        if (api->Version != NativeContract.Version || api->Size != sizeof(NativeApi))
            return (int)ConnectionStatus.ApiVersionMismatch;

        if (api->Connection.Status != ConnectionStatus.Ready)
            return (int)api->Connection.Status;

        nint previous = Native.Enter(api);

        try
        {
            var module = resolve();

            if (module == null)
                return (int)ConnectionStatus.ScriptFailure;

            module.Dispatch((ScriptEvent)kind, owner, value);
            return 0;
        }
        catch (Exception error)
        {
            // No managed exception may unwind through the C ABI into game code.
            System.Diagnostics.Trace.WriteLine(error);
            System.Diagnostics.Debug.WriteLine(error);
            return error is EngineCallException binding
                ? (int)binding.Status
                : (int)ConnectionStatus.ScriptFailure;
        }
        finally
        {
            Native.Leave(previous);
        }
    }
}

// Backing state for a NativeAOT mod. The mod exports S8ModLoad and S8ModDispatch
// and forwards them here; the manager calls S8ModLoad once before any event.
public static unsafe class AotModHost<TModule> where TModule : IScriptModule, new()
{
    private static ModContext? context;
    private static TModule? module;

    public static int Load(char* modDirectory, char* gameDirectory)
    {
        if (modDirectory == null || gameDirectory == null || context != null)
            return (int)ConnectionStatus.InvalidApi;

        context = new ModContext(new string(gameDirectory), new string(modDirectory));
        return 0;
    }

    public static int Dispatch(NativeApi* api, int kind, int owner, int value) =>
        ModDispatch.Run(api, () =>
        {
            if (module == null && context != null && ModDispatch.IsLoadEvent(kind))
            {
                var created = new TModule();
                created.Initialize(context);
                module = created;
            }

            return module;
        }, kind, owner, value);
}
