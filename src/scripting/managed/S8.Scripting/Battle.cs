using System.Runtime.InteropServices;

namespace S8.Scripting;

public sealed record BattleOfficer(ushort Id, IReadOnlyList<ushort> PersonalityIds);

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct BattleUnitRecord
{
    public ulong Token, Identity;
    public uint Soldiers, Capacity;
    public fixed ushort Officers[3];
    public fixed ushort Personalities[9];
}

// The component persists for the battle; its native token lasts one callback.
public sealed unsafe class BattleUnitComponent : Component
{
    private ulong token;

    public ulong Id { get; private set; }

    public bool IsCurrent => token != 0;

    public uint Soldiers { get; private set; }

    public uint Capacity { get; private set; }

    public IReadOnlyList<BattleOfficer> Officers { get; private set; } = [];

    public uint RestoreSoldiers(uint amount)
    {
        ArgumentOutOfRangeException.ThrowIfGreaterThan(amount, (uint)ushort.MaxValue);
        uint current = checked((uint)Native.Call(Operation.ReadBattleSoldiers, token));
        uint next = Math.Min(Capacity, current + amount);
        Soldiers = current;
        if (next <= current)
            return 0;

        Native.Call(Operation.SetBattleSoldiers, token, next);
        Soldiers = checked((uint)Native.Call(Operation.ReadBattleSoldiers, token));
        return Soldiers - current;
    }

    public void ShowPersonality(ushort officerId, ushort personalityId, string speech)
    {
        fixed (char* text = speech)
            Native.Call(Operation.ShowBattlePersonality, token,
                ((long)personalityId << 16) | officerId, text, checked((uint)((speech.Length + 1) * sizeof(char))));
    }

    internal void Apply(BattleUnitRecord record)
    {
        token = record.Token;
        Id = record.Identity;
        Soldiers = record.Soldiers;
        Capacity = record.Capacity;
        var officers = new List<BattleOfficer>(3);

        for (int member = 0; member < 3; ++member)
        {
            if (record.Officers[member] == ushort.MaxValue)
                continue;

            var personalities = new ushort[3];

            for (int slot = 0; slot < 3; ++slot)
                personalities[slot] = record.Personalities[member * 3 + slot];

            officers.Add(new(record.Officers[member], personalities));
        }

        Officers = officers;
    }

    internal ulong Token => token;

    internal void RestoreToken(ulong value) => token = value;
}

public sealed class BattleMembershipComponent : Component
{
    public BattleUnitComponent Unit { get; set; } = null!;

    public BattleOfficer Officer { get; set; } = null!;
}

// The manager supplies the activating trait; effects use the same presentation
// service for native portraits, badges, dialogue, configured voice and sound.
public abstract class ScriptedTraitComponent : Component, IBattleEffect
{
    public ushort PersonalityId { get; internal set; }

    // The value the trait's slot gives this effect ("{value}" in its description).
    public int Value { get; internal set; }

    // The battle unit of the officer holding this trait.
    public BattleUnitComponent Unit => GameObject.GetComponent<BattleMembershipComponent>()!.Unit;

    public void Show(string speech)
    {
        var membership = GameObject.GetComponent<BattleMembershipComponent>()!;

        membership.Unit.ShowPersonality(membership.Officer.Id, PersonalityId, speech);
    }
}

public interface IBattleEffect
{
}

public interface IBattleTurnStartEffect : IBattleEffect
{
    void OnBattleTurnStarted();
}

public interface IBattleUnitActionEndEffect : IBattleEffect
{
    // Only the unit whose native action cleanup completed is exposed.
    void OnBattleUnitActionEnded();
}

public interface IBattleTurnEndEffect : IBattleEffect
{
    // No next acting unit remains. Native turn-transition processing follows;
    // this does not mean every presentation in that processing has finished.
    void OnBattleTurnEnded();
}

public sealed record ScriptEffectDefinition(string Id, string Name, string Description);

// Effect IDs belong to the manager. They never enter the game's numeric effect
// slots, so the game's effect dispatcher cannot interpret them as engine IDs.
public sealed class ScriptEffectRegistry
{
    private readonly Dictionary<string, (
        ScriptEffectDefinition Definition,
        Func<GameObject, ushort, int, ScriptedTraitComponent> Attach,
        uint Events)> effects = [];

    public IEnumerable<ScriptEffectDefinition> Definitions => effects.Values.Select(effect => effect.Definition);

    public void Register<T>(ScriptEffectDefinition definition)
        where T : ScriptedTraitComponent, new()
    {
        uint events = 0;

        if (typeof(IBattleTurnStartEffect).IsAssignableFrom(typeof(T)))
            events |= 1U << (int)ScriptEvent.BattleTurnStarted;

        if (typeof(IBattleUnitActionEndEffect).IsAssignableFrom(typeof(T)))
            events |= 1U << (int)ScriptEvent.BattleUnitActionEnded;

        if (typeof(IBattleTurnEndEffect).IsAssignableFrom(typeof(T)))
            events |= 1U << (int)ScriptEvent.BattleTurnEnded;

        effects.Add(definition.Id, (definition,
            (gameObject, personality, value) => gameObject.AddComponent<T>(component =>
            {
                component.PersonalityId = personality;
                component.Value = value;
            }), events));
    }

    // The same as Register<T> without writing a class: pass only the moments the effect
    // acts on. The callback receives the trait, with its Unit and Show.
    public void Register(ScriptEffectDefinition definition,
        Action<ScriptedTraitComponent>? onTurnStart = null,
        Action<ScriptedTraitComponent>? onUnitActionEnd = null,
        Action<ScriptedTraitComponent>? onTurnEnd = null)
    {
        uint events = 0;

        if (onTurnStart != null)
            events |= 1U << (int)ScriptEvent.BattleTurnStarted;

        if (onUnitActionEnd != null)
            events |= 1U << (int)ScriptEvent.BattleUnitActionEnded;

        if (onTurnEnd != null)
            events |= 1U << (int)ScriptEvent.BattleTurnEnded;

        effects.Add(definition.Id, (definition,
            (gameObject, personality, value) => gameObject.AddComponent<CallbackEffect>(component =>
            {
                component.PersonalityId = personality;
                component.Value = value;
                component.TurnStart = onTurnStart;
                component.UnitActionEnd = onUnitActionEnd;
                component.TurnEnd = onTurnEnd;
            }), events));
    }

    private sealed class CallbackEffect : ScriptedTraitComponent,
        IBattleTurnStartEffect, IBattleUnitActionEndEffect, IBattleTurnEndEffect
    {
        public Action<ScriptedTraitComponent>? TurnStart, UnitActionEnd, TurnEnd;

        public void OnBattleTurnStarted() => TurnStart?.Invoke(this);

        public void OnBattleUnitActionEnded() => UnitActionEnd?.Invoke(this);

        public void OnBattleTurnEnded() => TurnEnd?.Invoke(this);
    }

    public ScriptedTraitComponent Attach(string id, GameObject owner, ushort personality, int value = 0) =>
        effects[id].Attach(owner, personality, value);

    public bool Contains(string id) => effects.ContainsKey(id);

    public bool Handles(string id, ScriptEvent kind) =>
        effects.TryGetValue(id, out var effect) && (effect.Events & (1U << (int)kind)) != 0;
}
