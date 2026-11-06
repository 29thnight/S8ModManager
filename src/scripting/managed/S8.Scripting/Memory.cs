using System.Runtime.InteropServices;

namespace S8.Scripting;

[StructLayout(LayoutKind.Sequential)]
public readonly struct MemoryReference(ulong address, ulong session)
{
    public readonly ulong Address = address, Session = session;

    public MemoryReference Offset(uint bytes) => new(checked(Address + bytes), Session);
}

[StructLayout(LayoutKind.Sequential)]
public struct MemoryRegion
{
    public ulong Base, Size;
    public uint Protection, Writable;
}

// OS page checks and transfer take place together at the C++ API entrance.
public static unsafe class MemoryAccess
{
    public static MemoryRegion Inspect(MemoryReference memory, int bytes)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(bytes);
        MemoryRegion region;

        Native.Call(Operation.InspectMemory, memory.Address, memory.Session > long.MaxValue ? -1 : (long)memory.Session, &region, checked((uint)bytes));
        return region;
    }

    public static T Read<T>(MemoryReference memory)
        where T : unmanaged
    {
        T result;

        Native.Call(Operation.ReadMemory, memory.Address, checked((long)memory.Session), &result, (uint)sizeof(T));
        return result;
    }

    public static void Write<T>(MemoryReference memory, in T value)
        where T : unmanaged
    {
        T copy = value;

        Native.Call(Operation.WriteMemory, memory.Address, checked((long)memory.Session), &copy, (uint)sizeof(T));
    }

    public static void Read(MemoryReference memory, Span<byte> buffer)
    {
        fixed (byte* data = buffer)
            Native.Call(Operation.ReadMemory, memory.Address, checked((long)memory.Session), data, (uint)buffer.Length);
    }

    public static void Write(MemoryReference memory, ReadOnlySpan<byte> buffer)
    {
        fixed (byte* data = buffer)
            Native.Call(Operation.WriteMemory, memory.Address, checked((long)memory.Session), data, (uint)buffer.Length);
    }
}

public sealed class MemoryComponent : Component
{
    public MemoryReference Reference { get; private set; }

    public void Bind(MemoryReference reference) => Reference = reference;

    public T Read<T>(uint offset = 0)
        where T : unmanaged => MemoryAccess.Read<T>(Reference.Offset(offset));

    public void Write<T>(uint offset, in T value)
        where T : unmanaged => MemoryAccess.Write(Reference.Offset(offset), value);
}
