using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using S8.Scripting;

internal static unsafe class CouncilStrategyChecks
{
    private static byte[] saved = [];
    private static int requests;
    private static bool completed;
    private static byte cityForce = 1;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static long Invoke(int operation, ulong target, long argument, void* data, uint bytes)
    {
        if (operation == 44)
        {
            if (data == null)
                return saved.Length;
            saved.CopyTo(new Span<byte>(data, (int)bytes));
            return 0;
        }
        if (operation == 45)
        {
            saved = new ReadOnlySpan<byte>(data, (int)bytes).ToArray();
            return 0;
        }
        if (operation == 29)
        {
            new Span<byte>(data, (int)bytes).Clear();
            *(ulong*)data = 90;
            *(ushort*)((byte*)data + 8) = 1;
            return 0;
        }

        if (operation is 30 or 31)
        {
            new Span<byte>(data, (int)bytes).Clear();
            *(ulong*)data = 0x100000 + target * 0x1000;
            *(ulong*)((byte*)data + 8) = 90;
            *(ushort*)((byte*)data + 16) = (ushort)target;
            return 1;
        }

        if (operation is 37 or 38)
        {
            new Span<byte>(data, (int)bytes).Clear();
            *(ushort*)((byte*)data + 2) = 100;
            *(ushort*)((byte*)data + 4) = 10;
            ((byte*)data)[9] = operation == 37 ? (byte)1 : target == 1 ? cityForce : (byte)2;
            ((byte*)data)[10] = 1;
            ((byte*)data)[11] = 1 | 2 | 4 | 8;
            return 0;
        }

        if (operation == 40)
        {
            *(uint*)((byte*)data + 8) = target <= 2 ? 1U : 0;
            *(uint*)((byte*)data + 16) = 3;
            *(uint*)((byte*)data + 20) = 500;
            return 0;
        }

        if (operation == 42)
        {
            *(ulong*)((byte*)data + 8) = (ulong)++requests;
            return 1;
        }

        if (operation == 43)
        {
            new Span<byte>(data, (int)bytes).Clear();
            *(uint*)data = completed ? 4U : 1U;
            return 0;
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

        void Reject(Action action, string message)
        {
            bool rejected = false;
            try { action(); }
            catch (InvalidOperationException) { rejected = true; }
            Check(rejected, message);
        }

        saved = [];
        requests = 0;
        completed = false;
        cityForce = 1;
        NativeApi api = new()
        {
            Version = NativeContract.Version,
            Size = (uint)sizeof(NativeApi),
            Connection = new() { Capabilities = NativeCapability.GameData | NativeCapability.Ui },
            Invoke = &Invoke
        };
        nint previous = Native.Enter(&api);
        try
        {
            using var world = new GameWorld(scriptAssembly: typeof(Component).Assembly);
            var strategy = world.GetPlayer().Character.AddComponent<CouncilStrategyComponent>();
            int councils = 0;
            strategy.QuarterlyCouncil += _ => ++councils;
            world.DispatchActivity(ScriptEvent.CouncilEntered, 0, (7 << 16) | 212);
            Check(councils == 1, "Quarterly council delivered");

            var proposal = strategy.SubmitMarch(1, 3);
            Reject(() => strategy.SubmitMarch(1, 3), "One proposal per officer per month");
            Reject(() => strategy.Decide(proposal, 999, true), "Unrelated officer cannot approve");
            strategy.Decide(proposal, 10, false, defer: true);
            Check(proposal.Status == ProposalStatus.Deferred && requests == 0, "Deferral does not execute");
            strategy.Decide(proposal, 100, true);
            Check(proposal.Status == ProposalStatus.Submitted && requests == 1, "Lord approves native request");

            var second = strategy.SubmitMarch(2, 3);
            Reject(() => strategy.Decide(second, 10, true), "Same city cannot submit another march this month");
            strategy.Decide(second, 10, false);
            Check(second.Status == ProposalStatus.Rejected, "Governor rejects without execution");

            strategy.SetCityOrder(1, new(CityStrategyGoal.LimitedOffensive, true, 3));
            strategy.ReevaluateCity(1);
            Check(requests == 1, "Delegation respects monthly limit");
            completed = true;
            world.DispatchActivity(ScriptEvent.CouncilEntered, 0, (8 << 16) | 212);
            Check(proposal.Status == ProposalStatus.Completed &&
                proposal.Result?.Status == MarchRequestStatus.RejectedByGame, "Native rejection is retained");
            Check(councils == 1, "Nonquarterly boundary does not emit quarterly council");

            world.DispatchActivity(ScriptEvent.MonthChanged, (8 << 16) | 212, (9 << 16) | 212);
            Check(requests == 3, "New month evaluates delegated player city and enemy city");
            strategy.ReevaluateCity(1);
            strategy.ReevaluateCity(2);
            Check(requests == 3, "Repeat evaluation does not duplicate requests");

            cityForce = 2;
            world.DispatchActivity(ScriptEvent.MonthChanged, (9 << 16) | 212, (10 << 16) | 212);
            Check(strategy.GetCityOrder(1) == null, "Ownership change releases previous delegation");
            strategy.SetCityOrder(2, new(CityStrategyGoal.Defend, true));
            world.DispatchActivity(ScriptEvent.MonthChanged, (10 << 16) | 212, (11 << 16) | 212);
            Check(!strategy.Proposals.Any(p => p.SourceCityId == 2 && p.Date.Month == 11),
                "Defensive order stops extra march path");

            var oldStore = world.SaveData;
            int beforeRestore = requests;
            world.EndSession();
            Check(world.Protagonist == null, "Strategy owner ends with the session");
            Reject(() => oldStore.Set("stale", 1), "Ended session cannot stage save data");

            using var restoredWorld = new GameWorld(scriptAssembly: typeof(Component).Assembly);
            var restored = restoredWorld.GetPlayer().Character.AddComponent<CouncilStrategyComponent>();
            Check(restored.GetCityOrder(2)?.Goal == CityStrategyGoal.Defend,
                "City delegation survives save data restoration");
            Check(restored.Proposals.Count == strategy.Proposals.Count &&
                restored.Proposals.Any(p => p.Status == ProposalStatus.Interrupted),
                "Pending requests restore as interrupted history");
            Check(requests == beforeRestore, "Restoring history never submits a march");
            restored.SubmitMarch(1, 3);
            Reject(() => restored.SubmitMarch(1, 3), "Restored date supports the monthly submission limit");
        }
        finally
        {
            Native.Leave(previous);
        }
        return checks;
    }
}
