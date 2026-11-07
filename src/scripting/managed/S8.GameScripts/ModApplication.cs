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
        Effects.Register<WoundedRecoveryComponent>(new(
            ScriptedTraitCatalog.RecoveryEffect,
            "턴 종료 부상병 전량 회복",
            "보유 장수의 소속 부대를 게임의 회복 상한까지 회복합니다."));
        scriptedTraits = new ScriptedTraitCatalog(Context, Effects);
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
        traits.Dispose();
        jewels.Dispose();
    }
}
