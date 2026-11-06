using S8.Scripting;

namespace S8.GameScripts;

public sealed class CouncilStrategySetup : WorldSetup
{
    private GameWorld? observedWorld;
    private Action<MonthChangedInfo>? monthChanged;

    public override void Configure(GameWorld world)
    {
        var strategy = world.GetPlayer().Character.AddComponent<CouncilStrategyComponent>();
        world.Events.Attach(new CouncilStrategyMenuEvent(strategy));
        world.Events.Attach(new CouncilStrategyMenuEvent(strategy, inCouncilRoom: true));

        string directory = world.Application!.Context.ModDirectory;
        if (!File.Exists(Path.Combine(directory, "council-strategy.trace")))
            return;

        string path = Path.Combine(directory, "council-strategy.log");
        void Write(string text) => File.AppendAllText(path,
            $"{DateTimeOffset.Now:O} session={world.SessionId} {text}\n");

        Write("enabled");
        strategy.ProposalChanged += proposal => Write(
            $"proposal date={proposal.Date.Year}/{proposal.Date.Month} officer={proposal.OfficerId} " +
            $"source={proposal.SourceCityId} target={proposal.TargetCityId} force={proposal.ForceId} " +
            $"status={proposal.Status} result={proposal.Result?.Status} nativeReturn={proposal.Result?.GameReturn}");
        strategy.QuarterlyCouncil += date => Write($"quarterly-council date={date.Year}/{date.Month}");
        observedWorld = world;
        monthChanged = month => Write(
            $"month date={month.Current.Year}/{month.Current.Month} proposals={strategy.Proposals.Count}");
        world.MonthChanged += monthChanged;
    }

    public override void Dispose()
    {
        if (observedWorld != null)
            observedWorld.MonthChanged -= monthChanged;
    }
}
