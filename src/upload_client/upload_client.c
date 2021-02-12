/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <stdio.h>
#include <string.h>
#include <zephyr.h>
#include <zephyr/types.h>
#include <toolchain/common.h>
#include <net/socket.h>
#include <nrf_socket.h>
#include <net/tls_credentials.h>
#include "upload_client.h"


#define SIN6(A) ((struct sockaddr_in6 *)(A))
#define SIN(A) ((struct sockaddr_in *)(A))

#define HOSTNAME_SIZE CONFIG_DOWNLOAD_CLIENT_MAX_HOSTNAME_SIZE
#define FILENAME_SIZE CONFIG_DOWNLOAD_CLIENT_MAX_FILENAME_SIZE

int url_parse_port(const char *url, uint16_t *port);
int url_parse_proto(const char *url, int *proto, int *type);
int url_parse_host(const char *url, char *host, size_t len);
int url_parse_file(const char *url, char *file, size_t len);

int http_parse(struct upload_client *client, size_t len);
static int http_post_request_send(struct upload_client *client);
static int socket_send(const int fd, const char* buf, size_t len);

static int socket_timeout_set(int fd)
{
	int err;

	if (CONFIG_DOWNLOAD_CLIENT_SOCK_TIMEOUT_MS == SYS_FOREVER_MS) {
		return 0;
	}

	const uint32_t timeout_ms = CONFIG_DOWNLOAD_CLIENT_SOCK_TIMEOUT_MS;

	struct timeval timeo = {
		.tv_sec = (timeout_ms / 1000),
		.tv_usec = (timeout_ms % 1000) * 1000,
	};

	err = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeo, sizeof(timeo));
	if (err) {
		printk("Failed to set socket timeout, errno %d", errno);
		return -errno;
	}

	return 0;
}

#ifdef USE_SEC_TAG_ARRAY
static int socket_sectag_set(int fd, uint32_t* sec_tag_array, int sec_tag_array_sz)
#else
static int socket_sectag_set(int fd, int sec_tag)	
#endif
{
	int err;
	int verify;
#ifndef USE_SEC_TAG_ARRAY
	sec_tag_t sec_tag_list[] = { sec_tag };
#endif

	enum {
		NONE = 0,
		OPTIONAL = 1,
		REQUIRED = 2,
	};

	verify = REQUIRED;

	err = setsockopt(fd, SOL_TLS, TLS_PEER_VERIFY, &verify, sizeof(verify));
	if (err) {
		printk("Failed to setup peer verification, errno %d", errno);
		return -errno;
	}

#ifdef USE_SEC_TAG_ARRAY
	//LOG_INF("Setting up TLS credentials array");
	err = setsockopt(fd, SOL_TLS, TLS_SEC_TAG_LIST, sec_tag_array,
			 sizeof(sec_tag_t) * sec_tag_array_sz);
#else
	err = setsockopt(fd, SOL_TLS, TLS_SEC_TAG_LIST, sec_tag_list,
			 sizeof(sec_tag_t) * ARRAY_SIZE(sec_tag_list));
#endif	
	if (err) {
		printk("Failed to setup socket security tag, errno %d", errno);
		return -errno;
	}

	return 0;
}

static int socket_apn_set(int fd, const char *apn)
{
	int err;
	size_t len;

	__ASSERT_NO_MSG(apn);

	len = strlen(apn);
	if (len >= IFNAMSIZ) {
		printk("Access point name is too long.");
		return -EINVAL;
	}


	err = setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, apn, len);
	if (err) {
		printk("Failed to bind socket to network, err %d", errno);
		return -ENETUNREACH;
	}

	return 0;
}

static int host_lookup(const char *host, int family, const char *apn,
		       struct sockaddr *sa)
{
	int err;
	struct addrinfo *ai;
	char hostname[HOSTNAME_SIZE];

	struct addrinfo hints = {
		.ai_family = family,
		.ai_next = apn ?
			&(struct addrinfo) {
				.ai_family    = AF_LTE,
				.ai_socktype  = SOCK_MGMT,
				.ai_protocol  = NPROTO_PDN,
				.ai_canonname = (char *)apn
			} : NULL,
	};

	/* Extract the hostname, without protocol or port */
	err = url_parse_host(host, hostname, sizeof(hostname));
	if (err) {
		return err;
	}

	err = getaddrinfo(hostname, NULL, &hints, &ai);
	if (err) {
		printk("Failed to resolve hostname\n");
		return -EHOSTUNREACH;
	}

	*sa = *(ai->ai_addr);
	freeaddrinfo(ai);

	return 0;
}

static int client_connect(struct upload_client *dl, const char *host,
			  struct sockaddr *sa, int *fd)
{
	int err;
	int type;
	uint16_t port;
	socklen_t addrlen;

	err = url_parse_proto(host, &dl->proto, &type);
	if (err) {
		printk("Protocol not specified, defaulting to HTTP(S)");
		type = SOCK_STREAM;
#ifdef USE_SEC_TAG_ARRAY
		if (dl->config.sec_tag_array != NULL) {
#else
		if (dl->config.sec_tag != -1) {
#endif			
			dl->proto = IPPROTO_TLS_1_2;
		} else {
			dl->proto = IPPROTO_TCP;
		}
	}

	if (dl->proto == IPPROTO_UDP || dl->proto == IPPROTO_DTLS_1_2) {
		if (!IS_ENABLED(CONFIG_COAP)) {
			return -EPROTONOSUPPORT;
		}
	}

	if (dl->proto == IPPROTO_TLS_1_2 || dl->proto == IPPROTO_DTLS_1_2) {
#ifdef USE_SEC_TAG_ARRAY
		if (dl->config.sec_tag_array == NULL) {
#else
		if (dl->config.sec_tag == -1) {		
#endif	
			printk("No security tag provided for TLS/DTLS");
			return -EINVAL;
		}
	}

	err = url_parse_port(host, &port);
	if (err) {
		switch (dl->proto) {
		case IPPROTO_TLS_1_2:
			port = 443;
			break;
		case IPPROTO_TCP:
			port = 80;
			break;
		case IPPROTO_DTLS_1_2:
			port = 5684;
			break;
		case IPPROTO_UDP:
			port = 5683;
			break;
		}
		//printk("Port not specified, using default: %d", port);
	}

	switch (sa->sa_family) {
	case AF_INET6:
		SIN6(sa)->sin6_port = htons(port);
		addrlen = sizeof(struct sockaddr_in6);
		break;
	case AF_INET:
		SIN(sa)->sin_port = htons(port);
		addrlen = sizeof(struct sockaddr_in);
		break;
	default:
		return -EAFNOSUPPORT;
	}

	*fd = socket(sa->sa_family, type, dl->proto);
	if (*fd < 0) {
		printk("Failed to create socket, err %d", errno);
		return -errno;
	}

	if (dl->config.apn != NULL && strlen(dl->config.apn)) {
		err = socket_apn_set(*fd, dl->config.apn);
		if (err) {
			goto cleanup;
		}
	}

	if ((dl->proto == IPPROTO_TLS_1_2 || dl->proto == IPPROTO_DTLS_1_2)
#ifdef USE_SEC_TAG_ARRAY
	     && (dl->config.sec_tag_array != NULL)) {
		err = socket_sectag_set(*fd, dl->config.sec_tag_array, dl->config.sec_tag_array_sz);
#else
		 && (dl->config.sec_tag != -1)) {
		err = socket_sectag_set(*fd, dl->config.sec_tag);		
#endif
		if (err) {
			goto cleanup;
		}
	}

	//LOG_INF("Connecting to %s", log_strdup(host));
	//LOG_DBG("fd %d, addrlen %d, fam %s, port %d", *fd, addrlen, str_family(sa->sa_family), port);

	err = connect(*fd, sa, addrlen);
	if (err) {
		//LOG_ERR("Unable to connect, errno %d", errno);
		printk("Unable to connect, errno %d\n", errno);
		err = -errno;
	}

cleanup:
	if (err) {
		/* Unable to connect, close socket */
		close(*fd);
		*fd = -1;
	}

	return err;
}

static int socket_send(const int fd, const char* buf, size_t len)
{
	int sent;
	size_t off = 0;

	while (len) {
		sent = send(fd, buf + off, len, 0);
		if (sent <= 0) {
			return -errno;
		}

		off += sent;
		len -= sent;
	}

	return 0;
}

#define POST_HTTPS_TEMPLATE_PREAMBLE                                      \
		"POST /%s HTTP/1.1\r\n"                                                 \
		"Host: %s\r\n"                                                         \
		"User-Agent: nRF91/0.1\r\n"                                           \
		"Accept: */*\r\n"															\
		"Content-Length: %d\r\n"													\
		"Content-Type: multipart/form-data; "									\
		"boundary=------------------------76a17771c6949e06\r\n\r\n"						
	#define POST_HTTPS_TEMPLATE_MIDAMBLE									\
		"------------------------76a17771c6949e06\r\n"								\
		"Content-Disposition: form-data; name=\"filename\"; filename=\"test5.dat\"\r\n" \
		"Content-Type: application/octet-stream\r\n\r\n"								

	#define POST_HTTPS_TEMPLATE_POSTAMBLE										\
		"--------------------------76a17771c6949e06--\r\n"                         

static int http_post_request_send(struct upload_client *client)
{
	int err;
	int len;
	//size_t off;
	char host[HOSTNAME_SIZE];
	char file[FILENAME_SIZE];

	__ASSERT_NO_MSG(client->host);
	__ASSERT_NO_MSG(client->file);

	err = url_parse_host(client->host, host, sizeof(host));
	if (err) {
		return err;
	}

	err = url_parse_file(client->file, file, sizeof(file));
	if (err) {
		return err;
	}

	/* We use range requests only for HTTPS, due to memory limitations.
	 * When using HTTP, we request the whole resource to minimize
	 * network usage (only one request/response are sent).
	 */
	if (client->proto == IPPROTO_TLS_1_2) {
		len = snprintf(client->buf,
			CONFIG_DOWNLOAD_CLIENT_BUF_SIZE,
			//POST_HTTPS_TEMPLATE, file, host, client->progress, off);
			POST_HTTPS_TEMPLATE_PREAMBLE, file, host, (client->file_size+208));
	} else {
		len = snprintf(client->buf,
			CONFIG_DOWNLOAD_CLIENT_BUF_SIZE,
			POST_HTTPS_TEMPLATE_PREAMBLE, file, host, (client->file_size+208));
	}

	if (len < 0 || len > CONFIG_DOWNLOAD_CLIENT_BUF_SIZE) {
		printk("Cannot create GET request, buffer too small");
		return -ENOMEM;
	}

#if 0
	if (IS_ENABLED(CONFIG_DOWNLOAD_CLIENT_LOG_HEADERS)) {
		//LOG_HEXDUMP_DBG(client->buf, len, "HTTP request");
	}
#endif

	/* Send preamble*/
	err = socket_send(client->fd, client->buf, len);
	if (err) {
		printk("Failed to send HTTP POST pre-amble, errno %d", errno);
		return err;
	}

	len = snprintf(client->buf,
			CONFIG_DOWNLOAD_CLIENT_BUF_SIZE,
			POST_HTTPS_TEMPLATE_MIDAMBLE);
	
	if (len < 0 || len > CONFIG_DOWNLOAD_CLIENT_BUF_SIZE) {
		printk("Cannot create GET request, buffer too small");
		return -ENOMEM;
	}

	/* Send mid-amble*/
	err = socket_send(client->fd, client->buf, len);
	if (err) {
		printk("Failed to send HTTP POST mid-amble, errno %d", errno);
		return err;
	}

	return 0;
}

static int request_send(struct upload_client *dl)
{
	switch (dl->proto) {
		case IPPROTO_TCP:
		case IPPROTO_TLS_1_2: {
			return http_post_request_send(dl);
		}
		case IPPROTO_UDP:
		case IPPROTO_DTLS_1_2:
			break;
	}

	return 0;
}

static int fragment_evt_send(struct upload_client *client, struct upload_client_evt *evt)
{
	__ASSERT(client->offset <= CONFIG_DOWNLOAD_CLIENT_BUF_SIZE,
		 "Buffer overflow!");

	evt->id = UPLOAD_CLIENT_EVT_FRAGMENT;
	evt->fragment.buf = 0;
	evt->fragment.len = 0;	
	return client->callback(evt);
}

void upload_thread(void *client, void *a, void *b)
{
	int rc = 0;
	size_t len;
	struct upload_client *const ul = client;
	struct upload_client_evt upload_fragment_evt;
	struct upload_client_evt evt_done = {
				.id = UPLOAD_CLIENT_EVT_DONE,
			};
	struct upload_client_evt evt_err = {
				.id = UPLOAD_CLIENT_EVT_ERROR,
			};
restart_and_suspend:
	k_thread_suspend(ul->tid);

		/* Ask for buffer from the application.
		 * If the application callback returns non-zero, stop.
		 */
		
		while((rc = fragment_evt_send(ul,  &upload_fragment_evt)) == 0) {
			/* Send out buffer. */
			rc = socket_send(ul->fd, upload_fragment_evt.fragment.buf, upload_fragment_evt.fragment.len);
			if (rc) {				
				printk("Failed to send upload data, errno %d", errno);
				goto restart_and_suspend;
			}
		}

		/* Send postamble. */
		len = snprintf(ul->buf,
			CONFIG_DOWNLOAD_CLIENT_BUF_SIZE,
			POST_HTTPS_TEMPLATE_POSTAMBLE);
	
		if (len < 0 || len > CONFIG_DOWNLOAD_CLIENT_BUF_SIZE) {
			printk("Cannot create postamble request, buffer too small\n");
			ul->callback(&evt_err);
			goto restart_and_suspend; 
		}
		rc = socket_send(ul->fd, ul->buf, len);
		if (rc) {
			printk("Failed to send upload postamble data, errno %d", errno);
			ul->callback(&evt_err);
			goto restart_and_suspend; 
		}

		
		ul->callback(&evt_done);
		
	/* Do not let the thread return, since it can't be restarted */
	goto restart_and_suspend;
}

int upload_client_init(struct upload_client *const client,
			 upload_client_callback_t callback)
{
	if (client == NULL || callback == NULL) {
		return -EINVAL;
	}

	client->fd = -1;
	client->callback = callback;

	/* The thread is spawned now, but it will suspend itself;
	 * it is resumed when the upload is started via the API.
	 */
	client->tid =
		k_thread_create(&client->thread, client->thread_stack,
				K_THREAD_STACK_SIZEOF(client->thread_stack),
				upload_thread, client, NULL, NULL,
				K_LOWEST_APPLICATION_THREAD_PRIO, 0, K_NO_WAIT);

	return 0;
}

int upload_client_connect(struct upload_client *client, const char *host,
			    const struct upload_client_cfg *config)
{
	int err;
	struct sockaddr sa;

	if (client == NULL || host == NULL || config == NULL) {
		return -EINVAL;
	}

	if (client->fd != -1) {
		/* Already connected */
		return 0;
	}

#if 0
	if (config->frag_size_override > CONFIG_DOWNLOAD_CLIENT_BUF_SIZE) {
		//LOG_ERR("The configured fragment size is larger than buffer");
		return -E2BIG;
	}
#endif

	/* Attempt IPv6 connection if configured, fallback to IPv4 */
	if (IS_ENABLED(CONFIG_DOWNLOAD_CLIENT_IPV6)) {
		err = host_lookup(host, AF_INET6, config->apn, &sa);
	}
	if (err || !IS_ENABLED(CONFIG_DOWNLOAD_CLIENT_IPV6)) {
		err = host_lookup(host, AF_INET, config->apn, &sa);
	}

	if (err) {
		return err;
	}

	client->config = *config; /* Shallow copy primitives. */
#ifdef USE_SEC_TAG_ARRAY
	/* Deep copy array fields */
	for (int i=0; i < client->config.sec_tag_array_sz; ++i) {
		client->config.sec_tag_array[i] = config->sec_tag_array[i];
	}
#endif
	client->host = host;

	err = client_connect(client, host, &sa, &client->fd);
	if (client->fd < 0) {
		return err;
	}

	/* Set socket timeout, if configured */
	err = socket_timeout_set(client->fd);
	if (err) {
		return err;
	}

	return 0;
}

int upload_client_disconnect(struct upload_client *const client)
{
	int err;

	if (client == NULL || client->fd < 0) {
		return -EINVAL;
	}

	err = close(client->fd);
	if (err) {
		printk("Failed to close socket, errno %d", errno);
		return -errno;
	}

	client->fd = -1;

	return 0;
}

int upload_client_start(struct upload_client *client, const char *file,
			  size_t from, size_t file_size)
{
	int err;

	if (client == NULL) {
		return -EINVAL;
	}

	if (client->fd < 0) {
		return -ENOTCONN;
	}

	client->file = file;
	client->file_size = file_size;
	client->progress = from;

	client->offset = 0;
	client->http.has_header = false;

	err = request_send(client);
	if (err) {
		return err;
	}

	//LOG_INF("Downloading: %s [%u]", log_strdup(client->file),	client->progress);

	/* Let the thread run */
	k_thread_resume(client->tid);

	return 0;
}

void upload_client_pause(struct upload_client *client)
{
	k_thread_suspend(client->tid);
}

void upload_client_resume(struct upload_client *client)
{
	k_thread_resume(client->tid);
}

int upload_client_file_size_get(struct upload_client *client, size_t *size)
{
	if (!client || !size) {
		return -EINVAL;
	}

	*size = client->file_size;

	return 0;
}
