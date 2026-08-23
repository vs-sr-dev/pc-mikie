-- Coverage-only trace (no registers): maps which addresses are really executed
-- CODE. Much lighter than the full trace.
-- Usage: mame mikie -debug -debugger none -autoboot_delay 0 -autoboot_script tools/trace_cov.lua
local OUT  = os.getenv("MIKIE_TRACE_OUT") or "trace/cov.log"
local SECS = tonumber(os.getenv("MIKIE_COV_SECS") or "40")

local dbg = manager.machine.debugger
dbg:command("trace " .. OUT .. ",maincpu,noloop")
dbg:command("go")
emu.wait(SECS)
dbg:command("trace off")
print("COV DONE " .. SECS .. "s")
manager.machine:exit()
