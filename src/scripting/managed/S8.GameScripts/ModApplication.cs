using S8.Scripting;

namespace S8.GameScripts;

public sealed class ModApplication : IScriptModule
{
    private readonly TraitEditorModule traits = new();
    private readonly JewelEditorModule jewels = new();
    private GameApplication application = null!;
    private GameWorld world => application.World;
    private int activeEntry;
#if S8_EXPERIMENTAL
    private ScriptedTraitCatalog scriptedTraits = null!;
#endif

    public bool CanReload => !jewels.HasDrafts;

    public GameWorld World => world;

    public void Initialize(ModContext context)
    {
        application = new GameApplication(context, GetType().Assembly);
        bool initialized = false;
        try
        {
#if S8_EXPERIMENTAL
            var effects = new ScriptEffectRegistry();

            effects.Register<WoundedRecoveryComponent>(new(
                ScriptedTraitCatalog.RecoveryEffect,
                "턴 종료 부상병 전량 회복",
                "보유 장수의 소속 부대를 게임의 회복 상한까지 회복합니다."));
            scriptedTraits = new ScriptedTraitCatalog(context.GameDirectory, effects, world);
            traits.ScriptedTraits = scriptedTraits;
#endif
            traits.Initialize(context);
            jewels.Initialize(context);

            application.Initialize();
            world.RefreshSession();
            initialized = true;
        }
        finally
        {
            if (!initialized)
                Dispose();
        }
    }

    public void Dispatch(ScriptEvent kind, int owner, int value)
    {
        world.RefreshSession();
        if (world.DispatchActivity(kind, owner, value))
            return;
        if (application.Dispatch(kind, owner, value))
            return;
        if (world.Events.DispatchUi(kind, owner, value))
            return;
        switch (kind)
        {
            case ScriptEvent.BattleSessionEnded:
#if S8_EXPERIMENTAL
                scriptedTraits.EndBattle();
#endif
                if (world.SessionId != 0)
                    world.Events.Signal(WorldEventTriggers.BattleEnded);
                return;

            case ScriptEvent.BattleTurnStarted:
            case ScriptEvent.BattleUnitActionEnded:
            case ScriptEvent.BattleTurnEnded:
#if S8_EXPERIMENTAL
                scriptedTraits.DispatchBattleEvent(kind);
#endif
                return;

            case ScriptEvent.Configure:
                return;

            case ScriptEvent.MenuClick:
                activeEntry = 0;
                traits.Dispatch(ScriptEvent.Open, 0, 0);
                return;

            case ScriptEvent.WindowClick:
                activeEntry = 1;
                jewels.Dispatch(kind, owner, value);
                return;

            case ScriptEvent.WindowClosing:
                jewels.Dispatch(kind, owner, value);
                return;

            case ScriptEvent.Open:
                activeEntry = value;
                break;
        }

        if (activeEntry == 0)
            traits.Dispatch(kind, owner, value);
        else
            jewels.Dispatch(kind, owner, value);
    }

    public void Dispose()
    {
#if S8_EXPERIMENTAL
        scriptedTraits?.Dispose();
#endif
        traits.Dispose();
        jewels.Dispose();
        application.Dispose();
    }
}
