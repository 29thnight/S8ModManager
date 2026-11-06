using System.Runtime.InteropServices;

namespace S8.Scripting;

public readonly record struct TraitEffectValue(ushort Type, ushort Value, ushort Parameter);

// One trait ID's definition. Effects null = keep the game's effect row; Name or
// Description null = keep the game's text. -1 leaves the integer fields unset.
public sealed record TraitDefinition
{
    public IReadOnlyList<TraitEffectValue>? Effects { get; init; }
    public bool HasMetadata { get; init; }
    public string? Name { get; init; }
    public string? Description { get; init; }
    public int CloneSource { get; init; } = -1;
    public int BackgroundTrait { get; init; } = -1;
    public int Grade { get; init; } = -1;
    public int Voice { get; init; } = -1;
}

// Applies a whole trait table to the game: effect rows, names and descriptions,
// and admission of named custom IDs 201..254 by save loading and scenario start.
// Publishing again replaces the previous table; the native runtime applies it
// on its own thread, so the call returns before the game shows the change.
public static unsafe class TraitCatalog
{
    public const int Count = 254;

    public static void Publish(IReadOnlyList<TraitDefinition?> definitions)
    {
        if (definitions.Count != Count)
            throw new ArgumentException($"A trait catalog has exactly {Count} entries (IDs 1..{Count}).", nameof(definitions));

        var records = new Record[Count];
        var pins = new List<GCHandle>();
        try
        {
            for (int index = 0; index < Count; ++index)
            {
                var definition = definitions[index];
                ref var record = ref records[index];
                record.CloneSource = record.BackgroundTrait = record.Grade = record.Voice = -1;
                if (definition == null)
                    continue;

                if (definition.Effects is { } effects)
                {
                    if (effects.Count != 6)
                        throw new ArgumentException($"Trait {index + 1} must have 6 effects.", nameof(definitions));
                    record.Present = 1;
                    for (int slot = 0; slot < 6; ++slot)
                    {
                        record.Effects[slot * 3] = effects[slot].Type;
                        record.Effects[slot * 3 + 1] = effects[slot].Value;
                        record.Effects[slot * 3 + 2] = effects[slot].Parameter;
                    }
                }

                if (!definition.HasMetadata)
                    continue;
                record.HasMetadata = 1;
                record.Name = Pin(definition.Name, pins);
                record.Description = Pin(definition.Description, pins);
                record.CloneSource = definition.CloneSource;
                record.BackgroundTrait = definition.BackgroundTrait;
                record.Grade = definition.Grade;
                record.Voice = definition.Voice;
            }

            fixed (Record* data = records)
                Native.Call(Operation.SetTraitCatalog, data: data, bytes: (uint)(Count * sizeof(Record)));
        }
        finally
        {
            foreach (var pin in pins)
                pin.Free();
        }
    }

    private static char* Pin(string? text, List<GCHandle> pins)
    {
        if (text == null)
            return null;
        var handle = GCHandle.Alloc((text + '\0').ToCharArray(), GCHandleType.Pinned);
        pins.Add(handle);
        return (char*)handle.AddrOfPinnedObject();
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Record
    {
        public char* Name, Description;
        public fixed ushort Effects[18];
        public byte Present, HasMetadata;
        public ushort Reserved;
        public int CloneSource, BackgroundTrait, Grade, Voice;
    }
}
