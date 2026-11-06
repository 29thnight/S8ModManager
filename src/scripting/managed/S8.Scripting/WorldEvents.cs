namespace S8.Scripting;

public static class WorldEventTriggers
{
    public const string SessionStarted = "world.session.started";
    public const string SceneEntered = "world.scene.entered";
    public const string SceneExited = "world.scene.exited";
    public const string BattleTurnStarted = "battle.turn.started";
    public const string BattleUnitActionEnded = "battle.unit.action.ended";
    public const string BattleTurnEnded = "battle.turn.ended";
    public const string BattleEnded = "battle.ended";
    public const string DomesticActivityCompleted = "domestic.activity.completed";
    public const string TalkCompleted = "communicate.talk.completed";
    public const string MonthChanged = "world.month.changed";
    public const string CouncilEntered = "council.entered";
}

// One instance belongs to one loaded session. Fields in derived events retain
// story progress between callbacks; they are not serialized into game saves.
public abstract class WorldEvent(string id, params string[] triggers)
{
    public string Id { get; } = id;

    public IReadOnlyList<string> Triggers { get; } = Array.AsReadOnly((string[])triggers.Clone());

    public GameWorld? World { get; internal set; }

    public GameObject? Owner { get; internal set; }

    public bool Enabled { get; set; } = true;

    public bool Once { get; init; }

    public int ExecutionCount { get; internal set; }

    public bool AwaitingChoice { get; internal set; }

    internal bool Running;

    internal bool Attached;

    public virtual bool CanExecute(WorldEventContext context) => true;

    public abstract void Execute(WorldEventContext context);

    public virtual void OnAttached() { }

    public virtual void OnDetached() { }
}

public sealed class ScriptedWorldEvent(
    string id, string trigger,
    Func<WorldEventContext, bool> condition,
    Action<WorldEventContext> effect) : WorldEvent(id, trigger)
{
    public override bool CanExecute(WorldEventContext context) => condition(context);

    public override void Execute(WorldEventContext context) => effect(context);
}

public static class EventEffects
{
    public static unsafe void Say(WorldEventContext context, ushort officerId, string text)
    {
        ulong session = context.World.SessionId;
        fixed (char* speech = text)
        {
            if (Native.Call(Operation.ShowOfficerDialogue, officerId, checked((long)session),
                    speech, checked((uint)((text.Length + 1) * sizeof(char)))) == 0)
                throw new KeyNotFoundException($"Officer {officerId} does not exist in the current session.");
        }
    }

    public static uint RestoreSoldiers(WorldEventContext context, ushort officerId, uint amount = 0)
    {
        var membership = context.Battle?.FindOfficer(officerId)?.GetComponent<BattleMembershipComponent>()
            ?? throw new InvalidOperationException("The officer is not present in this battle callback.");
        return membership.Unit.RestoreSoldiers(amount);
    }

    public static void ShowPersonality(WorldEventContext context, ushort officerId,
        ushort personalityId, string speech)
    {
        var membership = context.Battle?.FindOfficer(officerId)?.GetComponent<BattleMembershipComponent>()
            ?? throw new InvalidOperationException("The officer is not present in this battle callback.");
        membership.Unit.ShowPersonality(officerId, personalityId, speech);
    }
}

// Conditions can use arbitrary C# and component state; these combinators do
// not poll the game or impose a fixed editor condition/effect vocabulary.
public static class EventConditions
{
    public static Func<WorldEventContext, bool> All(params Func<WorldEventContext, bool>[] conditions) =>
        context => conditions.All(condition => condition(context));

    public static Func<WorldEventContext, bool> Any(params Func<WorldEventContext, bool>[] conditions) =>
        context => conditions.Any(condition => condition(context));

    public static Func<WorldEventContext, bool> Not(Func<WorldEventContext, bool> condition) =>
        context => !condition(context);
}

// Contexts expire when Execute/CanExecute returns. In particular, a dialogue
// continuation must not retain a native battle token from an earlier callback.
public sealed class WorldEventContext
{
    private bool active = true;
    private readonly WorldEventRuntime runtime;
    private readonly WorldEvent source;
    private readonly BattleWorld? battle;
    private readonly ulong session;

    internal WorldEventContext(WorldEventRuntime runtime, WorldEvent source,
        string trigger, object? payload, BattleWorld? battle)
    {
        this.runtime = runtime;
        this.source = source;
        this.battle = battle;
        session = runtime.World.SessionId;
        Trigger = trigger;
        Payload = payload;
    }

    public string Trigger { get; }

    public object? Payload { get; }

    public GameWorld World { get { RequireActive(); return runtime.World; } }

    public GameObject? Owner { get { RequireActive(); return source.Owner; } }

    public BattleWorld? Battle { get { RequireActive(); return battle; } }

    public void Signal(string name, object? payload = null)
    {
        RequireActive();
        runtime.Signal(name, payload);
    }

    // The callback receives a NEW context without battle access. Use it to
    // change story state, signal another event, or open another native dialog.
    public void Choose(string title, IReadOnlyList<UiRow> choices,
        Action<WorldEventContext, int> completed)
    {
        RequireActive();
        runtime.Choose(source, title, choices, completed);
    }

    public void Message(string title, string text, Action<WorldEventContext, int>? completed = null)
    {
        RequireActive();
        runtime.Choose(source, title, [new UiRow(0, "OK", text)], completed);
    }

    public void ChooseMenu(string title, string caption, IReadOnlyList<UiRow> choices,
        Action<WorldEventContext, int> completed)
    {
        RequireActive();
        runtime.Choose(source, title, choices, completed, DialogKind.Form, caption);
    }

    public void ChooseMenu(string title, string caption, IReadOnlyList<UiRow> choices,
        Action<WorldEventContext, int> completed, Action<DialogComponent> configure)
    {
        RequireActive();
        runtime.Choose(source, title, choices, completed, DialogKind.Form, caption, configure: configure);
    }

    // This selector uses the council's map and must be opened inside that scene.
    public void ChooseCity(string title, ushort sourceCityId, IReadOnlyList<ushort> cityIds,
        Action<WorldEventContext, int> completed)
    {
        RequireActive();
        var rows = cityIds.Select(id => new UiRow(id, "", "")).ToArray();
        runtime.Choose(source, title, rows, completed, DialogKind.CityMap, sourceCityId: sourceCityId);
    }

    public unsafe void OpenCouncilRoom(bool hidePalaceControls, Action<WorldEventContext>? ready = null)
    {
        RequireActive();
        Native.Call(Operation.OpenCouncilRoom, argument: hidePalaceControls ? 1 : 0);
        runtime.OnCouncilRoomReady(source, session, ready);
    }

    public unsafe void CloseCouncil()
    {
        RequireActive();
        Native.Call(Operation.CloseCouncilRoom);
    }

    private void RequireActive()
    {
        if (!active || !source.Attached || runtime.World.SessionId != session)
            throw new InvalidOperationException("The event callback context has expired.");
    }

    internal void Expire() => active = false;
}

public sealed class WorldEventRuntime
{
    private readonly Dictionary<string, WorldEvent> events = new(StringComparer.Ordinal);
    private readonly Dictionary<string, List<WorldEvent>> listeners = new(StringComparer.Ordinal);
    private readonly UiScene ui = new();
    private readonly Queue<Action> uiContinuations = [];
    private readonly Queue<(string Trigger, object? Payload)> signals = [];
    private bool dispatching;
    private (WorldEvent Source, ulong Session, Action<WorldEventContext> Ready)? councilRoomReady;

    internal void OnCouncilRoomReady(WorldEvent source, ulong session, Action<WorldEventContext>? ready)
    {
        councilRoomReady = ready == null ? null : (source, session, ready);
    }

    internal WorldEventRuntime(GameWorld world) => World = world;

    public GameWorld World { get; }

    public IReadOnlyCollection<WorldEvent> Events => events.Values;

    public T Attach<T>(T instance, GameObject? owner = null) where T : WorldEvent
    {
        if (World.SessionId == 0)
            throw new InvalidOperationException("Attach events inside a loaded session.");
        ArgumentException.ThrowIfNullOrWhiteSpace(instance.Id);
        if (instance.Attached || instance.World != null || instance.Triggers.Count == 0 ||
            instance.Triggers.Any(string.IsNullOrWhiteSpace) ||
            instance.Triggers.Distinct(StringComparer.Ordinal).Count() != instance.Triggers.Count ||
            (owner != null && (owner.World != World || owner.Scene != null)))
            throw new ArgumentException("An event needs unique triggers and a world-owned entity.", nameof(instance));

        events.Add(instance.Id, instance);
        instance.World = World;
        instance.Owner = owner;
        instance.Attached = true;

        foreach (string trigger in instance.Triggers)
        {
            if (!listeners.TryGetValue(trigger, out var group))
                listeners.Add(trigger, group = []);
            group.Add(instance);
        }

        instance.OnAttached();
        return instance;
    }

    public bool Detach(string id)
    {
        if (!events.Remove(id, out var instance))
            return false;

        foreach (string trigger in instance.Triggers)
        {
            var group = listeners[trigger];
            group.Remove(instance);
            if (group.Count == 0)
                listeners.Remove(trigger);
        }

        instance.Attached = false;
        instance.OnDetached();
        return true;
    }

    public void Signal(string trigger, object? payload = null)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(trigger);
        if (World.SessionId == 0)
            throw new InvalidOperationException("A loaded session is required.");
        Publish(trigger, payload);
    }

    // Signals raised by handlers are drained afterwards, without recursive
    // dispatch. A bounded drain also stops A->B->A signal loops on the game thread.
    internal void Publish(string trigger, object? payload = null, BattleWorld? battle = null)
    {
        if (dispatching)
        {
            signals.Enqueue((trigger, payload));
            return;
        }

        dispatching = true;
        ulong session = World.SessionId;
        try
        {
            int dispatched = 0;
            do
            {
                if (++dispatched > 256)
                    throw new InvalidOperationException("An event signal chain exceeded 256 deliveries.");
                Deliver(trigger, payload, battle, session);

                // Queued signals carry managed data, never an expiring native scope.
                battle = null;
                if (World.SessionId != session || !signals.TryDequeue(out var next))
                    break;
                (trigger, payload) = next;
            } while (true);
        }
        finally
        {
            signals.Clear();
            dispatching = false;
        }
    }

    private void Deliver(string trigger, object? payload, BattleWorld? battle, ulong session)
    {
        if (listeners.TryGetValue(trigger, out var group))
        {
            foreach (var instance in group.ToArray())
            {
                if (World.SessionId != session)
                    return;
                if (!instance.Attached || !instance.Enabled || instance.Running || instance.AwaitingChoice ||
                    (instance.Once && instance.ExecutionCount != 0))
                    continue;

                var context = new WorldEventContext(this, instance, trigger, payload, battle);
                instance.Running = true;
                try
                {
                    if (instance.CanExecute(context) && instance.Attached)
                    {
                        instance.Execute(context);
                        ++instance.ExecutionCount;
                    }
                }
                finally
                {
                    context.Expire();
                    instance.Running = false;
                }
            }
        }
    }

    public void DispatchBattle(BattleWorld battle)
    {
        if (battle.World != World || battle.CurrentEvent == null)
            throw new ArgumentException("An active battle callback in this world is required.", nameof(battle));
        string trigger = battle.CurrentEvent.Kind switch
        {
            ScriptEvent.BattleTurnStarted => WorldEventTriggers.BattleTurnStarted,
            ScriptEvent.BattleUnitActionEnded => WorldEventTriggers.BattleUnitActionEnded,
            ScriptEvent.BattleTurnEnded => WorldEventTriggers.BattleTurnEnded,
            _ => throw new ArgumentOutOfRangeException(nameof(battle))
        };

        if (dispatching)
        {
            // Native presentation can pump a nested game event. Deliver it
            // while its native scope is still alive; running handlers are skipped.
            Deliver(trigger, null, battle, World.SessionId);
            return;
        }
        Publish(trigger, battle: battle);
    }

    internal void Choose(WorldEvent source, string title, IReadOnlyList<UiRow> choices,
        Action<WorldEventContext, int>? completed, DialogKind kind = DialogKind.List,
        string caption = "항목을 선택해 주십시오", ushort sourceCityId = 0, Action<DialogComponent>? configure = null)
    {
        if (source.AwaitingChoice)
            throw new InvalidOperationException("The event already has a pending choice.");
        ulong session = World.SessionId;
        var dialog = ui.Create($"Event:{source.Id}");
        int menuChoice = -1;
        dialog.Selected = action =>
        {
            menuChoice = action;
            dialog.Close();
        };
        dialog.Clicked = action =>
        {
            menuChoice = action;
            dialog.Close();
        };
        dialog.Closing = (accepted, choice) =>
        {
            // Closing runs before the native frame is popped. Opening its successor
            // is admitted only by Resume(owner=0), after native cleanup has finished.
            uiContinuations.Enqueue(() =>
            {
                source.AwaitingChoice = false;
                if (!source.Attached || !source.Enabled || World.SessionId != session || completed == null)
                    return;

                var context = new WorldEventContext(this, source, "ui.choice.completed", null, null);
                try
                {
                    int result = kind == DialogKind.Form ? menuChoice : choice;
                    completed(context, accepted ? result : -1);
                }
                finally
                {
                    context.Expire();
                }
            });
        };
        dialog.Show(kind, title, choices, caption: caption, sourceCityId: sourceCityId);
        source.AwaitingChoice = true;
        configure?.Invoke(dialog);
    }

    public bool DispatchUi(ScriptEvent kind, int owner, int value)
    {
        if (kind == ScriptEvent.CouncilRoomReady)
        {
            var pending = councilRoomReady;
            councilRoomReady = null;
            if (pending is { } request && request.Source.Attached && request.Source.Enabled &&
                World.SessionId == request.Session)
            {
                var context = new WorldEventContext(this, request.Source, "ui.council.ready", null, null);
                try
                {
                    request.Ready(context);
                }
                finally
                {
                    context.Expire();
                }
            }
            return true;
        }
        if (kind == ScriptEvent.Resume && owner == 0 && uiContinuations.Count != 0)
        {
            int count = uiContinuations.Count;
            for (int index = 0; index < count; ++index)
                uiContinuations.Dequeue()();
            return true;
        }
        if (!ui.ContainsOwner(owner))
            return false;
        ui.Dispatch(kind, owner, value);
        return true;
    }

    internal void Clear()
    {
        ui.Dispose();
        councilRoomReady = null;
        uiContinuations.Clear();
        signals.Clear();
        foreach (string id in events.Keys.ToArray())
            Detach(id);
    }
}
