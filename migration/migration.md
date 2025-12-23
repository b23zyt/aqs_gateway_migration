# Header Files Migration
many header files need to have the prefix /zephyr
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

# Device Tree and GPIO Initialization

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
    extern void blink_led_entry_point(void)  // 添加void参数
    {
        bool led_state = false;

        while( lte_connecting )
        {
            gpio_pin_set_dt(&blu_led, (int)led_state);  // 使用新API
            led_state = !led_state;
            k_sleep(K_SECONDS(1));
        }

        while( mqtt_connecting )
        {
            gpio_pin_set_dt(&blu_led, (int)led_state);  // 使用新API
            led_state = !led_state;
            k_sleep(K_MSEC(500));  // K_SECONDS(0.5)改为K_MSEC(500)更清晰
        }

        // leave in solid ON state
        gpio_pin_set_dt(&blu_led, 0);  // 使用新API
        return;
    }
     ```

# LTE and Modem