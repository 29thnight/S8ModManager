using System.Globalization;
using S8.Scripting;

namespace S8.ManagerMod;

// The "모드" tab on the title screen's options: one row per mod the manager found.
// A toggle takes effect at once; the manager then sends Configure, and the tab is
// rebuilt from the new state. This mod lists itself without a toggle, so the tab
// that switches mods back on cannot be switched off from the tab.
public sealed class ManagerMod : ScriptMod
{
    public const string SelfId = "s8.manager";
    private static readonly string[] Toggle = ["끔", "켬"];
    private static readonly string[] Required = ["필수"];

    private ModCatalog catalog = null!;

    protected override void OnLoad() => Refresh();

    protected override void OnEvent(ScriptEvent kind, int owner, int value)
    {
        if (kind == ScriptEvent.Configure)
            Refresh();
    }

    private void Refresh()
    {
        catalog = Ordered(ModCatalog.Read());
        SetOptionsTab("모드", Rows(catalog), Changed);
    }

    // The manager lists skipped mods first, so a toggle reorders its list. Rows on
    // screen keep their index across a refresh, so the order must not depend on state.
    public static ModCatalog Ordered(ModCatalog catalog) => catalog with
    {
        Mods = catalog.Mods.OrderBy(mod => mod.Id, StringComparer.Ordinal)
            .ThenBy(mod => mod.Runtime, StringComparer.Ordinal)
            .ThenBy(mod => mod.Version, StringComparer.Ordinal)
            .ToList()
    };

    public static List<OptionRow> Rows(ModCatalog catalog) =>
        catalog.Mods.Select((mod, index) => mod.Id == SelfId
                ? new OptionRow(index, Label(mod), Describe(catalog, mod), Required)
                : new OptionRow(index, Label(mod), Describe(catalog, mod), Toggle, mod.Enabled ? 1 : 0))
            .ToList();

    private void Changed(int action, int choice)
    {
        var mod = catalog.Mods[action];
        if (mod.Id != SelfId)
            ModCatalog.SetEnabled(mod.Id, choice == 1);
    }

    // The options font has no middle dot: U+00B7 draws as two dots, and U+30FB and
    // the game's own tab glyph (U+E060) draw blank in rows and the description panel.
    private static string Label(ModInfo mod) => mod.Name.Replace('·', '/').Replace('・', '/');

    public static string Describe(ModCatalog catalog, ModInfo mod)
    {
        var lines = new List<string>
        {
            $"【{Label(mod)}】",
            $"{mod.Id} / 판 {mod.Version} / {mod.Runtime.ToUpperInvariant()}",
            "상태: " + mod.State switch
            {
                ModLoadState.Loaded => "적용 중",
                ModLoadState.Skipped => "건너뜀",
                _ => "불러오기 실패"
            }
        };
        if (mod.Reason.Length != 0)
            lines.Add("사유: " + mod.Reason);

        lines.Add(mod.ApiVersion == 0
            ? "요구 API: 제한 없음"
            : $"요구 API: {mod.ApiVersion} (매니저 {catalog.HostApiVersion}, {(mod.ApiVersion == catalog.HostApiVersion ? "일치" : "불일치")})");
        lines.Add(mod.GameBuild == 0
            ? "지원 게임 빌드: 제한 없음"
            : $"지원 게임 빌드: {Hex(mod.GameBuild)} (현재 {Hex(catalog.GameBuild)}, {(mod.GameBuild == catalog.GameBuild ? "일치" : "불일치")})");

        if (mod.Dependencies.Count != 0)
            lines.Add("필요 모드: " + string.Join(", ", mod.Dependencies));
        if (mod.Conflicts.Count != 0)
        {
            var active = mod.Conflicts.Where(id => catalog.Mods.Any(other => other.Id == id && other.State == ModLoadState.Loaded)).ToList();
            lines.Add("충돌 모드: " + string.Join(", ", mod.Conflicts) +
                      (active.Count == 0 ? " (현재 충돌 없음)" : $" (적용 중: {string.Join(", ", active)})"));
        }
        else
        {
            lines.Add("충돌 모드: 없음");
        }

        lines.Add("");
        lines.Add(mod.Id == SelfId ? "이 탭을 표시하는 모드라 끌 수 없습니다." : "켜고 끄면 바로 적용됩니다.");
        return string.Join("\n", lines);
    }

    private static string Hex(uint value) => value.ToString("X8", CultureInfo.InvariantCulture);
}
