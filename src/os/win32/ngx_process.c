
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>


int              ngx_argc;
char           **ngx_argv;
char           **ngx_os_argv;

ngx_int_t        ngx_last_process;
ngx_process_t    ngx_processes[NGX_MAX_PROCESSES];


ngx_pid_t
ngx_spawn_process(ngx_cycle_t *cycle, char *name, ngx_int_t respawn)
{
    u_long          rc, n, code;
    ngx_int_t       s;
    ngx_pid_t       pid;
    ngx_exec_ctx_t  ctx;
    HANDLE          events[2];
    char            file[MAX_PATH + 1];

    if (respawn >= 0) {
        s = respawn;

    } else {
        for (s = 0; s < ngx_last_process; s++) {
            if (ngx_processes[s].handle == NULL) {
                break;
            }
        }

        if (s == NGX_MAX_PROCESSES) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                          "no more than %d processes can be spawned",
                          NGX_MAX_PROCESSES);
            return NGX_INVALID_PID;
        }
    }

    n = GetModuleFileName(NULL, file, MAX_PATH);

    if (n == 0) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "GetModuleFileName() failed");
        return NGX_INVALID_PID;
    }

    file[n] = '\0';

    ngx_log_debug1(NGX_LOG_DEBUG_CORE, cycle->log, 0,
                   "GetModuleFileName: \"%s\"", file);

    ctx.path = file;
    ctx.name = name;
    ctx.args = GetCommandLine();
    ctx.argv = NULL;
    ctx.envp = NULL;

    pid = ngx_execute(cycle, &ctx);

    if (pid == NGX_INVALID_PID) {
        return pid;
    }

    ngx_memzero(&ngx_processes[s], sizeof(ngx_process_t));

    ngx_processes[s].handle = ctx.child;
    ngx_processes[s].pid = pid;
    ngx_processes[s].name = name;

    ngx_sprintf(ngx_processes[s].term_event, "ngx_%s_term_%P%Z", name, pid);
    ngx_sprintf(ngx_processes[s].quit_event, "ngx_%s_quit_%P%Z", name, pid);
    ngx_sprintf(ngx_processes[s].reopen_event, "ngx_%s_reopen_%P%Z",
                name, pid);

    events[0] = ngx_master_process_event;
    events[1] = ctx.child;

    rc = WaitForMultipleObjects(2, events, 0, 5000);

    ngx_time_update();

    ngx_log_debug1(NGX_LOG_DEBUG_CORE, cycle->log, 0,
                   "WaitForMultipleObjects: %ul", rc);

    switch (rc) {

    case WAIT_OBJECT_0:

        ngx_processes[s].term = OpenEvent(EVENT_MODIFY_STATE, 0,
                                          (char *) ngx_processes[s].term_event);
        if (ngx_processes[s].term == NULL) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "OpenEvent(\"%s\") failed",
                          ngx_processes[s].term_event);
            goto failed;
        }

        ngx_processes[s].quit = OpenEvent(EVENT_MODIFY_STATE, 0,
                                          (char *) ngx_processes[s].quit_event);
        if (ngx_processes[s].quit == NULL) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "OpenEvent(\"%s\") failed",
                          ngx_processes[s].quit_event);
            goto failed;
        }

        ngx_processes[s].reopen = OpenEvent(EVENT_MODIFY_STATE, 0,
                                       (char *) ngx_processes[s].reopen_event);
        if (ngx_processes[s].reopen == NULL) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "OpenEvent(\"%s\") failed",
                          ngx_processes[s].reopen_event);
            goto failed;
        }

        if (ResetEvent(ngx_master_process_event) == 0) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                          "ResetEvent(\"%s\") failed",
                          ngx_master_process_event_name);
            goto failed;
        }

        break;

    case WAIT_OBJECT_0 + 1:
        if (GetExitCodeProcess(ctx.child, &code) == 0) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "GetExitCodeProcess(%P) failed", pid);
        }

        ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                      "%s process %P exited with code %Xl",
                      name, pid, code);

        goto failed;

    case WAIT_TIMEOUT:
        ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                      "the event \"%s\" was not signaled for 5s",
                      ngx_master_process_event_name);
        goto failed;

    case WAIT_FAILED:
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "WaitForSingleObject(\"%s\") failed",
                      ngx_master_process_event_name);

        goto failed;
    }

    if (respawn >= 0) {
        return pid;
    }

    switch (respawn) {

    case NGX_PROCESS_RESPAWN:
        ngx_processes[s].just_spawn = 0;
        break;

    case NGX_PROCESS_JUST_RESPAWN:
        ngx_processes[s].just_spawn = 1;
        break;
    }

    if (s == ngx_last_process) {
        ngx_last_process++;
    }

    return pid;

failed:

    if (ngx_processes[s].reopen) {
        ngx_close_handle(ngx_processes[s].reopen);
    }

    if (ngx_processes[s].quit) {
        ngx_close_handle(ngx_processes[s].quit);
    }

    if (ngx_processes[s].term) {
        ngx_close_handle(ngx_processes[s].term);
    }

    TerminateProcess(ngx_processes[s].handle, 2);

    if (ngx_processes[s].handle) {
        ngx_close_handle(ngx_processes[s].handle);
        ngx_processes[s].handle = NULL;
    }

    return NGX_INVALID_PID;
}


#if (_WIN32_WINNT >= 0x0600)

#define ngx_win32_startupinfoex_t    STARTUPINFOEX
#define ngx_win32_init_attr_list     InitializeProcThreadAttributeList
#define ngx_win32_update_attr        UpdateProcThreadAttribute
#define ngx_win32_delete_attr_list   DeleteProcThreadAttributeList

#else

/*
 * STARTUPINFOEX, the InitializeProcThreadAttributeList() family and
 * PROC_THREAD_ATTRIBUTE_HANDLE_LIST require _WIN32_WINNT >= 0x0600, while
 * the rest of this code base is built with _WIN32_WINNT == 0x0501.  Rather
 * than raising the target version, and thereby changing the set of
 * declarations seen by every other module, the missing subset is spelled
 * out locally.  The kernel32.dll entry points are looked up at run time, so
 * the resulting binary still starts on systems older than Vista and simply
 * falls back to a child created without stderr.
 *
 * EXTENDED_STARTUPINFO_PRESENT itself needs no substitute: <winbase.h>
 * defines it unconditionally.
 */

/*
 * PROC_THREAD_ATTRIBUTE_HANDLE_LIST is built by ProcThreadAttributeValue()
 * in <winbase.h> and expands to
 * (2 | (FALSE ? 0 : 0x10000) | (TRUE ? 0 : 0x20000) | (FALSE ? 0 : 0x40000)),
 * which is a constant.  The macro itself is unusable here because the
 * ProcThreadAttributeHandleList enumerator it needs is fenced off too.
 */

#define PROC_THREAD_ATTRIBUTE_HANDLE_LIST   0x00020002


typedef struct {
    STARTUPINFO                          StartupInfo;
    void                                *lpAttributeList;
} ngx_win32_startupinfoex_t;


typedef BOOL (WINAPI *ngx_win32_init_attr_list_pt)(void *list, DWORD count,
    DWORD flags, size_t *size);
typedef BOOL (WINAPI *ngx_win32_update_attr_pt)(void *list, DWORD flags,
    DWORD_PTR attribute, void *value, size_t value_size, void *prev,
    size_t *return_size);
typedef void (WINAPI *ngx_win32_delete_attr_list_pt)(void *list);


static ngx_win32_init_attr_list_pt    ngx_win32_init_attr_list;
static ngx_win32_update_attr_pt       ngx_win32_update_attr;
static ngx_win32_delete_attr_list_pt  ngx_win32_delete_attr_list;
static ngx_uint_t                     ngx_win32_attr_checked;

#endif


ngx_pid_t
ngx_execute(ngx_cycle_t *cycle, ngx_exec_ctx_t *ctx)
{
    HANDLE                 hstderr;
    HANDLE                 handles[1];
    DWORD                  creation_flags;
    DWORD                  fdflags;
    ngx_uint_t             inherit;
    void                  *attrs;
    size_t                 attrs_size;
    PROCESS_INFORMATION    pi;
#if (_WIN32_WINNT < 0x0600)
    HMODULE                kernel32;
#endif
    ngx_win32_startupinfoex_t  si;

    inherit = 0;

#if (_WIN32_WINNT < 0x0600)
    if (ngx_win32_attr_checked == 0) {
        ngx_win32_attr_checked = 1;

        kernel32 = GetModuleHandle("kernel32.dll");

        if (kernel32 != NULL) {
            ngx_win32_init_attr_list = (ngx_win32_init_attr_list_pt)
                (void *) GetProcAddress(kernel32,
                                        "InitializeProcThreadAttributeList");
            ngx_win32_update_attr = (ngx_win32_update_attr_pt)
                (void *) GetProcAddress(kernel32, "UpdateProcThreadAttribute");
            ngx_win32_delete_attr_list = (ngx_win32_delete_attr_list_pt)
                (void *) GetProcAddress(kernel32,
                                        "DeleteProcThreadAttributeList");
        }
    }

    /*
     * On a system that predates Vista the attribute API is missing and the
     * child is created the way it always was, without the standard handles.
     */

    if (ngx_win32_init_attr_list == NULL
        || ngx_win32_update_attr == NULL
        || ngx_win32_delete_attr_list == NULL)
    {
        goto wrong_handles;
    }
#endif

    /*
     * A child created with bInheritHandles set to FALSE never receives the
     * standard handles, which is why a worker started by the master has no
     * stderr of its own.  Turning inheritance on for everything is not an
     * option either: it would duplicate the master's listening sockets, its
     * log files and the handles of every other worker into the new process.
     * A duplicated log file handle is what breaks the rename performed by
     * ngx_reopen_files() on Windows, because the file stays in the
     * delete-pending state while any copy of the handle remains open.
     * PROC_THREAD_ATTRIBUTE_HANDLE_LIST solves both problems at once by
     * letting inheritance be enabled and then narrowed to stderr alone.
     * Both features require Vista, hence the probing above.
     */

    hstderr = GetStdHandle(STD_ERROR_HANDLE);

    if (hstderr == NULL || hstderr == INVALID_HANDLE_VALUE) {
        goto wrong_handles;
    }

    /*
     * GetStdHandle() promises nothing about inheritable state: it returns
     * whatever handle the creating process passed down, or a console
     * handle, and neither is necessarily marked with HANDLE_FLAG_INHERIT.
     * Handing a non-inheritable handle to CreateProcess() together with
     * STARTF_USESTDHANDLES makes the call fail outright, so the flag is
     * raised when needed and the original state is restored afterwards,
     * leaving the master's handles exactly as they were found.
     */

    fdflags = 0;

    if (GetHandleInformation(hstderr, &fdflags) == 0) {
        goto wrong_handles;
    }

    inherit = (fdflags & HANDLE_FLAG_INHERIT) ? 0 : 1;

    if (inherit
        && SetHandleInformation(hstderr, HANDLE_FLAG_INHERIT,
                                HANDLE_FLAG_INHERIT)
           == 0)
    {
        ngx_log_error(NGX_LOG_WARN, cycle->log, ngx_errno,
                      "SetHandleInformation(stderr) failed, "
                      "the new process will not inherit stderr");

        goto wrong_handles;
    }

    /*
     * InitializeProcThreadAttributeList() is called first with a NULL list
     * in order to learn how much memory the list needs; it fails with
     * ERROR_INSUFFICIENT_BUFFER and reports the size in that case.
     */

    attrs_size = 0;

    if (ngx_win32_init_attr_list(NULL, 1, 0, &attrs_size) == 0
        && ngx_errno != ERROR_INSUFFICIENT_BUFFER)
    {
        ngx_log_error(NGX_LOG_WARN, cycle->log, ngx_errno,
                      "InitializeProcThreadAttributeList() failed, "
                      "the new process will not inherit stderr");

        goto failed;
    }

    attrs = ngx_alloc(attrs_size, cycle->log);

    if (attrs == NULL) {
        goto failed;
    }

    if (ngx_win32_init_attr_list(attrs, 1, 0, &attrs_size) == 0) {
        ngx_log_error(NGX_LOG_WARN, cycle->log, ngx_errno,
                      "InitializeProcThreadAttributeList() failed, "
                      "the new process will not inherit stderr");

        ngx_free(attrs);

        goto failed;
    }

    handles[0] = hstderr;

    if (ngx_win32_update_attr(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                              handles, sizeof(handles), NULL, NULL)
        == 0)
    {
        ngx_log_error(NGX_LOG_WARN, cycle->log, ngx_errno,
                      "UpdateProcThreadAttribute(PROC_THREAD_ATTRIBUTE_"
                      "HANDLE_LIST) failed, "
                      "the new process will not inherit stderr");

        ngx_win32_delete_attr_list(attrs);
        ngx_free(attrs);

        goto failed;
    }

    ngx_memzero(&si, sizeof(ngx_win32_startupinfoex_t));

    si.StartupInfo.cb = sizeof(ngx_win32_startupinfoex_t);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
    si.StartupInfo.hStdOutput = INVALID_HANDLE_VALUE;
    si.StartupInfo.hStdError = hstderr;
    si.lpAttributeList = attrs;

    creation_flags = CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;

    if (CreateProcess(ctx->path, ctx->args, NULL, NULL, 1, creation_flags,
                      NULL, NULL, (STARTUPINFO *) &si, &pi)
        == 0)
    {
        ngx_log_error(NGX_LOG_WARN, cycle->log, ngx_errno,
                      "CreateProcess(\"%s\") with inherited stderr failed, "
                      "retrying without it", ngx_argv[0]);

        ngx_win32_delete_attr_list(attrs);
        ngx_free(attrs);

        goto failed;
    }

    ngx_win32_delete_attr_list(attrs);
    ngx_free(attrs);

    if (inherit) {
        (void) SetHandleInformation(hstderr, HANDLE_FLAG_INHERIT, 0);
    }

    goto created;

failed:

    if (inherit) {
        (void) SetHandleInformation(hstderr, HANDLE_FLAG_INHERIT, 0);
    }

wrong_handles:

    /*
     * Nothing of the above is fatal: without a usable stderr, or without
     * the attribute API, the process is created exactly as it used to be,
     * with the standard handles simply left unset.
     */

    ngx_memzero(&si, sizeof(ngx_win32_startupinfoex_t));

    si.StartupInfo.cb = sizeof(STARTUPINFO);

    if (CreateProcess(ctx->path, ctx->args, NULL, NULL, 0, CREATE_NO_WINDOW,
                      NULL, NULL, (STARTUPINFO *) &si, &pi)
        == 0)
    {
        ngx_log_error(NGX_LOG_CRIT, cycle->log, ngx_errno,
                      "CreateProcess(\"%s\") failed", ngx_argv[0]);

        return 0;
    }

created:

    ctx->child = pi.hProcess;

    if (CloseHandle(pi.hThread) == 0) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "CloseHandle(pi.hThread) failed");
    }

    ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                  "start %s process %P", ctx->name, pi.dwProcessId);

    return pi.dwProcessId;
}
