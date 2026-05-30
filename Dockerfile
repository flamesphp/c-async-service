# Flames Async Service – Dockerfile
#
# Compiles and installs the cflames_async_service PHP extension into
# a standard PHP-Apache image.
#
# Build:
#   docker build -t flames-async .
#
# Test:
#   docker run --rm flames-async php -r "
#     \$a = new async(function(){ sleep(1); return 'hello'; });
#     \$b = new async(function(){ sleep(2); return 'world'; });
#     \$r = await(\$a, \$b);
#     echo \$r[0] . ' ' . \$r[1] . PHP_EOL;
#   "

ARG PHP_VERSION=8.5
FROM php:${PHP_VERSION}-apache

USER root

# ── System dependencies ─────────────────────────────────────────────────
RUN apt-get update && apt-get install -y --no-install-recommends \
        autoconf \
        automake \
        gcc \
        make \
        libtool \
        pkg-config \
        re2c \
    && rm -rf /var/lib/apt/lists/*

# ── Copy extension source ───────────────────────────────────────────────
COPY config.m4           /usr/src/flames-async/
COPY php_flames_async.h  /usr/src/flames-async/
COPY flames_async.c      /usr/src/flames-async/

# ── Build & install ─────────────────────────────────────────────────────
RUN cd /usr/src/flames-async \
    && phpize \
    && ./configure --enable-cflames-async-service \
    && make -j"$(nproc)" \
    && make install

# ── Enable the extension ────────────────────────────────────────────────
RUN echo "extension=cflames_async_service.so" \
        > /usr/local/etc/php/conf.d/50-flames-async.ini

# ── Enable Apache mod_rewrite ───────────────────────────────────────────
RUN a2enmod rewrite

WORKDIR /var/www/html
