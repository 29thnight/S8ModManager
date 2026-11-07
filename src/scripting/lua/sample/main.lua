-- Lua 예제 모드. 게임 안 편집 메뉴에 항목 둘을 붙이고, 달이 바뀔 때 기록을 남긴다.

menu.add("in_game_edit", "금 1000 받기", function()
  local me = game.player
  me.gold = me.gold + 1000
  log("금 지급: 무장 " .. me.id .. ", 지금 " .. me.gold)
end)

menu.add("in_game_edit", "능력치 만점", function()
  local me = game.player
  for _, stat in ipairs({ "leadership", "strength", "intelligence", "politics", "charm" }) do
    me[stat] = 100
  end
end)

events.on("month_changed", function(date)
  log(date.year .. "년 " .. date.month .. "월")
end)

-- 기재 254번을 가진 무장의 부대는 턴이 끝날 때 병력 100을 회복한다.
battle.on_turn_end(254, function(trait)
  local healed = trait.unit:restore_soldiers(100)
  if healed > 0 then
    trait:show("물러서지 마라!")
  end
end)
