/*
 * Copyright (c) 2014, Texas Instruments Incorporated - http://www.ti.com/
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/*---------------------------------------------------------------------------*/
#include "contiki-conf.h"
#include "rpl/rpl-private.h"
#include "mqtt.h"				//Biblioteca para utilizar MQTT
#include "net/rpl/rpl.h"
#include "net/ip/uip.h"
#include "net/ipv6/uip-icmp6.h"
#include "net/ipv6/sicslowpan.h"
#include "sys/etimer.h"
#include "sys/ctimer.h"
#include "lib/sensors.h"
#include "dev/leds.h"
#include "contiki.h"
#include "dev/serial-line.h"		     //Biblioteca para interface serial

#include <stdio.h>		
#include <string.h>

#include "dev/cc26xx-uart.h"	     //Bibliotecas para utilizar UART no cc2650
#include "net-uart.h"			
#include "ti-lib.h"			

//#define CC26XX_UART_CONF_BAUD_RATE	115200 //Definição do baud rate do UART0
/*---------------------------------------------------------------------------*/
/*
 * MQTT broker address
 */
static const char *broker_ip = MQTT_DEMO_BROKER_IP_ADDR;
/*---------------------------------------------------------------------------*/
/*
 * A timeout used when waiting for something to happen (e.g. to connect or to
 * disconnect)
 */
#define STATE_MACHINE_PERIODIC     (CLOCK_SECOND >> 1)
/*---------------------------------------------------------------------------*/
/* Provide visible feedback via LEDS during vacdrious states */
/* When connecting to broker */
#define CONNECTING_LED_DURATION    (CLOCK_SECOND >> 2)

/* Each time we try to publish */
#define PUBLISH_LED_ON_DURATION    (CLOCK_SECOND)
/*---------------------------------------------------------------------------*/
/* Connections and reconnections */
#define RETRY_FOREVER              0xFF
#define RECONNECT_INTERVAL         (CLOCK_SECOND * 2)

/*
 * Number of times to try reconnecting to the broker.
 * Can be a limited number (e.g. 3, 10 etc) or can be set to RETRY_FOREVER
 */
#define RECONNECT_ATTEMPTS         RETRY_FOREVER
#define CONNECTION_STABLE_TIME     (CLOCK_SECOND * 5)
/*---------------------------------------------------------------------------*/
static struct timer connection_life;
static uint8_t connect_attempt;
/*---------------------------------------------------------------------------*/
/* Various states */
static uint8_t state;
static uint8_t is_it_ready = 0;

#define STATE_INIT                    0
#define STATE_REGISTERED              1
#define STATE_CONNECTING              2
#define STATE_CONNECTED               3
#define STATE_PUBLISHING              4
#define STATE_DISCONNECTED            5
#define STATE_NEWCONFIG               6
#define STATE_CONFIG_ERROR         0xFE
#define STATE_ERROR                0xFF
/*---------------------------------------------------------------------------*/
#define CONFIG_EVENT_TYPE_ID_LEN     32
#define CONFIG_CMD_TYPE_LEN          8
#define CONFIG_IP_ADDR_STR_LEN       64
/*---------------------------------------------------------------------------*/
/* A timeout used when waiting to connect to a network */
#define NET_CONNECT_PERIODIC        (CLOCK_SECOND >> 2)
#define NO_NET_LED_DURATION         (NET_CONNECT_PERIODIC >> 1)
/*---------------------------------------------------------------------------*/
//PROCESS_NAME(mqtt_demo_process);
PROCESS_NAME(test_serial);
AUTOSTART_PROCESSES(&test_serial);
//AUTOSTART_PROCESSES(&mqtt_demo_process);
/*---------------------------------------------------------------------------*/
/**
 * \brief Data structure declaration for the MQTT client configuration
 */
typedef struct mqtt_client_config {
  char event_type_id[CONFIG_EVENT_TYPE_ID_LEN];
  char broker_ip[CONFIG_IP_ADDR_STR_LEN];
  char cmd_type[CONFIG_CMD_TYPE_LEN];
  clock_time_t pub_interval;
  uint16_t broker_port;
} mqtt_client_config_t;
/*---------------------------------------------------------------------------*/

/*
 * Buffers for Client ID and Topic.
 * Make sure they are large enough to hold the entire respective string
 */
static char client_id[BUFFER_SIZE];
static char pub_topic[BUFFER_SIZE];
static char sub_topic[BUFFER_SIZE];
static char username[BUFFER_SIZE];
static char password[BUFFER_SIZE];
/*---------------------------------------------------------------------------*/
/*
 * The main MQTT buffers.
 * We will need to increase if we start publishing more data.
 */
static struct mqtt_connection conn;
static char app_buffer[APP_BUFFER_SIZE];	//This is the payload char array
/*---------------------------------------------------------------------------*/
static struct mqtt_message *msg_ptr = 0;
static struct etimer publish_periodic_timer;
static struct ctimer ct;
static char *buf_ptr;
static uint16_t seq_nr_value = 0;
/*---------------------------------------------------------------------------*/
static mqtt_client_config_t conf;
/*---------------------------------------------------------------------------*/
//PROCESS(mqtt_demo_process, "MQTT Demo");
PROCESS(test_serial, "Test Serial");

/*---------------------------------------------------------------------------*/
int
ipaddr_sprintf(char *buf, uint8_t buf_len, const uip_ipaddr_t *addr)
{
  uint16_t a;
  uint8_t len = 0;
  int i, f;
  for(i = 0, f = 0; i < sizeof(uip_ipaddr_t); i += 2) {
    a = (addr->u8[i] << 8) + addr->u8[i + 1];
    if(a == 0 && f >= 0) {
      if(f++ == 0) {
        len += snprintf(&buf[len], buf_len - len, "::");
      }
    } else {
      if(f > 0) {
        f = -1;
      } else if(i > 0) {
        len += snprintf(&buf[len], buf_len - len, ":");
      }
      len += snprintf(&buf[len], buf_len - len, "%x", a);
    }
  }

  return len;
}
/*---------------------------------------------------------------------------*/
static void
publish_led_off(void *d)
{
  leds_off(LEDS_GREEN);
}
/*---------------------------------------------------------------------------*/
static void
pub_handler(const char *topic, uint16_t topic_len, const uint8_t *chunk,
            uint16_t chunk_len)
{
  printf("%s", chunk);

}

/*---------------------------------------------------------------------------*/
static void
mqtt_event(struct mqtt_connection *m, mqtt_event_t event, void *data)
{
  switch(event) {
  case MQTT_EVENT_CONNECTED: {
    printf("APP - Application has a MQTT connection\n");
    timer_set(&connection_life, CONNECTION_STABLE_TIME);
    state = STATE_CONNECTED;
    break;
  }
  case MQTT_EVENT_DISCONNECTED: {
    printf("APP - MQTT Disconnect. Reason %u\n", *((mqtt_event_t *)data));

    state = STATE_DISCONNECTED;
    process_poll(&test_serial);
    break;
  }
  case MQTT_EVENT_PUBLISH: {
    msg_ptr = data;

    /* Implement first_flag in publish message? */
    if(msg_ptr->first_chunk) {
      msg_ptr->first_chunk = 0;
      //printf("APP - Application received a publish on topic '%s'. Payload "
      //   "size is %i bytes. Content:\n\n",
     //    msg_ptr->topic, msg_ptr->payload_length);
    }

    pub_handler(msg_ptr->topic, strlen(msg_ptr->topic), msg_ptr->payload_chunk,
                msg_ptr->payload_length);
    break;
  }
  case MQTT_EVENT_SUBACK: {
   	printf("APP - Application is subscribed to topic successfully\n");
	is_it_ready = 1;
	
    break;
  }
  case MQTT_EVENT_UNSUBACK: {
    printf("APP - Application is unsubscribed to topic successfully\n");
    break;
  }
  case MQTT_EVENT_PUBACK: {
    printf("APP - Publishing complete.\n");
    break;
  }
  default:
    printf("APP - Application got a unhandled MQTT event: %i\n", event);
    break;
  }
}
/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/
static int
construct_username(void)
{
  int len = snprintf(username, BUFFER_SIZE, "");		
	printf("%s\n", username);  
	if(len < 0 || len >= BUFFER_SIZE) {
    printf("Username too large: %d, Buffer %d\n", len, BUFFER_SIZE);
    return 0;
  }

  return 1;
}
/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/
static int
construct_password(void)
{
  int len = snprintf(password, BUFFER_SIZE, ""); 
  printf("senha: %s\n", password);     
  if(len < 0 || len >= BUFFER_SIZE) {
    printf("password too large: %d, Buffer %d\n", len, BUFFER_SIZE);
    return 0;
  }

  return 1;
}
/*---------------------------------------------------------------------------*/

static int
construct_sub_topic(void)
{
  int len = snprintf(sub_topic, BUFFER_SIZE, "teste/sub");
  if(len < 0 || len >= BUFFER_SIZE) {
   	printf("Sub Topic too large: %d, Buffer %d\n", len, BUFFER_SIZE);
    return 0;
  }

  printf("Subscription topic %s\n", sub_topic);

  return 1;
}
/*---------------------------------------------------------------------------*/
static int
construct_pub_topic(void)
{
  int len = snprintf(pub_topic, BUFFER_SIZE, "teste/pub");
  if(len < 0 || len >= BUFFER_SIZE) {
    printf("Pub Topic too large: %d, Buffer %d\n", len, BUFFER_SIZE);
    return 0;
  }

  return 1;
}


/*---------------------------------------------------------------------------*/

static int
construct_client_id(void)
{
  int len = snprintf(client_id, BUFFER_SIZE, "d:%02x%02x%02x%02x%02x%02x",
                     linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1],
                     linkaddr_node_addr.u8[2], linkaddr_node_addr.u8[5],
                     linkaddr_node_addr.u8[6], linkaddr_node_addr.u8[7]);

  /* len < 0: Error. Len >= BUFFER_SIZE: Buffer too small */
  if(len < 0 || len >= BUFFER_SIZE) {
    printf("Client ID: %d, Buffer %d\n", len, BUFFER_SIZE);
    return 0;
  }

  return 1;
}
/*---------------------------------------------------------------------------*/
static void
update_config(void)
{
  if(construct_client_id() == 0) {
    /* Fatal error. Client ID larger than the buffer */
    state = STATE_CONFIG_ERROR;
    return;
  }

  if(construct_sub_topic() == 0) {
    /* Fatal error. Topic larger than the buffer */
    state = STATE_CONFIG_ERROR;
    return;
  }

  if(construct_pub_topic() == 0) {
    /* Fatal error. Topic larger than the buffer */
    state = STATE_CONFIG_ERROR;
    return;
  }
  if(construct_username() == 0) {
    /* Fatal error. Topic larger than the buffer */
    state = STATE_CONFIG_ERROR;
    return;
  }
  if(construct_password() == 0) {
    /* Fatal error. Topic larger than the buffer */
    state = STATE_CONFIG_ERROR;
    return;
  }

  /* Reset the counter */
  seq_nr_value = 0;

  state = STATE_INIT;

  /*
   * Schedule next timer event ASAP
   * If we entered an error state then we won't do anything when it fires.
   * Since the error at this stage is a config error, we will only exit this
   * error state if we get a new config.
   */
  etimer_set(&publish_periodic_timer, 0);

  return;
}
/*---------------------------------------------------------------------------*/
static int
init_config()
{
  /* Populate configuration with default values */
  memset(&conf, 0, sizeof(mqtt_client_config_t));
  memcpy(conf.event_type_id, DEFAULT_EVENT_TYPE_ID,
         strlen(DEFAULT_EVENT_TYPE_ID));
  memcpy(conf.broker_ip, broker_ip, strlen(broker_ip));
  memcpy(conf.cmd_type, DEFAULT_SUBSCRIBE_CMD_TYPE, 4);

  conf.broker_port = DEFAULT_BROKER_PORT;
  conf.pub_interval = DEFAULT_PUBLISH_INTERVAL;

  return 1;
}
/*---------------------------------------------------------------------------*/
static void
subscribe(void)
{
  /* Publish MQTT topic in IBM quickstart format */
  mqtt_status_t status;
  status = mqtt_subscribe(&conn, NULL, sub_topic, MQTT_QOS_LEVEL_0);

 // printf("APP - Subscribing to %s\n", sub_topic);
  if(status == MQTT_STATUS_OUT_QUEUE_FULL) {
  //  printf("APP - Tried to subscribe but command queue was full!\n");
  }
}
/*---------------------------------------------------------------------------*/
static void
publish(void)
{ 	
    
  //uint16_t aux;
  //int remaining = APP_BUFFER_SIZE;
  buf_ptr = app_buffer;
  //int len;
  //process_start(&test_serial, "Test Serial");	
  //process_poll(&test_serial);
 	
  printf("\nPublicando...\n");
  /*
  snprintf(buf_ptr, remaining, "{\"variable\": \"CO\",\"unit\" : \"ppm\",\"value\" : \"421.83\", \"variable\":\"LPG\",\"unit\":\"ppm\",\"value\":\"1448.36\" }");
  */
  //snprintf(buf_ptr, remaining, "hello from node, I'm working!");
  /* Put our Default route's string representation in a buffer */
  char def_rt_str[64];
  memset(def_rt_str, 0, sizeof(def_rt_str));
  ipaddr_sprintf(def_rt_str, sizeof(def_rt_str), uip_ds6_defrt_choose());

 
	
  mqtt_publish(&conn, NULL, pub_topic, (uint8_t *)app_buffer,
               strlen(app_buffer), MQTT_QOS_LEVEL_0, MQTT_RETAIN_OFF);		//Etapa de publicação
  //keep_uart_on();

  //printf("APP - Publish to %s: %s\n", pub_topic, app_buffer);
}
/*---------------------------------------------------------------------------*/
static void
connect_to_broker(void)
{
  //mqtt_set_username_password(&conn, username, password);
  /* Connect to MQTT server */
  mqtt_connect(&conn, conf.broker_ip, conf.broker_port,
               conf.pub_interval * 3);

  state = STATE_CONNECTING;
}
/*---------------------------------------------------------------------------*/
static void
state_machine(void)
{
  switch(state) {
  case STATE_INIT:
    /* If we have just been configured register MQTT connection */
    mqtt_register(&conn, &test_serial, client_id, mqtt_event,
                  MAX_TCP_SEGMENT_SIZE);

    conn.auto_reconnect = 0;
    connect_attempt = 1;

    state = STATE_REGISTERED;
    printf("Init\n");

    /* Notice there is no "break" here, it will continue to the
     * STATE_REGISTERED
     */
  case STATE_REGISTERED:
    if(uip_ds6_get_global(ADDR_PREFERRED) != NULL) {
      /* Registered and with a public IP. Connect */
    //  printf("Registered. Connect attempt %u\n", connect_attempt);
      connect_to_broker();

    } else {
      leds_on(LEDS_GREEN);
      ctimer_set(&ct, NO_NET_LED_DURATION, publish_led_off, NULL);
    }
    etimer_set(&publish_periodic_timer, NET_CONNECT_PERIODIC);
    return;
    break;

  case STATE_CONNECTING:
    leds_on(LEDS_GREEN);
    ctimer_set(&ct, CONNECTING_LED_DURATION, publish_led_off, NULL);
    /* Not connected yet. Wait */
   // printf("Connecting (%u)\n", connect_attempt);
    break;

  case STATE_CONNECTED:
    /* Notice there's no "break" here, it will continue to subscribe */

  case STATE_PUBLISHING:
    /* If the timer expired, the connection is stable. */
    if(timer_expired(&connection_life)) {
      /*
       * Intentionally using 0 here instead of 1: We want RECONNECT_ATTEMPTS
       * attempts if we disconnect after a successful connect
       */
      connect_attempt = 0;
    }

    if(mqtt_ready(&conn) && conn.out_buffer_sent) {
      /* Connected. Publish */
      if(state == STATE_CONNECTED) {
        subscribe();
        state = STATE_PUBLISHING;

      } else {
        leds_on(LEDS_GREEN);
        printf("Publishing\n");
        ctimer_set(&ct, PUBLISH_LED_ON_DURATION, publish_led_off, NULL);
        publish();
      }
      etimer_set(&publish_periodic_timer, conf.pub_interval);

      /* Return here so we don't end up rescheduling the timer */
      return;

    } else {
      /*
       * Our publish timer fired, but some MQTT packet is already in flight
       * (either not sent at all, or sent but not fully ACKd).
       *
       * This can mean that we have lost connectivity to our broker or that
       * simply there is some network delay. In both cases, we refuse to
       * trigger a new message and we wait for TCP to either ACK the entire
       * packet after retries, or to timeout and notify us.
       */
      printf("Publishing... (MQTT state=%d, q=%u)\n", conn.state,
          conn.out_queue_full);
    }
    break;

  case STATE_DISCONNECTED:
    printf("Disconnected\n");
    if(connect_attempt < RECONNECT_ATTEMPTS ||
       RECONNECT_ATTEMPTS == RETRY_FOREVER) {
      /* Disconnect and backoff */
      clock_time_t interval;
      mqtt_disconnect(&conn);
      connect_attempt++;

      interval = connect_attempt < 3 ? RECONNECT_INTERVAL << connect_attempt :
        RECONNECT_INTERVAL << 3;

      printf("Disconnected. Attempt %u in %lu ticks\n", connect_attempt, interval);

      etimer_set(&publish_periodic_timer, interval);

      state = STATE_REGISTERED;
      return;

    } else {
      /* Max reconnect attempts reached. Enter error state */
      state = STATE_ERROR;
      printf("Aborting connection after %u attempts\n", connect_attempt - 1);
    }
    break;

  case STATE_CONFIG_ERROR:
    /* Idle away. The only way out is a new config */
    printf("Bad configuration.\n");
    return;

  case STATE_ERROR:
  default:
    leds_on(LEDS_GREEN);
    printf("Default case: State=0x%02x\n", state);
    return;
  }

  /* If we didn't return so far, reschedule ourselves */
  etimer_set(&publish_periodic_timer, STATE_MACHINE_PERIODIC);
}


/*---------------------FUNÇÃO IMPORTANTE PARA FUNCIONAMENTO DO UART----------*/

static void
keep_uart_on(void)
{
  cc26xx_uart_set_input(serial_line_input_byte);
}

static void
release_uart(void)
{
	cc26xx_uart_set_input(NULL);
}

/*---------------------------------------------------------------------------*/
 
 //AUTOSTART_PROCESSES(&test_serial);
 
 
 
PROCESS_THREAD(test_serial, ev, data)
{
	PROCESS_BEGIN();
	
	
	buf_ptr = app_buffer; 
	int len;	
	
	if(init_config() != 1) {
		PROCESS_EXIT();
	}

	update_config();
   
	printf("\nProcesso para receber dados via serial, aguardando dados...\n");
	//keep_uart_on();
   
	//printf("Entrando no while principal");
	while(is_it_ready != 1){
			PROCESS_YIELD();			
			if((ev == PROCESS_EVENT_TIMER && data == &publish_periodic_timer) ||
       			ev == PROCESS_EVENT_POLL) {
              	state_machine();
    		}			
			//state_machine();
			//for(unsigned long int i=0; i<50000; i++);	
		}
	printf("Entrando no while principal");
	while(1){
		
		//if(is_it_ready){
			keep_uart_on();
		//}

		PROCESS_WAIT_EVENT_UNTIL(ev == serial_line_event_message);
		if(ev == serial_line_event_message) {
			printf("received line: %s\n", (char *)data);
	   		int remaining = APP_BUFFER_SIZE;
			len = snprintf(buf_ptr, remaining, (char *)data);

			printf("\nDado armazenado no buffer com sucesso\n");			
	     	remaining -= len;
	     	buf_ptr += len;

	 		release_uart();  

	     	if(len < 0 || len >= remaining) {
				printf("Buffer too short. Have %d, need %d + \\0\n", remaining, len);
				return;
			
			//state_machine();
			
			}
			
			state_machine();
		}
	
	}
	
	PROCESS_END();
}

/*------------------------------------------------------------------------*/
/*------------------------------------------------------------------------*/


/*
PROCESS_THREAD(mqtt_demo_process, ev, data)
{

  PROCESS_BEGIN();

 // printf("MQTT Demo Process\n");

  if(init_config() != 1) {
    PROCESS_EXIT();
  }

  update_config();
  //printf("Entrando no while principal");
  while(1) {
    PROCESS_YIELD();
    
    if((ev == PROCESS_EVENT_TIMER && data == &publish_periodic_timer) ||
       ev == PROCESS_EVENT_POLL) {
              	state_machine();
    }
    
    
    
  }

  PROCESS_END();
}*/
/*---------------------------------------------------------------------------*/
