using System.Runtime.InteropServices;

namespace S8.Scripting;

// A session's player controls an existing officer entity. Changing protagonist
// changes Character, without replacing the officer registry or this Player.
public sealed class Player
{
    private readonly GameWorld world;
    private readonly ulong session;
    private bool active = true;

    internal Player(GameWorld world)
    {
        this.world = world;
        session = world.SessionId;
        Resources = new PlayerResources(this);
    }

    public GameObject Character
    {
        get
        {
            if (!active || world.SessionId != session || world.Protagonist == null)
                throw new InvalidOperationException("The player is no longer available in this session.");
            return world.Protagonist;
        }
    }

    public PlayerResources Resources { get; }

    public PlayerJewelsComponent Jewels => Character.GetComponent<PlayerJewelsComponent>()!;

    internal void Invalidate() => active = false;
}

public sealed class PlayerJewelsComponent : JewelComponent
{
    public override void OnEnable()
    {
        if (GameObject.World?.Protagonist != GameObject)
            throw new InvalidOperationException("Player jewels require the current protagonist GameObject.");
        base.OnEnable();
    }
}

public sealed class PlayerResources
{
    private readonly Player player;

    internal PlayerResources(Player player) => this.player = player;

    public uint Money => player.Character.GetComponent<OfficerStatsComponent>()!.Gold;

    public unsafe byte Privileges
    {
        get
        {
            var officer = player.Character.GetComponent<OfficerComponent>()!;
            return checked((byte)Native.Call(Operation.ReadPlayerPrivilege, officer.Id,
                checked((long)officer.Memory.Session)));
        }
    }

    public uint SetMoney(uint value) => player.Character.GetComponent<OfficerStatsComponent>()!.SetGold(value);

    public uint AddMoney(int delta) => player.Character.GetComponent<OfficerStatsComponent>()!.AddGold(delta);

    public unsafe void AddFriendshipTokens(int delta, bool showMessage = false)
    {
        var officer = player.Character.GetComponent<OfficerComponent>()!;
        TokenChange change = new() { Delta = delta, ShowMessage = showMessage ? 1u : 0u };
        Native.Call(Operation.AddFriendshipTokens, officer.Id, checked((long)officer.Memory.Session),
            &change, (uint)sizeof(TokenChange));
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct TokenChange
    {
        public int Delta;
        public uint ShowMessage;
    }
}
