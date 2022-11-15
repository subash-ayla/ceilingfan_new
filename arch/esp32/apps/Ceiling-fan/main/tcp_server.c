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
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
/*#include "protocol_examples_common.h"*/

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include <ayla/log.h>


#define PORT                        9999
#define KEEPALIVE_IDLE              30
#define KEEPALIVE_INTERVAL          1
#define KEEPALIVE_COUNT             10

static char gl_server_ip[16] = "192.168.1.199";
static char gl_set_server_ip_flag;

static char rx_buffer[1000];

static void do_retransmit(const int sock)
{
	int len;

	do {
		memset(rx_buffer, 0, sizeof(rx_buffer));
		len = recv(sock, rx_buffer, sizeof(rx_buffer), 0);
		if (len < 0) {
			log_put(LOG_INFO "%s: Error occurred during receiving:"
				" errno %d", __func__, errno);
		} else if (len == 0) {
			log_put(LOG_INFO "%s: Connection closed", __func__);
		} else {
			log_put(LOG_INFO "%s: Received %d bytes",
				__func__, len);
		}
	} while (len > 0);
}

void tcp_set_server_ip(const char *ip)
{
	strncpy(gl_server_ip,  ip, sizeof(gl_server_ip));
	log_put(LOG_INFO "%s: IP %s", __func__, gl_server_ip);
	gl_set_server_ip_flag = true;
}

int tcp_get_server_ip_flag(void)
{
	return gl_set_server_ip_flag;
}

void tcp_server_task(void *pvParameters)
{
	char addr_str[128];
	int addr_family = AF_INET;
	int ip_protocol = 0;
	int keepAlive = 1;
	int keepIdle = KEEPALIVE_IDLE;
	int keepInterval = KEEPALIVE_INTERVAL;
	int keepCount = KEEPALIVE_COUNT;
	struct sockaddr_in dest_addr_ip4;

	log_put(LOG_INFO "%s: running", __func__);

	dest_addr_ip4.sin_addr.s_addr = inet_addr(gl_server_ip);
	dest_addr_ip4.sin_family = AF_INET;
	dest_addr_ip4.sin_port = htons(PORT);
	ip_protocol = IPPROTO_IP;

	int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
	if (listen_sock < 0) {
		log_put(LOG_INFO "%s: socket errno %d", __func__, errno);
		vTaskDelete(NULL);
		return;
	}
	int opt = 1;
	setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	log_put(LOG_INFO "%s: Socket created", __func__);

	int err = bind(listen_sock,
		(struct sockaddr *)&dest_addr_ip4, sizeof(dest_addr_ip4));
	if (err != 0) {
		log_put(LOG_INFO "%s: bind errno %d", __func__, errno);
		goto CLEAN_UP;
	}
	log_put(LOG_INFO "%s: Socket bound, port %d", __func__, PORT);

	err = listen(listen_sock, 1);
	if (err != 0) {
		log_put(LOG_INFO "%s: listen errno %d", __func__, errno);
		goto CLEAN_UP;
	}
	log_put(LOG_INFO "%s: listened", __func__);

	while (1) {
		log_put(LOG_INFO "%s: Socket listening", __func__);

		/* Large enough for both IPv4 or IPv6 */
		struct sockaddr_storage source_addr;
		socklen_t addr_len = sizeof(source_addr);
		int sock = accept(listen_sock,
			(struct sockaddr *)&source_addr, &addr_len);
		if (sock < 0) {
			log_put(LOG_INFO "%s: accept errno %d",
				__func__, errno);
			break;
		}

		/* Set tcp keepalive option */
		setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE,
		&keepAlive, sizeof(int));
		setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE,
			&keepIdle, sizeof(int));
		setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL,
			&keepInterval, sizeof(int));
		setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT,
			&keepCount, sizeof(int));
		/* Convert ip address to string */
		if (source_addr.ss_family == PF_INET) {
			inet_ntoa_r(
				((struct sockaddr_in *)&source_addr)->sin_addr,
				addr_str, sizeof(addr_str) - 1);
		}
		log_put(LOG_INFO "%s: accepted %s", __func__, addr_str);

		do_retransmit(sock);

		shutdown(sock, 0);
		close(sock);
	}

CLEAN_UP:
	close(listen_sock);
	vTaskDelete(NULL);
}

