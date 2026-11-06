using System.Runtime.InteropServices;

namespace S8.Scripting;

public enum InputResult
{
    Unrepresentable = -2,
    InvalidEncoding = -1,
    Cancelled = 0,
    Accepted = 1
}

public enum DialogKind
{
    List,
    Form,
    CompactForm,
    Text,
    MultilineText,
    Message,
    GemTree,
    MultiSelect,
    CityMap
}

// Tab: the form tab that shows this row (see DialogComponent.Show tabs).
public readonly record struct UiRow(int Action, string Label, string Text, int Height = 64, bool Enabled = true, int Tab = 0);
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeRow
{
    public int Action, Height;
    public char* Label;
    public char* Text;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct DialogSpec
{
    public int Owner;
    public DialogKind Kind;
    public int ConfirmCloses, Reserved;
    public char* Title;
    public char* Caption;
    public NativeRow* Rows;
    public int Count, Padding;
    public int* Selection;
    public int SelectionCount, SelectionPadding;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct DialogTabs
{
    public char** Labels;
    public int Count, Reserved;
    public fixed byte RowTabs[64];
}

public sealed class UiScene : IDisposable
{
    private readonly Dictionary<int, DialogComponent> dialogs = [];
    private readonly List<GameObject> objects = [];
    private static int nextOwner;
    // Which scene created each live dialog, so ScriptMod can route dialog events.
    private static readonly Dictionary<int, UiScene> owners = [];

    internal bool ContainsOwner(int owner) => dialogs.ContainsKey(owner);

    internal static bool Route(ScriptEvent kind, int owner, int value)
    {
        if (!owners.TryGetValue(owner, out var scene))
            return false;
        scene.Dispatch(kind, owner, value);
        return true;
    }

    public GameObject CreateObject(string name)
    {
        var gameObject = new GameObject(name);

        objects.Add(gameObject);
        return gameObject;
    }

    public DialogComponent Create(string name)
    {
        var gameObject = CreateObject(name);
        var dialog = gameObject.AddComponent<DialogComponent>();

        dialog.Owner = checked(++nextOwner);
        dialogs.Add(dialog.Owner, dialog);
        owners.Add(dialog.Owner, this);
        return dialog;
    }

    public void Dispatch(ScriptEvent kind, int owner, int value)
    {
        if (!dialogs.TryGetValue(owner, out var dialog))
            return;

        try
        {
            dialog.Dispatch(kind, value);
        }
        finally
        {
            if (kind == ScriptEvent.Closing)
            {
                dialogs.Remove(owner);
                owners.Remove(owner);
                objects.Remove(dialog.GameObject);
                dialog.GameObject.Dispose();
            }
        }
    }

    public void Dispose()
    {
        foreach (var gameObject in objects)
            gameObject.Dispose();

        objects.Clear();
        foreach (int owner in dialogs.Keys)
            owners.Remove(owner);
        dialogs.Clear();
    }
}

public sealed unsafe class DialogComponent : Component
{
    public int Owner { get; internal set; }

    public Action<int>? Selected, Clicked;
    public Action<bool, int>? Closing;
    public Action? Resumed;
    public Action<InputResult, string>? TextCompleted;
    public Action<bool, int>? NumberCompleted;
    public Action<int>? TreeClicked;
    // Raised with the new tab index when the player switches a form tab.
    public Action<int>? TabChanged;

    public void Show(
        DialogKind kind,
        string title,
        IReadOnlyList<UiRow> rows,
        bool confirmCloses = true,
        string caption = "항목을 선택해 주십시오",
        IReadOnlyList<int>? initialSelection = null,
        ushort sourceCityId = 0,
        IReadOnlyList<string>? tabs = null)
    {
        if (kind is not (DialogKind.Form or DialogKind.CompactForm) && rows.Any(row => !row.Enabled))
            throw new ArgumentException("Disabled rows require a form dialog.", nameof(rows));
        if (tabs != null && (kind != DialogKind.Form || tabs.Count is < 2 or > 8 ||
                             rows.Any(row => row.Action is < 0 or >= 64 || row.Tab < 0 || row.Tab >= tabs.Count)))
            throw new ArgumentException("Tabs need a form dialog, 2..8 tabs and rows with actions 0..63 on an existing tab.", nameof(tabs));
        if (tabs == null && rows.Any(row => row.Tab != 0))
            throw new ArgumentException("Row tabs require dialog tabs.", nameof(rows));

        var allocations = new List<nint>();
        var nativeRows = new NativeRow[rows.Count];
        int[] selection = initialSelection?.ToArray() ?? [];

        try
        {
            for (int i = 0; i < rows.Count; ++i)
            {
                nint label = Marshal.StringToHGlobalUni(rows[i].Label), text = Marshal.StringToHGlobalUni(rows[i].Text);

                allocations.Add(label);
                allocations.Add(text);
                nativeRows[i] = new NativeRow
                {
                    Action = rows[i].Action,
                    Height = rows[i].Height,
                    Label = (char*)label,
                    Text = (char*)text
                };
            }

            fixed (char* t = title, c = caption)
            fixed (NativeRow* r = nativeRows)
            fixed (int* selected = selection)
            {
                var spec = new DialogSpec
                {
                    Owner = Owner,
                    Kind = kind,
                    ConfirmCloses = confirmCloses ? 1 : 0,
                    Reserved = sourceCityId,
                    Title = t,
                    Caption = c,
                    Rows = r,
                    Count = rows.Count,
                    Selection = selected,
                    SelectionCount = selection.Length
                };

                // Tabs are part of the dialog's construction, so they precede ShowDialog.
                if (tabs != null)
                    SetTabs(tabs, rows, allocations);
                Native.Call(Operation.ShowDialog, data: &spec, bytes: (uint)sizeof(DialogSpec));
            }

            foreach (var row in rows)
                if (!row.Enabled)
                    Native.Call(Operation.DisableDialogRow, (ulong)Owner, row.Action);
        }
        finally
        {
            foreach (nint allocation in allocations)
                Marshal.FreeHGlobal(allocation);
        }
    }

    private void SetTabs(IReadOnlyList<string> tabs, IReadOnlyList<UiRow> rows, List<nint> allocations)
    {
        var labels = new char*[tabs.Count];
        for (int i = 0; i < tabs.Count; ++i)
        {
            nint label = Marshal.StringToHGlobalUni(tabs[i]);
            allocations.Add(label);
            labels[i] = (char*)label;
        }

        var spec = new DialogTabs { Count = tabs.Count };
        foreach (var row in rows)
            spec.RowTabs[row.Action] = (byte)row.Tab;

        fixed (char** pointers = labels)
        {
            spec.Labels = pointers;
            Native.Call(Operation.SetDialogTabs, (ulong)Owner, data: &spec, bytes: (uint)sizeof(DialogTabs));
        }
    }

    public int[] ReadSelection()
    {
        var selection = new int[8192];
        int count;

        fixed (int* values = selection)
            count = checked((int)Native.Call(Operation.ReadSelection, (ulong)Owner,
                data: values, bytes: (uint)(selection.Length * sizeof(int))));

        return selection[..count];
    }

    public void Close(bool accepted = true) => Native.Call(Operation.CloseDialog, (ulong)Owner, accepted ? 1 : 0);

    // Replaces an existing image slot on a native council command button.
    public void ApplyRowSprite(int rowAction, int imageSlot, string resourceName) =>
        UiSprites.Apply(resourceName, Owner, rowAction, imageSlot);

    public void HideFormDecoration() => Native.Call(Operation.HideFormDecoration, (ulong)Owner);

    public void SetButtonIcon(int action, int resource, int x, int y, int size)
    {
        int* icon = stackalloc int[] { resource, x, y, size };
        Native.Call(Operation.SetFormButtonIcon, (ulong)Owner, action, icon, 4 * sizeof(int));
    }

    public void SetButtonLabel(int action, int x, int y, int fontSize)
    {
        int* style = stackalloc int[] { x, y, fontSize };
        Native.Call(Operation.SetFormButtonLabel, (ulong)Owner, action, style, 3 * sizeof(int));
    }

    public void SetButtonBounds(int action, int x, int y, int width, int height)
    {
        int* bounds = stackalloc int[] { x, y, width, height };
        Native.Call(Operation.SetFormButtonBounds, (ulong)Owner, action, bounds, 4 * sizeof(int));
    }

    // Draws the row's value as a game badge: a pill image with the text centred on it.
    // Call right after Show to lay it out; later calls swap the image and text colour.
    public void SetRowBadge(int action, int resource, int width, int height, int palette)
    {
        int* badge = stackalloc int[] { resource, width, height, palette };
        Native.Call(Operation.SetRowBadge, (ulong)Owner, action, badge, 4 * sizeof(int));
    }

    public void SetRow(UiRow row)
    {
        fixed (char* l = row.Label, t = row.Text)
        {
            var value = new NativeRow
            {
                Action = row.Action,
                Height = row.Height,
                Label = l,
                Text = t
            };

            Native.Call(Operation.SetRow, (ulong)Owner, data: &value, bytes: (uint)sizeof(NativeRow));
        }
    }

    public void EditText(string initial, int maximumCharacters, bool multiline = false)
    {
        fixed (char* text = initial)
        {
            var config = new TextConfig
            {
                Initial = text,
                Limit = maximumCharacters,
                Multiline = multiline ? 1 : 0
            };

            Native.Call(Operation.InputText, (ulong)Owner, data: &config, bytes: (uint)sizeof(TextConfig));
        }
    }

    public void EditNumber(int initial, int minimum = 0, int maximum = 65535)
    {
        var config = new NumberConfig
        {
            Initial = initial,
            Minimum = minimum,
            Maximum = maximum
        };

        Native.Call(Operation.InputNumber, (ulong)Owner, data: &config, bytes: (uint)sizeof(NumberConfig));
    }

    internal void Dispatch(ScriptEvent kind, int value)
    {
        switch (kind)
        {
            case ScriptEvent.Select:
                Selected?.Invoke(value);
                break;
            case ScriptEvent.Click:
                Clicked?.Invoke(value);
                break;
            case ScriptEvent.Closing:
                Closing?.Invoke(value >= 0, value);
                break;
            case ScriptEvent.Resume:
                Resumed?.Invoke();
                break;
            case ScriptEvent.TextInput:
                TextCompleted?.Invoke((InputResult)value, value == 1 ? Native.ReadString(Operation.ReadInputText) : "");
                break;
            case ScriptEvent.TreeClick:
                TreeClicked?.Invoke(value);
                break;
            case ScriptEvent.NumberInput:
                NumberCompleted?.Invoke(value >= 0, value);
                break;
            case ScriptEvent.TabChanged:
                TabChanged?.Invoke(value);
                break;
        }
    }

    public override void OnDisable()
    {
        Selected = Clicked = null;
        Closing = null;
        Resumed = null;
        TextCompleted = null;
        NumberCompleted = null;
        TreeClicked = null;
        TabChanged = null;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct TextConfig
    {
        public char* Initial;
        public int Limit, Multiline;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NumberConfig
    {
        public int Initial, Minimum, Maximum, Reserved;
    }
}

public static unsafe class UiArchive
{
    public static byte[] ReadResource(uint archiveHash, long recordOffset)
    {
        int size = checked((int)Native.Call(Operation.ReadArchiveResource, archiveHash, recordOffset));
        if (size == 0)
            return [];
        byte[] data = new byte[size];
        fixed (byte* buffer = data)
            Native.Call(Operation.ReadArchiveResource, archiveHash, recordOffset, buffer, (uint)size);
        return data;
    }
}