-- agent: нативный хелпер world_backup и связь с ним.
--
-- Основа — agent.luau из game-status 0.2.3 и надзор из лабы (lab.luau). Нового здесь три вещи:
--   1. Канал команд. Сначала постоянное соединение porthole.tcp_open (проверено в Porthole 3.0.2);
--      если его нет, оно бросает ошибку или по нему не пришёл pong, — откат на porthole.tcp_send.
--      Откат запоминается до перезапуска Porthole, чтобы не терять 5 с на каждом старте хелпера.
--   2. Хелпер считается готовым только после первого pong: значит, команды реально доходят.
--   3. Надзор внутри модуля: ping раз в ~10 с, нет pong за ~5 с — перезапуск с нарастающей паузой.
--
-- События хелпера всегда приходят строками stdout через run_helper.on_output, канал влияет только
-- на отправку команд. Хелпер запускается только внутри сессии: при входе в лобби Porthole
-- перезапускает хелперы (проверено в лабе), поэтому start() вызывается из session().
local util = require("./util")

local agent = {}
agent.__index = agent

local PROTO = 1
-- Все сроки в секундах по os.clock(), а не в тиках: в логе 0.0.2 Porthole при загрузке дополнения
-- вызвал on_tick 20 раз за 27 мс, и счёт по тикам сбросил только что запущенный хелпер.
-- os.clock() в Luau — монотонный счётчик секунд (не время суток), для сроков он и нужен.
local BACKOFF = { 5, 10, 20, 60 }   -- пауза перед перезапуском после 1-й, 2-й, 3-й, 4-й+ неудачи
local STABLE = 60                   -- после минуты нормальной работы счётчик неудач сбрасывается
local PING_EVERY = 10
local PONG_WAIT = 5
local READY_WAIT = 10               -- от запуска процесса до строки ready

local function now() return os.clock() end

-- Состояния (agent.state):
--   off        вне сессии, хелпер не нужен
--   starting   процесс запущен, ждём hello/ready
--   linking    хелпер слушает порт, проверяем канал: ждём pong
--   ready      pong пришёл, команды доходят
--   restarting хелпер упал или завис, ждём паузу перед перезапуском
--   fatal      запустить не получится (нет free_port, run_helper отказал, чужая версия протокола)

local function make_token(P)
	local ok, t = pcall(P.random_token, 24)
	if ok and type(t) == "string" and #t >= 16 and #t <= 64 and string.match(t, "^[%w]+$") then return t end
	local parts = {}
	for _ = 1, 32 do parts[#parts + 1] = string.format("%x", math.random(0, 15)) end
	return table.concat(parts)
end

-- opts: native (имя DLL без расширения), on_event(kind, fields, line), on_change() — состояние изменилось
function agent.new(P, opts)
	local self = setmetatable({}, agent)
	self.P = P
	self.opts = opts
	self.state = "off"
	self.channel = nil       -- "tcp_open" | "tcp_send", пока есть хелпер
	self.open_broken = false -- tcp_open не подошёл: до перезапуска Porthole сразу tcp_send
	self.conn = nil          -- объект tcp_open
	self.helper = nil
	self.port = nil
	self.token = nil
	self.caps = {}
	self.version = nil
	self.fatal_reason = nil
	self.wanted = false
	self.fails = 0
	self.restart_at = nil    -- когда перезапустить хелпер (os.clock)
	self.ready_at = nil      -- когда хелпер стал готов
	self.started_at = nil
	self.ping_at = nil
	self.last_pong = nil
	return self
end

function agent:log(s) self.P.log("world-backup: " .. s) end

function agent:_set(state)
	if self.state == state then return end
	self.state = state
	if self.opts.on_change then self.opts.on_change() end
end

function agent:_fatal(reason)
	self.fatal_reason = util.first_line(reason)
	self:log("agent fatal reason=" .. self.fatal_reason)
	self:_set("fatal")
end

-- ---------- tcp_open ----------

-- Вызвать функцию объекта tcp_open: conn.send(text), conn.close().
-- Способ вызова проверен в Porthole 3.0.2 (0.0.1, лог: call=plain): это функции в таблице, не методы.
function agent:_conn_call(name, ...)
	local conn = self.conn
	local okf, f = pcall(function() return conn[name] end)
	if not okf or type(f) ~= "function" then return false, "no function " .. name end
	return pcall(f, ...)
end

function agent:_close_conn()
	if self.conn then self:_conn_call("close") end
	self.conn = nil
end

-- tcp_open не подошёл: закрыть, запомнить, дальше tcp_send
function agent:_open_failed(reason)
	self:log("agent tcp_open unusable reason=" .. util.first_line(reason) .. ", switching to tcp_send")
	self:_close_conn()
	self.open_broken = true
	self.channel = "tcp_send"
	if self.opts.on_change then self.opts.on_change() end
end

-- ---------- отправка ----------

-- отправить готовую строку текущим каналом; при сбое tcp_open — повтор через tcp_send
function agent:_write(data)
	if self.channel == "tcp_open" then
		local ok, err = self:_conn_call("send", data)
		if ok then return true end
		self:_open_failed("send: " .. tostring(err))
	end
	local ok, err = pcall(self.P.tcp_send, self.port, data)
	if not ok then self:log("agent tcp_send failed error=" .. util.first_line(err)) end
	return ok
end

local function line_of(token, ...)
	local parts = { token }
	for i = 1, select("#", ...) do
		parts[#parts + 1] = (string.gsub(tostring((select(i, ...))), "[\t\r\n]", " "))
	end
	return table.concat(parts, "\t") .. "\n"
end

function agent:_ping()
	self.ping_at = now()
	self:_write(line_of(self.token, "ping"))
end

-- Отправить команду хелперу. false, если хелпер не готов или отправка не удалась.
-- Каждая команда, кроме ping, пишется в лог (урок лабы: так видно, дошла ли она до Porthole).
function agent:send(cmd, ...)
	if self.state ~= "ready" then
		self:log("agent send skipped (state=" .. self.state .. ") cmd=" .. tostring(cmd))
		return false
	end
	local ok = self:_write(line_of(self.token, cmd, ...))
	self:log("agent send cmd=" .. tostring(cmd) .. " channel=" .. tostring(self.channel) .. " ok=" .. tostring(ok))
	return ok
end

function agent:has(cap) return self.caps[cap] == true end

-- ---------- жизненный цикл ----------

function agent:start()
	self.wanted = true
	if self.helper or self.state == "fatal" then return end
	local P = self.P
	local okp, port = pcall(P.free_port)
	port = okp and util.int(port, 1024, 65535) or nil
	if not port then return self:_fatal("free_port") end
	self.port = port
	self.token = make_token(P)
	self.caps, self.version, self.channel = {}, nil, nil
	self.bind_retry = false
	self.ping_at, self.last_pong, self.ready_at = nil, nil, nil
	local current
	local ok, h = pcall(P.run_helper, {
		native = self.opts.native,
		args = { "--agent", tostring(port), self.token },
		elevate = false,
		on_output = function(line)
			if self.helper ~= current then return end
			self:_on_line(line)
		end,
		on_exit = function(code, note)
			if self.helper ~= current then return end
			self:log("agent exited code=" .. tostring(code) .. " note=" .. util.first_line(note))
			self.helper = nil
			self:_close_conn()
			self:_schedule_restart()
		end,
	})
	if not ok or not h then return self:_fatal(h or "run_helper") end
	self.helper = h
	current = h
	self.started_at = now()
	self:log("agent start port=" .. port)
	self:_set("starting")
end

function agent:_schedule_restart()
	if self.wanted and self.state ~= "fatal" then
		if self.bind_retry then
			-- порт заняли между free_port и запуском: это не сбой хелпера, сразу новый порт
			self.bind_retry = false
			self.restart_at = now()
		else
			self.fails += 1
			self.restart_at = now() + BACKOFF[math.min(self.fails, #BACKOFF)]
		end
		self:_set("restarting")
	elseif self.state ~= "fatal" then
		self:_set("off")
	end
end

-- остановить процесс хелпера (без планирования перезапуска)
function agent:_kill()
	local h = self.helper
	self.helper = nil
	self:_close_conn()
	self.channel = nil
	self.ping_at = nil
	if h then pcall(function() h:stop() end) end
end

function agent:stop()
	if self.helper then self:log("agent stop port=" .. tostring(self.port)) end
	self.wanted = false
	self.restart_at = nil
	self:_kill()
	if self.state ~= "fatal" then self:_set("off") end
end

-- хелпер завис: убить и перезапустить после паузы
function agent:_drop(reason)
	self:log("agent dropped reason=" .. reason)
	self:_kill()
	self:_schedule_restart()
end

-- хелпер слушает порт: выбрать канал и проверить его ping-ом
function agent:_link()
	local P = self.P
	self.channel = "tcp_send"
	if not self.open_broken and self:has("persist") then
		if type(P.tcp_open) ~= "function" then
			self.open_broken = true
			self:log("agent tcp_open not available, using tcp_send")
		else
			local ok, conn = pcall(P.tcp_open, self.port)
			if ok and conn ~= nil then
				self.conn = conn
				self.channel = "tcp_open"
			else
				self.open_broken = true
				self:log("agent tcp_open failed error=" .. util.first_line(conn) .. ", using tcp_send")
			end
		end
	end
	self:_set("linking")
	self:_ping()
end

function agent:_on_line(line)
	local kind, f = util.parse_fields(line)
	if kind == "hello" then
		if tonumber(f.proto) ~= PROTO then
			self:stop()
			return self:_fatal("proto " .. tostring(f.proto))
		end
		self.version = util.clean(f.version, 32)
		self.caps = {}
		for c in string.gmatch(f.caps or "", "[^,]+") do self.caps[c] = true end
	elseif kind == "ready" then
		self:_link()
	elseif kind == "pong" then
		self.ping_at, self.last_pong = nil, now()
		if self.state == "linking" then
			self:log("agent ready version=" .. tostring(self.version) .. " port=" .. tostring(self.port)
				.. " channel=" .. tostring(self.channel))
			self.ready_at = now()
			self:_set("ready")
		end
	elseif kind == "error" and f.reason == "bind" then
		-- порт успели занять между free_port и запуском; хелпер сейчас выйдет, on_exit возьмёт новый порт
		self.bind_retry = true
	else
		if kind == "error" then
			self:log("agent error reason=" .. tostring(f.reason) .. " detail=" .. tostring(f.detail))
		end
		if self.opts.on_event then self.opts.on_event(kind, f, line) end
	end
end

-- вызывать из on_tick; сроки считаются по часам, так что частота тиков не важна
function agent:tick()
	local t = now()
	if self.restart_at then
		if t >= self.restart_at then
			self.restart_at = nil
			if self.wanted and not self.helper then self:start() end
		end
		return
	end
	if not self.helper then return end
	if self.state == "starting" and t - (self.started_at or t) >= READY_WAIT then
		self:_drop("no_ready")
		return
	end
	if self.state == "ready" and self.fails > 0 and self.ready_at and t - self.ready_at >= STABLE then
		self.fails = 0
	end
	if self.ping_at and t - self.ping_at >= PONG_WAIT then
		if self.state == "linking" and self.channel == "tcp_open" then
			self:_open_failed("no pong")
			self:_ping()
		else
			self:_drop("no_pong")
		end
		return
	end
	if self.state == "ready" and not self.ping_at and t - (self.last_pong or t) >= PING_EVERY then
		self:_ping()
	end
end

-- событие сессии: role ~= "" — мы в лобби (хост или участник), "" — сессии нет
function agent:session(role)
	self:stop()
	if type(role) == "string" and role ~= "" then
		if self.state == "fatal" then self.state = "off" end -- новая сессия — новая попытка
		self.fatal_reason = nil
		self.fails = 0
		self:start()
	end
end

return agent
