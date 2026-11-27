local count = 0
local function increment()
  count = count + 1
end
do
  local item <close> = setmetatable({}, {__close = increment})
end
return count
