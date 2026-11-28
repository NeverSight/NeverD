local trace = ""
local function record(left, right)
  if type(left) == "number" then
    trace = trace .. "L"
    return left
  end
  trace = trace .. "R"
  return right
end
local value = setmetatable({}, {__add = record})
local a = 7 + value
local b = value + 9
local receiver = setmetatable({}, {
  __index = function(obj, key)
    trace = trace .. key
    return function(self, n)
      return obj == self, (-2) ^ n
    end
  end
})
local same, power = receiver:probe(4)
return trace, a, b, same, power
