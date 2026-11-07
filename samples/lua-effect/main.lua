-- Lua 기재 효과 예제.
-- 효과는 한 번 정의하고, 어느 기재의 몇 번째 칸에 얼마의 값으로 넣을지는 기재 편집기에서 고른다.
-- description 의 {value} 는 칸의 값으로 바뀌어 게임의 기재 설명에 붙는다.
-- 돌려준 문자열은 부대 대사로 뜬다.
effect "부상병 회복" {
  description = "턴이 끝날 때 부상병의 {value}%를 회복한다, 단 최대 3000",

  turn_end = function(unit, value)
    if unit:heal(unit.wounded * value / 100, 3000) > 0 then
      return "부상병을 치료하라!"
    end
  end,
}
