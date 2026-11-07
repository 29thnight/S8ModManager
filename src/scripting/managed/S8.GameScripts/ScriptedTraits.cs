using System.Text.Json;
using System.Text.Json.Nodes;
using S8.Scripting;

namespace S8.GameScripts;

// The trait slots that hold scripted effects, with the value each slot gives its
// effect. Effects are defined by Lua mods in the shared EffectCatalog; this file
// only says which traits hold them, and the SDK of the defining mod runs them.
public sealed class ScriptedTraitCatalog : IDisposable
{
    private readonly string directory;
    private readonly string bindingsPath;
    private readonly Dictionary<ushort, EffectSlot?[]> bindings = [];
    private string? baseline;

    public ScriptedTraitCatalog(ModContext context)
    {
        directory = context.ModDirectory;
        bindingsPath = Path.Combine(directory, "san8r_script_traits.json");
        Reload();
        EffectCatalog.UseBindings(this, personality => bindings.GetValueOrDefault(personality));
    }

    public IReadOnlyList<ScriptEffectDefinition> Effects => EffectCatalog.Definitions;

    public EffectSlot?[] GetSlots(ushort personality) => bindings.TryGetValue(personality, out var slots)
        ? (EffectSlot?[])slots.Clone()
        : new EffectSlot?[6];

    public void Reload()
    {
        string? content = File.Exists(bindingsPath) ? File.ReadAllText(bindingsPath) : null;

        if (content == baseline)
            return;

        using var document = JsonDocument.Parse(content ?? "{\"version\":3,\"traits\":{}}");

        int version = document.RootElement.GetProperty("version").GetInt32();

        if (version is not (1 or 2 or 3))
            throw new InvalidDataException("지원하지 않는 스크립트 기재 파일 버전입니다.");

        var next = new Dictionary<ushort, EffectSlot?[]>();
        using var native = version == 1 && document.RootElement.GetProperty("traits").EnumerateObject().Any()
            ? JsonDocument.Parse(File.ReadAllText(Path.Combine(directory, "san8r_traits_config.json")))
            : null;

        foreach (var entry in document.RootElement.GetProperty("traits").EnumerateObject())
        {
            if (!ushort.TryParse(entry.Name, out var id) || id is < 1 or > 254 ||
                entry.Value.ValueKind != JsonValueKind.Array)
                throw new InvalidDataException("스크립트 기재 ID 또는 효과 목록이 잘못되었습니다.");

            var values = entry.Value.EnumerateArray().Select(Slot).ToArray();

            if (version >= 2 ? values.Length != 6 : values.Length > 6)
                throw new InvalidDataException("스크립트 효과 슬롯 구성이 잘못되었습니다.");

            var slots = new EffectSlot?[6];

            if (version >= 2)
                values.CopyTo(slots, 0);
            else
            {
                // Move the old separate effect list into unoccupied native slots.
                var row = native!.RootElement.GetProperty("traits").EnumerateArray()
                    .First(item => item.GetProperty("id").GetUInt16() == id);
                int[] free = row.GetProperty("effects").EnumerateArray()
                    .Select((effect, slot) => (effect, slot))
                    .Where(item => item.effect.GetProperty("type").GetInt32() == 0)
                    .Select(item => item.slot).ToArray();

                if (free.Length < values.Length)
                    throw new InvalidDataException($"기재 {id}에 스크립트 효과를 넣을 빈 효과 슬롯이 없습니다.");

                for (int index = 0; index < values.Length; ++index)
                    slots[free[index]] = values[index];
            }

            next.Add(id, slots);
        }

        bindings.Clear();

        foreach (var entry in next)
            bindings.Add(entry.Key, entry.Value);

        baseline = content;
    }

    // Version 1 and 2 hold bare IDs (value 0); version 3 holds {"id", "value"}.
    private static EffectSlot? Slot(JsonElement value)
    {
        switch (value.ValueKind)
        {
            case JsonValueKind.Null:
                return null;
            case JsonValueKind.String when !string.IsNullOrWhiteSpace(value.GetString()):
                return new EffectSlot(value.GetString()!, 0);
            case JsonValueKind.Object when value.TryGetProperty("id", out var id) && id.ValueKind == JsonValueKind.String
                && !string.IsNullOrWhiteSpace(id.GetString()):
                return new EffectSlot(id.GetString()!, value.TryGetProperty("value", out var number) ? number.GetInt32() : 0);
            default:
                throw new InvalidDataException("스크립트 효과 슬롯 구성이 잘못되었습니다.");
        }
    }

    public string? CheckForExternalChange() =>
        (File.Exists(bindingsPath) ? File.ReadAllText(bindingsPath) : null) != baseline
            ? "스크립트 기재 파일이 외부에서 변경되었습니다. 편집기를 다시 여십시오."
            : null;

    // The caller checks external changes once, before committing either file.
    public void Save(ushort personality, EffectSlot?[] slots)
    {
        var selected = (EffectSlot?[])slots.Clone();
        var next = new Dictionary<ushort, EffectSlot?[]>(bindings) { [personality] = selected };
        var traits = new JsonObject();

        foreach (var entry in next.OrderBy(entry => entry.Key))
            traits[entry.Key.ToString()] = new JsonArray(entry.Value.Select(slot => slot is { } held
                ? new JsonObject { ["id"] = held.Id, ["value"] = held.Value }
                : null).ToArray<JsonNode?>());

        string content = new JsonObject { ["version"] = 3, ["traits"] = traits }
            .ToJsonString(new JsonSerializerOptions { WriteIndented = true });
        string temporary = bindingsPath + "." + Guid.NewGuid().ToString("N") + ".tmp";

        File.WriteAllText(temporary, content);
        File.Move(temporary, bindingsPath, true);
        bindings[personality] = selected;
        baseline = content;
    }

    public void Dispose() => EffectCatalog.Withdraw(this);
}
