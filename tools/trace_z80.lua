-- Reference Z80 (sound CPU) trace: registers + cycle counter.
--
-- Same rules as tools/trace.lua: `noloop` is mandatory, and never call
-- manager.machine:exit() (use -seconds_to_run so the file is flushed).
--
-- The one extra trick: `tracelog` resolves its symbols against, and writes to,
-- the debugger's *visible* CPU - which is the 6809 by default. Without setting
-- visible_cpu the action is dropped silently and the log holds nothing but
-- disassembly. `focus` would do it too, but it also stops every other CPU.
--
-- MIKIE_TRACE_SKIP  emulated seconds to run before tracing starts
-- MIKIE_TRACE_SECS  emulated seconds to trace
local OUT  = os.getenv("MIKIE_Z80_OUT")  or "trace/audiocpu.log"
local SKIP = tonumber(os.getenv("MIKIE_TRACE_SKIP") or "0")
local SECS = tonumber(os.getenv("MIKIE_TRACE_SECS") or "3")

local fmt = 'AF=%04X BC=%04X DE=%04X HL=%04X IX=%04X IY=%04X SP=%04X ' ..
            'AF2=%04X BC2=%04X DE2=%04X HL2=%04X I=%02X R=%02X ' ..
            'IM=%X IFF1=%X HALT=%X CYC=%d\n'
local dbg = manager.machine.debugger
dbg.visible_cpu = manager.machine.devices[":audiocpu"]
dbg:command("go")
if SKIP > 0 then emu.wait(SKIP) end
dbg.visible_cpu = manager.machine.devices[":audiocpu"]
dbg:command('trace ' .. OUT .. ',audiocpu,noloop,{tracelog "' .. fmt ..
            '",af,bc,de,hl,ix,iy,sp,af2,bc2,de2,hl2,i,r,im,iff1,halt,totalcycles}')
emu.wait(SECS)
dbg:command("trace off")
print("Z80 TRACE DONE skip=" .. SKIP .. " secs=" .. SECS)
