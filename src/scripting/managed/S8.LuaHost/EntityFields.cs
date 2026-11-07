using S8.Scripting;

namespace S8.LuaHost;

// Lua field names over the SDK handles. Setters reject values outside the
// field's range instead of wrapping them.
internal static class EntityFields
{
    public static double Get(Officer o, string key) => key switch
    {
        "leadership" => o.Leadership,
        "strength" => o.Strength,
        "intelligence" => o.Intelligence,
        "politics" => o.Politics,
        "charm" => o.Charm,
        "gold" => o.Gold,
        "action_points" => o.ActionPoints,
        "civil_fame" => o.CivilFame,
        "martial_fame" => o.MartialFame,
        "infamy" => o.Infamy,
        _ => throw Unknown("무장", key, OfficerKeys)
    };

    public static void Set(Officer o, string key, double value)
    {
        switch (key)
        {
            case "leadership": o.Leadership = Byte(value, key); break;
            case "strength": o.Strength = Byte(value, key); break;
            case "intelligence": o.Intelligence = Byte(value, key); break;
            case "politics": o.Politics = Byte(value, key); break;
            case "charm": o.Charm = Byte(value, key); break;
            case "gold": o.Gold = UInt(value, key); break;
            case "action_points": o.ActionPoints = Byte(value, key); break;
            case "civil_fame": o.CivilFame = UShort(value, key); break;
            case "martial_fame": o.MartialFame = UShort(value, key); break;
            case "infamy": o.Infamy = UShort(value, key); break;
            default: throw Unknown("무장", key, OfficerKeys);
        }
    }

    public static double Get(City c, string key) => key switch
    {
        "gold" => c.Gold,
        "grain" => c.Grain,
        "troops" => c.Troops,
        "development" => c.Development,
        "commerce" => c.Commerce,
        "defense" => c.Defense,
        "technology" => c.Technology,
        "revolt" => c.Revolt,
        "development_limit" => c.DevelopmentLimit,
        "commerce_limit" => c.CommerceLimit,
        "defense_limit" => c.DefenseLimit,
        "troop_limit" => c.TroopLimit,
        "technology_limit" => c.TechnologyLimit,
        _ => throw Unknown("도시", key, CityKeys)
    };

    public static void Set(City c, string key, double value)
    {
        uint v = UInt(value, key);
        switch (key)
        {
            case "gold": c.Gold = v; break;
            case "grain": c.Grain = v; break;
            case "troops": c.Troops = v; break;
            case "development": c.Development = v; break;
            case "commerce": c.Commerce = v; break;
            case "defense": c.Defense = v; break;
            case "technology": c.Technology = v; break;
            case "revolt": c.Revolt = v; break;
            case "development_limit": c.DevelopmentLimit = v; break;
            case "commerce_limit": c.CommerceLimit = v; break;
            case "defense_limit": c.DefenseLimit = v; break;
            case "troop_limit": c.TroopLimit = v; break;
            case "technology_limit" or "name" or "id": throw new ArgumentException($"도시의 {key} 는 읽기 전용입니다.");
            default: throw Unknown("도시", key, CityKeys);
        }
    }

    private const string OfficerKeys = "id, leadership, strength, intelligence, politics, charm, gold, action_points, civil_fame, martial_fame, infamy";
    private const string CityKeys = "id, name, gold, grain, troops, development, commerce, defense, technology, revolt, *_limit";

    private static ArgumentException Unknown(string what, string key, string keys) =>
        new($"{what}에 없는 값: {key} ({keys})");

    private static byte Byte(double v, string key) => checked((byte)Whole(v, key, byte.MaxValue));
    private static ushort UShort(double v, string key) => checked((ushort)Whole(v, key, ushort.MaxValue));
    private static uint UInt(double v, string key) => checked((uint)Whole(v, key, uint.MaxValue));

    private static double Whole(double v, string key, double max) =>
        v == Math.Floor(v) && v >= 0 && v <= max ? v : throw new ArgumentException($"{key}: 0~{max} 사이의 정수가 필요합니다 ({v}).");
}
