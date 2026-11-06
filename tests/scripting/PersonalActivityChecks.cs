using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using S8.Scripting;

internal static unsafe class PersonalActivityChecks
{
    private static ulong session = 70;

    private sealed class Counter : Component, IDomesticActivityCompletedHandler, ITalkCompletedHandler,
        IMonthChangedHandler, ICouncilEnteredHandler
    {
        public Counter() { }

        public int Repairs, Talks, Months, Councils, Disables;

        public void OnDomesticActivityCompleted(DomesticActivityInfo activity) => ++Repairs;

        public void OnTalkCompleted(TalkInfo talk) => ++Talks;

        public void OnMonthChanged(MonthChangedInfo month) => ++Months;

        public void OnCouncilEntered(CouncilEnteredInfo council) => ++Councils;

        public override void OnDisable() => ++Disables;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static long Invoke(int operation, ulong target, long argument, void* data, uint bytes)
    {
        if (operation == 29)
        {
            new Span<byte>(data, (int)bytes).Clear();
            *(ulong*)data = session;
            *(ushort*)((byte*)data + 8) = 1;
            return 0;
        }

        if (operation == 30 || operation == 31)
        {
            new Span<byte>(data, (int)bytes).Clear();
            *(ulong*)data = 0x100000 + target * 0x1000;
            *(ulong*)((byte*)data + 8) = session;
            *(ushort*)((byte*)data + 16) = (ushort)target;
            return 1;
        }

        return (int)ConnectionStatus.InvalidApi;
    }

    public static int Run()
    {
        int checks = 0;
        void Check(bool value, string message)
        {
            if (!value)
                throw new Exception(message);
            ++checks;
        }

        NativeApi api = new()
        {
            Version = NativeContract.Version,
            Size = (uint)sizeof(NativeApi),
            Connection = new() { Capabilities = NativeCapability.GameData },
            Invoke = &Invoke
        };
        nint previous = Native.Enter(&api);

        try
        {
            using var world = new GameWorld();
            var officer = world.GetOfficer(1);
            var partner = world.GetOfficer(2);
            var unrelated = world.GetOfficer(3).AddComponent<Counter>();
            var counter = officer.AddComponent<Counter>();
            var partnerCounter = partner.AddComponent<Counter>();
            int repairSignals = 0, talkSignals = 0;
            DomesticActivityInfo? repair = null;
            TalkInfo? talk = null;

            world.DomesticActivityCompleted += value => repair = value;
            world.TalkCompleted += value => talk = value;
            world.Events.Attach(new ScriptedWorldEvent("repair-check", WorldEventTriggers.DomesticActivityCompleted,
                _ => true, _ => ++repairSignals));
            world.Events.Attach(new ScriptedWorldEvent("talk-check", WorldEventTriggers.TalkCompleted,
                _ => true, _ => ++talkSignals));

            Check(world.DispatchActivity(ScriptEvent.DomesticActivityCompleted, 1, 0x15), "Repair event handled");
            Check(counter.Repairs == 1 && partnerCounter.Repairs == 0 && unrelated.Repairs == 0,
                "Repair reaches only its officer component");
            Check(repair?.Kind == DomesticActivityKind.GateRepair && repair.SessionId == session
                && ReferenceEquals(repair.Officer, officer), "Repair payload retains canonical entity and session");
            Check(repairSignals == 1, "World event receives the repair trigger once");

            Check(world.DispatchActivity(ScriptEvent.TalkCompleted, 1, 2), "Talk event handled");
            Check(counter.Repairs == 1 && counter.Talks == 1 && partnerCounter.Talks == 1 && unrelated.Talks == 0,
                "Both participants receive talk while component fields persist across activity types");
            Check(talk?.SessionId == session && ReferenceEquals(talk.Officer, officer)
                && ReferenceEquals(talk.Partner, partner), "Talk payload identifies both canonical participants");
            Check(talkSignals == 1, "World event receives the talk trigger once");
            Check(ReferenceEquals(world.GetOfficer(1).GetComponent<Counter>(), counter),
                "Resolving an officer does not recreate its component");
            Check(!world.DispatchActivity(ScriptEvent.Click, 0, 0), "UI events are not consumed as activities");

            var city = world.GetCity(1).AddComponent<Counter>();
            MonthChangedInfo? month = null;
            CouncilEnteredInfo? council = null;
            int monthSignals = 0, councilSignals = 0;
            world.MonthChanged += value => month = value;
            world.CouncilEntered += value => council = value;
            world.Events.Attach(new ScriptedWorldEvent("month-check", WorldEventTriggers.MonthChanged,
                _ => true, _ => ++monthSignals));
            world.Events.Attach(new ScriptedWorldEvent("council-check", WorldEventTriggers.CouncilEntered,
                _ => true, _ => ++councilSignals));

            Check(world.DispatchActivity(ScriptEvent.MonthChanged, (12 << 16) | 192, (1 << 16) | 193),
                "Month event handled");
            Check(month?.Previous == new GameDate(192, 12) && month.Current == new GameDate(193, 1)
                && month.SessionId == session, "Month payload preserves year rollover and session");
            Check(counter.Months == 1 && partnerCounter.Months == 1 && unrelated.Months == 1 && city.Months == 1,
                "Month reaches all materialized officer and city components once");
            Check(monthSignals == 1, "Month world trigger fires once");

            Check(world.DispatchActivity(ScriptEvent.CouncilEntered, 0, (1 << 16) | 193), "Council event handled");
            Check(council?.Date == new GameDate(193, 1) && council.SessionId == session,
                "Council payload preserves date and session");
            Check(counter.Councils == 1 && partnerCounter.Councils == 1 && unrelated.Councils == 1 && city.Councils == 1
                && counter.Months == 1 && counter.Talks == 1, "Council retains existing component state");
            Check(councilSignals == 1, "Council world trigger fires once");

            session = 71;
            world.RefreshSession();
            Check(counter.Disables == 1 && partnerCounter.Disables == 1 && unrelated.Disables == 1,
                "Changing save sessions releases activity components");
            Check(!ReferenceEquals(world.GetOfficer(1), officer)
                && world.GetOfficer(1).GetComponent<Counter>() == null, "Component state does not leak to a new session");
        }
        finally
        {
            Native.Leave(previous);
        }

        return checks;
    }
}
