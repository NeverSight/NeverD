local function nextValue(limit, previous)
  if previous < limit then
    return previous + 1, previous
  end
end
local sum = 0
for key, value in nextValue, 3, 0 do
  sum = sum + key + value
end
return sum
