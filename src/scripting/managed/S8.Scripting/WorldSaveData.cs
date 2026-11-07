using System.Text.Json;

namespace S8.Scripting;

// Values are snapshots: changing a returned object requires another Set call.
// Every mod has its own WorldSaveData over the one blob the game saves, so nothing is
// cached here: each call reads the blob, and Set writes back only its own key over what
// the other mods have staged since.
public sealed class WorldSaveData
{
    private sealed record Envelope(int Version, Dictionary<string, JsonElement> Values);

    private readonly ulong session;
    private readonly uint generation;
    private bool invalidated;

    internal WorldSaveData(ulong session, uint generation)
    {
        this.session = session;
        this.generation = generation;
        Read();
    }

    public T? Get<T>(string key)
    {
        ObjectDisposedException.ThrowIf(invalidated, this);
        return Read().TryGetValue(key, out var value) ? value.Deserialize<T>() : default;
    }

    public unsafe void Set<T>(string key, T value)
    {
        ObjectDisposedException.ThrowIf(invalidated, this);
        ArgumentException.ThrowIfNullOrWhiteSpace(key);
        var next = Read();
        next[key] = JsonSerializer.SerializeToElement(value);
        byte[] data = JsonSerializer.SerializeToUtf8Bytes(new Envelope(1, next));
        fixed (byte* pointer = data)
            Native.Call(Operation.StageWorldSaveData, session, generation, pointer, (uint)data.Length);
    }

    internal void Invalidate() => invalidated = true;

    private unsafe Dictionary<string, JsonElement> Read()
    {
        int length = checked((int)Native.Call(Operation.ReadWorldSaveData, session, generation));
        if (length == 0)
            return [];

        byte[] data = new byte[length];
        fixed (byte* pointer = data)
            Native.Call(Operation.ReadWorldSaveData, session, generation, pointer, (uint)length);

        // Disk JSON is checked here, before any component receives it.
        var envelope = JsonSerializer.Deserialize<Envelope>(data)
            ?? throw new InvalidDataException("Missing world save envelope.");
        if (envelope.Version != 1 || envelope.Values == null)
            throw new InvalidDataException("Unsupported world save schema.");
        return envelope.Values;
    }
}
