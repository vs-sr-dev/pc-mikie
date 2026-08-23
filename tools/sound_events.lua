-- Log everything the main CPU tells the sound board, stamped with the 6809
-- cycle it happened at, plus every read of the command latch by the Z80.
--
-- MAME has no Lua binding for total_cycles(), but machine.time is the local
-- time of whichever device is executing, and a CPU's local time advances in
-- whole cycles of attoseconds_per_cycle - so the cycle count comes back exact.
--
-- MIKIE_EV_OUT   output file
-- MIKIE_EV_SECS  emulated seconds to watch
local OUT  = os.getenv("MIKIE_EV_OUT")  or "trace/sound_events.log"
local SECS = tonumber(os.getenv("MIKIE_EV_SECS") or "40")

local ATTO_PER_6809 = 651041666666      -- 1e18 // 1536000, as MAME truncates it
local ATTO_PER_Z80  = 279365114         -- 1e18 // 3579545

local f = assert(io.open(OUT, "w"))
local main = manager.machine.devices[":maincpu"].spaces["program"]
local snd  = manager.machine.devices[":audiocpu"].spaces["program"]

local function cyc(per)
  local t = manager.machine.time
  -- a double keeps ~16 digits, so the rounding error here is a billionth of
  -- a cycle even after a minute of emulated time
  return math.floor((t.seconds * 1e18 + t.attoseconds) / per + 0.5)
end

local taps = {}
taps[#taps+1] = main:install_write_tap(0x2000, 0x2007, "ls259", function (off, data)
  f:write(string.format("LS259 %d %d %d\n", cyc(ATTO_PER_6809), off & 7, data & 1))
end)
taps[#taps+1] = main:install_write_tap(0x2400, 0x2400, "latch", function (off, data)
  f:write(string.format("LATCH %d %02X\n", cyc(ATTO_PER_6809), data))
end)
taps[#taps+1] = snd:install_read_tap(0x8003, 0x8003, "latchr", function (off, data)
  f:write(string.format("READ %d %02X\n", cyc(ATTO_PER_Z80), data))
end)

emu.wait(SECS)
f:close()
print("SOUND EVENTS DONE -> " .. OUT)
