using S8.GameScripts;
using S8.Scripting;

// The trait file shared with the overlay cheat: where it lives and how voices join it.
internal static class TraitConfigurationChecks
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

        string game = Path.Combine(Path.GetTempPath(), "s8-trait-config-" + Guid.NewGuid().ToString("N"));
        string mod = Path.Combine(game, "scripts", "mods", "s8.editors");
        Directory.CreateDirectory(mod);
        try
        {
            Check(TraitConfiguration.ConfigPath(game, mod) == Path.Combine(mod, TraitConfiguration.FileName),
                "Without hid.dll the editor keeps its own file");
            File.WriteAllBytes(Path.Combine(game, "hid.dll"), []);
            Check(TraitConfiguration.SharesCheatFile(game) &&
                  TraitConfiguration.ConfigPath(game, mod) == Path.Combine(game, TraitConfiguration.FileName),
                "With hid.dll the editor edits the cheat's file in the game folder");
            File.WriteAllText(Path.Combine(mod, TraitConfiguration.FileName),
                "{\"traits\":[],\"customNames\":{\"3\":{\"name\":\"a\",\"voice\":12},\"4\":{\"name\":\"b\"}}}");
            TraitConfiguration.MoveToCheatFile(game, mod);
            string shared = File.ReadAllText(Path.Combine(game, TraitConfiguration.FileName));
            Check(!shared.Contains("voice") && shared.Contains("\"a\"") &&
                  File.ReadAllText(Path.Combine(mod, TraitConfiguration.VoiceFileName)).Contains("12") &&
                  File.Exists(Path.Combine(mod, TraitConfiguration.FileName)),
                "Earlier edits move to the cheat's file once, voices split out");
            File.WriteAllText(Path.Combine(game, TraitConfiguration.FileName), "{\"traits\":[]}");
            TraitConfiguration.MoveToCheatFile(game, mod);
            Check(File.ReadAllText(Path.Combine(game, TraitConfiguration.FileName)) == "{\"traits\":[]}",
                "The cheat's existing file is never overwritten");
        }
        finally
        {
            Directory.Delete(game, true);
        }

        var definitions = new TraitDefinition?[TraitCatalog.Count];
        definitions[3] = new TraitDefinition { HasMetadata = true, Name = "a" };
        definitions[4] = new TraitDefinition();
        Check(TraitConfiguration.MergeVoices(definitions, "{\"3\":12,\"4\":7,\"5\":9}"u8) &&
              definitions[3]!.Voice == 12 && definitions[4]!.Voice == -1 && definitions[5] == null,
            "Voices apply only to traits the configuration names");
        Check(!TraitConfiguration.MergeVoices(definitions, "{\"3\":141}"u8) &&
              !TraitConfiguration.MergeVoices(definitions, "[1]"u8),
            "Bad voice file is rejected");
        return checks;
    }
}
