/* BSD Socket API Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
/*#include "protocol_examples_common.h"
#include "addr_from_stdin.h"*/
#include "lwip/err.h"
#include "lwip/sockets.h"
#include <ayla/log.h>
#include "app_logic.h"


#define PORT 9999

static char gl_client_ip[16] = "192.168.1.199";
static char gl_set_client_ip_flag;

static const char *payload = \
	"00000000001111111111222222222233333333334444444444"
	"55555555556666666666777777777788888888889999999999"
	"00000000001111111111222222222233333333334444444444"
	"55555555556666666666777777777788888888889999999999"
	"00000000001111111111222222222233333333334444444444"
	"55555555556666666666777777777788888888889999999999"
	"00000000001111111111222222222233333333334444444444"
	"55555555556666666666777777777788888888889999999999"
	"00000000001111111111222222222233333333334444444444"
	"55555555556666666666777777777788888888889999999999"
	"00000000001111111111222222222233333333334444444444"
	"55555555556666666666777777777788888888889999999999"
	"00000000001111111111222222222233333333334444444444"
	"55555555556666666666777777777788888888889999999999"
	"00000000001111111111222222222233333333334444444444"
	"55555555556666666666777777777788888888889999999999"
	"00000000001111111111222222222233333333334444444444"
	"55555555556666666666777777777788888888889999999999"
	"00000000001111111111222222222233333333334444444444"
	"55555555556666666666777777777788888888889999999999";


void tcp_set_client_ip(const char *ip)
{
	strncpy(gl_client_ip,  ip, sizeof(gl_client_ip));
	log_put(LOG_INFO "%s: IP %s", __func__, gl_client_ip);
	gl_set_client_ip_flag = true;
}

int tcp_get_client_ip_flag(void)
{
	return gl_set_client_ip_flag;
}

void tcp_client_task(void *pvParameters)
{
	int addr_family = 0;
	int ip_protocol = 0;
	log_put(LOG_INFO "%s: running", __func__);

	while (1) {
		struct sockaddr_in dest_addr;
		dest_addr.sin_addr.s_addr = inet_addr(gl_client_ip);
		dest_addr.sin_family = AF_INET;
		dest_addr.sin_port = htons(PORT);
		addr_family = AF_INET;
		ip_protocol = IPPROTO_IP;

		int sock =  socket(addr_family, SOCK_STREAM, ip_protocol);
		if (sock < 0) {
			log_put(LOG_INFO "%s: socket errno %d",
				__func__, errno);
			break;
		}
		log_put(LOG_INFO "%s: socket to %s:%d",
			__func__, gl_client_ip, PORT);

		int err = connect(sock,
			(struct sockaddr *)&dest_addr,
			sizeof(struct sockaddr_in6));
		if (err != 0) {
			log_put(LOG_INFO "%s: connect errno %d",
				__func__, errno);
			break;
		}
		log_put(LOG_INFO "%s: Successfully connected", __func__);

		while (1) {
			int err = send(sock, payload, strlen(payload), 0);
			if (err < 0) {
				log_put(LOG_INFO "%s: send errno %d",
					__func__, errno);
				break;
			} else {
				log_put(LOG_INFO "%s: send ret %d",
					__func__, err);
			}
		}

		if (sock != -1) {
			log_put(LOG_INFO "%s: Shutting down socket"
				" and restarting.", __func__);
			shutdown(sock, 0);
			close(sock);
		}
	}

	app_clear_tcp_task_type();
	gl_set_client_ip_flag = false;
	vTaskDelete(NULL);
}

