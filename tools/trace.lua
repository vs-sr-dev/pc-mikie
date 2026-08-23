-- Reference MC6809E trace: registers + cycle counter.
-- This is the oracle the recompiled code is verified against (tools/difftrace.py).
--
-- `noloop` is MANDATORY: without it MAME collapses repetitions into
-- "(loops for N instructions)" and the log is useless as a reference.
--
-- MIKIE_TRACE_SKIP  emulated seconds to run before tracing starts
-- MIKIE_TRACE_SECS  emulated seconds to trace
--
-- Do NOT call manager.machine:exit() here: exiting straight after "trace off"
-- can hang MAME with the trace file still buffered, silently truncating it.
-- Let -seconds_to_run shut the machine down so the file is flushed and closed.
local OUT  = os.getenv("MIKIE_TRACE_OUT") or "trace/maincpu.log"
local SKIP = tonumber(os.getenv("MIKIE_TRACE_SKIP") or "0")
local SECS = tonumber(os.getenv("MIKIE_TRACE_SECS") or "3")

-- NB: '\\n' so Lua emits a literal backslash-n; MAME's tracelog parses it itself.
local fmt = 'A=%02X B=%02X X=%04X Y=%04X U=%04X S=%04X DP=%02X CC=%02X CYC=%d\\n'
local dbg = manager.machine.debugger
dbg:command("go")
if SKIP > 0 then emu.wait(SKIP) end
dbg:command('trace ' .. OUT .. ',maincpu,noloop,{tracelog "' .. fmt ..
            '",a,b,x,y,u,s,dp,cc,totalcycles}')
emu.wait(SECS)
dbg:command("trace off")
print("TRACE DONE skip=" .. SKIP .. " secs=" .. SECS)
