/*
 * Flames Async Service - PHP Extension
 *
 * Provides real OS-level parallelism for PHP closures using fork(2) + pipe(2).
 *
 * PHP API:
 *   \Flames\Async\Async\Service::async(callable $fn): \Flames\Async\Async\Service\Task
 *   \Flames\Async\Async\Service::await(\Flames\Async\Async\Service\Task ...$tasks): mixed
 *
 *   \Flames\Async\Service\Task::isDone(): bool
 *   \Flames\Async\Service\Task::result(): mixed
 *
 * Requires PHP 8.5+
 */

#ifndef PHP_FLAMES_ASYNC_H
#define PHP_FLAMES_ASYNC_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "zend_exceptions.h"

/* -----------------------------------------------------------------------
 * Compile-time version guard – hard error if PHP < 8.5
 * ----------------------------------------------------------------------- */
#if PHP_VERSION_ID < 80500
# error "Flames Async Service requires PHP 8.5 or newer (PHP_VERSION_ID >= 80500)"
#endif

extern zend_module_entry flames_async_module_entry;
#define phpext_flames_async_ptr &flames_async_module_entry

#define PHP_FLAMES_ASYNC_VERSION "1.0.0"
#define PHP_FLAMES_ASYNC_EXTNAME "cflames_async_service"

/* Version string embedded from config.yml at build time. */
#define PHP_FLAMES_ASYNC_CONFIG_VERSION "a3f8c2e1b7d94056f2a1c3e8b5d07f4a9c2e6b81"

#ifdef PHP_WIN32
#   define PHP_FLAMES_ASYNC_API __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
#   define PHP_FLAMES_ASYNC_API __attribute__((visibility("default")))
#else
#   define PHP_FLAMES_ASYNC_API
#endif

#ifdef ZTS
#include "TSRM.h"
#endif

/* -----------------------------------------------------------------------
 * Internal struct that backs a \Flames\Async\Service\Task object.
 *
 * NOTE: `std` MUST be the last field.  PHP computes the zend_object address
 * by subtracting XtOffsetOf(flames_async_task, std) from the object pointer;
 * any fields after `std` would be silently ignored / corrupted.
 *
 * NOTE: `zend_bool` was deprecated in PHP 8.4 and replaced by plain `bool`.
 * ----------------------------------------------------------------------- */
typedef struct _flames_async_task {
    pid_t        child_pid;    /* PID of the forked child                  */
    int          pipe_read_fd; /* parent reads results from this fd        */
    bool         done;         /* true once result has been collected      */
    bool         has_error;    /* true if child reported an error          */
    zval         result;       /* unserialized return value from child     */
    zend_string *error_msg;    /* error message string (when has_error)    */
    zend_object  std;          /* MUST be last                             */
} flames_async_task;

static inline flames_async_task *flames_async_task_from_obj(zend_object *obj) {
    return (flames_async_task *)((char *)obj - XtOffsetOf(flames_async_task, std));
}

#define FLAMES_ASYNC_TASK_FROM_ZVAL(zv) \
    flames_async_task_from_obj(Z_OBJ_P(zv))

/* -----------------------------------------------------------------------
 * Class entries
 *   flames_async_service_ce  → \Flames\Async\Service
 *   flames_async_task_ce     → \Flames\Async\Service\Task
 * ----------------------------------------------------------------------- */
extern zend_class_entry *flames_async_service_ce;
extern zend_class_entry *flames_async_task_ce;

/* __flames_c_async_service_version__(): string */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_get_async_service_version, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* -----------------------------------------------------------------------
 * Arginfo
 *
 * IMPORTANT: ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX stringifies its
 * `classname` argument via the # operator.  Backslash-separated namespace
 * tokens (e.g. Flames\\Async\\Async\\Service\\Task) are NOT valid C tokens and
 * will cause a preprocessor/compiler error.  For namespaced return types
 * we therefore omit the return-type annotation in arginfo and rely on the
 * runtime instanceof check performed in the method body instead.
 * ----------------------------------------------------------------------- */

/* Service::async(callable $fn) */
ZEND_BEGIN_ARG_INFO_EX(arginfo_Service_async, 0, 0, 1)
    ZEND_ARG_CALLABLE_INFO(0, fn, 0)
ZEND_END_ARG_INFO()

/* Service::await(mixed ...$tasks) – validated at runtime via instanceof */
ZEND_BEGIN_ARG_INFO_EX(arginfo_Service_await, 0, 0, 1)
    ZEND_ARG_VARIADIC_INFO(0, tasks)
ZEND_END_ARG_INFO()

/* Task::isDone(): bool */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_Task_isDone, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

/* Task::result(): mixed */
ZEND_BEGIN_ARG_INFO_EX(arginfo_Task_result, 0, 0, 0)
ZEND_END_ARG_INFO()

/* -----------------------------------------------------------------------
 * Method declarations
 * ----------------------------------------------------------------------- */

PHP_METHOD(FlamesAsyncService, async);
PHP_METHOD(FlamesAsyncService, await);

PHP_METHOD(FlamesAsyncServiceTask, isDone);
PHP_METHOD(FlamesAsyncServiceTask, result);

/* Global function */
PHP_FUNCTION(flames_c_async_service_version);

#endif /* PHP_FLAMES_ASYNC_H */
