/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native-title boundary only; the clock renderer remains unchanged.
 * Follows the public native kit's self-update example: ask the shell to close
 * the title instead of reaching its unsupported libc exit path.
 */
#include <stddef.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdint.h>
extern int sceSysmoduleLoadModuleInternal(unsigned);
extern int sceKernelOpen(const char *, int, unsigned short);
extern int64_t sceKernelWrite(int, const void *, size_t);
extern int sceKernelClose(int);
extern int sceKernelSendNotificationRequest(int, void *, size_t, int);
_Noreturn void catchReturnFromMain(int status);

static int startup_log = -1;

int aurora_native_log(const char *text, size_t length)
{
    if (startup_log < 0) return -1;
    return (int)sceKernelWrite(startup_log, text, length);
}

static void startup_record(const char *name, int result)
{
    char line[192];
    int length = snprintf(line, sizeof(line), "%s: 0x%08x\n", name, (unsigned)result);
    if (startup_log >= 0 && length > 0 && (size_t)length < sizeof(line))
        (void)sceKernelWrite(startup_log, line, (size_t)length);
}

static _Noreturn void startup_failure(const char *name, int result)
{
    struct { unsigned char reserved[45]; char message[3075]; } notice = {0};
    snprintf(notice.message, sizeof(notice.message),
             "Aurora Clock could not initialize %s (0x%08x). See its startup log.",
             name, (unsigned)result);
    sceKernelSendNotificationRequest(0, &notice, sizeof(notice), 0);
    catchReturnFromMain(1);
}

/* Read actual GOT slots, not a C function-address comparison that the compiler
 * can fold to true for a strong declaration. */
#define REQUIRE_IMPORT(symbol) do { \
    void *address; \
    __asm__ volatile("movq " #symbol "@GOTPCREL(%%rip), %0" : "=r"(address)); \
    startup_record(#symbol, address ? 0 : -1); \
    if (!address) startup_failure(#symbol, -1); \
} while (0)
/* Request process-local modules through the native title API, not the payload
 * loader's out-of-sandbox filesystem paths. Only the display, controller and
 * shell services used by this standalone app are requested.
 * No console settings or system files are changed.
 */
__attribute__((constructor)) static void initialize_native_dependencies(void)
{
    static const struct { const char *name; unsigned id; } modules[] = {
        {"libSceUserService", 0x80000011},
        {"libScePad", 0x80000024},
        {"libSceVideoOut", 0x80000022},
        {"libSceSystemService", 0x80000010}
    };
    startup_log = sceKernelOpen("/data/aurora-clock-PPSA99840-startup.log",
                               O_WRONLY | O_CREAT | O_TRUNC, 0666);
    REQUIRE_IMPORT(sceSysmoduleLoadModuleInternal);
    for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); ++i) {
        int result = sceSysmoduleLoadModuleInternal(modules[i].id);
        startup_record(modules[i].name, result);
        if (result < 0) startup_failure(modules[i].name, result);
    }
    REQUIRE_IMPORT(sceUserServiceInitialize);
    REQUIRE_IMPORT(scePadInit);
    REQUIRE_IMPORT(sceVideoOutOpen);
    REQUIRE_IMPORT(sceSystemServiceLoadExec);
#undef REQUIRE_IMPORT
    startup_record("Native dependencies ready", 0);
}
extern int sceSystemServiceLoadExec(const char *, const char **);
extern int sceKernelUsleep(unsigned int);

_Noreturn void catchReturnFromMain(int status)
{
    startup_record("main returned", status);
    if (startup_log >= 0) {
        sceKernelClose(startup_log);
        startup_log = -1;
    }
    void *close_entry;
    __asm__ volatile("movq sceSystemServiceLoadExec@GOTPCREL(%%rip), %0" : "=r"(close_entry));
    int result = close_entry ? sceSystemServiceLoadExec("exit", NULL) : -1;
    struct { unsigned char reserved[45]; char message[3075]; } notice = {0};
    snprintf(notice.message, sizeof(notice.message),
             "Aurora Clock stopped (status %d; close result 0x%08x). Close the app from Games.",
             status, (unsigned)result);
    sceKernelSendNotificationRequest(0, &notice, sizeof(notice), 0);
    /* The shell owns final teardown. Never fall into the unsupported exit(). */
    for (;;) sceKernelUsleep(1000000);
}
