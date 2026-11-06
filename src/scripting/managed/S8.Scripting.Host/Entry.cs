using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using S8.Scripting;

namespace S8.Scripting.Host;

// JIT mod host. The native manager has already chosen one build per mod id, so
// each registered slot is a distinct mod; Register still refuses a repeated id.
public static unsafe class Entry
{
    private static readonly List<ModSlot> slots = [];

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static int Register(char* id, char* modDirectory, char* gameDirectory, char* assembly, char* type)
    {
        try
        {
            if (id == null || modDirectory == null || gameDirectory == null || assembly == null || type == null)
                return (int)ConnectionStatus.InvalidApi;

            var slot = new ModSlot(new string(id), new string(modDirectory), new string(gameDirectory),
                new string(assembly), new string(type));

            if (slots.Any(existing => string.Equals(existing.Id, slot.Id, StringComparison.OrdinalIgnoreCase)))
                return (int)ConnectionStatus.ScriptFailure;

            slots.Add(slot);
            return slots.Count - 1;
        }
        catch (Exception error)
        {
            System.Diagnostics.Trace.WriteLine(error);
            return (int)ConnectionStatus.ScriptFailure;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static int Dispatch(NativeApi* api, int handle, int kind, int owner, int value)
    {
        if (handle < 0 || handle >= slots.Count)
            return (int)ConnectionStatus.InvalidApi;

        var slot = slots[handle];
        return ModDispatch.Run(api, () => slot.Resolve(kind), kind, owner, value);
    }

    // The manager switched the mod off: stop it now. The slot stays registered and
    // the next load event loads the mod again if it is switched back on.
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static int Unload(NativeApi* api, int handle)
    {
        if (handle < 0 || handle >= slots.Count)
            return (int)ConnectionStatus.InvalidApi;

        var slot = slots[handle];
        return ModDispatch.Run(api, slot.Unload);
    }

    private sealed class ModSlot(string id, string modDirectory, string gameDirectory, string assembly, string type)
    {
        private AssemblyLoadContext? context;
        private IScriptModule? module;
        private Assembly? loadedAssembly;
        private string? loadedIdentity;
        private LiveSource? live;
        private bool swapPending;

        public string Id { get; } = id;

        public void Unload()
        {
            var stopped = module;
            var stoppedContext = context;
            module = null;
            context = null;
            loadedAssembly = null;
            loadedIdentity = null;
            live = null;
            swapPending = false;
            if (stopped == null)
                return;
            try
            {
                stopped.Disable();
            }
            finally
            {
                stopped.Dispose();
                stoppedContext?.Unload();
            }
        }

        // Live coding: with a source\ folder, edited method bodies apply in place on
        // the next event (hot reload). Otherwise, or for structural edits, a rebuilt
        // assembly replaces the running module on the next open, unless the module
        // holds unsaved state.
        public IScriptModule? Resolve(int kind)
        {
            if (live != null && loadedAssembly != null && live.HasChanges())
            {
                swapPending |= !live.TryPatch(loadedAssembly, out string reason);
                System.Diagnostics.Trace.WriteLine($"[S8 live] {Id}: {reason}");
            }

            if (!ModDispatch.IsLoadEvent(kind))
                return module;

            string manifest = Path.Combine(modDirectory, "mod.json");
            string identity = File.GetLastWriteTimeUtc(assembly).Ticks + ":" + File.GetLastWriteTimeUtc(manifest).Ticks;
            bool stale = live != null ? swapPending : identity != loadedIdentity;

            if (module != null && (!stale || !module.CanReload))
                return module;

            if (module == null)
                live = LiveSource.Find(modDirectory, assembly);
            else if (live != null)
                live.Rebuild();

            var next = new ScriptContext(assembly);
            using var stream = new MemoryStream(live?.Image ?? File.ReadAllBytes(assembly));
            var loaded = next.LoadFromStream(stream);
            var sdk = typeof(IScriptModule).Assembly.GetName();
            var reference = loaded.GetReferencedAssemblies().FirstOrDefault(candidate => candidate.Name == sdk.Name);

            if (reference?.Version != sdk.Version)
            {
                next.Unload();
                throw new EngineCallException(ConnectionStatus.ApiVersionMismatch, "LoadMod");
            }

            var replacement = (IScriptModule)Activator.CreateInstance(loaded.GetType(type, throwOnError: true)!)!;

            replacement.Initialize(new ModContext(gameDirectory, modDirectory));
            module?.Dispose();
            context?.Unload();
            context = next;
            module = replacement;
            loadedAssembly = loaded;
            loadedIdentity = identity;
            swapPending = false;
            return module;
        }
    }

    private sealed class ScriptContext(string path) : AssemblyLoadContext(isCollectible: true)
    {
        private readonly AssemblyDependencyResolver resolver = new(path);

        protected override Assembly? Load(AssemblyName name)
        {
            if (name.Name == typeof(IScriptModule).Assembly.GetName().Name)
                return typeof(IScriptModule).Assembly;

            string? dependency = resolver.ResolveAssemblyToPath(name);

            if (dependency == null)
                return null;

            using var stream = new MemoryStream(File.ReadAllBytes(dependency));

            return LoadFromStream(stream);
        }
    }
}
