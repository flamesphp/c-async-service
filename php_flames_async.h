#ifndef PHP_FLAMES_ASYNC_H
#define PHP_FLAMES_ASYNC_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "zend_exceptions.h"

#if PHP_VERSION_ID < 80500
# error "Flames Async Service requires PHP 8.5 or newer (PHP_VERSION_ID >= 80500)"
#endif

extern zend_module_entry flames_async_module_entry;
#define phpext_flames_async_ptr &flames_async_module_entry

#define PHP_FLAMES_ASYNC_VERSION "1.0.0"
#define PHP_FLAMES_ASYNC_EXTNAME "cflames_async_service"

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

typedef struct _flames_async_task {
    pid_t        child_pid;
    int          pipe_read_fd;
    bool         done;
    bool         has_error;
    zval         result;
    zend_string *error_msg;
    zend_object  std;
} flames_async_task;

static inline flames_async_task *flames_async_task_from_obj(zend_object *obj) {
    return (flames_async_task *)((char *)obj - XtOffsetOf(flames_async_task, std));
}

#define FLAMES_ASYNC_TASK_FROM_ZVAL(zv) \
    flames_async_task_from_obj(Z_OBJ_P(zv))

extern zend_class_entry *flames_async_service_ce;
extern zend_class_entry *flames_async_task_ce;

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_get_async_service_version, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_Service_async, 0, 0, 1)
    ZEND_ARG_CALLABLE_INFO(0, fn, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_Service_await, 0, 0, 1)
    ZEND_ARG_VARIADIC_INFO(0, tasks)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_Task_isDone, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_Task_result, 0, 0, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(FlamesAsyncService, async);
PHP_METHOD(FlamesAsyncService, await);

PHP_METHOD(FlamesAsyncServiceTask, isDone);
PHP_METHOD(FlamesAsyncServiceTask, result);

PHP_FUNCTION(flames_c_async_service_version);

#endif
