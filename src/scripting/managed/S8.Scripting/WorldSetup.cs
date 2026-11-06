namespace S8.Scripting;

// Public, concrete subclasses in the mod assembly are discovered by GameWorld.
// A fresh setup instance is created for each loaded session, before SessionStarted.
public abstract class WorldSetup : IDisposable
{
    public virtual int Order => 0;

    public abstract void Configure(GameWorld world);

    // Release subscriptions or other resources owned by the setup. Entity
    // components and attached events are released by the world itself.
    public virtual void Dispose() { }
}
