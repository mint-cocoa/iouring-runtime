local paths = {
  "/",
  "/health",
  "/",
  "/",
  "/health",
}

local index = 0

request = function()
  index = index + 1
  local path = paths[((index - 1) % #paths) + 1]
  return wrk.format(nil, path)
end
