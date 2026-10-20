#ifndef LINKR_DEBUGGER_SERIAL_RUNTIME_STUB_UART_H_
#define LINKR_DEBUGGER_SERIAL_RUNTIME_STUB_UART_H_

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>

enum uart_config_parity {
	UART_CFG_PARITY_NONE,
	UART_CFG_PARITY_ODD,
	UART_CFG_PARITY_EVEN,
};

enum uart_config_stop_bits {
	UART_CFG_STOP_BITS_0_5,
	UART_CFG_STOP_BITS_1,
	UART_CFG_STOP_BITS_1_5,
	UART_CFG_STOP_BITS_2,
};

enum uart_config_data_bits {
	UART_CFG_DATA_BITS_5,
	UART_CFG_DATA_BITS_6,
	UART_CFG_DATA_BITS_7,
	UART_CFG_DATA_BITS_8,
};

enum uart_config_flow_control {
	UART_CFG_FLOW_CTRL_NONE,
	UART_CFG_FLOW_CTRL_RTS_CTS,
};

enum uart_line_ctrl {
	UART_LINE_CTRL_RTS = 2,
	UART_LINE_CTRL_DTR = 4,
};

struct uart_config {
	uint32_t baudrate;
	uint8_t parity;
	uint8_t stop_bits;
	uint8_t data_bits;
	uint8_t flow_ctrl;
};

typedef void (*uart_irq_callback_user_data_t)(const struct device *dev,
					      void *user_data);

static inline int
uart_irq_callback_user_data_set(const struct device *dev,
				uart_irq_callback_user_data_t callback,
				void *user_data)
{
	(void)dev;
	(void)callback;
	(void)user_data;
	return -ENOSYS;
}

static inline void uart_irq_update(const struct device *dev) { (void)dev; }

static inline int uart_irq_is_pending(const struct device *dev)
{
	(void)dev;
	return 0;
}

static inline int uart_irq_rx_ready(const struct device *dev)
{
	(void)dev;
	return 0;
}

static inline int uart_fifo_read(const struct device *dev, uint8_t *data,
				 const int capacity)
{
	(void)dev;
	(void)data;
	(void)capacity;
	return -ENOSYS;
}

static inline int uart_err_check(const struct device *dev)
{
	(void)dev;
	return -ENOSYS;
}

static inline int uart_irq_tx_ready(const struct device *dev)
{
	(void)dev;
	return 0;
}

static inline int uart_fifo_fill(const struct device *dev, const uint8_t *data,
				 int length)
{
	(void)dev;
	(void)data;
	(void)length;
	return -ENOSYS;
}

static inline int uart_irq_tx_complete(const struct device *dev)
{
	(void)dev;
	return -ENOSYS;
}

static inline void uart_irq_rx_enable(const struct device *dev) { (void)dev; }

static inline void uart_irq_rx_disable(const struct device *dev) { (void)dev; }

static inline void uart_irq_tx_enable(const struct device *dev) { (void)dev; }

static inline void uart_irq_tx_disable(const struct device *dev) { (void)dev; }

static inline int uart_config_get(const struct device *dev,
				  struct uart_config *config)
{
	(void)dev;
	(void)config;
	return -ENOSYS;
}

static inline int uart_configure(const struct device *dev,
				 const struct uart_config *config)
{
	(void)dev;
	(void)config;
	return -ENOSYS;
}

static inline int uart_line_ctrl_get(const struct device *dev, uint32_t control,
				     uint32_t *value)
{
	(void)dev;
	(void)control;
	(void)value;
	return -ENOSYS;
}

#endif
