-- Take a series of snapshots at fixed times, then close MAME.
local shots = { 3, 8, 14, 22, 30 }   -- seconds of emulated time
local i = 1
local function tick()
  if i > #shots then manager.machine:exit() return end
  emu.wait(shots[i] - (i > 1 and shots[i-1] or 0))
  manager.machine.video:snapshot()
  print("SNAP " .. shots[i] .. "s")
  i = i + 1
  tick()
end
tick()
