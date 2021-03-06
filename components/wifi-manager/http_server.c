/*
Copyright (c) 2017-2019 Tony Pottier

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

@file http_server.c
@author Tony Pottier
@brief Defines all functions necessary for the HTTP server to run.

Contains the freeRTOS task for the HTTP listener and all necessary support
function to process requests, decode URLs, serve files, etc. etc.

@note http_server task cannot run without the wifi_manager task!
@see https://idyl.io
@see https://github.com/tonyp7/esp32-wifi-manager
*/

#include "http_server.h"
#include "cmd_system.h"
#include <inttypes.h>
#include "squeezelite-ota.h"
#include "nvs_utilities.h"
#include <stdio.h>
#include <stdlib.h>
#include "cJSON.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"

#define HTTP_STACK_SIZE	(5*1024)

/* @brief tag used for ESP serial console messages */
static const char TAG[] = "http_server";
/* @brief task handle for the http server */
static TaskHandle_t task_http_server = NULL;
static StaticTask_t task_http_buffer;
#if RECOVERY_APPLICATION
static StackType_t task_http_stack[HTTP_STACK_SIZE];
#else
static StackType_t EXT_RAM_ATTR task_http_stack[HTTP_STACK_SIZE];
#endif
SemaphoreHandle_t http_server_config_mutex = NULL;

/**
 * @brief embedded binary data.
 * @see file "component.mk"
 * @see https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html#embedding-binary-data
 */
extern const uint8_t style_css_start[] asm("_binary_style_css_start");
extern const uint8_t style_css_end[]   asm("_binary_style_css_end");
extern const uint8_t jquery_gz_start[] asm("_binary_jquery_min_js_gz_start");
extern const uint8_t jquery_gz_end[] asm("_binary_jquery_min_js_gz_end");
extern const uint8_t popper_gz_start[] asm("_binary_popper_min_js_gz_start");
extern const uint8_t popper_gz_end[] asm("_binary_popper_min_js_gz_end");
extern const uint8_t bootstrap_js_gz_start[] asm("_binary_bootstrap_min_js_gz_start");
extern const uint8_t bootstrap_js_gz_end[] asm("_binary_bootstrap_min_js_gz_end");
extern const uint8_t bootstrap_css_gz_start[] asm("_binary_bootstrap_min_css_gz_start");
extern const uint8_t bootstrap_css_gz_end[] asm("_binary_bootstrap_min_css_gz_end");
extern const uint8_t code_js_start[] asm("_binary_code_js_start");
extern const uint8_t code_js_end[] asm("_binary_code_js_end");
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");


/* const http headers stored in ROM */
const static char http_hdr_template[] = "HTTP/1.1 200 OK\nContent-type: %s\nAccept-Ranges: bytes\nContent-Length: %d\nContent-Encoding: %s\nAccess-Control-Allow-Origin: *\n\n";
const static char http_html_hdr[] = "HTTP/1.1 200 OK\nContent-type: text/html\nAccess-Control-Allow-Origin: *\nAccept-Encoding: identity\n\n";
const static char http_css_hdr[] = "HTTP/1.1 200 OK\nContent-type: text/css\nCache-Control: public, max-age=31536000\nAccess-Control-Allow-Origin: *\n\n";
const static char http_js_hdr[] = "HTTP/1.1 200 OK\nContent-type: text/javascript\nAccess-Control-Allow-Origin: *\n\n";
const static char http_400_hdr[] = "HTTP/1.1 400 Bad Request\nContent-Length: 0\n\n";
const static char http_404_hdr[] = "HTTP/1.1 404 Not Found\nContent-Length: 0\n\n";
const static char http_503_hdr[] = "HTTP/1.1 503 Service Unavailable\nContent-Length: 0\n\n";
const static char http_ok_json_no_cache_hdr[] = "HTTP/1.1 200 OK\nContent-type: application/json\nCache-Control: no-store, no-cache, must-revalidate, max-age=0\nPragma: no-cache\nAccess-Control-Allow-Origin: *\nAccept-Encoding: identity\n\n";
const static char http_redirect_hdr_start[] = "HTTP/1.1 302 Found\nLocation: http://";
const static char http_redirect_hdr_end[] = "/\n\n";


void http_server_start() {
	ESP_LOGD(TAG,  "http_server_start ");
	if(task_http_server == NULL) {
		task_http_server = xTaskCreateStatic( (TaskFunction_t) &http_server, "http_server", HTTP_STACK_SIZE, NULL, 
										 WIFI_MANAGER_TASK_PRIORITY, task_http_stack, &task_http_buffer);
	}
}
void http_server(void *pvParameters) {
	http_server_config_mutex = xSemaphoreCreateMutex();
	struct netconn *conn, *newconn;
	err_t err;
	conn = netconn_new(NETCONN_TCP);
	netconn_bind(conn, IP_ADDR_ANY, 80);
	netconn_listen(conn);
	ESP_LOGI(TAG,   "HTTP Server listening on 80/tcp");
	do {
		err = netconn_accept(conn, &newconn);
		if(err == ERR_OK) {
			http_server_netconn_serve(newconn);
			netconn_delete(newconn);
		}
		else
		{
			ESP_LOGE(TAG,  "Error accepting new connection. Terminating HTTP server");
		}
		taskYIELD();  /* allows the freeRTOS scheduler to take over if needed. */
	} while(err == ERR_OK);

	netconn_close(conn);
	netconn_delete(conn);
	vSemaphoreDelete(http_server_config_mutex);
	http_server_config_mutex = NULL;
	vTaskDelete( NULL );
}


char* http_server_get_header(char *request, char *header_name, int *len) {
	*len = 0;
	char *ret = NULL;
	char *ptr = NULL;

	ptr = strstr(request, header_name);
	if(ptr) {
		ret = ptr + strlen(header_name);
		ptr = ret;
		while (*ptr != '\0' && *ptr != '\n' && *ptr != '\r') {
			(*len)++;
			ptr++;
		}
		return ret;
	}
	return NULL;
}
char* http_server_search_header(char *request, char *header_name, int *len, char ** parm_name, char ** next_position, char * bufEnd) {
	*len = 0;
	char *ret = NULL;
	char *ptr = NULL;
	int currentLength=0;

	ESP_LOGV(TAG,   "searching for header name: [%s]", header_name);
	ptr = strstr(request, header_name);


	if(ptr!=NULL && ptr<bufEnd) {
		ret = ptr + strlen(header_name);
		ptr = ret;
		currentLength=(int)(ptr-request);
		ESP_LOGV(TAG,   "found string at %d", currentLength);

		while (*ptr != '\0' && *ptr != '\n' && *ptr != '\r' && *ptr != ':' && ptr<bufEnd) {
			ptr++;
		}
		if(*ptr==':') {
			currentLength=(int)(ptr-ret);
			ESP_LOGV(TAG,   "Found parameter name end, length : %d", currentLength);
			// save the parameter name: the string between header name and ":"
			*parm_name=malloc(currentLength+1);
			if(*parm_name==NULL) {
				ESP_LOGE(TAG,   "Unable to allocate memory for new header name");
				return NULL;
			}
			memset(*parm_name, 0x00,currentLength+1);
			strncpy(*parm_name,ret,currentLength);
			ESP_LOGV(TAG,   "Found parameter name : %s ", *parm_name);
			ptr++;
			while (*ptr == ' ' && ptr<bufEnd) {
				ptr++;
			}

		}
		ret=ptr;
		while (*ptr != '\0' && *ptr != '\n' && *ptr != '\r'&& ptr<bufEnd) {
			(*len)++;
			ptr++;
		}
		// Terminate value inside its actual buffer so we can treat it as individual string
		*ptr='\0';
		currentLength=(int)(ptr-ret);
		ESP_LOGV(TAG,   "Found parameter value end, length : %d, 	value: %s", currentLength,ret );

		*next_position=++ptr;
		return ret;
	}
	ESP_LOGD(TAG,   "No more match for : %s", header_name);
	return NULL;
}
void http_server_send_resource_file(struct netconn *conn,const uint8_t * start, const uint8_t * end, char * content_type,char * encoding) {
	uint16_t len=end - start;
	size_t  buff_length= sizeof(http_hdr_template)+strlen(content_type)+strlen(encoding);
	char * http_hdr=malloc(buff_length);
	if( http_hdr == NULL) {
		ESP_LOGE(TAG,  "Cound not allocate %d bytes for headers.",buff_length);
		netconn_write(conn, http_503_hdr, sizeof(http_503_hdr) - 1, NETCONN_NOCOPY);
	}
	else
	{
		memset(http_hdr,0x00,buff_length);
		snprintf(http_hdr, buff_length-1,http_hdr_template,content_type,len,encoding);
		netconn_write(conn, http_hdr, strlen(http_hdr), NETCONN_NOCOPY);
		ESP_LOGD(TAG,  "sending response : %s",http_hdr);
		netconn_write(conn, start, end - start, NETCONN_NOCOPY);
		free(http_hdr);
	}
}

err_t http_server_send_config_json(struct netconn *conn) {
	char * json = config_alloc_get_json(false);
	if(json!=NULL){
		ESP_LOGD(TAG,  "config json : %s",json );
		netconn_write(conn, http_ok_json_no_cache_hdr, sizeof(http_ok_json_no_cache_hdr) - 1, NETCONN_NOCOPY);
		netconn_write(conn, json, strlen(json), NETCONN_NOCOPY);
		free(json);
	}
	else{
		ESP_LOGD(TAG,  "Error retrieving config json string. ");
		netconn_write(conn, http_503_hdr, sizeof(http_503_hdr) - 1, NETCONN_NOCOPY);
	}

	return ESP_OK;
}

void http_server_process_config(struct netconn *conn, 	char *inbuf) {

	// Here, we are passed a buffer which contains the http request

//		netbuf_data(inbuf, (void**)&buf, &buflen);
//		err = netconn_recv(conn, &inbuf);
//		if(err == ERR_OK) {
//
//		/* extract the first line of the request */
//		char *save_ptr = buf;
//		char *line = strtok_r(save_ptr, new_line, &save_ptr);
//		ESP_LOGD(TAG,  "Processing line %s",line);
	ESP_LOGD(TAG,  "Processing request buffer: \n%s",inbuf);
	char *last = NULL;
	char *ptr = NULL;
	last = ptr = inbuf;
	bool bHeaders= true;
	while(ptr!=NULL && *ptr != '\0') {
		// Move to the end of the line, or to the end of the buffer
		if(bHeaders) {
			while (*ptr != '\0' && *ptr != '\n' && *ptr != '\r') {
				ptr++;
			}
			// terminate the header string
			if( *(ptr) == '\0' ) {
				ESP_LOGD(TAG,   "End of buffer found");
				return;
			}
			*ptr = '\0';
			if( *(ptr+1) == '\n' ) {
				*(ptr+1)='\0';
				ptr+=2;
			}
			if(ptr==last) {
				ESP_LOGD(TAG,  "Processing body. ");
				break;
			}
			if(strlen(last)>0) {
				ESP_LOGD(TAG,  "Found Header Line %s ", last);
				//Content-Type: application/json
			}
			else {
				ESP_LOGD(TAG,  "Found end of headers");
				bHeaders = false;
			}
			last=ptr;
		}
		else {
			//ESP_LOGD(TAG,  "Body content: %s", last);
			//cJSON * json = cJSON_Parse(last);
			//cJSON_Delete(json);
			//todo:  implement body json parsing
			// right now, body is coming as compressed, so we need some type of decompression to happen.
			return;
		}
	}
	return ;

}

void dump_net_buffer(void * buf, u16_t buflen) {
	char * curbuf = malloc(buflen+1);
	ESP_LOGV(TAG,  "netconn buffer, length=%u",buflen);
	if(curbuf==NULL) {
		ESP_LOGE(TAG,  "Unable to show netconn buffer.  Malloc failed");
	}
	memset(curbuf,0x0, buflen+1);
	memcpy(curbuf,buf,buflen);
	ESP_LOGV(TAG,  "netconn buffer content:\n%s",curbuf);
	free(curbuf);
}

void http_server_netconn_serve(struct netconn *conn) {

	struct netbuf *inbuf;
	char *buf = NULL;
	u16_t buflen = 0;
	err_t err;
	ip_addr_t remote_add;
	u16_t port;
	ESP_LOGV(TAG,  "Serving page.  Getting device AP address.");
	const char new_line[2] = "\n";
	char * ap_ip_address= config_alloc_get_default(NVS_TYPE_STR, "ap_ip_address", DEFAULT_AP_IP, 0);
	if(ap_ip_address==NULL){
		ESP_LOGE(TAG,  "Unable to retrieve default AP IP Address");
		netconn_write(conn, http_503_hdr, sizeof(http_503_hdr) - 1, NETCONN_NOCOPY);
		netconn_close(conn);
		return;
	}
	ESP_LOGV(TAG,  "Getting remote device IP address.");
	netconn_getaddr(conn,	&remote_add,	&port,	0);
	char * remote_address = strdup(ip4addr_ntoa(ip_2_ip4(&remote_add)));
	ESP_LOGD(TAG,  "Local Access Point IP address is: %s. Remote device IP address is %s. Receiving request buffer", ap_ip_address, remote_address);

	u16_t bufsize = 0;
	netconn_set_recvtimeout(conn, 50);
	while (netconn_recv(conn, &inbuf) == ERR_OK) {
		do {
			u8_t *rcvbuf;
			u16_t rcvlen;
			netbuf_data(inbuf, (void**)&rcvbuf, &rcvlen);
			dump_net_buffer(rcvbuf, rcvlen);
			if (buflen + rcvlen > bufsize) {
				bufsize += rcvlen - bufsize < 2048 ? 2048 : rcvlen - bufsize;
				buf = realloc(buf, bufsize);
			}
			memcpy(buf + buflen, rcvbuf, rcvlen);
			buflen += rcvlen;
			ESP_LOGI(TAG, "received netbuf of %hu", rcvlen);
		} while (netbuf_next(inbuf) != -1);
		netbuf_delete(inbuf);
	}

	if(buflen) {
		ESP_LOGV(TAG,  "Getting data buffer.");
		int lenH = 0;
		/* extract the first line of the request */
		char *save_ptr = buf;
		char *line = strtok_r(save_ptr, new_line, &save_ptr);
		char *temphost = http_server_get_header(save_ptr, "Host: ", &lenH);
		char * host = malloc(lenH+1);
		memset(host,0x00,lenH+1);
		if(lenH>0){
			strlcpy(host,temphost,lenH+1);
		}
		ESP_LOGD(TAG,  "http_server_netconn_serve Host: [%s], host: [%s], Processing line [%s]",remote_address,host,line);

		if(line) {

			/* captive portal functionality: redirect to access point IP for HOST that are not the access point IP OR the STA IP */
			const char * host_name=NULL;
			if((err=tcpip_adapter_get_hostname(TCPIP_ADAPTER_IF_STA, &host_name )) !=ESP_OK) {
				ESP_LOGE(TAG,  "Unable to get host name. Error: %s",esp_err_to_name(err));
			}
			else {
				ESP_LOGI(TAG,"System host name %s, http requested host: %s.",host_name, host);
			}

			/* determine if Host is from the STA IP address */
			wifi_manager_lock_sta_ip_string(portMAX_DELAY);
			bool access_from_sta_ip = lenH > 0?strcasestr(host, wifi_manager_get_sta_ip_string()):false;
			wifi_manager_unlock_sta_ip_string();
			bool access_from_host_name = (host_name!=NULL) && strcasestr(host,host_name);

			if(lenH > 0 && !strcasestr(host, ap_ip_address) && !(access_from_sta_ip || access_from_host_name)) {
				ESP_LOGI(TAG,  "Redirecting host [%s] to AP IP Address : %s",remote_address, ap_ip_address);
				netconn_write(conn, http_redirect_hdr_start, sizeof(http_redirect_hdr_start) - 1, NETCONN_NOCOPY);
				netconn_write(conn, ap_ip_address, strlen(ap_ip_address), NETCONN_NOCOPY);
				netconn_write(conn, http_redirect_hdr_end, sizeof(http_redirect_hdr_end) - 1, NETCONN_NOCOPY);
			}
			else {
                //static stuff
				/* default page */
				if(strstr(line, "GET / ")) {
					netconn_write(conn, http_html_hdr, sizeof(http_html_hdr) - 1, NETCONN_NOCOPY);
					netconn_write(conn, index_html_start, index_html_end- index_html_start, NETCONN_NOCOPY);
				}
				else if(strstr(line, "GET /code.js ")) {
					netconn_write(conn, http_js_hdr, sizeof(http_js_hdr) - 1, NETCONN_NOCOPY);
					netconn_write(conn, code_js_start, code_js_end - code_js_start, NETCONN_NOCOPY);
				}
				else if(strstr(line, "GET /style.css ")) {
					netconn_write(conn, http_css_hdr, sizeof(http_css_hdr) - 1, NETCONN_NOCOPY);
					netconn_write(conn, style_css_start, style_css_end - style_css_start, NETCONN_NOCOPY);
				}
				else if(strstr(line, "GET /jquery.js ")) {
					http_server_send_resource_file(conn,jquery_gz_start, jquery_gz_end, "text/javascript", "gzip" );
				}
				else if(strstr(line, "GET /popper.js ")) {
					http_server_send_resource_file(conn,popper_gz_start, popper_gz_end, "text/javascript", "gzip" );
				}
				else if(strstr(line, "GET /bootstrap.js ")) {
					http_server_send_resource_file(conn,bootstrap_js_gz_start, bootstrap_js_gz_end, "text/javascript", "gzip" );
				}
				else if(strstr(line, "GET /bootstrap.css ")) {
					http_server_send_resource_file(conn,bootstrap_css_gz_start, bootstrap_css_gz_end, "text/css", "gzip" );
				}

                //dynamic stuff
				else if(strstr(line, "GET /scan.json ")) {
					ESP_LOGI(TAG,  "Starting wifi scan");
					wifi_manager_scan_async();
				}
				else if(strstr(line, "GET /ap.json ")) {
					/* if we can get the mutex, write the last version of the AP list */
					ESP_LOGI(TAG,  "Processing ap.json request");
					if(wifi_manager_lock_json_buffer(( TickType_t ) 10)) {
						netconn_write(conn, http_ok_json_no_cache_hdr, sizeof(http_ok_json_no_cache_hdr) - 1, NETCONN_NOCOPY);
						char *buff = wifi_manager_alloc_get_ap_list_json();
						wifi_manager_unlock_json_buffer();
						if(buff!=NULL){
							netconn_write(conn, buff, strlen(buff), NETCONN_NOCOPY);
							free(buff);
						}
						else {
							ESP_LOGD(TAG,  "Error retrieving ap list json string. ");
							netconn_write(conn, http_503_hdr, sizeof(http_503_hdr) - 1, NETCONN_NOCOPY);
						}
					}
					else {
						netconn_write(conn, http_503_hdr, sizeof(http_503_hdr) - 1, NETCONN_NOCOPY);
						ESP_LOGE(TAG,   "http_server_netconn_serve: GET /ap.json failed to obtain mutex");
					}
					/* request a wifi scan */
					ESP_LOGI(TAG,  "Starting wifi scan");
					wifi_manager_scan_async();
					ESP_LOGI(TAG,  "Done serving ap.json");
				}
				else if(strstr(line, "GET /config.json ")) {
					ESP_LOGI(TAG,  "Serving config.json");
					ESP_LOGI(TAG,   "About to get config from flash");
					http_server_send_config_json(conn);
					ESP_LOGD(TAG,  "Done serving config.json");
				}
				else if(strstr(line, "POST /config.json ")) {
					ESP_LOGI(TAG,  "Serving POST config.json");
					int lenA=0;
					char * last_parm=save_ptr;
					char * next_parm=save_ptr;
					char  * last_parm_name=NULL;
					bool bErrorFound=false;
					bool bOTA=false;
					char * otaURL=NULL;
					// todo:  implement json body parsing
					//http_server_process_config(conn,save_ptr);

					while(last_parm!=NULL) {
						// Search will return
						ESP_LOGD(TAG,   "Getting parameters from X-Custom headers");
						last_parm = http_server_search_header(next_parm, "X-Custom-", &lenA, &last_parm_name,&next_parm,buf+buflen);
						if(last_parm!=NULL && last_parm_name!=NULL) {
							ESP_LOGI(TAG,   "http_server_netconn_serve: POST config.json, config %s=%s", last_parm_name, last_parm);
							if(strcmp(last_parm_name, "fwurl")==0) {
								// we're getting a request to do an OTA from that URL
								ESP_LOGW(TAG,   "Found OTA request!");
								otaURL=strdup(last_parm);
								bOTA=true;
							}
							else {
								ESP_LOGV(TAG,   "http_server_netconn_serve: POST config.json Storing parameter");
								if(config_set_value(NVS_TYPE_STR, last_parm_name , last_parm) != ESP_OK){
									ESP_LOGE(TAG,  "Unable to save nvs value.");
								}
							}
						}
						if(last_parm_name!=NULL) {
							free(last_parm_name);
							last_parm_name=NULL;
						}
					}
					if(bErrorFound) {
						netconn_write(conn, http_400_hdr, sizeof(http_400_hdr) - 1, NETCONN_NOCOPY); //400 invalid request
					}
					else {
						netconn_write(conn, http_ok_json_no_cache_hdr, sizeof(http_ok_json_no_cache_hdr) - 1, NETCONN_NOCOPY); //200ok
						if(bOTA) {

#if RECOVERY_APPLICATION
							ESP_LOGW(TAG,   "Starting process OTA for url %s",otaURL);
#else
							ESP_LOGW(TAG,   "Restarting system to process OTA for url %s",otaURL);
#endif
							wifi_manager_reboot_ota(otaURL);
							free(otaURL);
						}
					}
					ESP_LOGI(TAG,  "Done Serving POST config.json");
				} 
				else if(strstr(line, "POST /connect.json ")) {
					ESP_LOGI(TAG,   "http_server_netconn_serve: POST /connect.json");
					bool found = false;
					int lenS = 0, lenP = 0, lenN = 0;
					char *ssid = NULL, *password = NULL;
					ssid = http_server_get_header(save_ptr, "X-Custom-ssid: ", &lenS);
					password = http_server_get_header(save_ptr, "X-Custom-pwd: ", &lenP);
					char * new_host_name_b = http_server_get_header(save_ptr, "X-Custom-host_name: ", &lenN);
					if(lenN > 0){
						lenN++;
						char * new_host_name = malloc(lenN);
						strlcpy(new_host_name, new_host_name_b, lenN);
						if(config_set_value(NVS_TYPE_STR, "host_name", new_host_name) != ESP_OK){
							ESP_LOGE(TAG,  "Unable to save host name configuration");
						}
						free(new_host_name);
					}

					if(ssid && lenS <= MAX_SSID_SIZE && password && lenP <= MAX_PASSWORD_SIZE) {
						wifi_config_t* config = wifi_manager_get_wifi_sta_config();
						memset(config, 0x00, sizeof(wifi_config_t));
						memcpy(config->sta.ssid, ssid, lenS);
						memcpy(config->sta.password, password, lenP);
						ESP_LOGD(TAG,   "http_server_netconn_serve: wifi_manager_connect_async() call, with ssid: %s, password: %s", config->sta.ssid, config->sta.password);
						wifi_manager_connect_async();
						netconn_write(conn, http_ok_json_no_cache_hdr, sizeof(http_ok_json_no_cache_hdr) - 1, NETCONN_NOCOPY); //200ok
						found = true;
					}
					else{
						ESP_LOGE(TAG,  "SSID or Password invalid");
					}


					if(!found) {
						/* bad request the authentification header is not complete/not the correct format */
						netconn_write(conn, http_400_hdr, sizeof(http_400_hdr) - 1, NETCONN_NOCOPY);
						ESP_LOGE(TAG,   "bad request the authentification header is not complete/not the correct format");
					}

					ESP_LOGI(TAG,   "http_server_netconn_serve: done serving connect.json");
				}
				else if(strstr(line, "DELETE /connect.json ")) {
					ESP_LOGI(TAG,   "http_server_netconn_serve: DELETE /connect.json");
					/* request a disconnection from wifi and forget about it */
					wifi_manager_disconnect_async();
					netconn_write(conn, http_ok_json_no_cache_hdr, sizeof(http_ok_json_no_cache_hdr) - 1, NETCONN_NOCOPY); /* 200 ok */
					ESP_LOGI(TAG,   "http_server_netconn_serve: done serving DELETE /connect.json");
				}
				else if(strstr(line, "POST /reboot_ota.json ")) {
					ESP_LOGI(TAG,   "http_server_netconn_serve: POST reboot_ota.json");
					netconn_write(conn, http_ok_json_no_cache_hdr, sizeof(http_ok_json_no_cache_hdr) - 1, NETCONN_NOCOPY); /* 200 ok */
					wifi_manager_reboot(OTA);
					ESP_LOGI(TAG,   "http_server_netconn_serve: done serving POST reboot_ota.json");
				}
				else if(strstr(line, "POST /reboot.json ")) {
					ESP_LOGI(TAG,   "http_server_netconn_serve: POST reboot.json");
					netconn_write(conn, http_ok_json_no_cache_hdr, sizeof(http_ok_json_no_cache_hdr) - 1, NETCONN_NOCOPY); /* 200 ok */
					wifi_manager_reboot(RESTART);
					ESP_LOGI(TAG,   "http_server_netconn_serve: done serving POST reboot.json");
				}
				else if(strstr(line, "POST /recovery.json ")) {
					ESP_LOGI(TAG,   "http_server_netconn_serve: POST recovery.json");
					netconn_write(conn, http_ok_json_no_cache_hdr, sizeof(http_ok_json_no_cache_hdr) - 1, NETCONN_NOCOPY); /* 200 ok */
					wifi_manager_reboot(RECOVERY);
					ESP_LOGI(TAG,   "http_server_netconn_serve: done serving POST recovery.json");
				}
				else if(strstr(line, "GET /status.json ")) {
					ESP_LOGI(TAG,  "Serving status.json");
					if(wifi_manager_lock_json_buffer(( TickType_t ) 10)) {
						char *buff = wifi_manager_alloc_get_ip_info_json();
						wifi_manager_unlock_json_buffer();
						if(buff) {
							netconn_write(conn, http_ok_json_no_cache_hdr, sizeof(http_ok_json_no_cache_hdr) - 1, NETCONN_NOCOPY);
							netconn_write(conn, buff, strlen(buff), NETCONN_NOCOPY);
							free(buff);
						}
						else {
							netconn_write(conn, http_503_hdr, sizeof(http_503_hdr) - 1, NETCONN_NOCOPY);
						}

					}
					else {
						netconn_write(conn, http_503_hdr, sizeof(http_503_hdr) - 1, NETCONN_NOCOPY);
						ESP_LOGE(TAG,   "http_server_netconn_serve: GET /status failed to obtain mutex");
					}
					ESP_LOGI(TAG,  "Done Serving status.json");
				}
				else {
					netconn_write(conn, http_400_hdr, sizeof(http_400_hdr) - 1, NETCONN_NOCOPY);
					ESP_LOGE(TAG,   "bad request from host: %s, request %s",remote_address, line);
				}
			}
		}
		else {
			ESP_LOGE(TAG,   "URL not found processing for remote host : %s",remote_address);
			netconn_write(conn, http_404_hdr, sizeof(http_404_hdr) - 1, NETCONN_NOCOPY);
		}
		free(host);
		free(buf);
	}

	free(ap_ip_address);
	free(remote_address);
	netconn_close(conn);
	/* free the buffer */

}

bool http_server_lock_json_object(TickType_t xTicksToWait) {
	ESP_LOGD(TAG,  "Locking config json object");
	if(http_server_config_mutex) {
		if( xSemaphoreTake( http_server_config_mutex, xTicksToWait ) == pdTRUE ) {
			ESP_LOGV(TAG,  "config Json object locked!");
			return true;
		}
		else {
			ESP_LOGW(TAG,  "Semaphore take failed. Unable to lock config Json object mutex");
			return false;
		}
	}
	else {
		ESP_LOGW(TAG,  "Unable to lock config Json object mutex");
		return false;
	}

}

void http_server_unlock_json_object() {
	ESP_LOGD(TAG,  "Unlocking json buffer!");
	xSemaphoreGive( http_server_config_mutex );
}

void strreplace(char *src, char *str, char *rep)
{
    char *p = strstr(src, str);
    if(p)
    {
        int len = strlen(src)+strlen(rep)-strlen(str);
        char r[len];
        memset(r, 0, len);
        if( p >= src ) {
            strncpy(r, src, p-src);
            r[p-src]='\0';
            strncat(r, rep, strlen(rep));
            strncat(r, p+strlen(str), p+strlen(str)-src+strlen(src));
            strcpy(src, r);
            strreplace(p+strlen(rep), str, rep);
        }
    }
}

