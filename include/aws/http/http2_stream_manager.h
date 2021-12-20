#ifndef AWS_HTTP2_STREAM_MANAGER_H
#define AWS_HTTP2_STREAM_MANAGER_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/http.h>

struct aws_http2_stream_manager;
struct aws_client_bootstrap;
struct aws_http_connection;
struct aws_http_connection_manager;
struct aws_socket_options;
struct aws_tls_connection_options;
struct proxy_env_var_settings;
struct aws_http2_setting;
struct aws_http_make_request_options;
struct aws_http_stream;

/**
 * Always invoked asynchronously when the stream was created, successfully or not.
 * When stream is NULL, error code will be set to indicate what happened.
 * If there is a stream returned, you own the stream completely.
 * Invoked on the same thread as other callback of the stream, which will be the thread of the connection.
 */
typedef void(
    aws_http2_stream_manager_on_stream_acquired_fn)(struct aws_http_stream *stream, int error_code, void *user_data);

/**
 * Invoked asynchronously when the stream manager has been shutdown completely.
 * Never invoked when `aws_http2_stream_manager_new` failed.
 */
typedef void(aws_http2_stream_manager_shutdown_complete_fn)(void *user_data);

/**
 * HTTP/2 stream manager configuration struct.
 *
 * Contains all of the configuration needed to create an http2 connection as well as
 * connection manager under the hood.
 */
struct aws_http2_stream_manager_options {
    /**
     * basic http connection configuration
     */
    struct aws_client_bootstrap *bootstrap;
    const struct aws_socket_options *socket_options;
    /**
     * If TLS options is set, you also need to handle ALPN, otherwise, may not able to get HTTP/2 connection and fail
     * the stream manager.
     * If TLS options not set, prior knowledge will be used.
     */
    const struct aws_tls_connection_options *tls_connection_options;
    struct aws_byte_cursor host;
    uint16_t port;

    /**
     * HTTP/2 Stream window control.
     * If set to true, the read back pressure mechanism will be enabled for streams created.
     * The initial window size can be set through `initial window size`
     */
    bool enable_read_back_pressure;
    /**
     * Optional.
     * If set, it will be sent to the peer as the `AWS_HTTP2_SETTINGS_INITIAL_WINDOW_SIZE` in the initial settings for
     * HTTP/2 connection.
     * If not set, the default will be used, which is 65,535 (2^16-1)(RFC-7540 6.5.2)
     * Ignored if enable_read_back_pressure is false.
     */
    size_t initial_window_size;

    /* Connection monitor for the underlying connections made */
    const struct aws_http_connection_monitoring_options *monitoring_options;

    /* Optional. Proxy configuration for underlying http connection */
    const struct aws_http_proxy_options *proxy_options;
    const struct proxy_env_var_settings *proxy_ev_settings;
    /**
     * Optional.
     * Default is no limit. 0 will be considered as using the default value.
     * The ideal number of concurrent streams for a connection. Stream manager will try to create a new connection if
     * one connection reaches this number. But, if the max connections reaches, manager will reuse connections to create
     * the acquired steams as much as possible. */
    size_t ideal_concurrent_streams_per_connection;
    /**
     * Optional.
     * Default is no limit. 0 will be considered as using the default value.
     * The real number of concurrent streams per connection will be controlled by the minmal value of the setting from
     * other end and the value here.
     */
    size_t max_concurrent_streams_per_connection;
    /**
     * Required.
     * The max number of connections will be open at same time. If all the connections are full, manager will wait until
     * available to vender more streams */
    size_t max_connections;

    /**
     * Required.
     * When the stream manager finishes deleting all the resources, the callback will be invoked.
     */
    void *shutdown_complete_user_data;
    aws_http2_stream_manager_shutdown_complete_fn *shutdown_complete_callback;
};

struct aws_http2_stream_manager_acquire_stream_options {
    /**
     * Required.
     * Invoked when the stream finishes acquiring by stream manager.
     */
    aws_http2_stream_manager_on_stream_acquired_fn *callback;
    void *user_data;
    /* see `aws_http_make_request_options` */
    const struct aws_http_make_request_options *options;
};

AWS_EXTERN_C_BEGIN

AWS_HTTP_API
void aws_http2_stream_manager_acquire(struct aws_http2_stream_manager *manager);

AWS_HTTP_API
void aws_http2_stream_manager_release(struct aws_http2_stream_manager *manager);

AWS_HTTP_API
struct aws_http2_stream_manager *aws_http2_stream_manager_new(
    struct aws_allocator *allocator,
    struct aws_http2_stream_manager_options *options);

/**
 * Acquire a stream from stream manager asynchronously.
 *
 * TODO: We can probably not require activate, but it will always not hurt if you activate as a normal stream.
 * You must call aws_http_stream_activate to begin execution of the request. Note aws_http_stream_activate can fail
 * because of the underlying connection lifetime(GOAWAY received or connection shutting down). For those case, release
 * the stream and acquire a new one is recommended.
 *
 * @param http2_stream_manager
 * @param options
 * @param callback
 * @param user_data
 */
AWS_HTTP_API
void aws_http2_stream_manager_acquire_stream(
    struct aws_http2_stream_manager *http2_stream_manager,
    const struct aws_http2_stream_manager_acquire_stream_options *acquire_stream_option);

AWS_EXTERN_C_END
#endif /* AWS_HTTP2_STREAM_MANAGER_H */
