using S8.Scripting;

namespace S8.GameScripts;

public sealed class CouncilStrategyMenuSetup : GameSetup
{
    private const int MenuAction = 700;
    private const int CouncilRoomAction = 701;
    private GameWorld world = null!;

    public override void Configure(GameApplication application)
    {
        world = application.World;
        var menu = application.CreateObject("CouncilStrategyMenu").AddComponent<MenuExtensionComponent>();
        menu.AddPalace(MenuAction, "월간 상신");
        menu.AddPalace(CouncilRoomAction, "평정실 진입");
    }

    public override bool Handle(ScriptEvent kind, int owner, int value)
    {
        if (kind != ScriptEvent.MenuClick || value is not (MenuAction or CouncilRoomAction))
            return false;

        if (world.SessionId != 0)
            world.Events.Signal(value == CouncilRoomAction
                ? CouncilStrategyMenuEvent.RoomTrigger
                : CouncilStrategyMenuEvent.Trigger);
        return true;
    }
}

// Choice continuations receive a fresh session context from the common event UI.
internal sealed class CouncilStrategyMenuEvent(CouncilStrategyComponent strategy, bool inCouncilRoom = false)
    : WorldEvent(inCouncilRoom ? "s8.council-strategy.room" : "s8.council-strategy.menu",
        inCouncilRoom ? RoomTrigger : Trigger)
{
    internal const string Trigger = "s8.council-strategy.menu.open";
    internal const string RoomTrigger = "s8.council-strategy.room.open";
    private const int LeaveCouncil = 4;

    public override void Execute(WorldEventContext context)
    {
        if (!inCouncilRoom)
        {
            ShowMain(context);
            return;
        }

        context.OpenCouncilRoom(hidePalaceControls: true, ready: ShowMain);
    }

    private void ShowMain(WorldEventContext context)
    {
        string month = strategy.CurrentDate is { } date ? $"{date.Year}년 {date.Month}월" : "날짜 관측 대기";
        var world = context.World;
        ushort officer = PlayerId(world);
        var membership = world.GetPlayer().Character.GetComponent<OfficerAffiliationComponent>()!.Read();
        var city = membership.CityId is { } cityId
            ? world.GetCity(cityId).GetComponent<CityAffiliationComponent>()!.Read()
            : (CityAffiliation?)null;
        bool affiliated = city != null && membership.ForceId != null;
        bool commands = city is { } command && Commands(officer, command);
        string authority = membership.LordId == officer ? "군주" : city?.GovernorId == officer ? "태수" :
            affiliated ? "상신 가능 / 태수·군주 지휘 권한 없음" : "소속 세력·도시 없음";
        bool canSubmit = affiliated && strategy.CurrentDate != null && !strategy.HasSubmittedThisMonth(officer);
        string submission = !affiliated ? "소속 세력과 도시가 있어야 합니다" : strategy.CurrentDate == null
            ? "월 변경·평정 진입으로 날짜 확인 필요" : strategy.HasSubmittedThisMonth(officer)
            ? "이번 달 상신 완료 / 장수당 월 1건" : "일반 상신 또는 특권 1개로 상급자 승인 후 출진";
        string title = $"월간 상신·결재 — {month}";
        string caption = $"{authority} / 특권 {world.GetPlayer().Resources.Privileges}개 / 뒤로: 궁성으로 복귀";
        UiRow[] choices =
            [new(0, inCouncilRoom ? "출진" : "출진 상신", submission, Enabled: canSubmit),
             new(1, "결재", commands ? "지휘 도시의 상신을 승인·보류·거절" : "태수·군주 지휘 권한 필요", Enabled: commands),
             new(2, "기록", affiliated ? "자기 세력의 상신·출진 결과" : "소속 세력 필요", Enabled: affiliated),
             new(3, inCouncilRoom ? "목표" : "도시 목표·위임", commands ? "지휘 도시의 복구·방어·공세 위임" : "태수·군주 지휘 권한 필요", Enabled: commands)];

        if (inCouncilRoom)
            context.ChooseMenu("", "",
                [.. choices, new UiRow(LeaveCouncil, "퇴장", "궁성으로 돌아갑니다")], SelectCommand, dialog =>
                {
                    dialog.HideFormDecoration();
                    const string activeSprite = "000_btn_command_0";
                    const string disabledSprite = "000_btn_command_3";
                    const int activeBackgroundSlot = 3;
                    const int disabledBackgroundSlot = 11;
                    UiSprites.Register(activeSprite, 0x61);
                    UiSprites.Register(disabledSprite, 0x66);
                    // Swords, order, log, check mark, and exit.
                    ReadOnlySpan<int> icons = [0x855, 0x28E, 0xDC, 0x225, 0x7C];
                    for (int index = 0; index < choices.Length; ++index)
                    {
                        UiRow choice = choices[index];
                        int x = 252 + index * 178;
                        dialog.SetButtonBounds(choice.Action, x, 966, 162, 50);
                        dialog.SetButtonIcon(choice.Action, icons[index], x + 37, 974, 34);
                        dialog.SetButtonLabel(choice.Action, x + 76, 976, 25);
                        string sprite = choice.Enabled ? activeSprite : disabledSprite;
                        if (UiSprites.Find(sprite).HasValue)
                            dialog.ApplyRowSprite(choice.Action,
                                choice.Enabled ? activeBackgroundSlot : disabledBackgroundSlot, sprite);
                    }
                    int leaveX = 252 + choices.Length * 178;
                    dialog.SetButtonBounds(LeaveCouncil, leaveX, 966, 162, 50);
                    dialog.SetButtonIcon(LeaveCouncil, icons[^1], leaveX + 37, 974, 34);
                    dialog.SetButtonLabel(LeaveCouncil, leaveX + 76, 976, 25);
                    dialog.ApplyRowSprite(LeaveCouncil, activeBackgroundSlot, activeSprite);
                });
        else
            context.ChooseMenu(title, caption, choices, SelectCommand);

        void SelectCommand(WorldEventContext next, int choice)
        {
            switch (choice)
            {
                case 0: ShowTargets(next); break;
                case 1: ShowProposals(next, decisions: true); break;
                case 2: ShowProposals(next, decisions: false); break;
                case 3: ShowCities(next); break;
                case -1 when inCouncilRoom:
                case LeaveCouncil: next.CloseCouncil(); break;
            }
        }
    }

    private static ushort PlayerId(GameWorld world) =>
        world.GetPlayer().Character.GetComponent<OfficerComponent>()!.Id;

    private static bool Commands(ushort officer, CityAffiliation city) =>
        city.GovernorId == officer || city.LordId == officer;

    private static string CityName(GameWorld world, ushort id) =>
        world.GetCity(id).GetComponent<CityComponent>()!.Name;

    private void ShowTargets(WorldEventContext context)
    {
        var world = context.World;
        ushort officer = PlayerId(world);
        var membership = world.GetPlayer().Character.GetComponent<OfficerAffiliationComponent>()!.Read();
        if (strategy.CurrentDate == null)
        {
            context.Message("출진 상신", "월 변경 또는 평정 진입을 한 번 확인한 뒤 이용할 수 있습니다.");
            return;
        }
        if (membership.CityId is not { } city || membership.ForceId == null)
        {
            context.Message("출진 상신", "세력과 도시에 소속된 장수만 상신할 수 있습니다.");
            return;
        }
        if (strategy.HasSubmittedThisMonth(officer))
        {
            context.Message("출진 상신", "이번 달에는 이미 상신했습니다. 장수당 월 1건입니다.");
            return;
        }

        var targets = world.GetCity(city).GetComponent<CityAiComponent>()!.GetAttackCandidates();
        if (targets.Length == 0)
        {
            context.Message("출진 상신", "게임이 허용한 인접 공격 대상이 없습니다.", (returned, _) => ShowMain(returned));
            return;
        }
        string title = $"{CityName(world, city)} — 출진 제안 대상";
        if (inCouncilRoom)
            context.ChooseCity(title, city,
                targets.Select(target => target.Target.GetComponent<CityComponent>()!.Id).ToArray(), SelectTarget);
        else
            context.Choose(title, targets.Select(target =>
            {
                ushort id = target.Target.GetComponent<CityComponent>()!.Id;
                return new UiRow(id, CityName(world, id), $"방어 추정 {target.EstimatedDefense}");
            }).ToArray(), SelectTarget);

        void SelectTarget(WorldEventContext next, int selectedCity)
        {
            if (selectedCity < 0)
            {
                ShowMain(next);
                return;
            }
            ushort target = checked((ushort)selectedCity);
            string route = $"{CityName(next.World, city)} → {CityName(next.World, target)}";
            var command = next.World.GetCity(city).GetComponent<CityAffiliationComponent>()!.Read();
            ushort? approver = command.GovernorId is { } governor && governor != officer
                ? governor : command.LordId is { } lord && lord != officer ? lord : null;
            byte privileges = next.World.GetPlayer().Resources.Privileges;
            var actions = new List<UiRow>
            {
                new(0, "일반 상신", $"{route} / 결재 대기로 등록")
            };
            bool cityReady = !strategy.HasMarchAttemptThisMonth(city) &&
                !strategy.Proposals.Any(p => p.SourceCityId == city && p.Status == ProposalStatus.Submitted);
            string reason = approver == null ? "직속 태수·군주 결재 대상이 없음" :
                privileges == 0 ? "특권 없음" : !cityReady ? "이번 달 출진 시도 또는 처리 중인 요청 있음" :
                command.GovernorId == approver ? "태수에게 특권 건의 가능" : "군주에게 특권 건의 가능";
            actions.Add(new(1, "특권 건의", $"{reason} / 특권 {privileges}개 보유 / 1개 사용",
                Enabled: privileges > 0 && approver != null && cityReady));
            next.ChooseMenu($"출진 상신 — {route}", "일반 상신은 결재 대기, 특권 건의는 승인 대화 후 출진합니다", actions,
                (confirmed, action) =>
                {
                    if (action < 0)
                    {
                        ShowTargets(confirmed);
                        return;
                    }
                    if (action == 1)
                    {
                        EventEffects.Say(confirmed, officer,
                            "특권을 사용하여 출진을 건의합니다. 출진을 허가해 주십시오.");
                        EventEffects.Say(confirmed, approver!.Value,
                            "그 건의를 받아들이겠다. 출진 준비를 진행하라.");
                        strategy.SubmitPrivilegedMarch(officer, target, approver.Value);
                        if (inCouncilRoom)
                            confirmed.CloseCouncil();
                        return;
                    }
                    strategy.SubmitMarch(officer, target);
                    confirmed.Message("상신 접수", "결재 대기 상태로 등록했습니다.", (returned, _) => ShowMain(returned));
                });
        }
    }

    private void ShowProposals(WorldEventContext context, bool decisions)
    {
        var world = context.World;
        var membership = world.GetPlayer().Character.GetComponent<OfficerAffiliationComponent>()!.Read();
        ushort officer = PlayerId(world);
        strategy.RefreshResults();
        var proposals = strategy.Proposals.Where(proposal =>
        {
            if (membership.ForceId != proposal.ForceId)
                return false;
            if (!decisions)
                return true;
            if (proposal.Status is not (ProposalStatus.Pending or ProposalStatus.Deferred))
                return false;
            var city = world.GetCity(proposal.SourceCityId).GetComponent<CityAffiliationComponent>()!.Read();
            return city.ForceId == proposal.ForceId && Commands(officer, city);
        })
            .Reverse().ToArray();
        if (proposals.Length == 0)
        {
            context.Message(decisions ? "결재" : "기록", "표시할 상신이 없습니다.", (returned, _) => ShowMain(returned));
            return;
        }
        context.Choose(decisions ? "결재 대기" : "상신·출진 기록",
            proposals.Select((proposal, index) => new UiRow(index,
                $"{CityName(world, proposal.SourceCityId)} → {CityName(world, proposal.TargetCityId)}",
                $"{proposal.Date.Year}/{proposal.Date.Month} {Status(proposal)}")).ToArray(),
            (next, index) =>
            {
                if (index < 0)
                {
                    ShowMain(next);
                    return;
                }
                var proposal = proposals[index];
                if (!decisions)
                {
                    next.Message("요청 기록", $"장수 {proposal.OfficerId} / {CityName(next.World, proposal.SourceCityId)} → {CityName(next.World, proposal.TargetCityId)} / " +
                        $"{Status(proposal)} (요청 실행과 전투 승패는 별개)",
                        (returned, _) => ShowProposals(returned, decisions: false));
                    return;
                }

                bool canApprove = !strategy.HasMarchAttemptThisMonth(proposal.SourceCityId) &&
                    !strategy.Proposals.Any(p => p.SourceCityId == proposal.SourceCityId && p.Status == ProposalStatus.Submitted);
                var choices = new List<UiRow>();
                choices.Add(new(0, "승인", canApprove ? "출진 요청 등록 / 다음 활동·달력 경계에서 게임이 실행 여부 판단" :
                    "이번 달 출진 시도 또는 처리 중인 요청 있음", Enabled: canApprove));
                choices.Add(new(1, "보류", "결재 대기로 유지"));
                choices.Add(new(2, "거절", "이번 상신 종료"));
                next.ChooseMenu($"{CityName(next.World, proposal.SourceCityId)} → {CityName(next.World, proposal.TargetCityId)} 결재", "상신 처리 방침을 선택해 주십시오", choices,
                    (confirmed, action) =>
                    {
                        if (action < 0)
                        {
                            ShowProposals(confirmed, decisions: true);
                            return;
                        }
                        strategy.Decide(proposal, PlayerId(confirmed.World), action == 0, defer: action == 1);
                        confirmed.Message("결재 결과", Status(proposal), (returned, _) => ShowProposals(returned, decisions: true));
                    });
            });
    }

    private void ShowCities(WorldEventContext context)
    {
        var world = context.World;
        var force = world.GetPlayer().Character.GetComponent<OfficerAffiliationComponent>()!.Read().ForceId;
        ushort officer = PlayerId(world);
        var cities = new List<ushort>();
        for (ushort id = 1; id <= 51; ++id)
        {
            var city = world.GetCity(id).GetComponent<CityAffiliationComponent>()!.Read();
            if (force != null && city.ForceId == force && Commands(officer, city))
                cities.Add(id);
        }
        if (cities.Count == 0)
        {
            context.Message("도시 목표·위임", "주인공이 태수 또는 군주로 지휘하는 도시가 없습니다.", (returned, _) => ShowMain(returned));
            return;
        }
        context.Choose("도시 목표·위임", cities.Select((id, index) =>
            new UiRow(index, CityName(world, id), Order(strategy.GetCityOrder(id)))).ToArray(),
            (next, index) =>
            {
                if (index < 0)
                {
                    ShowMain(next);
                    return;
                }
                ushort city = cities[index];
                next.ChooseMenu($"{CityName(next.World, city)} 목표", "도시의 목표와 출진 위임 여부를 선택해 주십시오", [new(0, "복구", "추가 자동 출진 중지"),
                    new(1, "방어", "추가 자동 출진 중지"),
                    new(2, "제한 공세 위임", "게임이 허용한 인접 대상 자동 선택 / 도시당 월 1회 시도")],
                    (confirmed, goal) =>
                    {
                        if (goal < 0)
                        {
                            ShowCities(confirmed);
                            return;
                        }
                        var affiliation = confirmed.World.GetCity(city).GetComponent<CityAffiliationComponent>()!.Read();
                        if (!Commands(PlayerId(confirmed.World), affiliation))
                        {
                            confirmed.Message("권한 변경", "현재 이 도시의 지휘 권한이 없습니다.");
                            return;
                        }
                        var order = new CityStrategyOrder((CityStrategyGoal)goal, goal == 2);
                        strategy.SetCityOrder(city, order);
                        confirmed.Message("도시 목표 설정", Order(order), (returned, _) => ShowCities(returned));
                    });
            });
    }

    private static string Order(CityStrategyOrder? order) => order?.Goal switch
    {
        CityStrategyGoal.Rebuild => "복구 / 추가 자동 출진 중지",
        CityStrategyGoal.Defend => "방어 / 추가 자동 출진 중지",
        CityStrategyGoal.LimitedOffensive when order.DelegateMarch => "제한 공세 / 출진 위임",
        _ => "추가 자동 출진 위임 없음"
    };

    private static string Status(MarchProposal proposal) => proposal.Status switch
    {
        ProposalStatus.Pending => "결재 대기",
        ProposalStatus.Deferred => "보류",
        ProposalStatus.Rejected => "거절",
        ProposalStatus.Submitted => "출진 요청 등록 / 결과 회수 대기",
        ProposalStatus.Interrupted => "이전 요청 추적 중단 / 자동 재실행 안 함",
        _ => proposal.Result?.Status switch
        {
            MarchRequestStatus.Invoked => "게임 출진 경로 실행 완료",
            MarchRequestStatus.RejectedByGame => "게임이 출진을 거절함",
            MarchRequestStatus.OwnershipChanged => "도시 소유권 변경으로 취소",
            MarchRequestStatus.TargetNoLongerEligible => "공격 대상 자격 변경으로 취소",
            MarchRequestStatus.NoPrivilege => "실행 전 특권 부족으로 취소",
            MarchRequestStatus.ApprovalAuthorityChanged => "주인공·소속 도시·결재권자 변경으로 취소",
            _ => $"요청 결과 회수: {proposal.Result?.Status}"
        }
    };
}
