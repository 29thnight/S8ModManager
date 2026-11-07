using System.Runtime.InteropServices;
using System.Text;

namespace S8.LuaHost;

// The LuaJIT (Lua 5.1) C API, bound by export lookup. Mods load from a stream
// into a collectible context with no native probing, so the DLL is loaded by path.
internal sealed unsafe class LuaApi
{
    public const int RegistryIndex = -10000;
    public const int GlobalsIndex = -10002;
    public const int FirstUpvalue = GlobalsIndex - 1;

    public const int TypeNil = 0, TypeBoolean = 1, TypeNumber = 3, TypeString = 4, TypeFunction = 6, TypeUserdata = 7;

    public readonly delegate* unmanaged[Cdecl]<nint> NewState;
    public readonly delegate* unmanaged[Cdecl]<nint, void> Close;
    public readonly delegate* unmanaged[Cdecl]<nint, byte*, nuint, byte*, int> LoadBuffer;
    public readonly delegate* unmanaged[Cdecl]<nint, int, int, int, int> PCall;
    public readonly delegate* unmanaged[Cdecl]<nint, int, int, void> Call;
    public readonly delegate* unmanaged[Cdecl]<nint, delegate* unmanaged[Cdecl]<nint, int>, int, void> PushCClosure;
    public readonly delegate* unmanaged[Cdecl]<nint, int, byte*, void> SetField;
    public readonly delegate* unmanaged[Cdecl]<nint, int, byte*, void> GetField;
    public readonly delegate* unmanaged[Cdecl]<nint, void> PushNil;
    public readonly delegate* unmanaged[Cdecl]<nint, double, void> PushNumber;
    public readonly delegate* unmanaged[Cdecl]<nint, byte*, nuint, void> PushLString;
    public readonly delegate* unmanaged[Cdecl]<nint, int, void> PushBoolean;
    public readonly delegate* unmanaged[Cdecl]<nint, void*, void> PushLightUserdata;
    public readonly delegate* unmanaged[Cdecl]<nint, int, void> PushValue;
    public readonly delegate* unmanaged[Cdecl]<nint, int, void> Insert;
    public readonly delegate* unmanaged[Cdecl]<nint, int, nuint*, byte*> ToLString;
    public readonly delegate* unmanaged[Cdecl]<nint, int, double> ToNumber;
    public readonly delegate* unmanaged[Cdecl]<nint, int, void*> ToUserdata;
    public readonly delegate* unmanaged[Cdecl]<nint, int, int> Type;
    public readonly delegate* unmanaged[Cdecl]<nint, int> GetTop;
    public readonly delegate* unmanaged[Cdecl]<nint, int, void> SetTop;
    public readonly delegate* unmanaged[Cdecl]<nint, int, int, void> CreateTable;
    public readonly delegate* unmanaged[Cdecl]<nint, nuint, void*> NewUserdata;
    public readonly delegate* unmanaged[Cdecl]<nint, int, int> Ref;
    public readonly delegate* unmanaged[Cdecl]<nint, int, int, void> Unref;
    public readonly delegate* unmanaged[Cdecl]<nint, int, int, void> RawGetI;
    public readonly delegate* unmanaged[Cdecl]<nint, int, int> SetMetatable;
    public readonly delegate* unmanaged[Cdecl]<nint, int, int> GetMetatable;
    public readonly delegate* unmanaged[Cdecl]<nint, int, int, int> RawEqual;
    public readonly delegate* unmanaged[Cdecl]<nint, int> OpenBase, OpenString, OpenTable, OpenMath, OpenBit;

    public LuaApi(string path)
    {
        nint library = NativeLibrary.Load(path);
        nint Export(string name) => NativeLibrary.GetExport(library, name);

        NewState = (delegate* unmanaged[Cdecl]<nint>)Export("luaL_newstate");
        Close = (delegate* unmanaged[Cdecl]<nint, void>)Export("lua_close");
        LoadBuffer = (delegate* unmanaged[Cdecl]<nint, byte*, nuint, byte*, int>)Export("luaL_loadbuffer");
        PCall = (delegate* unmanaged[Cdecl]<nint, int, int, int, int>)Export("lua_pcall");
        Call = (delegate* unmanaged[Cdecl]<nint, int, int, void>)Export("lua_call");
        PushCClosure = (delegate* unmanaged[Cdecl]<nint, delegate* unmanaged[Cdecl]<nint, int>, int, void>)Export("lua_pushcclosure");
        SetField = (delegate* unmanaged[Cdecl]<nint, int, byte*, void>)Export("lua_setfield");
        GetField = (delegate* unmanaged[Cdecl]<nint, int, byte*, void>)Export("lua_getfield");
        PushNil = (delegate* unmanaged[Cdecl]<nint, void>)Export("lua_pushnil");
        PushNumber = (delegate* unmanaged[Cdecl]<nint, double, void>)Export("lua_pushnumber");
        PushLString = (delegate* unmanaged[Cdecl]<nint, byte*, nuint, void>)Export("lua_pushlstring");
        PushBoolean = (delegate* unmanaged[Cdecl]<nint, int, void>)Export("lua_pushboolean");
        PushLightUserdata = (delegate* unmanaged[Cdecl]<nint, void*, void>)Export("lua_pushlightuserdata");
        PushValue = (delegate* unmanaged[Cdecl]<nint, int, void>)Export("lua_pushvalue");
        Insert = (delegate* unmanaged[Cdecl]<nint, int, void>)Export("lua_insert");
        ToLString = (delegate* unmanaged[Cdecl]<nint, int, nuint*, byte*>)Export("lua_tolstring");
        ToNumber = (delegate* unmanaged[Cdecl]<nint, int, double>)Export("lua_tonumber");
        ToUserdata = (delegate* unmanaged[Cdecl]<nint, int, void*>)Export("lua_touserdata");
        Type = (delegate* unmanaged[Cdecl]<nint, int, int>)Export("lua_type");
        GetTop = (delegate* unmanaged[Cdecl]<nint, int>)Export("lua_gettop");
        SetTop = (delegate* unmanaged[Cdecl]<nint, int, void>)Export("lua_settop");
        CreateTable = (delegate* unmanaged[Cdecl]<nint, int, int, void>)Export("lua_createtable");
        NewUserdata = (delegate* unmanaged[Cdecl]<nint, nuint, void*>)Export("lua_newuserdata");
        Ref = (delegate* unmanaged[Cdecl]<nint, int, int>)Export("luaL_ref");
        Unref = (delegate* unmanaged[Cdecl]<nint, int, int, void>)Export("luaL_unref");
        RawGetI = (delegate* unmanaged[Cdecl]<nint, int, int, void>)Export("lua_rawgeti");
        SetMetatable = (delegate* unmanaged[Cdecl]<nint, int, int>)Export("lua_setmetatable");
        GetMetatable = (delegate* unmanaged[Cdecl]<nint, int, int>)Export("lua_getmetatable");
        RawEqual = (delegate* unmanaged[Cdecl]<nint, int, int, int>)Export("lua_rawequal");
        OpenBase = (delegate* unmanaged[Cdecl]<nint, int>)Export("luaopen_base");
        OpenString = (delegate* unmanaged[Cdecl]<nint, int>)Export("luaopen_string");
        OpenTable = (delegate* unmanaged[Cdecl]<nint, int>)Export("luaopen_table");
        OpenMath = (delegate* unmanaged[Cdecl]<nint, int>)Export("luaopen_math");
        OpenBit = (delegate* unmanaged[Cdecl]<nint, int>)Export("luaopen_bit");
    }

    public void PushString(nint state, string value)
    {
        byte[] bytes = Encoding.UTF8.GetBytes(value);
        fixed (byte* data = bytes)
            PushLString(state, data, (nuint)bytes.Length);
    }

    public string? ReadString(nint state, int index)
    {
        nuint length;
        byte* data = ToLString(state, index, &length);
        return data == null ? null : Encoding.UTF8.GetString(data, checked((int)length));
    }

    public void SetNamed(nint state, int index, string name)
    {
        byte[] bytes = Encoding.UTF8.GetBytes(name + "\0");
        fixed (byte* data = bytes)
            SetField(state, index, data);
    }

    public void GetNamed(nint state, int index, string name)
    {
        byte[] bytes = Encoding.UTF8.GetBytes(name + "\0");
        fixed (byte* data = bytes)
            GetField(state, index, data);
    }
}
