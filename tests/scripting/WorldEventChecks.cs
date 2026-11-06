using S8.Scripting;

internal static class WorldEventChecks
{
    private sealed class StoryEvent() : WorldEvent("story", "story.advance")
    {
        public int Progress;
        public int DetachCount;

        public override void Execute(WorldEventContext context) => ++Progress;

        public override void OnDetached() => ++DetachCount;
    }

    public static int Run()
    {
        int checks = 0;
        void Check(bool value, string message)
        {
            if (!value)
                throw new Exception(message);
            ++checks;
        }

        using var world = new GameWorld();
        StoryEvent? story = null;
        world.SessionStarted += _ => story = world.Events.Attach(new StoryEvent());
        world.BeginSession(1);
        world.Events.Signal("story.advance");
        world.EnterScene(GameSceneKind.City, 1);
        world.EnterScene(GameSceneKind.Council, 1);
        world.Events.Signal("story.advance");
        Check(story!.Progress == 2, "Story state survives city/council transitions");

        var previous = story;
        world.BeginSession(2);
        Check(previous.DetachCount == 1 && story != previous && story!.Progress == 0,
            "Loading replaces session event instances and their state");

        int calls = 0;
        WorldEventContext? retained = null;
        bool eligible = false;
        world.Events.Attach(new ScriptedWorldEvent("once", "check",
            EventConditions.All(_ => eligible, EventConditions.Not(_ => false)),
            context => { retained = context; ++calls; }) { Once = true });
        world.Events.Signal("check");
        eligible = true;
        world.Events.Signal("check");
        world.Events.Signal("check");
        Check(calls == 1, "A conditional once event waits for eligibility and runs only once");
        bool expired = false;
        try { retained!.Signal("late"); }
        catch (InvalidOperationException) { expired = true; }
        Check(expired, "An event cannot reuse its expired callback context");

        var order = new List<int>();
        world.Events.Attach(new ScriptedWorldEvent("first", "chain", _ => true, context =>
        {
            order.Add(1);
            context.Signal("followup", 7);
            order.Add(2);
        }));
        world.Events.Attach(new ScriptedWorldEvent("second", "followup", _ => true, context =>
        {
            Check(context.Battle == null && (int)context.Payload! == 7,
                "Signals carry managed payloads without native battle scope");
            order.Add(3);
        }));
        world.Events.Signal("chain");
        Check(order.SequenceEqual(new[] { 1, 2, 3 }), "Signals run after the current handler");

        int skipped = 0;
        world.Events.Attach(new ScriptedWorldEvent("remove", "remove", _ => true,
            _ => world.Events.Detach("removed")));
        world.Events.Attach(new ScriptedWorldEvent("removed", "remove", _ => true, _ => ++skipped));
        world.Events.Signal("remove");
        Check(skipped == 0, "Detaching during dispatch prevents the removed event from running");

        world.Events.Attach(new ScriptedWorldEvent("loop", "loop", _ => true, context => context.Signal("loop")));
        bool bounded = false;
        try { world.Events.Signal("loop"); }
        catch (InvalidOperationException) { bounded = true; }
        Check(bounded, "A cyclic event chain cannot block the game thread indefinitely");
        world.Events.Detach("loop");
        world.Events.Signal("story.advance");
        Check(story!.Progress == 1, "Dispatch remains usable after rejecting a cyclic chain");

        world.EndSession();
        Check(world.Events.Events.Count == 0 && story.DetachCount == 1,
            "Session exit releases every event once");
        return checks;
    }
}
