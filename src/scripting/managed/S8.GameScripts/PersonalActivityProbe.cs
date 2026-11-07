using S8.Scripting;

namespace S8.GameScripts;

// Opt-in diagnostic component. It changes its own fields, not game data.
public sealed class PersonalActivityProbeSetup : WorldSetup
{
    public override void Configure(GameWorld world)
    {
        string directory = world.Application!.Context.ModDirectory;

        if (File.Exists(Path.Combine(directory, "lifecycle-probe.enabled")))
            world.GetPlayer().Character.AddComponent<PersonalActivityProbe>(
                probe => probe.LogPath = Path.Combine(directory, "lifecycle-probe.log"));
    }
}

public sealed class PersonalActivityProbe : Component,
    IDomesticActivityCompletedHandler, ITalkCompletedHandler, IMonthChangedHandler, ICouncilEnteredHandler
{
    private readonly Guid instance = Guid.NewGuid();
    private int repairs, talks, months, councils, total;

    internal string LogPath { get; set; } = null!;

    public override void OnEnable() => Write("enabled");

    public void OnDomesticActivityCompleted(DomesticActivityInfo activity)
    {
        ++repairs;
        ++total;
        Write("repair.completed", $"activity={(int)activity.Kind}");
    }

    public void OnTalkCompleted(TalkInfo talk)
    {
        ++talks;
        ++total;
        Write("talk.completed", $"partner={talk.Partner.GetComponent<OfficerComponent>()!.Id}");
    }

    public void OnMonthChanged(MonthChangedInfo month)
    {
        ++months;
        ++total;
        Write("month.changed", $"previous={month.Previous.Year}/{month.Previous.Month} current={month.Current.Year}/{month.Current.Month}");
    }

    public void OnCouncilEntered(CouncilEnteredInfo council)
    {
        ++councils;
        ++total;
        Write("council.entered", $"date={council.Date.Year}/{council.Date.Month}");
    }

    private void Write(string kind, string details = "")
    {
        var world = GameObject.World!;
        ushort officerId = GameObject.GetComponent<OfficerComponent>()!.Id;
        bool sameObject = ReferenceEquals(world.GetOfficer(officerId), GameObject);
        byte politics = world.Officer(officerId).Politics;

        File.AppendAllText(LogPath,
            $"{DateTimeOffset.Now:O} event={kind} instance={instance:N} session={world.SessionId} " +
            $"officer={officerId} total={total} repairs={repairs} talks={talks} months={months} councils={councils} " +
            $"sameObject={sameObject} politics={politics} managedThread={Environment.CurrentManagedThreadId} {details}\n");
    }
}
