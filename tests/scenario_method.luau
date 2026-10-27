-- объект tcp_open с методами (self первым аргументом), как у userdata
H.open_mode = "method"
set_role("host"); H.role_cb("host")
bring_up()
check("ping went through method call", last_open_cmd() == "ping")
agent_say("pong")
check("ready over tcp_open", tab_has("канал tcp_open"))
check("log: call=method", log_has("call=method"))
H.end_cb()
check("closed with method call", H.open_closed == 1)
done()
