using System.Runtime.CompilerServices;

[assembly: InternalsVisibleTo("S8.LuaHost")]
[assembly: InternalsVisibleTo("S8.Scripting.Smoke")]

namespace S8.Scripting;

// Runs this mod's scripted effects. An effect is defined once (name, description,
// what it does at each battle moment) and published to EffectCatalog; which traits
// hold it, and with what value, comes from the trait slots the editor supplies.
// Authoring is open to Lua only, so Register and Bind stay internal.
public sealed class BattleEffects : IDisposable
{
    private readonly Dictionary<ushort, List<EffectSlot>> fixedBindings = [];
    private readonly Dictionary<(ushort Officer, ushort Personality, string Effect, int Value), ScriptedTraitComponent> attached = [];
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

    internal void Register(ScriptEffectDefinition definition,
        Action<ScriptedTraitComponent>? onTurnStart = null,
        Action<ScriptedTraitComponent>? onUnitActionEnd = null,
        Action<ScriptedTraitComponent>? onTurnEnd = null)
    {
        if (Registry.Contains(definition.Id))
            throw new ArgumentException($"효과 {definition.Id} 를 이미 정의했습니다.");
        EffectCatalog.Define(this, definition);
        Registry.Register(definition, onTurnStart, onUnitActionEnd, onTurnEnd);
    }

    // Without an editor: an officer holding this trait (1..254) runs the effect.
    internal void Bind(ushort personalityId, string effectId, int value = 0)
    {
        if (!Registry.Contains(effectId))
            throw new ArgumentException($"Effect {effectId} is not registered.", nameof(effectId));

        if (!fixedBindings.TryGetValue(personalityId, out var slots))
            fixedBindings.Add(personalityId, slots = []);

        var slot = new EffectSlot(effectId, value);
        if (!slots.Contains(slot))
            slots.Add(slot);
    }

    private IEnumerable<EffectSlot> EffectsOf(ushort personality)
    {
        if (fixedBindings.TryGetValue(personality, out var fixedSlots))
            foreach (var slot in fixedSlots)
                yield return slot;

        if (EffectCatalog.SlotsOf(personality) is { } slots)
            foreach (var slot in slots)
                if (slot is { } held && Registry.Contains(held.Id))
                    yield return held;
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
                var applied = new HashSet<EffectSlot>();

                foreach (var officer in unit.Officers)
                    foreach (ushort personality in officer.PersonalityIds)
                        foreach (var slot in EffectsOf(personality))
                        {
                            if (!Registry.Handles(slot.Id, kind) || !applied.Add(slot))
                                continue;

                            Run(attached[(officer.Id, personality, slot.Id, slot.Value)], kind);
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
        var retained = new HashSet<(ushort Officer, ushort Personality, string Effect, int Value)>();

        foreach (var owner in World.Officers)
        {
            var officer = owner.GetComponent<OfficerComponent>()!;

            foreach (ushort personality in officer.PersonalityIds)
                foreach (var slot in EffectsOf(personality))
                {
                    var key = (officer.Id, personality, slot.Id, slot.Value);

                    if (retained.Add(key) && !attached.ContainsKey(key))
                        attached.Add(key, Registry.Attach(slot.Id, owner, personality, slot.Value));
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
        EffectCatalog.Withdraw(this);
        battle?.Dispose();

        foreach (var component in attached.Values)
            component.GameObject.RemoveComponent(component);

        attached.Clear();
    }
}
