-- Основной путь: tcp_open работает (функции в таблице), жизненный цикл в сессии
check("tab added", H.tabs[1] == "Бэкап миров")
check("no helper outside session", #H.helpers == 0)
check("tab: wait for lobby", tab_has("Хелпер запустится, когда ты зайдёшь в лобби"))
check("dev note shown", tab_has("Тестовая сборка"))

set_role("host"); H.role_cb("host")
check("helper started in session", #H.helpers == 1)
local spec = helper().spec
check("native name", spec.native == "world_backup")
check("agent args", spec.args[1] == "--agent" and spec.args[2] == "41000" and #spec.args[3] >= 16)
check("not elevated", spec.elevate == false)
check("tab: starting", tab_has("Запускаю хелпер"))

bring_up()
check("tcp_open to agent port", H.open_calls[1] == 41000)
check("first ping over tcp_open", last_open_cmd() == "ping" and #H.tcp == 0)
check("token first", string.sub(H.open_sent[1], 1, #spec.args[3] + 1) == spec.args[3] .. "\t")
check("still starting until pong", tab_has("Запускаю хелпер"))
agent_say("pong")
check("tab: ready with channel", tab_has("Хелпер работает · канал tcp_open"))
check("log: ready + channel", log_has("agent ready version=0.0.1 port=41000 channel=tcp_open"))
check("log: start", log_has("agent start port=41000"))
check("log: session at load", log_has('session event=load role="" type=string roster=0'))
check("log: role change", log_has('session event=role_change role="host" type=string roster=1'))

-- ping раз в ~10 с (20 тиков) по тому же соединению
local n = #H.open_sent
ticks(19); check("no ping before 10 s", #H.open_sent == n)
ticks(1); check("ping after 10 s", #H.open_sent == n + 1 and last_open_cmd() == "ping")
agent_say("pong")
ticks(20); agent_say("pong")
check("no tcp_send used", #H.tcp == 0)

-- хелпер завис: pong не пришёл за 5 с -> убить и перезапустить через паузу
ticks(20); check("ping sent", last_open_cmd() == "ping")
ticks(10)
check("hung helper stopped", H.helpers[1].stopped)
check("conn closed on drop", H.open_closed == 1)
check("tab: restarting", tab_has("перезапускаю"))
ticks(9); check("waits backoff", #H.helpers == 1)
ticks(1); check("restarted", #H.helpers == 2 and helper().spec.args[2] == "41001")
bring_up(); agent_say("pong")
check("tcp_open used again", H.open_calls[2] == 41001 and tab_has("канал tcp_open"))

-- Porthole перезапускает хелпер (как при входе в лобби): on_exit -> перезапуск
helper_exit(0)
check("tab: restarting after exit", tab_has("перезапускаю"))
ticks(19); check("backoff grows after 2nd failure", #H.helpers == 2)
ticks(1); check("restarted after exit", #H.helpers == 3)
bring_up(); agent_say("pong")

-- выход из сессии: хелпер остановлен, соединение закрыто, перезапусков нет
local closed = H.open_closed
set_role(""); H.end_cb()
check("helper stopped at session end", helper().stopped)
check("log: stop", log_has("agent stop port=41002"))
check("log: session end", log_has("session event=session_end role=\"nil\""))
check("conn closed at session end", H.open_closed == closed + 1)
check("tab: wait again", tab_has("Хелпер запустится"))
ticks(300); check("no restart outside session", #H.helpers == 3)

-- строки старого хелпера игнорируются
agent_say("pong")
check("stale lines ignored", tab_has("Хелпер запустится"))
check("tree under limit", H.max_text <= 4096)
check("no notifications", #H.notes == 0)
done()
