using System.Globalization;
using System.Runtime.InteropServices;

namespace S8.Scripting;

// A choice row on the title screen's options tab: ◀ Choices[Current] ▶, with
// Description shown in the game's description panel while the row is hovered.
public readonly record struct OptionRow(int Action, string Label, string Description, IReadOnlyList<string> Choices,
    int Current = 0);

public enum ModLoadState
{
    Loaded,
    Skipped,
    Failed
}

// One mod the manager found this run. GameBuild 0 and ApiVersion 0 mean "any".
public sealed record ModInfo(
    string Id,
    string Name,
    string Version,
    string Runtime,
    bool Enabled,
    ModLoadState State,
    string Reason,
    int ApiVersion,
    uint GameBuild,
    IReadOnlyList<string> Dependencies,
    IReadOnlyList<string> Conflicts);

public sealed record ModCatalog(int HostApiVersion, uint GameBuild, IReadOnlyList<ModInfo> Mods)
{
    public static unsafe ModCatalog Read()
    {
        int capacity = 8192;
        while (true)
        {
            var buffer = new char[capacity];
            int length;
            fixed (char* data = buffer)
                length = checked((int)Native.Call(Operation.ReadModList, data: data, bytes: (uint)(capacity * sizeof(char))));
            if (length <= capacity)
                return Parse(new string(buffer, 0, length));
            capacity = length;
        }
    }

    // Written to mods.json; the manager applies it on the next start.
    public static unsafe void SetEnabled(string id, bool enabled)
    {
        fixed (char* text = id)
            Native.Call(Operation.SetModEnabled, argument: enabled ? 1 : 0, data: text, bytes: (uint)(id.Length * sizeof(char)));
    }

    // The ReadModList text: a header line (host API, game build) and one line per mod.
    public static ModCatalog Parse(string text)
    {
        var lines = text.Split('\n', StringSplitOptions.RemoveEmptyEntries);
        if (lines.Length == 0)
            throw new FormatException("Empty mod list.");

        var header = lines[0].Split('\t');
        var mods = new List<ModInfo>();
        foreach (var line in lines.Skip(1))
        {
            var f = line.Split('\t');
            if (f.Length != 11)
                throw new FormatException("Mod list line has " + f.Length + " fields.");
            mods.Add(new ModInfo(f[0], f[1], f[2], f[3], f[4] == "1",
                f[5] switch
                {
                    "loaded" => ModLoadState.Loaded,
                    "skipped" => ModLoadState.Skipped,
                    _ => ModLoadState.Failed
                },
                f[6], int.Parse(f[7], CultureInfo.InvariantCulture), Hex(f[8]), List(f[9]), List(f[10])));
        }
        return new ModCatalog(int.Parse(header[0], CultureInfo.InvariantCulture), Hex(header[1]), mods);
    }

    private static uint Hex(string text) =>
        text.Length == 0 ? 0 : uint.Parse(text, NumberStyles.HexNumber, CultureInfo.InvariantCulture);

    private static string[] List(string text) => text.Split(',', StringSplitOptions.RemoveEmptyEntries);
}

internal static unsafe class OptionsTabNative
{
    private static int nextOwner;

    public static int NewOwner() => Interlocked.Increment(ref nextOwner);

    // Copies the rows into native strings for the call; the engine keeps its own copies.
    public static void Register(int owner, string label, IReadOnlyList<OptionRow> rows)
    {
        var allocations = new List<nint>();
        nint Text(string value)
        {
            nint text = Marshal.StringToHGlobalUni(value);
            allocations.Add(text);
            return text;
        }

        try
        {
            var nativeRows = new NativeOptionRow[rows.Count];
            for (int i = 0; i < rows.Count; ++i)
            {
                var choices = (char**)Marshal.AllocHGlobal(rows[i].Choices.Count * sizeof(char*));
                allocations.Add((nint)choices);
                for (int c = 0; c < rows[i].Choices.Count; ++c)
                    choices[c] = (char*)Text(rows[i].Choices[c]);
                nativeRows[i] = new NativeOptionRow
                {
                    Action = rows[i].Action,
                    Current = rows[i].Current,
                    Label = (char*)Text(rows[i].Label),
                    Description = (char*)Text(rows[i].Description),
                    Choices = choices,
                    ChoiceCount = rows[i].Choices.Count
                };
            }

            fixed (NativeOptionRow* pointer = nativeRows)
            {
                var tab = new NativeOptionsTab { Owner = owner, Count = rows.Count, Label = (char*)Text(label), Rows = pointer };
                Native.Call(Operation.SetOptionsTab, data: &tab, bytes: (uint)sizeof(NativeOptionsTab));
            }
        }
        finally
        {
            foreach (nint allocation in allocations)
                Marshal.FreeHGlobal(allocation);
        }
    }

    public static void Release(int owner) => Native.Call(Operation.ReleaseUiRegistration, (ulong)owner);

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeOptionRow
    {
        public int Action, Current;
        public char* Label;
        public char* Description;
        public char** Choices;
        public int ChoiceCount, Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeOptionsTab
    {
        public int Owner, Count;
        public char* Label;
        public NativeOptionRow* Rows;
    }
}
