dnl config.m4 - Flames Async Service PHP Extension build configuration
dnl
dnl Requires PHP 8.5 or newer.
dnl
dnl Usage:
dnl   phpize
dnl   ./configure --enable-cflames-async-service
dnl   make
dnl   make install

PHP_ARG_ENABLE(
    [cflames_async_service],
    [whether to enable Flames Async Service support],
    [AS_HELP_STRING(
        [--enable-cflames-async-service],
        [Enable Flames Async Service fork-based parallel execution extension])],
    [no])

if test "$PHP_CFLAMES_ASYNC_SERVICE" != "no"; then

    dnl ── PHP version guard: require >= 8.5 ─────────────────────────────────
    AC_MSG_CHECKING([PHP version (>= 8.5 required)])
    php_version_str=$($PHP_CONFIG --version 2>/dev/null)
    php_major=$(printf '%s' "$php_version_str" | cut -d. -f1)
    php_minor=$(printf '%s' "$php_version_str" | cut -d. -f2)
    if test -z "$php_major" || test -z "$php_minor"; then
        AC_MSG_WARN([could not determine PHP version from php-config; proceeding])
    elif test "$php_major" -lt 8 || \
         { test "$php_major" -eq 8 && test "$php_minor" -lt 5; }; then
        AC_MSG_ERROR([Flames Async Service requires PHP 8.5 or newer. Found: $php_version_str])
    else
        AC_MSG_RESULT([$php_version_str, ok])
    fi

    AC_DEFINE(HAVE_FLAMES_ASYNC, 1, [Whether Flames Async is enabled])
    PHP_NEW_EXTENSION(cflames_async_service, flames_async.c, $ext_shared)
    PHP_SUBST(CFLAMES_ASYNC_SERVICE_SHARED_LIBADD)
    PHP_ADD_MAKEFILE_FRAGMENT
fi
