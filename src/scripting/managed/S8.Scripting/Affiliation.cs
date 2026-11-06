using System.Runtime.InteropServices;

namespace S8.Scripting;

// RoleId is the native role-data ID, not a byte extracted from its address.
// RankId comes from the separate rank-data reference. Current mission is unbound.
public readonly record struct OfficerAffiliation(
    byte RoleId, byte? RankId, byte? ForceId, ushort? LordId, byte? CityId);

public readonly record struct CityAffiliation(
    byte? ForceId, ushort? LordId, ushort? GovernorId, ulong? DistrictNumber);

[StructLayout(LayoutKind.Sequential)]
internal struct AffiliationRecord
{
    public ushort Entity, Lord, Governor;
    public byte Rank, Reserved;
    public byte Role, Force, City, Flags;
    public uint Padding;
    public ulong DistrictNumber;
}

public sealed unsafe class OfficerAffiliationComponent : OfficerDataComponent
{
    public OfficerAffiliation Read()
    {
        AffiliationRecord record;
        Native.Call(Operation.ReadOfficerAffiliation, Officer.Id, checked((long)Officer.Memory.Session),
            &record, (uint)sizeof(AffiliationRecord));

        return new(record.Role,
            (record.Flags & 32) != 0 ? record.Rank : null,
            (record.Flags & 1) != 0 ? record.Force : null,
            (record.Flags & 4) != 0 ? record.Lord : null,
            (record.Flags & 2) != 0 ? record.City : null);
    }
}

public sealed unsafe class CityAffiliationComponent : Component
{
    private CityComponent city = null!;

    public override void OnEnable() => city = GameObject.GetComponent<CityComponent>()
        ?? throw new InvalidOperationException("This component requires a city GameObject.");

    public CityAffiliation Read()
    {
        AffiliationRecord record;
        Native.Call(Operation.ReadCityAffiliation, city.Id, checked((long)city.Memory.Session),
            &record, (uint)sizeof(AffiliationRecord));

        return new(
            (record.Flags & 1) != 0 ? record.Force : null,
            (record.Flags & 4) != 0 ? record.Lord : null,
            (record.Flags & 8) != 0 ? record.Governor : null,
            (record.Flags & 16) != 0 ? record.DistrictNumber : null);
    }
}
