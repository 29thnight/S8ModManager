using System.Buffers.Binary;
using System.Runtime.InteropServices;

namespace S8.Scripting;

public readonly record struct PersonalityEffect(ushort Id, ushort Value, ushort Parameter);
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct PersonalityRecord
{
    public MemoryReference Memory;
    public ushort Id, Voice;
    public fixed ushort Effects[18];
}

// Names use engine getters (RVA 170E2D0/170ABB0). No native setter for these
// strings has been established. The mod's persistence layer owns text overrides.
public sealed unsafe class PersonalityComponent : Component
{
    public ushort Id { get; private set; }

    public MemoryReference Memory { get; private set; }

    public ushort Voice { get; private set; }

    public PersonalityEffect[] Effects { get; private set; } = [];

    public string Name => Native.ReadString(Operation.ReadPersonalityName, Id);

    public string Description => Native.ReadString(Operation.ReadPersonalityDescription, Id);

    public bool Bind(ushort id)
    {
        PersonalityRecord record;
        // Absent records return 0; native errors use negative status codes.

        if (Native.Call(Operation.GetPersonality, id, data: &record, bytes: (uint)sizeof(PersonalityRecord)) == 0)
            return false;

        Id = record.Id;
        Memory = record.Memory;
        Voice = record.Voice;
        Effects = new PersonalityEffect[6];

        for (int i = 0; i < 6; ++i)
            Effects[i] = new(record.Effects[i * 3], record.Effects[i * 3 + 1], record.Effects[i * 3 + 2]);

        return true;
    }

    // This operation explicitly uses the standard memory tool; it is not an
    // invented game setter. Preserve the two unknown bytes before voice.

    public void WriteValues(ReadOnlySpan<PersonalityEffect> effects, ushort voice)
    {
        if (effects.Length != 6)
            throw new ArgumentException("Six effect slots are required.", nameof(effects));

        if (!Bind(Id))
            throw new InvalidOperationException("The game record is no longer present.");

        Span<byte> data = stackalloc byte[40];
        var destination = Memory.Offset(0x0A);

        MemoryAccess.Read(destination, data);

        for (int i = 0; i < 6; ++i)
        {
            BinaryPrimitives.WriteUInt16LittleEndian(data[(i * 6)..], effects[i].Id);
            BinaryPrimitives.WriteUInt16LittleEndian(data[(i * 6 + 2)..], effects[i].Value);
            BinaryPrimitives.WriteUInt16LittleEndian(data[(i * 6 + 4)..], effects[i].Parameter);
        }

        BinaryPrimitives.WriteUInt16LittleEndian(data[38..], voice);
        MemoryAccess.Write(destination, data);
    }
}
