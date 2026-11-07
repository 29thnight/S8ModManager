namespace S8.Scripting;

// Runs registered battle effects. ScriptMod owns one and routes the battle events
// to it, so registering an effect and binding it to a trait is all a mod writes.
// The host attaches each effect once per (officer, trait, effect), applies an
// effect at most once per unit per event, and detaches effects whose trait left.
public sealed class BattleEffects : IDisposable
{
    private readonly Dictionary<ushort, List<string>> fixedBindings = [];
    private readonly Dictionary<(ushort Officer, ushort Personality, string Effect), ScriptedTraitComponent> attached = [];
    private Func<ushort, IReadOnlyList<string?>?>? bindingSource;
    private BattleWorld? battle;

    internal BattleEffects(GameWorld world)
    {
        World = world;
        world.SessionEnded += OnSessionEnded;
    }

    public GameWorld World { get; }

    public ScriptEffectRegistry Registry { get; } = new();

    // The current battle, available while an effect runs.
    public BattleWorld Battle => battle ??= new BattleWorld(World);

    public IEnumerable<ScriptEffectDefinition> Definitions => Registry.Definitions;

    public void Register<T>(ScriptEffectDefinition definition)
        where T : ScriptedTraitComponent, new() => Registry.Register<T>(definition);

    public void Register(ScriptEffectDefinition definition,
        Action<ScriptedTraitComponent>? onTurnStart = null,
        Action<ScriptedTraitComponent>? onUnitActionEnd = null,
        Action<ScriptedTraitComponent>? onTurnEnd = null) =>
        Registry.Register(definition, onTurnStart, onUnitActionEnd, onTurnEnd);

    // An officer holding this trait (personality ID 1..254) runs the effect.
    public void Bind(ushort personalityId, string effectId)
    {
        if (!Registry.Contains(effectId))
            throw new ArgumentException($"Effect {effectId} is not registered.", nameof(effectId));

        if (!fixedBindings.TryGetValue(personalityId, out var effects))
            fixedBindings.Add(personalityId, effects = []);

        if (!effects.Contains(effectId))
            effects.Add(effectId);
    }

    // For mods that keep trait-to-effect bindings elsewhere (a file the user edits).
    // Read on every battle event, so changes apply from the next event.
    public void UseBindings(Func<ushort, IReadOnlyList<string?>?> source) => bindingSource = source;

    private IEnumerable<string> EffectsOf(ushort personality)
    {
        if (fixedBindings.TryGetValue(personality, out var fixedEffects))
            foreach (string effect in fixedEffects)
                yield return effect;

        if (bindingSource?.Invoke(personality) is { } slots)
            foreach (string? effect in slots)
                if (effect != null && Registry.Contains(effect))
                    yield return effect;
    }

    internal void Dispatch(ScriptEvent kind)
    {
        var current = Battle;
        try
        {
            current.Refresh();
            Attach();

            foreach (var unitObject in current.EventUnits)
            {
                var unit = unitObject.GetComponent<BattleUnitComponent>()!;
                var applied = new HashSet<string>();

                foreach (var officer in unit.Officers)
                    foreach (ushort personality in officer.PersonalityIds)
                        foreach (string effect in EffectsOf(personality))
                        {
                            if (!Registry.Handles(effect, kind) || !applied.Add(effect))
                                continue;

                            Run(attached[(officer.Id, personality, effect)], kind);
                        }
            }

            World.Events.DispatchBattle(current);
        }
        finally
        {
            current.EndEvent();
        }
    }

    private void Attach()
    {
        var retained = new HashSet<(ushort Officer, ushort Personality, string Effect)>();

        foreach (var owner in World.Officers)
        {
            var officer = owner.GetComponent<OfficerComponent>()!;

            foreach (ushort personality in officer.PersonalityIds)
                foreach (string effect in EffectsOf(personality))
                {
                    var key = (officer.Id, personality, effect);

                    if (retained.Add(key) && !attached.ContainsKey(key))
                        attached.Add(key, Registry.Attach(effect, owner, personality));
                }
        }

        foreach (var key in attached.Keys.Where(key => !retained.Contains(key)).ToArray())
        {
            var component = attached[key];

            component.GameObject.RemoveComponent(component);
            attached.Remove(key);
        }
    }

    private static void Run(ScriptedTraitComponent component, ScriptEvent kind)
    {
        switch (kind)
        {
            case ScriptEvent.BattleTurnStarted when component is IBattleTurnStartEffect start:
                start.OnBattleTurnStarted();
                break;

            case ScriptEvent.BattleUnitActionEnded when component is IBattleUnitActionEndEffect action:
                action.OnBattleUnitActionEnded();
                break;

            case ScriptEvent.BattleTurnEnded when component is IBattleTurnEndEffect end:
                end.OnBattleTurnEnded();
                break;
        }
    }

    internal void EndBattle() => battle?.Dispose();

    private void OnSessionEnded(ulong session) => attached.Clear();

    public void Dispose()
    {
        World.SessionEnded -= OnSessionEnded;
        battle?.Dispose();

        foreach (var component in attached.Values)
            component.GameObject.RemoveComponent(component);

        attached.Clear();
    }
}
