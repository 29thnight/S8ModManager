using System.Runtime.InteropServices;

namespace S8.Scripting;

// SDK names for the two integers returned by the game's generic action check.
public enum ActionAvailabilityStatus : int
{
    Unavailable = 1,
    InsufficientResources = 2,
    Available = 3
}

public enum ActionUnavailableReason : int
{
    Allowed = 1,
    RoleNotAllowed = 2,
    PlayerRestriction = 3,
    InsufficientGold = 4,
    InsufficientActionPoints = 5
}

public readonly record struct ActionAvailability(
    ActionAvailabilityStatus Status,
    ActionUnavailableReason Reason)
{
    public bool CanAct => Status == ActionAvailabilityStatus.Available;
}

public enum GameActionRequestStatus : int
{
    Invoked = 1,
    ActionUnavailable = 2,
    TargetUnavailable = 3
}

public readonly record struct DomesticActivityRequestResult(
    GameActionRequestStatus Status,
    ActionAvailability Availability,
    int TargetCondition,
    int TargetReason)
{
    // Invoked means the game routine returned, not that a particular stat changed.
    public bool WasInvoked => Status == GameActionRequestStatus.Invoked;
}

public sealed class OfficerActionsComponent : OfficerDataComponent
{
    [StructLayout(LayoutKind.Sequential)]
    private struct Request
    {
        public ulong Session;
        public ActionAvailabilityStatus Status;
        public ActionUnavailableReason Reason;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ExecutionRequest
    {
        public ulong Session;
        public ActionAvailabilityStatus Availability;
        public ActionUnavailableReason Reason;
        public GameActionRequestStatus Status;
        public int TargetCondition;
        public int TargetReason;
        public uint Reserved;
    }

    /// <summary>
    /// Checks the game's generic role, gold and action-point requirements.
    /// The ID is the game's action ID (1..100), not a menu row index.
    /// Destination and partner conditions require their own action-specific checks.
    /// Call during a game callback; no action is executed or explanation UI opened.
    /// </summary>
    public unsafe ActionAvailability CheckAvailability(byte actionId)
    {
        Request request = new() { Session = Officer.Memory.Session };

        Native.Call(Operation.QueryOfficerActionAvailability, Officer.Id, actionId,
            &request, (uint)sizeof(Request));

        return new(request.Status, request.Reason);
    }

    /// <summary>
    /// Synchronously requests a native domestic action (IDs 1, 11, 21, 31 or 41).
    /// Checks the officer and current city before entering the game's execution routine.
    /// The game handles costs, results and applicable presentation; no extra participants
    /// or custom native callbacks are supplied. Requires a game callback.
    /// </summary>
    public unsafe DomesticActivityRequestResult RequestDomesticActivity(byte actionId)
    {
        ExecutionRequest request = new() { Session = Officer.Memory.Session };

        Native.Call(Operation.RequestDomesticActivity, Officer.Id, actionId,
            &request, (uint)sizeof(ExecutionRequest));

        return new(request.Status, new(request.Availability, request.Reason), request.TargetCondition,
            request.TargetReason);
    }
}
