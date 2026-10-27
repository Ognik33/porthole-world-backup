-- Признак лобби и порядок событий, как в логах Porthole 3.0.2
-- при загрузке role() = "", потом on_role_change("idle")
H.role_cb("idle")
ticks(20)
check("no helper outside lobby", #H.helpers == 0)

-- вход в лобби: on_session_end и on_role_change("host") в один момент, role() может ещё отставать
H.end_cb()
H.role_cb("host")
check("helper started on role event", #H.helpers == 1)
ticks(3) -- 1,5 с: role() всё ещё "idle"
check("short lag of role() ignored", not helper().stopped)
set_role("host")
ticks(10)
check("helper kept", not helper().stopped and #H.helpers == 1)
check("one start only", log_has("agent start port=41000") and not log_has("agent start port=41001"))

-- обратный порядок событий при входе: сначала роль, потом конец «idle»-сессии
H.end_cb()
check("session_end with role host keeps helper", not helper().stopped)

-- выход из лобби без единого события: опрос role() останавливает хелпер через 2 с
set_role("idle")
ticks(4); check("still running within grace", not helper().stopped)
ticks(1); check("stopped by poll", helper().stopped and log_has('session event=poll role="idle"'))
ticks(40); check("no restart after leave", #H.helpers == 1)

-- участник лобби: любая роль, кроме "" и "idle", считается сессией
set_role("member"); H.role_cb("member")
check("member starts helper", #H.helpers == 2)
-- смена роли внутри сессии перезапускает хелпер
set_role("host"); H.role_cb("host")
check("role switch restarts", H.helpers[2].stopped and #H.helpers == 3)
done()
