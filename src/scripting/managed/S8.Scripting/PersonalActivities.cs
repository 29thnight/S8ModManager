namespace S8.Scripting;

public enum DomesticActivityKind
{
    GateRepair = 0x15
}

public sealed record DomesticActivityInfo(
    ulong SessionId, GameObject Officer, DomesticActivityKind Kind);

public sealed record TalkInfo(
    ulong SessionId, GameObject Officer, GameObject Partner);

public readonly record struct GameDate(ushort Year, byte Month);

public sealed record MonthChangedInfo(ulong SessionId, GameDate Previous, GameDate Current);

public sealed record CouncilEnteredInfo(ulong SessionId, GameDate Date);

public interface IDomesticActivityCompletedHandler
{
    void OnDomesticActivityCompleted(DomesticActivityInfo activity);
}

public interface ITalkCompletedHandler
{
    void OnTalkCompleted(TalkInfo talk);
}

public interface IMonthChangedHandler
{
    // After the native monthly advance returns, before the next phase is ready.
    void OnMonthChanged(MonthChangedInfo month);
}

public interface ICouncilEnteredHandler
{
    // After entrance events and the transition to the council command state.
    void OnCouncilEntered(CouncilEnteredInfo council);
}

public sealed partial class GameWorld
{
    public event Action<DomesticActivityInfo>? DomesticActivityCompleted;

    public event Action<TalkInfo>? TalkCompleted;

    public event Action<MonthChangedInfo>? MonthChanged;

    public event Action<CouncilEnteredInfo>? CouncilEntered;

    // Called by the mod entry point after the native operation and its
    // presentation have returned. Components remain owned by this session.
    public bool DispatchActivity(ScriptEvent kind, int officerId, int value)
    {
        switch (kind)
        {
            case ScriptEvent.MonthChanged:
            {
                var month = new MonthChangedInfo(SessionId,
                    new GameDate((ushort)(officerId & 0xFFFF), (byte)(officerId >> 16)),
                    new GameDate((ushort)(value & 0xFFFF), (byte)(value >> 16)));
                var handlers = officers.Values.Concat(cities.Values)
                    .SelectMany(entity => entity.GetComponents<IMonthChangedHandler>()).ToArray();

                foreach (var handler in handlers)
                    handler.OnMonthChanged(month);

                MonthChanged?.Invoke(month);
                Events.Publish(WorldEventTriggers.MonthChanged, month);
                return true;
            }

            case ScriptEvent.CouncilEntered:
            {
                var council = new CouncilEnteredInfo(SessionId,
                    new GameDate((ushort)(value & 0xFFFF), (byte)(value >> 16)));
                var handlers = officers.Values.Concat(cities.Values)
                    .SelectMany(entity => entity.GetComponents<ICouncilEnteredHandler>()).ToArray();

                foreach (var handler in handlers)
                    handler.OnCouncilEntered(council);

                CouncilEntered?.Invoke(council);
                Events.Publish(WorldEventTriggers.CouncilEntered, council);
                return true;
            }

            case ScriptEvent.DomesticActivityCompleted:
            {
                var officer = GetOfficer(checked((ushort)officerId));
                var activity = new DomesticActivityInfo(SessionId, officer, (DomesticActivityKind)value);

                foreach (var handler in officer.GetComponents<IDomesticActivityCompletedHandler>())
                    handler.OnDomesticActivityCompleted(activity);

                DomesticActivityCompleted?.Invoke(activity);
                Events.Publish(WorldEventTriggers.DomesticActivityCompleted, activity);
                return true;
            }

            case ScriptEvent.TalkCompleted:
            {
                var officer = GetOfficer(checked((ushort)officerId));
                var partner = GetOfficer(checked((ushort)value));
                var talk = new TalkInfo(SessionId, officer, partner);

                foreach (var handler in officer.GetComponents<ITalkCompletedHandler>())
                    handler.OnTalkCompleted(talk);

                if (!ReferenceEquals(officer, partner))
                    foreach (var handler in partner.GetComponents<ITalkCompletedHandler>())
                        handler.OnTalkCompleted(talk);

                TalkCompleted?.Invoke(talk);
                Events.Publish(WorldEventTriggers.TalkCompleted, talk);
                return true;
            }

            default:
                return false;
        }
    }
}
