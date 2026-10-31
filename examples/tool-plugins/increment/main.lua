return function(input, context)
  local args = assert(input:decode())
  return {value = args.value + 1}
end
