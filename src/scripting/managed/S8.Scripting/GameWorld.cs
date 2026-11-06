using System.Runtime.InteropServices;
using System.Runtime.CompilerServices;
using System.Reflection;

namespace S8.Scripting;

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct WorldEntityBinding
{
    public MemoryReference Memory;
    public ushort Id;
    public fixed ushort Personalities[3];
}

public sealed class ProtagonistComponent : Component
{
    public OfficerComponent Officer => GameObject.GetComponent<OfficerComponent>()!;
}

// These are SDK scene categories. Native city/council transition bindings
// will select them after their engine boundaries have been established.
public enum GameSceneKind
{
    City,
    Council,
    Battle
}

public sealed class OfficerComponent : Component
{
    public ushort Id { get; private set; }

    public MemoryReference Memory { get; private set; }

    public bool IsBound => Memory.Address != 0;

    public IReadOnlyList<ushort> PersonalityIds { get; internal set; } = [];

    internal unsafe void Apply(WorldEntityBinding binding)
    {
        Id = binding.Id;
        Memory = binding.Memory;
        PersonalityIds = new ushort[]
        {
            binding.Personalities[0], binding.Personalities[1], binding.Personalities[2]
        };
    }

    internal void Invalidate() => Memory = default;

    public override void OnDisable() => Invalidate();
}

public sealed class CityComponent : Component
{
    public ushort Id { get; private set; }

    public string Name => Native.ReadString(Operation.ReadCityName, Id, checked((long)Memory.Session));

    public MemoryReference Memory { get; private set; }

    public bool IsBound => Memory.Address != 0;

    internal void Apply(WorldEntityBinding binding)
    {
        Id = binding.Id;
        Memory = binding.Memory;
    }

    internal void Invalidate() => Memory = default;

    public override void OnDisable() => Invalidate();
}

// A world belongs to one loaded game session, not to one screen or battle.
public sealed partial class GameWorld : IDisposable
{
    private readonly Dictionary<ushort, GameObject> officers = [];
    private readonly Dictionary<ushort, GameObject> cities = [];
    private Player? player;
    private uint saveGeneration;
    private WorldSaveData? saveData;

    public WorldSaveData SaveData => SessionId != 0
        ? saveData ??= new WorldSaveData(SessionId, saveGeneration)
        : throw new InvalidOperationException("A loaded game session is required.");
    private readonly ConstructorInfo[] setupConstructors;
    private readonly List<WorldSetup> setups = [];

    [MethodImpl(MethodImplOptions.NoInlining)]
    public GameWorld(JewelCatalog? jewelDefinitions = null, Assembly? scriptAssembly = null)
    {
        JewelDefinitions = jewelDefinitions;
        Events = new WorldEventRuntime(this);
        // Collect once from this mod, not from every assembly in the process.
        setupConstructors = (scriptAssembly ?? Assembly.GetCallingAssembly()).GetExportedTypes()
            .Where(type => !type.IsAbstract && !type.ContainsGenericParameters && typeof(WorldSetup).IsAssignableFrom(type))
            .Select(type => type.GetConstructor(Type.EmptyTypes)
                ?? throw new InvalidOperationException($"World setup {type.FullName} requires a public parameterless constructor."))
            .ToArray();
    }

    internal JewelCatalog? JewelDefinitions { get; }

    public GameApplication? Application { get; internal set; }

    public WorldEventRuntime Events { get; }

    public Player GetPlayer()
    {
        RefreshSession();
        if (player == null || Protagonist == null)
            throw new InvalidOperationException("A loaded session with a protagonist is required.");
        return player;
    }

    public ulong SessionId { get; private set; }

    public GameScene? CurrentScene { get; private set; }

    public GameObject? Protagonist { get; private set; }

    public IReadOnlyCollection<GameObject> Officers => officers.Values;

    public IReadOnlyCollection<GameObject> Cities => cities.Values;

    public event Action<ulong>? SessionStarted;

    public event Action<ulong>? SessionEnded;

    public event Action<GameScene>? SceneEntered;

    public event Action<GameScene>? SceneExited;

    public event Action<GameObject?>? ProtagonistChanged;

    public void BeginSession(ulong id, ushort? protagonistId = null)
    {
        ArgumentOutOfRangeException.ThrowIfZero(id);

        if (SessionId == id)
        {
            if (protagonistId is { } current)
                ObserveProtagonist(current);
            return;
        }

        EndSession();
        SessionId = id;
        player = new Player(this);

        bool configured = false;
        try
        {
            if (protagonistId is { } protagonist)
                ObserveProtagonist(protagonist);

            foreach (var constructor in setupConstructors)
                setups.Add((WorldSetup)constructor.Invoke(null));

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
                EndSession();
        }

        SessionStarted?.Invoke(id);
        Events.Publish(WorldEventTriggers.SessionStarted);
    }

    // Read the manager's published snapshot inside a game callback. Access to
    // the cached Protagonist afterwards does not require another native call.
    public unsafe void RefreshSession()
    {
        WorldContext context;

        Native.Call(Operation.GetWorldContext, data: &context, bytes: (uint)sizeof(WorldContext));

        if (SessionId != 0 && (SessionId != context.Epoch || saveGeneration != context.SaveGeneration))
            EndSession();

        saveGeneration = context.SaveGeneration;
        if (context.Epoch != 0 && context.Protagonist != ushort.MaxValue)
            BeginSession(context.Epoch, context.Protagonist);
    }

    public void ObserveProtagonist(ushort id)
    {
        if (Protagonist?.GetComponent<OfficerComponent>()!.Id == id)
            return;

        var next = ResolveOfficer(id);

        if (Protagonist is { } previous)
        {
            previous.RemoveComponent(previous.GetComponent<PlayerJewelsComponent>()!);
            previous.RemoveComponent(previous.GetComponent<ProtagonistComponent>()!);
        }

        Protagonist = next;
        next.AddComponent<ProtagonistComponent>();
        next.AddComponent<PlayerJewelsComponent>();
        ProtagonistChanged?.Invoke(next);
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct WorldContext
    {
        public ulong Epoch;
        public ushort Protagonist, Reserved;
        public uint SaveGeneration;
    }

    public GameObject GetOfficer(ushort id)
    {
        RefreshSession();
        return ResolveOfficer(id);
    }

    private unsafe GameObject ResolveOfficer(ushort id)
    {
        if (SessionId == 0)
            throw new InvalidOperationException("A loaded game session is required.");

        officers.TryGetValue(id, out var gameObject);
        gameObject?.GetComponent<OfficerComponent>()!.Invalidate();
        WorldEntityBinding binding;

        if (Native.Call(Operation.ResolveOfficer, id, checked((long)SessionId), &binding,
                (uint)sizeof(WorldEntityBinding)) == 0)
            throw new KeyNotFoundException($"Officer {id} does not exist in the current game session.");

        if (gameObject == null)
        {
            gameObject = new GameObject($"Officer:{id}") { World = this };
            var resolved = binding;
            gameObject.AddComponent<OfficerComponent>(component => component.Apply(resolved));
            gameObject.AddComponent<OfficerStatsComponent>();
            gameObject.AddComponent<OfficerFameComponent>();
            gameObject.AddComponent<OfficerTrainingComponent>();
            gameObject.AddComponent<OfficerTraitsComponent>();
            gameObject.AddComponent<OfficerAffiliationComponent>();
            gameObject.AddComponent<OfficerActionsComponent>();
            officers.Add(id, gameObject);
        }
        else
            gameObject.GetComponent<OfficerComponent>()!.Apply(binding);

        return gameObject;
    }

    public unsafe GameObject GetCity(ushort id)
    {
        RefreshSession();
        if (SessionId == 0)
            throw new InvalidOperationException("A loaded game session is required.");

        cities.TryGetValue(id, out var gameObject);
        gameObject?.GetComponent<CityComponent>()!.Invalidate();
        WorldEntityBinding binding;

        if (Native.Call(Operation.ResolveCity, id, checked((long)SessionId), &binding,
                (uint)sizeof(WorldEntityBinding)) == 0)
            throw new KeyNotFoundException($"City {id} does not exist in the current game session.");

        if (gameObject == null)
        {
            gameObject = new GameObject($"City:{id}") { World = this };
            var resolved = binding;
            gameObject.AddComponent<CityComponent>(component => component.Apply(resolved));
            gameObject.AddComponent<CityAdministrationComponent>();
            gameObject.AddComponent<CityAffiliationComponent>();
            gameObject.AddComponent<CityAiComponent>();
            cities.Add(id, gameObject);
        }
        else
            gameObject.GetComponent<CityComponent>()!.Apply(binding);

        return gameObject;
    }

    public GameObject? FindOfficer(ushort id) => officers.GetValueOrDefault(id);

    public GameObject? FindCity(ushort id) => cities.GetValueOrDefault(id);

    public GameScene EnterScene(GameSceneKind kind, ulong id)
    {
        if (SessionId == 0)
            throw new InvalidOperationException("A loaded game session is required.");

        if (CurrentScene is { } current && current.Kind == kind && current.Id == id)
            return current;

        LeaveScene();
        var next = new GameScene(this, kind, id);

        CurrentScene = next;
        SceneEntered?.Invoke(next);
        Events.Publish(WorldEventTriggers.SceneEntered, next);
        return next;
    }

    public void LeaveScene()
    {
        var previous = CurrentScene;

        CurrentScene = null;
        previous?.Dispose();

        if (previous != null)
        {
            SceneExited?.Invoke(previous);
            if (SessionId != 0)
                Events.Publish(WorldEventTriggers.SceneExited, previous);
        }
    }

    public void EndSession()
    {
        if (SessionId == 0)
            return;

        ulong previousSession = SessionId;
        bool hadProtagonist = Protagonist != null;

        saveData?.Invalidate();
        saveData = null;
        SessionId = 0;
        Protagonist = null;
        for (int index = setups.Count - 1; index >= 0; --index)
            setups[index].Dispose();
        setups.Clear();
        Events.Clear();
        player?.Invalidate();
        player = null;
        LeaveScene();

        foreach (var gameObject in officers.Values)
            gameObject.Dispose();

        foreach (var gameObject in cities.Values)
            gameObject.Dispose();

        officers.Clear();
        cities.Clear();
        if (hadProtagonist)
            ProtagonistChanged?.Invoke(null);
        SessionEnded?.Invoke(previousSession);
    }

    public void Dispose() => EndSession();
}

// References to world entities do not transfer ownership to the scene.
public sealed class GameScene : IDisposable
{
    private readonly HashSet<GameObject> entities = [];
    private readonly HashSet<GameObject> owned = [];
    private bool disposed;

    internal GameScene(GameWorld world, GameSceneKind kind, ulong id)
    {
        World = world;
        Kind = kind;
        Id = id;
    }

    public GameWorld World { get; }

    public GameSceneKind Kind { get; }

    public ulong Id { get; }

    public IReadOnlyCollection<GameObject> Entities => entities;

    public IReadOnlyCollection<GameObject> OwnedObjects => owned;

    internal event Action? Disposing;

    public GameObject CreateObject(string name)
    {
        ObjectDisposedException.ThrowIf(disposed, this);
        var gameObject = new GameObject(name) { World = World, Scene = this };

        owned.Add(gameObject);
        entities.Add(gameObject);
        return gameObject;
    }

    public void Track(GameObject gameObject)
    {
        ObjectDisposedException.ThrowIf(disposed, this);

        if (gameObject.World != World || (gameObject.Scene != null && gameObject.Scene != this))
            throw new ArgumentException("The object belongs to a different world or scene.", nameof(gameObject));

        entities.Add(gameObject);
    }

    public void Untrack(GameObject gameObject)
    {
        if (owned.Remove(gameObject))
            gameObject.Dispose();

        entities.Remove(gameObject);
    }

    public void Dispose()
    {
        if (disposed)
            return;

        if (World.CurrentScene == this)
        {
            World.LeaveScene();
            return;
        }

        disposed = true;
        Disposing?.Invoke();
        Disposing = null;

        foreach (var gameObject in owned)
            gameObject.Dispose();

        owned.Clear();
        entities.Clear();
    }
}
