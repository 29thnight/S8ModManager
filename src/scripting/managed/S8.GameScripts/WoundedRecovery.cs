using S8.Scripting;

namespace S8.GameScripts;

public sealed class WoundedRecoveryComponent : ScriptedTraitComponent, IBattleTurnEndEffect
{
    public void OnBattleTurnEnded()
    {
        var unit = GameObject.GetComponent<BattleMembershipComponent>()!.Unit;

        if (unit.Soldiers > 0 && unit.Capacity > unit.Soldiers)
        {
            Show("부상병을 치료하라!");
            unit.RestoreSoldiers(unit.Capacity - unit.Soldiers);
        }
    }
}
