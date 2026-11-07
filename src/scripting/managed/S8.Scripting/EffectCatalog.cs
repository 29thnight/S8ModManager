namespace S8.Scripting;

// One trait slot holding a scripted effect and the value the trait gives it.
public readonly record struct EffectSlot(string Id, int Value);

// Scripted effects shared by every loaded mod. S8.Scripting is loaded once for
// all JIT mods, so a Lua mod defines an effect here and the trait editor (another
// mod) lists it, renders its description and supplies the trait slots that hold it.
public static class EffectCatalog
{
    private static readonly Dictionary<string, (ScriptEffectDefinition Definition, object Owner)> definitions = [];
    private static (object Owner, Func<ushort, IReadOnlyList<EffectSlot?>?> Source)? bindings;

    // Raised when definitions are added or withdrawn, for editors that cache the list.
    public static event Action? Changed;

    public static IReadOnlyList<ScriptEffectDefinition> Definitions =>
        definitions.Values.Select(entry => entry.Definition).OrderBy(definition => definition.Name, StringComparer.Ordinal).ToArray();

    public static ScriptEffectDefinition? Find(string id) =>
        definitions.TryGetValue(id, out var entry) ? entry.Definition : null;

    // "{value}" in the description becomes the slot's value.
    public static string Render(EffectSlot slot) => Find(slot.Id) is { } definition
        ? definition.Description.Replace("{value}", slot.Value.ToString())
        : $"(없는 효과 {slot.Id})";

    // The mod that owns the trait slot file. One provider at a time.
    public static void UseBindings(object owner, Func<ushort, IReadOnlyList<EffectSlot?>?> source)
    {
        ArgumentNullException.ThrowIfNull(owner);
        bindings = (owner, source);
    }

    public static void Withdraw(object owner)
    {
        bool removed = false;

        foreach (string id in definitions.Where(entry => ReferenceEquals(entry.Value.Owner, owner)).Select(entry => entry.Key).ToArray())
            removed |= definitions.Remove(id);
        if (bindings is { } current && ReferenceEquals(current.Owner, owner))
            bindings = null;
        if (removed)
            Changed?.Invoke();
    }

    internal static void Define(object owner, ScriptEffectDefinition definition)
    {
        if (string.IsNullOrWhiteSpace(definition.Id))
            throw new ArgumentException("효과 ID가 비어 있습니다.");
        if (definitions.TryGetValue(definition.Id, out var existing) && !ReferenceEquals(existing.Owner, owner))
            throw new ArgumentException($"효과 {definition.Id} 는 다른 모드가 이미 정의했습니다.");
        definitions[definition.Id] = (definition, owner);
        Changed?.Invoke();
    }

    internal static IReadOnlyList<EffectSlot?>? SlotsOf(ushort personality) => bindings?.Source(personality);
}
