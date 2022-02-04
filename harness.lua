-- Call like this:
-- lua harness.lua gigi.lua "size=1024,1024&ID=0230k123101310331&RAW=1&bbox=0,0,4096,4096"
dofile(arg[1])
print(query_handler(arg[2]))
