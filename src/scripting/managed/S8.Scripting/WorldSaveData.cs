using System.Text.Json;

namespace S8.Scripting;

// Values are snapshots: changing a returned object requires another Set call.
public sealed class WorldSaveData
{
    private sealed record Envelope(int Version, Dictionary<string, JsonElement> Values);

    private readonly ulong session;
    private readonly uint generation;
    private Dictionary<string, JsonElement> values = [];
    private bool invalidated;

    internal unsafe WorldSaveData(ulong session, uint generation)
    {
        this.session = session;
        this.generation = generation;
        int length = checked((int)Native.Call(Operation.ReadWorldSaveData, session, generation));
        if (length == 0)
            return;

        byte[] data = new byte[length];
        fixed (byte* pointer = data)
            Native.Call(Operation.ReadWorldSaveData, session, generation, pointer, (uint)length);

        // Disk JSON is checked here, before any component receives it.
        var envelope = JsonSerializer.Deserialize<Envelope>(data)
            ?? throw new InvalidDataException("Missing world save envelope.");
        if (envelope.Version != 1 || envelope.Values == null)
            throw new InvalidDataException("Unsupported world save schema.");

        values = envelope.Values;
    }

    public T? Get<T>(string key)
    {
        ObjectDisposedException.ThrowIf(invalidated, this);
        return values.TryGetValue(key, out var value) ? value.Deserialize<T>() : default;
    }

    public unsafe void Set<T>(string key, T value)
    {
        ObjectDisposedException.ThrowIf(invalidated, this);
        ArgumentException.ThrowIfNullOrWhiteSpace(key);
        var next = new Dictionary<string, JsonElement>(values)
        {
            [key] = JsonSerializer.SerializeToElement(value)
        };
        byte[] data = JsonSerializer.SerializeToUtf8Bytes(new Envelope(1, next));
        fixed (byte* pointer = data)
            Native.Call(Operation.StageWorldSaveData, session, generation, pointer, (uint)data.Length);

        values = next;
    }

    internal void Invalidate() => invalidated = true;
}
