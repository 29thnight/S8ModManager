using S8.Scripting;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace S8.GameScripts;

// This is a mod. No editor command, page ID, draft or save request is sent to C++.
public sealed class TraitEditorModule : IDisposable
{
#if S8_EXPERIMENTAL
    // Scripted effects are unfinished and excluded from release builds (0.0.1 ships trait and jewel editing only).
    public ScriptedTraitCatalog ScriptedTraits { get; set; } = null!;
#endif

    private ModContext context = null!;
    private UiScene? scene;
    private readonly List<PersonalityComponent> personalities = [];
    private readonly Dictionary<ushort, Definition> definitions = [];
    private DialogComponent catalog = null!, detail = null!;
    private readonly Dictionary<int, TextLabelComponent> detailLabels = [];
    private Draft? draft;
    private string? pendingError;
    private Store store = null!;
    private readonly Dictionary<ushort, (string Name, string Description)> accepted = [];
    private static readonly (ushort Id, string Label)[] Voices = [
        (0, "없음"),
        (3, "일신시담"),
        (7, "병귀신속"),
        (12, "노당익장"),
        (13, "복룡"),
        (14, "낭고"),
        (19, "만인적"),
        (22, "산도강습"),
        (24, "의협"),
        (60, "초세지걸"),
        (61, "금강불괴"),
        (99, "대덕"),
        (117, "금마초"),
        (118, "소패왕"),
        (139, "수화폐월"),
        (140, "괄목")
    ];

    public void Initialize(ModContext value)
    {
        context = value;
        using var data = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(context.AssetDirectory, "effect_definitions.json")));

        foreach (var item in data.RootElement.EnumerateArray())
        {
            var definition = new Definition(item.GetProperty("template").GetString()!);

            if (item.TryGetProperty("valueMap", out var values))
                foreach (var pair in values.EnumerateObject())
                    definition.Values[int.Parse(pair.Name)] = pair.Value.GetString()!;

            if (item.TryGetProperty("paramMap", out var parameters))
                foreach (var pair in parameters.EnumerateObject())
                    definition.Parameters[int.Parse(pair.Name)] = pair.Value.GetString()!;

            definitions[item.GetProperty("type").GetUInt16()] = definition;
        }

        // Apply the saved custom traits before any save is loaded.
        if (TraitConfiguration.Publish(context) is { } error)
            System.Diagnostics.Trace.WriteLine("[S8 mods] " + error);
    }

    // The first save is seeded from the game's default trait table (read only).
    public static string DefaultTraitsPath(ModContext context)
    {
        string own = Path.Combine(context.ModDirectory, "S8RPK_traits_default.json");
        return File.Exists(own) ? own : Path.Combine(context.GameDirectory, "S8RPK_traits_default.json");
    }

    public void Open()
    {
        Dispose();
        scene = new UiScene();
        store = new Store(context);
#if S8_EXPERIMENTAL
        ScriptedTraits.Reload();
#endif

        for (ushort id = 1; id <= 254; ++id)
        {
            var gameObject = scene.CreateObject($"Personality:{id}");
            var personality = gameObject.AddComponent<PersonalityComponent>();

            if (!personality.Bind(id))
            {
                gameObject.Dispose();
                continue;
            }

            personalities.Add(personality);
        }

        catalog = scene.Create("TraitCatalog");
        catalog.Selected = index => OpenDetail(personalities[index]);
        catalog.Resumed = () =>
        {
            for (int i = 0; i < personalities.Count; ++i)
                if (accepted.TryGetValue(personalities[i].Id, out var text))
                    catalog.SetRow(new(i, text.Name, text.Description));
        };
        catalog.Show(DialogKind.List, "기재 편집", personalities.Select((p, i) => new UiRow(i, p.Name, p.Description)).ToArray(), false);
    }

    private void OpenDetail(PersonalityComponent source)
    {
        // Copy the engine values into mod-owned state. Cancel discards this copy.
        source.Bind(source.Id);
        draft = new Draft(
            source,
            accepted.TryGetValue(source.Id, out var text) ? text.Name : source.Name,
            accepted.TryGetValue(source.Id, out text) ? text.Description : source.Description);
#if S8_EXPERIMENTAL
        draft.ScriptEffects = ScriptedTraits.GetSlots(source.Id);

        for (int slot = 0; slot < draft.ScriptEffects.Length; ++slot)
            if (draft.ScriptEffects[slot] != null)
                draft.Effects[slot] = default;
#endif

        Span<byte> grade = stackalloc byte[2];

        MemoryAccess.Read(source.Memory.Offset(0x2E), grade);
        draft.Grade = store.ReadGrade(source.Id, System.Buffers.Binary.BinaryPrimitives.ReadUInt16LittleEndian(grade));
        detail = scene!.Create("TraitDetail");
        detailLabels.Clear();

        foreach (var row in DetailRows())
        {
            var label = detail.GameObject.AddComponent<TextLabelComponent>();

            label.Bind(detail, row.Action, row.Label);
            detailLabels.Add(row.Action, label);
            var button = detail.GameObject.AddComponent<ButtonComponent>();

            button.Bind(detail, row.Action);
            button.Clicked = () =>
            {
                if (row.Action < 2)
                    EditText(row.Action == 1);
                else if (row.Action == 10)
                    SelectGrade();
                else if (row.Action == 9)
                    SelectVoice();
                else
                    SelectEffect(row.Action - 2);
            };
        }

        detail.Resumed = RefreshDetail;
        detail.Closing = (confirmed, _) =>
        {
            if (!confirmed)
                return;

            var current = draft!;
#if S8_EXPERIMENTAL
            string? error = ScriptedTraits.CheckForExternalChange() ?? store.Save(current);
#else
            string? error = store.Save(current);
#endif

            error ??= TraitConfiguration.Publish(context);
            if (error != null)
            {
                pendingError = error;
                return;
            }

#if S8_EXPERIMENTAL
            ScriptedTraits.Save(current.Source.Id, current.ScriptEffects);
#endif
            // Publishing the saved configuration above applied effects and voice to the game.
            accepted[current.Source.Id] = (current.Name, current.Description);
        };
        catalog.Resumed = () =>
        {
            if (accepted.TryGetValue(source.Id, out var updated))
                catalog.SetRow(new(personalities.IndexOf(source), updated.Name, updated.Description));

            ShowPendingError();
        };
        detail.Show(DialogKind.Form, "기재 편집", DetailRows());
    }

    private UiRow[] DetailRows()
    {
        var current = draft!;
        var rows = new List<UiRow>
        {
            new(0, "이름", current.Name),
            new(1, "설명", current.Description, 104)
        };

        for (int i = 0; i < 6; ++i)
#if S8_EXPERIMENTAL
            rows.Add(new(i + 2, $"효과 {i + 1}", current.ScriptEffects[i] is { } script
                ? ScriptedTraits.Effects.FirstOrDefault(effect => effect.Id == script)?.Name ?? script
                : Describe(current.Effects[i])));
#else
            rows.Add(new(i + 2, $"효과 {i + 1}", Describe(current.Effects[i])));
#endif

        rows.Add(new(9, "발동 음성", VoiceLabel(current.Voice)));
        rows.Add(new(10, "기재 색상", GradeLabel(current.Grade)));
        return rows.ToArray();
    }

    private void RefreshDetail()
    {
        foreach (var row in DetailRows())
            detailLabels[row.Action].Text = row.Text;

        ShowPendingError();
    }

    private void ShowPendingError()
    {
        if (pendingError == null)
            return;

        string error = pendingError;

        pendingError = null;
        scene!.Create("EditorMessage").Show(DialogKind.Message, "기재 편집", [new(0, "확인", error)]);
    }

    private void EditText(bool description)
    {
        var current = draft!;
        var dialog = scene!.Create(description ? "DescriptionInput" : "NameInput");
        var input = dialog.GameObject.AddComponent<TextInputComponent>();

        input.Bind(dialog);
        input.Completed = (result, text) =>
        {
            if (result != InputResult.Accepted)
            {
                if ((int)result < 0)
                    pendingError = "게임 입력 인코딩으로 변환할 수 없는 문자열입니다.";

                return;
            }

            bool valid = !text.Contains('\0') && (description ? text.Length <= 512 && Tokens(text).SequenceEqual(Tokens(current.OriginalDescription)) : text.Length is> 0 and <= 5);

            if (!valid)
            {
                pendingError = "이름 길이 또는 설명의 서식 토큰을 확인해 주십시오.";
                return;
            }

            if (description)
                current.Description = text;
            else
                current.Name = text;
        };
        dialog.Show(description ? DialogKind.MultilineText : DialogKind.Text, "기재 편집", []);
        input.Show(description ? current.Description : current.Name, description ? 513 : 6, description);
    }

    private static IEnumerable<string> Tokens(string text) => Regex.Matches(text, @"%(?:[-+ #0]*\d*(?:\.\d+)?[diuoxXfFeEgGcs]|%)").Select(m => m.Value);

    private void SelectVoice()
    {
        var dialog = scene!.Create("VoiceSelection");

        dialog.Selected = _ => dialog.Close();
        dialog.Closing = (confirmed, id) =>
        {
            if (confirmed)
                draft!.Voice = checked((ushort)id);
        };
        dialog.Show(
            DialogKind.List,
            "발동 음성 선택",
            Voices.Select(v => new UiRow(v.Id, VoiceLabel(v.Id), v.Id == 0 ? "캐릭터 음성을 재생하지 않습니다." : "발동 캐릭터의 음성으로 재생합니다.")).ToArray());
    }

    private static string GradeLabel(ushort grade) => grade switch
    {
        1 => "금색",
        2 => "녹색 · 긍정 효과",
        3 => "적색 · 부정 효과",
        _ => $"미지정 ({grade})"
    };

    private void SelectGrade()
    {
        var dialog = scene!.Create("TraitGradeSelection");

        dialog.Selected = _ => dialog.Close();
        dialog.Closing = (confirmed, id) =>
        {
            if (confirmed)
                draft!.Grade = checked((ushort)id);
        };
        dialog.Show(DialogKind.List, "기재 색상", Enumerable.Range(1, 3)
            .Select(grade => new UiRow(grade, GradeLabel((ushort)grade), "기재 배지와 목록에 표시할 색상입니다.")).ToArray());
    }

    private void SelectEffect(int slot)
    {
        var current = draft!;
        PersonalityEffect working = current.Effects[slot];
        string? script = current.ScriptEffects[slot];
#if S8_EXPERIMENTAL
        var scripts = ScriptedTraits.Effects.ToArray();
#else
        // Without scripted effects the list only offers native effects (ids below 1000).
        var scripts = Array.Empty<ScriptEffectDefinition>();
#endif
        var dialog = scene!.Create("EffectSelection");

        dialog.Selected = id =>
        {
            if (id >= 1000)
            {
                script = scripts[id - 1000].Id;
                working = default;
                return;
            }

            if (id == 0)
            {
                script = null;
                working = default;
                return;
            }

            var before = working;
            string? beforeScript = script;

            script = null;

            working = working with
            {
                Id = checked((ushort)id)
            };
            var parameters = scene.Create("EffectParameters");
            var keypad = parameters.GameObject.AddComponent<NumberInputComponent>();

            keypad.Bind(parameters);
            UiRow[] Rows() => [new(0, "값", working.Value.ToString()), new(1, "P", working.Parameter.ToString())];
            parameters.Clicked = field =>
            {
                keypad.Completed = (confirmed, value) =>
                {
                    if (!confirmed)
                        return;

                    working = field == 0 ? working with
                    {
                        Value = checked((ushort)value)
                    }

                    : working with
                    {
                        Parameter = checked((ushort)value)
                    };
                    parameters.SetRow(Rows()[field]);
                };
                keypad.Show(field == 0 ? working.Value : working.Parameter);
            };
            parameters.Closing = (confirmed, _) =>
            {
                if (!confirmed)
                {
                    working = before;
                    script = beforeScript;
                }
            };
            parameters.Show(DialogKind.CompactForm, "효과 매개변수", Rows());
        };
        dialog.Closing = (confirmed, _) =>
        {
            if (confirmed)
            {
                current.Effects[slot] = working;
                current.ScriptEffects[slot] = script;
            }
        };
        var rows = new List<UiRow>
        {
            new(0, "(없음)", "이 효과 슬롯을 비웁니다.")
        };

        rows.AddRange(definitions.OrderBy(p => p.Key).Select(p => new UiRow(p.Key, p.Key.ToString(), p.Value.Template)));
        rows.AddRange(scripts.Select((effect, index) => new UiRow(1000 + index, effect.Name, effect.Description)));
        dialog.Show(DialogKind.List, "효과 선택", rows);
    }

    private string Describe(PersonalityEffect effect)
    {
        if (effect.Id == 0)
            return "(없음)";

        if (!definitions.TryGetValue(effect.Id, out var definition))
            return $"효과 {effect.Id} / 값 {effect.Value} / P {effect.Parameter}";

        string value = definition.Values.GetValueOrDefault(effect.Value, effect.Value.ToString());
        string parameter = definition.Parameters.GetValueOrDefault(effect.Parameter, effect.Parameter.ToString());

        return definition.Template.Replace("{value}", value).Replace("{param}", parameter) + $"  [값 {effect.Value} / P {effect.Parameter}]";
    }

    private static string VoiceLabel(ushort id) => $"{id} — {Voices.FirstOrDefault(v => v.Id == id).Label ?? "미등록"}";

    public void Dispose()
    {
        scene?.Dispose();
        scene = null;

        personalities.Clear();
        accepted.Clear();
        detailLabels.Clear();
        draft = null;
    }

    private sealed class Definition(string template)
    {
        public string Template { get; } = template;

        public Dictionary<int, string> Values { get; } = [];

        public Dictionary<int, string> Parameters { get; } = [];
    }

    private sealed class Draft(PersonalityComponent source, string name, string description)
    {
        public PersonalityComponent Source { get; } = source;

        public string OriginalDescription { get; } = description;

        public string Name = name, Description = description;
        public PersonalityEffect[] Effects = source.Effects.ToArray();
        public ushort Voice = source.Voice;
        public ushort Grade;
        public string?[] ScriptEffects = new string?[6];
    }

    // Mod configuration is not presented as an engine API. The existing catalog
    // service observes this file and applies name/description overrides to game getters.

    private sealed class Store
    {
        private readonly string path;
        private string? baseline;
        private JsonObject root;

        public Store(ModContext context)
        {
            path = Path.Combine(context.ModDirectory, "san8r_traits_config.json");
            baseline = File.Exists(path) ? File.ReadAllText(path) : null;
            string initial = baseline ?? File.ReadAllText(TraitEditorModule.DefaultTraitsPath(context));

            root = JsonNode.Parse(initial)!.AsObject();

            if (root["traits"] is not JsonArray || (root["customNames"] != null && root["customNames"] is not JsonObject))
                throw new InvalidDataException("The trait configuration schema is not supported.");
        }

        public string? Save(Draft draft)
        {
            if ((File.Exists(path) ? File.ReadAllText(path) : null) != baseline)
                return "기재 파일이 외부에서 변경되었습니다. 편집기를 다시 열어 주십시오.";

            var next = root.DeepClone().AsObject();
            var row = next["traits"]!.AsArray().FirstOrDefault(n => n?["id"]?.GetValue<int>() == draft.Source.Id);

            if (row == null)
                return "저장 파일에 해당 기재 레코드가 없습니다.";

            row["effects"] = new JsonArray(draft.Effects.Select(e => (JsonNode)new JsonObject { ["type"] = e.Id, ["value"] = e.Value, ["param"] = e.Parameter }).ToArray());
            var metadata = next["customNames"] as JsonObject ?? new JsonObject();

            if (next["customNames"] == null)
                next["customNames"] = metadata;

            var entry = metadata[(draft.Source.Id - 1).ToString()] as JsonObject ?? new JsonObject();

            if (metadata[(draft.Source.Id - 1).ToString()] == null)
                metadata[(draft.Source.Id - 1).ToString()] = entry;

            entry["name"] = draft.Name;
            entry["desc"] = draft.Description;
            entry["voice"] = draft.Voice;
            entry["grade"] = draft.Grade;
            string content = next.ToJsonString(new JsonSerializerOptions { WriteIndented = true });
            string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";

            File.WriteAllText(temporary, content);
            File.Move(temporary, path, true);
            baseline = content;
            root = next;
            return null;
        }

        public ushort ReadGrade(ushort id, ushort fallback) =>
            root["customNames"]?[(id - 1).ToString()]?["grade"]?.GetValue<ushort>() ?? fallback;
    }
}
