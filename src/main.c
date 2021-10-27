/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr.h>
#include <stdio.h>
#include <drivers/uart.h>
#include <string.h>
#include <random/rand32.h>
#include <net/mqtt.h>
#include <net/socket.h>
#include <modem/at_cmd.h>
#include <modem/lte_lc.h>
#include <logging/log.h>

#include <drivers/gpio.h>

#include "pm/pm.h"
#include "power/reboot.h"

#if defined(CONFIG_MODEM_KEY_MGMT)
#include <modem/modem_key_mgmt.h>
#endif
#if defined(CONFIG_LWM2M_CARRIER)
#include <lwm2m_carrier.h>
#endif
#include <dk_buttons_and_leds.h>

#include "certificates.h"

//IotHub Device Connection Parameters
static const char* CONFIG_MQTT_BROKER_PASSWORD = "SharedAccessSignature sr=Aquahub.azure-devices.net%2Fdevices%2Fdemodevice&sig=hN0X4frfUS4fmqcTRizfL8aZUO6FVSYmg%2Bs%2BVM5GVxI%3D&se=1646332644";
static const char* CONFIG_MQTT_BROKER_USERNAME = "Aquahub.azure-devices.net/demodevice/?api-version=2018-06-30";

// Fanstel Gateway Version
#define BLG840_M2 // new gateway
#ifndef BLG840_M2
	#define BLG840_M1 // old gateway
#endif

#ifdef BLG840_M1
	/* GPIO for LED */
	static struct device *led_dev;

	/* The devicetree node identifier for the "led0" alias. */
	#define RED_LED_NODE	DT_ALIAS(led0)

	#if DT_NODE_HAS_STATUS(RED_LED_NODE, okay)
	#define RED_LED			DT_GPIO_LABEL(RED_LED_NODE, gpios)
	#define RED_LED_PIN		DT_GPIO_PIN(RED_LED_NODE, gpios)
	#define RED_LED_FLAGS	DT_GPIO_FLAGS(RED_LED_NODE, gpios)
	#else
	/* A build error here means your board isn't set up to blink an LED. */
	#error "Unsupported board: led0 devicetree alias is not defined"
	#define RED_LED	""
	#define RED_LED_PIN	0
	#define RED_LED_FLAGS	0
	#endif

	/* The devicetree node identifier for the "led1" alias. */
	#define BLU_LED_NODE DT_ALIAS(led1)

	#if DT_NODE_HAS_STATUS(BLU_LED_NODE, okay)
	#define BLU_LED			DT_GPIO_LABEL(BLU_LED_NODE, gpios)
	#define BLU_LED_PIN		DT_GPIO_PIN(BLU_LED_NODE, gpios)
	#define BLU_LED_FLAGS	DT_GPIO_FLAGS(BLU_LED_NODE, gpios)
	#else
	/* A build error here means your board isn't set up to blink an LED. */
	#error "Unsupported board: led0 devicetree alias is not defined"
	#define BLU_LED	""
	#define BLU_LED_PIN	0
	#define BLU_LED_FLAGS	0
	#endif

	/* Thread to Blink LED */
	#define MY_TH_STACK_SIZE 500
	#define MY_TH_PRIORITY 5

	K_THREAD_STACK_DEFINE(my_th_stack_area, MY_TH_STACK_SIZE);
	struct k_thread my_thread_data;

	volatile bool lte_connecting = false;
	volatile bool mqtt_connecting = false;
	extern void blink_led_entry_point();

#endif

LOG_MODULE_REGISTER(mqtt_simple, CONFIG_MQTT_SIMPLE_LOG_LEVEL);

/* Buffers for MQTT client. */
static uint8_t rx_buffer[CONFIG_MQTT_MESSAGE_BUFFER_SIZE];
static uint8_t tx_buffer[CONFIG_MQTT_MESSAGE_BUFFER_SIZE];
static uint8_t payload_buf[CONFIG_MQTT_PAYLOAD_BUFFER_SIZE];

/* The mqtt client struct */
static struct mqtt_client client;

/* MQTT Broker details. */
static struct sockaddr_storage broker;

/* File descriptor */
static struct pollfd fds;

/* UART variables */
#define UART_BUF_SIZE   256
const static struct device *dev_uart;
static K_FIFO_DEFINE(fifo_uart_tx_data);
static K_FIFO_DEFINE(fifo_uart_rx_data);
static uint8_t uart_rxbuf[UART_BUF_SIZE];
static uint16_t uart_rx_leng = 0;
struct uart_data_t {
	void  *fifo_reserved;
	uint8_t    data[UART_BUF_SIZE];
	uint16_t   len;
};
uint8_t data_uart[UART_BUF_SIZE];
uint8_t data_uart_temp[UART_BUF_SIZE];
volatile bool unsent_data = false;
volatile bool nRF52840_started = false;

static void sendCloudMsg(void);

typedef enum {
     UART_STRING_OK,                    // Complete transmission, string looks normal
     UART_STRING_INCOMPLETE,            // Transmission is likely not yet complete
     UART_STRING_THROWAWAY_COMPLETE,     // Transmission is complete, but string has some unwanted chars
} uart_str_check_t;

// since printk not showing
#define DEBUG_PRINT_BUF_SIZE 500
uint8_t debug_print[DEBUG_PRINT_BUF_SIZE];

/* Timer stuff */
//#define APP_USE_TIMERS_FOR_WORKQUEUE // alternate option is to submit to workqueue directly in uart_cb
#ifdef APP_USE_TIMERS_FOR_WORKQUEUE
    #define TIMER_INTERVAL_MSEC 500
	volatile uint16_t nRun = 0;
    struct k_timer timer_msg_send;
#endif

// Workqueue stuff
#define MY_WQ_STACK_SIZE 2048
#define MY_WQ_PRIORITY 5
K_THREAD_STACK_DEFINE(my_wq_stack_area, MY_WQ_STACK_SIZE);

struct k_work_q  	queue_work_msg_send; // Work queue structure

struct k_work    	work_msg_send;       			// Work item structure
void work_handler_msg_send(struct k_work * work);	// Work item handler
K_WORK_DEFINE(my_work, work_handler_msg_send);		// Define work item

#ifdef APP_USE_TIMERS_FOR_WORKQUEUE
void timer_handler_msg_send(struct k_timer *timer_id);
#endif


#if defined(CONFIG_MQTT_LIB_TLS)
static int certificates_provision(void)
{
	int err = 0;

	LOG_INF("Provisioning certificates");

#if defined(CONFIG_NRF_MODEM_LIB) && defined(CONFIG_MODEM_KEY_MGMT)

	err = modem_key_mgmt_write(CONFIG_MQTT_TLS_SEC_TAG,
				   MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
				   CA_CERTIFICATE,
				   strlen(CA_CERTIFICATE));
	if (err) {
		LOG_ERR("Failed to provision CA certificate: %d", err);
		return err;
	}

#elif defined(CONFIG_BOARD_QEMU_X86) && defined(CONFIG_NET_SOCKETS_SOCKOPT_TLS)

	err = tls_credential_add(CONFIG_MQTT_TLS_SEC_TAG,
				 TLS_CREDENTIAL_CA_CERTIFICATE,
				 CA_CERTIFICATE,
				 sizeof(CA_CERTIFICATE));
	if (err) {
		LOG_ERR("Failed to register CA certificate: %d", err);
		return err;
	}

#endif

	return err;
}
#endif /* defined(CONFIG_MQTT_LIB_TLS) */

#if defined(CONFIG_NRF_MODEM_LIB)

/**@brief Recoverable modem library error. */
void nrf_modem_recoverable_error_handler(uint32_t err)
{
	LOG_ERR("Modem library recoverable error: %u", (unsigned int)err);
}

#endif /* defined(CONFIG_NRF_MODEM_LIB) */

#if defined(CONFIG_LWM2M_CARRIER)
K_SEM_DEFINE(carrier_registered, 0, 1);
int lwm2m_carrier_event_handler(const lwm2m_carrier_event_t *event)
{
	switch (event->type) {
	case LWM2M_CARRIER_EVENT_BSDLIB_INIT:
		LOG_INF("LWM2M_CARRIER_EVENT_BSDLIB_INIT");
		break;
	case LWM2M_CARRIER_EVENT_CONNECTING:
		LOG_INF("LWM2M_CARRIER_EVENT_CONNECTING");
		break;
	case LWM2M_CARRIER_EVENT_CONNECTED:
		LOG_INF("LWM2M_CARRIER_EVENT_CONNECTED");
		break;
	case LWM2M_CARRIER_EVENT_DISCONNECTING:
		LOG_INF("LWM2M_CARRIER_EVENT_DISCONNECTING");
		break;
	case LWM2M_CARRIER_EVENT_DISCONNECTED:
		LOG_INF("LWM2M_CARRIER_EVENT_DISCONNECTED");
		break;
	case LWM2M_CARRIER_EVENT_BOOTSTRAPPED:
		LOG_INF("LWM2M_CARRIER_EVENT_BOOTSTRAPPED");
		break;
	case LWM2M_CARRIER_EVENT_REGISTERED:
		LOG_INF("LWM2M_CARRIER_EVENT_REGISTERED");
		k_sem_give(&carrier_registered);
		break;
	case LWM2M_CARRIER_EVENT_DEFERRED:
		LOG_INF("LWM2M_CARRIER_EVENT_DEFERRED");
		break;
	case LWM2M_CARRIER_EVENT_FOTA_START:
		LOG_INF("LWM2M_CARRIER_EVENT_FOTA_START");
		break;
	case LWM2M_CARRIER_EVENT_REBOOT:
		LOG_INF("LWM2M_CARRIER_EVENT_REBOOT");
		break;
	case LWM2M_CARRIER_EVENT_LTE_READY:
		LOG_INF("LWM2M_CARRIER_EVENT_LTE_READY");
		break;
	case LWM2M_CARRIER_EVENT_ERROR:
		LOG_ERR("LWM2M_CARRIER_EVENT_ERROR: code %d, value %d",
			((lwm2m_carrier_event_error_t *)event->data)->code,
			((lwm2m_carrier_event_error_t *)event->data)->value);
		break;
	default:
		LOG_WRN("Unhandled LWM2M_CARRIER_EVENT: %d", event->type);
		break;
	}

	return 0;
}
#endif /* defined(CONFIG_LWM2M_CARRIER) */

/**@brief Function to print strings without null-termination
 */
static void data_print(uint8_t *prefix, uint8_t *data, size_t len)
{
	char buf[len + 1];

	memcpy(buf, data, len);
	buf[len] = 0;
	LOG_INF("%s%s", log_strdup(prefix), log_strdup(buf));
}

/**@brief Function to publish data on the configured topic
 */
static int data_publish(struct mqtt_client *c, enum mqtt_qos qos,
	uint8_t *data, size_t len)
{
	struct mqtt_publish_param param;

	param.message.topic.qos = qos;
	param.message.topic.topic.utf8 = CONFIG_MQTT_PUB_TOPIC;
	param.message.topic.topic.size = strlen(CONFIG_MQTT_PUB_TOPIC);
	param.message.payload.data = data;
	param.message.payload.len = len;
	param.message_id = sys_rand32_get();
	param.dup_flag = 0;
	param.retain_flag = 0;

	data_print("Publishing: ", data, len);
	LOG_INF("to topic: %s len: %u",
		CONFIG_MQTT_PUB_TOPIC,
		(unsigned int)strlen(CONFIG_MQTT_PUB_TOPIC));

	return mqtt_publish(c, &param);
}

/**@brief Function to subscribe to the configured topic
 */
static int subscribe(void)
{
	struct mqtt_topic subscribe_topic = {
		.topic = {
			.utf8 = CONFIG_MQTT_SUB_TOPIC,
			.size = strlen(CONFIG_MQTT_SUB_TOPIC)
		},
		.qos = MQTT_QOS_1_AT_LEAST_ONCE
	};

	const struct mqtt_subscription_list subscription_list = {
		.list = &subscribe_topic,
		.list_count = 1,
		.message_id = 1234
	};

	LOG_INF("Subscribing to: %s len %u", CONFIG_MQTT_SUB_TOPIC,
		(unsigned int)strlen(CONFIG_MQTT_SUB_TOPIC));

	return mqtt_subscribe(&client, &subscription_list);
}

/**@brief Function to read the published payload.
 */
static int publish_get_payload(struct mqtt_client *c, size_t length)
{
	if (length > sizeof(payload_buf)) {
		return -EMSGSIZE;
	}

	return mqtt_readall_publish_payload(c, payload_buf, length);
}

/**@brief MQTT client event handler
 */
void mqtt_evt_handler(struct mqtt_client *const c,
		      const struct mqtt_evt *evt)
{
	int err;

	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result != 0) {
			LOG_ERR("MQTT connect failed: %d", evt->result);
			break;
		}

		LOG_INF("MQTT client connected");
		subscribe();
		break;

	case MQTT_EVT_DISCONNECT:
		LOG_INF("MQTT client disconnected: %d", evt->result);
		break;

	case MQTT_EVT_PUBLISH: {
		const struct mqtt_publish_param *p = &evt->param.publish;

		LOG_INF("MQTT PUBLISH result=%d len=%d",
			evt->result, p->message.payload.len);
		err = publish_get_payload(c, p->message.payload.len);

		if (p->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
			const struct mqtt_puback_param ack = {
				.message_id = p->message_id
			};

			/* Send acknowledgment. */
			mqtt_publish_qos1_ack(&client, &ack);
		}

		if (err >= 0) {
			data_print("Received: ", payload_buf,
				p->message.payload.len);
			/* Echo back received data */
			data_publish(&client, MQTT_QOS_1_AT_LEAST_ONCE,
				payload_buf, p->message.payload.len);
		} else {
			LOG_ERR("publish_get_payload failed: %d", err);
			LOG_INF("Disconnecting MQTT client...");

			err = mqtt_disconnect(c);
			if (err) {
				LOG_ERR("Could not disconnect: %d", err);
			}
		}
	} break;

	case MQTT_EVT_PUBACK:
		if (evt->result != 0) {
			LOG_ERR("MQTT PUBACK error: %d", evt->result);
			break;
		}

		LOG_INF("PUBACK packet id: %u", evt->param.puback.message_id);
		break;

	case MQTT_EVT_SUBACK:
		if (evt->result != 0) {
			LOG_ERR("MQTT SUBACK error: %d", evt->result);
			break;
		}

#ifdef BLG840_M1
		mqtt_connecting = false; // ends blinky thread
#endif

		LOG_INF("SUBACK packet id: %u", evt->param.suback.message_id);
		break;

	case MQTT_EVT_PINGRESP:
		if (evt->result != 0) {
			LOG_ERR("MQTT PINGRESP error: %d", evt->result);
		}
		break;

	default:
		LOG_INF("Unhandled MQTT event type: %d", evt->type);
		break;
	}
}

/**@brief Resolves the configured hostname and
 * initializes the MQTT broker structure
 */
static int broker_init(void)
{
	int err;
	struct addrinfo *result;
	struct addrinfo *addr;
	struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM
	};

	err = getaddrinfo(CONFIG_MQTT_BROKER_HOSTNAME, NULL, &hints, &result);
	if (err) {
		LOG_ERR("getaddrinfo failed: %d", err);
		return -ECHILD;
	}

	addr = result;

	/* Look for address of the broker. */
	while (addr != NULL) {
		/* IPv4 Address. */
		if (addr->ai_addrlen == sizeof(struct sockaddr_in)) {
			struct sockaddr_in *broker4 =
				((struct sockaddr_in *)&broker);
			char ipv4_addr[NET_IPV4_ADDR_LEN];

			broker4->sin_addr.s_addr =
				((struct sockaddr_in *)addr->ai_addr)
				->sin_addr.s_addr;
			broker4->sin_family = AF_INET;
			broker4->sin_port = htons(CONFIG_MQTT_BROKER_PORT);

			inet_ntop(AF_INET, &broker4->sin_addr.s_addr,
				  ipv4_addr, sizeof(ipv4_addr));
			LOG_INF("IPv4 Address found %s", log_strdup(ipv4_addr));

			break;
		} else {
			LOG_ERR("ai_addrlen = %u should be %u or %u",
				(unsigned int)addr->ai_addrlen,
				(unsigned int)sizeof(struct sockaddr_in),
				(unsigned int)sizeof(struct sockaddr_in6));
		}

		addr = addr->ai_next;
	}

	/* Free the address. */
	freeaddrinfo(result);

	return err;
}

#if defined(CONFIG_NRF_MODEM_LIB)
#define IMEI_LEN 15
#define CGSN_RESPONSE_LENGTH 19
#define CLIENT_ID_LEN sizeof("nrf-") + IMEI_LEN
#else
#define RANDOM_LEN 10
#define CLIENT_ID_LEN sizeof(CONFIG_BOARD) + 1 + RANDOM_LEN
#endif /* defined(CONFIG_NRF_MODEM_LIB) */

/* Function to get the client id */
static const uint8_t* client_id_get(void)
{
	static uint8_t client_id[MAX(sizeof(CONFIG_MQTT_CLIENT_ID),
				     CLIENT_ID_LEN)];

	if (strlen(CONFIG_MQTT_CLIENT_ID) > 0) {
		snprintf(client_id, sizeof(client_id), "%s",
			 CONFIG_MQTT_CLIENT_ID);
		goto exit;
	}

#if defined(CONFIG_NRF_MODEM_LIB)
	char imei_buf[CGSN_RESPONSE_LENGTH + 1];
	int err;

	if (!IS_ENABLED(CONFIG_AT_CMD_SYS_INIT)) {
		err = at_cmd_init();
		if (err) {
			LOG_ERR("at_cmd failed to initialize, error: %d", err);
			goto exit;
		}
	}

	err = at_cmd_write("AT+CGSN", imei_buf, sizeof(imei_buf), NULL);
	if (err) {
		LOG_ERR("Failed to obtain IMEI, error: %d", err);
		goto exit;
	}

	imei_buf[IMEI_LEN] = '\0';

	snprintf(client_id, sizeof(client_id), "nrf-%.*s", IMEI_LEN, imei_buf);
#else
	uint32_t id = sys_rand32_get();
	snprintf(client_id, sizeof(client_id), "%s-%010u", CONFIG_BOARD, id);
#endif /* !defined(NRF_CLOUD_CLIENT_ID) */

exit:
	LOG_DBG("client_id = %s", log_strdup(client_id));

	return client_id;
}

/**@brief Initialize the MQTT client structure
 */
static int client_init(struct mqtt_client *client)
{
	int err;

	mqtt_client_init(client);

	err = broker_init();
	if (err) {
		// LOG_ERR("Failed to initialize broker connection");
		return err;
	}

	/* MQTT client configuration */
    static struct mqtt_utf8 password;
    static struct mqtt_utf8 user_name;

    password.utf8 = (uint8_t*)CONFIG_MQTT_BROKER_PASSWORD;
    password.size = strlen(CONFIG_MQTT_BROKER_PASSWORD);
    user_name.utf8 = (uint8_t*)CONFIG_MQTT_BROKER_USERNAME;
    user_name.size = strlen(CONFIG_MQTT_BROKER_USERNAME);

    client->broker = &broker;
	client->evt_cb = mqtt_evt_handler;
	client->client_id.utf8 = client_id_get();
	client->client_id.size = strlen(client->client_id.utf8);
	client->password = &password;
    client->user_name = &user_name;
	client->protocol_version = MQTT_VERSION_3_1_1;

	/* MQTT buffers configuration */
	client->rx_buf = rx_buffer;
	client->rx_buf_size = sizeof(rx_buffer);
	client->tx_buf = tx_buffer;
	client->tx_buf_size = sizeof(tx_buffer);

	/* MQTT transport configuration */
#if defined(CONFIG_MQTT_LIB_TLS)
	struct mqtt_sec_config *tls_cfg = &(client->transport).tls.config;
	static sec_tag_t sec_tag_list[] = { CONFIG_MQTT_TLS_SEC_TAG };

	LOG_INF("TLS enabled");
	client->transport.type = MQTT_TRANSPORT_SECURE;

	tls_cfg->peer_verify = CONFIG_MQTT_TLS_PEER_VERIFY;
	tls_cfg->cipher_count = 0;
	tls_cfg->cipher_list = NULL;
	tls_cfg->sec_tag_count = ARRAY_SIZE(sec_tag_list);
	tls_cfg->sec_tag_list = sec_tag_list;
	tls_cfg->hostname = CONFIG_MQTT_BROKER_HOSTNAME;

#if defined(CONFIG_NRF_MODEM_LIB)
	tls_cfg->session_cache = IS_ENABLED(CONFIG_MQTT_TLS_SESSION_CACHING) ?
					    TLS_SESSION_CACHE_ENABLED :
					    TLS_SESSION_CACHE_DISABLED;
#else
	/* TLS session caching is not supported by the Zephyr network stack */
	tls_cfg->session_cache = TLS_SESSION_CACHE_DISABLED;

#endif

#else
	client->transport.type = MQTT_TRANSPORT_NON_SECURE;
#endif

	return err;
}

/**@brief Initialize the file descriptor structure used by poll.
 */
static int fds_init(struct mqtt_client *c)
{
	if (c->transport.type == MQTT_TRANSPORT_NON_SECURE) {
		fds.fd = c->transport.tcp.sock;
	} else {
#if defined(CONFIG_MQTT_LIB_TLS)
		fds.fd = c->transport.tls.sock;
#else
		return -ENOTSUP;
#endif
	}

	fds.events = POLLIN;

	return 0;
}

#if defined(CONFIG_DK_LIBRARY)
static void button_handler(uint32_t button_states, uint32_t has_changed)
{
	if (has_changed & button_states &
	    BIT(CONFIG_BUTTON_EVENT_BTN_NUM - 1)) {
		int ret;

		ret = data_publish(&client,
				   MQTT_QOS_1_AT_LEAST_ONCE,
				   CONFIG_BUTTON_EVENT_PUBLISH_MSG,
				   sizeof(CONFIG_BUTTON_EVENT_PUBLISH_MSG)-1);
		if (ret) {
			LOG_ERR("Publish failed: %d", ret);
		}
	}
}
#endif

/**@brief Configures modem to provide LTE link. Blocks until link is
 * successfully established.
 */
static int modem_configure(void)
{
#if defined(CONFIG_LTE_LINK_CONTROL)
	/* Turn off LTE power saving features for a more responsive demo. Also,
	 * request power saving features before network registration. Some
	 * networks rejects timer updates after the device has registered to the
	 * LTE network.
	 */
	LOG_INF("Disabling PSM and eDRX");
	lte_lc_psm_req(false);
	lte_lc_edrx_req(false);

	if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT)) {
		/* Do nothing, modem is already turned on
		 * and connected.
		 */
	} else {
#if defined(CONFIG_LWM2M_CARRIER)
		/* Wait for the LWM2M_CARRIER to configure the modem and
		 * start the connection.
		 */
		LOG_INF("Waitng for carrier registration...");
		k_sem_take(&carrier_registered, K_FOREVER);
		LOG_INF("Registered!");
#else /* defined(CONFIG_LWM2M_CARRIER) */
		int err;

		LOG_INF("LTE Link Connecting...");
		err = lte_lc_init_and_connect();
		if (err == -EALREADY) {
			LOG_INF("LTE already initialized... now connecting...");
			err = lte_lc_connect();
		}
		
		if(err)
		{
			LOG_INF("Failed to establish LTE connection: %d", err);
			return err;
		}
		LOG_INF("LTE Link Connected!");

#endif /* defined(CONFIG_LWM2M_CARRIER) */
	}
#endif /* defined(CONFIG_LTE_LINK_CONTROL) */

	return 0;
}

// check if the string received over UART is "Application started"
static uart_str_check_t check_uart_first_str()
{
    // Expected format is: "Application started/n"

    // Start by checking ending of string
    // If not properly terminated then assume more data is still coming
    if( uart_rxbuf[uart_rx_leng-1] != 0x0a )  // 0x0a == /n
    {
        return UART_STRING_INCOMPLETE;
    }

    // Check the string contains Application started
    if( strstr(uart_rxbuf, "Application started") != NULL )
    {
        return UART_STRING_OK;
    }
       else
       {
               return UART_STRING_INCOMPLETE; // TODO: This is not ideal
       }

}

// check if the string received over UART is a complete packet or garbage data
static uart_str_check_t check_uart_str()
{
    // Expected format is: "/r/n{ ........ }/r/n"

    // Start by checking ending of string
    // If not properly terminated then assume more data is still coming
    if( (uart_rxbuf[uart_rx_leng-3] != 0x7d) || // 0x7d == }
        (uart_rxbuf[uart_rx_leng-2] != 0x0d) || // 0x0d == /r
        (uart_rxbuf[uart_rx_leng-1] != 0x0a) )  // 0x0a == /n
    {
        return UART_STRING_INCOMPLETE;
    }

    // Check the opening of the string
    // If it does not start properly then there was some error in transmission
    if( (uart_rxbuf[0] != 0x0d) || // 0x0d == /r
        (uart_rxbuf[1] != 0x0a) || // 0x0a == /n
        (uart_rxbuf[2] != 0x7b) )  // 0x7b == {
    {
        return UART_STRING_THROWAWAY_COMPLETE;
    }
    
    // Check the string only contains 2 LF, 2 CR, 1 {, 1 }
    uint8_t num_LF = 0; // \n, 0x0a
    uint8_t num_CR = 0; // \r, 0x0d
    uint8_t num_ob = 0; // {,  0x7b
    uint8_t num_cb = 0; // },  0x7d
    
    // count each character by iterating over buffer
    for( uint8_t i = 0; i < uart_rx_leng; i++)
    {
        switch( uart_rxbuf[i] )
        {
            case 0x0a:
                num_LF++;
                break;

            case 0x0d:
                num_CR++;
                break;

            case 0x7b:
                num_ob++;
                break;

            case 0x7d:
                num_cb++;
                break;
        }
    }

    if( num_LF != 2 ||
        num_CR != 2 ||
        num_ob != 1 ||
        num_cb != 1 )
    {
        return UART_STRING_THROWAWAY_COMPLETE;
    }
    else
    {
        return UART_STRING_OK;
    }

}


static void uart_cb(const struct device *uart, void *user_data)
{
    uint8_t temp_rx[UART_BUF_SIZE];
    uint16_t data_length=0;
    uart_irq_update(uart);

    if (uart_irq_rx_ready(uart)) // get new characters from FIFO
    {
        data_length = uart_fifo_read(uart, temp_rx, sizeof(temp_rx));
        temp_rx[data_length] = 0;
    }

    for(int i=0; i<data_length; i++) // copy from temp into rxbuf
    {
        uart_rxbuf[uart_rx_leng] = temp_rx[i];
        uart_rx_leng++;
    }

    if( uart_rx_leng >= UART_BUF_SIZE ) // Buffer filled up, something went wrong
    {
        // discard and start over
        memset(uart_rxbuf, '\0', sizeof(uart_rxbuf));
        uart_rx_leng = 0; 
    }
    else if( !nRF52840_started && (uart_rx_leng > 15) )
	{
		uint8_t ret_val = check_uart_first_str();

		switch( ret_val )
		{
				case UART_STRING_OK:
						nRF52840_started = true;
		memset(uart_rxbuf, '\0', sizeof(uart_rxbuf));
		uart_rx_leng = 0;
		break;
				case UART_STRING_INCOMPLETE:
						break;
		}
	}
	else if( uart_rx_leng > 100 ) // assume packets shorter than this are garbage or incomplete
    {
        uint8_t ret_val = check_uart_str();
        
        switch( ret_val )
        {
        
            case UART_STRING_OK: // a complete string with no issues
                if (unsent_data == false)
                {
                    memset(data_uart, '\0', sizeof(data_uart)); // clear the UART data string
                    strncpy(data_uart, uart_rxbuf+2, uart_rx_leng - 4); // copy in the new UART data
                    unsent_data = true; // set flag for new data ready to be sent to cloud

                    #ifndef APP_USE_TIMERS_FOR_WORKQUEUE
                        k_work_submit_to_queue(&queue_work_msg_send, &work_msg_send); // submit to work queue
                    #endif

                    memset(uart_rxbuf, '\0', sizeof(uart_rxbuf));
                    uart_rx_leng = 0; 
                }
                break;
            
            case UART_STRING_INCOMPLETE:
                // do nothing, wait for string to terminate
                break;

            case UART_STRING_THROWAWAY_COMPLETE:
//                memset(debug_print, '\0', sizeof(debug_print));
//                sprintf(debug_print, "%s", uart_rxbuf);
                memset(uart_rxbuf, '\0', sizeof(uart_rxbuf));
                uart_rx_leng = 0;
                break;
        }
    }

    if (uart_irq_tx_ready(uart))
    {
        struct uart_data_t *buf =
                k_fifo_get(&fifo_uart_tx_data, K_NO_WAIT);
        uint16_t written = 0;
        /* Nothing in the FIFO, nothing to send */
        if (!buf)
        {
            uart_irq_tx_disable(uart);
            return;
        }
        while (buf->len > written)
        {
            written += uart_fifo_fill(uart,
                                      &buf->data[written],
                                      buf->len - written);
        }
        while (!uart_irq_tx_complete(uart))
        {
            /* Wait for the last byte to get
             * shifted out of the module
             */
        }
        if (k_fifo_is_empty(&fifo_uart_tx_data))
        {
                uart_irq_tx_disable(uart);
        }
        k_free(buf);
    }
}


static int init_uart(void)
{
    dev_uart = device_get_binding("UART_0");
    if (!dev_uart)
    {
        return -ENXIO;
    }

    uart_irq_callback_set(dev_uart, uart_cb);
    uart_irq_rx_enable(dev_uart);

    return 0;
}

#ifdef BLG840_M1
extern void blink_led_entry_point()
{
	bool led_state = false;

	while( lte_connecting )
	{
		gpio_pin_set(led_dev, BLU_LED_PIN, (int)led_state);
		led_state = !led_state;
		k_sleep(K_SECONDS(1));
	}

	while( mqtt_connecting )
	{
		gpio_pin_set(led_dev, BLU_LED_PIN, (int)led_state);
		led_state = !led_state;
		k_sleep(K_SECONDS(0.5));
	}

	// leave in solid ON state
	gpio_pin_set(led_dev, BLU_LED_PIN, 0);
	return;
}
#endif


static void sendCloudMsg(void)
{
    int err = data_publish(&client, MQTT_QOS_1_AT_LEAST_ONCE, data_uart, strlen(data_uart));
	
    if (err )
    {
        LOG_INF("Publish error: %d", err);
    }
}


void work_handler_msg_send(struct k_work *work)
{
    sendCloudMsg();
    unsent_data = false;
}

#ifdef APP_USE_TIMERS_FOR_WORKQUEUE
void timer_handler_msg_send(struct k_timer *timer_id)
{
    //  printk("timer____\r\n");
    if (nRun == 0)
    {
        if (unsent_data == true) 
        {
            printk("___ALL TRUE \r\n");
            //      nRun=1;
            unsent_data = false;
            k_work_submit_to_queue(&queue_work_msg_send, &work_msg_send);
        }
    }
}
#endif

void main(void)
{
	int err;
	uint8_t lte_connect_attempt = 0;
	uint32_t mqtt_connect_attempt = 0;

	LOG_INF("The MQTT simple sample started");

#ifdef BLG840_M1

	led_dev = device_get_binding(RED_LED);
	if (led_dev == NULL) {
		return;
	}

    err = gpio_pin_configure(led_dev, RED_LED_PIN, GPIO_OUTPUT_INACTIVE | RED_LED_FLAGS); //p0.02 == LED RED, logic reversed?
	if (err < 0) {
		return;
	}
    err = gpio_pin_configure(led_dev, BLU_LED_PIN, GPIO_OUTPUT_ACTIVE | BLU_LED_FLAGS); //p0.03 == LED BLUE, logic reversed?
	if (err < 0) {
		return;
	}

#endif

	err = init_uart();
    if (!err)
    {
        //printk("UART enabled\n");
    }
    else
    {
        printk("UART enable error\n");
        #if !defined(CONFIG_DEBUG) && defined(CONFIG_REBOOT)
            sys_reboot(0);
        #endif 
    }

	k_sleep(K_SECONDS(15));

	while(!nRF52840_started){
			printk("Waiting for nRF52840\n");
			k_sleep(K_SECONDS(1));
	}


#if defined(CONFIG_MQTT_LIB_TLS)
	err = certificates_provision();
	if (err != 0) {
		// LOG_ERR("Failed to provision certificates");
		return;
	}
#endif /* defined(CONFIG_MQTT_LIB_TLS) */

#ifdef BLG840_M1
	// to blink LED
	lte_connecting = true;
	k_tid_t my_tid = k_thread_create(&my_thread_data, my_th_stack_area,
								K_THREAD_STACK_SIZEOF(my_th_stack_area),
								blink_led_entry_point,
								NULL, NULL, NULL,
								MY_TH_PRIORITY, 0, K_NO_WAIT);
	//Turn off RED LED
	gpio_pin_set(led_dev, RED_LED_PIN, 1);
#endif

	do {
		// For nRF52840
		LOG_INF("STATE_LTE_CONNECTING");
	
		err = modem_configure();
		if (err) {
			LOG_INF("Retrying in %d seconds. Attempt %d failed.",
				CONFIG_LTE_CONNECT_RETRY_DELAY_S, lte_connect_attempt+1);
			
			if (lte_connect_attempt++ >= 4)
			{
#if !defined(CONFIG_DEBUG) && defined(CONFIG_REBOOT)
				sys_reboot(0);
#endif
			}
			k_sleep(K_SECONDS(CONFIG_LTE_CONNECT_RETRY_DELAY_S));
		}
	} while (err);

#ifdef BLG840_M1
	mqtt_connecting = true;
	lte_connecting = false;
#endif

	// For nRF52840
	LOG_INF("STATE_MQTT_CONNECTING");
	err = client_init(&client);
	if (err != 0) {
		LOG_ERR("client_init: %d", err);
		return;
	}

#if defined(CONFIG_DK_LIBRARY)
	dk_buttons_init(button_handler);
#endif

do_connect:
	if (mqtt_connect_attempt++ > 0) {
		LOG_INF("Reconnecting in %d seconds...",
			CONFIG_MQTT_RECONNECT_DELAY_S);
		k_sleep(K_SECONDS(CONFIG_MQTT_RECONNECT_DELAY_S));
	}
	err = mqtt_connect(&client);
	if (err != 0) {
		LOG_ERR("mqtt_connect %d", err);
		goto do_connect;
	}

	err = fds_init(&client);
	if (err != 0) {
		LOG_ERR("fds_init: %d", err);
		return;
	}
    
	/* Packet Sending */
    k_work_init(&work_msg_send, work_handler_msg_send);   // Initialize the work items that will be submitted
    k_work_queue_start( &queue_work_msg_send, my_wq_stack_area,        
                    K_THREAD_STACK_SIZEOF(my_wq_stack_area), MY_WQ_PRIORITY, NULL); // Initialize & start the work queue that they are submitted to

    #ifdef APP_USE_TIMERS_FOR_WORKQUEUE
        k_timer_init(&timer_msg_send, timer_handler_msg_send, NULL);
        k_timer_start(&timer_msg_send, K_MSEC(TIMER_INTERVAL_MSEC), K_MSEC(TIMER_INTERVAL_MSEC));
    #endif

	while (1) {
		err = poll(&fds, 1, mqtt_keepalive_time_left(&client));
		if (err < 0) {
			LOG_ERR("poll: %d", errno);
			break;
		}

		err = mqtt_live(&client);
		if ((err != 0) && (err != -EAGAIN)) {
			LOG_ERR("ERROR: mqtt_live: %d", err);
			break;
		}

		if ((fds.revents & POLLIN) == POLLIN) {
			err = mqtt_input(&client);
			if (err != 0) {
				LOG_ERR("mqtt_input: %d", err);
				break;
			}
		}

		if ((fds.revents & POLLERR) == POLLERR) {
			// LOG_ERR("POLLERR");
			break;
		}

		if ((fds.revents & POLLNVAL) == POLLNVAL) {
			// LOG_ERR("POLLNVAL");
			break;
		}
	}

	LOG_DBG("Disconnecting MQTT client...");

	err = mqtt_disconnect(&client);
	if (err) {
		LOG_ERR("Could not disconnect MQTT client: %d", err);
	}

	// rebooting is more simple to implement for now
	#if !defined(CONFIG_DEBUG) && defined(CONFIG_REBOOT)
		sys_reboot(0);
	#endif
	// goto do_connect;
}
