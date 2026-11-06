using System.Runtime.InteropServices;

namespace S8.Scripting;

[StructLayout(LayoutKind.Sequential)]
internal struct BattleContextRecord
{
    public ulong Session, Sequence, ActingUnit;
    public ScriptEvent Kind;
    public uint Reserved;
    public ulong WorldSession;
}

public sealed record BattleEventInfo(
    ulong SessionId, ulong Sequence, ScriptEvent Kind, ulong ActingUnitId, ulong WorldSessionId);

// A battle adapter shares the loaded-session world. It owns only its scene's
// units and memberships; officer GameObjects and their effects belong to World.
public sealed unsafe class BattleWorld : IDisposable
{
    private readonly Dictionary<ulong, GameObject> units = [];
    private readonly Dictionary<ushort, GameObject> officers = [];
    private readonly Stack<EventScope> scopes = [];
    private GameScene? scene;

    public BattleWorld(GameWorld world) => World = world;

    public GameWorld World { get; }

    private sealed record EventScope(
        BattleEventInfo? Event,
        IReadOnlyList<GameObject> Units,
        Dictionary<ulong, ulong> Tokens);

    public ulong SessionId { get; private set; }

    public BattleEventInfo? CurrentEvent { get; private set; }

    public IReadOnlyCollection<GameObject> Units => units.Values;

    public IReadOnlyCollection<GameObject> Officers => officers.Values;

    public IReadOnlyList<GameObject> EventUnits { get; private set; } = [];

    public GameObject? FindUnit(ulong id) => units.GetValueOrDefault(id);

    public GameObject? FindOfficer(ushort id) => officers.GetValueOrDefault(id);

    public void Refresh()
    {
        BattleContextRecord context;

        Native.Call(Operation.GetBattleContext, data: &context, bytes: (uint)sizeof(BattleContextRecord));
        int count = checked((int)Native.Call(Operation.ReadBattleUnits));
        var records = new BattleUnitRecord[count];

        fixed (BattleUnitRecord* data = records)
            Native.Call(Operation.ReadBattleUnits, data: data, bytes: checked((uint)(count * sizeof(BattleUnitRecord))));

        World.BeginSession(context.WorldSession);

        if (SessionId != context.Session)
        {
            Dispose();
            SessionId = context.Session;
            scene = World.EnterScene(GameSceneKind.Battle, context.Session);
            scene.Disposing += ReleaseSceneBindings;
        }

        // A native presentation can pump another battle event before the outer
        // callback returns. Restore the outer callback's tokens afterwards.
        scopes.Push(new(CurrentEvent, EventUnits, units.ToDictionary(
            pair => pair.Key, pair => pair.Value.GetComponent<BattleUnitComponent>()!.Token)));

        var presentUnits = new HashSet<ulong>();
        var presentOfficers = new HashSet<ushort>();

        foreach (var record in records)
        {
            presentUnits.Add(record.Identity);

            if (!units.TryGetValue(record.Identity, out var gameObject))
            {
                gameObject = scene!.CreateObject($"BattleUnit:{record.Identity}");
                gameObject.AddComponent<BattleUnitComponent>();
                units.Add(record.Identity, gameObject);
            }

            var unit = gameObject.GetComponent<BattleUnitComponent>()!;

            unit.Apply(record);

            foreach (var officer in unit.Officers)
            {
                presentOfficers.Add(officer.Id);

                if (!officers.TryGetValue(officer.Id, out var owner))
                {
                    owner = World.GetOfficer(officer.Id);
                    owner.AddComponent<BattleMembershipComponent>();
                    officers.Add(officer.Id, owner);
                    scene!.Track(owner);
                }

                var membership = owner.GetComponent<BattleMembershipComponent>()!;

                membership.Unit = unit;
                membership.Officer = officer;
                owner.GetComponent<OfficerComponent>()!.PersonalityIds = officer.PersonalityIds;
            }
        }

        foreach (ushort id in officers.Keys.Where(id => !presentOfficers.Contains(id)).ToArray())
        {
            var owner = officers[id];

            owner.RemoveComponent(owner.GetComponent<BattleMembershipComponent>()!);
            scene!.Untrack(owner);
            officers.Remove(id);
        }

        foreach (ulong id in units.Keys.Where(id => !presentUnits.Contains(id)).ToArray())
        {
            scene!.Untrack(units[id]);
            units.Remove(id);
        }

        CurrentEvent = new(context.Session, context.Sequence, context.Kind, context.ActingUnit, context.WorldSession);
        EventUnits = context.ActingUnit == 0 ? units.Values.ToArray() : [units[context.ActingUnit]];
    }

    public void EndEvent()
    {
        scopes.TryPop(out var previous);

        foreach (var gameObject in units.Values)
        {
            var unit = gameObject.GetComponent<BattleUnitComponent>()!;

            unit.RestoreToken(previous?.Tokens.GetValueOrDefault(unit.Id) ?? 0);
        }

        CurrentEvent = previous?.Event;
        EventUnits = previous?.Units ?? [];
    }

    public void Dispose()
    {
        scene?.Dispose();
    }

    private void ReleaseSceneBindings()
    {
        scopes.Clear();
        EndEvent();

        foreach (var gameObject in officers.Values)
            gameObject.RemoveComponent(gameObject.GetComponent<BattleMembershipComponent>()!);

        officers.Clear();
        units.Clear();
        SessionId = 0;
        scene = null;
    }
}
