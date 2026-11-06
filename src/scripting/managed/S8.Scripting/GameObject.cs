namespace S8.Scripting;

public sealed class GameObject : IDisposable
{
    private readonly List<Component> components = [];
    private bool disposed;
    private GameApplication? application;

    public string Name { get; }

    public GameApplication? Application
    {
        get => application ?? World?.Application;
        internal set => application = value;
    }

    public GameWorld? World { get; internal set; }

    public GameScene? Scene { get; internal set; }

    internal GameObject(string name) => Name = name;

    public T AddComponent<T>(Action<T>? initialize = null)
        where T : Component, new()
    {
        ObjectDisposedException.ThrowIf(disposed, this);
        var component = new T
        {
            GameObject = this
        };

        initialize?.Invoke(component);
        components.Add(component);
        component.OnEnable();
        return component;
    }

    public T? GetComponent<T>()
        where T : Component => components.OfType<T>().FirstOrDefault();

    public T[] GetComponents<T>()
        where T : class => components.OfType<T>().ToArray();

    public bool RemoveComponent(Component component)
    {
        if (!components.Remove(component))
            return false;

        component.OnDisable();
        return true;
    }

    public void Dispose()
    {
        if (disposed)
            return;

        disposed = true;

        for (int i = components.Count - 1; i >= 0; --i)
            components[i].OnDisable();

        components.Clear();
    }
}

public abstract class Component
{
    public GameObject GameObject { get; internal set; } = null!;

    public virtual void OnEnable()
    {
    }

    public virtual void OnDisable()
    {
    }
}

public sealed record ModContext(string GameDirectory, string AssetDirectory);
public interface IScriptModule : IDisposable
{
    bool CanReload => true;

    void Initialize(ModContext context);

    void Dispatch(ScriptEvent kind, int owner, int value);
}
