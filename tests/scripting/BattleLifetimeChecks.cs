using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using S8.Scripting;

internal static unsafe class BattleLifetimeChecks
{
    [StructLayout(LayoutKind.Sequential)]
    private struct Context
    {
        public ulong Session, Sequence, ActingUnit;
        public ScriptEvent Kind;
        public uint Reserved;
        public ulong WorldSession;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Unit
    {
        public ulong Token, Identity;
        public uint Soldiers, Capacity;
        public fixed ushort Officers[3];
        public fixed ushort Personalities[9];
    }

    public sealed class Counter : Component
    {
        public int Value, InitialValue, Enables, Disables;

        public override void OnEnable()
        {
            InitialValue = Value;
            ++Enables;
        }

        public override void OnDisable() => ++Disables;
    }

    public sealed class TraitCounter : ScriptedTraitComponent, IBattleTurnEndEffect
    {
        public int Calls, Enables;
        public ushort InitialPersonality;

        public override void OnEnable()
        {
            InitialPersonality = PersonalityId;
            ++Enables;
        }

        public void OnBattleTurnEnded() => ++Calls;
    }

    private static Context current;
    private static Unit[] roster = [];
    private static ulong lastToken;
    private static ushort protagonist = ushort.MaxValue;
    private static int checks;
    private static ulong addressOffset;

    private static void Check(bool value, string message)
    {
        if (!value)
            throw new Exception(message);

        ++checks;
    }

    private static Unit Record(ulong id, ulong token, ushort officer, uint soldiers = 90)
    {
        Unit result = new() { Identity = id, Token = token, Soldiers = soldiers, Capacity = 100 };

        result.Officers[0] = officer;
        result.Officers[1] = result.Officers[2] = ushort.MaxValue;
        result.Personalities[0] = 254;
        return result;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static long Invoke(int operation, ulong target, long argument, void* data, uint bytes)
    {
        if (operation == 29)
        {
            new Span<byte>(data, (int)bytes).Clear();
            *(ulong*)data = current.WorldSession;
            *(ushort*)((byte*)data + 8) = protagonist;
            return 0;
        }

        if (operation is 30 or 31)
        {
            new Span<byte>(data, (int)bytes).Clear();
            if (argument != (long)current.WorldSession)
                return (int)ConnectionStatus.StaleSession;
            if (operation == 30 ? target is not (565 or 100) : target != 1)
                return 0;

            *(ulong*)data = 0x100000 + addressOffset + target * 0x1000;
            *(ulong*)((byte*)data + 8) = current.WorldSession;
            *(ushort*)((byte*)data + 16) = (ushort)target;
            *(ushort*)((byte*)data + 18) = 254;
            return 1;
        }

        if (operation == 28) // GetBattleContext, ABI v8.
        {
            *(Context*)data = current;
            return 0;
        }

        if (operation == 25) // ReadBattleUnits always provides the full roster.
        {
            if (data != null)
                fixed (Unit* source = roster)
                    Buffer.MemoryCopy(source, data, bytes, roster.Length * sizeof(Unit));

            return roster.Length;
        }

        if (operation is 26 or 55)
        {
            lastToken = target;
            int index = Array.FindIndex(roster, unit => unit.Token == target);
            if (operation == 55)
                return roster[index].Soldiers;
            roster[index].Soldiers = (uint)argument;
            return 0;
        }

        return (int)ConnectionStatus.InvalidApi;
    }

    public static int Run()
    {
        Check(sizeof(Unit) == 48 && sizeof(Context) == 40, "Battle v8 wire layouts");

        NativeApi api = new()
        {
            Version = NativeContract.Version,
            Size = (uint)sizeof(NativeApi),
            Connection = new() { Capabilities = NativeCapability.GameData },
            Invoke = &Invoke
        };
        nint previous = Native.Enter(&api);

        try
        {
            current = new() { WorldSession = 10 };
            protagonist = 565;
            using (var sceneWorld = new GameWorld())
            {
                int starts = 0, ends = 0;

                sceneWorld.SessionStarted += _ => ++starts;
                sceneWorld.SessionEnded += _ => ++ends;
                sceneWorld.BeginSession(10);
                sceneWorld.BeginSession(10);
                var officer = sceneWorld.GetOfficer(565);
                var city = sceneWorld.GetCity(1);
                Check(officer.GetComponent<OfficerComponent>()!.IsBound
                    && city.GetComponent<CityComponent>()!.IsBound, "Entities hold resolved native references");
                ulong oldAddress = officer.GetComponent<OfficerComponent>()!.Memory.Address;
                addressOffset = 0x100000;
                Check(sceneWorld.GetOfficer(565) == officer
                    && officer.GetComponent<OfficerComponent>()!.Memory.Address != oldAddress,
                    "Native relocation refreshes the binding without replacing the GameObject");
                bool missing = false;
                try { sceneWorld.GetCity(2); }
                catch (KeyNotFoundException) { missing = true; }
                Check(missing && sceneWorld.FindCity(2) == null, "Missing entity does not create a GameObject");
                var sceneOfficerState = officer.AddComponent<Counter>(counter => counter.Value = 23);
                var cityState = city.AddComponent<Counter>();
                var cityScene = sceneWorld.EnterScene(GameSceneKind.City, 1);

                cityScene.Track(officer);
                cityScene.Track(city);
                var temporary = cityScene.CreateObject("CityWindow").AddComponent<Counter>();

                sceneWorld.EnterScene(GameSceneKind.Council, 1);
                Check(temporary.Disables == 1, "Scene exit disposes scene-owned objects");
                Check(sceneWorld.GetOfficer(565) == officer && sceneWorld.GetCity(1) == city
                    && sceneOfficerState.Value == 23 && sceneOfficerState.Disables == 0 && cityState.Disables == 0,
                    "City/council transition preserves world entities and components");
                sceneWorld.CurrentScene!.Dispose();
                Check(sceneWorld.CurrentScene == null, "Direct scene disposal releases the world's current scene");
                sceneWorld.EndSession();
                sceneWorld.EndSession();
                Check(starts == 1 && ends == 1 && sceneOfficerState.Disables == 1 && cityState.Disables == 1,
                    "Loaded-session lifecycle disposes entities once");
            }

            using (var loaded = new GameWorld())
            {
                current = new() { WorldSession = 90 };
                protagonist = 565;
                bool availableAtStart = false;
                loaded.SessionStarted += _ => availableAtStart = loaded.Protagonist != null;
                loaded.RefreshSession();
                var hero = loaded.Protagonist!;
                var role = hero.GetComponent<ProtagonistComponent>();
                var state = hero.AddComponent<Counter>(counter => counter.Value = 17);
                loaded.RefreshSession();
                Check(availableAtStart && hero == loaded.GetOfficer(565)
                    && hero.GetComponent<ProtagonistComponent>() == role,
                    "Loaded protagonist is the stable officer object and exists before session notification");

                protagonist = 100;
                loaded.RefreshSession();
                Check(loaded.Protagonist == loaded.GetOfficer(100) && state.Disables == 0
                    && hero.GetComponent<ProtagonistComponent>() == null,
                    "Changing protagonist moves only the role");

                current = new() { WorldSession = 91 };
                protagonist = 565;
                loaded.RefreshSession();
                Check(loaded.Protagonist != hero && state.Disables == 1, "Loading replaces the protagonist object");
                loaded.EndSession();
                Check(loaded.Protagonist == null, "Session exit clears the protagonist reference");
            }

            using var game = new GameWorld();
            using var world = new BattleWorld(game);

            current = new() { WorldSession = 1, Session = 1, Sequence = 1, ActingUnit = 11, Kind = ScriptEvent.BattleUnitActionEnded };
            roster = [Record(11, 101, 565), Record(22, 102, 100)];
            world.Refresh();
            var unitObject = world.FindUnit(11)!;
            var officerObject = world.FindOfficer(565)!;
            var unit = unitObject.GetComponent<BattleUnitComponent>()!;
            var unitState = unitObject.AddComponent<Counter>(counter => counter.Value = 7);
            var officerState = officerObject.AddComponent<Counter>(counter => counter.Value = 13);
            var removedUnit = world.FindUnit(22)!.AddComponent<Counter>();
            var removedOfficer = world.FindOfficer(100)!.AddComponent<Counter>();
            var registry = new ScriptEffectRegistry();

            registry.Register<TraitCounter>(new("counter", "Counter", "Persistent effect state"));
            var effect = (TraitCounter)registry.Attach("counter", officerObject, 254);
            effect.OnBattleTurnEnded();

            Check(world.Units.Count == 2 && world.EventUnits.Count == 1, "Action scope retains the full roster");
            Check(effect.InitialPersonality == 254, "Effect identity is available during OnEnable");
            Check(unitState.InitialValue == 7 && unitState.Enables == 1, "Initialize before OnEnable");
            world.EndEvent();
            Check(!unit.IsCurrent, "Native token expires after the event");

            current = new() { WorldSession = 1, Session = 1, Sequence = 2, Kind = ScriptEvent.BattleTurnEnded };
            roster = [Record(11, 201, 565, 95), Record(22, 202, 100)];
            world.Refresh();
            Check(ReferenceEquals(world.FindUnit(11), unitObject), "Unit GameObject survives events");
            Check(ReferenceEquals(world.FindOfficer(565), officerObject), "Officer GameObject survives events");
            Check(unitState.Value == 7 && officerState.Value == 13, "Component fields survive events");
            effect.OnBattleTurnEnded();
            Check(ReferenceEquals(officerObject.GetComponent<TraitCounter>(), effect)
                && effect.Calls == 2 && effect.Enables == 1, "Effect instance state survives events");
            Check(unit.Soldiers == 95 && unitState.Enables == 1, "Snapshot refresh does not enable components again");

            current = new() { WorldSession = 1, Session = 1, Sequence = 3, ActingUnit = 22, Kind = ScriptEvent.BattleUnitActionEnded };
            roster = [Record(11, 301, 565, 95), Record(22, 302, 100)];
            world.Refresh();
            world.FindUnit(22)!.GetComponent<BattleUnitComponent>()!.RestoreSoldiers(0);
            Check(lastToken == 302, "Nested event uses its own native token");
            world.EndEvent();
            roster = [Record(11, 201, 565, 95), Record(22, 202, 100)];
            Check(unit.RestoreSoldiers(20) == 5 && unit.Soldiers == 100,
                "Script clamps restoration and publishes the actual engine result");
            Check(lastToken == 201 && world.CurrentEvent!.Sequence == 2, "Nested event restores the outer scope");
            world.EndEvent();
            Check(!unit.IsCurrent, "Outer event exit invalidates restored tokens");

            current = new() { WorldSession = 1, Session = 1, Sequence = 4, Kind = ScriptEvent.BattleTurnStarted };
            roster = [Record(11, 401, 565)];
            world.Refresh();
            Check(removedUnit.Disables == 1 && removedOfficer.Disables == 0, "Retiring a unit preserves its world officer");
            Check(game.FindOfficer(100)!.GetComponent<BattleMembershipComponent>() == null,
                "Retiring an officer removes only battle membership");
            Check(ReferenceEquals(world.FindUnit(11), unitObject), "Other unit retirement preserves this unit");
            var detached = officerObject.AddComponent<Counter>();
            Check(officerObject.RemoveComponent(detached) && !officerObject.RemoveComponent(detached)
                && detached.Disables == 1, "Removing an effect disables it once");
            world.EndEvent();

            world.Dispose();
            Check(officerState.Disables == 0 && effect.Calls == 2 && game.FindOfficer(565) == officerObject,
                "Battle exit preserves officer and effect state");
            Check(officerObject.GetComponent<BattleMembershipComponent>() == null, "Battle exit detaches membership");

            current = new() { WorldSession = 1, Session = 2, Sequence = 5, Kind = ScriptEvent.BattleTurnStarted };
            roster = [Record(11, 501, 565)];
            world.Refresh();
            Check(world.FindOfficer(565) == officerObject && officerState.Value == 13,
                "Another battle reuses the loaded-session officer");
            world.EndEvent();

            current = new() { WorldSession = 2, Session = 3, Sequence = 6, Kind = ScriptEvent.BattleTurnStarted };
            roster = [Record(11, 601, 565)];
            world.Refresh();
            Check(unitState.Disables == 1 && officerState.Disables == 1, "Session replacement disables old components");
            Check(removedOfficer.Disables == 1, "Loading another session disposes officers absent from battle");
            Check(!ReferenceEquals(world.FindUnit(11), unitObject), "Reused identity in another session creates a new object");
            var final = world.FindUnit(11)!.AddComponent<Counter>();
            world.EndEvent();
            world.Dispose();
            world.Dispose();
            Check(final.Disables == 1 && world.SessionId == 0, "Session disposal is idempotent");
        }
        finally
        {
            Native.Leave(previous);
        }

        return checks;
    }
}
