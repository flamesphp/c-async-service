ARG PHP_VERSION=8.5
FROM php:${PHP_VERSION}-apache

USER root

RUN apt-get update && apt-get install -y --no-install-recommends \
        autoconf \
        automake \
        gcc \
        make \
        libtool \
        pkg-config \
        re2c \
    && rm -rf /var/lib/apt/lists/*

COPY config.m4           /usr/src/flames-async/
COPY php_flames_async.h  /usr/src/flames-async/
COPY flames_async.c      /usr/src/flames-async/

RUN cd /usr/src/flames-async \
    && phpize \
    && ./configure --enable-cflames-async-service \
    && make -j"$(nproc)" \
    && make install

RUN echo "extension=cflames_async_service.so" \
        > /usr/local/etc/php/conf.d/50-flames-async.ini

RUN a2enmod rewrite

WORKDIR /var/www/html
