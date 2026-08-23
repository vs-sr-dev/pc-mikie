-- Trace with the cycle counter: used to derive the 6809 cycle table and the
-- exact vblank position EMPIRICALLY rather than by transcription.
local OUT  = os.getenv("MIKIE_TRACE_OUT") or "trace/cyc.log"
local SECS = tonumber(os.getenv("MIKIE_CYC_SECS") or "0.3")
local dbg = manager.machine.debugger
dbg:command('trace ' .. OUT .. ',maincpu,noloop,{tracelog "CYC=%d\n",totalcycles}')
dbg:command("go")
emu.wait(SECS)
dbg:command("trace off")
print("CYC DONE")
manager.machine:exit()
