namespace S8.Scripting;

// Application scope: title/scenario/editor services survive world replacement.
public abstract class GameSetup : IDisposable
{
    public virtual int Order => 0;

    public abstract void Configure(GameApplication application);

    // Return true only when this setup owns and consumes the callback.
    public virtual bool Handle(ScriptEvent kind, int owner, int value) => false;

    public virtual void Dispose() { }
}
