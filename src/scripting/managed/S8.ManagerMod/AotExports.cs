#if S8_AOT
using System.Runtime.InteropServices;
using S8.Scripting;

namespace S8.ManagerMod;

public static unsafe class AotExports
{
    [UnmanagedCallersOnly(EntryPoint = "S8ModLoad")]
    public static int Load(char* modDirectory, char* gameDirectory) =>
        AotModHost<ManagerMod>.Load(modDirectory, gameDirectory);

    [UnmanagedCallersOnly(EntryPoint = "S8ModDispatch")]
    public static int Dispatch(NativeApi* api, int kind, int owner, int value) =>
        AotModHost<ManagerMod>.Dispatch(api, kind, owner, value);

    [UnmanagedCallersOnly(EntryPoint = "S8ModUnload")]
    public static int Unload(NativeApi* api) => AotModHost<ManagerMod>.Unload(api);
}
#endif
