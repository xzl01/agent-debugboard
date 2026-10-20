#ifndef LINKR_DEBUGGER_HTTP_NET_TEST_STUBS_H_
#define LINKR_DEBUGGER_HTTP_NET_TEST_STUBS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Keep this host test focused on the HTTP callback implementation. */
#define RADXA_LINKR_DEBUGGER_REBOOT_H_
#define RADXA_LINKR_DEBUGGER_HTTP_H_
#define RADXA_LINKR_DEBUGGER_BUILD_INFO_H_
#define RADXA_LINKR_DEBUGGER_CAPTIVE_PORTAL_H_
#define RADXA_LINKR_DEBUGGER_CAPTURE_ARBITER_H_
#define RADXA_LINKR_DEBUGGER_CONTROL_H_
#define RADXA_LINKR_DEBUGGER_GPIO_ERROR_H_
#define RADXA_LINKR_DEBUGGER_CONFIG_HTTP_H_
#define RADXA_LINKR_DEBUGGER_FLASH_ARBITER_H_
#define RADXA_LINKR_DEBUGGER_CONFIG_SUMMARY_H_
#define RADXA_LINKR_DEBUGGER_HTTP_TASK_RESPONSE_H_
#define RADXA_LINKR_DEBUGGER_JSON_CURSOR_H_
#define RADXA_LINKR_DEBUGGER_TASK_HTTP_H_
#define RADXA_LINKR_DEBUGGER_TASK_CATALOG_H_
#define RADXA_LINKR_DEBUGGER_MONITORING_H_
#define RADXA_LINKR_DEBUGGER_MODEL_H_
#define RADXA_LINKR_DEBUGGER_NETWORK_H_
#define RADXA_LINKR_DEBUGGER_OTA_H_
#define RADXA_LINKR_DEBUGGER_PD_H_
#define RADXA_LINKR_DEBUGGER_SHELL_H_
#define RADXA_LINKR_DEBUGGER_WS_H_

#define CONFIG_LINKR_DEBUGGER_LOG_LEVEL 0
#define CONFIG_HTTP_SERVER_MAX_CLIENTS 1

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define ARG_UNUSED(value) ((void)(value))
#define BIT(bit) (1UL << (bit))
#define BIT64(bit) (UINT64_C(1) << (bit))
#define IS_ENABLED(...) 0
#define K_FOREVER 0
#define K_MSEC(ms) (ms)
#define LOG_LEVEL_INF 0
#define LOG_MODULE_REGISTER(...)
#define LOG_WRN(...)
#define snprintk snprintf
#define vsnprintk vsnprintf

struct k_mutex { int unused; };
struct k_work { int unused; };
struct k_work_delayable { struct k_work work; };
static inline void k_mutex_init(struct k_mutex *mutex) { ARG_UNUSED(mutex); }
static inline void k_mutex_lock(struct k_mutex *mutex, int timeout)
{
	ARG_UNUSED(mutex);
	ARG_UNUSED(timeout);
}
static inline void k_mutex_unlock(struct k_mutex *mutex) { ARG_UNUSED(mutex); }
static inline void k_work_init_delayable(struct k_work_delayable *work,
					 void (*handler)(struct k_work *))
{
	ARG_UNUSED(work);
	ARG_UNUSED(handler);
}
static inline int k_work_reschedule(struct k_work_delayable *work, int timeout)
{
	ARG_UNUSED(work);
	ARG_UNUSED(timeout);
	return 0;
}
static inline int64_t k_uptime_get(void) { return 0; }

enum http_method {
	HTTP_DELETE = 0,
	HTTP_GET = 1,
	HTTP_HEAD = 2,
	HTTP_POST = 3,
	HTTP_PUT = 4,
};

enum http_status {
	HTTP_200_OK = 200,
	HTTP_201_CREATED = 201,
	HTTP_202_ACCEPTED = 202,
	HTTP_302_FOUND = 302,
	HTTP_400_BAD_REQUEST = 400,
	HTTP_403_FORBIDDEN = 403,
	HTTP_404_NOT_FOUND = 404,
	HTTP_405_METHOD_NOT_ALLOWED = 405,
	HTTP_409_CONFLICT = 409,
	HTTP_413_PAYLOAD_TOO_LARGE = 413,
	HTTP_500_INTERNAL_SERVER_ERROR = 500,
	HTTP_503_SERVICE_UNAVAILABLE = 503,
};

enum http_transaction_status {
	HTTP_SERVER_TRANSACTION_ABORTED = -1,
	HTTP_SERVER_REQUEST_DATA_MORE = 0,
	HTTP_SERVER_REQUEST_DATA_FINAL = 1,
	HTTP_SERVER_TRANSACTION_COMPLETE = 2,
};

struct http_header {
	const char *name;
	const char *value;
};

struct http_request_ctx {
	char *data;
	size_t data_len;
};

struct http_response_ctx {
	enum http_status status;
	const struct http_header *headers;
	size_t header_count;
	const uint8_t *body;
	size_t body_len;
	bool final_chunk;
};

struct http_client_ctx {
	enum http_method method;
	char url_buffer[256];
	uint8_t buffer[512];
	int fd;
	int server_state;
};

struct http_resource_detail_common {
	int type;
	unsigned long bitmask_of_supported_http_methods;
	const char *content_encoding;
	const char *content_type;
};

struct http_resource_detail_dynamic {
	struct http_resource_detail_common common;
	int (*cb)(struct http_client_ctx *, enum http_transaction_status,
		  const struct http_request_ctx *, struct http_response_ctx *, void *);
	void *user_data;
	struct http_client_ctx *holder;
};

struct http_resource_detail_static {
	struct http_resource_detail_common common;
	const uint8_t *static_data;
	size_t static_data_len;
};

struct http_resource_detail_websocket {
	struct http_resource_detail_common common;
	int (*cb)(int, struct http_request_ctx *, void *);
	uint8_t *data_buffer;
	size_t data_buffer_len;
	void *user_data;
};

struct linkr_debugger_http_service_stub { int *fd; };

#define HTTP_RESOURCE_TYPE_DYNAMIC 1
#define HTTP_RESOURCE_TYPE_STATIC 2
#define HTTP_RESOURCE_TYPE_WEBSOCKET 3
#define HTTP_RESOURCE_DEFINE(...)
#define HTTP_SERVICE_DEFINE(name, ...) \
	struct linkr_debugger_http_service_stub name = { 0 }

enum json_tokens { JSON_TOK_STRING_BUF, JSON_TOK_NUMBER };
struct json_obj_descr { size_t offset; enum json_tokens type; };
#define JSON_OBJ_DESCR_PRIM(type_, field_, token_) \
	{ offsetof(type_, field_), token_ }
int64_t json_obj_parse(char *payload, size_t len,
		       const struct json_obj_descr *descr, size_t descr_len,
		       void *value);

struct sensor_value { int32_t val1; int32_t val2; };

struct linkr_debugger_rail_desc {
	const char *name;
	const char *signal;
	uint8_t pin;
	bool controllable;
};
enum linkr_debugger_adc_kind {
	LINKR_DEBUGGER_ADC_KIND_CURRENT,
	LINKR_DEBUGGER_ADC_KIND_VOLTAGE,
};
struct linkr_debugger_current_desc {
	const char *name;
	const char *signal;
	const char *sensor;
	enum linkr_debugger_adc_kind kind;
	size_t adc_index;
};
struct linkr_debugger_safe_gpio_desc {
	uint8_t pin;
	const char *note;
	const char *layout_group;
	const char *layout_label;
	uint8_t layout_row;
	uint8_t layout_column;
};
struct linkr_debugger_current_sample {
	bool rail_enabled;
	bool raw_available;
	bool voltage_valid;
	int32_t raw;
	int32_t mv;
	int32_t voltage_uv;
	int32_t current_ua;
	struct sensor_value value;
};
struct linkr_debugger_watchdog_status {
	bool supported;
	bool armed;
	bool automatic;
	bool healthy;
	uint32_t timeout_ms;
	bool bootloader_on_timeout;
	bool fault_injection_available;
	bool fault_injection_armed;
	const char *failing_service;
};

enum linkr_debugger_sd_route {
	LINKR_DEBUGGER_SD_ROUTE_TARGET,
	LINKR_DEBUGGER_SD_ROUTE_USB_READER,
};
enum linkr_debugger_usb_route {
	LINKR_DEBUGGER_USB_ROUTE_PC,
	LINKR_DEBUGGER_USB_ROUTE_TARGET,
};
enum linkr_debugger_tf_wp_route { LINKR_DEBUGGER_TF_WP_ROUTE_WRITABLE };

#define LINKR_DEBUGGER_PD_MAX_SOURCE_PROFILES 10U
#define LINKR_DEBUGGER_PD_VOLTAGE_20V 20000U
enum linkr_debugger_pd_mode {
	LINKR_DEBUGGER_PD_MODE_FIXED,
	LINKR_DEBUGGER_PD_MODE_PPS,
};
struct linkr_debugger_pd_fixed_profile { uint16_t voltage_mv; uint16_t max_current_ma; };
struct linkr_debugger_pd_programmable_range {
	uint16_t min_voltage_mv;
	uint16_t max_voltage_mv;
	uint16_t step_mv;
	uint16_t max_current_ma;
	uint16_t max_power_w;
};
struct linkr_debugger_pd_status {
	bool available;
	enum linkr_debugger_pd_mode mode;
	unsigned int protocol;
	bool bc, qc2, qc3, pd_active, epr, avs;
	uint8_t gear;
	uint16_t voltage_mv, pps_mv, max_current_ma;
	bool capabilities_valid;
	uint8_t fixed_count, pps_count, avs_count;
	struct linkr_debugger_pd_fixed_profile fixed[LINKR_DEBUGGER_PD_MAX_SOURCE_PROFILES];
	struct linkr_debugger_pd_programmable_range pps[LINKR_DEBUGGER_PD_MAX_SOURCE_PROFILES];
	struct linkr_debugger_pd_programmable_range avs_ranges[LINKR_DEBUGGER_PD_MAX_SOURCE_PROFILES];
	int last_error;
};

struct linkr_debugger_monitoring_pressure {
	bool available;
	const char *reason;
	const char *coverage;
	const char *limiting_component;
	const char *limiting_name;
	uint32_t pressure_pct_x100;
	uint32_t tie_count;
};
struct linkr_debugger_monitoring_snapshot {
	struct {
		bool available;
		const char *reason, *source;
		int32_t celsius_val1, celsius_val2;
		int error;
	} temperature;
	struct {
		bool available;
		const char *reason, *source;
		size_t free_bytes, allocated_bytes, max_allocated_bytes, total_bytes;
		int error;
	} heap;
	struct {
		bool available;
		const char *reason, *source, *coverage, *limiting_component, *limiting_name;
		uint32_t pressure_pct_x100, system_heap_pressure_pct_x100;
		struct { size_t total_bytes, image_reserved_bytes; uint32_t reserved_pct_x100; } physical;
		struct {
			size_t thread_count, measured_count, error_count, total_bytes, used_high_water_bytes;
			uint32_t max_pressure_pct_x100;
			const char *max_pressure_thread;
		} stacks;
		struct linkr_debugger_monitoring_pressure current_pressure;
		struct linkr_debugger_monitoring_pressure peak_pressure;
	} memory;
	struct {
		bool available;
		const char *reason;
		int64_t uptime_ms;
		uint64_t uptime_seconds;
		int error;
	} runtime;
	struct {
		bool available;
		const char *reason;
		uint32_t active_pct_x100, window_ms;
		uint64_t busy_cycles_delta, total_cycles_delta;
		int error;
	} cpu;
};

struct linkr_debugger_config_service_status { int unused; };
struct linkr_debugger_config_summary_buffer {
	char *data;
	size_t capacity;
	size_t length;
	size_t tail_reserve;
};
struct linkr_debugger_gpio_error {
	const char *code;
	const char *message;
	bool forbidden;
};
struct linkr_debugger_ws_session_info {
	uint8_t slot;
	bool connected;
	uint32_t session_id;
	char ws_path[64];
};

enum linkr_debugger_captive_method {
	LINKR_DEBUGGER_CAPTIVE_METHOD_GET,
	LINKR_DEBUGGER_CAPTIVE_METHOD_HEAD,
	LINKR_DEBUGGER_CAPTIVE_METHOD_OTHER,
};
enum linkr_debugger_captive_action {
	LINKR_DEBUGGER_CAPTIVE_ACTION_CAPPORT_JSON,
	LINKR_DEBUGGER_CAPTIVE_ACTION_REDIRECT,
	LINKR_DEBUGGER_CAPTIVE_ACTION_METHOD_NOT_ALLOWED,
	LINKR_DEBUGGER_CAPTIVE_ACTION_NONE,
};
enum linkr_debugger_task_http_route {
	LINKR_DEBUGGER_TASK_HTTP_ROUTE_TASKS,
	LINKR_DEBUGGER_TASK_HTTP_ROUTE_CATALOG,
};
enum linkr_debugger_config_http_route { LINKR_DEBUGGER_CONFIG_HTTP_ROUTE_CONFIG };

#define LINKR_DEBUGGER_CAPTIVE_PORTAL_URL "/"
#define LINKR_DEBUGGER_CONFIG_SERVICE_OK 0
#define LINKR_DEBUGGER_GPIO_NAME_BUFSZ 5U
#define LINKR_DEBUGGER_NET_DEVICE_HOST 1U
#define LINKR_DEBUGGER_NET_SUBNET_MIN 1U
#define LINKR_DEBUGGER_NET_SUBNET_MAX 254U
#define LINKR_DEBUGGER_TASK_CATALOG_HTTP_PATH "/api/v1/tasks/catalog"
#define LINKR_DEBUGGER_TASK_HTTP_PATH "/api/v1/tasks"
#define LINKR_DEBUGGER_TASK_MAX_TASK_ID_LEN 64U
#define LINKR_DEBUGGER_WS_MAX_CLIENTS 4U
#define LINKR_DEBUGGER_WS_RECV_BUFFER_SIZE 16U
#define LINKR_DEBUGGER_REBOOT_DELAY_MS 100U

static const struct linkr_debugger_rail_desc linkr_debugger_rails[1];
static const size_t linkr_debugger_rail_count;
static const struct linkr_debugger_current_desc linkr_debugger_currents[1];
static const size_t linkr_debugger_adc_count;
static const struct linkr_debugger_safe_gpio_desc linkr_debugger_safe_gpios[1];
static const size_t linkr_debugger_safe_gpio_count;

static inline const struct linkr_debugger_rail_desc *linkr_debugger_find_rail(const char *name)
{
	ARG_UNUSED(name);
	return NULL;
}
static inline bool linkr_debugger_parse_bool_arg(const char *arg, bool *value)
{
	ARG_UNUSED(arg);
	ARG_UNUSED(value);
	return false;
}
static inline int linkr_debugger_power_output_set(
	const struct linkr_debugger_rail_desc *rail, bool enabled)
{
	ARG_UNUSED(rail);
	ARG_UNUSED(enabled);
	return -1;
}
static inline bool linkr_debugger_power_output_enabled(
	const struct linkr_debugger_rail_desc *rail)
{
	ARG_UNUSED(rail);
	return false;
}
static inline int linkr_debugger_sd_route_set(enum linkr_debugger_sd_route route)
{
	ARG_UNUSED(route);
	return -1;
}
static inline int linkr_debugger_usb_route_set(enum linkr_debugger_usb_route route)
{
	ARG_UNUSED(route);
	return -1;
}
static inline bool linkr_debugger_parse_tf_wp_route(
	const char *arg, enum linkr_debugger_tf_wp_route *route)
{
	ARG_UNUSED(arg);
	ARG_UNUSED(route);
	return false;
}
static inline int linkr_debugger_tf_wp_route_set(enum linkr_debugger_tf_wp_route route)
{
	ARG_UNUSED(route);
	return -1;
}
static inline const struct linkr_debugger_safe_gpio_desc *
linkr_debugger_find_safe_gpio_by_identifier(const char *identifier)
{
	ARG_UNUSED(identifier);
	return NULL;
}
static inline int linkr_debugger_gpio_set_input(
	const struct linkr_debugger_safe_gpio_desc *desc)
{
	ARG_UNUSED(desc);
	return -1;
}
static inline int linkr_debugger_gpio_set_output(
	const struct linkr_debugger_safe_gpio_desc *desc, bool value)
{
	ARG_UNUSED(desc);
	ARG_UNUSED(value);
	return -1;
}
static inline int linkr_debugger_reboot_schedule(void) { return -1; }
static inline const char *linkr_debugger_captive_capport_body(void) { return ""; }
static inline bool linkr_debugger_captive_method_has_body(
	enum linkr_debugger_captive_method method)
{
	ARG_UNUSED(method);
	return false;
}
static inline bool linkr_debugger_format_gpio_name(uint8_t pin, char *buf, size_t len)
{
	ARG_UNUSED(pin);
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
	return false;
}
static inline int linkr_debugger_gpio_get(
	const struct linkr_debugger_safe_gpio_desc *desc, int *value)
{
	ARG_UNUSED(desc);
	ARG_UNUSED(value);
	return -1;
}
static inline const char *linkr_debugger_safe_gpio_direction_name(size_t index)
{
	ARG_UNUSED(index);
	return "";
}
static inline const char *linkr_debugger_safe_gpio_direction(
	const struct linkr_debugger_safe_gpio_desc *desc)
{
	ARG_UNUSED(desc);
	return "";
}
static inline void linkr_debugger_watchdog_status_get(
	struct linkr_debugger_watchdog_status *status)
{
	ARG_UNUSED(status);
}
static inline const char *linkr_debugger_sd_route_name(void) { return ""; }
static inline const char *linkr_debugger_usb_route_name(void) { return ""; }
static inline const char *linkr_debugger_tf_wp_route_name(void) { return ""; }
static inline void linkr_debugger_monitoring_snapshot_get(
	struct linkr_debugger_monitoring_snapshot *snapshot)
{
	ARG_UNUSED(snapshot);
}
static inline int linkr_debugger_config_service_status_get(
	struct linkr_debugger_config_service_status *status)
{
	ARG_UNUSED(status);
	return -1;
}
int linkr_debugger_pd_get_status_short(struct linkr_debugger_pd_status *status);
static inline const char *linkr_debugger_build_profile(void) { return ""; }
static inline const char *linkr_debugger_build_image_version(void) { return ""; }
static inline const char *linkr_debugger_mcu_name(void) { return ""; }
static inline const char *linkr_debugger_usb_mode(void) { return ""; }
static inline const char *linkr_debugger_power_capture_protocol(void) { return ""; }
static inline int linkr_debugger_config_summary_append(
	struct linkr_debugger_config_summary_buffer *buffer,
	const struct linkr_debugger_config_service_status *status)
{
	ARG_UNUSED(buffer);
	ARG_UNUSED(status);
	return 0;
}
static inline void linkr_debugger_ws_publish_state_change(void) {}
static inline const struct linkr_debugger_current_desc *linkr_debugger_find_adc(
	const char *name)
{
	ARG_UNUSED(name);
	return NULL;
}
static inline int linkr_debugger_current_read(
	const struct linkr_debugger_current_desc *current,
	struct linkr_debugger_current_sample *sample)
{
	ARG_UNUSED(current);
	ARG_UNUSED(sample);
	return -1;
}
static inline const char *linkr_debugger_reserved_gpios(void) { return ""; }
static inline const struct linkr_debugger_gpio_error *linkr_debugger_gpio_configure_error(
	bool output, int error)
{
	ARG_UNUSED(output);
	ARG_UNUSED(error);
	return NULL;
}
static inline int linkr_debugger_bootloader_now(void) { return -1; }
int linkr_debugger_pd_get_status(struct linkr_debugger_pd_status *status);
int linkr_debugger_pd_set_pps(uint16_t voltage_mv);
int linkr_debugger_pd_set_fixed(uint16_t voltage_mv);
static inline int linkr_debugger_ws_session_create(struct linkr_debugger_ws_session_info *info)
{
	ARG_UNUSED(info);
	return -1;
}
static inline int linkr_debugger_ws_session_lookup(
	uint32_t session_id, struct linkr_debugger_ws_session_info *info)
{
	ARG_UNUSED(session_id);
	ARG_UNUSED(info);
	return -1;
}
static inline int linkr_debugger_ws_session_delete(uint32_t session_id)
{
	ARG_UNUSED(session_id);
	return -1;
}
static inline int linkr_debugger_task_http_handle_delete_one(
	struct http_client_ctx *client, struct http_response_ctx *response,
	const char *identifier)
{
	ARG_UNUSED(client);
	ARG_UNUSED(response);
	ARG_UNUSED(identifier);
	return -1;
}
static inline enum linkr_debugger_captive_action linkr_debugger_captive_select_action(
	enum linkr_debugger_captive_method method, const char *path)
{
	ARG_UNUSED(method);
	ARG_UNUSED(path);
	return LINKR_DEBUGGER_CAPTIVE_ACTION_NONE;
}
static inline void linkr_debugger_ota_init(void) {}

const char *linkr_debugger_json_schema(void);
int linkr_debugger_json_append_string(void *context,
				      int (*write)(void *, const char *, size_t),
				      const char *value, size_t len);
int linkr_debugger_net_format_ipv4(uint8_t subnet, uint8_t host,
				   char *buf, size_t cap);
uint8_t linkr_debugger_network_subnet(void);
int linkr_debugger_network_set_subnet_with_result(uint8_t subnet,
						  bool *reconnect_required);
static inline int linkr_debugger_task_http_handle(
	struct http_client_ctx *client, enum http_transaction_status status,
	const struct http_request_ctx *request, struct http_response_ctx *response,
	void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(status);
	ARG_UNUSED(request);
	ARG_UNUSED(response);
	ARG_UNUSED(user_data);
	return -1;
}
static inline int linkr_debugger_config_http_handle(
	struct http_client_ctx *client, enum http_transaction_status status,
	const struct http_request_ctx *request, struct http_response_ctx *response,
	void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(status);
	ARG_UNUSED(request);
	ARG_UNUSED(response);
	ARG_UNUSED(user_data);
	return -1;
}
static inline int linkr_debugger_ws_setup(
	int socket, struct http_request_ctx *request, void *user_data)
{
	ARG_UNUSED(socket);
	ARG_UNUSED(request);
	ARG_UNUSED(user_data);
	return -1;
}
const char *linkr_debugger_http_listener_host(void);
uint16_t linkr_debugger_http_listener_port(void);

#endif /* LINKR_DEBUGGER_HTTP_NET_TEST_STUBS_H_ */
