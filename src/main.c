/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>
#include <stdio.h>
#include <drivers/uart.h>
#include <string.h>

#include <net/mqtt.h>
#include <net/socket.h>
#include <lte_lc.h>

#include "power/power.h"
#include "power/reboot.h"

#if defined(CONFIG_LWM2M_CARRIER)
#include <lwm2m_carrier.h>
#endif

#include "certs.h"
#include <modem_key_mgmt.h>
#include <at_cmd.h> // todo: probably don't need this

static const char* CONFIG_MQTT_BROKER_PASSWORD = "SharedAccessSignature sr=Aquahub.azure-devices.net%2Fdevices%2Faquadevice&sig=MjnaRLJFb5UxsQu8PCQtrP8UUIyQbncREHFL%2BvlkCHw%3D&se=1614554527";
static const char* CONFIG_MQTT_BROKER_USERNAME = "Aquahub.azure-devices.net/aquadevice/?api-version=2018-06-30";

#if defined(CONFIG_MQTT_LIB_TLS)
static sec_tag_t sec_tag_list[] = { CONFIG_SEC_TAG };
#endif /* defined(CONFIG_MQTT_LIB_TLS) */ 

/* Buffers for MQTT client. */
static u8_t rx_buffer[CONFIG_MQTT_MESSAGE_BUFFER_SIZE];
static u8_t tx_buffer[CONFIG_MQTT_MESSAGE_BUFFER_SIZE];
static u8_t payload_buf[CONFIG_MQTT_PAYLOAD_BUFFER_SIZE];

/* The mqtt client struct */
static struct mqtt_client client;

/* MQTT Broker details. */
static struct sockaddr_storage broker;

/* Connected flag */
static bool connected;

/* File descriptor */
static struct pollfd fds;

#if defined(CONFIG_BSD_LIBRARY)

/* UART variables */
#define UART_BUF_SIZE           128
static struct device  *dev_uart;
static K_FIFO_DEFINE(fifo_uart_tx_data);
static K_FIFO_DEFINE(fifo_uart_rx_data);
static u8_t uart_rxbuf[UART_BUF_SIZE];
static u8_t uart_rx_leng = 0;
struct uart_data_t {
	void  *fifo_reserved;
	u8_t    data[UART_BUF_SIZE];
	u16_t   len;
};
uint8_t data_uart[UART_BUF_SIZE];
uint8_t data_uart_temp[UART_BUF_SIZE];
volatile bool unsent_data = false;

volatile uint16_t nRun = 0;
uint32_t numPublished = 0;
static void sendCloudMsg(void);

// since printk not showing
#define DEBUG_PRINT_BUF_SIZE 500
uint8_t debug_print[DEBUG_PRINT_BUF_SIZE];

// Timer stuff
//#define APP_USE_TIMERS_FOR_WORKQUEUE // alternate option is to submit to workqueue directly in uart_cb
#ifdef APP_USE_TIMERS_FOR_WORKQUEUE
    #define TIMER_INTERVAL_MSEC 500
    struct k_timer timer_msg_send;
#endif

// Workqueue stuff
#define MY_STACK_SIZE 512
#define MY_PRIORITY 5
K_THREAD_STACK_DEFINE(my_stack_area, MY_STACK_SIZE);

struct k_work_q  queue_work_msg_send; // Work queue structure
struct k_work    work_msg_send;       // Work item structure
void work_handler_msg_send(struct k_work * work);
K_WORK_DEFINE(my_work, work_handler_msg_send);
void timer_handler_msg_send(struct k_timer *timer_id);

/**@brief Recoverable BSD library error. */
void bsd_recoverable_error_handler(uint32_t err)
{
    printk("bsdlib recoverable error: %u\n", (unsigned int)err);
}

#endif /* defined(CONFIG_BSD_LIBRARY) */

#if defined(CONFIG_LWM2M_CARRIER)
K_SEM_DEFINE(carrier_registered, 0, 1);

void lwm2m_carrier_event_handler(const lwm2m_carrier_event_t *event)
{
    switch (event->type)
    {
    case LWM2M_CARRIER_EVENT_BSDLIB_INIT:
            printk("LWM2M_CARRIER_EVENT_BSDLIB_INIT\n");
            break;
    case LWM2M_CARRIER_EVENT_CONNECT:
            printk("LWM2M_CARRIER_EVENT_CONNECT\n");
            break;
    case LWM2M_CARRIER_EVENT_DISCONNECT:
            printk("LWM2M_CARRIER_EVENT_DISCONNECT\n");
            break;
    case LWM2M_CARRIER_EVENT_READY:
            printk("LWM2M_CARRIER_EVENT_READY\n");
            k_sem_give(&carrier_registered);
            break;
    case LWM2M_CARRIER_EVENT_FOTA_START:
            printk("LWM2M_CARRIER_EVENT_FOTA_START\n");
            break;
    case LWM2M_CARRIER_EVENT_REBOOT:
            printk("LWM2M_CARRIER_EVENT_REBOOT\n");
            break;
    }
}
#endif /* defined(CONFIG_LWM2M_CARRIER) */

/**@brief Function to print strings without null-termination
 */
static void data_print(u8_t *prefix, u8_t *data, size_t len)
{
    char buf[len + 1];

    memcpy(buf, data, len);
    buf[len] = 0;
    printk("%s%s\n", prefix, buf);
}

/**@brief Function to publish data on the configured topic
 */
static int data_publish(struct mqtt_client *c, enum mqtt_qos qos,
	u8_t *data, size_t len)
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

//    data_print("Publishing: ", data, len);
//    printk("to topic: %s len: %u\n",
//            CONFIG_MQTT_PUB_TOPIC,
//            (unsigned int)strlen(CONFIG_MQTT_PUB_TOPIC));

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

//    printk("Subscribing to: %s len %u\n", CONFIG_MQTT_SUB_TOPIC,
//            (unsigned int)strlen(CONFIG_MQTT_SUB_TOPIC));

    return mqtt_subscribe(&client, &subscription_list);
}

/**@brief Function to read the published payload.
 */
static int publish_get_payload(struct mqtt_client *c, size_t length)
{
    u8_t *buf = payload_buf;
    u8_t *end = buf + length;

    if (length > sizeof(payload_buf))
    {
        return -EMSGSIZE;
    }

    while (buf < end)
    {
        int ret = mqtt_read_publish_payload(c, buf, end - buf);

        if (ret < 0)
        {
            int err;

            if (ret != -EAGAIN)
            {
                return ret;
            }

            printk("mqtt_read_publish_payload: EAGAIN\n");

            err = poll(&fds, 1, K_SECONDS(CONFIG_MQTT_KEEPALIVE));
            if (err > 0 && (fds.revents & POLLIN) == POLLIN)
            {
                continue;
            }
            else
            {
                return -EIO;
            }
        }

        if (ret == 0)
        {
                return -EIO;
        }

        buf += ret;
    }

    return 0;
}

/**@brief MQTT client event handler
 */
void mqtt_evt_handler(struct mqtt_client *const c,
		      const struct mqtt_evt *evt)
{
    int err;

    switch (evt->type)
    {
    case MQTT_EVT_CONNACK:
        if (evt->result != 0) 
        {
            printk("MQTT connect failed %d\n", evt->result);
            break;
        }

        connected = true;
        //printk("[%s:%d] MQTT client connected!\n", __func__, __LINE__);
        subscribe();
        break;

    case MQTT_EVT_DISCONNECT:
        printk("[%s:%d] MQTT client disconnected %d\n", __func__,
               __LINE__, evt->result);

        connected = false;
        break;

    case MQTT_EVT_PUBLISH: {
        const struct mqtt_publish_param *p = &evt->param.publish;

//        printk("[%s:%d] MQTT PUBLISH result=%d len=%d\n", __func__,
//               __LINE__, evt->result, p->message.payload.len);
        err = publish_get_payload(c, p->message.payload.len);
        if (err >= 0)
        {
//            data_print("Received: ", payload_buf,
//                    p->message.payload.len);
            /* Echo back received data */
            data_publish(&client, MQTT_QOS_1_AT_LEAST_ONCE,
                    payload_buf, p->message.payload.len);
        }
        else
        {
            printk("mqtt_read_publish_payload: Failed! %d\n", err);
            printk("Disconnecting MQTT client...\n");

            err = mqtt_disconnect(c);
            if (err)
            {
                printk("Could not disconnect: %d\n", err);
            }
        }
    } break;

    case MQTT_EVT_PUBACK:
        if (evt->result != 0)
        {
            printk("MQTT PUBACK error %d\n", evt->result);
            break;
        }

        printk("[%s:%d] PUBACK packet id: %u\n", __func__, __LINE__,
                        evt->param.puback.message_id);
        nRun = 0;
        numPublished++;
        //printk("Messages published this session: %i\n", numPublished);
        break;

    case MQTT_EVT_SUBACK:
        if (evt->result != 0)
        {
                printk("MQTT SUBACK error %d\n", evt->result);
                break;
        }

        printk("[%s:%d] SUBACK packet id: %u\n", __func__, __LINE__,
                        evt->param.suback.message_id);
        break;

    default:
        printk("[%s:%d] default: %d\n", __func__, __LINE__,
                        evt->type);
        break;
    }
}

/**@brief Resolves the configured hostname and
 * initializes the MQTT broker structure
 */
static void broker_init(void)
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
            printk("ERROR: getaddrinfo failed %d\n", err);

            return;
    }

    addr = result;
    err = -ENOENT;

    /* Look for address of the broker. */
    while (addr != NULL)
    {
        /* IPv4 Address. */
        if (addr->ai_addrlen == sizeof(struct sockaddr_in))
        {
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
            //printk("IPv4 Address found %s\n", ipv4_addr);

            break;
        }
        else
        {
            printk("ai_addrlen = %u should be %u or %u\n",
                    (unsigned int)addr->ai_addrlen,
                    (unsigned int)sizeof(struct sockaddr_in),
                    (unsigned int)sizeof(struct sockaddr_in6));
        }

        addr = addr->ai_next;
        break;
    }

    /* Free the address. */
    freeaddrinfo(result);
}

/**@brief Initialize the MQTT client structure
 */
static void client_init(struct mqtt_client *client)
{
    //printk("mqtt_client_init(client)\r\n");
    mqtt_client_init(client);

    //printk("broker_init()\r\n");
    broker_init();
    //printk("broker_init() finished\r\n");

    /* MQTT client configuration */
    static struct mqtt_utf8 password;
    static struct mqtt_utf8 user_name;

    password.utf8 = (u8_t*)CONFIG_MQTT_BROKER_PASSWORD;
    password.size = strlen(CONFIG_MQTT_BROKER_PASSWORD);
    user_name.utf8 = (u8_t*)CONFIG_MQTT_BROKER_USERNAME;
    user_name.size = strlen(CONFIG_MQTT_BROKER_USERNAME);

    client->broker = &broker;
    client->evt_cb = mqtt_evt_handler;
    client->client_id.utf8 = (u8_t *)CONFIG_MQTT_CLIENT_ID;
    client->client_id.size = strlen(CONFIG_MQTT_CLIENT_ID);
    client->password = &password;
    client->user_name = &user_name;
    client->protocol_version = MQTT_VERSION_3_1_1;

    /* MQTT buffers configuration */
    client->rx_buf = rx_buffer;
    client->rx_buf_size = sizeof(rx_buffer);
    client->tx_buf = tx_buffer;
    client->tx_buf_size = sizeof(tx_buffer);

#if defined(CONFIG_MQTT_LIB_TLS)
    struct mqtt_sec_config *tls_config = &client->transport.tls.config;
    
    client->transport.type = MQTT_TRANSPORT_SECURE;
    
    //tls_config->peer_verify = CONFIG_PEER_VERIFY;
    tls_config->peer_verify = 1;
    tls_config->cipher_count = 0;
    tls_config->cipher_list = NULL;
    tls_config->sec_tag_count = ARRAY_SIZE(sec_tag_list);
    tls_config->sec_tag_list = sec_tag_list;
    tls_config->hostname = CONFIG_MQTT_BROKER_HOSTNAME;
#else /* MQTT transport configuration */
    client->transport.type = MQTT_TRANSPORT_NON_SECURE;
#endif /* defined(CONFIG_MQTT_LIB_TLS) */
}

/**@brief Initialize the file descriptor structure used by poll.
 */
static int fds_init(struct mqtt_client *c)
{
    if (c->transport.type == MQTT_TRANSPORT_NON_SECURE)
    {
        fds.fd = c->transport.tcp.sock;
    }
    else
    {
#if defined(CONFIG_MQTT_LIB_TLS)
        fds.fd = c->transport.tls.sock;
#else
        return -ENOTSUP;
#endif
    }

    fds.events = POLLIN;

    return 0;
}

/**@brief Configures modem to provide LTE link. Blocks until link is
 * successfully established.
 */
static void modem_configure(void)
{
#if defined(CONFIG_LTE_LINK_CONTROL)
    if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT))
    {
        /* Do nothing, modem is already turned on
         * and connected.
         */
    }
    else
    {
#if defined(CONFIG_LWM2M_CARRIER)
        /* Wait for the LWM2M_CARRIER to configure the modem and
         * start the connection.
         */
        printk("Waitng for carrier registration...\n");
        k_sem_take(&carrier_registered, K_FOREVER);
        printk("Registered!\n");
#else /* defined(CONFIG_LWM2M_CARRIER) */
        int err;

        //printk("LTE Link Connecting ...\n");
        err = lte_lc_init_and_connect();
        __ASSERT(err == 0, "LTE link could not be established.");
        //printk("LTE Link Connected!\n");
#endif /* defined(CONFIG_LWM2M_CARRIER) */
    }
#endif /* defined(CONFIG_LTE_LINK_CONTROL) */
}

static void certificate_init(void) // todo - probably shouldn't rewrite the credentials EVERY time (at least, not every time an mqtt connection fails
{
    int err;
    enum lte_lc_func_mode system_mode_now;
    nrf_sec_tag_t sec_tag = (nrf_sec_tag_t) sec_tag_list[0];
    bool cred_exists = false;
    u8_t perm_flags_tmp;
    //printk("\n Add cert \n");

    // Get current system mode
    err = lte_lc_func_mode_get(&system_mode_now);
    if (err < 0)
    {
        printk("Error getting system mode");
    }

    // Turn modem offline if it isn't already
    if (system_mode_now != LTE_LC_FUNC_MODE_OFFLINE)
    {
        //printk("Modem is not offline - will make it offline\n");
        err = lte_lc_offline();
        __ASSERT(err == 0, "Could not turn modem offline.");
    }

    // Check if credential exists already - if so, delete
    err = modem_key_mgmt_exists(sec_tag, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN, &cred_exists, &perm_flags_tmp);
    __ASSERT(err == 0, "Could not check if key exists.");
    if (cred_exists)
    {
        //printk("Cred exists\n");
        err = modem_key_mgmt_delete(sec_tag, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN);
        if (err < 0)
        {
            printk("ERROR: key delete %d\n", errno);
        }
    }

    // Write credential
    err = modem_key_mgmt_write(sec_tag, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN, certificates, strlen(certificates));
    //printk("strlen(certificates): %i \n", strlen(certificates));
    if (err < 0)
    {
        printk("ERROR: key write %d\n", err);
    }

  }


static void uart_cb(struct device *uart)
{
    uint8_t temp_rx[UART_BUF_SIZE];
    int data_length=0;
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

    if( uart_rx_leng == UART_BUF_SIZE ) // Buffer filled up, something went wrong
    {
        // discard and start over
        memset(uart_rxbuf, '\0', sizeof(uart_rxbuf));
        uart_rx_leng = 0; 

    }
    else if( uart_rx_leng > 35 ) // assume packets shorter than this are garbage or incomplete
    {
        if( uart_rxbuf[uart_rx_leng-1] == 0x0a &&
            uart_rxbuf[uart_rx_leng-2] == 0x0d ) // message terminated with CR and LF
        {
            if( uart_rxbuf[0]              == 0x7b &&
                uart_rxbuf[uart_rx_leng-3] == 0x7d )  // "{" and "}" character are first and last
            {
                if (unsent_data == false)
                {
                    memset(data_uart, '\0', sizeof(data_uart)); // clear the UART data string
                    strncpy(data_uart, uart_rxbuf, uart_rx_leng - 2); // copy in the new UART data
                    unsent_data = true; // set flag for new data ready to be sent to cloud

                    #ifndef APP_USE_TIMERS_FOR_WORKQUEUE
                        k_work_submit_to_queue(&queue_work_msg_send, &work_msg_send); // submit to work queue
                    #endif

                    memset(uart_rxbuf, '\0', sizeof(uart_rxbuf));
                    uart_rx_leng = 0; 
                }
            }
            else // some other message format, discard
            {
                memset(uart_rxbuf, '\0', sizeof(uart_rxbuf));
                uart_rx_leng = 0;
            }
        }
    }


//    if (uart_rx_leng > 0)
//    {
//        if ( (uart_rx_leng == UART_BUF_SIZE) || 
//             (uart_rxbuf[uart_rx_leng - 1] == 0x0a) || 
//             (uart_rxbuf[uart_rx_leng - 1] == 0x0d) || 
//             (uart_rxbuf[uart_rx_leng - 2] == 0x0d) )
//        {              
//            if (uart_rx_leng > 35 && uart_rx_leng < UART_BUF_SIZE)
//            {
//                //printk("C:%s\n",data_uart);
//                if (unsent_data == false)
//                {
//                    uint8_t pos_modifier = 0;
//                    if (uart_rxbuf[uart_rx_leng - 1] == 0x0a) pos_modifier++;
//                    if (uart_rxbuf[uart_rx_leng - 1] == 0x0d) pos_modifier++;
//                    if (uart_rxbuf[uart_rx_leng - 2] == 0x0d) pos_modifier++;
//                    memset(data_uart, '\0', sizeof(data_uart));
//                    strncpy(data_uart, uart_rxbuf, uart_rx_leng - pos_modifier);
//                    unsent_data = true;
//
//                    #ifndef APP_USE_TIMERS_FOR_WORKQUEUE
//                        k_work_submit_to_queue(&queue_work_msg_send, &work_msg_send);
//                    #endif
//                }
//            }
//            uart_rxbuf[0] = 0;
//            memset(uart_rxbuf, '\0', sizeof(uart_rxbuf));
//            uart_rx_leng = 0;     
//        }
//    }

    if (uart_irq_tx_ready(uart))
    {
        struct uart_data_t *buf =
                k_fifo_get(&fifo_uart_tx_data, K_NO_WAIT);
        u16_t written = 0;
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

static void sendCloudMsg(void)
{
    int err_dp = data_publish(&client, MQTT_QOS_1_AT_LEAST_ONCE, data_uart, strlen(data_uart));
    if (err_dp == 0)
    {
        memset(debug_print, '\0', sizeof(debug_print));
        sprintf(debug_print, "Published: %s", data_uart);
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
    k_sleep(K_SECONDS(6));

    printk("The MQTT simple sample started\n");

    uint8_t mqtt_conn_attempts = 0;
    uint8_t mqtt_conn_max_tries = 10;

    do
    {
        // wait 5 seconds before attempting to reconnect
        if (mqtt_conn_attempts > 0)
        {
          k_sleep(K_SECONDS(5));
        }
        certificate_init();

        modem_configure();

        client_init(&client);

        err = mqtt_connect(&client);
        if (err != 0)
        {
                printk("ERROR: mqtt_connect %d\n", err);
        }
        mqtt_conn_attempts++;
        //printk("MQTT_CONN_ATTEMPTS: %i\n", mqtt_conn_attempts);
    } while(err != 0 && mqtt_conn_attempts < mqtt_conn_max_tries);
    // if still not connected, end program

    if (err != 0)
    {
        #if !defined(CONFIG_DEBUG) && defined(CONFIG_REBOOT)
            sys_reboot(0);
        #endif
        
        return;
    }

    err = fds_init(&client);
    if (err != 0)
    {
        printk("ERROR: fds_init %d\n", err);
    
        #if !defined(CONFIG_DEBUG) && defined(CONFIG_REBOOT)
            sys_reboot(0);
        #endif
        return;
    }

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
    
    k_work_init(&work_msg_send, work_handler_msg_send);   // Initialize the work items that will be submitted
    k_work_q_start( &queue_work_msg_send, my_stack_area,        
                    K_THREAD_STACK_SIZEOF(my_stack_area), MY_PRIORITY); // Initialize & start the work queue that they are submitted to

    #ifdef APP_USE_TIMERS_FOR_WORKQUEUE
        k_timer_init(&timer_msg_send, timer_handler_msg_send, NULL);
        k_timer_start(&timer_msg_send, K_MSEC(TIMER_INTERVAL_MSEC), K_MSEC(TIMER_INTERVAL_MSEC));
    #endif

    int i=0;
//    strncpy ( data_uart_temp, "Temp", sizeof("Temp"));
    while (1)
    {

        k_sleep(K_SECONDS(0.001));

        if(i<300000)
        {
            ++i;
            //printk("Add and continue");
        }
        else
        {
            #if !defined(CONFIG_DEBUG) && defined(CONFIG_REBOOT)                      
                sys_reboot(0);
            #endif
        }

        err = poll(&fds, 1, mqtt_keepalive_time_left(&client));
        if (err < 0)
        {
            printk("ERROR: poll %d\n", errno); // err or errno?
            break;
        }

        err = mqtt_live(&client);
        if ((err != 0) && (err != -EAGAIN))
        {
            printk("ERROR: mqtt_live %d\n", err);
            break;
        }

        if ((fds.revents & POLLIN) == POLLIN)
        {
            err = mqtt_input(&client);
            if (err != 0)
            {
                printk("ERROR: mqtt_input %d\n", err);
                break;
            }
        }

        if ((fds.revents & POLLERR) == POLLERR)
        {
            printk("POLLERR\n");
            break;
        }

        if ((fds.revents & POLLNVAL) == POLLNVAL)
        {
            printk("POLLNVAL\n");
            break;
        }

        /*
        if(strlen(data_uart)>2)

        if (nRun == 0) {
        if (vChanged == true) {
        nRun=1;
        vChanged = false;
        sendCloudMsg();
        }
        }*/

//        if( strcmp(data_uart,"")) // UART not empty
//        {
//            if( strcmp(data_uart,data_uart_temp)) // UART has new data (not same as prev)
//            {
//                memset(data_uart_temp, '\0', sizeof(data_uart_temp));
//                strncpy ( data_uart_temp, data_uart, sizeof(data_uart));
//                sendCloudMsg();
//            }
//        }
          
    }

    printk("Disconnecting MQTT client...\n");

    err = mqtt_disconnect(&client);
    if (err)
    {
            printk("Could not disconnect MQTT client. Error: %d\n", err);
    }
    #if !defined(CONFIG_DEBUG) && defined(CONFIG_REBOOT)                      
         sys_reboot(0);
    #endif
}
