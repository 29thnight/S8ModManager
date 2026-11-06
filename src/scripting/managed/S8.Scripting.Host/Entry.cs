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

    private sealed class ModSlot(string id, string modDirectory, string gameDirectory, string assembly, string type)
    {
        private AssemblyLoadContext? context;
        private IScriptModule? module;
        private string? loadedIdentity;

        public string Id { get; } = id;

        // Live coding: a rebuilt assembly replaces the running module on the next
        // open, unless the module holds unsaved state.
        public IScriptModule? Resolve(int kind)
        {
            if (!ModDispatch.IsLoadEvent(kind))
                return module;

            string manifest = Path.Combine(modDirectory, "mod.json");
            string identity = File.GetLastWriteTimeUtc(assembly).Ticks + ":" + File.GetLastWriteTimeUtc(manifest).Ticks;

            if (module != null && (identity == loadedIdentity || !module.CanReload))
                return module;

            var next = new ScriptContext(assembly);
            using var stream = new MemoryStream(File.ReadAllBytes(assembly));
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
            loadedIdentity = identity;
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
