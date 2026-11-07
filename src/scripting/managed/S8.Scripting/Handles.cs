namespace S8.Scripting;

// Plain value handles over the same native requests the components use, so the
// common edits read as properties: `officer.Gold += 100`. A handle carries the
// session it was taken in; after a load or a new game the native side rejects it
// (EngineCallException with StaleSession) instead of touching another record.
public readonly struct Officer : IEquatable<Officer>
{
    internal Officer(ushort id, ulong session)
    {
        Id = id;
        Session = session;
    }

    public ushort Id { get; }

    public ulong Session { get; }

    public byte Leadership { get => Stat(OfficerStat.Leadership); set => SetStat(OfficerStat.Leadership, value); }
    public byte Strength { get => Stat(OfficerStat.Strength); set => SetStat(OfficerStat.Strength, value); }
    public byte Intelligence { get => Stat(OfficerStat.Intelligence); set => SetStat(OfficerStat.Intelligence, value); }
    public byte Politics { get => Stat(OfficerStat.Politics); set => SetStat(OfficerStat.Politics, value); }
    public byte Charm { get => Stat(OfficerStat.Charm); set => SetStat(OfficerStat.Charm, value); }

    public uint Gold
    {
        get => checked((uint)Read(EntityValueKind.Gold));
        set => Write(EntityValueKind.Gold, 0, value);
    }

    public byte ActionPoints
    {
        get => checked((byte)Read(EntityValueKind.ActionPoints));
        set => Write(EntityValueKind.ActionPoints, 0, value);
    }

    public ushort CivilFame { get => Fame(OfficerFame.Civil); set => SetFame(OfficerFame.Civil, value); }
    public ushort MartialFame { get => Fame(OfficerFame.Martial); set => SetFame(OfficerFame.Martial, value); }
    public ushort Infamy { get => Fame(OfficerFame.Infamy); set => SetFame(OfficerFame.Infamy, value); }

    private byte Stat(OfficerStat stat) => checked((byte)Read(EntityValueKind.Stat, (uint)stat));
    private void SetStat(OfficerStat stat, byte value) => Write(EntityValueKind.Stat, (uint)stat, value);
    private ushort Fame(OfficerFame kind) => checked((ushort)Read(EntityValueKind.Fame, (uint)kind));
    private void SetFame(OfficerFame kind, ushort value) => Write(EntityValueKind.Fame, (uint)kind, value);

    private long Read(EntityValueKind kind, uint index = 0) =>
        EntityData.Call(Operation.ReadEntityValue, Id, Session, kind, index);

    private void Write(EntityValueKind kind, uint index, long value) =>
        EntityData.Call(Operation.SetEntityValue, Id, Session, kind, index, value);

    public bool Equals(Officer other) => Id == other.Id && Session == other.Session;
    public override bool Equals(object? obj) => obj is Officer other && Equals(other);
    public override int GetHashCode() => HashCode.Combine(Id, Session);
    public static bool operator ==(Officer left, Officer right) => left.Equals(right);
    public static bool operator !=(Officer left, Officer right) => !left.Equals(right);
    public override string ToString() => $"Officer {Id}";
}

public readonly struct City : IEquatable<City>
{
    internal City(ushort id, ulong session)
    {
        Id = id;
        Session = session;
    }

    public ushort Id { get; }

    public ulong Session { get; }

    public string Name => Native.ReadString(Operation.ReadCityName, Id, checked((long)Session));

    public uint Gold { get => Get(CityValue.Gold); set => Set(CityValue.Gold, value); }
    public uint Grain { get => Get(CityValue.Grain); set => Set(CityValue.Grain, value); }
    public uint Troops { get => Get(CityValue.Troops); set => Set(CityValue.Troops, value); }
    public uint Development { get => Get(CityValue.Development); set => Set(CityValue.Development, value); }
    public uint Commerce { get => Get(CityValue.Commerce); set => Set(CityValue.Commerce, value); }
    public uint Defense { get => Get(CityValue.Defense); set => Set(CityValue.Defense, value); }
    public uint Technology { get => Get(CityValue.Technology); set => Set(CityValue.Technology, value); }
    public uint Revolt { get => Get(CityValue.Revolt); set => Set(CityValue.Revolt, value); }
    public uint DevelopmentLimit { get => Get(CityValue.DevelopmentLimit); set => Set(CityValue.DevelopmentLimit, value); }
    public uint CommerceLimit { get => Get(CityValue.CommerceLimit); set => Set(CityValue.CommerceLimit, value); }
    public uint DefenseLimit { get => Get(CityValue.DefenseLimit); set => Set(CityValue.DefenseLimit, value); }
    public uint TroopLimit { get => Get(CityValue.TroopLimit); set => Set(CityValue.TroopLimit, value); }

    // No setter: the game's technology-limit setter has not been established.
    public uint TechnologyLimit => Get(CityValue.TechnologyLimit);

    private uint Get(CityValue field) => checked((uint)EntityData.Call(Operation.ReadEntityValue,
        Id, Session, EntityValueKind.City, (uint)field));

    private void Set(CityValue field, uint value) => EntityData.Call(Operation.SetEntityValue,
        Id, Session, EntityValueKind.City, (uint)field, value);

    public bool Equals(City other) => Id == other.Id && Session == other.Session;
    public override bool Equals(object? obj) => obj is City other && Equals(other);
    public override int GetHashCode() => HashCode.Combine(Id, Session);
    public static bool operator ==(City left, City right) => left.Equals(right);
    public static bool operator !=(City left, City right) => !left.Equals(right);
    public override string ToString() => $"City {Id}";
}

public sealed partial class GameWorld
{
    // Resolves the officer in the current session; throws if it does not exist.
    public Officer Officer(ushort id) =>
        new(GetOfficer(id).GetComponent<OfficerComponent>()!.Id, SessionId);

    public City City(ushort id) =>
        new(GetCity(id).GetComponent<CityComponent>()!.Id, SessionId);

    // The officer the player controls now.
    public Officer PlayerOfficer
    {
        get
        {
            var character = GetPlayer().Character.GetComponent<OfficerComponent>()!;
            return new(character.Id, SessionId);
        }
    }
}
