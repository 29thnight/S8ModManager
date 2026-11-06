using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using S8.Scripting;

// ABI/layout and managed composition checks. The real native DLL is only used
// for its rejection paths in this non-game process; no validation bypass exists.
unsafe class Program
{
    static NativeApi api;
    static delegate* unmanaged[Cdecl]<NativeApi*, int, int, int, int> dispatch;
    static delegate* unmanaged[Cdecl]<NativeApi*, int, int, int, int, int> managedDispatch;
    static int managedHandle = -1;
    // A second running mod. The native side delivers every event to every mod, so the
    // harness does too; a mod must act only on dialogs and menus it created.
    static int otherHandle = -1;
    // Menu and window-row actions by label, as the mods last registered them.
    static readonly Dictionary<string, int> actions = [];
    static int checks;
    static readonly List<View> views = [];
    // Trait record fixture; grade (offset 0x2E) is 1 as in real game records.
    static byte[] memory = CreateTraitRecord();

    static byte[] CreateTraitRecord()
    {
        var record = new byte[64];
        record[0x2E] = 1;
        return record;
    }
    static readonly byte[] jewelBitmap = new byte[34];
    static readonly ulong[] jewelMask = new ulong[8];
    static readonly List<(ushort RawId, bool Open, ulong Mask)> jewelCommands = [];
    static int[] selectedJewels = [];
    static bool windowDirty;
    static (byte Present, int Voice, string? Name)? publishedTraits;
    static (int Owner, string[] Labels, byte[] RowTabs)? dialogTabs;
    static (int Owner, int Action, int Resource, int Palette)? rowBadge;
    static int councilUiRequests;
    static bool councilUiAuthority = true;
    static bool councilUiComplete;
    static readonly List<ushort> councilSpeakers = [];
    static readonly List<(int Row, int Resource)> councilSprites = [];
    static int privilegedUiRequests;
    static int councilUiClosures;
    static bool councilRoomOpen;

    static void Check(bool value, string text)
    {
        if (!value)
            throw new Exception(text);

        ++checks;
    }

    static int Send(ScriptEvent kind, int owner = 0, int value = 0)
    {
        fixed (NativeApi* p = &api)
        {
            if (managedDispatch == null)
                return dispatch(p, (int)kind, owner, value);
            int result = managedDispatch(p, managedHandle, (int)kind, owner, value);
            if (otherHandle >= 0)
                managedDispatch(p, otherHandle, (int)kind, owner, value);
            return result;
        }
    }

    static int Main(string[] args)
    {
        if (args.Length == 1 && args[0] == "--council-strategy")
        {
            Console.WriteLine($"Council strategy checks passed: {CouncilStrategyChecks.Run()}");
            return 0;
        }

        Check(typeof(GameObject).GetConstructors().Length == 0, "GameObject exposes no public constructor");
        checks += SetupChecks.Run();
        checks += WorldEventChecks.Run();
        checks += EntityDataChecks.Run();
        checks += PersonalActivityChecks.Run();
        checks += BattleLifetimeChecks.Run();
        checks += LiveSourceChecks.Run();
        checks += OptionsChecks.Run();
        string output = Path.GetFullPath(args[0]);
        nint dll = NativeLibrary.Load(Path.Combine(output, "xinput1_4.dll"));

        dispatch = (delegate* unmanaged[Cdecl]<NativeApi*, int, int, int, int> )NativeLibrary.GetExport(dll, "S8ScriptDispatch");
        api = new NativeApi
        {
            Version = NativeContract.Version,
            Size = (uint)sizeof(NativeApi),
            Invoke = &Invoke,
            Connection = new EngineConnection
            {
                Status = ConnectionStatus.Ready,
                GameBuild = 0x69A67ED1,
                Capabilities = NativeCapability.Ui | NativeCapability.GameData | NativeCapability.Memory
            }
        };
        Check(sizeof(NativeApi) == 32, "ABI v10 layout");
        Check(dispatch(null, 0, 0, 0) == (int)ConnectionStatus.InvalidApi, "Null API rejected");
        api.Version = 1;
        Check(Send(ScriptEvent.Open) == (int)ConnectionStatus.ApiVersionMismatch, "Old ABI rejected");
        api.Version = NativeContract.Version;
        Check(Send(ScriptEvent.Open) == (int)ConnectionStatus.UnsupportedGameBuild, "Non-game executable rejected");
        string host = Path.GetDirectoryName(typeof(S8.Scripting.Host.Entry).Assembly.Location)!;
        string game = Path.GetDirectoryName(host)!;
        string generated = Path.GetFullPath(Path.Combine("tests", "scripting", "bin")) + Path.DirectorySeparatorChar;

        if (!game.StartsWith(generated, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Run this test from the repository with its default generated output directory.");

        string builtMod = Path.Combine(output, "scripts", "mods", "s8.editors");
        string mod = Path.Combine(host, "mods", "s8.editors");
        Directory.CreateDirectory(Path.Combine(mod, "assets"));
        foreach (string file in new[] { "S8.GameScripts.dll", "mod.json", Path.Combine("assets", "effect_definitions.json"), Path.Combine("assets", "jewel_definitions.json") })
            File.Copy(Path.Combine(builtMod, file), Path.Combine(mod, file), true);
        fixed (char* id = "s8.editors", modPath = mod, gamePath = game, assembly = Path.Combine(mod, "S8.GameScripts.dll"), type = "S8.GameScripts.ModApplication")
        {
            var register = (delegate* unmanaged[Cdecl]<char*, char*, char*, char*, char*, int>)&S8.Scripting.Host.Entry.Register;
            managedHandle = register(id, modPath, gamePath, assembly, type);
            Check(managedHandle == 0, "JIT mod registered");
            Check(register(id, modPath, gamePath, assembly, type) < 0, "Same mod id is not registered twice");
        }
        // The game always runs the manager mod beside the editors, and every mod receives
        // every event; dialogs and actions must stay with the mod that created them.
        string builtManager = Path.Combine(output, "scripts", "mods", "s8.manager");
        string manager = Path.Combine(host, "mods", "s8.manager");
        Directory.CreateDirectory(manager);
        foreach (string file in new[] { "S8.ManagerMod.dll", "mod.json" })
            File.Copy(Path.Combine(builtManager, file), Path.Combine(manager, file), true);
        fixed (char* id = "s8.manager", modPath = manager, gamePath = game, assembly = Path.Combine(manager, "S8.ManagerMod.dll"), type = "S8.ManagerMod.ManagerMod")
        {
            var register = (delegate* unmanaged[Cdecl]<char*, char*, char*, char*, char*, int>)&S8.Scripting.Host.Entry.Register;
            otherHandle = register(id, modPath, gamePath, assembly, type);
            Check(otherHandle == 1, "Manager mod registered beside the editors");
        }
        managedDispatch = &S8.Scripting.Host.Entry.Dispatch;
        File.WriteAllText(Path.Combine(game, "S8RPK_traits_default.json"), """{"version":2,"traits":[{"id":1,"index":0,"effects":[{"type":0,"value":0,"param":0},{"type":0,"value":0,"param":0},{"type":0,"value":0,"param":0},{"type":0,"value":0,"param":0},{"type":0,"value":0,"param":0},{"type":0,"value":0,"param":0}]}],"customNames":{}}""");
        // Generated test directory only. Do not share this path with a game install.
        string config = Path.Combine(mod, "san8r_traits_config.json");
        string forcedFile = Path.Combine(mod, "S8.Editors.jewels.json");

        if (File.Exists(forcedFile))
            File.Delete(forcedFile);

        if (File.Exists(config))
            File.Delete(config);

        api.Connection.Status = ConnectionStatus.CodeMismatch;
        Check(Send(ScriptEvent.Open) == (int)ConnectionStatus.CodeMismatch, "C# rejects failed connection");
        api.Connection.Status = ConnectionStatus.Ready;
        api.Connection.Capabilities = NativeCapability.Ui;
        Check(Send(ScriptEvent.Open) == (int)ConnectionStatus.MissingCapability, "Unavailable service rejected when first used");
        api.Connection.Capabilities = NativeCapability.Ui | NativeCapability.GameData | NativeCapability.Memory;
        Check(Send(ScriptEvent.Open) == 0, "Manifest loads a mod");
        Check(Send(ScriptEvent.MenuClick, 0, actions["기재 편집"]) == 0, "Registered menu opens the trait editor");
        Check(views[^1].Kind == DialogKind.List && views[^1].Rows.Count == 1, "Catalog built from engine data");
        int catalog = views[^1].Owner;

        Check(Send(ScriptEvent.Select, catalog, 0) == 0, "Selection dispatch");
        var detail = views[^1];

        Check(detail.Rows.Count == 11 && detail.Rows[0].Label == "이름", "C# composes detail form");
        Check(dialogTabs is { } tabs && tabs.Owner == detail.Owner && tabs.Labels.SequenceEqual(["기본", "효과"]) &&
              tabs.RowTabs[1] == 0 && tabs.RowTabs[10] == 0 && tabs.RowTabs[2] == 1 && tabs.RowTabs[11] == 1,
              "Detail form groups rows into basic and effect tabs");
        Check(detail.Rows.Single(r => r.Action == 10).Label == "기재 등급" &&
              !detail.Rows.Single(r => r.Action == 11).Enabled, "Grade row and disabled script effect row");
        Check(rowBadge is { } badge && badge.Owner == detail.Owner && badge.Action == 10 &&
              badge.Resource is >= 830 and <= 832 && badge.Palette is 0x85 or 0x8F or 0x90,
              "Grade row drawn as the game's trait badge");
        Check(Send(ScriptEvent.TabChanged, detail.Owner, 1) == 0, "Tab change reaches the dialog");
        Check(Send(ScriptEvent.Click, detail.Owner, 2) == 0, "Effect editor dispatch");
        int effects = views[^1].Owner;

        Check(Send(ScriptEvent.Select, effects, 102) == 0, "Effect selection");
        int parameters = views[^1].Owner;

        Check(Send(ScriptEvent.Click, parameters, 1) == 0, "Number input requested");
        Check(Send(ScriptEvent.NumberInput, parameters, 45) == 0, "Number callback");
        Check(views[^1].Rows.Single(r => r.Action == 1).Text == "45", "C# draft updated");
        Check(Send(ScriptEvent.Closing, parameters, -1) == 0, "Parameter cancel");
        Check(Send(ScriptEvent.Closing, effects, 0) == 0, "Effect confirm");
        Check(Send(ScriptEvent.Resume, detail.Owner) == 0, "Refresh through generic SetRow");
        Check(detail.Rows.Single(r => r.Action == 2).Text == "(없음)", "Canceled effect restored by C#");
        Check(Send(ScriptEvent.Click, detail.Owner, 9) == 0, "Voice list composed");
        int voices = views[^1].Owner;

        Check(Send(ScriptEvent.Closing, voices, 3) == 0, "Voice callback");
        Check(Send(ScriptEvent.Closing, detail.Owner, -1) == 0 && !File.Exists(config), "Cancel does not persist");
        Check(Send(ScriptEvent.Closed) == 0, "Components disposed");
        Check(Send(ScriptEvent.MenuClick, 0, actions["기재"]) == 0, "Session reopened from the in-game menu");
        catalog = views[^1].Owner;
        Check(Send(ScriptEvent.Select, catalog, 0) == 0, "Record reopened");
        detail = views[^1];
        Check(Send(ScriptEvent.Click, detail.Owner, 9) == 0, "Voice list reopened");
        Check(Send(ScriptEvent.Closing, views[^1].Owner, 3) == 0, "Voice selected");
        Check(Send(ScriptEvent.Closing, detail.Owner, 0) == 0 && File.Exists(config), "C# mod persists values");
        using (var saved = System.Text.Json.JsonDocument.Parse(File.ReadAllText(config)))
            Check(saved.RootElement.GetProperty("customNames").GetProperty("0").GetProperty("voice").GetUInt16() == 3,
                "Voice is persisted for native application");
        Check(BitConverter.ToUInt16(memory, 0x30) == 0, "Managed editor does not write voice directly");
        Check(publishedTraits is { } published && published.Present == 1 && published.Voice == 3 && published.Name == "대덕",
            "Saved trait is published to the native trait runtime");
        Check(Send(ScriptEvent.Closed) == 0, "Final cleanup");
        Check(Send(ScriptEvent.WindowClick, 42, actions["보주"]) == 0, "Managed jewel window opens");
        int tree = views[^1].Owner;
        Check(views[^1].Kind == DialogKind.GemTree, "Game tree resource requested");
        Check(Send(ScriptEvent.TreeClick, tree, 2) == 0 && windowDirty && jewelCommands.Count == 0,
            "Jewel click changes draft without submission");
        Check(Send(ScriptEvent.Closing, tree, -1) == 0 && Send(ScriptEvent.Closed) == 0,
            "Closing tree retains parent transaction");
        Check(Send(ScriptEvent.WindowClosing, 42, 0) == 0 && jewelCommands.Count == 0,
            "Parent cancel discards jewel changes");

        Check(Send(ScriptEvent.WindowClick, 43, actions["보주"]) == 0, "Jewel transaction reopened");
        tree = views[^1].Owner;
        Check(Send(ScriptEvent.TreeClick, tree, 2) == 0, "Jewel opening staged");
        Check(Send(ScriptEvent.Closing, tree, -1) == 0 && Send(ScriptEvent.Closed) == 0 && jewelCommands.Count == 0,
            "Tree close alone never commits");
        Check(Send(ScriptEvent.WindowClosing, 43, 1) == 0 && jewelCommands.Count == 1 &&
            jewelCommands[0].RawId == 66 && jewelCommands[0].Open,
            "Parent confirm submits catalog raw ID");

        Check(Send(ScriptEvent.WindowClick, 44, actions["보주"]) == 0, "Forced jewel transaction opens");
        tree = views[^1].Owner;
        Check(Send(ScriptEvent.TreeClick, tree, -1) == 0 && views[^1].Kind == DialogKind.MultiSelect,
            "Forced application uses common multi-select");
        selectedJewels = [2];
        Check(Send(ScriptEvent.Closing, views[^1].Owner, 0) == 0 && Send(ScriptEvent.Resume, tree) == 0,
            "Selection result returns to managed draft");
        Check(Send(ScriptEvent.Closing, tree, -1) == 0 && Send(ScriptEvent.Closed) == 0 &&
            Send(ScriptEvent.WindowClosing, 44, 1) == 0 && jewelCommands.Count == 2 &&
            jewelCommands[^1].RawId == 0 && jewelCommands[^1].Mask == 4,
            "Forced mask commits only through parent confirmation");
        Check(File.ReadAllText(forcedFile) == "{\"forced\":[4,0,0,0,0,0,0,0]}",
            "Forced mask is persisted by the mod, not by the cheat config");

        fixed (NativeApi* currentApi = &api)
        {
            nint previousApi = Native.Enter(currentApi);
            try
            {
                using var choiceWorld = new GameWorld(scriptAssembly: typeof(Component).Assembly);
                choiceWorld.BeginSession(1);
                int continuations = 0;
                choiceWorld.Events.Attach(new ScriptedWorldEvent("choice-test", "choice-test", _ => true,
                    context => context.Choose("First", [new(0, "Next", "")], (next, result) =>
                    {
                        ++continuations;
                        next.Message("Second", "After native cleanup", (_, _) => ++continuations);
                    })));
                choiceWorld.Events.Signal("choice-test");
                int firstOwner = views[^1].Owner;
                int countBeforeClose = views.Count;
                choiceWorld.Events.DispatchUi(ScriptEvent.Closing, firstOwner, 0);
                Check(continuations == 0 && views.Count == countBeforeClose,
                    "Closing never opens the successor before native cleanup");
                choiceWorld.Events.DispatchUi(ScriptEvent.Resume, 0, 0);
                Check(continuations == 1 && views.Count == countBeforeClose + 1,
                    "Ready callback opens exactly one successor");
                choiceWorld.Events.DispatchUi(ScriptEvent.Resume, 0, 0);
                Check(continuations == 1, "Repeated ready callback does not replay the choice");
                choiceWorld.Events.DispatchUi(ScriptEvent.Closing, views[^1].Owner, 0);
                choiceWorld.EndSession();
                choiceWorld.Events.DispatchUi(ScriptEvent.Resume, 0, 0);
                Check(continuations == 1, "Session exit discards a pending choice continuation");
            }
            finally
            {
                Native.Leave(previousApi);
            }
        }

        void ChooseCouncilRow(int action)
        {
            var view = views[^1];
            if (view.Kind == DialogKind.CityMap ||
                (view.Kind == DialogKind.List && view.Rows.All(row => row.Label.StartsWith("도시 "))))
                action = view.Rows[action].Action;
            if (!view.Rows.Single(row => row.Action == action).Enabled)
                return;
            int owner = views[^1].Owner;
            var activation = view.Kind == DialogKind.Form ? ScriptEvent.Click : ScriptEvent.Select;
            Check(Send(activation, owner, action) == 0, "Council selection callback");
            Check(Send(ScriptEvent.Closing, owner, action) == 0 && Send(ScriptEvent.Resume, 0) == 0,
                "Council continuation after native cleanup");
        }
        void OpenCouncil(int action)
        {
            // Cancel back through native child windows before reopening the hub.
            for (int depth = 0; councilRoomOpen && depth < 4; ++depth)
            {
                Check(Send(ScriptEvent.Closing, views[^1].Owner, -1) == 0 &&
                    Send(ScriptEvent.Resume, 0) == 0, "Return to council hub");
            }
            Check(!councilRoomOpen, "Previous council room closed");
            int before = views.Count;
            Check(Send(ScriptEvent.MenuClick, 0, 701) == 0 && views.Count == before,
                "Council room waits for the game scene");
            councilSprites.Clear();
            Check(Send(ScriptEvent.CouncilRoomReady) == 0 && views.Count == before + 1 &&
                views[^1].Kind == DialogKind.Form, "Script opens the independent command form");
            Check(councilSprites.SequenceEqual(views[^1].Rows.Select(row =>
                (row.Action, row.Enabled ? 0x61 : 0x66))),
                "Script selects the sprite for each council button's enabled state");
            Check(Send(ScriptEvent.CouncilRoomReady) == 0 && views.Count == before + 1,
                "Duplicate scene-ready notification does not reopen the form");
            ChooseCouncilRow(action);
        }
        Check(Send(ScriptEvent.CouncilEntered, 0, (7 << 16) | 212) == 0, "Council date observed for menu");
        OpenCouncil(0);
        ChooseCouncilRow(0);
        ChooseCouncilRow(0);
        Check(councilUiRequests == 0 && views[^1].Rows[0].Text.Contains("결재 대기"),
            "Submitting through UI does not execute a march");
        ChooseCouncilRow(0);
        OpenCouncil(0);
        Check(!views[^1].Rows[0].Enabled && views[^1].Rows[0].Text.Contains("상신 완료"),
            "UI disables submission and explains the monthly limit");
        ChooseCouncilRow(0);
        OpenCouncil(1);
        ChooseCouncilRow(0);
        ChooseCouncilRow(1);
        Check(councilUiRequests == 0 && views[^1].Rows[0].Text == "보류", "UI defers without requesting march");
        ChooseCouncilRow(0);
        OpenCouncil(1);
        ChooseCouncilRow(0);
        ChooseCouncilRow(0);
        Check(councilUiRequests == 1, "UI approval submits one native march request");
        ChooseCouncilRow(0);
        Check(Send(ScriptEvent.CouncilEntered, 0, (8 << 16) | 212) == 0, "Next date for rejection check");
        OpenCouncil(0);
        ChooseCouncilRow(0);
        ChooseCouncilRow(0);
        ChooseCouncilRow(0);
        OpenCouncil(1);
        ChooseCouncilRow(0);
        Check(!views[^1].Rows.Single(row => row.Action == 0).Enabled, "Pending city request disables approval");
        ChooseCouncilRow(2);
        Check(councilUiRequests == 1 && views[^1].Rows[0].Text == "거절", "UI rejection does not submit a march");
        ChooseCouncilRow(0);
        OpenCouncil(3);
        ChooseCouncilRow(0);
        ChooseCouncilRow(1);
        Check(views[^1].Rows[0].Text.StartsWith("방어"), "UI sets city defensive delegation policy");
        ChooseCouncilRow(0);
        councilUiAuthority = false;
        OpenCouncil(3);
        Check(!views[^1].Rows.Single(row => row.Action == 3).Enabled,
            "UI blocks delegation without command authority");
        ChooseCouncilRow(0);

        councilUiComplete = true;
        Check(Send(ScriptEvent.CouncilEntered, 0, (9 << 16) | 212) == 0, "Next date for privilege proposal");
        int closuresBeforePrivilege = councilUiClosures;
        Check(Send(ScriptEvent.Closing, views[^1].Owner, -1) == 0 && Send(ScriptEvent.Resume, 0) == 0,
            "Close the hub before reopening it with the new date and authority");
        OpenCouncil(0);
        ChooseCouncilRow(0);
        Check(views[^1].Rows.Any(row => row.Action == 1 && row.Enabled), "Subordinate with privilege can request immediate approval");
        ChooseCouncilRow(1);
        Check(councilSpeakers.SequenceEqual(new ushort[] { 565, 999 }), "Proposer speaks before the actual superior");
        Check(privilegedUiRequests == 1 && councilUiRequests == 1,
            "Privilege proposal uses the privileged native request, not a normal pending proposal");
        Check(councilUiClosures == closuresBeforePrivilege + 2,
            "Cancelling the hub and executing a privileged proposal each request scene closure");

        if (managedDispatch != null)
        {
            // Switched off from the options tab: the mod undoes its game changes, gets
            // no events, and is loaded again by the next load event once switched on.
            jewelMask[0] = 1;
            var unload = (delegate* unmanaged[Cdecl]<NativeApi*, int, int>)&S8.Scripting.Host.Entry.Unload;
            int stopped;
            fixed (NativeApi* p = &api)
                stopped = unload(p, managedHandle);
            Check(stopped == 0 && publishedTraits is { Present: 0, Name: null } && jewelMask.All(word => word == 0),
                "Switched-off mod restores the game's traits and jewel effects");
            Check(Send(ScriptEvent.MenuClick, 0, actions["기재 편집"]) != 0, "Stopped mod is not running");
            Check(Send(ScriptEvent.Open) == 0 && Send(ScriptEvent.MenuClick, 0, actions["기재 편집"]) == 0,
                "Switched back on, the mod loads with the next load event");
        }

        bool rejected = false;

        try
        {
            MemoryAccess.Read<int>(new MemoryReference(0x10000, 1));
        }
        catch (InvalidOperationException)
        {
            rejected = true;
        }

        Check(rejected, "Calls outside callback rejected");
        Console.WriteLine($"PASS: {checks} ABI/composition checks");
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static long Invoke(int operation, ulong target, long argument, void* data, uint bytes)
    {
        switch (operation)
        {
            case 37: // Council menu fixture: protagonist belongs to city 1.
            case 38:
                new Span<byte>(data, (int)bytes).Clear();
                *(ushort*)((byte*)data + 2) = councilUiAuthority ? (ushort)565 : (ushort)999;
                *(ushort*)((byte*)data + 4) = councilUiAuthority ? (ushort)565 : (ushort)999;
                ((byte*)data)[9] = operation == 37 || target == 1 ? (byte)1 : (byte)2;
                ((byte*)data)[10] = 1;
                ((byte*)data)[11] = 1 | 2 | 4 | 8;
                return 0;
            case 40:
                new Span<byte>(data, (int)bytes).Clear();
                *(uint*)((byte*)data + 8) = 1;
                *(uint*)((byte*)data + 16) = 3;
                *(uint*)((byte*)data + 20) = 500;
                return 0;
            case 42:
                *(ulong*)((byte*)data + 8) = (ulong)++councilUiRequests;
                return 1;
            case 43:
                new Span<byte>(data, (int)bytes).Clear();
                *(uint*)data = councilUiComplete ? 3u : 1u;
                return 0;
            case 46: // ReadPlayerPrivilege.
                return 1;
            case 48: // DisableDialogRow.
                var disabledView = views.Single(view => view.Owner == (int)target);
                int disabledIndex = disabledView.Rows.FindIndex(row => row.Action == (int)argument);
                disabledView.Rows[disabledIndex] = disabledView.Rows[disabledIndex] with { Enabled = false };
                return 0;
            case 49: // ReadCityName.
                string cityName = $"도시 {target}";
                cityName.AsSpan().CopyTo(new Span<char>(data, (int)bytes / 2));
                ((char*)data)[cityName.Length] = '\0';
                return cityName.Length;
            case 50: // CloseCouncilRoom.
                councilRoomOpen = false;
                ++councilUiClosures;
                return 0;
            case 57: // HideFormDecoration.
                return data == null && bytes == 0 && argument == 0 ? 0 : -1;
            case 52: // RegisterUiSprite.
            case 53: // FindUiSprite.
            case 54: // ApplyUiSprite.
                if (data == null || bytes != 36)
                    return -1;
                int resource = new string((char*)data) switch
                {
                    "000_btn_command_0" => 0x61,
                    "000_btn_command_3" => 0x66,
                    _ => 0
                };
                if (operation == 52)
                    return resource != 0 && argument == resource ? 0 : -1;
                if (operation == 53)
                    return resource;
                if (resource == 0 || (uint)argument != (resource == 0x61 ? 3u : 11u) ||
                    (argument >> 32) is < 0 or > 4)
                    return -1;
                councilSprites.Add(((int)(argument >> 32), resource));
                return 0;
            case 58: // SetFormButtonLabel.
                int* label = (int*)data;
                return bytes == 12 && label[0] == 328 + argument * 178 && label[1] == 976 &&
                    label[2] == 25 ? 0 : -1;
            case 59: // SetFormButtonIcon.
                ReadOnlySpan<int> icons = [0x855, 0x28E, 0xDC, 0x225, 0x7C];
                int* icon = (int*)data;
                return bytes == 16 && argument >= 0 && argument < icons.Length && icon[0] == icons[(int)argument] &&
                    icon[1] == 289 + argument * 178 && icon[2] == 974 && icon[3] == 34 ? 0 : -1;
            case 56: // SetFormButtonBounds.
                int* bounds = (int*)data;
                return bytes == 16 && bounds[0] == 252 + argument * 178 && bounds[1] == 966 &&
                    bounds[2] == 162 && bounds[3] == 50 ? 0 : -1;
            case 51: // OpenCouncilRoom.
                if (councilRoomOpen || argument != 1 || data != null || bytes != 0)
                    return -1;
                councilRoomOpen = true;
                return 0;
            case 32: // ShowOfficerDialogue.
                councilSpeakers.Add((ushort)target);
                return 1;
            case 47: // RequestPrivilegedCityMarch.
                if (bytes != 24 || *(ushort*)((byte*)data + 16) != 565 ||
                    *(ushort*)((byte*)data + 18) != 999 || *(ushort*)((byte*)data + 20) != 999 ||
                    *(ushort*)((byte*)data + 22) != 999 || councilSpeakers.Count != 2)
                    return -1;
                *(ulong*)((byte*)data + 8) = (ulong)++privilegedUiRequests;
                return 1;
            case 44: // ReadWorldSaveData: no state in this UI fixture.
            case 45: // StageWorldSaveData.
                return 0;
            case 30: // ResolveOfficer.
            case 31: // ResolveCity.
                new Span<byte>(data, (int)bytes).Clear();
                *(ulong*)data = 0x100000 + target * 0x1000;
                *(ulong*)((byte*)data + 8) = 1;
                *(ushort*)((byte*)data + 16) = (ushort)target;
                return 1;

            case 29: // GetWorldContext.
                new Span<byte>(data, (int)bytes).Clear();
                *(ulong*)data = 1;
                *(ushort*)((byte*)data + 8) = 565;
                return 0;

            case 0:
                var spec = *(Spec*)data;
                var view = new View(spec.Owner, spec.Kind, []);
                for (int i = 0; i < spec.Count; ++i)
                    view.Rows.Add(new(spec.Rows[i].Action, new string (spec.Rows[i].Label), new string (spec.Rows[i].Text)));
                views.Add(view);
                return 0;
            case 1:
                return 0;
            case 2:
                var row = *(Row*)data;
                var existing = views.Single(v => v.Owner == (int)target);
                int index = existing.Rows.FindIndex(r => r.Action == row.Action);
                existing.Rows[index] = new(row.Action, new string (row.Label), new string (row.Text));
                return 0;
            case 3:
            case 4:
                return 0;
            case 6:
                if (target != 1)
                    return 0;
                new Span<byte>(data, (int)bytes).Clear();
                *(ulong*)data = 0x10000;
                *((ulong*)data + 1) = 1;
                *((ushort*)((byte*)data + 16)) = 1;
                *((ushort*)((byte*)data + 18)) = 99;
                return 1;
            case 7:
            case 8:
                string text = operation == 7 ? "대덕" : "설명";
                text.AsSpan().CopyTo(new Span<char>(data, (int)bytes / 2));
                ((char*)data)[text.Length] = '\0';
                return text.Length;
            case 10:
                memory.AsSpan((int)(target - 0x10000), (int)bytes).CopyTo(new Span<byte>(data, (int)bytes));
                return bytes;
            case 11:
                new ReadOnlySpan<byte>(data, (int)bytes).CopyTo(memory.AsSpan((int)(target - 0x10000), (int)bytes));
                return bytes;
            case 12:
                return 1;
            case 13: // RegisterMenu
                actions[new string(*(char**)((byte*)data + 16))] = ((int*)data)[1];
                return 0;
            case 14: // RegisterWindowRow
                actions[new string(*(char**)((byte*)data + 8))] = ((int*)data)[1];
                return 0;
            case 24: // ReleaseUiRegistration
                return 0;
            case 16: // ReadGameState
                new Span<byte>(data, (int)bytes).Clear();
                *(ulong*)data = 1;
                jewelMask.AsSpan().CopyTo(new Span<ulong>((byte*)data + 8, 8));
                jewelBitmap.AsSpan().CopyTo(new Span<byte>((byte*)data + 72, 34));
                ((byte*)data)[106] = 1;
                ((byte*)data)[107] = 1;
                return 0;
            case 17: // SubmitGameCommand
                for (int offset = 0; offset < bytes; offset += 80)
                {
                    byte* command = (byte*)data + offset;
                    ushort raw = *(ushort*)(command + 72);
                    bool opened = command[74] != 0;
                    new ReadOnlySpan<ulong>(command + 8, 8).CopyTo(jewelMask);
                    jewelCommands.Add((raw, opened, jewelMask[0]));
                    if (raw != 0)
                    {
                        byte bit = (byte)(1 << (raw & 7));
                        if (opened)
                            jewelBitmap[raw >> 3] |= bit;
                        else
                            jewelBitmap[raw >> 3] &= (byte)~bit;
                    }
                }
                return 0;
            case 18: // SetWindowDirty
                windowDirty = argument != 0;
                return 0;
            case 19: // SetTreeNodes
            case 20: // SetTreeFooter
            case 21: // GetTreeCategory
            case 22: // GetGemCategory
                return 0;
            case 62: // SetTraitCatalog: record 0 is trait ID 1.
                if (bytes != 254 * 72)
                    return -1;
                char* name = *(char**)data;
                publishedTraits = (((byte*)data)[52], *(int*)((byte*)data + 68), name == null ? null : new string(name));
                return 0;
            case 63: // SetDialogTabs: labels, count, reserved, row tab per action.
                if (bytes != 80)
                    return -1;
                int tabCount = *(int*)((byte*)data + 8);
                char** tabLabels = *(char***)data;
                dialogTabs = ((int)target,
                    Enumerable.Range(0, tabCount).Select(i => new string(tabLabels[i])).ToArray(),
                    new Span<byte>((byte*)data + 16, 64).ToArray());
                return 0;
            case 61: // SetJewelForceMask: eight words.
                if (bytes != 64)
                    return -1;
                new ReadOnlySpan<ulong>(data, 8).CopyTo(jewelMask);
                return 0;
            case 64: // SetRowBadge: resource, width, height, palette.
                if (bytes != 16)
                    return -1;
                rowBadge = ((int)target, (int)argument, ((int*)data)[0], ((int*)data)[3]);
                return 0;
            case 65: // SetOptionsTab: the manager mod's tab.
                return 0;
            case 66: // ReadModList: host API version and game build, no mods.
                "21\t0\n".AsSpan().CopyTo(new Span<char>(data, (int)bytes / 2));
                return 5;
            case 23: // ReadSelection
                selectedJewels.AsSpan().CopyTo(new Span<int>(data, (int)bytes / 4));
                return selectedJewels.Length;
            default:
                return -1;
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    struct Row
    {
        public int Action, Height;
        public char* Label;
        public char* Text;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct Spec
    {
        public int Owner;
        public DialogKind Kind;
        public int Confirm, Reserved;
        public char* Title;
        public char* Caption;
        public Row* Rows;
        public int Count, Padding;
        public int* Selection;
        public int SelectionCount, SelectionPadding;
    }

    sealed record View(int Owner, DialogKind Kind, List<UiRow> Rows);
}

