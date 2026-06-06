#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <signal.h>

#include "php_flames_async.h"
#include "ext/standard/php_var.h"
#include "ext/spl/spl_exceptions.h"
#include "main/php_streams.h"
#include "main/php_network.h"
#include "zend_smart_str.h"
#include "zend_exceptions.h"

#define ASYNC_STATUS_OK    ((char)0x01)
#define ASYNC_STATUS_ERROR ((char)0x02)

/* PHP 8.5 removed zend_call_method_with_N_params macros. Use zend_call_method. */
#define FLAMES_CALL0(obj, ce, proxy, name, ret) \
    zend_call_method((obj), (ce), (proxy), (name), sizeof(name)-1, (ret), 0, NULL, NULL)
#define FLAMES_CALL1(obj, ce, proxy, name, ret, a1) \
    zend_call_method((obj), (ce), (proxy), (name), sizeof(name)-1, (ret), 1, (a1), NULL)

/* Suppress warn_unused_result on write() calls inside the child process.
 * The child is about to _exit() regardless, so there is nothing to recover. */
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
static __attribute__((noinline)) void flames_write_discard(int fd, const void *buf, size_t n)
{
    write(fd, buf, n);
}
#pragma GCC diagnostic pop
#else
static void flames_write_discard(int fd, const void *buf, size_t n)
{
    (void)write(fd, buf, n);
}
#endif

/* ---- Async I/O hooks (Swoole-style TCP/SSL stream interception) ---- */

static bool flames_async_hooks_enabled = 1;
static int  g_flames_hooks_installed   = 0;

/* Set to true while a Fiber managed by Service::async() is executing.
 * Avoids depending on any PHP-version-specific internal fiber field. */
static bool g_flames_fiber_executing   = false;

/*
 * PHP 8.5 removed Fiber::getSuspendedValue().  The suspended value is now
 * only available as the return of start() / resume() / throw().
 * We store it here after every fiber call so that flames_process_fiber_step()
 * can read it without calling the removed method.
 */
static zval g_flames_last_suspended_value;

/* Wrappers that maintain g_flames_fiber_executing around every start/resume/throw
 * and capture the suspended value into g_flames_last_suspended_value. */
static inline void flames_fiber_start(zend_object *fib_obj, zval *ret)
{
    g_flames_fiber_executing = true;
    FLAMES_CALL0(fib_obj, zend_ce_fiber, NULL, "start", ret);
    g_flames_fiber_executing = false;
    zval_ptr_dtor(&g_flames_last_suspended_value);
    ZVAL_COPY(&g_flames_last_suspended_value, ret);
}

static inline void flames_fiber_resume(zend_object *fib_obj, zval *ret, zval *val)
{
    g_flames_fiber_executing = true;
    FLAMES_CALL1(fib_obj, zend_ce_fiber, NULL, "resume", ret, val);
    g_flames_fiber_executing = false;
    zval_ptr_dtor(&g_flames_last_suspended_value);
    ZVAL_COPY(&g_flames_last_suspended_value, ret);
}

static inline void flames_fiber_throw_obj(zend_object *fib_obj, zval *ret, zval *ex)
{
    g_flames_fiber_executing = true;
    FLAMES_CALL1(fib_obj, zend_ce_fiber, NULL, "throw", ret, ex);
    g_flames_fiber_executing = false;
    zval_ptr_dtor(&g_flames_last_suspended_value);
    ZVAL_COPY(&g_flames_last_suspended_value, ret);
}

static php_stream_ops          g_flames_hook_sock_ops;
static const php_stream_ops   *g_flames_socket_ops_ptr  = NULL;
static ssize_t (*flames_orig_sock_read)(php_stream *stream, char *buf, size_t count);
static ssize_t (*flames_orig_sock_write)(php_stream *stream, const char *buf, size_t count);

static php_stream_ops          g_flames_hook_ssl_ops;
static const php_stream_ops   *g_flames_openssl_ops_ptr = NULL;
static ssize_t (*flames_orig_ssl_read)(php_stream *stream, char *buf, size_t count);
static ssize_t (*flames_orig_ssl_write)(php_stream *stream, const char *buf, size_t count);
static int g_flames_ssl_hooks_ready = 0;

static int  flames_igbinary_checked   = 0;
static bool flames_igbinary_available = false;

static void flames_detect_igbinary(void)
{
    if (flames_igbinary_checked) {
        return;
    }
    flames_igbinary_available = zend_hash_str_exists(
        EG(function_table),
        "igbinary_serialize",
        sizeof("igbinary_serialize") - 1);
    flames_igbinary_checked = 1;
}

static void flames_serialize(zval *value, smart_str *buf)
{
    flames_detect_igbinary();

    if (flames_igbinary_available) {
        zval fn_name, retval, arg;
        ZVAL_STRING(&fn_name, "igbinary_serialize");
        ZVAL_COPY(&arg, value);

        if (call_user_function(NULL, NULL, &fn_name, &retval, 1, &arg) == SUCCESS
            && Z_TYPE(retval) == IS_STRING)
        {
            smart_str_appendl(buf, Z_STRVAL(retval), Z_STRLEN(retval));
        }

        zval_ptr_dtor(&fn_name);
        zval_ptr_dtor(&arg);
        zval_ptr_dtor(&retval);
    } else {
        php_serialize_data_t var_hash;
        PHP_VAR_SERIALIZE_INIT(var_hash);
        php_var_serialize(buf, value, &var_hash);
        PHP_VAR_SERIALIZE_DESTROY(var_hash);
    }
}

static int flames_unserialize(zval *result, const char *data, size_t len)
{
    flames_detect_igbinary();

    if (flames_igbinary_available) {
        zval fn_name, arg;
        ZVAL_STRING(&fn_name, "igbinary_unserialize");
        ZVAL_STRINGL(&arg, data, len);

        int ret = call_user_function(NULL, NULL, &fn_name, result, 1, &arg);

        zval_ptr_dtor(&fn_name);
        zval_ptr_dtor(&arg);
        return ret;
    } else {
        php_unserialize_data_t var_hash;
        const unsigned char *p   = (const unsigned char *)data;
        const unsigned char *end = p + len;

        ZVAL_UNDEF(result);
        PHP_VAR_UNSERIALIZE_INIT(var_hash);
        int ok = php_var_unserialize(result, &p, end, &var_hash);
        PHP_VAR_UNSERIALIZE_DESTROY(var_hash);
        return ok ? SUCCESS : FAILURE;
    }
}

static zend_object_handlers flames_async_task_handlers;

zend_class_entry *flames_async_service_ce;
zend_class_entry *flames_async_task_ce;

static zend_object *flames_async_task_create_object(zend_class_entry *ce)
{
    flames_async_task *task = ecalloc(
        1, sizeof(flames_async_task) + zend_object_properties_size(ce));

    ZVAL_UNDEF(&task->result);
    task->pipe_read_fd = -1;
    task->child_pid    = -1;
    task->done         = 0;
    task->has_error    = 0;
    task->error_msg    = NULL;

    zend_object_std_init(&task->std, ce);
    object_properties_init(&task->std, ce);
    task->std.handlers = &flames_async_task_handlers;

    return &task->std;
}

static void flames_async_task_free_object(zend_object *obj)
{
    flames_async_task *task = flames_async_task_from_obj(obj);

    if (task->pipe_read_fd >= 0) {
        close(task->pipe_read_fd);
        task->pipe_read_fd = -1;
    }

    if (!task->done && task->child_pid > 0) {
        kill(task->child_pid, SIGKILL);
        waitpid(task->child_pid, NULL, 0);
        task->child_pid = -1;
    }

    if (!Z_ISUNDEF(task->result)) {
        zval_ptr_dtor(&task->result);
        ZVAL_UNDEF(&task->result);
    }

    if (task->error_msg) {
        zend_string_release(task->error_msg);
        task->error_msg = NULL;
    }

    zend_object_std_dtor(obj);
}

static void flames_async_collect(flames_async_task *task)
{
    if (task->done) {
        return;
    }

    if (task->pipe_read_fd < 0) {
        ZVAL_NULL(&task->result);
        task->done = 1;
        return;
    }

    smart_str buf = {0};
    char      tmp[8192];
    ssize_t   n;

    while ((n = read(task->pipe_read_fd, tmp, sizeof(tmp))) > 0) {
        smart_str_appendl(&buf, tmp, (size_t)n);
    }

    close(task->pipe_read_fd);
    task->pipe_read_fd = -1;

    waitpid(task->child_pid, NULL, 0);
    task->child_pid = -1;
    task->done      = 1;

    if (!buf.s || ZSTR_LEN(buf.s) == 0) {
        task->has_error = 1;
        task->error_msg = zend_string_init(
            "fork: child process terminated without a result", 47, 0);
        ZVAL_NULL(&task->result);
        smart_str_free(&buf);
        return;
    }

    const char *raw    = ZSTR_VAL(buf.s);
    size_t      rawlen = ZSTR_LEN(buf.s);
    char        status = raw[0];

    if (status == ASYNC_STATUS_OK && rawlen > 1) {
        if (flames_unserialize(&task->result, raw + 1, rawlen - 1) != SUCCESS
            || Z_ISUNDEF(task->result))
        {
            ZVAL_FALSE(&task->result);
            task->has_error = 1;
            task->error_msg = zend_string_init(
                "fork: failed to unserialize child return value", 46, 0);
        }
    } else if (status == ASYNC_STATUS_ERROR && rawlen > 1) {
        ZVAL_NULL(&task->result);
        task->has_error = 1;
        task->error_msg = zend_string_init(raw + 1, rawlen - 1, 0);
    } else {
        ZVAL_NULL(&task->result);
    }

    smart_str_free(&buf);
}

PHP_METHOD(FlamesAsyncServiceTask, isDone)
{
    ZEND_PARSE_PARAMETERS_NONE();

    flames_async_task *task = FLAMES_ASYNC_TASK_FROM_ZVAL(getThis());

    if (task->done) {
        RETURN_TRUE;
    }

    if (task->pipe_read_fd < 0) {
        RETURN_FALSE;
    }

    fd_set         rset;
    struct timeval tv = {0, 0};
    FD_ZERO(&rset);
    FD_SET(task->pipe_read_fd, &rset);

    if (select(task->pipe_read_fd + 1, &rset, NULL, NULL, &tv) > 0
        && FD_ISSET(task->pipe_read_fd, &rset))
    {
        flames_async_collect(task);
        RETURN_TRUE;
    }

    RETURN_FALSE;
}

PHP_METHOD(FlamesAsyncServiceTask, result)
{
    ZEND_PARSE_PARAMETERS_NONE();

    flames_async_task *task = FLAMES_ASYNC_TASK_FROM_ZVAL(getThis());
    flames_async_collect(task);

    if (task->has_error && task->error_msg) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0,
            "%s", ZSTR_VAL(task->error_msg));
        return;
    }

    ZVAL_COPY(return_value, &task->result);
}

PHP_METHOD(FlamesAsyncService, fork)
{
    zval *callable;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(callable)
    ZEND_PARSE_PARAMETERS_END();

    object_init_ex(return_value, flames_async_task_ce);
    flames_async_task *task = FLAMES_ASYNC_TASK_FROM_ZVAL(return_value);

    int fds[2];
    if (pipe(fds) != 0) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0,
            "Flames\\Async\\Async\\Service::fork(): pipe() failed: %s", strerror(errno));
        return;
    }

    pid_t pid = fork();

    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        zend_throw_exception_ex(spl_ce_RuntimeException, 0,
            "Flames\\Async\\Async\\Service::fork(): fork() failed: %s", strerror(errno));
        return;
    }

    if (pid == 0) {
        close(fds[0]);

        if (EG(exception)) {
            zend_clear_exception();
        }

        zval retval;
        ZVAL_UNDEF(&retval);

        int call_ok = (call_user_function(NULL, NULL, callable, &retval, 0, NULL) == SUCCESS)
                      && !EG(exception);

        if (call_ok) {
            smart_str sbuf = {0};
            flames_serialize(&retval, &sbuf);
            zval_ptr_dtor(&retval);

            char status = ASYNC_STATUS_OK;
            flames_write_discard(fds[1], &status, 1);

            if (sbuf.s && ZSTR_LEN(sbuf.s) > 0) {
                const char *p   = ZSTR_VAL(sbuf.s);
                size_t      rem = ZSTR_LEN(sbuf.s);
                while (rem > 0) {
                    ssize_t w = write(fds[1], p, rem);
                    if (w <= 0) break;
                    p   += (size_t)w;
                    rem -= (size_t)w;
                }
                smart_str_free(&sbuf);
            }

        } else {
            const char  *errmsg = "Unknown error in fork closure";
            size_t       errlen = 30;
            zend_string *exc_str = NULL;

            if (EG(exception)) {
                zval rv;
                zval *prop = zend_read_property(
                    EG(exception)->ce, EG(exception),
                    "message", sizeof("message") - 1, 1, &rv);

                if (prop && Z_TYPE_P(prop) == IS_STRING) {
                    exc_str = zend_string_copy(Z_STR_P(prop));
                    errmsg  = ZSTR_VAL(exc_str);
                    errlen  = ZSTR_LEN(exc_str);
                }
                zend_clear_exception();
            }

            char status = ASYNC_STATUS_ERROR;
            flames_write_discard(fds[1], &status, 1);
            flames_write_discard(fds[1], errmsg, errlen);

            if (exc_str) {
                zend_string_release(exc_str);
            }
        }

        close(fds[1]);
        _exit(0);
    }

    close(fds[1]);
    task->pipe_read_fd = fds[0];
    task->child_pid    = pid;
}

PHP_METHOD(FlamesAsyncService, wait)
{
    zval *tasks     = NULL;
    int   num_tasks = 0;

    ZEND_PARSE_PARAMETERS_START(1, -1)
        Z_PARAM_VARIADIC('+', tasks, num_tasks)
    ZEND_PARSE_PARAMETERS_END();

    for (int i = 0; i < num_tasks; i++) {
        if (Z_TYPE(tasks[i]) != IS_OBJECT
            || !instanceof_function(Z_OBJCE(tasks[i]), flames_async_task_ce))
        {
            zend_throw_exception_ex(zend_ce_type_error, 0,
                "Flames\\Async\\Async\\Service::wait(): argument %d must be an instance of "
                "Flames\\Async\\Async\\Service\\Task, %s given",
                i + 1,
                Z_TYPE(tasks[i]) == IS_OBJECT
                    ? ZSTR_VAL(Z_OBJCE(tasks[i])->name)
                    : zend_get_type_by_const(Z_TYPE(tasks[i])));
            return;
        }
    }

    if (num_tasks == 1) {
        flames_async_task *task = FLAMES_ASYNC_TASK_FROM_ZVAL(&tasks[0]);
        flames_async_collect(task);

        if (task->has_error && task->error_msg) {
            zend_throw_exception_ex(spl_ce_RuntimeException, 0,
                "%s", ZSTR_VAL(task->error_msg));
            return;
        }

        ZVAL_COPY(return_value, &task->result);

    } else {
        array_init(return_value);

        for (int i = 0; i < num_tasks; i++) {
            flames_async_task *task = FLAMES_ASYNC_TASK_FROM_ZVAL(&tasks[i]);
            flames_async_collect(task);

            if (task->has_error && task->error_msg) {
                zend_throw_exception_ex(spl_ce_RuntimeException, 0,
                    "fork[%d]: %s", i, ZSTR_VAL(task->error_msg));
                return;
            }

            zval copy;
            ZVAL_COPY(&copy, &task->result);
            add_next_index_zval(return_value, &copy);
        }
    }
}

/* ====================================================================
 * ASYNC / AWAIT  –  PHP Fibre-based cooperative concurrency engine
 * ====================================================================
 *
 * Architecture
 * ------------
 *  Service::async(callable $fn) : Promise
 *      Wraps $fn in a PHP Fiber, starts it immediately.
 *      Returns an outer "completion" Promise that resolves when the
 *      Fiber returns (or rejects when it throws uncaught).
 *
 *  Service::await(Promise $p) : mixed
 *      - Inside a Fiber : calls Fiber::suspend($p), handing $p to the
 *        scheduler.  Returns when the scheduler calls $fiber->resume().
 *      - Main thread    : drives the event loop until $p settles.
 *
 *  Service::run() : void
 *      Drives the epoll event loop until all pending coroutines finish.
 *
 *  Promise::forFd(int $fd) : static
 *      Create a Promise backed by a file descriptor.  The event loop
 *      calls read() on $fd when epoll signals EPOLLIN, resolves the
 *      Promise with the raw bytes, and resumes the waiting Fiber.
 *
 *  Promise::resolve(mixed $value) / Promise::reject(string $msg)
 *      Allow external code (I/O drivers, timers, …) to settle a
 *      Promise manually.  The event loop polls them every tick.
 *
 * Bridge (C motor ↔ Fiber)
 * ------------------------
 *  await() calls Fiber::suspend($promise).  The scheduler detects the
 *  suspension via $fiber->getSuspendedValue(), finds the Promise and
 *  its fd, and registers (fd → Fiber) in epoll.  When epoll fires it
 *  reads the fd, resolves the Promise, and calls $fiber->resume($value)
 *  or $fiber->throw($exception).  Fiber::suspend() then returns $value
 *  inside await(), which returns it transparently to user code.
 *
 * Error propagation
 * -----------------
 *  Any uncaught exception inside a Fiber rejects the outer Promise.
 *  await() re-throws the rejection as a RuntimeException, preserving a
 *  clean stack trace visible to the user.
 * ==================================================================== */

#define FLAMES_EPOLL_MAX_EVENTS 64
#define FLAMES_MAX_WAITERS      512
#define FLAMES_PROMISE_PENDING  0
#define FLAMES_PROMISE_RESOLVED 1
#define FLAMES_PROMISE_REJECTED 2

/* ---- Promise object ---- */

static zend_object_handlers flames_async_promise_handlers;
zend_class_entry           *flames_async_promise_ce;

static zend_object *flames_async_promise_create_object(zend_class_entry *ce)
{
    flames_async_promise *p = ecalloc(
        1, sizeof(flames_async_promise) + zend_object_properties_size(ce));

    p->fd           = -1;
    p->state        = FLAMES_PROMISE_PENDING;
    p->wait_mode    = FLAMES_PROMISE_WAIT_READ;
    p->epoll_events = EPOLLIN;
    ZVAL_UNDEF(&p->result);
    p->error_msg    = NULL;

    zend_object_std_init(&p->std, ce);
    object_properties_init(&p->std, ce);
    p->std.handlers = &flames_async_promise_handlers;

    return &p->std;
}

static void flames_async_promise_free_object(zend_object *obj)
{
    flames_async_promise *p = flames_async_promise_from_obj(obj);
    if (!Z_ISUNDEF(p->result)) { zval_ptr_dtor(&p->result); }
    if (p->error_msg)          { zend_string_release(p->error_msg); }
    zend_object_std_dtor(obj);
}

/* ---- Event loop state ---- */

typedef struct {
    int  fd;
    int  close_fd;   /* 1 = this fd is owned (timerfd), close when removing */
    zval fiber;
    zval inner_promise;
    zval outer_promise;
} flames_waiter_t;

static int             g_epoll_fd     = -1;
static flames_waiter_t g_waiters[FLAMES_MAX_WAITERS];
static int             g_waiter_count = 0;

static void flames_loop_init(void)
{
    if (g_epoll_fd >= 0) { return; }
    g_epoll_fd     = epoll_create1(EPOLL_CLOEXEC);
    g_waiter_count = 0;
    ZVAL_NULL(&g_flames_last_suspended_value);
}

static void flames_loop_destroy(void)
{
    for (int i = 0; i < g_waiter_count; i++) {
        if (g_waiters[i].fd >= 0) {
            epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, g_waiters[i].fd, NULL);
        }
        zval_ptr_dtor(&g_waiters[i].fiber);
        zval_ptr_dtor(&g_waiters[i].inner_promise);
        zval_ptr_dtor(&g_waiters[i].outer_promise);
    }
    g_waiter_count = 0;
    zval_ptr_dtor(&g_flames_last_suspended_value);
    ZVAL_NULL(&g_flames_last_suspended_value);
    if (g_epoll_fd >= 0) { close(g_epoll_fd); g_epoll_fd = -1; }
}

static void flames_loop_add_waiter(int fd, zval *fiber, zval *inner_p, zval *outer_p)
{
    if (g_waiter_count >= FLAMES_MAX_WAITERS) { return; }

    unsigned int epoll_events = EPOLLIN;
    int          close_fd     = 0;
    if (inner_p && Z_TYPE_P(inner_p) == IS_OBJECT
        && instanceof_function(Z_OBJCE_P(inner_p), flames_async_promise_ce))
    {
        flames_async_promise *pp = FLAMES_ASYNC_PROMISE_FROM_ZVAL(inner_p);
        epoll_events = pp->epoll_events;
        close_fd     = pp->close_fd;
    }

    if (fd >= 0) {
        struct epoll_event ev;
        ev.events  = epoll_events | EPOLLONESHOT;
        ev.data.fd = fd;
        epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    }

    int i = g_waiter_count++;
    g_waiters[i].fd       = fd;
    g_waiters[i].close_fd = close_fd;
    ZVAL_COPY(&g_waiters[i].fiber,         fiber);
    ZVAL_COPY(&g_waiters[i].inner_promise, inner_p);
    ZVAL_COPY(&g_waiters[i].outer_promise, outer_p);
}

static void flames_loop_remove_waiter(int idx)
{
    if (g_waiters[idx].fd >= 0) {
        epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, g_waiters[idx].fd, NULL);
        if (g_waiters[idx].close_fd) {
            close(g_waiters[idx].fd);
        }
    }
    zval_ptr_dtor(&g_waiters[idx].fiber);
    zval_ptr_dtor(&g_waiters[idx].inner_promise);
    zval_ptr_dtor(&g_waiters[idx].outer_promise);

    int last = --g_waiter_count;
    if (idx != last) {
        g_waiters[idx] = g_waiters[last];
    }
}

/* Inject a RuntimeException into a suspended Fiber */
static void flames_fiber_throw_msg(zval *fiber, const char *msg, size_t mlen)
{
    zval ex, arg, ret;
    object_init_ex(&ex, spl_ce_RuntimeException);
    ZVAL_STRINGL(&arg, msg, mlen);
    FLAMES_CALL1(Z_OBJ(ex), spl_ce_RuntimeException, NULL, "__construct", NULL, &arg);
    zval_ptr_dtor(&arg);
    flames_fiber_throw_obj(Z_OBJ_P(fiber), &ret, &ex);
    zval_ptr_dtor(&ex);
    zval_ptr_dtor(&ret);
}

/*
 * Called after $fiber->start() or $fiber->resume().
 * Handles three outcomes:
 *   1. Fiber threw an uncaught exception  → reject outer_promise
 *   2. Fiber terminated normally          → resolve outer_promise
 *   3. Fiber suspended on a Promise       → register waiter in epoll
 */
static void flames_process_fiber_step(zval *fiber, zval *outer_promise)
{
    flames_async_promise *op = FLAMES_ASYNC_PROMISE_FROM_ZVAL(outer_promise);

    if (EG(exception)) {
        if (op->state == FLAMES_PROMISE_PENDING) {
            zval *prop, rv;
            prop = zend_read_property(EG(exception)->ce, EG(exception),
                "message", sizeof("message") - 1, 1, &rv);
            op->error_msg = (prop && Z_TYPE_P(prop) == IS_STRING)
                ? zend_string_copy(Z_STR_P(prop))
                : zend_string_init("Unhandled exception in coroutine", 31, 0);
            op->state = FLAMES_PROMISE_REJECTED;
        }
        zend_clear_exception();
        return;
    }

    zval is_term;
    FLAMES_CALL0(Z_OBJ_P(fiber), zend_ce_fiber, NULL, "isTerminated", &is_term);

    if (Z_TYPE(is_term) == IS_TRUE) {
        if (op->state == FLAMES_PROMISE_PENDING) {
            zval ret;
            FLAMES_CALL0(Z_OBJ_P(fiber), zend_ce_fiber, NULL, "getReturn", &ret);
            ZVAL_COPY(&op->result, &ret);
            op->state = FLAMES_PROMISE_RESOLVED;
            zval_ptr_dtor(&ret);
        }
        zval_ptr_dtor(&is_term);
        return;
    }
    zval_ptr_dtor(&is_term);

    /* Fiber suspended — read the Promise from g_flames_last_suspended_value.
     * (Fiber::getSuspendedValue() was removed in PHP 8.5; the suspended value
     *  is the return of start() / resume() / throw(), already captured above.) */
    zval inner;
    ZVAL_COPY(&inner, &g_flames_last_suspended_value);

    if (Z_TYPE(inner) != IS_OBJECT
        || !instanceof_function(Z_OBJCE(inner), flames_async_promise_ce))
    {
        /* Unknown suspend value — resume with null and recurse */
        zval nv, ret;
        ZVAL_NULL(&nv);
        flames_fiber_resume(Z_OBJ_P(fiber), &ret, &nv);
        zval_ptr_dtor(&ret);
        zval_ptr_dtor(&inner);
        flames_process_fiber_step(fiber, outer_promise);
        return;
    }

    flames_async_promise *ip = FLAMES_ASYNC_PROMISE_FROM_ZVAL(&inner);

    if (ip->state == FLAMES_PROMISE_RESOLVED) {
        zval ret;
        flames_fiber_resume(Z_OBJ_P(fiber), &ret, &ip->result);
        zval_ptr_dtor(&ret);
        zval_ptr_dtor(&inner);
        flames_process_fiber_step(fiber, outer_promise);
        return;
    }

    if (ip->state == FLAMES_PROMISE_REJECTED) {
        const char *msg  = ip->error_msg ? ZSTR_VAL(ip->error_msg) : "Promise rejected";
        size_t      mlen = ip->error_msg ? ZSTR_LEN(ip->error_msg) : 16;
        flames_fiber_throw_msg(fiber, msg, mlen);
        zval_ptr_dtor(&inner);
        flames_process_fiber_step(fiber, outer_promise);
        return;
    }

    /* Pending — register fiber+promise in the event loop */
    flames_loop_add_waiter(ip->fd, fiber, &inner, outer_promise);
    zval_ptr_dtor(&inner);
}

/* Read all available bytes from fd and resolve the Promise */
static void flames_resolve_fd_promise(flames_async_promise *p, int fd)
{
    smart_str buf = {0};
    char      tmp[8192];
    ssize_t   r;

    while ((r = read(fd, tmp, sizeof(tmp))) > 0) {
        smart_str_appendl(&buf, tmp, (size_t)r);
    }

    if (buf.s && ZSTR_LEN(buf.s) > 0) {
        ZVAL_STR_COPY(&p->result, buf.s);
    } else {
        ZVAL_NULL(&p->result);
    }
    p->state = FLAMES_PROMISE_RESOLVED;
    smart_str_free(&buf);
}

/* One iteration of the event loop */
static void flames_loop_tick(int timeout_ms)
{
    if (g_epoll_fd < 0 || g_waiter_count == 0) { return; }

    /* Drain manually-resolved (fd == -1) Promises */
    for (int i = g_waiter_count - 1; i >= 0; i--) {
        if (g_waiters[i].fd >= 0) { continue; }

        flames_async_promise *ip = FLAMES_ASYNC_PROMISE_FROM_ZVAL(
            &g_waiters[i].inner_promise);
        if (ip->state == FLAMES_PROMISE_PENDING) { continue; }

        zval fib = g_waiters[i].fiber;
        zval out = g_waiters[i].outer_promise;
        Z_ADDREF(fib); Z_ADDREF(out);
        flames_loop_remove_waiter(i);

        if (ip->state == FLAMES_PROMISE_RESOLVED) {
            zval ret;
            flames_fiber_resume(Z_OBJ(fib), &ret, &ip->result);
            zval_ptr_dtor(&ret);
        } else {
            const char *msg  = ip->error_msg ? ZSTR_VAL(ip->error_msg) : "Promise rejected";
            size_t      mlen = ip->error_msg ? ZSTR_LEN(ip->error_msg) : 16;
            flames_fiber_throw_msg(&fib, msg, mlen);
        }

        flames_process_fiber_step(&fib, &out);
        zval_ptr_dtor(&fib);
        zval_ptr_dtor(&out);
        if (EG(exception)) { return; }
    }

    /* Wait for fd-backed I/O events */
    int has_fd = 0;
    for (int i = 0; i < g_waiter_count; i++) {
        if (g_waiters[i].fd >= 0) { has_fd = 1; break; }
    }
    if (!has_fd) { return; }

    struct epoll_event events[FLAMES_EPOLL_MAX_EVENTS];
    int n = epoll_wait(g_epoll_fd, events, FLAMES_EPOLL_MAX_EVENTS, timeout_ms);

    for (int e = 0; e < n; e++) {
        int fd  = events[e].data.fd;
        int idx = -1;
        for (int i = 0; i < g_waiter_count; i++) {
            if (g_waiters[i].fd == fd) { idx = i; break; }
        }
        if (idx < 0) { continue; }

        flames_async_promise *ip = FLAMES_ASYNC_PROMISE_FROM_ZVAL(
            &g_waiters[idx].inner_promise);

        if (ip->state == FLAMES_PROMISE_PENDING) {
            if (ip->wait_mode == FLAMES_PROMISE_WAIT_READY) {
                ZVAL_TRUE(&ip->result);
                ip->state = FLAMES_PROMISE_RESOLVED;
            } else {
                flames_resolve_fd_promise(ip, fd);
            }
        }

        zval fib = g_waiters[idx].fiber;
        zval out = g_waiters[idx].outer_promise;
        Z_ADDREF(fib); Z_ADDREF(out);
        flames_loop_remove_waiter(idx);

        if (ip->state == FLAMES_PROMISE_RESOLVED) {
            zval ret;
            flames_fiber_resume(Z_OBJ(fib), &ret, &ip->result);
            zval_ptr_dtor(&ret);
        } else {
            const char *msg  = ip->error_msg ? ZSTR_VAL(ip->error_msg) : "I/O error";
            size_t      mlen = ip->error_msg ? ZSTR_LEN(ip->error_msg) : 9;
            flames_fiber_throw_msg(&fib, msg, mlen);
        }

        flames_process_fiber_step(&fib, &out);
        zval_ptr_dtor(&fib);
        zval_ptr_dtor(&out);
        if (EG(exception)) { return; }
    }
}

/* ====================================================================
 * ASYNC I/O HOOKS  –  automatic non-blocking TCP/SSL inside Fibers
 * ==================================================================== */

static bool flames_hooks_active(void)
{
    return flames_async_hooks_enabled && g_flames_fiber_executing;
}

static int flames_stream_get_fd(php_stream *stream)
{
    php_socket_t fd = -1;

    if (!stream) {
        return -1;
    }

    if (php_stream_cast(stream,
            PHP_STREAM_AS_FD_FOR_SELECT | PHP_STREAM_CAST_INTERNAL,
            (void *)&fd, 1) != SUCCESS)
    {
        return -1;
    }

    return (int)fd;
}

static void flames_stream_set_nonblock(php_stream *stream)
{
    int fd;

    if (!stream) {
        return;
    }

    php_stream_set_option(stream, PHP_STREAM_OPTION_BLOCKING, 0, NULL);

    fd = flames_stream_get_fd(stream);
    if (fd >= 0) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
    }
}

static bool flames_stream_would_block(php_stream *stream)
{
    if (!stream || stream->eof) {
        return false;
    }

    return errno == EAGAIN || errno == EWOULDBLOCK;
}

static void flames_stream_ensure_hooked(php_stream *stream)
{
    if (!stream) {
        return;
    }

    if (stream->ops == (const php_stream_ops *)&g_flames_hook_sock_ops
        || (g_flames_ssl_hooks_ready
            && stream->ops == (const php_stream_ops *)&g_flames_hook_ssl_ops))
    {
        return;
    }

    if (g_flames_socket_ops_ptr && stream->ops == g_flames_socket_ops_ptr) {
        stream->ops = (const php_stream_ops *)&g_flames_hook_sock_ops;
        flames_stream_set_nonblock(stream);
        return;
    }

    if (g_flames_ssl_hooks_ready && g_flames_openssl_ops_ptr
        && stream->ops == g_flames_openssl_ops_ptr)
    {
        stream->ops = (const php_stream_ops *)&g_flames_hook_ssl_ops;
        flames_stream_set_nonblock(stream);
    }
}

/* ---- Async sleep via timerfd ---- */

/*
 * Original handlers for sleep() and usleep() – saved during MINIT so we
 * can call through to the real implementation when outside a coroutine.
 */
static zif_handler orig_sleep_handler  = NULL;
static zif_handler orig_usleep_handler = NULL;

/*
 * Core async-sleep: create a timerfd, register it with epoll via a Promise,
 * and suspend the current Fiber until the timer fires.
 * Falls back to blocking usleep() if timerfd is unavailable.
 */
static void flames_do_async_sleep(double seconds)
{
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0) {
        /* timerfd unavailable – fall back to blocking */
        useconds_t usec = (useconds_t)(seconds * 1e6);
        if (usec > 0) { usleep(usec); }
        return;
    }

    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec  = (time_t)seconds;
    its.it_value.tv_nsec = (long)((seconds - (time_t)seconds) * 1000000000L);
    if (its.it_value.tv_sec == 0 && its.it_value.tv_nsec == 0) {
        its.it_value.tv_nsec = 1; /* minimum 1 ns */
    }
    timerfd_settime(tfd, 0, &its, NULL);

    flames_loop_init();

    zval promise_zv, fn, suspend_ret;
    object_init_ex(&promise_zv, flames_async_promise_ce);
    flames_async_promise *p = FLAMES_ASYNC_PROMISE_FROM_ZVAL(&promise_zv);
    p->fd           = tfd;
    p->wait_mode    = FLAMES_PROMISE_WAIT_READY;
    p->epoll_events = EPOLLIN;
    p->close_fd     = 1; /* timerfd is owned – close after epoll fires */

    ZVAL_STRING(&fn, "Fiber::suspend");
    call_user_function(NULL, NULL, &fn, &suspend_ret, 1, &promise_zv);
    zval_ptr_dtor(&fn);
    zval_ptr_dtor(&suspend_ret);
    zval_ptr_dtor(&promise_zv);
}

/* flames_async_sleep(float $seconds): void – callable from PHP */
PHP_FUNCTION(flames_async_sleep)
{
    double seconds;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_DOUBLE(seconds)
    ZEND_PARSE_PARAMETERS_END();

    if (seconds <= 0.0) { return; }

    if (g_flames_fiber_executing) {
        flames_do_async_sleep(seconds);
    } else {
        useconds_t usec = (useconds_t)(seconds * 1e6);
        if (usec > 0) { usleep(usec); }
    }
}

/* Replacement for sleep() – non-blocking inside a managed coroutine */
PHP_FUNCTION(flames_patched_sleep)
{
    if (!g_flames_fiber_executing) {
        orig_sleep_handler(INTERNAL_FUNCTION_PARAM_PASSTHRU);
        return;
    }
    zend_long secs;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(secs)
    ZEND_PARSE_PARAMETERS_END();
    if (secs > 0) { flames_do_async_sleep((double)secs); }
    RETURN_LONG(0);
}

/* Replacement for usleep() – non-blocking inside a managed coroutine */
PHP_FUNCTION(flames_patched_usleep)
{
    if (!g_flames_fiber_executing) {
        orig_usleep_handler(INTERNAL_FUNCTION_PARAM_PASSTHRU);
        return;
    }
    zend_long usec;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(usec)
    ZEND_PARSE_PARAMETERS_END();
    if (usec > 0) { flames_do_async_sleep((double)usec / 1e6); }
}

/* Monkey-patch sleep() and usleep() at module startup */
static void flames_patch_sleep_functions(void)
{
    zend_function *fn;

    fn = zend_hash_str_find_ptr(CG(function_table), "sleep", sizeof("sleep") - 1);
    if (fn && fn->type == ZEND_INTERNAL_FUNCTION) {
        orig_sleep_handler = fn->internal_function.handler;
        fn->internal_function.handler = zif_flames_patched_sleep;
    }

    fn = zend_hash_str_find_ptr(CG(function_table), "usleep", sizeof("usleep") - 1);
    if (fn && fn->type == ZEND_INTERNAL_FUNCTION) {
        orig_usleep_handler = fn->internal_function.handler;
        fn->internal_function.handler = zif_flames_patched_usleep;
    }
}

/*
 * Suspend the current Fiber until $fd is readable/writable.
 * The scheduler (process_fiber_step + loop_tick) registers the fd in
 * epoll and resumes the Fiber when the event fires.
 */
static void flames_await_fd(int fd, unsigned int events)
{
    zval promise_zv, fn, suspend_ret;

    if (fd < 0 || !flames_hooks_active()) {
        return;
    }

    flames_loop_init();

    object_init_ex(&promise_zv, flames_async_promise_ce);
    flames_async_promise *p = FLAMES_ASYNC_PROMISE_FROM_ZVAL(&promise_zv);
    p->fd           = fd;
    p->wait_mode    = FLAMES_PROMISE_WAIT_READY;
    p->epoll_events = events;

    ZVAL_STRING(&fn, "Fiber::suspend");
    call_user_function(NULL, NULL, &fn, &suspend_ret, 1, &promise_zv);
    zval_ptr_dtor(&fn);
    zval_ptr_dtor(&suspend_ret);
    zval_ptr_dtor(&promise_zv);
}

static ssize_t flames_hook_stream_read(
    php_stream *stream, char *buf, size_t count,
    ssize_t (*orig_read)(php_stream *, char *, size_t))
{
    if (!flames_hooks_active()) {
        return orig_read(stream, buf, count);
    }

    flames_stream_ensure_hooked(stream);

    for (;;) {
        ssize_t n = orig_read(stream, buf, count);

        if (n > 0) {
            return n;
        }

        if (stream->eof) {
            return n;
        }

        if (!flames_stream_would_block(stream)) {
            return n;
        }

        flames_await_fd(flames_stream_get_fd(stream), EPOLLIN);
    }
}

static ssize_t flames_hook_stream_write(
    php_stream *stream, const char *buf, size_t count,
    ssize_t (*orig_write)(php_stream *, const char *, size_t))
{
    if (!flames_hooks_active()) {
        return orig_write(stream, buf, count);
    }

    flames_stream_ensure_hooked(stream);

    for (;;) {
        ssize_t n = orig_write(stream, buf, count);

        if (n > 0) {
            return n;
        }

        if (!flames_stream_would_block(stream)) {
            return n;
        }

        flames_await_fd(flames_stream_get_fd(stream), EPOLLOUT);
    }
}

static ssize_t flames_hook_sock_read(php_stream *stream, char *buf, size_t count)
{
    return flames_hook_stream_read(stream, buf, count, flames_orig_sock_read);
}

static ssize_t flames_hook_sock_write(php_stream *stream, const char *buf, size_t count)
{
    return flames_hook_stream_write(stream, buf, count, flames_orig_sock_write);
}

static ssize_t flames_hook_ssl_read(php_stream *stream, char *buf, size_t count)
{
    return flames_hook_stream_read(stream, buf, count, flames_orig_ssl_read);
}

static ssize_t flames_hook_ssl_write(php_stream *stream, const char *buf, size_t count)
{
    return flames_hook_stream_write(stream, buf, count, flames_orig_ssl_write);
}

static void flames_hooks_install(void)
{
    if (g_flames_hooks_installed) {
        return;
    }

    g_flames_socket_ops_ptr = (const php_stream_ops *)dlsym(
        RTLD_DEFAULT, "php_stream_socket_ops");

    if (g_flames_socket_ops_ptr) {
        memcpy(&g_flames_hook_sock_ops, g_flames_socket_ops_ptr, sizeof(php_stream_ops));
        flames_orig_sock_read  = g_flames_socket_ops_ptr->read;
        flames_orig_sock_write = g_flames_socket_ops_ptr->write;
        g_flames_hook_sock_ops.read  = flames_hook_sock_read;
        g_flames_hook_sock_ops.write = flames_hook_sock_write;
    }

    g_flames_openssl_ops_ptr = (const php_stream_ops *)dlsym(
        RTLD_DEFAULT, "php_openssl_socket_ops");

    if (g_flames_openssl_ops_ptr) {
        memcpy(&g_flames_hook_ssl_ops, g_flames_openssl_ops_ptr, sizeof(php_stream_ops));
        flames_orig_ssl_read  = g_flames_openssl_ops_ptr->read;
        flames_orig_ssl_write = g_flames_openssl_ops_ptr->write;
        g_flames_hook_ssl_ops.read  = flames_hook_ssl_read;
        g_flames_hook_ssl_ops.write = flames_hook_ssl_write;
        g_flames_ssl_hooks_ready    = 1;
    }

    g_flames_hooks_installed = 1;
}




static PHP_INI_MH(OnUpdateFlamesAsyncHooks)
{
    flames_async_hooks_enabled = zend_ini_parse_bool(new_value);
    if (flames_async_hooks_enabled) {
        flames_hooks_install();
    }
    return SUCCESS;
}

PHP_INI_BEGIN()
    PHP_INI_ENTRY("flames.async_hooks", "1", PHP_INI_ALL, OnUpdateFlamesAsyncHooks)
PHP_INI_END()

PHP_FUNCTION(flames_stream_to_fd)
{
    zval       *zstream;
    php_stream *stream;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_RESOURCE(zstream)
    ZEND_PARSE_PARAMETERS_END();

    php_stream_from_zval(stream, zstream);
    if (!stream) {
        RETURN_LONG(-1);
    }

    RETURN_LONG(flames_stream_get_fd(stream));
}

/* ---- Promise PHP methods ---- */

PHP_METHOD(FlamesAsyncPromise, forFd)
{
    zend_long fd;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(fd)
    ZEND_PARSE_PARAMETERS_END();

    object_init_ex(return_value, flames_async_promise_ce);
    flames_async_promise *p = FLAMES_ASYNC_PROMISE_FROM_ZVAL(return_value);
    p->fd           = (int)fd;
    p->wait_mode    = FLAMES_PROMISE_WAIT_READ;
    p->epoll_events = EPOLLIN;
}

PHP_METHOD(FlamesAsyncPromise, forReadable)
{
    zend_long fd;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(fd)
    ZEND_PARSE_PARAMETERS_END();

    object_init_ex(return_value, flames_async_promise_ce);
    flames_async_promise *p = FLAMES_ASYNC_PROMISE_FROM_ZVAL(return_value);
    p->fd           = (int)fd;
    p->wait_mode    = FLAMES_PROMISE_WAIT_READY;
    p->epoll_events = EPOLLIN;
}

PHP_METHOD(FlamesAsyncPromise, forWritable)
{
    zend_long fd;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(fd)
    ZEND_PARSE_PARAMETERS_END();

    object_init_ex(return_value, flames_async_promise_ce);
    flames_async_promise *p = FLAMES_ASYNC_PROMISE_FROM_ZVAL(return_value);
    p->fd           = (int)fd;
    p->wait_mode    = FLAMES_PROMISE_WAIT_READY;
    p->epoll_events = EPOLLOUT;
}

PHP_METHOD(FlamesAsyncPromise, resolve)
{
    zval *value;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(value)
    ZEND_PARSE_PARAMETERS_END();

    flames_async_promise *p = FLAMES_ASYNC_PROMISE_FROM_ZVAL(getThis());
    if (p->state != FLAMES_PROMISE_PENDING) { return; }
    ZVAL_COPY(&p->result, value);
    p->state = FLAMES_PROMISE_RESOLVED;
}

PHP_METHOD(FlamesAsyncPromise, reject)
{
    zend_string *msg;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(msg)
    ZEND_PARSE_PARAMETERS_END();

    flames_async_promise *p = FLAMES_ASYNC_PROMISE_FROM_ZVAL(getThis());
    if (p->state != FLAMES_PROMISE_PENDING) { return; }
    p->error_msg = zend_string_copy(msg);
    p->state     = FLAMES_PROMISE_REJECTED;
}

PHP_METHOD(FlamesAsyncPromise, isPending)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_BOOL(FLAMES_ASYNC_PROMISE_FROM_ZVAL(getThis())->state == FLAMES_PROMISE_PENDING);
}

/* ---- Service::async / await / run ---- */

PHP_METHOD(FlamesAsyncService, async)
{
    zval *callable;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(callable)
    ZEND_PARSE_PARAMETERS_END();

    flames_loop_init();

    /* Outer completion promise returned to the caller */
    object_init_ex(return_value, flames_async_promise_ce);

    /* Create and immediately start the Fiber */
    zval fiber_obj;
    object_init_ex(&fiber_obj, zend_ce_fiber);
    FLAMES_CALL1(Z_OBJ(fiber_obj), zend_ce_fiber, NULL, "__construct", NULL, callable);

    if (EG(exception)) {
        zval_ptr_dtor(&fiber_obj);
        return;
    }

    zval start_ret;
    flames_fiber_start(Z_OBJ(fiber_obj), &start_ret);
    zval_ptr_dtor(&start_ret);

    flames_process_fiber_step(&fiber_obj, return_value);
    zval_ptr_dtor(&fiber_obj);
}

PHP_METHOD(FlamesAsyncService, await)
{
    zval *promise;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(promise)
    ZEND_PARSE_PARAMETERS_END();

    if (Z_TYPE_P(promise) != IS_OBJECT
        || !instanceof_function(Z_OBJCE_P(promise), flames_async_promise_ce))
    {
        zend_throw_exception_ex(zend_ce_type_error, 0,
            "Flames\\Async\\Service::await(): argument must be an instance of "
            "Flames\\Async\\Async\\Promise");
        return;
    }

    flames_async_promise *p = FLAMES_ASYNC_PROMISE_FROM_ZVAL(promise);

    if (p->state == FLAMES_PROMISE_RESOLVED) {
        ZVAL_COPY(return_value, &p->result);
        return;
    }
    if (p->state == FLAMES_PROMISE_REJECTED) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0, "%s",
            p->error_msg ? ZSTR_VAL(p->error_msg) : "Promise rejected");
        return;
    }

    if (g_flames_fiber_executing) {
        /*
         * Inside a coroutine: suspend this Fiber, passing the Promise to
         * the scheduler.  The scheduler resumes us with the resolved value
         * via $fiber->resume($value).  Fiber::suspend() returns that value.
         */
        zval fn, suspend_ret;
        ZVAL_STRING(&fn, "Fiber::suspend");
        call_user_function(NULL, NULL, &fn, &suspend_ret, 1, promise);
        zval_ptr_dtor(&fn);
        if (!EG(exception)) {
            ZVAL_COPY(return_value, &suspend_ret);
        }
        zval_ptr_dtor(&suspend_ret);
        return;
    }

    /* Main thread: drive the event loop until the Promise settles */
    flames_loop_init();
    while (p->state == FLAMES_PROMISE_PENDING && g_waiter_count > 0) {
        flames_loop_tick(100);
        if (EG(exception)) { return; }
    }

    if (p->state == FLAMES_PROMISE_RESOLVED) {
        ZVAL_COPY(return_value, &p->result);
    } else if (p->state == FLAMES_PROMISE_REJECTED) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0, "%s",
            p->error_msg ? ZSTR_VAL(p->error_msg) : "Promise rejected");
    } else {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0,
            "Flames\\Async\\Service::await(): promise did not resolve (no pending I/O)");
    }
}

PHP_METHOD(FlamesAsyncService, run)
{
    ZEND_PARSE_PARAMETERS_NONE();
    flames_loop_init();
    while (g_waiter_count > 0) {
        flames_loop_tick(100);
        if (EG(exception)) { return; }
    }
}

PHP_FUNCTION(flames_c_async_service_version)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_STRING(PHP_FLAMES_ASYNC_CONFIG_VERSION);
}

static const zend_function_entry flames_async_service_methods[] = {
    PHP_ME(FlamesAsyncService, fork,  arginfo_Service_fork,  ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(FlamesAsyncService, wait,  arginfo_Service_wait,  ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(FlamesAsyncService, async, arginfo_Service_async, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(FlamesAsyncService, await, arginfo_Service_await, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(FlamesAsyncService, run,   arginfo_Service_run,   ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_FE_END
};

static const zend_function_entry flames_async_task_methods[] = {
    PHP_ME(FlamesAsyncServiceTask, isDone, arginfo_Task_isDone, ZEND_ACC_PUBLIC)
    PHP_ME(FlamesAsyncServiceTask, result, arginfo_Task_result, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static const zend_function_entry flames_async_promise_methods[] = {
    PHP_ME(FlamesAsyncPromise, forFd,        arginfo_Promise_forFd,        ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(FlamesAsyncPromise, forReadable,  arginfo_Promise_forReadable,  ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(FlamesAsyncPromise, forWritable,  arginfo_Promise_forWritable,  ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(FlamesAsyncPromise, resolve,      arginfo_Promise_resolve,      ZEND_ACC_PUBLIC)
    PHP_ME(FlamesAsyncPromise, reject,       arginfo_Promise_reject,       ZEND_ACC_PUBLIC)
    PHP_ME(FlamesAsyncPromise, isPending,    arginfo_Promise_isPending,    ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static const zend_function_entry flames_async_functions[] = {
    PHP_NAMED_FE(
        __flames_c_async_service_version__,
        PHP_FN(flames_c_async_service_version),
        arginfo_get_async_service_version)
    PHP_FE(flames_stream_to_fd, arginfo_flames_stream_to_fd)
    PHP_FE(flames_async_sleep,  arginfo_flames_async_sleep)
    PHP_FE_END
};
PHP_MINIT_FUNCTION(flames_async)
{
    zend_class_entry ce;

    REGISTER_INI_ENTRIES();

    if (flames_async_hooks_enabled) {
        flames_hooks_install();
    }

    flames_patch_sleep_functions();

    INIT_CLASS_ENTRY(ce, "Flames\\Async\\Async\\Service", flames_async_service_methods);
    flames_async_service_ce = zend_register_internal_class(&ce);
    flames_async_service_ce->ce_flags |= ZEND_ACC_FINAL
                                       | ZEND_ACC_NO_DYNAMIC_PROPERTIES;

    INIT_CLASS_ENTRY(ce, "Flames\\Async\\Async\\Service\\Task", flames_async_task_methods);
    flames_async_task_ce = zend_register_internal_class(&ce);
    flames_async_task_ce->create_object = flames_async_task_create_object;
    flames_async_task_ce->ce_flags     |= ZEND_ACC_FINAL
                                        | ZEND_ACC_NO_DYNAMIC_PROPERTIES
                                        | ZEND_ACC_NOT_SERIALIZABLE;

    memcpy(&flames_async_task_handlers,
           zend_get_std_object_handlers(),
           sizeof(zend_object_handlers));
    flames_async_task_handlers.offset    = XtOffsetOf(flames_async_task, std);
    flames_async_task_handlers.free_obj  = flames_async_task_free_object;
    flames_async_task_handlers.clone_obj = NULL; /* cloning a Task is undefined */

    INIT_CLASS_ENTRY(ce, "Flames\\Async\\Async\\Promise", flames_async_promise_methods);
    flames_async_promise_ce = zend_register_internal_class(&ce);
    flames_async_promise_ce->create_object = flames_async_promise_create_object;
    flames_async_promise_ce->ce_flags     |= ZEND_ACC_FINAL
                                           | ZEND_ACC_NO_DYNAMIC_PROPERTIES
                                           | ZEND_ACC_NOT_SERIALIZABLE;

    memcpy(&flames_async_promise_handlers,
           zend_get_std_object_handlers(),
           sizeof(zend_object_handlers));
    flames_async_promise_handlers.offset    = XtOffsetOf(flames_async_promise, std);
    flames_async_promise_handlers.free_obj  = flames_async_promise_free_object;
    flames_async_promise_handlers.clone_obj = NULL;

    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(flames_async)
{
    flames_loop_destroy();
    g_flames_hooks_installed = 0;
    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(flames_async)
{
    UNREGISTER_INI_ENTRIES();
    return SUCCESS;
}

PHP_MINFO_FUNCTION(flames_async)
{
    php_info_print_table_start();
    php_info_print_table_header(2, "Flames Async Service", "enabled");
    php_info_print_table_row(2, "Version",       PHP_FLAMES_ASYNC_VERSION);
    php_info_print_table_row(2, "Parallelism",   "fork + fiber coroutines");
    php_info_print_table_row(2, "Async Hooks",   flames_async_hooks_enabled ? "enabled" : "disabled");
    php_info_print_table_row(2, "Hook Transport",
        (g_flames_socket_ops_ptr && g_flames_ssl_hooks_ready) ? "TCP + SSL (php_stream)"
        : (g_flames_socket_ops_ptr ? "TCP (php_stream)" : "unavailable"));
    php_info_print_table_row(2, "ZTS required",  "no");
    php_info_print_table_row(2, "Serializer",
        zend_hash_str_exists(EG(function_table),
            "igbinary_serialize", sizeof("igbinary_serialize") - 1)
        ? "igbinary" : "native");
    php_info_print_table_end();

    DISPLAY_INI_ENTRIES();
}

zend_module_entry flames_async_module_entry = {
    STANDARD_MODULE_HEADER,
    PHP_FLAMES_ASYNC_EXTNAME,
    flames_async_functions,
    PHP_MINIT(flames_async),
    PHP_MSHUTDOWN(flames_async),
    NULL,
    PHP_RSHUTDOWN(flames_async),
    PHP_MINFO(flames_async),
    PHP_FLAMES_ASYNC_VERSION,
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_CFLAMES_ASYNC_SERVICE
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(flames_async)
#endif
