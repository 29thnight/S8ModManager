using System.Runtime.InteropServices;

namespace S8.Scripting;

public static class NativeContract
{
    public static readonly uint Version = (uint)typeof(NativeContract).Assembly.GetName().Version!.Major;
}

[Flags]
public enum NativeCapability : ulong
{
    None = 0,
    Ui = 1,
    GameData = 2,
    Memory = 4
}

public enum ConnectionStatus
{
    Ready = 0,
    InvalidApi = -1,
    ApiVersionMismatch = -2,
    UnsupportedGameBuild = -3,
    CodeMismatch = -4,
    MissingCapability = -5,
    RuntimeUnavailable = -6,
    ScriptFailure = -7,
    InvalidBinding = -8,
    MemoryAccessDenied = -9,
    StaleSession = -10
}

[StructLayout(LayoutKind.Sequential)]
public struct EngineConnection
{
    public ConnectionStatus Status;
    public uint GameBuild;
    public NativeCapability Capabilities;
}

public enum ScriptEvent
{
    Open,
    Select,
    Click,
    Closing,
    Closed,
    Resume,
    TextInput,
    NumberInput,
    Configure,
    MenuClick,
    WindowClick,
    WindowClosing,
    TreeClick,
    BattleTurnStarted,
    BattleUnitActionEnded,
    BattleTurnEnded,
    BattleSessionEnded,
    DomesticActivityCompleted,
    TalkCompleted,
    MonthChanged,
    CouncilEntered,
    CouncilRoomReady,
    TabChanged
}

internal enum Operation
{
    ShowDialog,
    CloseDialog,
    SetRow,
    InputText,
    InputNumber,
    ReadInputText,
    GetPersonality,
    ReadPersonalityName,
    ReadPersonalityDescription,
    InspectMemory,
    ReadMemory,
    WriteMemory,
    GetSession,
    RegisterMenu,
    RegisterWindowRow,
    RequestOverlay,
    ReadGameState,
    SubmitGameCommand,
    SetWindowDirty,
    SetTreeNodes,
    SetTreeFooter,
    GetTreeCategory,
    GetGemCategory,
    ReadSelection,
    ReleaseUiRegistration,
    ReadBattleUnits,
    SetBattleSoldiers,
    ShowBattlePersonality,
    GetBattleContext,
    GetWorldContext,
    ResolveOfficer,
    ResolveCity,
    ShowOfficerDialogue,
    ReadEntityValue,
    SetEntityValue,
    AddEntityValue,
    AddFriendshipTokens,
    ReadOfficerAffiliation,
    ReadCityAffiliation,
    QueryOfficerActionAvailability,
    ReadCityAttackCandidates,
    RequestDomesticActivity,
    RequestCityMarch,
    ReadCityMarchResult,
    ReadWorldSaveData,
    StageWorldSaveData,
    ReadPlayerPrivilege,
    RequestPrivilegedCityMarch,
    DisableDialogRow,
    ReadCityName,
    CloseCouncilRoom,
    OpenCouncilRoom,
    RegisterUiSprite,
    FindUiSprite,
    ApplyUiSprite,
    ReadBattleSoldiers,
    SetFormButtonBounds,
    HideFormDecoration,
    SetFormButtonLabel,
    SetFormButtonIcon,
    ReadArchiveResource,
    SetJewelForceMask,
    SetTraitCatalog,
    SetDialogTabs,
    SetRowBadge
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct NativeApi
{
    public uint Version, Size;
    public EngineConnection Connection;
    public delegate* unmanaged[Cdecl]<int, ulong, long, void*, uint, long> Invoke;
}

public static unsafe class Native
{
    [ThreadStatic]
    private static NativeApi* api;

    public static nint Enter(NativeApi* value)
    {
        nint previous = (nint)api;

        api = value;
        return previous;
    }

    public static void Leave(nint previous) => api = (NativeApi*)previous;

    internal static long Call(Operation operation, ulong target = 0, long argument = 0, void* data = null, uint bytes = 0)
    {
        if (api == null)
            throw new InvalidOperationException("Engine calls require a game callback.");

        NativeCapability needed = operation switch
        {
            Operation.ShowBattlePersonality => NativeCapability.GameData | NativeCapability.Ui,
            Operation.ShowOfficerDialogue => NativeCapability.GameData | NativeCapability.Ui,
            Operation.ResolveOfficer or Operation.ResolveCity => NativeCapability.GameData,
            Operation.ReadEntityValue or Operation.SetEntityValue or Operation.AddEntityValue => NativeCapability.GameData,
            Operation.AddFriendshipTokens => NativeCapability.GameData,
            Operation.ReadOfficerAffiliation or Operation.ReadCityAffiliation => NativeCapability.GameData,
            Operation.QueryOfficerActionAvailability => NativeCapability.GameData,
            Operation.ReadCityAttackCandidates or Operation.RequestDomesticActivity => NativeCapability.GameData,
            Operation.RequestCityMarch => NativeCapability.GameData | NativeCapability.Ui,
            Operation.RequestPrivilegedCityMarch => NativeCapability.GameData | NativeCapability.Ui,
            Operation.ReadPlayerPrivilege or Operation.ReadCityName => NativeCapability.GameData,
            Operation.ReadWorldSaveData or Operation.StageWorldSaveData => NativeCapability.GameData,
            Operation.ReadCityMarchResult => NativeCapability.GameData,
            Operation.ReadBattleUnits or Operation.SetBattleSoldiers or Operation.ReadBattleSoldiers or Operation.GetBattleContext or Operation.GetWorldContext or
            Operation.GetPersonality or Operation.ReadPersonalityName or Operation.ReadPersonalityDescription
                => NativeCapability.GameData,
            Operation.InspectMemory or Operation.ReadMemory or Operation.WriteMemory or Operation.GetSession
                => NativeCapability.Memory,
            Operation.ReadGameState or Operation.SubmitGameCommand or Operation.GetGemCategory or Operation.SetJewelForceMask or
            Operation.SetTraitCatalog
                => NativeCapability.GameData,
            _ => NativeCapability.Ui
        };

        if ((api->Connection.Capabilities & needed) != needed)
            throw new EngineCallException(ConnectionStatus.MissingCapability, operation.ToString());

        long result = api->Invoke((int)operation, target, argument, data, bytes);

        if (result < 0)
            throw new EngineCallException((ConnectionStatus)result, operation.ToString());

        return result;
    }

    internal static string ReadString(Operation operation, ulong target = 0, long argument = 0)
    {
        char* buffer = stackalloc char[4096];
        int length = checked((int)Call(operation, target, argument, data: buffer, bytes: 8192));

        return new string (buffer, 0, length);
    }
}

public sealed class EngineCallException(ConnectionStatus status, string operation) : InvalidOperationException($"Engine operation {operation} rejected: {status}")
{
    public ConnectionStatus Status { get; } = status;
}
