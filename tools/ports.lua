-- Print the idle values of the input ports exactly as MAME sees them.
local p = manager.machine.ioport.ports
emu.wait(2)
for _, tag in ipairs({":SYSTEM", ":P1", ":P2", ":DSW1", ":DSW2", ":DSW3"}) do
  local port = p[tag]
  if port then
    print(string.format("%-8s = %02X", tag, port:read() & 0xFF))
    for name, f in pairs(port.fields) do
      print(string.format("     mask %02X  default %02X  %s", f.mask, f.defvalue, name))
    end
  else
    print(tag .. " not present")
  end
end
