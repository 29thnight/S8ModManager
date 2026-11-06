using System.Runtime.InteropServices;
using System.Text.Json;

namespace S8.Scripting;

public sealed record JewelState(ulong Session, ulong[] Forced, byte[] Opened);

public readonly record struct JewelChange(ushort Id, bool? Opened = null, bool? Forced = null);

// UI definition IDs and native unlock-bit IDs are different namespaces.
public sealed class JewelDefinition
{
    internal JewelDefinition(ushort id, ushort rawId, string name)
    {
        Id = id;
        RawId = rawId;
        Name = name;
    }

    public ushort Id { get; }
    public ushort RawId { get; }
    public string Name { get; }
}

public sealed class JewelCatalog
{
    private readonly Dictionary<ushort, JewelDefinition> definitions = [];

    private JewelCatalog() { }

    public IReadOnlyCollection<JewelDefinition> Definitions => definitions.Values;

    public JewelDefinition Get(ushort id) => definitions[id];

    public static JewelCatalog Load(string path)
    {
        using var document = JsonDocument.Parse(File.ReadAllBytes(path));
        var catalog = new JewelCatalog();
        foreach (var item in document.RootElement.EnumerateArray())
        {
            ushort id = item.GetProperty("id").GetUInt16();
            ushort raw = item.GetProperty("rawId").GetUInt16();
            if (id is 0 or >= 512 || raw is 0 or >= 272)
                throw new InvalidDataException("Invalid jewel definition or unlock-bit ID.");
            string name = item.GetProperty("name").GetString()
                ?? throw new InvalidDataException("A jewel name is required.");
            catalog.definitions.Add(id, new JewelDefinition(id, raw, name));
        }
        return catalog;
    }
}

// Publication and command submission use the manager's game-thread command service.
// These are manager operations, not names inferred for original game setters.
public unsafe class JewelComponent : Component
{
    private ulong? boundSession;
    private bool active;
    private bool playerOwned;

    public override void OnEnable()
    {
        boundSession = GameObject.World?.SessionId;
        playerOwned = GameObject.GetComponent<ProtagonistComponent>() != null;
        active = true;
    }

    public override void OnDisable() => active = false;

    private void RequireOwner()
    {
        if (!active || (playerOwned && GameObject.World!.Protagonist != GameObject))
            throw new InvalidOperationException("The jewel component no longer belongs to the current player.");
    }

    public JewelState Read()
    {
        RequireOwner();
        Publication state;
        Native.Call(Operation.ReadGameState, data: &state, bytes: (uint)sizeof(Publication));

        if (state.Ready == 0 || state.HasBitmap == 0 || state.Pending != 0 ||
            (boundSession is { } session && (state.Epoch != session || GameObject.World!.SessionId != session)))
            throw new InvalidOperationException("The current game state is not editable.");

        var forced = new ulong[8];
        var opened = new byte[34];

        for (int i = 0; i < forced.Length; ++i)
            forced[i] = state.Mask[i];

        for (int i = 0; i < opened.Length; ++i)
            opened[i] = state.Bitmap[i];

        return new(state.Epoch, forced, opened);
    }

    public byte Category(ushort id)
    {
        RequireOwner();
        return checked((byte)Native.Call(Operation.GetGemCategory, id));
    }

    private JewelDefinition Definition(ushort id) => (GameObject.World?.JewelDefinitions
        ?? throw new InvalidOperationException("The jewel catalog is not configured for this world.")).Get(id);

    public bool IsOpened(ushort id)
    {
        var definition = Definition(id);
        var state = Read();
        return (state.Opened[definition.RawId >> 3] & (1 << (definition.RawId & 7))) != 0;
    }

    public bool IsForced(ushort id)
    {
        var definition = Definition(id);
        var state = Read();
        return (state.Forced[definition.Id >> 6] & (1UL << (definition.Id & 63))) != 0;
    }

    public void Enable(ushort id) => Apply([new(id, Opened: true)]);

    public void Disable(ushort id) => Apply([new(id, Opened: false)]);

    public void ForceApply(ushort id) => Apply([new(id, Opened: true, Forced: true)]);

    // One snapshot and one queue submission for a script's whole jewel effect.
    public void Apply(IReadOnlyList<JewelChange> changes)
    {
        var state = Read();
        var commands = new List<(ushort RawId, bool Opened)>();
        bool forcedChanged = false;

        foreach (var change in changes)
        {
            var definition = Definition(change.Id);
            int byteIndex = definition.RawId >> 3;
            byte openedBit = (byte)(1 << (definition.RawId & 7));
            int wordIndex = definition.Id >> 6;
            ulong forcedBit = 1UL << (definition.Id & 63);

            if (change.Opened is { } opened)
            {
                if (opened)
                    state.Opened[byteIndex] |= openedBit;
                else
                {
                    state.Opened[byteIndex] &= (byte)~openedBit;
                    state.Forced[wordIndex] &= ~forcedBit;
                }
                commands.Add((definition.RawId, opened));
            }

            if (change.Forced is { } forced)
            {
                if (forced && (state.Opened[byteIndex] & openedBit) == 0)
                    throw new InvalidOperationException("Open the jewel before forcing its effect.");
                if (forced)
                    state.Forced[wordIndex] |= forcedBit;
                else
                    state.Forced[wordIndex] &= ~forcedBit;
                forcedChanged = true;
            }
        }

        if (commands.Count == 0 && forcedChanged)
            commands.Add((0, false));
        if (commands.Count != 0)
            Submit(state, commands);
    }

    public void Submit(JewelState state, IReadOnlyList<(ushort RawId, bool Opened)> changes)
    {
        RequireOwner();
        if (state.Forced.Length != 8 || state.Opened.Length != 34 || changes.Count is < 1 or > 256)
            throw new ArgumentException("The jewel state or command count is invalid.");

        var commands = new GameCommand[changes.Count];

        fixed (GameCommand* buffer = commands)
        {
            for (int index = 0; index < commands.Length; ++index)
            {
                buffer[index].Epoch = state.Session;
                buffer[index].RawId = changes[index].RawId;
                buffer[index].Open = changes[index].Opened ? (byte)1 : (byte)0;

                for (int word = 0; word < 8; ++word)
                    buffer[index].Mask[word] = state.Forced[word];
            }

            Native.Call(Operation.SubmitGameCommand, data: buffer,
                bytes: checked((uint)(commands.Length * sizeof(GameCommand))));
        }
    }

    // Restores a persisted forced-effect mask. Valid before any save is loaded.
    public static void RestoreForced(IReadOnlyList<ulong> forced)
    {
        if (forced.Count != 8)
            throw new ArgumentException("The forced jewel mask must have 8 words.", nameof(forced));

        var mask = forced.ToArray();

        fixed (ulong* words = mask)
            Native.Call(Operation.SetJewelForceMask, data: words, bytes: (uint)(mask.Length * sizeof(ulong)));
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Publication
    {
        public ulong Epoch;
        public fixed ulong Mask[8];
        public fixed byte Bitmap[34];
        public byte Ready, HasBitmap, Pending, MenusInstalled;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct GameCommand
    {
        public ulong Epoch;
        public fixed ulong Mask[8];
        public ushort RawId;
        public byte Open;
    }
}
