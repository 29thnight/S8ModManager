using System.Runtime.InteropServices;

namespace S8.Scripting;

public static unsafe class UiSprites
{
    // The caller supplies a verified game resource ID. Names are scoped to the loaded session.
    public static void Register(string name, ushort resourceId) =>
        Call(Operation.RegisterUiSprite, name, argument: resourceId);

    public static int? Find(string name)
    {
        long id = Call(Operation.FindUiSprite, name);
        return id == 0 ? null : checked((int)id);
    }

    internal static void Apply(string name, int owner, int rowAction, int imageSlot)
    {
        long address = ((long)(uint)rowAction << 32) | (uint)imageSlot;
        Call(Operation.ApplyUiSprite, name, (ulong)owner, address);
    }

    private static long Call(Operation operation, string name, ulong target = 0, long argument = 0)
    {
        fixed (char* text = name)
            return Native.Call(operation, target, argument, text, checked((uint)((name.Length + 1) * sizeof(char))));
    }
}

public enum MenuSurface
{
    TitleEdit,
    InGameEdit,
    Function,
    Palace
}

public sealed unsafe class MenuExtensionComponent : Component
{
    private readonly int owner = Native.NewUiId();

    public override void OnDisable() => Native.Call(Operation.ReleaseUiRegistration, (ulong)owner);

    // Returns the action number MenuClick carries for this entry.
    public int AddPalace(string label)
    {
        int action = Native.NewUiId();
        Add(MenuSurface.Palace, action, label, 0, 0);
        return action;
    }

    public void Add(MenuSurface surface, int action, string label, int templateId, int insertAfter)
    {
        fixed (char* text = label)
        {
            var item = new MenuItem
            {
                Surface = surface,
                Action = action,
                TemplateId = templateId,
                InsertAfter = insertAfter,
                Label = text,
                Owner = owner
            };

            Native.Call(Operation.RegisterMenu, data: &item, bytes: (uint)sizeof(MenuItem));
        }
    }

    public void AddWindowRow(int slot, int action, string label, string button)
    {
        fixed (char* l = label, b = button)
        {
            var row = new WindowRow { Slot = slot, Action = action, Label = l, Button = b, Owner = owner };

            Native.Call(Operation.RegisterWindowRow, data: &row, bytes: (uint)sizeof(WindowRow));
        }
    }

    public static void RequestOverlay() => Native.Call(Operation.RequestOverlay);

    public static void SetWindowDirty(int window, bool dirty) =>
        Native.Call(Operation.SetWindowDirty, (ulong)window, dirty ? 1 : 0);

    [StructLayout(LayoutKind.Sequential)]
    private struct MenuItem
    {
        public MenuSurface Surface;
        public int Action, TemplateId, InsertAfter;
        public char* Label;
        public int Owner, Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct WindowRow
    {
        public int Slot, Action;
        public char* Label;
        public char* Button;
        public int Owner, Reserved;
    }
}

[StructLayout(LayoutKind.Sequential)]
public readonly struct TreeNode(ushort id, byte opened, byte enabled)
{
    public readonly ushort Id = id;
    public readonly byte Opened = opened;
    public readonly byte Enabled = enabled;
}

public sealed unsafe class GemTreeComponent : Component
{
    private DialogComponent dialog = null!;

    public Action<ushort>? NodeClicked;
    public Action? PrimaryClicked, SecondaryClicked, BackRequested;

    public DialogComponent Dialog
    {
        get => dialog;
        set
        {
            dialog = value;
            dialog.TreeClicked = action =>
            {
                if (action >= 0)
                    NodeClicked?.Invoke(checked((ushort)action));
                else if (action == -1)
                    PrimaryClicked?.Invoke();
                else if (action == -2)
                    SecondaryClicked?.Invoke();
                else if (action == -3)
                    BackRequested?.Invoke();
            };
        }
    }

    public override void OnDisable()
    {
        if (dialog != null)
            dialog.TreeClicked = null;

        NodeClicked = null;
        PrimaryClicked = SecondaryClicked = BackRequested = null;
    }

    public int Category => checked((int)Native.Call(Operation.GetTreeCategory, (ulong)Dialog.Owner));

    public void SetNodes(ReadOnlySpan<TreeNode> nodes)
    {
        fixed (TreeNode* data = nodes)
            Native.Call(Operation.SetTreeNodes, (ulong)Dialog.Owner, data: data,
                bytes: checked((uint)(nodes.Length * sizeof(TreeNode))));
    }

    // The native resource exposes a custom label for its primary action only.
    public void SetFooter(int action, string? label = null, bool enabled = true)
    {
        fixed (char* text = label)
        {
            var footer = new TreeFooter { Action = action, Enabled = enabled ? 1 : 0, Label = text };

            Native.Call(Operation.SetTreeFooter, (ulong)Dialog.Owner, data: &footer, bytes: (uint)sizeof(TreeFooter));
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct TreeFooter
    {
        public int Action, Enabled;
        public char* Label;
    }
}
