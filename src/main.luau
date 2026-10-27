-- world-backup: резервные копии миров Minecraft, которые хостят через Porthole.
--
-- Сборка 0.0.x — каркас (шаг 1 плана): хелпер живёт только внутри сессии, связь с ним
-- проверяется ping-ом, вкладка показывает состояние и выбранный канал команд.
-- Определение мира (шаг 2), копирование (шаг 3), логика решений (шаг 4) и карточки миров (шаг 5)
-- подключаются сюда же.
local util = require("./util")
local tree = require("./tree")
local agentmod = require("./agent")

local P = porthole
local t = P.t

local function log(s) P.log("world-backup: " .. s) end

-- ---------- вкладка ----------
local tab
do
	local ok, res = pcall(P.ui.add_tab, { panel = "sharing", title = t("tab") })
	if ok then tab = res else log("add_tab failed: " .. util.first_line(res)) end
end

local agent
local notified_fatal = false

local function render()
	if not tab then return end
	local T = tree.new(t("tab_truncated"))
	local st = agent.state
	if st == "off" then
		T:status("info", t("helper_wait_lobby"))
	elseif st == "starting" or st == "linking" then
		T:status("info", t("helper_starting"))
	elseif st == "ready" then
		T:status("ok", t("helper_ready", { channel = agent.channel or "?" }))
	elseif st == "restarting" then
		T:status("error", t("helper_restarting"))
	else
		T:status("error", t("helper_failed", { reason = agent.fatal_reason or "?" }))
	end
	T:text(t("dev_note"), true)
	local built, size = T:build()
	local ok, err = pcall(P.ui.render, tab, built)
	if not ok then log("render failed: " .. util.first_line(err) .. " size=" .. size) end
end

-- ---------- хелпер ----------
agent = agentmod.new(P, {
	native = "world_backup",
	on_change = function()
		if agent.state == "fatal" and not notified_fatal then
			notified_fatal = true
			pcall(P.notify, "error", t("notify_helper_failed", { reason = agent.fatal_reason or "?" }))
		elseif agent.state == "ready" then
			notified_fatal = false
		end
		render()
	end,
	on_event = function(kind, f)
		-- шаг 2 и дальше: ответы resolve, backup, list и т. д.
	end,
})

-- ---------- жизненный цикл ----------
P.on_tick(function() agent:tick() end)
do
	local ok, err = pcall(P.on_role_change, function(role) agent:session(role) end)
	if not ok then log("on_role_change failed: " .. util.first_line(err)) end
	ok, err = pcall(P.on_session_end, function() agent:session("") end)
	if not ok then log("on_session_end failed: " .. util.first_line(err)) end
end
do
	local ok, role = pcall(P.role)
	agent:session(ok and role or "")
end
render()
