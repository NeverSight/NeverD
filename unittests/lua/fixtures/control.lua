local function fold(limit, enabled)
  local total = 0
  for index = 1, limit do
    if enabled and index % 2 == 0 then
      total = total + index
    end
  end
  return total
end
return fold(6, true), fold(6, false)
