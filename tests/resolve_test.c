// Тесты переносимой логики resolve.c (без Windows): разбор аргументов, пути, выбор мира.
// Сборка: gcc -Inative tests/resolve_test.c native/resolve.c native/main.c
#include "wb.h"

static int passed = 0, failed = 0;
#define CHECK(name, cond) do { if (cond) { passed++; } else { failed++; printf("FAIL %s (line %d)\n", name, __LINE__); } } while (0)

static const char *norm(const char *in) {
    static char out[RS_PATH];
    return rs_path_norm(in, out, sizeof out) ? out : "<fail>";
}

static const char *gdir(int argc, char **argv, const char *cwd, const char **how) {
    static char out[RS_PATH];
    rs_args a;
    rs_parse_args(argc, argv, &a);
    return rs_game_dir(&a, cwd, out, sizeof out, how) ? out : "<fail>";
}

int main(void) {
    // ---------- абсолютные пути ----------
    CHECK("abs drive", rs_is_abs("C:\\x") && rs_is_abs("d:/x"));
    CHECK("abs unc", rs_is_abs("\\\\srv\\share"));
    CHECK("not abs", !rs_is_abs("x\\y") && !rs_is_abs("C:x") && !rs_is_abs("\\x") && !rs_is_abs(""));

    // ---------- нормализация ----------
    CHECK("norm slashes", !strcmp(norm("c:/Games//MC/"), "C:\\Games\\MC"));
    CHECK("norm dots", !strcmp(norm("C:\\a\\.\\b\\..\\c"), "C:\\a\\c"));
    CHECK("norm above root", !strcmp(norm("C:\\..\\..\\x"), "C:\\x"));
    CHECK("norm root", !strcmp(norm("C:\\"), "C:\\"));
    CHECK("norm root after pop", !strcmp(norm("C:\\a\\.."), "C:\\"));
    CHECK("norm unc", !strcmp(norm("\\\\srv\\share\\a\\..\\..\\b"), "\\\\srv\\share\\b"));
    CHECK("norm cyrillic", !strcmp(norm("C:\\Игры\\Майнкрафт\\"), "C:\\Игры\\Майнкрафт"));
    CHECK("norm relative fails", !strcmp(norm("a\\b"), "<fail>"));

    // ---------- аргументы ----------
    {
        char *v[] = { "javaw.exe", "-Xmx4G", "net.minecraft.client.main.Main", "--username", "Ognik33",
                      "--gameDir", "C:\\Users\\u\\AppData\\Roaming\\.minecraft", "--accessToken", "SECRET" };
        rs_args a;
        rs_parse_args(9, v, &a);
        CHECK("vanilla gameDir", a.gamedir && !strcmp(a.gamedir, "C:\\Users\\u\\AppData\\Roaming\\.minecraft"));
        CHECK("vanilla launcher", !strcmp(a.launcher, "vanilla"));
        CHECK("gameDir is not the token", strcmp(a.gamedir, "SECRET") != 0);
    }
    {
        char *v[] = { "java", "--gameDir=D:/mc", "-Dminecraft.applet.TargetDirectory=E:\\old" };
        rs_args a;
        rs_parse_args(3, v, &a);
        CHECK("gameDir= form wins over targetdir", a.gamedir && !strcmp(a.gamedir, "D:/mc") && !strcmp(a.source, "gameDir"));
    }
    {
        char *v[] = { "java", "-Dminecraft.applet.TargetDirectory=E:\\old", "--GAMEDIR", "F:\\new" };
        rs_args a;
        rs_parse_args(4, v, &a);
        CHECK("gameDir case-insensitive, after targetdir", !strcmp(a.gamedir, "F:\\new"));
    }
    {
        char *v[] = { "javaw.exe", "-cp", "NewLaunch.jar", "org.multimc.EntryPoint" };
        rs_args a;
        rs_parse_args(4, v, &a);
        CHECK("multimc: no gameDir", a.gamedir == NULL && !strcmp(a.launcher, "multimc"));
    }
    {
        char *v[] = { "javaw.exe", "org.prismlauncher.EntryPoint" };
        rs_args a;
        rs_parse_args(2, v, &a);
        CHECK("prism", !strcmp(a.launcher, "prism"));
    }
    {
        char *v[] = { "java", "--gameDir" };
        rs_args a;
        rs_parse_args(2, v, &a);
        CHECK("gameDir without value ignored", a.gamedir == NULL);
    }

    // ---------- папка игры ----------
    {
        const char *how;
        char *v1[] = { "java", "--gameDir", "C:/mc/inst" };
        CHECK("abs gameDir", !strcmp(gdir(3, v1, "D:\\cwd", &how), "C:\\mc\\inst") && !strcmp(how, "gameDir"));
        char *v2[] = { "java", "--gameDir", "..\\inst" };
        CHECK("relative gameDir from cwd", !strcmp(gdir(3, v2, "D:\\launcher\\bin", &how), "D:\\launcher\\inst") && !strcmp(how, "gameDir+cwd"));
        CHECK("relative gameDir without cwd fails", !strcmp(gdir(3, v2, NULL, &how), "<fail>"));
        char *v3[] = { "java" };
        CHECK("no gameDir: cwd", !strcmp(gdir(1, v3, "C:\\MultiMC\\instances\\1.7.10\\.minecraft\\", &how), "C:\\MultiMC\\instances\\1.7.10\\.minecraft") && !strcmp(how, "cwd"));
        CHECK("no gameDir, no cwd fails", !strcmp(gdir(1, v3, NULL, &how), "<fail>"));
        char *v4[] = { "java", "--gameDir", "\\mc" };
        CHECK("root-relative gameDir", !strcmp(gdir(3, v4, "E:\\x\\y", &how), "E:\\mc"));
        char *v5[] = { "java", "--gameDir", "E:mc" };
        CHECK("drive-relative, same drive as cwd", !strcmp(gdir(3, v5, "E:\\x", &how), "E:\\x\\mc"));
        CHECK("drive-relative, other drive", !strcmp(gdir(3, v5, "C:\\x", &how), "E:\\mc"));
        char *v6[] = { "java", "--gameDir", "\\mc" };
        CHECK("root-relative on UNC cwd", !strcmp(gdir(3, v6, "\\\\nas\\games\\launcher", &how), "\\\\nas\\games\\mc"));
        char *v7[] = { "java", "--gameDir", "." };
        CHECK("gameDir dot", !strcmp(gdir(3, v7, "C:\\mc\\", &how), "C:\\mc"));
    }

    // ---------- выбор мира ----------
    {
        int picked, cand[20], nc;
        rs_world w[4] = { { "A", 0, 100 }, { "B", 1, 50 }, { "C", 0, 2000 }, { "D", -1, -1 } };
        CHECK("one locked -> lock", rs_pick(w, 4, 1000, &picked, cand, 20, &nc) == RS_LOCK && picked == 1);

        rs_world m[3] = { { "Old", 0, 500 }, { "Fresh", 0, 1001 }, { "Older", 0, 100 } };
        CHECK("none locked, one fresh -> mtime", rs_pick(m, 3, 1000, &picked, cand, 20, &nc) == RS_MTIME && picked == 1);
        CHECK("slack 2 s", rs_pick((rs_world[]){ { "X", 0, 998 } }, 1, 1000, &picked, cand, 20, &nc) == RS_MTIME);
        CHECK("start unknown -> ask from all", rs_pick(m, 3, 0, &picked, cand, 20, &nc) == RS_ASK && picked == -1 && nc == 3);
        CHECK("ask sorted newest first", cand[0] == 1 && cand[1] == 0 && cand[2] == 2);

        rs_world two[3] = { { "P", 1, 10 }, { "Q", 0, 20 }, { "R", 1, 30 } };
        CHECK("two locked -> ask among locked", rs_pick(two, 3, 1000, &picked, cand, 20, &nc) == RS_ASK && nc == 2 && cand[0] == 2 && cand[1] == 0);

        rs_world fr[3] = { { "F1", 0, 1100 }, { "Old", 0, 10 }, { "F2", 0, 1200 } };
        CHECK("two fresh -> ask among fresh", rs_pick(fr, 3, 1000, &picked, cand, 20, &nc) == RS_ASK && nc == 2 && cand[0] == 2);

        CHECK("no worlds -> ask, zero candidates", rs_pick(NULL, 0, 1000, &picked, cand, 20, &nc) == RS_ASK && nc == 0);

        rs_world many[30];
        for (int i = 0; i < 30; i++) { snprintf(many[i].name, sizeof many[i].name, "w%d", i); many[i].locked = 0; many[i].lock_mtime = i; }
        rs_pick(many, 30, 0, &picked, cand, 20, &nc);
        int sorted = nc == 20;
        for (int i = 0; i < 20 && sorted; i++) sorted = cand[i] == 29 - i;
        CHECK("candidates capped at 20, newest kept", sorted);
    }

    printf("OK %d checks\n", passed);
    return failed ? 1 : 0;
}
