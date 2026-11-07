using S8.Scripting;

namespace S8.GameScripts;

public sealed class ModApplication : ScriptMod
{
    private readonly TraitEditorModule traits = new();
    private readonly JewelEditorModule jewels = new();
#if S8_EXPERIMENTAL
    private ScriptedTraitCatalog scriptedTraits = null!;
#endif

    public override bool CanReload => !jewels.HasDrafts;

    protected override void OnLoad()
    {
#if S8_EXPERIMENTAL
        // Effects come from Lua mods; this mod only owns which traits hold them.
        scriptedTraits = new ScriptedTraitCatalog(Context);
        traits.ScriptedTraits = scriptedTraits;
#endif
        traits.Initialize(Context);
        jewels.Initialize(Context);

        AddMenu(MenuSurface.TitleEdit, "기재 편집", traits.Open);
        AddMenu(MenuSurface.InGameEdit, "기재", traits.Open);
        AddWindowRow(4, "보주", "보주편집", jewels.Open, jewels.CloseWindow);
    }

    protected override void OnUiClosed()
    {
        traits.Dispose();
        jewels.CloseUi();
    }

    // Switched off by the manager: the game's own traits and jewel effects return.
    protected override void OnDisabled()
    {
        TraitCatalog.Publish(new TraitDefinition?[TraitCatalog.Count]);
        JewelComponent.RestoreForced(new ulong[8]);
    }

    protected override void OnUnload()
    {
#if S8_EXPERIMENTAL
        scriptedTraits?.Dispose();
#endif
        traits.Dispose();
        jewels.Dispose();
    }
}
