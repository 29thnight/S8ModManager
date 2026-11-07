using System.Text.Json;
using System.Text.Json.Nodes;
using S8.Scripting;

namespace S8.GameScripts;

public sealed class ScriptedTraitCatalog
{
    public const string RecoveryEffect = "s8.battle.wounded-recovery";

    private readonly ModContext context;
    private readonly string directory;
    private readonly string bindingsPath;
    private readonly BattleEffects effects;
    private readonly Dictionary<ushort, string?[]> bindings = [];
    private string? baseline;

    // The SDK runs the effects; this catalog only supplies the user-edited bindings.
    public ScriptedTraitCatalog(ModContext context, BattleEffects effects)
    {
        this.context = context;
        directory = context.ModDirectory;
        bindingsPath = Path.Combine(directory, "san8r_script_traits.json");
        this.effects = effects;
        Reload();
        effects.UseBindings(personality => bindings.GetValueOrDefault(personality));
    }

    public IEnumerable<ScriptEffectDefinition> Effects => effects.Definitions;

    public string?[] GetSlots(ushort personality) => bindings.TryGetValue(personality, out var slots)
        ? (string?[])slots.Clone()
        : new string?[6];

    public void Reload()
    {
        string? content = File.Exists(bindingsPath) ? File.ReadAllText(bindingsPath) : null;

        if (content == baseline)
            return;

        using var document = JsonDocument.Parse(content ?? "{\"version\":1,\"traits\":{}}");

        int version = document.RootElement.GetProperty("version").GetInt32();

        if (version is not (1 or 2))
            throw new InvalidDataException("지원하지 않는 스크립트 기재 파일 버전입니다.");

        var next = new Dictionary<ushort, string?[]>();
        using var native = version == 1 && document.RootElement.GetProperty("traits").EnumerateObject().Any()
            ? JsonDocument.Parse(File.ReadAllText(Path.Combine(directory, "san8r_traits_config.json")))
            : null;

        foreach (var entry in document.RootElement.GetProperty("traits").EnumerateObject())
        {
            if (!ushort.TryParse(entry.Name, out var id) || id is < 1 or > 254 ||
                entry.Value.ValueKind != JsonValueKind.Array)
                throw new InvalidDataException("스크립트 기재 ID 또는 효과 목록이 잘못되었습니다.");

            var values = entry.Value.EnumerateArray().ToArray();

            if ((version == 2 ? values.Length != 6 : values.Length > 6) ||
                values.Any(value => value.ValueKind != JsonValueKind.String &&
                    !(version == 2 && value.ValueKind == JsonValueKind.Null)))
                throw new InvalidDataException("스크립트 효과 슬롯 구성이 잘못되었습니다.");

            var ids = values.Select(value => value.GetString()).ToArray();

            if (ids.Any(value => value != null && string.IsNullOrWhiteSpace(value)))
                throw new InvalidDataException("스크립트 효과 ID가 비어 있습니다.");

            var slots = new string?[6];

            if (version == 2)
                ids.CopyTo(slots, 0);
            else
            {
                // Move the old separate effect list into unoccupied native slots.
                var row = native!.RootElement.GetProperty("traits").EnumerateArray()
                    .First(item => item.GetProperty("id").GetUInt16() == id);
                int[] free = row.GetProperty("effects").EnumerateArray()
                    .Select((effect, slot) => (effect, slot))
                    .Where(item => item.effect.GetProperty("type").GetInt32() == 0)
                    .Select(item => item.slot).ToArray();

                if (free.Length < ids.Length)
                    throw new InvalidDataException($"기재 {id}에 스크립트 효과를 넣을 빈 효과 슬롯이 없습니다.");

                for (int index = 0; index < ids.Length; ++index)
                    slots[free[index]] = ids[index];
            }

            next.Add(id, slots);
        }

        bindings.Clear();

        foreach (var entry in next)
            bindings.Add(entry.Key, entry.Value);

        baseline = content;
    }

    public string? CheckForExternalChange() =>
        (File.Exists(bindingsPath) ? File.ReadAllText(bindingsPath) : null) != baseline
            ? "스크립트 기재 파일이 외부에서 변경되었습니다. 편집기를 다시 여십시오."
            : null;

    // The caller checks external changes once, before committing either file.
    public void Save(ushort personality, string?[] slots)
    {
        var selected = (string?[])slots.Clone();
        var next = new Dictionary<ushort, string?[]>(bindings) { [personality] = selected };
        string content = JsonSerializer.Serialize(new { version = 2, traits = next }, new JsonSerializerOptions { WriteIndented = true });
        string temporary = bindingsPath + "." + Guid.NewGuid().ToString("N") + ".tmp";

        File.WriteAllText(temporary, content);
        File.Move(temporary, bindingsPath, true);
        bindings[personality] = selected;
        baseline = content;
    }

    public void EnsureRecoveryTrait()
    {
        Reload();
        string path = Path.Combine(directory, "san8r_traits_config.json");
        string original = File.ReadAllText(File.Exists(path) ? path : TraitEditorModule.DefaultTraitsPath(context));
        var root = JsonNode.Parse(original)!.AsObject();
        var traits = root["traits"]!.AsArray();
        var names = root["customNames"] as JsonObject ?? new JsonObject();
        var existing = names.FirstOrDefault(entry => entry.Value?["name"]?.GetValue<string>() == "마왕의료");

        if (existing.Value != null)
            return; // An existing trait and its edited bindings belong to the user.

        // The manager already materializes custom records 71..254. Allocate an
        // unnamed, effect-free slot, preserving every existing named trait.
        var available = traits.Where(node => node!["id"]!.GetValue<int>() is >= 71 and <= 254)
            .OrderByDescending(node => node!["id"]!.GetValue<int>())
            .FirstOrDefault(node =>
            {
                int index = node!["id"]!.GetValue<int>() - 1;
                var text = names[index.ToString()];

                return string.IsNullOrEmpty(text?["name"]?.GetValue<string>()) &&
                    string.IsNullOrEmpty(text?["desc"]?.GetValue<string>()) &&
                    node["effects"]!.AsArray().All(effect => effect!["type"]!.GetValue<int>() == 0);
            });

        if (available == null)
            throw new InvalidOperationException("마왕의료를 만들 빈 커스텀 기재 슬롯이 없습니다.");

        ushort id = available["id"]!.GetValue<ushort>();
        names[(id - 1).ToString()] = new JsonObject
        {
            ["name"] = "마왕의료",
            ["desc"] = "전체 부대 행동 종료 시 이 기재를 보유한 장수의 소속 부대를 회복 상한까지 회복한다. 스크립트 효과.",
            ["grade"] = 1,
            ["cloneSrc"] = 30,
            ["bgTrait"] = 30,
            ["voice"] = 0
        };
        if (root["customNames"] == null)
            root["customNames"] = names;

        // Keep the six native slots empty. Script effect IDs live in the manager file.
        available["effects"] = new JsonArray(Enumerable.Range(0, 6).Select(_ => (JsonNode)new JsonObject
        {
            ["type"] = 0, ["value"] = 0, ["param"] = 0
        }).ToArray());

        string backup = path + ".before-script-traits.bak";

        if (File.Exists(path) && !File.Exists(backup))
            File.Copy(path, backup);

        Save(id, [RecoveryEffect, null, null, null, null, null]);
        string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";

        File.WriteAllText(temporary, root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
        File.Move(temporary, path, true);
    }
}
