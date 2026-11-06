namespace S8.Scripting;

public enum CityStrategyGoal { Rebuild, Defend, LimitedOffensive }

public enum ProposalStatus { Pending, Deferred, Rejected, Submitted, Completed, Interrupted }

public sealed record CityStrategyOrder(
    CityStrategyGoal Goal, bool DelegateMarch, ushort? TargetCityId = null);

public sealed class MarchProposal
{
    public ushort OfficerId { get; internal init; }

    public ushort SourceCityId { get; internal init; }

    public ushort TargetCityId { get; internal init; }

    public byte ForceId { get; internal init; }

    public GameDate Date { get; internal init; }

    public ProposalStatus Status { get; internal set; }

    public MarchRequest? Request { get; internal set; }

    public MarchRequestResult? Result { get; internal set; }
}

// Session-owned policy layer. Native AI remains responsible for formation,
// eligibility, costs and the player's battle participation prompt.
public sealed class CouncilStrategyComponent : Component,
    IMonthChangedHandler, ICouncilEnteredHandler
{
    private readonly Dictionary<ushort, (byte Force, CityStrategyOrder Order)> orders = [];
    private readonly Dictionary<ushort, GameDate> submissions = [];
    private readonly Dictionary<ushort, GameDate> evaluations = [];
    private readonly List<MarchProposal> proposals = [];
    private GameDate? date;
    private const string SaveKey = "s8.council-strategy";

    private sealed record SavedOrder(ushort City, byte Force, CityStrategyOrder Order);
    private sealed record SavedProposal(ushort Officer, ushort Source, ushort Target,
        byte Force, GameDate Date, ProposalStatus Status, MarchRequestResult? Result);
    private sealed record SavedStrategy(int Version, GameDate? Date, SavedOrder[] Orders,
        Dictionary<ushort, GameDate> Submissions, Dictionary<ushort, GameDate> Evaluations,
        SavedProposal[] Proposals);

    private void Persist() => GameObject.World!.SaveData.Set(SaveKey, new SavedStrategy(
        1, date, orders.Select(pair => new SavedOrder(pair.Key, pair.Value.Force, pair.Value.Order)).ToArray(),
        submissions, evaluations, proposals.Select(p => new SavedProposal(p.OfficerId,
            p.SourceCityId, p.TargetCityId, p.ForceId, p.Date, p.Status, p.Result)).ToArray()));

    public GameDate? CurrentDate => date;

    public bool HasSubmittedThisMonth(ushort officerId) =>
        date is { } current && submissions.GetValueOrDefault(officerId) == current;

    public bool HasMarchAttemptThisMonth(ushort cityId) =>
        date is { } current && evaluations.GetValueOrDefault(cityId) == current;

    public IReadOnlyList<MarchProposal> Proposals => proposals.AsReadOnly();

    public event Action<MarchProposal>? ProposalChanged;

    public event Action<GameDate>? QuarterlyCouncil;

    public override void OnEnable()
    {
        var saved = GameObject.World!.SaveData.Get<SavedStrategy>(SaveKey);
        if (saved != null)
        {
            if (saved.Version != 1 || saved.Orders == null || saved.Submissions == null ||
                saved.Evaluations == null || saved.Proposals == null ||
                saved.Orders.Any(o => o == null || o.Order == null || !Enum.IsDefined(o.Order.Goal)) ||
                saved.Orders.Select(o => o.City).Distinct().Count() != saved.Orders.Length ||
                saved.Proposals.Any(p => p == null || !Enum.IsDefined(p.Status)))
                throw new InvalidDataException("Unsupported council strategy save data.");

            date = saved.Date;
            foreach (var order in saved.Orders)
                orders.Add(order.City, (order.Force, order.Order));
            foreach (var entry in saved.Submissions)
                submissions.Add(entry.Key, entry.Value);
            foreach (var entry in saved.Evaluations)
                evaluations.Add(entry.Key, entry.Value);
            foreach (var proposal in saved.Proposals)
                proposals.Add(new MarchProposal
                {
                    OfficerId = proposal.Officer, SourceCityId = proposal.Source,
                    TargetCityId = proposal.Target, ForceId = proposal.Force, Date = proposal.Date,
                    Status = proposal.Status == ProposalStatus.Submitted
                        ? ProposalStatus.Interrupted : proposal.Status,
                    Result = proposal.Result
                });
        }
        GameObject.World.DomesticActivityCompleted += OnActivityCompleted;
        Persist();
    }

    public override void OnDisable()
    {
        GameObject.World!.DomesticActivityCompleted -= OnActivityCompleted;
    }

    public void SetCityOrder(ushort cityId, CityStrategyOrder order)
    {
        var world = GameObject.World!;
        var affiliation = world.GetCity(cityId).GetComponent<CityAffiliationComponent>()!.Read();
        if (affiliation.ForceId is not { } force)
            throw new ArgumentException("The city must belong to a faction.", nameof(cityId));

        if (order.TargetCityId is { } target)
            world.GetCity(target);

        orders[cityId] = (force, order);
        Persist();
    }

    public CityStrategyOrder? GetCityOrder(ushort cityId) =>
        orders.TryGetValue(cityId, out var entry) ? entry.Order : null;

    // One proposal per officer per observed game month. Caller supplies no
    // date or faction; both come from the current session.
    public MarchProposal SubmitMarch(ushort officerId, ushort targetCityId)
        => SubmitMarch(officerId, targetCityId, null);

    public MarchProposal SubmitPrivilegedMarch(ushort officerId, ushort targetCityId, ushort approvingOfficerId)
        => SubmitMarch(officerId, targetCityId, approvingOfficerId);

    private MarchProposal SubmitMarch(ushort officerId, ushort targetCityId, ushort? approvingOfficerId)
    {
        if (date is not { } current)
            throw new InvalidOperationException("Wait for the first month or council boundary.");

        var world = GameObject.World!;
        if (approvingOfficerId != null && world.GetPlayer().Character.GetComponent<OfficerComponent>()!.Id != officerId)
            throw new InvalidOperationException("Only the current protagonist can use a player privilege.");
        var officer = world.GetOfficer(officerId).GetComponent<OfficerAffiliationComponent>()!.Read();
        if (officer.CityId is not { } source || officer.ForceId is not { } force)
            throw new InvalidOperationException("The proposer must belong to a faction and city.");

        if (submissions.GetValueOrDefault(officerId) == current)
            throw new InvalidOperationException("This officer has already submitted a proposal this month.");

        if (!world.GetCity(source).GetComponent<CityAiComponent>()!.GetAttackCandidates()
                .Any(candidate => candidate.Target.GetComponent<CityComponent>()!.Id == targetCityId))
            throw new ArgumentException("The target is not an eligible adjacent city.", nameof(targetCityId));

        var proposal = new MarchProposal
        {
            OfficerId = officerId, SourceCityId = source, TargetCityId = targetCityId,
            ForceId = force, Date = current
        };
        if (approvingOfficerId is { } approver)
        {
            if (evaluations.GetValueOrDefault(source) == current ||
                proposals.Any(p => p.SourceCityId == source && p.Status == ProposalStatus.Submitted))
                throw new InvalidOperationException("This city already has a march attempt or request this month.");

            proposal.Request = world.GetCity(source).GetComponent<CityAiComponent>()!
                .RequestPrivilegedMarch(world.GetCity(targetCityId), approver);
            proposal.Status = ProposalStatus.Submitted;
            evaluations[source] = current;
        }
        submissions[officerId] = current;
        proposals.Add(proposal);
        Persist();
        ProposalChanged?.Invoke(proposal);
        return proposal;
    }

    // The deciding officer must be the current city governor or faction lord.
    // Delegation authorizes that same city's offensive plan between councils.
    public void Decide(MarchProposal proposal, ushort decidingOfficerId, bool approve, bool defer = false)
    {
        if (!proposals.Contains(proposal) ||
            proposal.Status is not (ProposalStatus.Pending or ProposalStatus.Deferred))
            throw new ArgumentException("The proposal is not awaiting a decision.", nameof(proposal));

        var city = GameObject.World!.GetCity(proposal.SourceCityId);
        var affiliation = city.GetComponent<CityAffiliationComponent>()!.Read();
        if (affiliation.ForceId != proposal.ForceId ||
            (affiliation.GovernorId != decidingOfficerId && affiliation.LordId != decidingOfficerId))
            throw new InvalidOperationException("The deciding officer does not command this city.");

        if (approve)
            Submit(proposal, city);
        else
            proposal.Status = defer ? ProposalStatus.Deferred : ProposalStatus.Rejected;

        Persist();
        ProposalChanged?.Invoke(proposal);
    }

    public void OnMonthChanged(MonthChangedInfo month)
    {
        date = month.Current;
        RefreshResults();
        for (ushort id = 1; id <= 51; ++id)
            ReevaluateCity(id);
        Persist();
    }

    public void OnCouncilEntered(CouncilEnteredInfo council)
    {
        date = council.Date;
        RefreshResults();
        if (council.Date.Month is 1 or 4 or 7 or 10)
            QuarterlyCouncil?.Invoke(council.Date);
        Persist();
    }

    private void OnActivityCompleted(DomesticActivityInfo activity)
    {
        RefreshResults();
        var city = activity.Officer.GetComponent<OfficerAffiliationComponent>()!.Read().CityId;
        if (city is { } id)
            ReevaluateCity(id);
        Persist();
    }

    // Also callable from subsequently verified ownership/supply-change hooks.
    // At most one execution attempt per city/month, including native rejection.
    public void ReevaluateCity(ushort cityId)
    {
        if (date is not { } current || evaluations.GetValueOrDefault(cityId) == current ||
            proposals.Any(p => p.SourceCityId == cityId && p.Status == ProposalStatus.Submitted))
            return;

        var world = GameObject.World!;
        var city = world.GetCity(cityId);
        var affiliation = city.GetComponent<CityAffiliationComponent>()!.Read();
        if (affiliation.ForceId is not { } force)
            return;

        CityStrategyOrder order;
        if (orders.TryGetValue(cityId, out var entry))
        {
            if (entry.Force != force)
            {
                orders.Remove(cityId);
                Persist();
                return;
            }
            order = entry.Order;
        }
        else
        {
            var playerForce = world.GetPlayer().Character.GetComponent<OfficerAffiliationComponent>()!.Read().ForceId;
            if (force == playerForce)
                return;

            order = new(CityStrategyGoal.LimitedOffensive, true);
        }

        if (!order.DelegateMarch || order.Goal != CityStrategyGoal.LimitedOffensive)
            return;

        var target = city.GetComponent<CityAiComponent>()!.GetAttackCandidates()
            .Where(candidate => order.TargetCityId == null ||
                candidate.Target.GetComponent<CityComponent>()!.Id == order.TargetCityId)
            .MinBy(candidate => candidate.EstimatedDefense);
        if (target == null)
            return;

        var proposal = new MarchProposal
        {
            SourceCityId = cityId, TargetCityId = target.Target.GetComponent<CityComponent>()!.Id,
            ForceId = force, Date = current
        };
        proposals.Add(proposal);
        Submit(proposal, city);
        Persist();
        ProposalChanged?.Invoke(proposal);
    }

    private void Submit(MarchProposal proposal, GameObject city)
    {
        if (date is not { } current)
            throw new InvalidOperationException("Wait for the first month or council boundary.");

        if (evaluations.GetValueOrDefault(proposal.SourceCityId) == current)
            throw new InvalidOperationException("This city has already attempted a march this month.");

        if (proposals.Any(p => p != proposal && p.SourceCityId == proposal.SourceCityId &&
                p.Status == ProposalStatus.Submitted))
            throw new InvalidOperationException("This city already has a pending march request.");

        proposal.Request = city.GetComponent<CityAiComponent>()!
            .RequestMarch(GameObject.World!.GetCity(proposal.TargetCityId));
        proposal.Status = ProposalStatus.Submitted;
        evaluations[proposal.SourceCityId] = current;
    }

    public void RefreshResults()
    {
        bool changed = false;
        foreach (var proposal in proposals.Where(p => p.Status == ProposalStatus.Submitted).ToArray())
        {
            var result = proposal.Request!.GetResult();
            if (!result.IsCompleted)
                continue;

            proposal.Result = result;
            proposal.Status = ProposalStatus.Completed;
            changed = true;
            ProposalChanged?.Invoke(proposal);
        }
        if (changed)
            Persist();
    }
}
