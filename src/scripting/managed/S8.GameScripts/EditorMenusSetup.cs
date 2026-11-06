using S8.Scripting;

namespace S8.GameScripts;

// The editor entry points exist before a save is loaded and across save changes.
public sealed class EditorMenusSetup : GameSetup
{
    public override void Configure(GameApplication application)
    {
        var menu = application.CreateObject("ModMenus").AddComponent<MenuExtensionComponent>();
        menu.Add(MenuSurface.TitleEdit, 0, "기재 편집", 5, 5);
        menu.Add(MenuSurface.InGameEdit, 0, "기재", 5, 5);
        menu.AddWindowRow(4, 2, "보주", "보주편집");
    }
}
