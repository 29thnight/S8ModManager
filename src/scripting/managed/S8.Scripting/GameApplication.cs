using System.Reflection;
using System.Runtime.CompilerServices;

namespace S8.Scripting;

// One mod load owns this application. A loaded save is a separate, shorter scope.
public sealed class GameApplication : IDisposable
{
    private readonly ConstructorInfo[] setupConstructors;
    private readonly List<GameSetup> setups = [];
    private readonly List<GameObject> objects = [];
    private bool initialized, disposed;

    [MethodImpl(MethodImplOptions.NoInlining)]
    public GameApplication(ModContext context, Assembly? scriptAssembly = null)
    {
        Context = context;
        var assembly = scriptAssembly ?? Assembly.GetCallingAssembly();
        setupConstructors = assembly.GetExportedTypes()
            .Where(type => !type.IsAbstract && !type.ContainsGenericParameters && typeof(GameSetup).IsAssignableFrom(type))
            .Select(type => type.GetConstructor(Type.EmptyTypes)
                ?? throw new InvalidOperationException($"Game setup {type.FullName} requires a public parameterless constructor."))
            .ToArray();
        // Optional: only mods that use JewelComponent ship this catalog.
        string jewels = Path.Combine(context.AssetDirectory, "jewel_definitions.json");
        World = new GameWorld(File.Exists(jewels) ? JewelCatalog.Load(jewels) : null, assembly)
        {
            Application = this
        };
        Root = new GameObject("Application") { Application = this };
        objects.Add(Root);
    }

    public ModContext Context { get; }

    public GameWorld World { get; }

    public GameObject Root { get; }

    public void Initialize()
    {
        ObjectDisposedException.ThrowIf(disposed, this);
        if (initialized)
            return;
        initialized = true;

        bool configured = false;
        try
        {
            foreach (var constructor in setupConstructors)
                setups.Add((GameSetup)constructor.Invoke(null));

            setups.Sort((left, right) =>
            {
                int order = left.Order.CompareTo(right.Order);
                return order != 0 ? order : StringComparer.Ordinal.Compare(left.GetType().FullName, right.GetType().FullName);
            });

            foreach (var setup in setups)
                setup.Configure(this);
            configured = true;
        }
        finally
        {
            if (!configured)
                Dispose();
        }
    }

    public GameObject CreateObject(string name)
    {
        ObjectDisposedException.ThrowIf(disposed, this);
        var gameObject = new GameObject(name) { Application = this };
        objects.Add(gameObject);
        return gameObject;
    }

    public bool Dispatch(ScriptEvent kind, int owner, int value)
    {
        ObjectDisposedException.ThrowIf(disposed, this);
        foreach (var setup in setups)
            if (setup.Handle(kind, owner, value))
                return true;
        return false;
    }

    public void Dispose()
    {
        if (disposed)
            return;
        disposed = true;
        World.Dispose();
        for (int index = setups.Count - 1; index >= 0; --index)
            setups[index].Dispose();
        setups.Clear();
        for (int index = objects.Count - 1; index >= 0; --index)
            objects[index].Dispose();
        objects.Clear();
    }
}
