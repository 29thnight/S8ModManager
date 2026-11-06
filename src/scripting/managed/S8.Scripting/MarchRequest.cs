using System.Runtime.InteropServices;

namespace S8.Scripting;

public enum MarchRequestStatus : uint
{
    Queued = 1,
    Executing,
    Invoked,
    RejectedByGame,
    DestinationNotAdjacent,
    NoFaction,
    InvalidEntity,
    OwnershipChanged,
    TargetNoLongerEligible,
    NoPrivilege,
    ApprovalAuthorityChanged
}

public readonly record struct MarchRequestResult(
    MarchRequestStatus Status,
    byte GameReturn,
    ushort SourceCityId,
    ushort DestinationCityId)
{
    public bool IsCompleted => Status is not (MarchRequestStatus.Queued or MarchRequestStatus.Executing);

    // The native execution path returned; this is not a victory or occupation result.
    public bool WasInvoked => Status == MarchRequestStatus.Invoked;
}

public sealed class MarchRequest
{
    private readonly ulong ticket;
    private MarchRequestResult? completed;

    public ulong SessionId { get; }

    public ushort SourceCityId { get; }

    public ushort DestinationCityId { get; }

    internal MarchRequest(ulong ticket, ulong session, ushort sourceCity, ushort destinationCity)
    {
        this.ticket = ticket;
        SessionId = session;
        SourceCityId = sourceCity;
        DestinationCityId = destinationCity;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Receipt
    {
        public MarchRequestStatus Status;
        public uint GameReturn;
        public ushort SourceCity;
        public ushort DestinationCity;
        public uint Reserved;
    }

    /// <summary>
    /// Reads a pending request during a game callback. The first completed read
    /// consumes its native receipt; later reads return this handle's cached result.
    /// Unread receipts from a previous session are rejected by the native bridge.
    /// </summary>
    public unsafe MarchRequestResult GetResult()
    {
        if (completed is { } previous)
            return previous;

        Receipt receipt;
        Native.Call(Operation.ReadCityMarchResult, ticket, checked((long)SessionId), &receipt,
            (uint)sizeof(Receipt));

        var result = new MarchRequestResult(receipt.Status, (byte)receipt.GameReturn,
            receipt.SourceCity, receipt.DestinationCity);
        if (result.IsCompleted)
            completed = result;

        return result;
    }
}
