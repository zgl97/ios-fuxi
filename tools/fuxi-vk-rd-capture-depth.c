/*
 * fuxi-vk-rd-capture: run fuxi-vk-smoke with Freedreno RD dumps enabled,
 * then replay the captured command streams through the raw NativeAdreno ABI.
 *
 * The iOS root filesystem has no persistent scratch space for large RD files,
 * so replay happens in the same boot and only a compact manifest is printed.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define DUMP_DIR "/tmp"
#define SMOKE_BIN "/usr/local/bin/fuxi-vk-smoke"
#define REPLAY_BIN "/usr/local/bin/fuxi-rd-replay"
#define DRAW_BIN "/usr/local/bin/nonexistent-a740-draw"

static int has_rd_suffix(const char *name) {
    size_t n = strlen(name);
    if (n >= 3 && strcmp(name + n - 3, ".rd") == 0) return 1;
    if (n >= 6 && strcmp(name + n - 6, ".rd.gz") == 0) return 1;
    return 0;
}

static void clean_old_dumps(void) {
    DIR *d = opendir(DUMP_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!has_rd_suffix(e->d_name)) continue;
        char path[1024];
        snprintf(path, sizeof(path), DUMP_DIR "/%s", e->d_name);
        unlink(path);
    }
    closedir(d);
}

static int run_child(const char *tag, const char *path, char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        printf("RD_CAPTURE ERROR fork %s: %s\n", tag, strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execv(path, argv);
        printf("RD_CAPTURE ERROR exec %s: %s\n", path, strerror(errno));
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        printf("RD_CAPTURE ERROR waitpid %s: %s\n", tag, strerror(errno));
        return -1;
    }
    if (WIFEXITED(status)) {
        printf("RD_CAPTURE CHILD %s exit=%d\n", tag, WEXITSTATUS(status));
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        printf("RD_CAPTURE CHILD %s signal=%d\n", tag, WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    printf("RD_CAPTURE CHILD %s status=%d\n", tag, status);
    return -1;
}

int main(void) {
    int console = open("/dev/console", O_WRONLY);
    if (console >= 0) {
        dup2(console, STDOUT_FILENO);
        dup2(console, STDERR_FILENO);
        close(console);
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    setenv("FD_RD_DUMP", "full", 1);
    setenv("FD_RD_DUMP_PATH", DUMP_DIR, 1);

    clean_old_dumps();
    printf("RD_CAPTURE BEGIN\n");

    char *smoke_argv[] = {(char *)SMOKE_BIN, NULL};
    int smoke_rc = run_child("smoke", SMOKE_BIN, smoke_argv);

    DIR *d = opendir(DUMP_DIR);
    if (!d) {
        printf("RD_CAPTURE ERROR opendir %s: %s\n", DUMP_DIR, strerror(errno));
        return 2;
    }
    struct dirent *e;
    unsigned files = 0;
    unsigned long long total = 0;
    while ((e = readdir(d)) != NULL) {
        if (!has_rd_suffix(e->d_name)) continue;
        char path[1024];
        snprintf(path, sizeof(path), DUMP_DIR "/%s", e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        files++;
        total += (unsigned long long)st.st_size;
        printf("RD_CAPTURE FILE %s %llu\n", e->d_name, (unsigned long long)st.st_size);
    }
    closedir(d);
    printf("RD_CAPTURE MANIFEST files=%u bytes=%llu smoke_rc=%d\n", files, total, smoke_rc);
    if (!files) return 3;

    char *replay_argv[] = {(char *)REPLAY_BIN, (char *)DUMP_DIR, (char *)"--dump", NULL};
    int replay_rc = run_child("replay", REPLAY_BIN, replay_argv);
    int draw_rc = -1;
    if (access(DRAW_BIN, X_OK) == 0) {
        char *draw_argv[] = {(char *)DRAW_BIN, NULL};
        draw_rc = run_child("draw", DRAW_BIN, draw_argv);
    }
    printf("RD_CAPTURE DONE files=%u bytes=%llu smoke_rc=%d replay_rc=%d draw_rc=%d\n", files, total, smoke_rc, replay_rc, draw_rc);
    return (replay_rc == 0 && (draw_rc == -1 || draw_rc == 0)) ? 0 : 4;
}
