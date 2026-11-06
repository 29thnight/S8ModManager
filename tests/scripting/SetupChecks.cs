using S8.Scripting;

// Public fixtures intentionally participate in discovery. They are dormant
// outside this check, including worlds created by the other smoke checks.
public static class SetupFixtureState
{
    public static bool Active, FailWorld;
    public static readonly List<string> Calls = [];
    public static SetupLifetimeCounter Global = null!, Session = null!;
}

public sealed class SetupLifetimeCounter : Component
{
    public int Value, Disables;
    public override void OnDisable() => ++Disables;
}

public sealed class FixtureGameSetup : GameSetup
{
    public override void Configure(GameApplication application)
    {
        if (!SetupFixtureState.Active)
            return;
        SetupFixtureState.Calls.Add("game");
        SetupFixtureState.Global = application.Root.AddComponent<SetupLifetimeCounter>(item => item.Value = 42);
    }

    public override bool Handle(ScriptEvent kind, int owner, int value) =>
        SetupFixtureState.Active && kind == ScriptEvent.Click && owner == 777;
}

public sealed class FixtureWorldSetup : WorldSetup
{
    public override int Order => -10;

    public override void Configure(GameWorld world)
    {
        if (!SetupFixtureState.Active)
            return;
        SetupFixtureState.Calls.Add("world");
        SetupFixtureState.Session = world.EnterScene(GameSceneKind.City, 1)
            .CreateObject("SessionFixture").AddComponent<SetupLifetimeCounter>();
    }

    public override void Dispose()
    {
        if (SetupFixtureState.Active)
            SetupFixtureState.Calls.Add("world.dispose");
    }
}

public sealed class FixtureWorldSetupAfter : WorldSetup
{
    public override int Order => 10;

    public override void Configure(GameWorld world)
    {
        if (!SetupFixtureState.Active)
            return;
        SetupFixtureState.Calls.Add("world.after");
        if (SetupFixtureState.FailWorld)
            throw new InvalidOperationException("Fixture configuration failure");
    }
}

internal static class SetupChecks
{
    public static int Run()
    {
        int checks = 0;
        void Check(bool result, string message)
        {
            if (!result)
                throw new Exception(message);
            ++checks;
        }

        SetupFixtureState.Active = true;
        SetupFixtureState.Calls.Clear();
        try
        {
            using var application = new GameApplication(new ModContext("fixture", Path.GetFullPath("resources")));
            application.Initialize();
            application.Initialize();
            Check(SetupFixtureState.Calls.SequenceEqual(new[] { "game" }) && application.World.SessionId == 0,
                "Global setup is discovered and runs once without a loaded session");
            Check(application.Dispatch(ScriptEvent.Click, 777, 0)
                && !application.Dispatch(ScriptEvent.Click, 778, 0), "Global setup consumes only its own callbacks");

            var world = application.World;
            world.SessionStarted += _ => SetupFixtureState.Calls.Add("started");
            world.BeginSession(1);
            var first = SetupFixtureState.Session;
            Check(SetupFixtureState.Calls.SequenceEqual(new[] { "game", "world", "world.after", "started" }),
                "World setups run by Order before session notification");
            Check(first.GameObject.Application == application && first.GameObject.World == world
                && application.Root.World == null, "World and application objects expose distinct ownership scopes");
            world.BeginSession(1);
            Check(first == SetupFixtureState.Session, "Refreshing a session does not duplicate configured components");

            world.BeginSession(2);
            Check(first.Disables == 1 && first != SetupFixtureState.Session,
                "Replacing a session releases old configured components and configures new ones");
            Check(SetupFixtureState.Global.Value == 42 && SetupFixtureState.Global.Disables == 0,
                "Application settings survive world replacement");

            world.EndSession();
            SetupFixtureState.FailWorld = true;
            bool failed = false;
            try { world.BeginSession(3); }
            catch (InvalidOperationException) { failed = true; }
            Check(failed && world.SessionId == 0 && SetupFixtureState.Session.Disables == 1,
                "Configuration failure releases the partially configured session");
            application.Dispose();
            Check(SetupFixtureState.Global.Disables == 1, "Application unload releases global components once");
        }
        finally
        {
            SetupFixtureState.Active = SetupFixtureState.FailWorld = false;
        }
        return checks;
    }
}
