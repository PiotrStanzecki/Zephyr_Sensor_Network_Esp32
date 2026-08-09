// SPDX-FileCopyrightText: 2024 Cesanta Software Limited
// SPDX-License-Identifier: GPL-2.0-only or commercial

#include "mongoose_glue.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <arpa/inet.h>
#include <string.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/adc.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

#define AP_SSID "ESP32_Mongoose_AP"
#define AP_PSK  "12345678"  


/* LED Strip Setup */
static const struct device *strip = DEVICE_DT_GET(DT_ALIAS(led_strip));
#define STRIP_NUM_PIXELS DT_PROP(DT_ALIAS(led_strip), chain_length)
static struct led_rgb pixels[STRIP_NUM_PIXELS];

/* Sensors Setup */
#define TEMP_SENSOR_NODE DT_NODELABEL(temp_sens)
static const struct device *temp_sensor = DEVICE_DT_GET(TEMP_SENSOR_NODE);

#define HUMIDITY_SENSOR_NODE DT_NODELABEL(humidity_sens)
static const struct device *humidity_sensor = DEVICE_DT_GET(HUMIDITY_SENSOR_NODE);

#define LIGHT_SENSOR_NODE DT_NODELABEL(light_sens)
static const struct adc_dt_spec light_sensor = ADC_DT_SPEC_GET(LIGHT_SENSOR_NODE);



/* Message queues for sensors */
typedef struct {
  struct sensor_value temp;
} temp_msg_t;

typedef struct {
  struct sensor_value humidity;
  struct sensor_value temperature;
} humidity_msg_t;

typedef struct {
  uint16_t raw_value;  
  uint32_t mv_value;   
} light_msg_t;


K_MSGQ_DEFINE(temp_msgq, sizeof(temp_msg_t), 10, 4);
K_MSGQ_DEFINE(humidity_msgq, sizeof(humidity_msg_t), 10, 4);
K_MSGQ_DEFINE(light_msgq, sizeof(light_msg_t), 10, 4);


/* Sensor read thread */
void sensor_read_thread(void *arg1, void *arg2, void *arg3) {


  int ret;

  uint16_t adc_buff;

  struct adc_sequence seq = {
    .buffer = &adc_buff,
    .buffer_size = sizeof(adc_buff),
    .resolution = 12,
  };

  ret = adc_sequence_init_dt(&light_sensor, &seq);

  if(!device_is_ready(temp_sensor)) {
    LOG_ERR("Temperature sensor device not ready!");
  }

  if(!device_is_ready(humidity_sensor)) {
    LOG_ERR("Humidity sensor device not ready!");
  }

  if(!device_is_ready(light_sensor.dev)) {
    LOG_ERR("Light sensor device not ready!");
  }

  ret = adc_channel_setup_dt(&light_sensor);
  if (ret < 0) {
    LOG_ERR("Failed to setup light sensor ADC channel (err %d)", ret);
  }
  


  while(1){

    if(sensor_sample_fetch(temp_sensor) == 0){
      struct sensor_value temp_val;
      sensor_channel_get(temp_sensor, SENSOR_CHAN_AMBIENT_TEMP, &temp_val);
      temp_msg_t temp_msg = { .temp = temp_val };
      k_msgq_put(&temp_msgq, &temp_msg, K_NO_WAIT);
    }

    if(sensor_sample_fetch(humidity_sensor) == 0){
      struct sensor_value humidity_val;
      struct sensor_value temp_val; 
      sensor_channel_get(humidity_sensor, SENSOR_CHAN_HUMIDITY, &humidity_val);
      sensor_channel_get(humidity_sensor, SENSOR_CHAN_AMBIENT_TEMP, &temp_val); 
      humidity_msg_t humidity_msg = { .humidity = humidity_val, .temperature = temp_val }; 
      k_msgq_put(&humidity_msgq, &humidity_msg, K_NO_WAIT);
    }


    ret = adc_read_dt(&light_sensor, &seq);
    if (ret == 0) {
      
      int32_t mv_value = adc_buff; 
      adc_raw_to_millivolts_dt(&light_sensor, &mv_value);
      
      light_msg_t light_msg = { .raw_value = adc_buff, .mv_value = (uint32_t)mv_value };
      k_msgq_put(&light_msgq, &light_msg, K_NO_WAIT);
    } else {
      LOG_ERR("Failed to read light sensor ADC (err %d)", ret);
    }

    k_sleep(K_MSEC(300)); 
  }
}



K_THREAD_DEFINE(sensor_thread_id, 2048, sensor_read_thread, NULL, NULL, NULL, 7, 0, 0);







K_SEM_DEFINE(run, 0, 1);


static void zeh(struct net_mgmt_event_callback *cb,
#if ZEPHYR_VERSION_CODE < 0x40200
                uint32_t mgmt_event,
#else
                uint64_t mgmt_event,
#endif
                struct net_if *iface) {
  if (mgmt_event == NET_EVENT_WIFI_AP_ENABLE_RESULT) {
    k_sem_give(&run);
  }
}








int main(void) {

  k_sleep(K_SECONDS(2));

  LOG_INF("Starting ESP32-S3 SoftAP and Mongoose Server...");


  if (!device_is_ready(strip)) {
    LOG_ERR("LED strip device not ready! Check your app.overlay.");
    return -1;
  }
  LOG_INF("RGB LED Strip ready.");


  struct net_mgmt_event_callback ncb;
  struct net_if *iface = net_if_get_default();

  if (!iface) {
    LOG_ERR("No default network interface found!");
    return -1;
  }


  net_if_up(iface);

  
  struct in_addr gw;
  inet_pton(AF_INET, "192.168.4.1", &gw);
  net_if_ipv4_addr_add(iface, &gw, NET_ADDR_MANUAL, 0);

  
  net_mgmt_init_event_callback(&ncb, zeh, NET_EVENT_WIFI_AP_ENABLE_RESULT);
  net_mgmt_add_event_callback(&ncb);

  
  struct wifi_connect_req_params ap_config = {
      .ssid = (const uint8_t *)AP_SSID,
      .ssid_length = strlen(AP_SSID),
      .psk = (const uint8_t *)AP_PSK,
      .psk_length = strlen(AP_PSK),
      .channel = 1,
      .security = strlen(AP_PSK) > 0 ? WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE,
  };

  LOG_INF("Starting Wi-Fi Access Point: %s", AP_SSID);
  int ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, &ap_config, sizeof(ap_config));
  if (ret != 0) {
    LOG_ERR("Failed to enable Wi-Fi AP mode! Error code: %d", ret);
    return -1;
  }

  
  k_sem_take(&run, K_FOREVER);
  LOG_INF("Wi-Fi Access Point active.");


  struct in_addr dhcp_pool_start;
  inet_pton(AF_INET, "192.168.4.10", &dhcp_pool_start);
  if (net_dhcpv4_server_start(iface, &dhcp_pool_start) < 0) {
    LOG_WRN("Failed to start DHCPv4 server");
  } else {
    LOG_INF("DHCPv4 server started (pool starts at 192.168.4.10)");
  }

  LOG_INF("Server running at: http://192.168.4.1");


  struct leds led_data = {0}; 
  struct sensors sensors_data = {0};

  
  // State tracking for the LED
  bool led_was_on = false;
  uint8_t last_r = 0, last_g = 0, last_b = 0;


  mongoose_init();

  mongoose_add_ws_reporter(300, "sensors");  
 

  temp_msg_t temp_msg;
  humidity_msg_t humidity_msg;
  light_msg_t light_msg;
  bool sensors_updated = false;

  






  while (1) {
  
    mongoose_poll();

 
    glue_get_leds(&led_data);
  
    glue_get_sensors(&sensors_data);

    
    if (k_msgq_get(&temp_msgq, &temp_msg, K_NO_WAIT) == 0) {
      sensors_data.Temp = sensor_value_to_double(&temp_msg.temp);  
      sensors_updated = true;
    }

    if (k_msgq_get(&humidity_msgq, &humidity_msg, K_NO_WAIT) == 0) {
      sensors_data.Humidity = sensor_value_to_double(&humidity_msg.humidity);  
      sensors_updated = true;
    }

    if (k_msgq_get(&light_msgq, &light_msg, K_NO_WAIT) == 0) {
      sensors_data.Light_lvl = (light_msg.raw_value / 3300.0) * 100;  
      sensors_updated = true;
    }
    

    

    if (led_data.led1_on) {
      if (!led_was_on || last_r != led_data.red_lvl || last_g != led_data.green_lvl || last_b != led_data.blue_lvl) {
        
        memset(&pixels, 0, sizeof(pixels)); 
        

        pixels[0].r = (uint8_t)led_data.red_lvl;
        pixels[0].g = (uint8_t)led_data.green_lvl;
        pixels[0].b = (uint8_t)led_data.blue_lvl;
        
        led_strip_update_rgb(strip, pixels, STRIP_NUM_PIXELS);
        

        led_was_on = true;
        last_r = led_data.red_lvl;
        last_g = led_data.green_lvl;
        last_b = led_data.blue_lvl;

      }
    } else if (led_was_on) {
      memset(&pixels, 0, sizeof(pixels));
      led_strip_update_rgb(strip, pixels, STRIP_NUM_PIXELS);
      led_was_on = false;
    }




    glue_set_leds(&led_data);
    
    if (sensors_updated) {
      glue_set_sensors(&sensors_data);
      glue_update_state();  
      sensors_updated = false;
    }

    
  
  }

  return 0;
}