/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

/**@file upload_client.h
 *
 * @defgroup dl_client Download client
 * @{
 * @brief Client for downloading an object.
 *
 * @details The download client provides APIs for:
 *  - connecting to a remote server,
 *  - downloading an object from the server,
 *  - disconnecting from the server,
 *  - receiving asynchronous event notifications on the download status.
 */

#ifndef UPLOAD_CLIENT_H__
#define UPLOAD_CLIENT_H__

#include <zephyr.h>
#include <zephyr/types.h>
#include <net/coap.h>

/* Lifted from autoconf.h of another build */
#define CONFIG_DOWNLOAD_CLIENT_BUF_SIZE 2048
#define CONFIG_DOWNLOAD_CLIENT_HTTP_FRAG_SIZE 2048
#define CONFIG_DOWNLOAD_CLIENT_HTTP_FRAG_SIZE_2048 1
#define CONFIG_DOWNLOAD_CLIENT_STACK_SIZE 4096
#define CONFIG_DOWNLOAD_CLIENT_MAX_HOSTNAME_SIZE 64
#define CONFIG_DOWNLOAD_CLIENT_MAX_FILENAME_SIZE 192
#define CONFIG_DOWNLOAD_CLIENT_SOCK_TIMEOUT_MS 4000

#define USE_SEC_TAG_ARRAY /* MQ  Allow upload_client lib to accept array of security tags instead of just one. */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Download client event IDs.
 */
enum upload_client_evt_id {
	/**
	 * Event contains a fragment.
	 * The application may return any non-zero value to stop the upload.
	 */
	UPLOAD_CLIENT_EVT_FRAGMENT,
	/**
	 * An error has occurred during download and
	 * the connection to the server has been lost.
	 *
	 * Error reason may be one of the following:
	 * - ECONNRESET: socket error, peer closed connection
	 * - EHOSTDOWN: host went down during download
	 * - EBADMSG: HTTP response header not as expected
	 * - E2BIG: HTTP response header could not fit in buffer
	 *
	 * In case of errors on the socket during send() or recv() (ECONNRESET),
	 * returning zero from the callback will let the library attempt
	 * to reconnect to the server and download the last fragment again.
	 * Otherwise, the application may return any non-zero value
	 * to stop the download.
	 *
	 * In case the upload is stopped, the application should manually
	 * disconnect (@ref upload_client_disconnect) to clean up the
	 * network socket as necessary before re-attempting the upload.
	 */
	UPLOAD_CLIENT_EVT_ERROR,
	/** Upload complete. */
	UPLOAD_CLIENT_EVT_DONE,
};

struct upload_fragment {
	void *buf;
	size_t len;
};

/**
 * @brief Upload client event.
 */
struct upload_client_evt {
	/** Event ID. */
	enum upload_client_evt_id id;

	union {
		/** Error cause. */
		int error;
		/** Fragment data. */
		struct upload_fragment fragment;
	};
};

/**
 * @brief Upload client configuration options.
 */
struct upload_client_cfg {
	/** Access point name identifying a packet data network.
	 *  Pass a null-terminated string with the APN
	 *  or NULL to use the default APN.
	 */
	const char *apn;
	/** Maximum fragment size to download. 0 indicates that Kconfigured
	 *  values shall be used.
	 */
	size_t frag_size_override;
	/** TLS security tag.
	 *  Pass -1 to disable TLS.
	 */
#ifdef USE_SEC_TAG_ARRAY
	int sec_tag_array_sz;
	int sec_tag_array[];
#else
	int sec_tag;
#endif

};

/**
 * @brief Download client asynchronous event handler.
 *
 * Through this callback, the application receives events, such as
 * download of a fragment, download completion, or errors.
 *
 * If the callback returns a non-zero value, the download stops.
 * To resume the download, use @ref upload_client_start().
 *
 * @param[in] event	The event.
 *
 * @return Zero to continue the download, non-zero otherwise.
 */
typedef int (*upload_client_callback_t)(
	struct upload_client_evt *event);

/**
 * @brief Download client instance.
 */
struct upload_client {
	/** Socket descriptor. */
	int fd;
	/** Response buffer. */
	char buf[CONFIG_DOWNLOAD_CLIENT_BUF_SIZE];
	/** Buffer offset. */
	size_t offset;

	/** Size of the file being uploaded, in bytes. */
	size_t file_size;
	/** Upload progress, number of bytes downloaded. */
	size_t progress;

	/** Server hosting the file, null-terminated. */
	const char *host;
	/** File name, null-terminated. */
	const char *file;
#ifndef USE_SEC_TAG_ARRAY	
	/** Configuration options. */
	struct upload_client_cfg config;
#endif	
	/** Protocol for current download. */
	int proto;

	struct  {
		/** Whether the HTTP header for
		 * the current fragment has been processed.
		 */
		bool has_header;
		/** The server has closed the connection. */
		bool connection_close;
	} http;

	struct {
		/** CoAP block context. */
		struct coap_block_context block_ctx;
	} coap;

	/** Internal thread ID. */
	k_tid_t tid;
	/** Internal download thread. */
	struct k_thread thread;
	/* Internal thread stack. */
	K_THREAD_STACK_MEMBER(thread_stack,
			      CONFIG_DOWNLOAD_CLIENT_STACK_SIZE);

	/** Event handler. */
	upload_client_callback_t callback;
	
#ifdef USE_SEC_TAG_ARRAY	
	/** Configuration options. */
	/* MQ moved to end of structure as upload_client_cfg has a flexible array member thus required by C99 to be at the end of the all enclosing superstructures. */
	struct upload_client_cfg config;
#endif
};

/**
 * @brief Initialize the download client.
 *
 * @param[in] client	Client instance.
 * @param[in] callback	Callback function.
 *
 * @retval int Zero on success, otherwise a negative error code.
 */
int upload_client_init(struct upload_client *client,
			 upload_client_callback_t callback);

/**
 * @brief Establish a connection to the server.
 *
 * @param[in] client	Client instance.
 * @param[in] host	Name of the host to connect to, null-terminated.
 *			Can include scheme and port number, defaults to
 *			HTTP or HTTPS if no scheme is provided.
 * @param[in] config	Configuration options.
 *
 * @retval int Zero on success, a negative error code otherwise.
 */
int upload_client_connect(struct upload_client *client, const char *host,
			    const struct upload_client_cfg *config);

/**
 * @brief Download a file.
 *
 * The download is carried out in fragments of up to
 * @option{CONFIG_DOWNLOAD_CLIENT_HTTP_FRAG_SIZE} bytes for HTTP, or
 * @option{CONFIG_DOWNLOAD_CLIENT_COAP_BLOCK_SIZE} bytes for CoAP,
 * which are delivered to the application
 * via @ref DOWNLOAD_CLIENT_EVT_FRAGMENT events.
 *
 * @param[in] client	Client instance.
 * @param[in] file	File to download, null-terminated.
 * @param[in] from	Offset from where to resume the download,
 *			or zero to download from the beginning.
 *
 * @retval int Zero on success, a negative error code otherwise.
 */
int upload_client_start(struct upload_client *client, const char *file,
			  size_t from, size_t file_size);

/**
 * @brief Pause the download.
 *
 * @param[in] client	Client instance.
 */
void upload_client_pause(struct upload_client *client);

/**
 * @brief Resume the download.
 *
 * @param[in] client	Client instance.
 */
void upload_client_resume(struct upload_client *client);

/**
 * @brief Retrieve the size of the file being downloaded, in bytes.
 *
 * The file size is only available after the download has begun.
 *
 * @param[in]  client	Client instance.
 * @param[out] size	File size.
 *
 * @retval int Zero on success, a negative error code otherwise.
 */
int upload_client_file_size_get(struct upload_client *client, size_t *size);

/**
 * @brief Disconnect from the server.
 *
 * @param[in] client	Client instance.
 *
 * @return Zero on success, a negative error code otherwise.
 */
int upload_client_disconnect(struct upload_client *client);

#ifdef __cplusplus
}
#endif

#endif /* UPLOAD_CLIENT_H__ */

/**@} */
