using System.Runtime.Loader;
using S8.Scripting.Host;

// Live mod sources: compile from source\, patch a changed method body in place
// (when the runtime allows edits), and fall back to a swap for structural edits.
internal static class LiveSourceChecks
{
    public static int Run()
    {
        int checks = 0;
        void Check(bool result, string message)
        {
            if (!result)
                throw new Exception(message);
            ++checks;
        }

        string mod = Path.Combine(Path.GetTempPath(), "s8-live-" + Guid.NewGuid().ToString("N"));
        string source = Path.Combine(mod, LiveSource.Folder);
        Directory.CreateDirectory(source);
        string file = Path.Combine(source, "Probe.cs");
        try
        {
            Check(LiveSource.Find(Path.Combine(mod, "missing"), "LiveProbe.dll") == null, "No source folder means no live mode");

            File.WriteAllText(file, "public static class Probe { public static int Count; public static int Value() => 1; public static void Bump() => Count++; }");
            var live = LiveSource.Find(mod, Path.Combine(mod, "LiveProbe.dll"), intervalMilliseconds: 0)!;
            var assembly = new AssemblyLoadContext("live", isCollectible: true).LoadFromStream(new MemoryStream(live.Image));
            var type = assembly.GetType("Probe")!;
            int Value() => (int)type.GetMethod("Value")!.Invoke(null, null)!;
            Check(assembly.GetName().Name == "LiveProbe" && Value() == 1, "Mod source compiled under the mod assembly name");
            Check(!live.HasChanges(), "Unchanged sources are not reported");

            type.GetMethod("Bump")!.Invoke(null, null);
            File.WriteAllText(file, "public static class Probe { public static int Count; public static int Value() => 2 + Count * 100; public static void Bump() => Count++; }");
            Check(live.HasChanges(), "Edited source detected");
            bool patched = live.TryPatch(assembly, out string reason);
            if (System.Reflection.Metadata.MetadataUpdater.IsSupported)
                Check(patched && Value() == 102, "Method body patched in place, state kept: " + reason);
            else
                Check(!patched, "Without runtime edit support the change waits for a swap");

            File.WriteAllText(file, "public static class Probe { public static int Count; public static int Value() => 3; public static int Added() => 4; public static void Bump() => Count++; }");
            Check(live.HasChanges() && !live.TryPatch(assembly, out _), "Added member requires a swap");
            var swapped = new AssemblyLoadContext("swap", isCollectible: true).LoadFromStream(new MemoryStream(live.Rebuild()));
            Check((int)swapped.GetType("Probe")!.GetMethod("Added")!.Invoke(null, null)! == 4, "Swap loads the rebuilt sources");

            File.WriteAllText(file, "public static class Probe { broken");
            Check(live.HasChanges() && live.TryPatch(swapped, out string error) && error.Contains("컴파일 실패"),
                "Sources that do not compile keep the running code");
        }
        finally
        {
            Directory.Delete(mod, recursive: true);
        }
        return checks;
    }
}
