-- Dump the video-related RAM at an exact frame number, so a rendering
-- difference can be attributed to the renderer or to the data it was given.
-- MIKIE_RAM_FRAME  frame index
-- MIKIE_RAM_OUT    output file ($2800-$3FFF, 6144 bytes)
local N   = tonumber(os.getenv("MIKIE_RAM_FRAME") or "1818")
local OUT = os.getenv("MIKIE_RAM_OUT") or "trace/mame_ram.bin"
local scr = manager.machine.screens[":screen"]
local mem = manager.machine.devices[":maincpu"].spaces["program"]

while scr:frame_number() < N do emu.wait_next_frame() end
local f = assert(io.open(OUT, "wb"))
for a = 0x2800, 0x3FFF do f:write(string.char(mem:read_u8(a))) end
f:close()
print("RAM dumped at frame " .. scr:frame_number() .. " -> " .. OUT)
