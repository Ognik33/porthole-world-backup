-- Поддельный porthole для Luau-тестов. Записывает всё, что дополнение делает, в H.*
-- Поведение взято из описи API лабы (Porthole 3.0.2); то, что лаба не проверила, настраивается:
--   H.open_mode = "plain"   объект tcp_open с функциями send(text) / close() (так в Porthole 3.0.2)
--               | "throws"  сам tcp_open бросает ошибку
--               | "send_fails" объект есть, но send бросает ошибку
--   porthole.tcp_open = nil  функции нет вовсе
H = {
	logs = {}, notes = {}, helpers = {}, tabs = {}, trees = {}, renders = 0, max_text = 0,
	tcp = {},           -- строки, отправленные через tcp_send
	open_calls = {},    -- порты, к которым звали tcp_open
	open_sent = {},     -- строки, отправленные через объект tcp_open
	open_closed = 0,
	open_mode = "plain",
	role_cb = nil, end_cb = nil, tick_cb = nil,
}
local role = ""
function set_role(r) role = r end

-- Поддельные часы: дополнение считает сроки по os.clock(), а таблица os в Luau только для чтения,
-- поэтому подменяем глобальную os целиком. Каждый тик сдвигает часы на 0,5 с, как в Porthole.
NOW = 1000
local real_os = os
os = setmetatable({ clock = function() return NOW end }, { __index = real_os })

local function fmt(s, vars)
	return (string.gsub(s, "{(%w+)}", function(k) return tostring((vars or {})[k] or "{" .. k .. "}") end))
end

-- Porthole считает текст дерева; здесь считаем строки массива и подписи, как заметно по ошибке
-- «a tree carries at most 4096 bytes of text». Дополнение считает с бо́льшим запасом.
local function text_bytes(x)
	if type(x) ~= "table" then return 0 end
	local n = 0
	for _, v in ipairs(x) do
		if type(v) == "string" then n += #v elseif type(v) == "table" then n += text_bytes(v) end
	end
	if type(x.label) == "string" then n += #x.label end
	return n
end

porthole = {
	t = function(key, vars)
		local s = STRINGS[key]
		assert(s, "missing translation key: " .. tostring(key))
		return fmt(s, vars)
	end,
	log = function(s) table.insert(H.logs, s) end,
	notify = function(level, s) table.insert(H.notes, level .. ": " .. s) end,
	role = function() return role end,
	free_port = function() return 41000 + #H.helpers end,
	random_token = function(n) return string.rep("a", n or 16) .. tostring(#H.helpers) end,
	on_role_change = function(f) H.role_cb = f end,
	on_session_end = function(f) H.end_cb = f end,
	on_tick = function(f) H.tick_cb = f end,
	tcp_send = function(port, text)
		assert(type(port) == "number", "tcp_send: port must be a number")
		assert(type(text) == "string" and utf8.len(text), "tcp_send: text must be UTF-8")
		table.insert(H.tcp, text)
	end,
	tcp_open = function(port)
		assert(type(port) == "number", "tcp_open: port must be a number")
		table.insert(H.open_calls, port)
		local mode = H.open_mode
		if mode == "throws" then error("tcp_open: connection refused") end
		local h = {}
		h.send = function(text)
			assert(type(text) == "string", "error converting Lua table to String")
			if H.open_mode == "send_fails" then error("connection reset") end
			table.insert(H.open_sent, text)
		end
		h.close = function() H.open_closed += 1 end
		return h
	end,
	run_helper = function(spec)
		local h = { spec = spec, stopped = false }
		function h.stop() h.stopped = true end
		table.insert(H.helpers, h)
		return h
	end,
	ui = {
		add_tab = function(o)
			assert(o.panel == "sharing" and type(o.title) == "string", "bad add_tab")
			table.insert(H.tabs, o.title)
			return #H.tabs
		end,
		render = function(tab, tree)
			local n = text_bytes(tree)
			H.max_text = math.max(H.max_text, n)
			if n > 4096 then error("a tree carries at most 4096 bytes of text") end
			H.trees[tab] = tree
			H.renders += 1
		end,
	},
}

-- ---------- помощники сценариев ----------
function helper() return H.helpers[#H.helpers] end
function agent_say(line) helper().spec.on_output(line) end
function helper_exit(code) helper().spec.on_exit(code or 1, "test") end
function ticks(n)
	for _ = 1, n do
		NOW += 0.5
		H.tick_cb()
	end
end
-- сдвинуть часы без тиков (для юнит-тестов агента, которые зовут a:tick() сами)
function advance(sec) NOW += sec end

-- имя команды из строки "<token>\t<cmd>\t...\n"
function cmd_of(text) return text and string.match(text, "^[^\t]+\t([^\t\n]+)") end
function last_open_cmd() return cmd_of(H.open_sent[#H.open_sent]) end
function last_tcp_cmd() return cmd_of(H.tcp[#H.tcp]) end

-- запустить хелпер в сессии и довести до строки ready
function bring_up(caps)
	agent_say("hello\tproto=1\tversion=0.0.1\tcaps=" .. (caps or "persist"))
	agent_say("ready\tport=" .. helper().spec.args[2])
end

function tree_text()
	local out = {}
	local function walk(n)
		if type(n) ~= "table" then return end
		for _, v in ipairs(n) do
			if type(v) == "string" then out[#out + 1] = v elseif type(v) == "table" then walk(v) end
		end
		if n.label then out[#out + 1] = n.label end
	end
	walk(H.trees[1])
	return table.concat(out, " | ")
end
function tab_has(s) return string.find(tree_text(), s, 1, true) ~= nil end
function log_has(s)
	for _, l in ipairs(H.logs) do if string.find(l, s, 1, true) then return true end end
	return false
end

local passed = 0
function check(name, cond)
	if not cond then error("FAIL " .. name, 2) end
	passed += 1
	print("PASS " .. name)
end
function done() print("OK " .. passed .. " checks") end
