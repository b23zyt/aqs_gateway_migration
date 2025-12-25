# 1.Header Files Migration
Many header files need to have the prefix /zephy
Here are some examples

1. `#include <zephyr.h>` → `#include <zephyr/kernel.h>`
   - https://github.com/nrfconnect/sdk-nrf/blob/main/samples/net/azure_iot_hub/src/main.c

2. `#include <drivers/uart.h>` → `#include <zephyr/drivers/uart.h>`
   - https://github.com/zephyrproject-rtos/zephyr/blob/main/include/zephyr/modem/backend/uart.h

3. `#include <random/rand32.h>` → `#include <zephyr/random/random.h>`
   - https://docs.zephyrproject.org/latest/doxygen/html/random_8h.html

4. `#include <net/mqtt.h>` → `#include <zephyr/net/mqtt.h>`
   - https://docs.zephyrproject.org/latest/doxygen/html/mqtt_8h.html

5. `#include <net/socket.h>` → `#include <zephyr/net/socket.h>`

6. `#include <modem/at_cmd.h>` → `#include <nrf_modem_at.h>`
   - **Note**: AT commands need to be updated accordingly
   - Old code (line 504):
     ```c
     char imei_buf[CGSN_RESPONSE_LENGTH + 1];
     err = at_cmd_write("AT+CGSN", imei_buf, sizeof(imei_buf), NULL);
     ```
   - New code:
     ```c
     char imei_buf[CGSN_RESPONSE_LENGTH + 1];
     err = nrf_modem_at_cmd(imei_buf, sizeof(imei_buf), "AT+CGSN");
     ```

7. `#include <logging/log.h>` → `#include <zephyr/logging/log.h>`
   - http://docs.nordicsemi.com/bundle/zephyr-apis-3.2.1/page/log_8h.html

8. `#include <drivers/gpio.h>` → `#include <zephyr/drivers/gpio.h>`
   - https://docs.nordicsemi.com/bundle/zephyr-apis-3.1.1/page/drivers_2gpio_8h.html

# 2.Device Tree and GPIO Initialization

## Reference
- GPIO Interface: https://docs.zephyrproject.org/latest/doxygen/html/group__gpio__interface.html
- gpio_dt_spec Structure: https://docs.zephyrproject.org/latest/doxygen/html/structgpio__dt__spec.html

1. **LED Device Initialization (Line 44)**
   - Old code: `static struct device *led_dev;`
   - New code: `static const struct device *led_dev;`

2. **GPIO Pin Information Extraction (Lines 48-74)**
   - Current approach is to obtains label, pin, and flags separately
   - New approach is to use `gpio_dt_spec` with `GPIO_DT_SPEC_GET()` function
   
   **New Code:**
   ```c
   #define RED_LED_NODE DT_ALIAS(led0)
   
   #if DT_NODE_HAS_STATUS(RED_LED_NODE, okay)
   static const struct gpio_dt_spec red_led = GPIO_DT_SPEC_GET(RED_LED_NODE, gpios);
   #else
   #error "Unsupported board: led0 devicetree alias is not defined"
   #endif
   
   #define BLU_LED_NODE DT_ALIAS(led1)
   
   #if DT_NODE_HAS_STATUS(BLU_LED_NODE, okay)
   static const struct gpio_dt_spec blu_led = GPIO_DT_SPEC_GET(BLU_LED_NODE, gpios);
   #else
   #error "Unsupported board: led1 devicetree alias is not defined"
   #endif
   ```

3. **GPIO device binding (Lines 974–977)**

   - `device_get_binding()` is not required when using `gpio_dt_spec`
  
   **New Code:**
   ```c
   if (!gpio_is_ready_dt(&red_led)) {
       LOG_ERR("Red LED device not ready");
       return;
   }
   ```

4. **New GPIO API calls (Many places)**
    **gpio_pin_configure**
    - Old Code:
    ```c
    err = gpio_pin_configure(led_dev, RED_LED_PIN, GPIO_OUTPUT_INACTIVE | RED_LED_FLAGS);
    ```

    - New code:
    ```c
    err = gpio_pin_configure_dt(&red_led, GPIO_OUTPUT_INACTIVE);
    ```

    **gpio_pin_set**
    - Old Code
    ```c 
    gpio_pin_set(led_dev, BLU_LED_PIN, (int)led_state);
    ```
    - New Code
    ```c
    gpio_pin_set_dt(&blu_led, (int)led_state);
    ```
5. **UART device binding (Line 881-884)**
     - Reference: https://docs.zephyrproject.org/latest/build/dts/howtos.html
     - Update to
     ```c
     #define UART_DEVICE_NODE DT_CHOSEN(zephyr_console)

    static int init_uart(void)
    {
        dev_uart = DEVICE_DT_GET(UART_DEVICE_NODE);
        if (!device_is_ready(dev_uart))
        {
            LOG_ERR("UART device not ready");
            return -ENXIO;
        }
    }
     ```
6. **blink_led_entry_point function update**
     - The new code is based on the new APIs above
     ```c
    extern void blink_led_entry_point(void)
    {
        bool led_state = false;

        while( lte_connecting )
        {
            gpio_pin_set_dt(&blu_led, (int)led_state);  // new API
            led_state = !led_state;
            k_sleep(K_SECONDS(1));
        }

        while( mqtt_connecting )
        {
            gpio_pin_set_dt(&blu_led, (int)led_state);  // new API
            led_state = !led_state;
            k_sleep(K_MSEC(500)); 
        }

        // leave in solid ON state
        gpio_pin_set_dt(&blu_led, 0);  // newAPI
        return;
    }
     ```

# 3.modem_configure() Function Update to Asynchronous Connection (Line 633)
1. **Disable PSM and eDRX (Line 642, 643)**
     - New Code
     ```c
    int err = lte_lc_psm_param_set(NULL);
    err = lte_lc_edrx_param_set(LTE_LC_LTE_MODE_NONE, NULL)
     ```
    
2. **LTE Connection (Line 660 - 672)**
     - Reference: https://github.com/nrfconnect/sdk-nrf/blob/main/samples/net/azure_iot_hub/src/main.c (Line 578)
     - New Code
    ```c
    LOG_INF("Bringing network interface up and connecting to the network");

    err = conn_mgr_all_if_up(true);
    if (err) {
        LOG_ERR("conn_mgr_all_if_up failed: %d", err);
        return err;
    }

    err = conn_mgr_all_if_connect(true);
    if (err) {
        LOG_ERR("conn_mgr_all_if_connect failed: %d", err);
        return err;
    }
    ```
3. **New AT Commands**
    - at_cmd_write( ) → nrf_modem_at_cmd( ) (mentioned in Part1)
    - Remove at_cmd_init( ) (Line 497)

4. **Overall Workflow**
    - Old workflow: modem_configure() → lte_lc_init_and_connect() → if (err == -EALREADY) lte_lc_connect() → Block until successful connected
    - New workflow: conn_mgr_all_if_up(true) → conn_mgr_all_if_connect(true) → k_sem_take(&network_connected_sem, K_FOREVER)

5. **Summary**
     - New modem_configure function
    ```c
    static int modem_configure(void)
    {
    #if defined(CONFIG_LTE_LINK_CONTROL)
        int err;

        LOG_INF("Configuring LTE link");

        /* Disable power saving features for responsive demo */
        err = lte_lc_psm_param_set(NULL);
        if (err && err != -EOPNOTSUPP) {
            LOG_WRN("Failed to disable PSM: %d", err);
        }

        err = lte_lc_edrx_param_set(LTE_LC_LTE_MODE_NONE, NULL);
        if (err && err != -EOPNOTSUPP) {
            LOG_WRN("Failed to disable eDRX: %d", err);
        }

        /* Use Connection Manager for network management */
        LOG_INF("Bringing network interface up");
        err = conn_mgr_all_if_up(true);
        if (err) {
            LOG_ERR("conn_mgr_all_if_up failed: %d", err);
            return err;
        }

        LOG_INF("Connecting to network");
        err = conn_mgr_all_if_connect(true);
        if (err) {
            LOG_ERR("conn_mgr_all_if_connect failed: %d", err);
            return err;
        }

        LOG_INF("Network configuration complete");
    #endif /* defined(CONFIG_LTE_LINK_CONTROL) */

        return 0;
    }
    ```
    - Add callback function (refer to the Azure example)
    ```c
    static K_SEM_DEFINE(network_connected_sem, 0, 1);

    static void l4_event_handler(struct net_mgmt_event_callback *cb,
                                uint64_t event,
                                struct net_if *iface)
    {
        switch (event) {
        case NET_EVENT_L4_CONNECTED:
            LOG_INF("Network connectivity established");
            k_sem_give(&network_connected_sem);
            break;
        case NET_EVENT_L4_DISCONNECTED:
            LOG_INF("Network connectivity lost");
            break;
        default:
            return;
        }
    }

    static struct net_mgmt_event_callback l4_cb;
    net_mgmt_init_event_callback(&l4_cb, l4_event_handler,
                                NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED);
    net_mgmt_add_event_callback(&l4_cb);

    err = modem_configure();
    if (err) {
        LOG_ERR("Modem configuration failed: %d", err);
        return;
    }

    //wait for connection
    LOG_INF("Waiting for network connection");
    k_sem_take(&network_connected_sem, K_FOREVER);
    LOG_INF("Connected to network");
    ```

# 4.client_init() Function Update
1. **MQTT version update (Line 553)**
    - The newest mqtt version is mqtt5.0, the old code uses mqtt3.1.1
    - New Code
    ```c
    client->protocol_version = MQTT_VERSION_5_0;
    ```
2. Azure IoT Hub Configuration
    - Reference: https://docs.nordicsemi.com/bundle/ncs-2.9.1/page/nrf/libraries/networking/azure_iot_hub.html
    - Old Code
    ```c
    static const char* CONFIG_MQTT_BROKER_PASSWORD = "SharedAccessSignature sr=AquaHub2.azure-devices.net%2Fdevices%2Faqsdevice27&sig=9tycPD9ocM%2BVb5B%2F18jzVQ8%2B3%2FIO0Dy185P6A46UBSk%3D&se=2500389785";
    static const char* CONFIG_MQTT_BROKER_USERNAME = "AquaHub2.azure-devices.net/aqsdevice27/?api-version=2018-06-30";
    ```
    - New Code
    ```c
    #include <net/azure_iot_hub.h>
    struct azure_iot_hub_config cfg = {
        .device_id = {
            .ptr = device_id,
            .size = strlen(device_id),
        },
        .hostname = {
            .ptr = "AquaHub2.azure-devices.net",
            .size = strlen("AquaHub2.azure-devices.net"),
        },
        .use_dps = true,
    };

    err = azure_iot_hub_connect(&cfg);
    if (err < 0) {
		LOG_ERR("azure_iot_hub_connect failed: %d", err);
		return 0;
	}
    ```

## 5. MQTT connection and reconnection Improvements
1. **MQTT reconnection attempts**
    - Old Code (Line 1064 - 1083) uses blocking reconnection
    - It should be changed to an event-driven reconnection mechanism
    - New Code:
    ```c
    enum mqtt_state {
        MQTT_STATE_DISCONNECTED,
        MQTT_STATE_CONNECTING,
        MQTT_STATE_CONNECTED,
        MQTT_STATE_ERROR
    };

    static enum mqtt_state mqtt_state = MQTT_STATE_DISCONNECTED;
    static uint8_t mqtt_connect_attempt = 0;
    static struct k_work_delayable mqtt_connect_work;

    static void mqtt_connect_work_fn(struct k_work *work)
    {
        int err;
        
        if (mqtt_state == MQTT_STATE_CONNECTED) {
            return;
        }
        
        mqtt_state = MQTT_STATE_CONNECTING;
        LOG_INF("MQTT connection attempt %d", mqtt_connect_attempt + 1);
        
        err = mqtt_connect(&client);
        if (err == 0) {
            LOG_INF("MQTT connect initiated successfully");
            mqtt_connect_attempt = 0;
        } else {
            LOG_ERR("mqtt_connect failed: %d", err);
            mqtt_state = MQTT_STATE_DISCONNECTED;
            
            mqtt_connect_attempt++;

            uint32_t backoff_sec = MIN(30, (1 << mqtt_connect_attempt));
            
            if (mqtt_connect_attempt >= 10) {
                LOG_ERR("Too many MQTT connection failures, rebooting...");
                #if defined(CONFIG_REBOOT)
                sys_reboot(SYS_REBOOT_COLD);
                #endif
                return;
            }
            
            LOG_INF("Retrying in %d seconds...", backoff_sec);
            k_work_schedule(&mqtt_connect_work, K_SECONDS(backoff_sec));
        }
    }

    static void mqtt_connect_init(void)
    {
        k_work_init_delayable(&mqtt_connect_work, mqtt_connect_work_fn);
    }

    static void mqtt_connect_start(void)
    {
        mqtt_connect_attempt = 0;
        k_work_schedule(&mqtt_connect_work, K_NO_WAIT);
    }
    ```
2. **MQTT event handler function (Line 328)**
    - No reconnection mechanism when CONNACK failed or CONNACK
    - New Code: 
    ```c
    case MQTT_EVT_CONNACK:
        if (evt->result != 0) {
            LOG_ERR("MQTT connect failed: %d", evt->result);
            mqtt_state = MQTT_STATE_ERROR;
            
            switch (evt->result) {
            case MQTT_CONNECTION_ACCEPTED:
                break;
            case MQTT_NOT_AUTHORIZED:
                LOG_ERR("Authentication failed - check credentials");
                break;
            case MQTT_UNACCEPTABLE_PROTOCOL_VERSION:
                LOG_ERR("Protocol version not supported");
                break;
            case MQTT_IDENTIFIER_REJECTED:
                LOG_ERR("Client ID rejected");
                break;
            case MQTT_SERVER_UNAVAILABLE:
                LOG_ERR("Server unavailable");
                break;
            case MQTT_BAD_USER_NAME_OR_PASSWORD:
                LOG_ERR("Bad username or password");
                break;
            case MQTT_NOT_AUTHORIZED_5:
                LOG_ERR("Not authorized (MQTT 5.0)");
                break;
            default:
                LOG_ERR("Unknown connection error: %d", evt->result);
                break;
            }
            
            k_work_schedule(&mqtt_connect_work, K_SECONDS(5));
            break;
        }

        LOG_INF("MQTT client connected");
        mqtt_state = MQTT_STATE_CONNECTED;
        mqtt_connect_attempt = 0;
        
        #ifdef BLG840_M1
        mqtt_connecting = false;
        #endif

        err = subscribe();
        if (err) {
            LOG_ERR("Failed to subscribe: %d", err);
        }
        break;

    case MQTT_EVT_DISCONNECT:
        LOG_INF("MQTT client disconnected: %d", evt->result);
        mqtt_state = MQTT_STATE_DISCONNECTED;
        
        #ifdef BLG840_M1
        mqtt_connecting = true;
        #endif
        
        LOG_INF("Scheduling reconnection...");
        k_work_schedule(&mqtt_connect_work, K_SECONDS(CONFIG_MQTT_RECONNECT_DELAY_S));
        break;
    ```

3. **Main loop Line (From Line 1102)**
    - Any error will break out of the loop and restart directly
    - There are recovery errors and fatal errors and restart is not required in the first case.
    - New Code: doesn't exit for all types of error
    ```c
	while (1) {
        // check status
		if (mqtt_state != MQTT_STATE_CONNECTED) {
			k_sleep(K_SECONDS(1));
			continue;
		}

		int32_t timeout = mqtt_keepalive_time_left(&client);
		if (timeout < 0) {
			timeout = 0;
		}

		err = poll(&fds, 1, timeout);
		if (err < 0) {
			LOG_ERR("poll error: %d (errno: %d)", err, errno);
			
			/* Error types */
			if (errno == EINTR) {
				/* Interrupted by the signal, continue */
				continue;
			} else if (errno == EBADF || errno == EINVAL) {
				/* The file descriptor is invalid, reconnect */
				LOG_ERR("Invalid file descriptor, reconnecting...");
				mqtt_disconnect(&client);
				mqtt_state = MQTT_STATE_DISCONNECTED;
				k_work_schedule(&mqtt_connect_work, K_SECONDS(1));
				continue;
			} else {
				/* Other errors */
				k_sleep(K_MSEC(100));
				continue;
			}
		}

		/* keep mqtt connection */
		err = mqtt_live(&client);
		if (err != 0 && err != -EAGAIN) {
			LOG_ERR("mqtt_live error: %d", err);

			if (err == -ENOTCONN) {
				LOG_INF("Connection lost, reconnecting...");
				mqtt_state = MQTT_STATE_DISCONNECTED;
				k_work_schedule(&mqtt_connect_work, K_SECONDS(1));
				continue;
			}

			k_sleep(K_MSEC(100));
			continue;
		}

		if ((fds.revents & POLLIN) == POLLIN) {
			err = mqtt_input(&client);
			if (err != 0) {
				LOG_ERR("mqtt_input error: %d", err);
				
				if (err == -ENOTCONN || err == -ECONNRESET) {
					LOG_INF("Connection error, reconnecting...");
					mqtt_disconnect(&client);
					mqtt_state = MQTT_STATE_DISCONNECTED;
					k_work_schedule(&mqtt_connect_work, K_SECONDS(1));
					continue;
				}
			}
		}

		if ((fds.revents & POLLERR) == POLLERR) {
			LOG_ERR("POLLERR detected");
			mqtt_disconnect(&client);
			mqtt_state = MQTT_STATE_DISCONNECTED;
			k_work_schedule(&mqtt_connect_work, K_SECONDS(2));
			continue;
		}

		if ((fds.revents & POLLNVAL) == POLLNVAL) {
			LOG_ERR("POLLNVAL detected - invalid socket");
			mqtt_disconnect(&client);
			mqtt_state = MQTT_STATE_DISCONNECTED;
			k_work_schedule(&mqtt_connect_work, K_SECONDS(2));
			continue;
		}

		if ((fds.revents & POLLHUP) == POLLHUP) {
			LOG_ERR("POLLHUP detected - connection hung up");
			mqtt_disconnect(&client);
			mqtt_state = MQTT_STATE_DISCONNECTED;
			k_work_schedule(&mqtt_connect_work, K_SECONDS(2));
			continue;
		}
	}
	
	#if defined(CONFIG_REBOOT)
	sys_reboot(SYS_REBOOT_COLD);
	#endif
    ```

## 6. UART Improvements
1. **UART initialization**
     - Old Code (line 879 - 891)
    ```c
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
    ```
    - Should change to use device tree:
    - Reference: http://docs.zephyrproject.org/latest/build/dts/howtos.html 
    - New Code:
    ```c
    #define UART_NODE DT_CHOSEN(zephyr_console)

    static int init_uart(void)
    {
        int err;
        
        dev_uart = DEVICE_DT_GET(UART_NODE);
        if (!device_is_ready(dev_uart)) {
            LOG_ERR("UART device not ready");
            return -ENODEV;
        }
        
        LOG_INF("UART device: %s", dev_uart->name);
        
        err = uart_irq_callback_user_data_set(dev_uart, uart_cb, NULL);
        if (err) {
            LOG_ERR("Failed to set UART callback: %d", err);
            return err;
        }
        
        uart_irq_rx_enable(dev_uart);
        
        LOG_INF("UART initialized successfully");
        
        return 0;
    }
    ```
## 7. Work Queue
1. **Work queue initialization (Line 1093 - 1095)**
    - Based on the Azure iot hub github repository, it should update to 
    ```c
    static int init_msg_send_workqueue(void)
    {
        k_work_init(&work_msg_send, work_handler_msg_send);
        k_work_init_delayable(&work_msg_send_delayed, work_handler_msg_send_delayed);

        k_work_queue_start(&queue_work_msg_send,
                        msg_send_wq_stack,
                        K_THREAD_STACK_SIZEOF(msg_send_wq_stack),
                        MSG_SEND_WQ_PRIORITY,
                        NULL);
        
        k_thread_name_set(&queue_work_msg_send.thread, "msg_send_wq");
        
        LOG_INF("Message send workqueue initialized");
        return 0;
    }

    /* call it inside main */
    err = init_msg_send_workqueue();
    if (err) {
        LOG_ERR("Failed to init workqueue: %d", err);
        return err;
    }
    ```
## 8. prj.conf
1. **New prj.conf** 
    - Add a section for network connection manager
    - Remove the AT Host section
    - Add a section for UART
    - Update Azure IoT Hub Configuration using CONFIG_AZURE_IOT_HUB_DEVICE_ID and CONFIG_AZURE_IOT_HUB_HOSTNAME
    - New file
    ```
    # Networking
    CONFIG_NETWORKING=y
    CONFIG_NET_NATIVE=n
    CONFIG_NET_SOCKETS_OFFLOAD=y
    CONFIG_NET_SOCKETS=y
    CONFIG_NET_SOCKETS_POSIX_NAMES=y

    # New
    CONFIG_NET_CONNECTION_MANAGER=y
    CONFIG_NET_MGMT=y
    CONFIG_NET_MGMT_EVENT=y
    CONFIG_NET_MGMT_EVENT_INFO=y

    # LTE link control
    CONFIG_LTE_NETWORK_USE_FALLBACK=n
    CONFIG_LTE_LINK_CONTROL=y
    CONFIG_LTE_AUTO_INIT_AND_CONNECT=n
    CONFIG_LTE_NETWORK_MODE_LTE_M=y # new

    # CONFIG_LTE_LOCK_BANDS=y
    # CONFIG_LTE_LOCK_BAND_MASK="00010001100001001000"

    # Modem library
    CONFIG_NRF_MODEM_LIB=y

    # UART
    CONFIG_UART_INTERRUPT_DRIVEN=y
    CONFIG_SERIAL=y

    # MQTT
    CONFIG_MQTT_LIB=y
    CONFIG_MQTT_LIB_TLS=y
    CONFIG_MQTT_CLEAN_SESSION=y
    CONFIG_MQTT_KEEPALIVE=900
    CONFIG_MQTT_MESSAGE_BUFFER_SIZE=1024
    CONFIG_MQTT_PAYLOAD_BUFFER_SIZE=1024

    # Azure IoT Hub
    CONFIG_AZURE_IOT_HUB=y
    CONFIG_AZURE_IOT_HUB_DEVICE_ID="aqsdevice27"
    CONFIG_AZURE_IOT_HUB_HOSTNAME="AquaHub2.azure-devices.net"
    CONFIG_AZURE_IOT_HUB_DPS=n

    # Key
    CONFIG_MODEM_KEY_MGMT=y

    # Application - Misc
    CONFIG_REBOOT=y
    CONFIG_SERIAL=y
    CONFIG_GPIO=y

    # Enable Logging
    CONFIG_LOG=y
    CONFIG_MQTT_SIMPLE_LOG_LEVEL_DBG=y

    # Memory
    CONFIG_MAIN_STACK_SIZE=8192
    CONFIG_HEAP_MEM_POOL_SIZE=4096

    # NewLib C
    CONFIG_NEWLIB_LIBC=y
    ```