/* UDP MultiCast Send/Receive Example

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
#include "protocol_examples_common.h"
#include "udp_cast_replay.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "stdsoap2.h"


/* The examples use simple configuration that you can set via
   project configuration.

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define UDP_PORT 3333
*/
#define UDP_PORT CONFIG_EXAMPLE_PORT

#define MULTICAST_LOOPBACK CONFIG_EXAMPLE_LOOPBACK

#define MULTICAST_TTL CONFIG_EXAMPLE_MULTICAST_TTL

#define MULTICAST_IPV4_ADDR CONFIG_EXAMPLE_MULTICAST_IPV4_ADDR
#define MULTICAST_IPV6_ADDR CONFIG_EXAMPLE_MULTICAST_IPV6_ADDR

#define LISTEN_ALL_IF   EXAMPLE_MULTICAST_LISTEN_ALL_IF

static const char *TAG = "multicast";
static const char *V4TAG = "mcast-ipv4";
static const char *WSDL = "WSDL";

struct soap soap;

// A simple function to simulate processing XML data streams
void processXmlStream(const char* xmlData) {
    const char *ptr = xmlData;

    while (*ptr != '\0') {
        // Find the start of an element
        if (*ptr == '<') {
            const char *start = ptr + 1;

            // Check if it's a closing tag
            if (*start != '/') {
                const char *end = strchr(start, '>');
                if (end) {
                    int len = end - start;

                    // Extract element name
                    char elementName[len + 1];
                    strncpy(elementName, start, len);
                    elementName[len] = '\0';

                    ESP_LOGD(WSDL, "Found element: %s\n", elementName);

                    // Here you'd add logic to handle specific elements, 
                    // perhaps pushing opening tags onto a stack and popping 
                    // them off when you encounter a corresponding closing tag.
                }
                ptr = end;
            }
        }
        ptr++;
    }
}
/* Add a socket, either IPV4-only or IPV6 dual mode, to the IPV4
   multicast group */
static int socket_add_ipv4_multicast_group(int sock, bool assign_source_if)
{
    struct ip_mreq imreq = { 0 };
    struct in_addr iaddr = { 0 };
    int err = 0;
    // Configure source interface
#if LISTEN_ALL_IF
    imreq.imr_interface.s_addr = IPADDR_ANY;
#else
    esp_netif_ip_info_t ip_info = { 0 };
    err = esp_netif_get_ip_info(get_example_netif(), &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(V4TAG, "Failed to get IP address info. Error 0x%x", err);
        goto err;
    }
    inet_addr_from_ip4addr(&iaddr, &ip_info.ip);
#endif // LISTEN_ALL_IF
    // Configure multicast address to listen to
    err = inet_aton(MULTICAST_IPV4_ADDR, &imreq.imr_multiaddr.s_addr);
    if (err != 1) {
        ESP_LOGE(V4TAG, "Configured IPV4 multicast address '%s' is invalid.", MULTICAST_IPV4_ADDR);
        // Errors in the return value have to be negative
        err = -1;
        goto err;
    }
    ESP_LOGI(TAG, "Configured IPV4 Multicast address %s", inet_ntoa(imreq.imr_multiaddr.s_addr));
    if (!IP_MULTICAST(ntohl(imreq.imr_multiaddr.s_addr))) {
        ESP_LOGW(V4TAG, "Configured IPV4 multicast address '%s' is not a valid multicast address. This will probably not work.", MULTICAST_IPV4_ADDR);
    }

    if (assign_source_if) {
        // Assign the IPv4 multicast source interface, via its IP
        // (only necessary if this socket is IPV4 only)
        err = setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &iaddr,
                         sizeof(struct in_addr));
        if (err < 0) {
            ESP_LOGE(V4TAG, "Failed to set IP_MULTICAST_IF. Error %d", errno);
            goto err;
        }
    }

    err = setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                         &imreq, sizeof(struct ip_mreq));
    if (err < 0) {
        ESP_LOGE(V4TAG, "Failed to set IP_ADD_MEMBERSHIP. Error %d", errno);
        goto err;
    }

 err:
    return err;
}


static int create_multicast_ipv4_socket(void)
{
    struct sockaddr_in saddr = { 0 };
    int sock = -1;
    int err = 0;

    sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(V4TAG, "Failed to create socket. Error %d", errno);
        return -1;
    }

    // Bind the socket to any address
    saddr.sin_family = PF_INET;
    saddr.sin_port = htons(UDP_PORT);
    saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    err = bind(sock, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in));
    if (err < 0) {
        ESP_LOGE(V4TAG, "Failed to bind socket. Error %d", errno);
        goto err;
    }


    // Assign multicast TTL (set separately from normal interface TTL)
    uint8_t ttl = MULTICAST_TTL;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(uint8_t));
    if (err < 0) {
        ESP_LOGE(V4TAG, "Failed to set IP_MULTICAST_TTL. Error %d", errno);
        goto err;
    }

#if MULTICAST_LOOPBACK
    // select whether multicast traffic should be received by this device, too
    // (if setsockopt() is not called, the default is no)
    uint8_t loopback_val = MULTICAST_LOOPBACK;
    err = setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP,
                     &loopback_val, sizeof(uint8_t));
    if (err < 0) {
        ESP_LOGE(V4TAG, "Failed to set IP_MULTICAST_LOOP. Error %d", errno);
        goto err;
    }
#endif

    // this is also a listening socket, so add it to the multicast
    // group for listening...
    err = socket_add_ipv4_multicast_group(sock, true);
    if (err < 0) {
        goto err;
    }

    // All set, socket is configured for sending and receiving
    return sock;

err:
    close(sock);
    return -1;
}


static void mcast_example_task(void *pvParameters)
{
    while (1) {
        int sock;

        sock = create_multicast_ipv4_socket();
        if (sock < 0) {
            ESP_LOGE(TAG, "Failed to create IPv4 multicast socket");
        }

        if (sock < 0) {
            // Nothing to do!
            vTaskDelay(5 / portTICK_PERIOD_MS);
            continue;
        }


        // set destination multicast addresses for sending from these sockets
        struct sockaddr_in sdestv4 = {
            .sin_family = PF_INET,
            .sin_port = htons(UDP_PORT),
        };
        // We know this inet_aton will pass because we did it above already
        inet_aton(MULTICAST_IPV4_ADDR, &sdestv4.sin_addr.s_addr);


        // Loop waiting for UDP received, and sending UDP packets if we don't
        // see any.
        int err = 1;
        while (err > 0) {
            struct timeval tv = {
                .tv_sec = 2,
                .tv_usec = 0,
            };
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sock, &rfds);

            int s = select(sock + 1, &rfds, NULL, NULL, &tv);
            if (s < 0) {
                ESP_LOGE(TAG, "Select failed: errno %d", errno);
                err = -1;
                break;
            }
            /* if Data was received */
            else if (s > 0) {
                if (FD_ISSET(sock, &rfds)) {
                    // Incoming datagram received
                    char raddr_name[32] = { 0 };


                    struct sockaddr_storage raddr; // Large enough for both IPv4 or IPv6
                    socklen_t socklen = sizeof(raddr);
                    int len = recvfrom(sock, soap.tmpbuf, sizeof(soap.tmpbuf)-1, 0,
                                       (struct sockaddr *)&raddr, &socklen);
                    if (len < 0) {
                        ESP_LOGE(TAG, "multicast recvfrom failed: errno %d", errno);
                        err = -1;
                        break;
                    }

                    // Get the sender's address as a string
                    if (raddr.ss_family == PF_INET) {
                        inet_ntoa_r(((struct sockaddr_in *)&raddr)->sin_addr,raddr_name, sizeof(raddr_name)-1);
                    }

                    ESP_LOGI(TAG, "received %d bytes from %s:", len, raddr_name);

                    //recvbuf[len] = 0; // Null-terminate whatever we received and treat like a string...
                    ESP_LOGI(TAG, "%s", soap.tmpbuf);

                    
                    /**************  replay to the incomming msg  ****************/
                    processXmlStream(soap.tmpbuf);
                    soap_peek_element(&soap);
                    // struct addrinfo hints = 
                    // {
                    //     .ai_flags = AI_PASSIVE,
                    //     .ai_socktype = SOCK_DGRAM,
                    // };
                    // struct addrinfo *res;

                    // hints.ai_family = AF_INET; // For an IPv4 socket

                    // int err = getaddrinfo(raddr_name,
                    //                     NULL,
                    //                     &hints,
                    //                     &res);

                    // /* destanation port set for tha sander port  */
                    // ((struct sockaddr_in *)res->ai_addr)->sin_port = ((struct sockaddr_in *)&raddr)->sin_port; /*htons(UDP_PORT);*/
                    // /* destanation ip address set for tha sander ip address  */
                    // inet_ntoa_r(((struct sockaddr_in *)res->ai_addr)->sin_addr, raddr_name, sizeof(raddr_name)-1);
                    // err = sendto(sock, ReplayBuff1, sizeof(ReplayBuff1), 0, res->ai_addr, res->ai_addrlen);
                    

                    // freeaddrinfo(res);
                    // if (err < 0) 
                    // {
                    //     ESP_LOGE(TAG, "IPV4 sendto failed. errno: %d", errno);
                    //     break;
                    // }
                    // ESP_LOGI(TAG, "Replaing to multicast address %s, Port%d\n\n Data\n%s\n",  raddr_name, ((struct sockaddr_in *)&raddr)->sin_port, ReplayBuff1);
                }
            }
        }

        ESP_LOGE(TAG, "Shutting down socket and restarting...");
        shutdown(sock, 0);
        close(sock);
    }

}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

    xTaskCreate(&mcast_example_task, "mcast_task", 4096, NULL, 5, NULL);
}
