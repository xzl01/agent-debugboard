#ifndef LINKR_DEBUGGER_OTA_TEST_STUBS_H_
#define LINKR_DEBUGGER_OTA_TEST_STUBS_H_

#include <stdbool.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define ARG_UNUSED(value) (void)(value)
#define BOOT_UPGRADE_TEST 1
#define HTTP_SERVER_REGISTER_HEADER_CAPTURE(name, value)
#define K_FOREVER 0
#define K_MSEC(value) (value)
#define LOG_ERR(...)
#define LOG_INF(...)
#define LOG_MODULE_REGISTER(...)
#define LOG_WRN(...)
#define SYS_REBOOT_COLD 0
#define snprintk snprintf
#define vsnprintk test_vsnprintk

enum http_status {
	HTTP_200_OK = 200,
	HTTP_201_CREATED = 201,
	HTTP_202_ACCEPTED = 202,
	HTTP_400_BAD_REQUEST = 400,
	HTTP_404_NOT_FOUND = 404,
	HTTP_405_METHOD_NOT_ALLOWED = 405,
	HTTP_409_CONFLICT = 409,
	HTTP_413_PAYLOAD_TOO_LARGE = 413,
	HTTP_415_UNSUPPORTED_MEDIA_TYPE = 415,
	HTTP_500_INTERNAL_SERVER_ERROR = 500,
};

enum http_transaction_status {
	HTTP_SERVER_REQUEST_DATA_MORE,
	HTTP_SERVER_REQUEST_DATA_FINAL,
	HTTP_SERVER_TRANSACTION_COMPLETE,
	HTTP_SERVER_TRANSACTION_ABORTED,
};

enum http_header_status {
	HTTP_HEADER_STATUS_OK,
	HTTP_HEADER_STATUS_DROPPED,
};

enum http_method {
	HTTP_GET,
	HTTP_POST,
};

struct http_header {
	const char *name;
	const char *value;
};

struct http_request_ctx {
	const struct http_header *headers;
	size_t header_count;
	enum http_header_status headers_status;
	const uint8_t *data;
	size_t data_len;
};

struct http_response_ctx {
	enum http_status status;
	const uint8_t *body;
	size_t body_len;
	bool final_chunk;
};

struct http_client_ctx {
	uint8_t buffer[512];
	uint8_t url_buffer[128];
	enum http_method method;
};

struct k_mutex {
	pthread_mutex_t native;
	bool initialized;
};

struct k_work {
	void (*handler)(struct k_work *work);
};

struct k_work_delayable {
	struct k_work work;
};

struct flash_area {
	size_t fa_size;
};

struct flash_img_context {
	const struct flash_area *flash_area;
};

struct flash_img_check {
	const uint8_t *match;
	size_t clen;
};

struct mcuboot_img_header {
	uint8_t mcuboot_version;
	struct {
		struct {
			uint32_t image_size;
		} v1;
	} h;
};

struct linkr_debugger_watchdog_status {
	bool supported;
	bool armed;
	bool healthy;
};

void k_mutex_init(struct k_mutex *mutex);
void k_mutex_lock(struct k_mutex *mutex, int timeout);
void k_mutex_unlock(struct k_mutex *mutex);
void k_work_init_delayable(struct k_work_delayable *work,
			   void (*handler)(struct k_work *work));
int k_work_reschedule(struct k_work_delayable *work, int delay_ms);
int k_work_cancel_delayable(struct k_work_delayable *work);

uint8_t flash_img_get_upload_slot(void);
uint32_t boot_get_image_start_offset(uint8_t area_id);
int flash_area_open(uint8_t area_id, const struct flash_area **area);
void flash_area_close(const struct flash_area *area);
int flash_img_init(struct flash_img_context *context);
int flash_img_buffered_write(struct flash_img_context *context,
			     const uint8_t *data, size_t len, bool final);
int flash_img_check(struct flash_img_context *context,
		    const struct flash_img_check *check, uint8_t area_id);
int boot_read_bank_header(uint8_t area_id, struct mcuboot_img_header *header,
			  size_t header_size);
int mcuboot_swap_type(void);
bool boot_is_img_confirmed(void);
int boot_write_img_confirmed(void);
int boot_request_upgrade(int upgrade_type);
void sys_reboot(int type);

const char *linkr_debugger_json_schema(void);
bool linkr_debugger_watchdog_ota_test_marker_present(void);
void linkr_debugger_watchdog_ota_test_marker_set(void);
void linkr_debugger_watchdog_ota_test_marker_clear(void);
bool linkr_debugger_watchdog_fault_injection_confirm_begin(void);
void linkr_debugger_watchdog_fault_injection_confirm_end(void);
void linkr_debugger_watchdog_status_get(struct linkr_debugger_watchdog_status *status);
int linkr_debugger_watchdog_prepare_planned_reboot(void);
int test_vsnprintk(char *buffer, size_t size, const char *format, va_list args);

#endif
