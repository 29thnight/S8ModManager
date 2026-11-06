namespace S8.Scripting;

// The base class a mod derives from. The SDK owns event routing: a mod registers
// menus and window rows with callbacks, and dialog events reach the UiScene that
// created the dialog without any forwarding code in the mod.
public abstract class ScriptMod : IScriptModule
{
    private readonly Dictionary<int, Action> menuActions = [];
    private readonly Dictionary<int, WindowRow> windowRows = [];
    private readonly Dictionary<int, WindowRow> openWindows = [];
    private MenuExtensionComponent? menus;
    private GameApplication? application;
    private int nextAction;

    public ModContext Context { get; private set; } = null!;

    public GameApplication Application => application
        ?? throw new InvalidOperationException("The mod is not loaded yet.");

    public GameWorld World => Application.World;

    // Return false while the mod holds unsaved state that a live reload would lose.
    public virtual bool CanReload => true;

    // Register menus and window rows and load mod assets here.
    protected abstract void OnLoad();

    // The whole native dialog stack closed. Release per-open state here.
    protected virtual void OnUiClosed() { }

    // Events the SDK does not route (battle, activity and similar notifications).
    protected virtual void OnEvent(ScriptEvent kind, int owner, int value) { }

    protected virtual void OnUnload() { }

    protected void AddMenu(MenuSurface surface, string label, Action clicked, int templateId = 5, int insertAfter = 5)
    {
        int action = nextAction++;
        menuActions.Add(action, clicked);
        Menus.Add(surface, action, label, templateId, insertAfter);
    }

    // clicked receives the game window id; closing receives it with whether the window was accepted.
    protected void AddWindowRow(int slot, string label, string button, Action<int> clicked,
        Action<int, bool>? closing = null)
    {
        int action = nextAction++;
        windowRows.Add(action, new WindowRow(clicked, closing));
        Menus.AddWindowRow(slot, action, label, button);
    }

    private MenuExtensionComponent Menus =>
        menus ??= Application.CreateObject("ModMenus").AddComponent<MenuExtensionComponent>();

    void IScriptModule.Initialize(ModContext context)
    {
        Context = context;
        application = new GameApplication(context, GetType().Assembly);
        bool loaded = false;
        try
        {
            OnLoad();
            application.Initialize();
            World.RefreshSession();
            loaded = true;
        }
        finally
        {
            if (!loaded)
                Dispose();
        }
    }

    void IScriptModule.Dispatch(ScriptEvent kind, int owner, int value)
    {
        World.RefreshSession();
        if (World.DispatchActivity(kind, owner, value) || Application.Dispatch(kind, owner, value)
            || World.Events.DispatchUi(kind, owner, value))
            return;

        switch (kind)
        {
            case ScriptEvent.MenuClick:
                if (menuActions.TryGetValue(value, out var menu))
                    menu();
                return;

            case ScriptEvent.WindowClick:
                if (windowRows.TryGetValue(value, out var row))
                {
                    openWindows[owner] = row;
                    row.Clicked(owner);
                }
                return;

            case ScriptEvent.WindowClosing:
                if (openWindows.Remove(owner, out var closing))
                    closing.Closing?.Invoke(owner, value == 1);
                return;

            case ScriptEvent.Closed:
                OnUiClosed();
                return;

            case ScriptEvent.BattleSessionEnded:
                OnEvent(kind, owner, value);
                if (World.SessionId != 0)
                    World.Events.Signal(WorldEventTriggers.BattleEnded);
                return;
        }

        if (!UiScene.Route(kind, owner, value))
            OnEvent(kind, owner, value);
    }

    public void Dispose()
    {
        try
        {
            OnUnload();
        }
        finally
        {
            application?.Dispose();
            application = null;
            menus = null;
        }
    }

    private sealed record WindowRow(Action<int> Clicked, Action<int, bool>? Closing);
}
