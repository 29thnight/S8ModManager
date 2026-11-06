using System.Runtime.InteropServices;

namespace S8.Scripting;

public sealed record AiCityTarget(GameObject Target, uint EstimatedDefense);

public sealed class CityAiComponent : Component
{
    private CityComponent city = null!;

    public override void OnEnable() => city = GameObject.GetComponent<CityComponent>()
        ?? throw new InvalidOperationException("This component requires a city GameObject.");

    [StructLayout(LayoutKind.Sequential)]
    private unsafe struct Request
    {
        public ulong Session;
        public uint Count;
        public uint Reserved;
        public fixed uint Candidates[12];
    }

    /// <summary>
    /// Selects the adjacent eligible city with the lowest game defense estimate.
    /// Returns null when no candidate exists. Does not create a march plan or execute an attack.
    /// This is a target-selection policy, not the game's complete faction strategy update.
    /// </summary>
    public AiCityTarget? SelectAttackTarget() => GetAttackCandidates()
        .MinBy(candidate => candidate.EstimatedDefense);

    /// <summary>
    /// Returns adjacent cities admitted by the game's faction-target check and
    /// their native defense estimates. Scripts can apply their own selection policy.
    /// </summary>
    public unsafe AiCityTarget[] GetAttackCandidates()
    {
        Request request = new() { Session = city.Memory.Session };
        Native.Call(Operation.ReadCityAttackCandidates, city.Id, data: &request,
            bytes: (uint)sizeof(Request));

        var candidates = new AiCityTarget[request.Count];
        for (int i = 0; i < candidates.Length; ++i)
            candidates[i] = new(GameObject.World!.GetCity((byte)request.Candidates[i * 2])!,
                request.Candidates[i * 2 + 1]);

        return candidates;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MarchSubmission
    {
        public ulong Session;
        public ulong Ticket;
        public ushort Proposer, Approver, Governor, Lord;
    }

    /// <summary>
    /// Queues the game's city AI march path for this city and the supplied destination.
    /// Runs after managed callbacks return at a measured personal/calendar boundary.
    /// Requests already queued at that boundary are processed in order. Requests
    /// created by nested callbacks wait for the next boundary. Changed city ownership
    /// or a failed current target check cancels the request before native execution.
    /// The game chooses officers and units and can reject the request under its own rules.
    /// This does not force player participation or a battle-control prompt. The original
    /// game decides whether a battle is needed and whether the player can control it.
    /// </summary>
    public unsafe MarchRequest RequestMarch(GameObject destination)
        => RequestMarch(destination, false);

    /// <summary>
    /// Queues a protagonist proposal with one native privilege consumption at execution.
    /// The source must be the protagonist's city and have a distinct governor or lord.
    /// Runs on the city command update after the mod confirmation dialogs close.
    /// One privilege is consumed before entering the original march routine, even if
    /// that routine subsequently rejects the march. This API does not show proposal dialogue.
    /// </summary>
    public MarchRequest RequestPrivilegedMarch(GameObject destination, ushort approvingOfficerId)
        => RequestMarch(destination, true, approvingOfficerId);

    private unsafe MarchRequest RequestMarch(GameObject destination, bool usePrivilege, ushort approvingOfficerId = 0)
    {
        var target = destination.GetComponent<CityComponent>()
            ?? throw new ArgumentException("The destination requires a city GameObject.", nameof(destination));
        if (target.Memory.Session != city.Memory.Session)
            throw new EngineCallException(ConnectionStatus.StaleSession, nameof(RequestMarch));

        MarchSubmission request = new()
        {
            Session = city.Memory.Session,
            Proposer = usePrivilege ? GameObject.World!.GetPlayer().Character.GetComponent<OfficerComponent>()!.Id : (ushort)0,
            Approver = approvingOfficerId
        };
        if (usePrivilege)
        {
            var affiliation = GameObject.GetComponent<CityAffiliationComponent>()!.Read();
            request.Governor = affiliation.GovernorId ?? 0;
            request.Lord = affiliation.LordId ?? 0;
            ushort expected = request.Governor != 0 && request.Governor != request.Proposer
                ? request.Governor : request.Lord;
            if (expected == 0 || expected == request.Proposer || expected != request.Approver)
                throw new InvalidOperationException("The approving officer no longer holds the required authority.");
        }
        if (Native.Call(usePrivilege ? Operation.RequestPrivilegedCityMarch : Operation.RequestCityMarch,
                city.Id, target.Id, &request,
                usePrivilege ? (uint)sizeof(MarchSubmission) : 16u) == 0)
            throw new InvalidOperationException("The march request queue is full. Read completed request results first.");

        return new(request.Ticket, request.Session, city.Id, target.Id);
    }
}
