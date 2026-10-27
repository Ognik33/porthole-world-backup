-- Юнит-тесты модулей (бандл в режиме --lib даёт __req)
local util = __req("util")
local tree = __req("tree")
local agentmod = __req("agent")

-- ---------- util ----------
check("clean strips control chars", util.clean("a\tb\1c", 10) == "abc")
check("clean cuts on char boundary", util.clean("Привет", 5) == "Пр")
check("clean rejects invalid utf8", util.clean("a\255b", 10) == nil)
check("parse_fields", (function()
	local k, f = util.parse_fields("pong\ta=1\tb=x=y")
	return k == "pong" and f.a == "1" and f.b == "x=y"
end)())
check("first_line drops traceback", util.first_line("boom\nstack traceback:\n  [C]: in ?") == "boom")
check("first_line drops traceback on one line", util.first_line("boom stack traceback: x") == "boom")
check("first_line of nil", util.first_line(nil) == "nil")

-- ---------- tree ----------
check("cost counts kind, text, id, label", tree.cost({ "row", tree.button("b1", "Ок", function() end) }) == #"row" + #"button" + #"b1" + #"Ок")
check("line cuts utf8 with ellipsis", tree.line(string.rep("я", 200), 11) == "яяяя…")
check("line keeps short", tree.line("abc") == "abc")
check("line drops traceback", tree.line("err\nstack traceback:\n x") == "err")
check("line fixes invalid utf8", utf8.len(tree.line("a\255b")) ~= nil)

do
	local T = tree.new("не влезло")
	for i = 1, 200 do T:text("Строка номер " .. i .. " с кириллицей, чтобы быстрее набрать байты") end
	local built, size = T:build()
	check("builder stops at budget", T.cut and size <= tree.LIMIT)
	check("overflow line is last", built[#built][2] == "не влезло" and built[#built].muted == true)
	local first_missing = false
	for i = 1, #built - 1 do if built[i][1] ~= "text" then first_missing = true end end
	check("only whole items kept", not first_missing)
end
do
	local T = tree.new("x")
	T:status("ok", "hi"); T:gap(); T:row(tree.button("a", "A", function() end))
	local built = T:build()
	check("small tree untouched", #built == 3 and not T.cut)
end
check("budget must leave room", not pcall(tree.new, string.rep("я", 60), 4090))

-- ---------- agent: отдельные экземпляры поверх поддельного porthole ----------
local function fresh(opts)
	local events = {}
	local a = agentmod.new(porthole, { native = "world_backup", on_event = function(k, f) events[#events + 1] = { k, f } end })
	return a, events
end

do -- send: только когда готов, токен первым, TAB и переводы строк внутри аргументов вычищены
	local a = fresh()
	check("send refused before session", a:send("resolve", "1") == false)
	a:session("host")
	bring_up()
	check("send refused while linking", a:send("resolve", "1") == false)
	agent_say("pong")
	check("ready", a.state == "ready" and a.channel == "tcp_open")
	check("send ok", a:send("backup", "7", "C:\\saves\\Мир\tс\nтабом", "clean") == true)
	local line = H.open_sent[#H.open_sent]
	check("args sanitized", line == a.token .. "\tbackup\t7\tC:\\saves\\Мир с табом\tclean\n")
	check("send logged", log_has("agent send cmd=backup channel=tcp_open ok=true"))

	-- tcp_open сломался посреди сессии: команда не теряется, уходит через tcp_send
	H.open_mode = "send_fails"
	local before = #H.tcp
	check("send survives broken tcp_open", a:send("list", "x") == true)
	check("resent over tcp_send", #H.tcp == before + 1 and last_tcp_cmd() == "list")
	check("channel now tcp_send", a.channel == "tcp_send" and a.open_broken)
	H.open_mode = "plain"
	a:stop()
end

do -- unknown events go to on_event, errors are logged too
	local a, events = fresh()
	a:session("host"); bring_up(); agent_say("pong")
	agent_say("world\tpid=1\tworld=C:\\x")
	agent_say("error\treason=bad_command\tdetail=zzz")
	check("event forwarded", events[1][1] == "world" and events[1][2].pid == "1")
	check("error forwarded and logged", events[2][1] == "error" and log_has("agent error reason=bad_command detail=zzz"))
	a:stop()
end

do -- tcp_open бросает ошибку: сразу tcp_send
	H.open_mode = "throws"
	local a = fresh()
	a:session("host"); bring_up()
	check("throws -> tcp_send ping", last_tcp_cmd() == "ping" and a.channel == "tcp_send")
	check("throw logged", log_has("tcp_open failed error=") and log_has("connection refused"))
	H.open_mode = "plain"
	a:stop()
end

do -- хелпер без cap persist (старый): tcp_open не пробуем
	local a = fresh()
	local calls = #H.open_calls
	a:session("host"); bring_up("")
	check("no persist -> no tcp_open", #H.open_calls == calls and a.channel == "tcp_send")
	a:stop()
end

do -- чужая версия протокола: fatal, процесс остановлен
	local a = fresh()
	a:session("host")
	agent_say("hello\tproto=2\tversion=9\tcaps=")
	check("proto mismatch fatal", a.state == "fatal" and a.fatal_reason == "proto 2" and helper().stopped)
	a:session("host")
	check("new session retries", a.state == "starting")
	a:stop()
end

do -- bind: порт заняли, это не считается неудачей
	local a = fresh()
	a:session("host")
	agent_say("error\treason=bind")
	helper_exit(3)
	check("bind not counted", a.fails == 0 and a.restart_in == 1)
	local n = #H.helpers
	a:tick()
	check("bind: new helper at once", #H.helpers == n + 1 and a.state == "starting")
	a:stop()
end
done()
