#ifndef LINKR_DEBUGGER_HTTP_NET_TEST_STUBS_H_
#define LINKR_DEBUGGER_HTTP_NET_TEST_STUBS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "linkr_debugger_net_settings.h"

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
#define RADXA_LINKR_DEBUGGER_JSON_CURSOR_H_
#define RADXA_LINKR_DEBUGGER_MONITORING_H_
#define RADXA_LINKR_DEBUGGER_MODEL_H_
#define RADXA_LINKR_DEBUGGER_NETWORK_H_
#define RADXA_LINKR_DEBUGGER_OTA_H_
#define RADXA_LINKR_DEBUGGER_PD_H_
#define RADXA_LINKR_DEBUGGER_SHELL_H_
#define RADXA_LINKR_DEBUGGER_SERIAL_H_
#define RADXA_LINKR_DEBUGGER_SERIAL_MODEL_H_
#define RADXA_LINKR_DEBUGGER_SERIAL_NET_H_
#define RADXA_LINKR_DEBUGGER_SERIAL_HTTP_H_
#define RADXA_LINKR_DEBUGGER_WS_H_

#define CONFIG_LINKR_DEBUGGER_LOG_LEVEL 0
#define CONFIG_HTTP_SERVER_MAX_CLIENTS 1
#define LINKR_DEBUGGER_TASK_HTTP_HOST_TEST 1

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define BUILD_ASSERT(condition, message) _Static_assert(condition, message)
#define ARG_UNUSED(value) ((void)(value))
#define BIT(bit) (1UL << (bit))
#define BIT64(bit) (UINT64_C(1) << (bit))
#define IS_ENABLED(...) 0
#define K_FOREVER 0
#define K_MSEC(ms) (ms)
#define K_MUTEX_DEFINE(name) static struct k_mutex name
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
	HTTP_507_INSUFFICIENT_STORAGE = 507,
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

enum http_header_status {
	HTTP_HEADER_STATUS_OK,
	HTTP_HEADER_STATUS_DROPPED,
	HTTP_HEADER_STATUS_NONE,
};

struct http_request_ctx {
	char *data;
	size_t data_len;
	struct http_header *headers;
	size_t header_count;
	enum http_header_status headers_status;
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
struct linkr_debugger_http_resource_stub {
	const char *path;
	void *detail;
};
#define HTTP_RESOURCE_DEFINE(name_, service_, path_, detail_) \
	static const struct linkr_debugger_http_resource_stub name_ = { path_, detail_ }
#define HTTP_SERVICE_DEFINE(name, ...) \
	struct linkr_debugger_http_service_stub name = { 0 }

enum json_tokens {
	JSON_TOK_STRING_BUF,
	JSON_TOK_NUMBER,
	JSON_TOK_INT64,
	JSON_TOK_UINT64,
};
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
	const char *parent;
	uint8_t pin;
	bool controllable;
};
enum linkr_debugger_adc_kind {
	LINKR_DEBUGGER_ADC_KIND_CURRENT,
	LINKR_DEBUGGER_ADC_KIND_VOLTAGE,
};
enum linkr_debugger_adc_role {
	LINKR_DEBUGGER_ADC_ROLE_NATIVE,
	LINKR_DEBUGGER_ADC_ROLE_PD_INPUT,
};
struct linkr_debugger_current_desc {
	const char *name;
	const char *signal;
	const char *sensor;
	const char *note;
	enum linkr_debugger_adc_kind kind;
	enum linkr_debugger_adc_role role;
	const char *unit;
	size_t adc_index;
	size_t sensor_index;
};
static inline const char *linkr_debugger_adc_role_name(enum linkr_debugger_adc_role role)
{
	return role == LINKR_DEBUGGER_ADC_ROLE_PD_INPUT ? "pd_input" : "native";
}
struct linkr_debugger_safe_gpio_desc {
	uint8_t pin;
	bool output_capable;
	const char *note;
	const char *layout_group;
	const char *layout_label;
	uint8_t layout_row;
	uint8_t layout_column;
};
struct linkr_debugger_gpio_layout_group_desc {
	const char *name;
	const char *edge;
	uint8_t order;
	uint8_t rows;
	uint8_t columns;
};
struct linkr_debugger_gpio_layout_cell_desc {
	uint8_t group_order;
	uint8_t pin_number;
	uint8_t row;
	uint8_t column;
	const char *label;
	const char *signal;
	const char *role;
	bool controllable;
	bool output_capable;
	uint8_t gpio_pin;
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

struct linkr_debugger_config_service_status {
	bool available;
	const char *reason;
	bool snapshot_present;
	uint8_t snapshot_version;
	size_t item_count;
	size_t saved_count;
	size_t applied_count;
	size_t pending_count;
	size_t failed_count;
	int failed_errno;
};
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
enum linkr_debugger_config_http_route { LINKR_DEBUGGER_CONFIG_HTTP_ROUTE_CONFIG };
enum linkr_debugger_serial_parity {
	LINKR_DEBUGGER_SERIAL_PARITY_NONE,
	LINKR_DEBUGGER_SERIAL_PARITY_ODD,
	LINKR_DEBUGGER_SERIAL_PARITY_EVEN,
};
enum linkr_debugger_serial_flow_control {
	LINKR_DEBUGGER_SERIAL_FLOW_NONE,
	LINKR_DEBUGGER_SERIAL_FLOW_RTS_CTS,
};
enum linkr_debugger_serial_state {
	LINKR_DEBUGGER_SERIAL_READY,
	LINKR_DEBUGGER_SERIAL_RECONFIG,
	LINKR_DEBUGGER_SERIAL_PAUSED,
	LINKR_DEBUGGER_SERIAL_FAULTED,
};
struct linkr_debugger_serial_config {
	uint32_t baud;
	uint8_t data_bits;
	enum linkr_debugger_serial_parity parity;
	uint8_t stop_bits;
	enum linkr_debugger_serial_flow_control flow_control;
};
struct linkr_debugger_serial_runtime_counters {
	uint64_t uart_rx_bytes;
	uint64_t uart_rx_pause_dropped;
	uint64_t cdc_rx_dropped;
	uint64_t cdc_tx_stale_dropped;
	uint64_t completion_overflow;
	uint64_t invalid_line_coding;
	uint64_t config_mailbox_busy;
};
struct linkr_debugger_serial_config_counters {
	uint64_t cdc_requests;
	uint64_t web_requests;
	uint64_t configure_count;
	uint64_t no_change;
	uint64_t invalid;
	uint64_t conflicts;
	uint64_t apply_succeeded;
	uint64_t apply_failed;
	uint64_t rollback_succeeded;
	uint64_t rollback_failed;
};
struct linkr_debugger_serial_status {
	struct linkr_debugger_serial_config current_config;
	struct linkr_debugger_serial_config_counters config_counters;
	struct linkr_debugger_serial_runtime_counters runtime_counters;
	uint64_t generation;
	uint64_t cdc_responder_generation;
	uint64_t rx_head;
	uint64_t tx_bytes_committed;
	uint64_t tx_accepted;
	uint64_t tx_completed;
	uint64_t tx_canceled;
	uint64_t last_config_request_id;
	uint64_t active_event_id;
	int last_config_result;
	enum linkr_debugger_serial_state state;
	uint8_t tx_used_slots;
	bool last_config_done;
	bool dtr;
	bool rts;
	bool usb_configured;
	bool admission_frozen;
};
#define LINKR_DEBUGGER_SERIAL_TX_SLOT_COUNT 8U
#define LINKR_DEBUGGER_SERIAL_BAUD_MIN 4800U
#define LINKR_DEBUGGER_SERIAL_BAUD_MAX 115200U
#define LINKR_DEBUGGER_SERIAL_RAW_TCP_PORT 5555U
#define LINKR_DEBUGGER_SERIAL_RAW_TCP_MAX_CLIENTS 2U
#define LINKR_DEBUGGER_SERIAL_WS_MAX_CLIENTS 4U
struct linkr_debugger_serial_net_endpoint_counters {
	uint64_t connected, disconnected, rx_bytes, rx_dropped, tx_bytes, rx_messages;
	uint64_t tx_queue_waits, send_would_block, send_failures;
	uint64_t inferred_rx_drops, stale_completions;
};
struct linkr_debugger_serial_net_counters {
	uint64_t listener_starts, listener_failures, accept_failures;
	uint64_t refused_clients, completion_dispatches, completion_orphans;
	uint64_t poll_failures;
};
enum linkr_debugger_serial_terminal_responder_transport {
	LINKR_DEBUGGER_SERIAL_TERMINAL_RESPONDER_CDC,
	LINKR_DEBUGGER_SERIAL_TERMINAL_RESPONDER_WEBSOCKET,
};
struct linkr_debugger_serial_terminal_responder {
	enum linkr_debugger_serial_terminal_responder_transport transport;
	int8_t slot;
	bool valid;
};
struct linkr_debugger_serial_net_status {
	struct linkr_debugger_serial_net_counters counters;
	struct linkr_debugger_serial_net_endpoint_counters ws[4], tcp[2];
	struct linkr_debugger_serial_terminal_responder terminal_responder;
	uint16_t tcp_port;
	uint8_t ws_attached, tcp_connected;
	bool initialized, listening;
};
enum linkr_debugger_serial_http_route {
	LINKR_DEBUGGER_SERIAL_HTTP_ROUTE_STATUS,
	LINKR_DEBUGGER_SERIAL_HTTP_ROUTE_CONFIG,
};
#define LINKR_DEBUGGER_SERIAL_HTTP_STATUS_PATH "/api/v1/serial/j4"
#define LINKR_DEBUGGER_SERIAL_HTTP_CONFIG_PATH "/api/v1/serial/j4/config"

#define LINKR_DEBUGGER_CAPTIVE_PORTAL_URL "/"
#define LINKR_DEBUGGER_CONFIG_SERVICE_OK 0
#define LINKR_DEBUGGER_GPIO_NAME_BUFSZ 5U
#define LINKR_DEBUGGER_WS_MAX_CLIENTS 4U
#define LINKR_DEBUGGER_WS_RECV_BUFFER_SIZE 16U
#define LINKR_DEBUGGER_REBOOT_DELAY_MS 100U

#define HTTP_TEST_RAIL_COUNT 5U
#define HTTP_TEST_ADC_COUNT 8U
#define HTTP_TEST_SAFE_GPIO_COUNT 11U
#define HTTP_TEST_MAPPED_GPIO_COUNT 11U
#define HTTP_TEST_LAYOUT_CELL_COUNT 24U

static const struct linkr_debugger_rail_desc linkr_debugger_rails[] = {
	{ .name = "12v_out", .signal = "GP02_12V_EN", .pin = 2U, .controllable = true },
	{ .name = "5v_out", .signal = "GP0_5V_EN", .pin = 0U, .controllable = true },
	{ .name = "vdd_5v", .signal = "GP01_5V_WS_EN", .pin = 1U, .controllable = true },
	{ .name = "20v_out", .signal = "GP03_20V_EN", .pin = 3U, .controllable = true },
	{ .name = "5v_usb_hub", .signal = "GPIO24_USB_CRL", .pin = 24U,
	  .parent = "vdd_5v", .controllable = true },
};
static const size_t linkr_debugger_rail_count = ARRAY_SIZE(linkr_debugger_rails);
static struct linkr_debugger_current_desc linkr_debugger_currents[] = {
	{ .name = "5v_out", .signal = "INA226_5V", .sensor = "INA226",
	  .kind = LINKR_DEBUGGER_ADC_KIND_CURRENT, .role = LINKR_DEBUGGER_ADC_ROLE_NATIVE,
	  .unit = "uA", .sensor_index = 0U },
	{ .name = "12v_out", .signal = "INA226_12V", .sensor = "INA226",
	  .kind = LINKR_DEBUGGER_ADC_KIND_CURRENT, .role = LINKR_DEBUGGER_ADC_ROLE_NATIVE,
	  .unit = "uA", .sensor_index = 1U },
	{ .name = "20v_out", .signal = "INA226_20V", .sensor = "INA226",
	  .kind = LINKR_DEBUGGER_ADC_KIND_CURRENT, .role = LINKR_DEBUGGER_ADC_ROLE_NATIVE,
	  .unit = "uA", .sensor_index = 2U },
	{ .name = "vdd_5v", .signal = "INA226_VDD_5V", .sensor = "INA226",
	  .kind = LINKR_DEBUGGER_ADC_KIND_CURRENT, .role = LINKR_DEBUGGER_ADC_ROLE_NATIVE,
	  .unit = "uA", .sensor_index = 3U },
	{ .name = "adc0", .signal = "GPIO26_ADC0", .sensor = "ADC",
	  .note = "PD input divider; ADC pad voltage",
	  .kind = LINKR_DEBUGGER_ADC_KIND_VOLTAGE, .role = LINKR_DEBUGGER_ADC_ROLE_PD_INPUT,
	  .unit = "uV", .adc_index = 0U },
	{ .name = "adc1", .signal = "GPIO27_ADC1", .sensor = "ADC",
	  .kind = LINKR_DEBUGGER_ADC_KIND_VOLTAGE, .role = LINKR_DEBUGGER_ADC_ROLE_NATIVE,
	  .unit = "uV", .adc_index = 1U },
	{ .name = "adc2", .signal = "GPIO28_ADC2", .sensor = "ADC",
	  .kind = LINKR_DEBUGGER_ADC_KIND_VOLTAGE, .role = LINKR_DEBUGGER_ADC_ROLE_NATIVE,
	  .unit = "uV", .adc_index = 2U },
	{ .name = "adc3", .signal = "GPIO29_ADC3", .sensor = "ADC",
	  .kind = LINKR_DEBUGGER_ADC_KIND_VOLTAGE, .role = LINKR_DEBUGGER_ADC_ROLE_NATIVE,
	  .unit = "uV", .adc_index = 3U },
};
static const size_t linkr_debugger_adc_count = ARRAY_SIZE(linkr_debugger_currents);
static const struct linkr_debugger_safe_gpio_desc linkr_debugger_safe_gpios[] = {
	{ .pin = 29U, .output_capable = false, .note = "J8_PIN6",
	  .layout_group = "J8", .layout_label = "GP29 / ADC3",
	  .layout_row = 4U, .layout_column = 0U },
	{ .pin = 5U, .output_capable = true, .note = "J8_PIN16",
	  .layout_group = "J8", .layout_label = "GP5",
	  .layout_row = 4U, .layout_column = 1U },
	{ .pin = 16U, .output_capable = true, .note = "J8_PIN5",
	  .layout_group = "J8", .layout_label = "GP16",
	  .layout_row = 5U, .layout_column = 0U },
	{ .pin = 6U, .output_capable = true, .note = "J8_PIN15",
	  .layout_group = "J8", .layout_label = "GP6",
	  .layout_row = 5U, .layout_column = 1U },
	{ .pin = 15U, .output_capable = true, .note = "J8_PIN4",
	  .layout_group = "J8", .layout_label = "GP15",
	  .layout_row = 6U, .layout_column = 0U },
	{ .pin = 14U, .output_capable = true, .note = "J8_PIN3",
	  .layout_group = "J8", .layout_label = "GP14",
	  .layout_row = 7U, .layout_column = 0U },
	{ .pin = 13U, .output_capable = true, .note = "J8_PIN2",
	  .layout_group = "J8", .layout_label = "GP13",
	  .layout_row = 8U, .layout_column = 0U },
	{ .pin = 12U, .output_capable = true, .note = "J8_PIN1",
	  .layout_group = "J8", .layout_label = "GP12",
	  .layout_row = 9U, .layout_column = 0U },
	{ .pin = 9U, .output_capable = true, .note = "CON_USER",
	  .layout_group = "J13", .layout_label = "GP9 / USER",
	  .layout_row = 0U, .layout_column = 0U },
	{ .pin = 8U, .output_capable = true, .note = "CON_REST",
	  .layout_group = "J13", .layout_label = "GP8 / RSET",
	  .layout_row = 0U, .layout_column = 1U },
	{ .pin = 7U, .output_capable = true, .note = "CON_MAS",
	  .layout_group = "J13", .layout_label = "GP7 / MASKROM",
	  .layout_row = 1U, .layout_column = 0U },
};
static const size_t linkr_debugger_safe_gpio_count = ARRAY_SIZE(linkr_debugger_safe_gpios);
static struct linkr_debugger_gpio_layout_group_desc linkr_debugger_gpio_layout_groups[] = {
	{ .name = "J8", .edge = "right", .order = 0U, .rows = 10U, .columns = 2U },
	{ .name = "J13", .edge = "right", .order = 1U, .rows = 2U, .columns = 2U },
};
static const size_t linkr_debugger_gpio_layout_group_count =
	ARRAY_SIZE(linkr_debugger_gpio_layout_groups);
static const char linkr_debugger_gpio_layout_view[] = "component-top";
#define HTTP_LAYOUT_CELL(group_, pin_, row_, column_, label_, signal_, role_) \
	{ group_, pin_, row_, column_, label_, signal_, role_, false, false, 0U }
#define HTTP_LAYOUT_GPIO(group_, pin_, row_, column_, label_, signal_, output_, gp_) \
	{ group_, pin_, row_, column_, label_, signal_, "gpio", true, output_, gp_ }
static const struct linkr_debugger_gpio_layout_cell_desc linkr_debugger_gpio_layout_cells[] = {
	HTTP_LAYOUT_CELL(0U, 10U, 0U, 0U, "GP21 / SDA", "RP2354_SDA", "i2c"),
	HTTP_LAYOUT_CELL(0U, 20U, 0U, 1U, "5V USB HUB", "5V_USB_HUB", "power"),
	HTTP_LAYOUT_CELL(0U, 9U, 1U, 0U, "GP20 / SCL", "RP2354_SCL", "i2c"),
	HTTP_LAYOUT_CELL(0U, 19U, 1U, 1U, "USB DM (hub)", "U3_D3M_HUB_CON", "usb"),
	HTTP_LAYOUT_CELL(0U, 8U, 2U, 0U, "GP27 / ADC1", "GPIO27_ADC1", "adc"),
	HTTP_LAYOUT_CELL(0U, 18U, 2U, 1U, "USB DP (hub)", "U3_D3P_HUB_CON", "usb"),
	HTTP_LAYOUT_CELL(0U, 7U, 3U, 0U, "GP28 / ADC2", "GPIO28_ADC2", "adc"),
	HTTP_LAYOUT_CELL(0U, 17U, 3U, 1U, "GND", "GND", "ground"),
	{ 0U, 6U, 4U, 0U, "GP29 / ADC3", "GPIO29_ADC3", "adc", true, false, 29U },
	HTTP_LAYOUT_GPIO(0U, 16U, 4U, 1U, "GP5", "GPIO5_CON", true, 5U),
	HTTP_LAYOUT_GPIO(0U, 5U, 5U, 0U, "GP16", "GPIO16", true, 16U),
	HTTP_LAYOUT_GPIO(0U, 15U, 5U, 1U, "GP6", "GPIO6_CON", true, 6U),
	HTTP_LAYOUT_GPIO(0U, 4U, 6U, 0U, "GP15", "GPIO15", true, 15U),
	HTTP_LAYOUT_CELL(0U, 14U, 6U, 1U, "GND", "GND", "ground"),
	HTTP_LAYOUT_GPIO(0U, 3U, 7U, 0U, "GP14", "GPIO14", true, 14U),
	HTTP_LAYOUT_CELL(0U, 13U, 7U, 1U, "USB DM", "MTT_USB2_DM", "usb"),
	HTTP_LAYOUT_GPIO(0U, 2U, 8U, 0U, "GP13", "GPIO13", true, 13U),
	HTTP_LAYOUT_CELL(0U, 12U, 8U, 1U, "USB DP", "MTT_USB2_DP", "usb"),
	HTTP_LAYOUT_GPIO(0U, 1U, 9U, 0U, "GP12", "GPIO12", true, 12U),
	HTTP_LAYOUT_CELL(0U, 11U, 9U, 1U, "GND", "GND", "ground"),
	HTTP_LAYOUT_GPIO(1U, 2U, 0U, 0U, "GP9 / USER", "GP09_USER", true, 9U),
	HTTP_LAYOUT_GPIO(1U, 3U, 0U, 1U, "GP8 / RSET", "GP08_REST", true, 8U),
	HTTP_LAYOUT_GPIO(1U, 1U, 1U, 0U, "GP7 / MASKROM", "GP07_MAS", true, 7U),
	HTTP_LAYOUT_CELL(1U, 4U, 1U, 1U, "GND", "GND", "ground"),
};
static const size_t linkr_debugger_gpio_layout_cell_count =
	ARRAY_SIZE(linkr_debugger_gpio_layout_cells);
static bool http_test_power_states[HTTP_TEST_RAIL_COUNT];
static int http_test_gpio_values[HTTP_TEST_SAFE_GPIO_COUNT];
static const char *http_test_gpio_directions[HTTP_TEST_SAFE_GPIO_COUNT];
static struct linkr_debugger_watchdog_status http_test_watchdog_status;
static struct linkr_debugger_monitoring_snapshot http_test_monitoring_snapshot;
static struct linkr_debugger_config_service_status http_test_config_status;

static inline const struct linkr_debugger_rail_desc *linkr_debugger_find_rail(const char *name)
{
	for (size_t i = 0U; i < linkr_debugger_rail_count; i++) {
		if (strcmp(name, linkr_debugger_rails[i].name) == 0 ||
		    strcmp(name, linkr_debugger_rails[i].signal) == 0) {
			return &linkr_debugger_rails[i];
		}
	}
	return NULL;
}
static inline bool linkr_debugger_parse_bool_arg(const char *arg, bool *value)
{
	if (strcmp(arg, "on") == 0) {
		*value = true;
		return true;
	}
	if (strcmp(arg, "off") == 0) {
		*value = false;
		return true;
	}
	return false;
}
static inline int linkr_debugger_power_output_set(
	const struct linkr_debugger_rail_desc *rail, bool enabled)
{
	size_t index = (size_t)(rail - linkr_debugger_rails);

	if (index >= ARRAY_SIZE(http_test_power_states)) {
		return -1;
	}
	http_test_power_states[index] = enabled;
	return 0;
}
static inline bool linkr_debugger_power_output_enabled(
	const struct linkr_debugger_rail_desc *rail)
{
	size_t index = (size_t)(rail - linkr_debugger_rails);

	return index < ARRAY_SIZE(http_test_power_states) && http_test_power_states[index];
}
static inline int linkr_debugger_sd_route_set(enum linkr_debugger_sd_route route)
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
	char name[8];

	for (size_t i = 0U; i < linkr_debugger_safe_gpio_count; i++) {
		(void)snprintf(name, sizeof(name), "GP%u",
			       (unsigned int)linkr_debugger_safe_gpios[i].pin);
		if (strcmp(identifier, name) == 0 ||
		    strcmp(identifier, linkr_debugger_safe_gpios[i].note) == 0) {
			return &linkr_debugger_safe_gpios[i];
		}
	}
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
static inline int linkr_debugger_captive_capport_body(const char *portal_url,
						char *buf, size_t cap)
{
	int written;

	if (portal_url == NULL || buf == NULL || cap == 0U) {
		return -1;
	}
	written = snprintf(buf, cap,
			   "{\"captive\":true,\"user-portal-url\":\"%s\","
			   "\"venue-info-url\":\"%s\"}\n",
			   portal_url, portal_url);
	return written < 0 || (size_t)written >= cap ? -1 : 0;
}
static inline bool linkr_debugger_captive_method_has_body(
	enum linkr_debugger_captive_method method)
{
	ARG_UNUSED(method);
	return false;
}
static inline bool linkr_debugger_format_gpio_name(uint8_t pin, char *buf, size_t len)
{
	int written = snprintf(buf, len, "GP%u", (unsigned int)pin);

	return written > 0 && (size_t)written < len;
}
static inline int linkr_debugger_gpio_get(
	const struct linkr_debugger_safe_gpio_desc *desc, int *value)
{
	size_t index = (size_t)(desc - linkr_debugger_safe_gpios);

	if (index >= ARRAY_SIZE(http_test_gpio_values)) {
		return -1;
	}
	*value = http_test_gpio_values[index];
	return 0;
}
static inline const char *linkr_debugger_safe_gpio_direction_name(size_t index)
{
	return index < ARRAY_SIZE(http_test_gpio_directions) ?
		http_test_gpio_directions[index] : "input";
}
static inline const char *linkr_debugger_safe_gpio_direction(
	const struct linkr_debugger_safe_gpio_desc *desc)
{
	size_t index = (size_t)(desc - linkr_debugger_safe_gpios);

	return index < ARRAY_SIZE(http_test_gpio_directions) ?
		http_test_gpio_directions[index] : "input";
}
static inline void linkr_debugger_watchdog_status_get(
	struct linkr_debugger_watchdog_status *status)
{
	*status = http_test_watchdog_status;
}
static inline const char *linkr_debugger_sd_route_name(void) { return "usb-reader"; }
static inline const char *linkr_debugger_tf_wp_route_name(void) { return "protected"; }
static inline void linkr_debugger_monitoring_snapshot_get(
	struct linkr_debugger_monitoring_snapshot *snapshot)
{
	*snapshot = http_test_monitoring_snapshot;
}
static inline int linkr_debugger_config_service_status_get(
	struct linkr_debugger_config_service_status *status)
{
	*status = http_test_config_status;
	return LINKR_DEBUGGER_CONFIG_SERVICE_OK;
}
int linkr_debugger_pd_get_status_short(struct linkr_debugger_pd_status *status);
static inline const char *linkr_debugger_build_profile(void) { return "production-400khz"; }
static inline const char *linkr_debugger_build_image_version(void) { return "0.3.0-test+full"; }
static inline const char *linkr_debugger_mcu_name(void) { return "RP2354A"; }
static inline const char *linkr_debugger_usb_mode(void) { return "ncm-cdc-acm"; }
static inline const char *linkr_debugger_power_capture_protocol(void) { return "host-stream-v1"; }
static inline int linkr_debugger_config_summary_append(
	struct linkr_debugger_config_summary_buffer *buffer,
	const struct linkr_debugger_config_service_status *status)
{
	char fragment[128];
	int written = snprintf(fragment, sizeof(fragment),
		",\"config\":{\"available\":%s,\"reason\":\"%s\","
		"\"saved_count\":%zu,\"pending_count\":%zu}",
		status->available ? "true" : "false", status->reason,
		status->saved_count, status->pending_count);

	if (written < 0 || (size_t)written >= sizeof(fragment) ||
	    (size_t)written + buffer->tail_reserve >= buffer->capacity - buffer->length) {
		return 0;
	}
	memcpy(buffer->data + buffer->length, fragment, (size_t)written + 1U);
	buffer->length += (size_t)written;
	return 1;
}
static inline void linkr_debugger_ws_publish_state_change(void) {}
static inline const struct linkr_debugger_current_desc *linkr_debugger_find_adc(
	const char *name)
{
	for (size_t i = 0U; i < linkr_debugger_adc_count; i++) {
		if (strcmp(name, linkr_debugger_currents[i].name) == 0 ||
		    strcmp(name, linkr_debugger_currents[i].signal) == 0) {
			return &linkr_debugger_currents[i];
		}
	}
	return NULL;
}
static inline int linkr_debugger_current_read(
	const struct linkr_debugger_current_desc *current,
	struct linkr_debugger_current_sample *sample)
{
	size_t index = (size_t)(current - linkr_debugger_currents);

	if (index >= linkr_debugger_adc_count) {
		return -1;
	}
	memset(sample, 0, sizeof(*sample));
	if (current->kind == LINKR_DEBUGGER_ADC_KIND_CURRENT) {
		sample->rail_enabled = (index % 2U) == 0U;
		sample->voltage_valid = true;
		sample->mv = 5000 + (int32_t)index;
		sample->voltage_uv = sample->mv * 1000;
		sample->current_ua = 100000 + (int32_t)index;
		sample->value.val2 = sample->current_ua;
	} else {
		sample->raw_available = true;
		if (current->role == LINKR_DEBUGGER_ADC_ROLE_PD_INPUT) {
			sample->raw = 3237;
			sample->mv = 2608;
			sample->value.val1 = 20;
			sample->value.val2 = 2;
		} else {
			sample->raw = 2000 + (int32_t)current->adc_index;
			sample->mv = 1200 + (int32_t)current->adc_index;
			sample->value.val1 = 1;
			sample->value.val2 = 200000 + (int32_t)current->adc_index * 1000;
		}
	}
	return 0;
}
static inline const char *linkr_debugger_reserved_gpios(void)
{
	return "GP00-GP04 GP10-GP11 GP19 GP22-GP25 GP26-GP28";
}
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
static inline enum linkr_debugger_captive_action linkr_debugger_captive_select_action(
	enum linkr_debugger_captive_method method, const char *path)
{
	ARG_UNUSED(method);
	ARG_UNUSED(path);
	return LINKR_DEBUGGER_CAPTIVE_ACTION_NONE;
}
static inline void linkr_debugger_ota_init(void) {}

const char *linkr_debugger_json_schema(void);
bool linkr_debugger_json_utf8_valid(const char *text, size_t text_len);
int linkr_debugger_json_append_string(void *context,
				      int (*write)(void *, const char *, size_t),
				      const char *value, size_t len);
int linkr_debugger_net_format_ipv4(uint32_t network, uint8_t host,
				   char *buf, size_t cap);
int linkr_debugger_net_format_mac(const uint8_t mac[LINKR_DEBUGGER_NET_MAC_LEN],
				  char *buf, size_t cap);
int linkr_debugger_net_format_network(uint32_t network, char *buf, size_t cap);
int linkr_debugger_net_format_url(uint32_t network, char *buf, size_t cap);
int linkr_debugger_net_parse_mac(const char *text,
				 uint8_t mac[LINKR_DEBUGGER_NET_MAC_LEN]);
int linkr_debugger_net_parse_network(const char *text, uint32_t *network);
int linkr_debugger_network_identity(struct linkr_debugger_net_identity *identity);
int linkr_debugger_network_set_identity_with_result(
	const struct linkr_debugger_net_identity *identity, bool *reconnect_required);
int linkr_debugger_serial_status_snapshot(
	struct linkr_debugger_serial_status *status);
int linkr_debugger_serial_net_status_snapshot(
	struct linkr_debugger_serial_net_status *status);
int linkr_debugger_serial_config_cas(
	const struct linkr_debugger_serial_config *config,
	uint64_t expected_generation, uint64_t *request_id);
static inline bool linkr_debugger_serial_config_valid(
	const struct linkr_debugger_serial_config *config)
{
	return config->baud >= LINKR_DEBUGGER_SERIAL_BAUD_MIN &&
		config->baud <= LINKR_DEBUGGER_SERIAL_BAUD_MAX &&
		config->data_bits >= 5U &&
		config->data_bits <= 8U && config->stop_bits >= 1U &&
		config->stop_bits <= 2U &&
		config->parity <= LINKR_DEBUGGER_SERIAL_PARITY_EVEN &&
		config->flow_control == LINKR_DEBUGGER_SERIAL_FLOW_NONE;
}
int linkr_debugger_serial_http_handle(
	struct http_client_ctx *client, enum http_transaction_status status,
	const struct http_request_ctx *request,
	struct http_response_ctx *response, void *user_data);
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
