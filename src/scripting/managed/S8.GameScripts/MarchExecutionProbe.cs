using S8.Scripting;

namespace S8.GameScripts;

// Explicitly armed, one-shot game execution test. No polling or automatic retry.
public sealed class MarchExecutionProbeSetup : WorldSetup
{
    public override void Configure(GameWorld world)
    {
        string directory = Path.Combine(world.Application!.Context.GameDirectory, "scripts");

        if (File.Exists(Path.Combine(directory, "march-probe.enabled")))
            world.GetPlayer().Character.AddComponent<MarchExecutionProbe>(
                probe =>
                {
                    probe.Directory = directory;
                    probe.MonthOnly = File.Exists(Path.Combine(directory, "march-probe.month-only"));
                    probe.PlayerCityOnly = File.Exists(Path.Combine(directory, "march-probe.player-city"));
                });
    }
}

public sealed class MarchExecutionProbe : Component,
    IDomesticActivityCompletedHandler, IMonthChangedHandler, ICouncilEnteredHandler
{
    private MarchRequest? request;
    private bool attempted;

    internal string Directory { get; set; } = null!;

    internal bool MonthOnly { get; set; }

    internal bool PlayerCityOnly { get; set; }

    public void OnDomesticActivityCompleted(DomesticActivityInfo activity) => Run("domestic.completed");

    public void OnMonthChanged(MonthChangedInfo month) => Run("month.changed");

    public void OnCouncilEntered(CouncilEnteredInfo council) => Run("council.entered");

    private void Run(string boundary)
    {
        if (request != null)
        {
            var result = request.GetResult();
            Write($"boundary={boundary} source={result.SourceCityId} target={result.DestinationCityId} " +
                $"status={result.Status} nativeReturn={result.GameReturn}");
            return;
        }

        if (attempted || (MonthOnly && boundary != "month.changed") ||
            !File.Exists(Path.Combine(Directory, "march-probe.arm")))
            return;

        attempted = true;
        File.Move(Path.Combine(Directory, "march-probe.arm"),
            Path.Combine(Directory, $"march-probe.used-{DateTime.UtcNow:yyyyMMddHHmmssfff}"));

        var world = GameObject.World!;
        var affiliation = world.GetPlayer().Character.GetComponent<OfficerAffiliationComponent>()!.Read();
        byte? force = affiliation.ForceId;
        Write($"boundary={boundary} playerCity={affiliation.CityId} playerRole={affiliation.RoleId} " +
            $"playerCityOnly={PlayerCityOnly}");

        for (ushort id = 1; id <= 51; ++id)
        {
            if (PlayerCityOnly && id != affiliation.CityId)
                continue;

            var city = world.GetCity(id);
            if (force == null || city.GetComponent<CityAffiliationComponent>()!.Read().ForceId != force)
                continue;

            var ai = city.GetComponent<CityAiComponent>()!;
            var target = PlayerCityOnly
                ? ai.GetAttackCandidates()
                    .Where(candidate => candidate.EstimatedDefense > 1 &&
                        candidate.Target.GetComponent<CityAffiliationComponent>()!.Read().ForceId != null)
                    .MinBy(candidate => candidate.EstimatedDefense)
                : ai.SelectAttackTarget();
            if (target == null)
                continue;

            request = ai.RequestMarch(target.Target);
            Write($"boundary={boundary} source={id} target={request.DestinationCityId} " +
                $"defense={target.EstimatedDefense} status={request.GetResult().Status}");
            return;
        }

        Write($"boundary={boundary} no eligible adjacent target for player faction={force}");
    }

    private void Write(string text) => File.AppendAllText(Path.Combine(Directory, "march-probe.log"),
        $"{DateTimeOffset.Now:O} session={GameObject.World!.SessionId} {text}\n");
}
