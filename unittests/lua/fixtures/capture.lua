local function accumulate(start, ...)
  local function add(step)
    start = start + step
    return start
  end
  local first = add(2)
  return first, add(3), ...
end
return accumulate(40, nil, "kept", nil)
