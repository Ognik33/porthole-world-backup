-- tcp_open открывается, но команды по нему не доходят: откат на tcp_send и запоминание отката
set_role("member"); H.role_cb("member")
bring_up()
check("ping over tcp_open", last_open_cmd() == "ping")
ticks(9); check("still waiting", #H.tcp == 0)
ticks(1)
check("switched to tcp_send", last_tcp_cmd() == "ping")
check("tcp_open conn closed", H.open_closed == 1)
check("log: switch reason", log_has("tcp_open unusable reason=no pong, switching to tcp_send"))
agent_say("pong")
check("tab: ready over tcp_send", tab_has("канал tcp_send"))
ticks(20); check("next ping over tcp_send", last_tcp_cmd() == "ping" and #H.open_sent == 1)
agent_say("pong")

-- после перезапуска хелпера tcp_open уже не пробуем: не теряем 5 с на каждом старте
helper_exit(1); ticks(10)
bring_up()
check("tcp_open not retried", #H.open_calls == 1 and last_tcp_cmd() == "ping")
agent_say("pong")
check("ready again", tab_has("канал tcp_send"))

-- и по tcp_send pong не пришёл: это уже зависший хелпер, перезапуск
ticks(20); ticks(10)
check("dropped on no pong over tcp_send", helper().stopped and tab_has("перезапускаю"))

-- хелпер не дошёл до ready за 10 с
ticks(20); check("third helper", #H.helpers == 3)
ticks(19); check("still starting", not helper().stopped)
ticks(1); check("dropped without ready", helper().stopped and log_has("reason=no_ready"))
done()
