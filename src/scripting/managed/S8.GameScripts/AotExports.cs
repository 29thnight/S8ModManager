#if S8_AOT
using System.Runtime.InteropServices;
using S8.Scripting;

namespace S8.GameScripts;

// NativeAOT entry points. The mod manager loads this library when the AOT build
// of s8.editors is selected over the JIT build (see mod.aot.json).
public static unsafe class AotExports
{
    [UnmanagedCallersOnly(EntryPoint = "S8ModLoad")]
    public static int Load(char* modDirectory, char* gameDirectory) =>
        AotModHost<ModApplication>.Load(modDirectory, gameDirectory);

    [UnmanagedCallersOnly(EntryPoint = "S8ModDispatch")]
    public static int Dispatch(NativeApi* api, int kind, int owner, int value) =>
        AotModHost<ModApplication>.Dispatch(api, kind, owner, value);
}
#endif
