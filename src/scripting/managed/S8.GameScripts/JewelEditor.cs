using S8.Scripting;
using System.Text.Json;

namespace S8.GameScripts;

public sealed class JewelEditorModule : IScriptModule
{
    private readonly UiScene dataScene = new();
    private JewelComponent data = null!;
    private Definition[] definitions = [];
    private readonly Dictionary<int, Draft> drafts = [];
    private UiScene? scene;
    private DialogComponent treeDialog = null!;
    private GemTreeComponent tree = null!;
    private Draft draft = null!;
    private int window;
    private string forcedPath = "";

    public bool HasDrafts => drafts.Count != 0;

    public void Initialize(ModContext context)
    {
        data = dataScene.CreateObject("PlayerJewels").AddComponent<JewelComponent>();
        string catalogPath = Path.Combine(context.AssetDirectory, "jewel_definitions.json");
        using var document = JsonDocument.Parse(File.ReadAllBytes(catalogPath));
        definitions = document.RootElement.EnumerateArray()
            .Select(item => new Definition(item.GetProperty("id").GetUInt16(),
                item.GetProperty("rawId").GetUInt16(), item.GetProperty("name").GetString()!))
            .ToArray();

        if (definitions.Any(item => item.Id >= 512 || item.RawId >= 272) ||
            definitions.Select(item => item.Id).Distinct().Count() != definitions.Length ||
            definitions.Select(item => item.RawId).Distinct().Count() != definitions.Length)
            throw new InvalidDataException("The jewel catalog contains invalid or duplicate identifiers.");

        // The forced-effect mask is mod state; the game save only keeps the opened bitmap.
        forcedPath = Path.Combine(context.GameDirectory, "S8.Editors.jewels.json");
        if (File.Exists(forcedPath))
        {
            using var saved = JsonDocument.Parse(File.ReadAllBytes(forcedPath));
            JewelComponent.RestoreForced(saved.RootElement.GetProperty("forced").EnumerateArray()
                .Select(word => word.GetUInt64()).ToArray());
        }
    }

    private void SaveForced(ulong[] forced)
    {
        string temporary = forcedPath + "." + Guid.NewGuid().ToString("N") + ".tmp";
        File.WriteAllText(temporary, "{\"forced\":[" + string.Join(",", forced) + "]}");
        File.Move(temporary, forcedPath, true);
    }

    public void Dispatch(ScriptEvent kind, int owner, int value)
    {
        if (kind == ScriptEvent.WindowClosing)
        {
            if (!drafts.Remove(owner, out var transaction))
                return;

            if (value == 1)
                Commit(transaction);

            return;
        }

        if (kind == ScriptEvent.WindowClick)
        {
            Open(owner);
            return;
        }

        if (kind == ScriptEvent.Closed)
        {
            scene?.Dispose();
            scene = null;
            return;
        }

        scene?.Dispatch(kind, owner, value);
    }

    private void Open(int owner)
    {
        var current = data.Read();
        window = owner;

        foreach (int stale in drafts.Where(item => item.Value.Initial.Session != current.Session)
                     .Select(item => item.Key).ToArray())
            drafts.Remove(stale);

        if (!drafts.TryGetValue(owner, out var retained))
        {
            draft = new Draft(current);
            drafts[owner] = draft;
        }
        else
        {
            draft = retained;
        }

        scene?.Dispose();
        scene = new UiScene();
        treeDialog = scene.Create("JewelTree");
        tree = treeDialog.GameObject.AddComponent<GemTreeComponent>();
        tree.Dialog = treeDialog;
        tree.NodeClicked = id =>
        {
            var definition = definitions.First(item => item.Id == id);
            SetOpened(definition, !IsOpened(definition));
            Refresh();
        };
        tree.PrimaryClicked = OpenForcedList;
        tree.SecondaryClicked = () =>
        {
            int category = tree.Category;

            foreach (var definition in definitions)
                if (data.Category(definition.Id) == category)
                    SetOpened(definition, false);

            Refresh();
        };
        tree.BackRequested = () => treeDialog.Close(false);
        treeDialog.Resumed = Refresh;
        treeDialog.Show(DialogKind.GemTree, "보주 편집", [new(0, "확인", "")], caption: "편집할 보주를 선택해 주십시오");
        Refresh();
    }

    private bool IsOpened(Definition definition) =>
        (draft.Opened[definition.RawId >> 3] & (1 << (definition.RawId & 7))) != 0;

    private void SetOpened(Definition definition, bool opened)
    {
        byte bit = (byte)(1 << (definition.RawId & 7));

        if (opened)
            draft.Opened[definition.RawId >> 3] |= bit;
        else
        {
            draft.Opened[definition.RawId >> 3] &= (byte)~bit;
            draft.Forced[definition.Id >> 6] &= ~(1UL << (definition.Id & 63));
        }
    }

    private void Refresh()
    {
        tree.SetNodes(definitions.Select(item => new TreeNode(item.Id, IsOpened(item) ? (byte)1 : (byte)0, 1)).ToArray());
        tree.SetFooter(0, "강제적용");
        tree.SetFooter(1);
        MenuExtensionComponent.SetWindowDirty(window,
            !draft.Opened.SequenceEqual(draft.Initial.Opened) || !draft.Forced.SequenceEqual(draft.Initial.Forced));
    }

    private void OpenForcedList()
    {
        var available = definitions.Where(IsOpened).ToArray();
        var selection = (ulong[])draft.Forced.Clone();
        var dialog = scene!.Create("ForcedJewels");

        dialog.Closing = (accepted, _) =>
        {
            if (!accepted)
                return;

            foreach (var item in available)
                selection[item.Id >> 6] &= ~(1UL << (item.Id & 63));

            foreach (int id in dialog.ReadSelection())
                selection[id >> 6] |= 1UL << (id & 63);

            draft.Forced = selection;
        };
        dialog.Show(DialogKind.MultiSelect, "보주 강제적용",
            available.Select(item => new UiRow(item.Id, item.Name, $"보조계통 강제적용 · ID {item.Id}")).ToArray(),
            caption: "강제적용할 개방 보주를 선택해 주십시오",
            initialSelection: available.Where(item => (selection[item.Id >> 6] & (1UL << (item.Id & 63))) != 0)
                .Select(item => (int)item.Id).ToArray());
    }

    private void Commit(Draft transaction)
    {
        var result = new JewelState(transaction.Initial.Session, transaction.Forced, transaction.Opened);
        var changes = new List<(ushort RawId, bool Opened)>();

        foreach (var item in definitions)
        {
            int bit = 1 << (item.RawId & 7);
            bool opened = (transaction.Opened[item.RawId >> 3] & bit) != 0;
            bool before = (transaction.Initial.Opened[item.RawId >> 3] & bit) != 0;

            if (opened == before)
                continue;

            changes.Add((item.RawId, opened));
        }

        if (changes.Count == 0 && !transaction.Forced.SequenceEqual(transaction.Initial.Forced))
            changes.Add((0, false));

        if (changes.Count == 0)
            return;

        data.Submit(result, changes);
        if (!transaction.Forced.SequenceEqual(transaction.Initial.Forced))
            SaveForced(transaction.Forced);
    }

    public void Dispose()
    {
        scene?.Dispose();
        scene = null;
        drafts.Clear();
        dataScene.Dispose();
    }

    private sealed record Definition(ushort Id, ushort RawId, string Name);

    private sealed class Draft(JewelState initial)
    {
        public JewelState Initial { get; } = initial;
        public byte[] Opened { get; } = (byte[])initial.Opened.Clone();
        public ulong[] Forced { get; set; } = (ulong[])initial.Forced.Clone();
    }
}
