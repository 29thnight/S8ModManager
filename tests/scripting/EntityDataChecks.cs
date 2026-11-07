using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using S8.Scripting;

internal static unsafe class EntityDataChecks
{
    [StructLayout(LayoutKind.Sequential)]
    private struct Request
    {
        public ulong Session;
        public uint Kind, Index;
        public long Value;
    }

    private static ulong session = 5;
    private static Request last;
    private static ulong lastTarget, submittedMask;
    private static ushort submittedRaw;
    private static int tokenDelta;
    private static bool emptyAffiliation, failAffiliation;
    private static readonly Dictionary<(ulong Id, uint Kind, uint Index), long> values = [];

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static long Invoke(int operation, ulong target, long argument, void* data, uint bytes)
    {
        if (operation == 29)
        {
            new Span<byte>(data, (int)bytes).Clear();
            *(ulong*)data = session;
            *(ushort*)((byte*)data + 8) = ushort.MaxValue;
            return 0;
        }
        if (operation is 30 or 31)
        {
            new Span<byte>(data, (int)bytes).Clear();
            *(ulong*)data = 0x100000 + target * 0x1000;
            *(ulong*)((byte*)data + 8) = session;
            *(ushort*)((byte*)data + 16) = (ushort)target;
            return 1;
        }
        if (operation is 33 or 34 or 35)
        {
            if (bytes != 24 || argument != 0)
                return (int)ConnectionStatus.InvalidApi;
            last = *(Request*)data;
            lastTarget = target;
            if (last.Session == 0)
                return (int)ConnectionStatus.InvalidApi;
            if (last.Session != session)
                return (int)ConnectionStatus.StaleSession;
            var key = (target, last.Kind, last.Index);
            if (operation == 34)
                values[key] = last.Kind == 3 ? 987 : last.Value;
            if (operation == 35)
                values[key] = values.GetValueOrDefault(key) + last.Value;
            return values.GetValueOrDefault(key);
        }
        if (operation == 36)
        {
            lastTarget = target;
            tokenDelta = *(int*)data;
            return 0;
        }
        if (operation is 37 or 38)
        {
            if (failAffiliation)
                return (int)ConnectionStatus.InvalidBinding;
            new Span<byte>(data, (int)bytes).Clear();
            *(ushort*)data = (ushort)target;
            if (emptyAffiliation)
                return 0;
            *(ushort*)((byte*)data + 2) = 100;
            *(ushort*)((byte*)data + 4) = 565;
            ((byte*)data)[6] = 7;
            ((byte*)data)[8] = 5;
            ((byte*)data)[9] = 12;
            ((byte*)data)[10] = 1;
            ((byte*)data)[11] = 1 | 2 | 4 | 8 | 16 | 32;
            *(ulong*)((byte*)data + 16) = 3;
            return 0;
        }
        if (operation == 16)
        {
            new Span<byte>(data, (int)bytes).Clear();
            *(ulong*)data = session;
            *(ulong*)((byte*)data + 8) = 1UL << 2;
            ((byte*)data)[72 + (66 >> 3)] = 1 << (66 & 7);
            ((byte*)data)[106] = ((byte*)data)[107] = 1;
            return 0;
        }
        if (operation == 17)
        {
            submittedMask = *(ulong*)((byte*)data + 8);
            submittedRaw = *(ushort*)((byte*)data + 72);
            return 0;
        }
        return (int)ConnectionStatus.InvalidApi;
    }

    public static int Run()
    {
        int checks = 0;
        void Check(bool result, string message)
        {
            if (!result)
                throw new Exception(message);
            ++checks;
        }

        NativeApi api = new()
        {
            Version = NativeContract.Version,
            Size = (uint)sizeof(NativeApi),
            Connection = new() { Capabilities = NativeCapability.GameData | NativeCapability.Ui },
            Invoke = &Invoke
        };
        nint previous = Native.Enter(&api);
        try
        {
            var catalog = JewelCatalog.Load(Path.Combine("resources", "jewel_definitions.json"));
            using var world = new GameWorld(catalog);
            world.BeginSession(session, 565);
            var officer = world.GetOfficer(565);
            var stats = officer.GetComponent<OfficerStatsComponent>()!;
            Check(stats.Set(OfficerStat.Intelligence, 95) == 95 && lastTarget == 565 && last.Index == 3,
                "Officer stat uses its native ID and the owning officer");
            Check(officer.GetComponent<OfficerFameComponent>()!.Set(OfficerFame.Civil, 1000) == 987,
                "Fame setter returns the native applied value instead of echoing the request");
            var training = officer.GetComponent<OfficerTrainingComponent>()!;
            Check(training.SetTactic(35, TrainingLevel.Three) == TrainingLevel.Three && last.Kind == 4,
                "Tactics use the tactic service");
            Check(training.SetSkill(24, TrainingLevel.Two) == TrainingLevel.Two && last.Kind == 5,
                "Skills use the distinct skill service");
            var traits = officer.GetComponent<OfficerTraitsComponent>()!;
            Check(traits.SetSlot(2, 254) == 254 && officer.GetComponent<OfficerComponent>()!.PersonalityIds[2] == 254,
                "Native trait assignment updates the officer snapshot");

            var city = world.GetCity(1).GetComponent<CityAdministrationComponent>()!;
            city.Set(CityValue.Gold, 100);
            Check(city.Add(CityValue.Gold, -30) == 70 && lastTarget == 1 && last.Kind == 7,
                "City resource deltas preserve their sign and entity kind");

            var affiliation = officer.GetComponent<OfficerAffiliationComponent>()!.Read();
            Check(affiliation.RoleId == 5 && affiliation.RankId == 7 && affiliation.ForceId == 12
                && affiliation.LordId == 100 && affiliation.CityId == 1,
                "Officer affiliation preserves native data IDs without address-byte decoding");
            var cityAffiliation = world.GetCity(1).GetComponent<CityAffiliationComponent>()!;
            var ownership = cityAffiliation.Read();
            Check(ownership.GovernorId == 565 && ownership.LordId == 100 && ownership.DistrictNumber == 3,
                "City ownership decodes distinct governor, ruler and district links");
            emptyAffiliation = true;
            ownership = cityAffiliation.Read();
            Check(ownership.ForceId == null && ownership.GovernorId == null && ownership.LordId == null,
                "An unoccupied city exposes absent links as nullable IDs");
            emptyAffiliation = false;
            failAffiliation = true;
            bool affiliationError = false;
            try { cityAffiliation.Read(); }
            catch (EngineCallException) { affiliationError = true; }
            failAffiliation = false;
            Check(affiliationError, "An unreadable ownership link is not reported as an unoccupied city");

            var definition = catalog.Get(2);
            var player = world.GetPlayer();
            Check(player == world.GetPlayer() && player.Character == officer, "Player identity and character use the session registry");
            var resources = player.Resources;
            Check(resources.AddMoney(1000) == 1000 && lastTarget == 565,
                "Player money uses the current officer's existing balance");
            var jewels = player.Jewels;
            Check(definition.RawId == 66 && jewels.IsOpened(2), "Jewel catalog separates UI IDs from unlock bits");
            jewels.Disable(2);
            Check(submittedRaw == 66 && (submittedMask & (1UL << 2)) == 0,
                "Closing a jewel submits its raw ID and clears its forced flag");
            jewels.ForceApply(2);
            Check(submittedRaw == 66 && (submittedMask & (1UL << 2)) != 0,
                "ForceApply opens and forces the jewel in one native request");

            world.ObserveProtagonist(100);
            Check(player.Character == world.FindOfficer(100), "The existing Player follows protagonist changes");
            resources.AddMoney(100);
            Check(lastTarget == 100, "A cached resource service follows the new protagonist");
            resources.AddFriendshipTokens(3);
            Check(lastTarget == 100 && tokenDelta == 3, "Friendship tokens use the current protagonist context");
            bool previousJewelsRejected = false;
            try { jewels.Read(); }
            catch (InvalidOperationException) { previousJewelsRejected = true; }
            world.ObserveProtagonist(565);
            Check(previousJewelsRejected && player.Jewels != jewels,
                "Changing away and back cannot reactivate an old jewel component");

            var me = world.PlayerOfficer;
            me.Gold = 50;
            me.Gold += 25;
            Check(me.Gold == 75 && lastTarget == 565 && last.Kind == 1, "Officer handle reads and writes gold by property");
            me.Intelligence = 90;
            Check(me.Intelligence == 90 && last.Index == 3, "Officer handle stat uses the native stat ID");
            Check(me == world.Officer(565), "Handles from the same session compare equal");
            var town = world.City(1);
            town.Grain = 400;
            Check(town.Grain == 400 && lastTarget == 1 && last.Kind == 7 && last.Index == (uint)CityValue.Grain,
                "City handle writes its own field");
            Check(typeof(City).GetProperty(nameof(City.TechnologyLimit))!.CanWrite == false,
                "A read-only city field has no setter");

            ++session;
            world.BeginSession(session, 565);
            bool staleHandleRejected = false;
            try { _ = me.Gold; }
            catch (EngineCallException) { staleHandleRejected = true; }
            Check(staleHandleRejected, "A handle from an earlier session is rejected");
            bool oldOfficerRejected = false, oldJewelsRejected = false, oldPlayerRejected = false, oldResourcesRejected = false;
            try { stats.Get(OfficerStat.Intelligence); }
            catch (EngineCallException) { oldOfficerRejected = true; }
            try { jewels.Read(); }
            catch (InvalidOperationException) { oldJewelsRejected = true; }
            try { _ = player.Character; }
            catch (InvalidOperationException) { oldPlayerRejected = true; }
            try { resources.AddMoney(1); }
            catch (InvalidOperationException) { oldResourcesRejected = true; }
            Check(oldOfficerRejected && oldJewelsRejected && oldPlayerRejected && oldResourcesRejected,
                "Old session components cannot edit the replacement session");
        }
        finally
        {
            Native.Leave(previous);
        }
        return checks;
    }
}
