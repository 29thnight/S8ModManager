using S8.Scripting;
using System.Text.Json;

namespace S8.GameScripts;

// Turns san8r_traits_config.json (in this mod's folder) into the trait catalog
// the native runtime applies. The rules follow the cheat's config reader so an
// existing file keeps meaning the same thing.
public static class TraitConfiguration
{
    public const string FileName = "san8r_traits_config.json";

    // Publishes the mod's configuration. No file means nothing custom to apply.
    public static string? Publish(ModContext context)
    {
        string path = Path.Combine(context.ModDirectory, FileName);
        if (!File.Exists(path))
            return null;

        var definitions = Read(File.ReadAllBytes(path), out string? error);
        if (definitions == null)
            return error;

        TraitCatalog.Publish(definitions);
        return null;
    }

    public static TraitDefinition?[]? Read(ReadOnlySpan<byte> json, out string? error)
    {
        error = "기재 설정 파일 형식이 올바르지 않습니다.";
        JsonDocument document;
        try
        {
            document = JsonDocument.Parse(json.ToArray());
        }
        catch (JsonException)
        {
            return null;
        }

        using (document)
        {
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object || !root.TryGetProperty("traits", out var traits) ||
                traits.ValueKind != JsonValueKind.Array)
                return null;

            var effects = new TraitEffectValue[TraitCatalog.Count][];
            foreach (var row in traits.EnumerateArray())
            {
                if (row.ValueKind != JsonValueKind.Object || !Integer(row, "index", 0, 253, out int index) ||
                    !Integer(row, "id", 1, 254, out int id) || id != index + 1 ||
                    !row.TryGetProperty("effects", out var slots) || slots.ValueKind != JsonValueKind.Array ||
                    slots.GetArrayLength() != 6)
                    return null;

                var values = new TraitEffectValue[6];
                int slot = 0;
                foreach (var effect in slots.EnumerateArray())
                {
                    if (effect.ValueKind != JsonValueKind.Object || !Integer(effect, "type", 0, 65535, out int type) ||
                        !Integer(effect, "value", 0, 65535, out int value) ||
                        !Integer(effect, "param", 0, 65535, out int parameter))
                        return null;
                    values[slot++] = new((ushort)type, (ushort)value, (ushort)parameter);
                }
                // Repeated indices: the last row wins, as in the original file format.
                effects[index] = values;
            }
            if (effects.All(row => row == null))
                return null;

            var definitions = new TraitDefinition?[TraitCatalog.Count];
            for (int index = 0; index < definitions.Length; ++index)
                if (effects[index] != null)
                    definitions[index] = new TraitDefinition { Effects = effects[index] };

            if (!root.TryGetProperty("customNames", out var names))
            {
                error = null;
                return definitions;
            }
            if (names.ValueKind != JsonValueKind.Object)
                return null;

            foreach (var entry in names.EnumerateObject())
            {
                if (!int.TryParse(entry.Name, out int index) || index < 0 || index >= TraitCatalog.Count ||
                    entry.Value.ValueKind != JsonValueKind.Object ||
                    !Text(entry.Value, "name", out string? name) || !Text(entry.Value, "desc", out string? description) ||
                    !Optional(entry.Value, "cloneSrc", int.MinValue, int.MaxValue, out int clone) ||
                    !Optional(entry.Value, "bgTrait", int.MinValue, int.MaxValue, out int background) ||
                    !Optional(entry.Value, "grade", 1, 3, out int grade) ||
                    !Optional(entry.Value, "voice", 0, 140, out int voice))
                    return null;

                definitions[index] = (definitions[index] ?? new TraitDefinition()) with
                {
                    HasMetadata = true,
                    Name = name ?? "",
                    Description = description ?? "",
                    CloneSource = clone,
                    BackgroundTrait = background,
                    Grade = grade,
                    Voice = voice
                };
            }
            error = null;
            return definitions;
        }
    }

    private static bool Integer(JsonElement row, string key, int minimum, int maximum, out int value)
    {
        value = 0;
        return row.TryGetProperty(key, out var field) && field.ValueKind == JsonValueKind.Number &&
               field.TryGetInt64(out long number) && number >= minimum && number <= maximum &&
               (value = (int)number) == number;
    }

    private static bool Optional(JsonElement row, string key, int minimum, int maximum, out int value)
    {
        value = -1;
        return !row.TryGetProperty(key, out _) || Integer(row, key, minimum, maximum, out value);
    }

    private static bool Text(JsonElement row, string key, out string? value)
    {
        value = null;
        if (!row.TryGetProperty(key, out var field))
            return true;
        if (field.ValueKind != JsonValueKind.String)
            return false;
        value = field.GetString();
        return value != null && !value.Contains('\0');
    }
}
