-- v9k.lua - MAME autoboot script that lets v9kmame.py drive a victor9k.
--
-- Protocol: the driver writes "<dir>/cmd.<n>" (one command per file, n = 1,
-- 2, ...); this script runs it and answers in "<dir>/ack.<n>". <dir> comes
-- from the V9K_CTL environment variable. Commands:
--   post <text>          type text through the natural keyboard (\r = Return)
--   press <name> [n]     hold the key whose field name is <name> for n frames
--                        (default 4); names are listed in <dir>/keys.txt
--   kbidle               "1" once queued typing and key presses are done
--   screen               the 25x80 text screen, decoded from VRAM at F000:0
--   snap                 save a PNG snapshot (driver picks up the newest)
--   time                 emulated seconds
--   field <port> <name> <value>   set an ioport field (e.g. serial baud)
--   exit                 quit MAME

local dir = os.getenv("V9K_CTL") or "."
local seq = 1
local presses = {}          -- {field=, left=frames}
local fields = {}           -- key name -> field
local frame = 0

local function write(path, text)
  local f = io.open(path, "w")
  if f then f:write(text) f:close() end
end

local function index_keys()
  local out = {}
  for tag, port in pairs(manager.machine.ioport.ports) do
    for name, field in pairs(port.fields) do
      if field.type_class == "keyboard" then
        fields[name] = field
        out[#out + 1] = name .. "\t" .. tag
      end
    end
  end
  table.sort(out)
  write(dir .. "/keys.txt", table.concat(out, "\n") .. "\n")
end

-- Display start (CRTC R12:R13) from the MC6845's save state; DOS scrolls by
-- moving it, igc resets it to 0. Text RAM wraps at 2048 words.
local function disp_start()
  local crtc = manager.machine.devices[":11a"]
  return emu.item(crtc.items["0/m_disp_start_addr"]):read(0) & 0x7ff
end

local function screen_text()
  local mem = manager.machine.devices[":8l"].spaces["program"]
  local start = disp_start()
  local rows = {}
  for y = 0, 24 do
    local chars = {}
    for x = 0, 79 do
      local glyph = mem:read_u16(0xF0000 + 2 * ((start + y * 80 + x) & 0x7ff)) & 0x07FF
      local c = glyph - 0x64                    -- igc's CHAR_BASE
      if c >= 32 and c < 127 then
        chars[#chars + 1] = string.char(c)
      elseif c == 0 or glyph == 0 then
        chars[#chars + 1] = " "
      else
        chars[#chars + 1] = "#"                 -- box drawing etc.
      end
    end
    rows[#rows + 1] = table.concat(chars)
  end
  return table.concat(rows, "\n") .. "\n"
end

local function run(line)
  local verb, rest = line:match("^(%S+)%s?(.*)$")
  if verb == "post" then
    manager.machine.natkeyboard:post((rest:gsub("\\r", "\r")))
    return "ok"
  elseif verb == "press" then
    local name, n = rest:match("^(.-)%s+(%d+)$")
    if not name then name, n = rest, "4" end
    local field = fields[name]
    if not field then return "error: no key " .. name end
    presses[#presses + 1] = { field = field, left = tonumber(n), down = false }
    return "ok"
  elseif verb == "chord" then
    -- chord <mod>|<key> [n]: hold mod and key together for n frames
    local mod, key, n = rest:match("^(.-)|(.-)%s+(%d+)$")
    if not mod then mod, key = rest:match("^(.-)|(.-)$") n = "4" end
    local f1, f2 = fields[mod], fields[key]
    if not (f1 and f2) then return "error: no key " .. rest end
    presses[#presses + 1] = { field = f1, field2 = f2, left = tonumber(n), down = false }
    return "ok"
  elseif verb == "kbidle" then
    return (manager.machine.natkeyboard.empty and #presses == 0) and "1" or "0"
  elseif verb == "screen" then
    return screen_text()
  elseif verb == "snap" then
    manager.machine.video:snapshot()
    return "ok"
  elseif verb == "time" then
    return string.format("%.3f", manager.machine.time:as_double())
  elseif verb == "field" then
    local tag, name, value = rest:match("^(%S+)%s+(.-)%s+(%d+)$")
    local port = tag and manager.machine.ioport.ports[tag]
    local field = port and port.fields[name]
    if not field then return "error: no field " .. rest end
    field.user_value = tonumber(value)
    return "ok"
  elseif verb == "exit" then
    manager.machine:exit()
    return "ok"
  end
  return "error: unknown command " .. tostring(verb)
end

emu.register_frame_done(function()
  frame = frame + 1
  if frame == 1 then
    index_keys()
    write(dir .. "/ready", "1")
  end

  -- One key press at a time: hold it, then release it for as many frames
  if #presses > 0 then
    local p = presses[1]
    if not p.down then
      p.field:set_value(1)
      if p.field2 then p.field2:set_value(1) end
      p.down = true
    end
    p.left = p.left - 1
    if p.left <= 0 then
      p.field:clear_value()
      if p.field2 then p.field2:clear_value() end
      if p.gap then
        table.remove(presses, 1)
      else
        p.gap = true
        p.left = 4
        p.down = true
      end
    end
  end

  if frame % 2 ~= 0 then return end
  local path = dir .. "/cmd." .. seq
  local f = io.open(path, "r")
  if not f then return end
  local line = f:read("l") or ""
  f:close()
  os.remove(path)
  write(dir .. "/ack." .. seq .. ".tmp", run(line))
  os.rename(dir .. "/ack." .. seq .. ".tmp", dir .. "/ack." .. seq)
  seq = seq + 1
end)
