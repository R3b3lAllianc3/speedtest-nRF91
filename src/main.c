/*
 * Copyright (c) 2020 Mark Qureshey
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr.h>
#include <stdlib.h>
#include <modem/bsdlib.h>
#include <modem/lte_lc.h>
#include <modem/at_cmd.h>
#include <modem/at_notif.h>
#include <modem/modem_key_mgmt.h>
#include <device.h>
#include <drivers/gpio.h>
#include <net/tls_credentials.h>
#include <net/socket.h>
#include <stdio.h>
#include <string.h>
#include <fs/fs.h>
#include <fs/littlefs.h>
#include <storage/flash_map.h>
#include <math.h>

#include "download_client_speedtest.h"
#include "upload_client.h"
#include "xread.h"

#define URL_DL_CONFIG_FILE "https://www.speedtest.net/speedtest-config.php"
#define URL_DL_SERVERS_FILE "https://www.speedtest.net/speedtest-servers-static.php?"
#define URL_SPEEDTEST_DOWNLOAD "/speedtest/random3500x3500.jpg"
#define SAVED_SERVER_FILE "speedtest-servers-static.xml"
#define TLS_SEC_TAG_ROOT 42
#define TLS_SEC_TAG_INTERMEDIATE 43
#define CERT_FILE_ROOT "../cert/speedtest_root.pem"
#define CERT_FILE_INTERMEDIATE "../cert/speedtest_intermediate.pem"

#define M_PI 3.14159265358979323846264338327950288

/* Matches LFS_NAME_MAX */
#define MAX_PATH_LEN 255
#define TEXT_DIVIDER_EQ "============================================\n"

#define STARTING_OFFSET 0

#define UPLOAD_FILE_SIZE UPLOAD_AND_DOWNLOAD_SIZE
/* To prevent bandwidth overuse, we limit download size to this limit. */
#define DOWNLOAD_LIMIT UPLOAD_AND_DOWNLOAD_SIZE
#define UPLOAD_AND_DOWNLOAD_SIZE (50 * 1024)

#define SW0_NODE	DT_ALIAS(sw0)

#if DT_NODE_HAS_STATUS(SW0_NODE, okay)
#define SW0_GPIO_LABEL	DT_GPIO_LABEL(SW0_NODE, gpios)
#define SW0_GPIO_PIN	DT_GPIO_PIN(SW0_NODE, gpios)
#define SW0_GPIO_FLAGS	(GPIO_INPUT | DT_GPIO_FLAGS(SW0_NODE, gpios))
#else
#error "Unsupported board: sw0 devicetree alias is not defined"
#define SW0_GPIO_LABEL	""
#define SW0_GPIO_PIN	0
#define SW0_GPIO_FLAGS	0
#endif

/*
 * The led0 devicetree alias is optional. If present, we'll use it
 * to turn on the LED whenever the button is pressed.
 */
#define LED0_NODE	DT_ALIAS(led0)

#if DT_NODE_HAS_STATUS(LED0_NODE, okay) && DT_NODE_HAS_PROP(LED0_NODE, gpios)
#define LED0_GPIO_LABEL	DT_GPIO_LABEL(LED0_NODE, gpios)
#define LED0_GPIO_PIN	DT_GPIO_PIN(LED0_NODE, gpios)
#define LED0_GPIO_FLAGS	(GPIO_OUTPUT | DT_GPIO_FLAGS(LED0_NODE, gpios))
#endif

K_SEM_DEFINE(main_sem, 0, 1);

static const char root_cert[] = {
	#include CERT_FILE_ROOT
};

static const char imm_cert[] = {
	#include CERT_FILE_INTERMEDIATE
};
BUILD_ASSERT(sizeof(root_cert) < KB(4), "Certificate too large");

/* LED helpers, which use the led0 devicetree alias if it's available. */
static const struct device *initialize_led(void);

static struct gpio_callback button_cb_data;

static struct download_client downloader;
/* security tags for HTTPS access to speedtest.net */
static struct download_client_cfg config_security_dl = { .apn = 0,\
											 .frag_size_override = 0, \
											 .sec_tag_array_sz = 2 /* # of items in security tags index list */, \
											 .sec_tag_array = {TLS_SEC_TAG_ROOT, TLS_SEC_TAG_INTERMEDIATE} /* Security tags index list */};

static struct upload_client uploader;
static struct upload_client_cfg config_no_security_ul = { .apn = 0, \
											 .frag_size_override = 0, \
											 .sec_tag_array_sz = 0 /* # of items in security tags index list */, \
											 .sec_tag_array = {0, 0} /* No HTTPS in upload to speedtest.net */};

static struct download_client_cfg config_no_security_dl = { .apn = 0, \
											 .frag_size_override = 0, \
											 .sec_tag_array_sz = 0 /* # of items in security tags index list */, \
											 .sec_tag_array = {0, 0} /* No HTTPS in upload to speedtest.net */};


typedef struct client_data {
    char ip[512];
    double latitude;
    double longitude;
    char isp[128];
} client_data_t;

typedef struct server_data {
    char url[512];
    double latitude;
    double longitude;
    char name[128];
    char country[128];
    double distance;
    //int latency;  //not yet implemented
    //char domain_name[128];
    //struct sockaddr_in servinfo;
} server_data_t;

static client_data_t client_data = {0};
static server_data_t closest_server_data = {0};

static bool file_downloaded = false;
static char server_fname[MAX_PATH_LEN*2];

static struct fs_file_t file;

static int process_downloaded_servers_file(struct fs_file_t *server_file);

static char scratch_buf[CONFIG_DOWNLOAD_CLIENT_BUF_SIZE] = {0};
static char line_buf[512] = {0};

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(storage);
static struct fs_mount_t lfs_storage_mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &storage,
	.storage_dev = (void *)FLASH_AREA_ID(storage),
	.mnt_point = "/lfs",
};
static char mount_point_name[MAX_PATH_LEN];

static int64_t ref_time_download;
static int64_t ref_time_upload;

static const struct device *button = NULL;
static const struct device *led = NULL;

static bool erase_server_list_file = false;

int url_parse_port(const char *url, uint16_t *port);
int url_parse_proto(const char *url, int *proto, int *type);
int url_parse_host(const char *url, char *host, size_t len);
int url_parse_file(const char *url, char *file, size_t len);

void button_pressed(const struct device *dev, struct gpio_callback *cb,
		    uint32_t pins)
{
	//printk("Button pressed at %" PRIu32 "\n", k_cycle_get_32());
	erase_server_list_file = true;
	gpio_pin_set(led, LED0_GPIO_PIN, true);
	return;
}

/* Initialize AT communications */
static int at_comms_init(void)
{
	int err;

	err = at_cmd_init();
	if (err) {
		printk("Failed to initialize AT commands, err %d\n", err);
		return err;
	}

	err = at_notif_init();
	if (err) {
		printk("Failed to initialize AT notifications, err %d\n", err);
		return err;
	}

	return 0;
}

/* Provision certificate to modem */
static int cert_provision(void)
{
	int err;
	bool exists;
	uint8_t unused;

	err = modem_key_mgmt_exists(TLS_SEC_TAG_ROOT,
				    MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
				    &exists, &unused);
	if (err) {
		printk("Failed to check for certificates err %d\n", err);
		return err;
	}

	if (exists) {
		/* For the sake of simplicity we delete what is provisioned
		 * with our security tag and reprovision our certificate.
		 */
		err = modem_key_mgmt_delete(TLS_SEC_TAG_ROOT,
					    MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN);
		if (err) {
			printk("Failed to delete existing certificate, err %d\n",
			       err);
		}
		err = modem_key_mgmt_delete(TLS_SEC_TAG_INTERMEDIATE,
					    MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN);
		if (err) {
			printk("Failed to delete existing certificate, err %d\n",
			       err);
		}
	}

	/*  Provision certificate to the modem */
	err = modem_key_mgmt_write(TLS_SEC_TAG_ROOT,
				   MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
				   root_cert, sizeof(root_cert) - 1);
	if (err) {
		printk("Failed to provision root certificate, err %d\n", err);
		return err;
	}
	/*  Provision certificate to the modem */
	err = modem_key_mgmt_write(TLS_SEC_TAG_INTERMEDIATE,
				   MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
				   imm_cert, sizeof(imm_cert) - 1);
	if (err) {
		printk("Failed to provision immediate certificate, err %d\n", err);
		return err;
	}

	return 0;
}

static void save_client_info(xr_type_t type, const xr_str_t* name, const xr_str_t* val)
{
	if ((type != xr_type_attribute) || (!name) || (!val))
		return;

	if (strncmp(name->cstr, "ip", name->len) == 0) {
		snprintf(client_data.ip, sizeof(client_data.ip), "%.*s", val->len, val->cstr);
	}
	else
	if (strncmp(name->cstr, "lat", name->len) == 0) {
		client_data.latitude = atof(val->cstr);
	}
	else
	if (strncmp(name->cstr, "lon", name->len) == 0) {
		client_data.longitude = atof(val->cstr);
	}
	else
	if (strncmp(name->cstr, "isp", name->len) == 0) {
		snprintf(client_data.isp, sizeof(client_data.isp), "%.*s", val->len, val->cstr);
	}
}

static void xml_parser_handler_config_file(xr_type_t type, const xr_str_t* name, const xr_str_t* val, void* user_data)
{
    switch (type) {
        case xr_type_element_start:
            //printf("element_start: <%.*s>\n", name->len, name->cstr);
            return;
        case xr_type_element_end:
            //printf("element_end: </%.*s>\n", name->len, name->cstr);
            return;
        case xr_type_attribute:
            //printf("type_attribute: %.*s=\"%.*s\"\n", name->len, name->cstr, val->len, val->cstr);
			save_client_info(type, name, val);
            return;
        case xr_type_error:
            //printf("type_error: xml parsing error\n");
            return;
    }
}

static int process_downloaded_config_file(void)
{
	//int rc;
	char *p, *lp;
	char *endp;
	int i;

	p = scratch_buf;
	lp = line_buf;
	
	endp = (scratch_buf + CONFIG_DOWNLOAD_CLIENT_BUF_SIZE);
	while(p < endp) {
		i = 0;
		while ((*p != '\n') && (i < (sizeof(line_buf) - 2)) && (p < endp)) {
			*lp++ = *p++;
			++i;
		}
		if (*p == '\n') { //we have a proper line; process it.
			*lp++ = '\n';
			*lp = '\0';
			lp = line_buf; //reset line buffer ptr to start of the buffer
			++p;
			//printf("\nline_buf = %s", line_buf);
			xr_read(&xml_parser_handler_config_file, line_buf, NULL);
		}
		if (i == (sizeof(line_buf) - 2)) { //line buffer is full
			//printk("Line buffer full; skipping to next line!\n");
			//there is no more space in the dest. line buffer so move to next line in src. buffer.
			while ((p < endp) && (*p++ != '\n'));
			lp = line_buf; //reset line buffer ptr to start of the buffer
		}
		if (p == endp) {
			p = scratch_buf; //reset to start of source buffer
			break; //reload src. buffer
		}
	}

	return 1;
}

/* callback for speedtest-config.php downloading & processing. */
static int callback_for_config_file(const struct download_client_evt *event)
{
	static size_t downloaded;
	static size_t file_size;
	static size_t saved_fragment_len = 0;

	if (downloaded == 0) {
		download_client_file_size_get(&downloader, &file_size);
		downloaded += STARTING_OFFSET;
	}

	switch (event->id) {
		case DOWNLOAD_CLIENT_EVT_FRAGMENT:
			downloaded += event->fragment.len;
			if (saved_fragment_len < sizeof(scratch_buf)) {
				int min = MIN(event->fragment.len, (sizeof(scratch_buf) - saved_fragment_len));
				memcpy(&scratch_buf+saved_fragment_len, event->fragment.buf, min);
				saved_fragment_len += min;
			}
			return 0;

		case DOWNLOAD_CLIENT_EVT_DONE:
		{
			process_downloaded_config_file();

			printf(TEXT_DIVIDER_EQ);
			printf("Your IP Address : %s\n", client_data.ip);
			printf("Your IP Location: %0.4lf, %0.4lf\n", client_data.latitude, client_data.longitude);
			printf("Your ISP        : %s\n", client_data.isp);
			printf(TEXT_DIVIDER_EQ);

			downloaded = 0;
			saved_fragment_len = 0;
			k_sem_give(&main_sem); //signal main to continue
			return 0;
		}

		case DOWNLOAD_CLIENT_EVT_ERROR:
			printk("Error %d during download of configuration data\n", event->error);
			downloaded = 0;
			/* Stop download */
			return -1;
	}

	return 0;
}

/* Haversine formula */
static double calc_dist_haversine(double lat1, double lon1, double lat2, double lon2)
{
    int R = 6371;  //Radius of the Earth
    double dlat, dlon, a, c, d;

    dlat = (lat2-lat1)*M_PI/180;
    dlon = (lon2-lon1)*M_PI/180;

    a = pow(sin(dlat/2), 2) + cos(lat1*M_PI/180)*cos(lat2*M_PI/180)*pow(sin(dlon/2), 2);
    c = 2 * atan2(sqrt(a), sqrt(1-a));
    d = R * c;
    return d;
}

static void calculate_distance(xr_type_t type, const xr_str_t* name, const xr_str_t* val)
{
	static bool first_time = true;
	double distance_calculated;
	static server_data_t server_data_tmp = {0};

	if ((type != xr_type_attribute) || (!name) || (!val))
		return;

	/** Assuming we encounter "url", "lat" then "lon" always in that order **/
	if (strncmp(name->cstr, "url", name->len) == 0) {
		snprintf(server_data_tmp.url, sizeof(server_data_tmp.url), "%.*s", val->len, val->cstr);
		//printf("latitude is %0.4lf\n", lat);
	}
	else
	if (strncmp(name->cstr, "lat", name->len) == 0) {
		server_data_tmp.latitude = atof(val->cstr);
		//printf("latitude is %0.4lf\n", lat);
	}
	else
	if (strncmp(name->cstr, "lon", name->len) == 0) {
		server_data_tmp.longitude = atof(val->cstr);
		distance_calculated = calc_dist_haversine(client_data.latitude, client_data.longitude, server_data_tmp.latitude, server_data_tmp.longitude);
		server_data_tmp.distance = distance_calculated;

		/* At this point, we should have URL, longitude, and latitude for one record so we can calculate distance &
		   save its URL. */

		/* Find and save nearest server. */
		if (first_time) /*first time only*/ {
			memcpy(&closest_server_data, &server_data_tmp, sizeof(server_data_t));
			first_time = false;
		}
		else {
			if (closest_server_data.distance > server_data_tmp.distance)
				memcpy(&closest_server_data, &server_data_tmp, sizeof(server_data_t));
		}

		//printf("longitude is %0.4lf\n", lon);
		//printf("Distance for (%0.4lf, %0.4lf) is %0.4lf\n", lat, lon, distance));
		//printf("Current shortest distance for (%0.4lf, %0.4lf) is %0.4lf @ %s\n", client_data.latitude, client_data.longitude, closest_server_data.distance, closest_server_data.url);
	}
	return;
}

static void xml_parser_handler_servers_file(xr_type_t type, const xr_str_t* name, const xr_str_t* val, void* user_data)
{
    switch (type) {
        case xr_type_element_start:
            //printf("element_start: <%.*s>\n", name->len, name->cstr);
            return;
        case xr_type_element_end:
            //printf("element_end: </%.*s>\n", name->len, name->cstr);
            return;
        case xr_type_attribute:
            //printf("type_attribute: %.*s=\"%.*s\"\n", name->len, name->cstr, val->len, val->cstr);
			calculate_distance(type, name, val);
            return;
        case xr_type_error:
            //printf("type_error: xml parsing error\n");
            return;
    }
}

static int process_downloaded_servers_file(struct fs_file_t *server_file)
{
	int rc;
	char *p, *lp;
	char *endp;
	int i;

	if (!server_file)
		return -1;

	rc = fs_seek(server_file, 0, FS_SEEK_SET);
	if (rc < 0) {
		printk("fs_seek() error: %d\n", rc);
		return -1;
	}

	p = scratch_buf;
	lp = line_buf;
	while((rc = fs_read(server_file, scratch_buf, sizeof(scratch_buf))) > 0) {
		endp = (scratch_buf + rc);
		while(p < endp) {
			i = 0;
			while ((*p != '\n') && (i < (sizeof(line_buf) - 2)) && (p < endp)) {
				*lp++ = *p++;
				++i;
			}
			if (*p == '\n') { //we have a proper line; process it.
				*lp++ = '\n';
				*lp = '\0';
				lp = line_buf; //reset line buffer ptr to start of the buffer
				++p;
				//printf("\nline_buf = %s", line_buf);
				xr_read(&xml_parser_handler_servers_file, line_buf, NULL);
			}
			if (i == (sizeof(line_buf) - 2)) { //line buffer is full
				printk("Line buffer full; skipping to next line!\n");
				//there is no more space in the dest. line buffer so move to next line in src. buffer.
				while ((p < endp) && (*p++ != '\n'));
				lp = line_buf; //reset line buffer ptr to start of the buffer
			}
			if (p == endp) {
				p = scratch_buf; //reset to start of source buffer
				break; //reload src. buffer
			}
		}
	}
	return 1;
}

/* callback for speedtest-servers-static.php downloading & processing. */
static int callback_for_servers_file(const struct download_client_evt *event)
{
	static size_t downloaded;
	static size_t file_size;
	static bool file_open = false;
	int rc;

	if (downloaded == 0) {
		download_client_file_size_get(&downloader, &file_size);
		downloaded += STARTING_OFFSET;
	}

	/* Create and open file for writing. */
	if (!file_open) {
		snprintf(server_fname, sizeof(server_fname), "%s%s", mount_point_name, SAVED_SERVER_FILE);
		rc = fs_open(&file, server_fname, FS_O_WRITE | FS_O_CREATE);
		if (rc < 0) {
			printk("FAIL: open %s: %d\n", server_fname, rc);
			file_open = false;
			return -1;
		}
		file_open = true;
	}

	switch (event->id) {
		case DOWNLOAD_CLIENT_EVT_FRAGMENT: {
			downloaded += event->fragment.len;
			rc = fs_write(&file, event->fragment.buf, event->fragment.len);
			if (rc < 0) {
				printk("Error writing data to file: %d", rc);				
				return -1; //error
			}
			return(0);
		}
		case DOWNLOAD_CLIENT_EVT_DONE: {
			file_open = false;			
			downloaded = 0;
			rc = fs_sync(&file); //flush buffers to flash
			if (rc < 0) {
				printk("Error flushing data to flash: %d", rc);
				return -1; //error
			}
			process_downloaded_servers_file(&file);
			fs_close(&file);
			k_sem_give(&main_sem); //signal main to continue
			return 0;
		}
		case DOWNLOAD_CLIENT_EVT_ERROR: {
			printk("Error %d during download of server list\n", event->error);
			downloaded = 0;
			/* Delete the file as it is invalid. */
			(void)fs_unlink(server_fname);
			/* Stop download */
			return -1;
		}
	}

	return 0;
}

static int callback_for_speed_test(const struct download_client_evt *event)
{
	static size_t downloaded;
	static size_t file_size;
	uint32_t speed;
	int64_t ms_elapsed;

	if (downloaded == 0) {
		download_client_file_size_get(&downloader, &file_size);
		downloaded += STARTING_OFFSET;
	}

	switch (event->id) {
		case DOWNLOAD_CLIENT_EVT_FRAGMENT:
			downloaded += event->fragment.len;
			
			if (downloaded > DOWNLOAD_LIMIT) {
				ms_elapsed = k_uptime_delta(&ref_time_download);
				speed = ((float)downloaded / ms_elapsed) * MSEC_PER_SEC;

				printk("Download: %lld ms @ %d bytes per sec, total %d bytes\n",
							ms_elapsed, speed, downloaded);				
				file_downloaded = true;
				k_sem_give(&main_sem); //signal main to continue
				return 1; //stop
			}

			return 0;

		case DOWNLOAD_CLIENT_EVT_DONE:
			file_downloaded = true;	
			k_sem_give(&main_sem); //signal main to continue	
			return 0;

		case DOWNLOAD_CLIENT_EVT_ERROR:
			printk("Error %d during download\n", event->error);
			downloaded = 0;
			file_downloaded = false;		
			k_sem_give(&main_sem); //signal main to continue
			/* Stop download */
			return -1;
	}

	return 0;
}

static int callback_upload(struct upload_client_evt *event)
{
	static size_t uploaded=0;
	static size_t count=0;
	const size_t file_size = UPLOAD_FILE_SIZE;

	uint32_t speed;
	int64_t ms_elapsed;

	switch (event->id) {
	case UPLOAD_CLIENT_EVT_FRAGMENT:
		//printk("\n Event: UPLOAD_CLIENT_EVT_FRAGMENT #%d, size = %d bytes\n", (count+1), uploaded);
		if (uploaded < file_size) {
			/* Copy send buffer */
			//event->fragment.len =  CONFIG_DOWNLOAD_CLIENT_BUF_SIZE;
			event->fragment.len =  1024;
			event->fragment.buf = scratch_buf;
			uploaded += event->fragment.len;			
			++count;
			return 0;
		} else {
			//printk("\r[ %d bytes ] ", uploaded);
			count = 0;
			return 1; //Stop uploading
		}

	case UPLOAD_CLIENT_EVT_DONE:
		//printk("\n Event: UPLOAD_CLIENT_EVT_DONE\n");
		ms_elapsed = k_uptime_delta(&ref_time_upload);
		speed = ((float)uploaded / ms_elapsed) * MSEC_PER_SEC;

		printk("Upload  : %lld ms @ %d bytes per sec, total %d bytes\n", ms_elapsed, speed, uploaded);
		//printk("Bye\n");
		k_sem_give(&main_sem);
		uploaded = 0;
		return 0;

	case UPLOAD_CLIENT_EVT_ERROR:
		printk("Error %d during upload\n", event->error);
		uploaded = 0;
		/* Stop upload */
		return -1;
	}

	return 0;
}

static int init_button_and_led(void)
{
	int ret;

	button = device_get_binding(SW0_GPIO_LABEL);
	if (button == NULL) {
		printk("Error: didn't find %s device\n", SW0_GPIO_LABEL);
		return 1;
	}

	ret = gpio_pin_configure(button, SW0_GPIO_PIN, SW0_GPIO_FLAGS);
	if (ret != 0) {
		printk("Error %d: failed to configure %s pin %d\n",
		       ret, SW0_GPIO_LABEL, SW0_GPIO_PIN);
		return 1;
	}

	ret = gpio_pin_interrupt_configure(button,
					   SW0_GPIO_PIN,
					   GPIO_INT_EDGE_TO_ACTIVE);
	if (ret != 0) {
		printk("Error %d: failed to configure interrupt on %s pin %d\n",
			ret, SW0_GPIO_LABEL, SW0_GPIO_PIN);
		return 1;
	}

	gpio_init_callback(&button_cb_data, button_pressed, BIT(SW0_GPIO_PIN));
	gpio_add_callback(button, &button_cb_data);
	//printk("Set up button at %s pin %d\n", SW0_GPIO_LABEL, SW0_GPIO_PIN);

	led = initialize_led();

	return 0;
}

#ifdef LED0_GPIO_LABEL
static const struct device *initialize_led(void)
{
	const struct device *led;
	int ret;

	led = device_get_binding(LED0_GPIO_LABEL);
	if (led == NULL) {
		printk("Didn't find LED device %s\n", LED0_GPIO_LABEL);
		return NULL;
	}

	ret = gpio_pin_configure(led, LED0_GPIO_PIN, LED0_GPIO_FLAGS);
	if (ret != 0) {
		printk("Error %d: failed to configure LED device %s pin %d\n",
		       ret, LED0_GPIO_LABEL, LED0_GPIO_PIN);
		return NULL;
	}

	//printk("Set up LED at %s pin %d\n", LED0_GPIO_LABEL, LED0_GPIO_PIN);

	return led;
}

#else  /* !defined(LED0_GPIO_LABEL) */
static const struct device *initialize_led(void)
{
	printk("No LED device was defined\n");
	return NULL;
}

#endif	/* LED0_GPIO_LABEL */

/* Set up Filesystem operations. */
static int setup_flash_filesystem(struct fs_mount_t *mp)
{
	unsigned int id = (uintptr_t)mp->storage_dev;
	struct fs_statvfs sbuf;
	const struct flash_area *pfa;
	int err;

	err = flash_area_open(id, &pfa);
	if (err < 0) {
		printk("FAIL: unable to find flash area %u: %d\n", id, err);
		return 1;
	}
	
	/* Wipe flash contents if button 1 press detected. */
	if (erase_server_list_file == true) {		
		err = flash_area_erase(pfa, 0, pfa->fa_size);
		if (err < 0) {
			printk("FAIL: unable to erase flash area, err %d\n", err);
			return 1;
		}
	}	
	flash_area_close(pfa);

	err = fs_mount(mp);
	if (err < 0) {
		printk("FAIL: mount id %u at %s: %d\n", (unsigned int)mp->storage_dev, mp->mnt_point, err);
		return 1;
	}

	err = fs_statvfs(mp->mnt_point, &sbuf);
	if (err < 0) {
		printk("FAIL: statvfs: %d\n", err);
		return 1;
	}
	/*
	printk("%s: bsize = %lu; frsize = %lu;" " blocks = %lu; bfree = %lu\n",
	       mp->mnt_point,
	       sbuf.f_bsize, sbuf.f_frsize,
	       sbuf.f_blocks, sbuf.f_bfree);
	*/

	snprintf(mount_point_name, sizeof(mount_point_name), "%s/", mp->mnt_point);
	return 0;
}

void main(void)
{
	int err;
	struct fs_mount_t *mp = &lfs_storage_mnt;
	char *p;

	printf("Speedtest for Nordic nRF9160 started\n");

	err = bsdlib_init();
	if (err) {
		printk("Failed to initialize bsdlib!");
		return;
	}

	/* Initialize AT comms in order to provision the certificate */
	err = at_comms_init();
	if (err) {
		printk("Failed to initialize AT communication!");
		return;
	}

	/* Initialize button & LED. */
	err = init_button_and_led();
	if (err) {
		printk("Failed to initialize button or LED!");
		return;
	}

	printf("**Press Button 1 now to erase cached list of servers**\n");

	printk("Provisioning certificates.. ");
	/* Provision certificates before connecting to the network */
	err = cert_provision();
	if (err) {
		return;
	}
	printk("OK\n");

	printk("Waiting for network.. ");
	err = lte_lc_init_and_connect();
	if (err) {
		printk("Failed to connect to the LTE network, err %d\n", err);
		return;
	}
	printk("OK\n");

	printk("Initializing Flash Filesystem.. ");
	err = setup_flash_filesystem(mp);
	if (err) {
		printk("Failed to initialize flash filesystem\n");
		return;
	}
	printk("OK\n");

	/* Download & process speedtest-config.php */
	printf(TEXT_DIVIDER_EQ);
	printk("Getting client information..\n");
	err = download_client_init(&downloader, callback_for_config_file);
	if (err) {
		printk("Failed to initialize the client, err %d", err);
		return;
	}

	err = download_client_connect(&downloader, URL_DL_CONFIG_FILE, &config_security_dl);
	if (err) {
		printk("Failed to connect, err %d", err);
		return;
	}

	ref_time_download = k_uptime_get();

	err = download_client_start(&downloader, URL_DL_CONFIG_FILE, STARTING_OFFSET);
	if (err) {
		printk("Failed to start the downloader, err %d", err);
		return;
	}
	k_sem_take(&main_sem, K_FOREVER);
	download_client_disconnect(&downloader);

	/***********************************************************************/
	/* Download & process speedtest-servers-static.php */
	
	printk("Getting server list..\n");
	snprintf(server_fname, sizeof(server_fname), "%s%s", mount_point_name, SAVED_SERVER_FILE);
	
	/* Check if file exists on the filesystem */
	err = fs_open(&file, server_fname, FS_O_READ);
	if (err < 0) {
		printk("No cached file found. Downloading..\n");

		/* Download & process speedtest-servers-static.php */
		err = download_client_init(&downloader, callback_for_servers_file);
		if (err) {
			printk("Failed to initialize the client, err %d", err);
			return;
		}

		err = download_client_connect(&downloader, URL_DL_SERVERS_FILE, &config_security_dl);
		if (err) {
			printk("Failed to connect, err %d", err);
			return;
		}

		ref_time_download = k_uptime_get();

		err = download_client_start(&downloader, URL_DL_SERVERS_FILE, STARTING_OFFSET);
		if (err) {
			printk("Failed to start the downloader, err %d", err);
			return;
		}
	} else {
		printk("Cached file found. Skipping download.\n");
		process_downloaded_servers_file(&file);
		k_sem_give(&main_sem); //signal main to continue
		fs_close(&file);
	}

	k_sem_take(&main_sem, K_FOREVER);

	download_client_disconnect(&downloader);

	err = url_parse_host(closest_server_data.url, scratch_buf, sizeof(scratch_buf));
	if (err < 0) {
		printk("Invalid data for nearest server\n");
		return;
	}
	
	printf(TEXT_DIVIDER_EQ);
	printf("Nearest server  : %s\n", scratch_buf);
	printf(TEXT_DIVIDER_EQ);

	/***********************************************************************/
	/* Download test */
	printk("Running speed test..\n");
	printf(TEXT_DIVIDER_EQ);

	//Compose URL
	memset(&server_fname[0], 0, sizeof(server_fname));
	p = server_fname;
	strncpy(p, "http://", sizeof(server_fname));
	p += strlen("http://");
	
	//temp
	/*
	err = url_parse_host(closest_server_data.url, p, sizeof(server_fname));
	if (err < 0) {
		printk("Invalid data for nearest server\n");
		return;
	}
	*/
	//temp
	strcpy(p, "speedtest.ccvn.com");

	p = server_fname;
	p += strlen(server_fname);
	strcpy(p, "/speedtest/random3500x3500.jpg");

	err = download_client_init(&downloader, callback_for_speed_test);
	if (err) {
		printk("Failed to initialize the client, err %d", err);
		return;
	}

	err = download_client_connect(&downloader, server_fname, &config_no_security_dl);
	if (err) {
		printk("Failed to connect, err %d", err);
		return;
	}

	ref_time_download = k_uptime_get();

	err = download_client_start(&downloader, server_fname, STARTING_OFFSET);
	if (err) {
		printk("Failed to start the downloader, err %d", err);
		return;
	}

	k_sem_take(&main_sem, K_FOREVER);
	if (!file_downloaded) {
		printk("Error downloading..is %s down??\n", server_fname);
		return;
	}
	download_client_disconnect(&downloader);

	/***********************************************************************/
	/* Upload test */
	memset(&scratch_buf, 0x5A, CONFIG_DOWNLOAD_CLIENT_BUF_SIZE);
	//Compose URL
	memset(&server_fname[0], 0, sizeof(server_fname));
	p = server_fname;
	strncpy(p, "http://", sizeof(server_fname));
	p += strlen("http://");
	
	//temp
	/*
	err = url_parse_host(closest_server_data.url, p, sizeof(server_fname));
	if (err < 0) {
		printk("Invalid data for nearest server\n");
		return;
	}
	*/
	//temp
	strcpy(p, "speedtest.ccvn.com");

	p = server_fname;
	p += strlen(server_fname);
	strcpy(p, "/speedtest/upload.php");

	err = upload_client_init(&uploader, callback_upload);
	if (err) {
		printk("Failed to initialize the client, err %d", err);
		return;
	}

	err = upload_client_connect(&uploader, server_fname, &config_no_security_ul);
	if (err) {
		printk("Failed to connect, err %d", err);
		return;
	}

	ref_time_upload = k_uptime_get();

	err = upload_client_start(&uploader, server_fname, STARTING_OFFSET, UPLOAD_FILE_SIZE);
	if (err) {
		printk("Failed to start the uploader, err %d", err);
		return;
	}

	k_sem_take(&main_sem, K_FOREVER);
	printf(TEXT_DIVIDER_EQ);
	upload_client_disconnect(&uploader);
	/***********************************************************************/

	err = fs_unmount(mp);
	//printk("%s unmount: %d\n", mp->mnt_point, err);
	printf("Speedtest for Nordic nRF9160 finished\n");
	return;
}
