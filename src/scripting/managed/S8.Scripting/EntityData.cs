using System.Runtime.InteropServices;

namespace S8.Scripting;

public enum OfficerStat : byte
{
    Leadership = 1,
    Strength,
    Intelligence,
    Politics,
    Charm
}

public enum OfficerFame : byte { Civil, Martial, Infamy }

public enum TrainingLevel : byte { Unlearned, One, Two, Three }

public enum CityValue : uint
{
    Gold,
    Grain,
    Troops,
    Development,
    Commerce,
    Defense,
    Technology,
    DevelopmentLimit,
    CommerceLimit,
    DefenseLimit,
    TechnologyLimit,
    TroopLimit,
    Revolt
}

internal enum EntityValueKind : uint
{
    Stat, Gold, ActionPoints, Fame, Tactic, Skill, Personality, City
}

internal static unsafe class EntityData
{
    [StructLayout(LayoutKind.Sequential)]
    private struct Request
    {
        public ulong Session;
        public EntityValueKind Kind;
        public uint Index;
        public long Value;
    }

    internal static long Call(Operation operation, ushort id, ulong session,
        EntityValueKind kind, uint index = 0, long value = 0)
    {
        Request request = new() { Session = session, Kind = kind, Index = index, Value = value };
        return Native.Call(operation, id, data: &request, bytes: (uint)sizeof(Request));
    }
}

// The entity kind is checked once when a standard component is attached.
// Each native request resolves the live record using its ID and session.
public abstract class OfficerDataComponent : Component
{
    protected OfficerComponent Officer { get; private set; } = null!;

    public override void OnEnable() => Officer = GameObject.GetComponent<OfficerComponent>()
        ?? throw new InvalidOperationException("This component requires an officer GameObject.");
}

public sealed class OfficerStatsComponent : OfficerDataComponent
{
    public byte Get(OfficerStat stat) => checked((byte)EntityData.Call(Operation.ReadEntityValue,
        Officer.Id, Officer.Memory.Session, EntityValueKind.Stat, (uint)stat));

    public byte Set(OfficerStat stat, byte value) => checked((byte)EntityData.Call(Operation.SetEntityValue,
        Officer.Id, Officer.Memory.Session, EntityValueKind.Stat, (uint)stat, value));

    public uint Gold => checked((uint)EntityData.Call(Operation.ReadEntityValue,
        Officer.Id, Officer.Memory.Session, EntityValueKind.Gold));

    public uint SetGold(uint value) => checked((uint)EntityData.Call(Operation.SetEntityValue,
        Officer.Id, Officer.Memory.Session, EntityValueKind.Gold, value: value));

    public uint AddGold(int delta) => checked((uint)EntityData.Call(Operation.AddEntityValue,
        Officer.Id, Officer.Memory.Session, EntityValueKind.Gold, value: delta));

    public byte ActionPoints => checked((byte)EntityData.Call(Operation.ReadEntityValue,
        Officer.Id, Officer.Memory.Session, EntityValueKind.ActionPoints));

    public byte SetActionPoints(byte value) => checked((byte)EntityData.Call(Operation.SetEntityValue,
        Officer.Id, Officer.Memory.Session, EntityValueKind.ActionPoints, value: value));
}

public sealed class OfficerFameComponent : OfficerDataComponent
{
    public ushort Get(OfficerFame kind) => checked((ushort)EntityData.Call(Operation.ReadEntityValue,
        Officer.Id, Officer.Memory.Session, EntityValueKind.Fame, (uint)kind));

    public ushort Set(OfficerFame kind, ushort value) => checked((ushort)EntityData.Call(Operation.SetEntityValue,
        Officer.Id, Officer.Memory.Session, EntityValueKind.Fame, (uint)kind, value));
}

public sealed class OfficerTrainingComponent : OfficerDataComponent
{
    // Native IDs, not UI row indexes: tactics 1..35, skills 1..24.
    public TrainingLevel GetTactic(byte id) => (TrainingLevel)EntityData.Call(Operation.ReadEntityValue,
        Officer.Id, Officer.Memory.Session, EntityValueKind.Tactic, id);

    public TrainingLevel SetTactic(byte id, TrainingLevel level) => (TrainingLevel)EntityData.Call(Operation.SetEntityValue,
        Officer.Id, Officer.Memory.Session, EntityValueKind.Tactic, id, (byte)level);

    public TrainingLevel GetSkill(byte id) => (TrainingLevel)EntityData.Call(Operation.ReadEntityValue,
        Officer.Id, Officer.Memory.Session, EntityValueKind.Skill, id);

    public TrainingLevel SetSkill(byte id, TrainingLevel level) => (TrainingLevel)EntityData.Call(Operation.SetEntityValue,
        Officer.Id, Officer.Memory.Session, EntityValueKind.Skill, id, (byte)level);
}

public sealed class OfficerTraitsComponent : OfficerDataComponent
{
    // Native slots are zero-based; personality ID 0 clears the slot.
    public ushort GetSlot(byte slot) => checked((ushort)EntityData.Call(Operation.ReadEntityValue,
        Officer.Id, Officer.Memory.Session, EntityValueKind.Personality, slot));

    public ushort SetSlot(byte slot, ushort personalityId)
    {
        ushort applied = checked((ushort)EntityData.Call(Operation.SetEntityValue,
            Officer.Id, Officer.Memory.Session, EntityValueKind.Personality, slot, personalityId));
        var ids = Officer.PersonalityIds.ToArray();
        ids[slot] = applied;
        Officer.PersonalityIds = ids;
        return applied;
    }
}

public sealed class CityAdministrationComponent : Component
{
    private CityComponent city = null!;

    public override void OnEnable() => city = GameObject.GetComponent<CityComponent>()
        ?? throw new InvalidOperationException("This component requires a city GameObject.");

    public uint Get(CityValue field) => checked((uint)EntityData.Call(Operation.ReadEntityValue,
        city.Id, city.Memory.Session, EntityValueKind.City, (uint)field));

    // TechnologyLimit is read-only until its game setter is established.
    public uint Set(CityValue field, uint value) => checked((uint)EntityData.Call(Operation.SetEntityValue,
        city.Id, city.Memory.Session, EntityValueKind.City, (uint)field, value));

    // Native delta operations exist for Gold, Grain and Troops only.
    public uint Add(CityValue field, int delta) => checked((uint)EntityData.Call(Operation.AddEntityValue,
        city.Id, city.Memory.Session, EntityValueKind.City, (uint)field, delta));
}
