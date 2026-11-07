using S8.ManagerMod;
using S8.Scripting;

// The mods tab: the manager's mod list text and the rows the manager mod builds from it.
internal static class OptionsChecks
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

        var catalog = ModCatalog.Parse(
            "21\t69A67ED1\n" +
            "s8.manager\t모드 관리\t0.0.1\tjit\t1\tloaded\t\t21\t\t\t\n" +
            "s8.editors\t기재·보주 편집\t0.0.1\tjit\t1\tloaded\t\t21\t69A67ED1\ts8.core>=1.0.0\tother\n" +
            "old.mod\t옛 모드\t1.2.3\taot\t0\tskipped\tmods.json에서 꺼진 모드입니다.\t20\t12345678\t\tother\n");
        Check(catalog.HostApiVersion == 21 && catalog.GameBuild == 0x69A67ED1 && catalog.Mods.Count == 3,
            "Mod list header and lines parsed");
        var old = catalog.Mods[2];
        Check(old is { Id: "old.mod", Version: "1.2.3", Runtime: "aot", Enabled: false, State: ModLoadState.Skipped, ApiVersion: 20, GameBuild: 0x12345678 } &&
              old.Reason.Contains("꺼진") && old.Conflicts.SequenceEqual(["other"]),
            "Skipped mod keeps its reason and declarations");
        Check(catalog.Mods[1].Dependencies.SequenceEqual(["s8.core>=1.0.0"]), "Dependencies listed");

        var rows = ManagerMod.Rows(catalog);
        Check(rows.Count == 3 && rows[0].Choices.Count == 1 && rows[1].Choices.SequenceEqual(["끔", "켬"]) &&
              rows[1].Current == 1 && rows[2].Current == 0,
            "Manager row has no toggle; other rows show their mods.json state");
        Check(rows[1].Label == "기재/보주 편집" && !rows[1].Description.Contains('・'), "No middle dot, which the options font draws blank");
        Check(rows[1].Description.Contains("일치") && !rows[1].Description.Contains("불일치"),
            "Matching API and game build reported");
        Check(rows[2].Description.Contains("건너뜀") && rows[2].Description.Contains("사유:") &&
              rows[2].Description.Contains("불일치") && rows[2].Description.Contains("현재 충돌 없음"),
            "Skipped mod shows state, reason, mismatches and conflicts");
        Check(ModCatalog.Parse("21\t\n").GameBuild == 0, "Unknown game build parses as zero");
        var external = ManagerMod.Rows(ModCatalog.Parse(
            "21\t\nexternal.hid\thid.dll\t-\texternal\t1\tloaded\t\t0\t\t\t\n"))[0];
        Check(external.Choices.SequenceEqual(["외부"]) && external.Description.Contains("외부 DLL") &&
              !external.Description.Contains("EXTERNAL") && !external.Description.Contains("켜고 끄면"),
            "External DLL is listed as a mod without a toggle");
        var reordered = ManagerMod.Ordered(catalog with { Mods = [catalog.Mods[2], catalog.Mods[0], catalog.Mods[1]] });
        Check(reordered.Mods.Select(mod => mod.Id).SequenceEqual(["old.mod", "s8.editors", "s8.manager"]),
            "Tab rows keep one order whatever the manager's list order");
        return checks;
    }
}
