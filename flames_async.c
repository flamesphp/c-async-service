/*
 * Flames Async Service - PHP Extension
 *
 * Fork-based real parallelism for PHP closures.
 *
 * PHP API
 * ───────
 *   use Flames\Async\Async\Service;
 *
 *   $task  = Service::async(callable $fn)         → Task object, fork starts immediately
 *   $value = Service::await($task)                → blocks, returns value
 *   $arr   = Service::await($task1, $task2, …)    → blocks, returns indexed array
 *
 *   $task->isDone()   → non-blocking poll (bool)
 *   $task->result()   → same as Service::await($task)
 *
 * Serialization
 * ─────────────
 *   If the igbinary extension is loaded, igbinary_serialize / igbinary_unserialize
 *   are used for the pipe payload (faster, binary, supports more types).
 *   Otherwise falls back to PHP's native serialize / unserialize.
 *   The decision is made once per process and cached.
 *
 * Wire protocol (1-byte status header)
 * ─────────────────────────────────────
 *   0x01 + <serialized payload>   → success
 *   0x02 + <utf-8 error message>  → exception / call failure
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <signal.h>

#include "php_flames_async.h"
#include "ext/standard/php_var.h"
#include "ext/spl/spl_exceptions.h"
#include "zend_smart_str.h"
#include "zend_exceptions.h"

/* =========================================================================
 * Wire-protocol status bytes
 * ========================================================================= */

#define ASYNC_STATUS_OK    ((char)0x01)
#define ASYNC_STATUS_ERROR ((char)0x02)

/* =========================================================================
 * igbinary runtime detection (cached after first call)
 * ========================================================================= */

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

/* -----------------------------------------------------------------
 * Serialize a zval → smart_str buffer
 *   uses igbinary if available, otherwise native serialize
 * ----------------------------------------------------------------- */
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

/* -----------------------------------------------------------------
 * Unserialize raw bytes → zval
 *   uses igbinary if available, otherwise native unserialize
 *   returns SUCCESS or FAILURE
 * ----------------------------------------------------------------- */
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

/* =========================================================================
 * Task object handlers  (\Flames\Async\Service\Task)
 * ========================================================================= */

static zend_object_handlers flames_async_task_handlers;

zend_class_entry *flames_async_service_ce;
zend_class_entry *flames_async_task_ce;

/* -----------------------------------------------------------------
 * Object allocator
 * ----------------------------------------------------------------- */
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

/* -----------------------------------------------------------------
 * Object destructor – clean up on last-reference drop
 * ----------------------------------------------------------------- */
static void flames_async_task_free_object(zend_object *obj)
{
    flames_async_task *task = flames_async_task_from_obj(obj);

    if (task->pipe_read_fd >= 0) {
        close(task->pipe_read_fd);
        task->pipe_read_fd = -1;
    }

    if (!task->done && task->child_pid > 0) {
        /* Task was discarded without being awaited – reap child. */
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

/* =========================================================================
 * Internal: block until child result is collected from pipe
 * ========================================================================= */
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

    /* Blocking read – drains the pipe until the child closes its write end. */
    smart_str buf = {0};
    char      tmp[8192];
    ssize_t   n;

    while ((n = read(task->pipe_read_fd, tmp, sizeof(tmp))) > 0) {
        smart_str_appendl(&buf, tmp, (size_t)n);
    }

    close(task->pipe_read_fd);
    task->pipe_read_fd = -1;

    /* Reap child. */
    waitpid(task->child_pid, NULL, 0);
    task->child_pid = -1;
    task->done      = 1;

    if (!buf.s || ZSTR_LEN(buf.s) == 0) {
        task->has_error = 1;
        task->error_msg = zend_string_init(
            "async: child process terminated without a result", 48, 0);
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
                "async: failed to unserialize child return value", 47, 0);
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

/* =========================================================================
 * \Flames\Async\Service\Task – instance methods
 * ========================================================================= */

/* {{{ Task::isDone(): bool */
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

    /* Non-blocking poll via select(). */
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
/* }}} */

/* {{{ Task::result(): mixed
 *
 * Blocks until the child finishes and returns its return value.
 * Throws RuntimeException if the child threw an exception.
 */
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
/* }}} */

/* =========================================================================
 * \Flames\Async\Async\Service – static methods
 * ========================================================================= */

/* {{{ Service::async(callable $fn): \Flames\Async\Service\Task
 *
 * Forks immediately and starts executing $fn() in the child.
 * Returns a Task object without waiting for the child.
 */
PHP_METHOD(FlamesAsyncService, async)
{
    zval *callable;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(callable)
    ZEND_PARSE_PARAMETERS_END();

    /* Create the Task object that will hold the fork state. */
    object_init_ex(return_value, flames_async_task_ce);
    flames_async_task *task = FLAMES_ASYNC_TASK_FROM_ZVAL(return_value);

    int fds[2];
    if (pipe(fds) != 0) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0,
            "Flames\\Async\\Async\\Service::async(): pipe() failed: %s", strerror(errno));
        return;
    }

    pid_t pid = fork();

    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        zend_throw_exception_ex(spl_ce_RuntimeException, 0,
            "Flames\\Async\\Async\\Service::async(): fork() failed: %s", strerror(errno));
        return;
    }

    if (pid == 0) {
        /* ================================================================
         * CHILD PROCESS
         * ================================================================ */
        close(fds[0]); /* child never reads */

        if (EG(exception)) {
            zend_clear_exception();
        }

        zval retval;
        ZVAL_UNDEF(&retval);

        int call_ok = (call_user_function(NULL, NULL, callable, &retval, 0, NULL) == SUCCESS)
                      && !EG(exception);

        if (call_ok) {
            /* ---- SUCCESS ---- */
            smart_str sbuf = {0};
            flames_serialize(&retval, &sbuf);
            zval_ptr_dtor(&retval);

            char status = ASYNC_STATUS_OK;
            write(fds[1], &status, 1);

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
            /* ---- EXCEPTION / ERROR ---- */
            const char  *errmsg = "Unknown error in async closure";
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
            write(fds[1], &status, 1);
            write(fds[1], errmsg, errlen);

            if (exc_str) {
                zend_string_release(exc_str);
            }
        }

        close(fds[1]);
        _exit(0); /* intentional: bypass PHP/C++ atexit handlers in child */
    }

    /* ================================================================
     * PARENT PROCESS
     * ================================================================ */
    close(fds[1]); /* parent never writes */
    task->pipe_read_fd = fds[0];
    task->child_pid    = pid;
}
/* }}} */

/* {{{ Service::await(\Flames\Async\Service\Task ...$tasks): mixed
 *
 * Blocks until all listed tasks finish, then returns:
 *   - single task  → the task's return value directly
 *   - multiple     → indexed array [ result0, result1, … ]
 *
 * All children were forked at Service::async() time and are already
 * running concurrently.  Total wall-time ≈ max(task durations), not sum.
 */
PHP_METHOD(FlamesAsyncService, await)
{
    zval *tasks     = NULL;
    int   num_tasks = 0;

    ZEND_PARSE_PARAMETERS_START(1, -1)
        Z_PARAM_VARIADIC('+', tasks, num_tasks)
    ZEND_PARSE_PARAMETERS_END();

    /* Validate: every argument must be a Task instance. */
    for (int i = 0; i < num_tasks; i++) {
        if (Z_TYPE(tasks[i]) != IS_OBJECT
            || !instanceof_function(Z_OBJCE(tasks[i]), flames_async_task_ce))
        {
            zend_throw_exception_ex(zend_ce_type_error, 0,
                "Flames\\Async\\Async\\Service::await(): argument %d must be an instance of "
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
                    "async[%d]: %s", i, ZSTR_VAL(task->error_msg));
                return;
            }

            zval copy;
            ZVAL_COPY(&copy, &task->result);
            add_next_index_zval(return_value, &copy);
        }
    }
}
/* }}} */

/* =========================================================================
 * __flames_c_async_service_version__()
 *
 * Returns the version string embedded from config.yml at build time.
 * ========================================================================= */
PHP_FUNCTION(flames_c_async_service_version)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_STRING(PHP_FLAMES_ASYNC_CONFIG_VERSION);
}

/* =========================================================================
 * Module registration
 * ========================================================================= */

static const zend_function_entry flames_async_service_methods[] = {
    PHP_ME(FlamesAsyncService, async, arginfo_Service_async, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(FlamesAsyncService, await, arginfo_Service_await, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_FE_END
};

static const zend_function_entry flames_async_task_methods[] = {
    PHP_ME(FlamesAsyncServiceTask, isDone, arginfo_Task_isDone, ZEND_ACC_PUBLIC)
    PHP_ME(FlamesAsyncServiceTask, result, arginfo_Task_result, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static const zend_function_entry flames_async_functions[] = {
    PHP_NAMED_FE(
        __flames_c_async_service_version__,
        PHP_FN(flames_c_async_service_version),
        arginfo_get_async_service_version)
    PHP_FE_END
};
PHP_MINIT_FUNCTION(flames_async)
{
    zend_class_entry ce;

    /* \Flames\Async\Async\Service */
    INIT_CLASS_ENTRY(ce, "Flames\\Async\\Async\\Service", flames_async_service_methods);
    flames_async_service_ce = zend_register_internal_class(&ce);
    flames_async_service_ce->ce_flags |= ZEND_ACC_FINAL
                                       | ZEND_ACC_NO_DYNAMIC_PROPERTIES;

    /* \Flames\Async\Async\Service\Task */
    INIT_CLASS_ENTRY(ce, "Flames\\Async\\Async\\Service\\Task", flames_async_task_methods);
    flames_async_task_ce = zend_register_internal_class(&ce);
    flames_async_task_ce->create_object = flames_async_task_create_object;
    flames_async_task_ce->ce_flags     |= ZEND_ACC_FINAL
                                        | ZEND_ACC_NO_DYNAMIC_PROPERTIES
                                        | ZEND_ACC_NOT_SERIALIZABLE;

    /* Custom object handlers for Task */
    memcpy(&flames_async_task_handlers,
           zend_get_std_object_handlers(),
           sizeof(zend_object_handlers));
    flames_async_task_handlers.offset    = XtOffsetOf(flames_async_task, std);
    flames_async_task_handlers.free_obj  = flames_async_task_free_object;
    flames_async_task_handlers.clone_obj = NULL; /* cloning a Task is undefined */

    return SUCCESS;
}

/* -----------------------------------------------------------------
 * MINFO: shown in phpinfo()
 * ----------------------------------------------------------------- */
PHP_MINFO_FUNCTION(flames_async)
{
    php_info_print_table_start();
    php_info_print_table_header(2, "Flames Async Service", "enabled");
    php_info_print_table_row(2, "Version",      PHP_FLAMES_ASYNC_VERSION);
    php_info_print_table_row(2, "Parallelism",  "fork-based (OS processes)");
    php_info_print_table_row(2, "ZTS required", "no");
    php_info_print_table_row(2, "Serializer",
        zend_hash_str_exists(EG(function_table),
            "igbinary_serialize", sizeof("igbinary_serialize") - 1)
        ? "igbinary" : "native");
    php_info_print_table_end();
}

/* -----------------------------------------------------------------
 * Module entry
 * ----------------------------------------------------------------- */
zend_module_entry flames_async_module_entry = {
    STANDARD_MODULE_HEADER,
    PHP_FLAMES_ASYNC_EXTNAME,
    flames_async_functions,
    PHP_MINIT(flames_async),
    NULL,                   /* MSHUTDOWN */
    NULL,                   /* RINIT     */
    NULL,                   /* RSHUTDOWN */
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
