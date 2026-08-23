-- Snapshot at precise frame numbers, so the recompiled renderer can be
-- compared against MAME frame-for-frame rather than "at about the same time".
-- MIKIE_SNAP_FRAMES  comma-separated list of frame indices
local list = os.getenv("MIKIE_SNAP_FRAMES") or "200,720"
local want = {}
for n in string.gmatch(list, "[^,]+") do want[#want+1] = tonumber(n) end
table.sort(want)

local scr = manager.machine.screens[":screen"]
for _, n in ipairs(want) do
  while scr:frame_number() < n do emu.wait_next_frame() end
  manager.machine.video:snapshot()
  print("SNAP frame " .. scr:frame_number())
end
