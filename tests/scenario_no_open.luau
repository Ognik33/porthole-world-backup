-- в Porthole нет tcp_open: сразу tcp_send, без задержки на проверку
porthole.tcp_open = nil
set_role("host"); H.role_cb("host")
bring_up()
check("ping over tcp_send", last_tcp_cmd() == "ping")
agent_say("pong")
check("ready over tcp_send", tab_has("канал tcp_send"))
check("log: not available", log_has("tcp_open not available"))

-- run_helper отказал: состояние fatal, одно уведомление, без бесконечных перезапусков
set_role(""); H.end_cb()
porthole.run_helper = function() error("native world_backup not allowed\nstack traceback:\n  [C]: in ?", 0) end
set_role("host"); H.role_cb("host")
check("tab: failed with first line only", tab_has("Хелпер не запустился: native world_backup not allowed") and not tab_has("traceback"))
check("one notification", #H.notes == 1 and string.find(H.notes[1], "^error: Бэкап миров") ~= nil)
ticks(300); check("no restart loop", #H.notes == 1)
done()
