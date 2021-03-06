/*
Copyright (c) 2013, Intel Corporation

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
THE POSSIBILITY OF SUCH DAMAGE.

David Navarro <david.navarro@intel.com>

*/


#include "liblwm2m.h"
#include "commandline.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>

#define MAX_PACKET_SIZE 128

static int g_quit = 0;

extern lwm2m_object_t * get_object_device();
extern lwm2m_object_t * get_object_firmware();
extern lwm2m_object_t * get_test_object();

static void prv_quit(char * buffer,
                     void * user_data)
{
    g_quit = 1;
}

void handle_sigint(int signum)
{
    g_quit = 2;
}

void print_usage(void)
{
    fprintf(stderr, "Usage: lwm2mclient\r\n");
    fprintf(stderr, "Launch a LWM2M client.\r\n\n");
}

typedef struct
{
    int                     sock;
    struct sockaddr_storage addr;
    size_t                  addrLen;
} connection_t;

static uint8_t prv_buffer_send(void * sessionH,
                               uint8_t * buffer,
                               size_t length,
                               void * userdata)
{
    ssize_t nbSent;
    size_t offset;
    connection_t * connP = (connection_t*) sessionH;

    offset = 0;
    while (offset != length)
    {
        nbSent = sendto(connP->sock, buffer + offset, length - offset, 0, (struct sockaddr *)&(connP->addr), connP->addrLen);
        if (nbSent == -1) return COAP_500_INTERNAL_SERVER_ERROR;
        offset += nbSent;
    }
    return COAP_NO_ERROR;
}

static connection_t * prv_newConnection(struct sockaddr * addr,
                                        size_t addrLen)
{
    connection_t * connP;

    connP = (connection_t *)malloc(sizeof(connection_t));
    if (connP != NULL)
    {
        memcpy(&(connP->addr), addr, addrLen);
        connP->addrLen = addrLen;
    }

    return connP;
}

static void prv_output_buffer(uint8_t * buffer,
                              int length)
{
    int i;
    uint8_t array[16];

    i = 0;
    while (i < length)
    {
        int j;
        fprintf(stderr, "  ");

        memcpy(array, buffer+i, 16);

        for (j = 0 ; j < 16 && i+j < length; j++)
        {
            fprintf(stderr, "%02X ", array[j]);
        }
        while (j < 16)
        {
            fprintf(stderr, "   ");
            j++;
        }
        fprintf(stderr, "  ");
        for (j = 0 ; j < 16 && i+j < length; j++)
        {
            if (isprint(array[j]))
                fprintf(stderr, "%c ", array[j]);
            else
                fprintf(stderr, ". ");
        }
        fprintf(stderr, "\n");

        i += 16;
    }
}

int get_socket()
{
    int s = -1;
    struct addrinfo hints;
    struct addrinfo *res;
    struct addrinfo *p;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = PF_INET6;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    getaddrinfo(NULL, "5683", &hints, &res);

    for(p = res ; p != NULL && s == -1 ; p = p->ai_next)
    {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s >= 0)
        {
            if (-1 == bind(s, p->ai_addr, p->ai_addrlen))
            {
                close(s);
                s = -1;
            }
        }
    }

    freeaddrinfo(res);

    return s;
}

static void prv_output_servers(char * buffer,
                               void * user_data)
{
    lwm2m_context_t * lwm2mH = (lwm2m_context_t *) user_data;
    lwm2m_server_t * targetP;

    targetP = lwm2mH->serverList;

    if (targetP == NULL)
    {
        fprintf(stdout, "No server.\r\n");
        return;
    }

    for (targetP = lwm2mH->serverList ; targetP != NULL ; targetP = targetP->next)
    {
        fprintf(stdout, "Server ID %d:\r\n", targetP->shortID);
        fprintf(stdout, "\tstatus: ");
        switch(targetP->status)
        {
        case STATE_UNKNOWN:
            fprintf(stdout, "UNKNOWN\r\n");
            break;
        case STATE_REG_PENDING:
            fprintf(stdout, "REGISTRATION PENDING\r\n");
            break;
        case STATE_REGISTERED:
            fprintf(stdout, "REGISTERED location: \"%s\"\r\n", targetP->location);
            break;
        }
        fprintf(stdout, "\r\n");
    }
}

static void prv_change(char * buffer,
                       void * user_data)
{
    lwm2m_context_t * lwm2mH = (lwm2m_context_t *) user_data;
    lwm2m_uri_t uri;
    int result;
    size_t length;

    if (buffer[0] == 0) goto syntax_error;

    length = 0;
    // remove white space
    while (length < strlen(buffer) && isspace(buffer[length]))
    {
        length++;
    }
    // find end of URI
    while (length < strlen(buffer) && !isspace(buffer[length]))
    {
        length++;
    }

    result = lwm2m_stringToUri(buffer, length, &uri);
    if (result == 0) goto syntax_error;

    buffer += length;
    while (buffer[0] != 0 && isspace(buffer[0])) buffer++;
    if (buffer[0] == 0)
    {
        lwm2m_resource_value_changed(lwm2mH, &uri);
    }
    else
    {
        int i;

        i = 0;
        while (i < lwm2mH->numObject)
        {
            if (uri.objectId == lwm2mH->objectList[i]->objID)
            {
                if (lwm2mH->objectList[i]->writeFunc != NULL)
                {
                    if (COAP_204_CHANGED == lwm2mH->objectList[i]->writeFunc(&uri,
                                                                             buffer, strlen(buffer),
                                                                             lwm2mH->objectList[i]))
                    {
                        lwm2m_resource_value_changed(lwm2mH, &uri);
                        return;
                    }
                }
                fprintf(stdout, "Failed to change value !");
                return;
            }
            i++;
        }

        fprintf(stdout, "Object not found !");
    }
    return;

syntax_error:
    fprintf(stdout, "Syntax error !");
}

static connection_t * prv_createConnection(char * host,
                                           uint16_t port)
{
    char portStr[6];
    struct addrinfo hints;
    struct addrinfo *servinfo = NULL;
    struct addrinfo *p;
    int sock;
    struct sockaddr *sa;
    socklen_t sl;
    connection_t * connP = NULL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    if (0 >= sprintf(portStr, "%hu", port)) return NULL;
    if (0 != getaddrinfo(host, portStr, &hints, &servinfo) || servinfo == NULL) return NULL;

    // we test the various addresses
    sock = -1;
    for(p = servinfo ; p != NULL && sock == -1 ; p = p->ai_next)
    {
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock >= 0)
        {
            sa = p->ai_addr;
            sl = p->ai_addrlen;
            if (-1 == connect(sock, p->ai_addr, p->ai_addrlen))
            {
                close(sock);
                sock = -1;
            }
        }
    }
    if (sock >= 0)
    {
        connP = prv_newConnection(sa, sl);
        close(sock);
    }

    return connP;
}


int main(int argc, char *argv[])
{
    int socket;
    int result;
    lwm2m_context_t * lwm2mH = NULL;
    lwm2m_object_t * objArray[3];
    lwm2m_security_t security;
    int i;
    connection_t * connP;

    /*
     * The function start by setting up the command line interface (which may or not be useful depending on your project)
     *
     * This is an array of commands describes as { name, description, long description, callback, userdata }.
     * The firsts tree are easy to understand, the callback is the function that will be called when this command is typed
     * and in the last one will be stored the lwm2m context (allowing access to the server settings and the objects).
     */
    command_desc_t commands[] =
    {
            {"list", "List known servers.", NULL, prv_output_servers, NULL},
            {"change", "Change the value of resource.", " change URI [DATA]\r\n"
                                                        "   URI: uri of the resource such as /3/0, /3/0/2\r\n"
                                                        "   DATA: (optional) new value\r\n", prv_change, NULL},
            {"quit", "Quit the client gracefully.", NULL, prv_quit, NULL},
            {"^C", "Quit the client abruptly (without sending a de-register message).", NULL, NULL, NULL},

            COMMAND_END_LIST
    };

    /*
     *This call an internal function that create an IPV6 socket on the port 5683.
     */
    socket = get_socket();
    if (socket < 0)
    {
        fprintf(stderr, "Failed to open socket: %d\r\n", errno);
        return -1;
    }

    /*
     * Now the main function fill an array with each object, this list will be later passed to liblwm2m.
     * Those functions are located in their respective object file.
     */
    objArray[0] = get_object_device();
    if (NULL == objArray[0])
    {
        fprintf(stderr, "Failed to create Device object\r\n");
        return -1;
    }

    objArray[1] = get_object_firmware();
    if (NULL == objArray[1])
    {
        fprintf(stderr, "Failed to create Firmware object\r\n");
        return -1;
    }

    objArray[2] = get_test_object();
    if (NULL == objArray[2])
    {
        fprintf(stderr, "Failed to create test object\r\n");
        return -1;
    }

    /*
     * The liblwm2m library is now initialized with the name of the client - which shall be unique for each client -
     * the number of objects we will be passing through, the object constructor array and the function that will be in
     * charge to send the buffer (containing the LWM2M packets) to the network
     */
    lwm2mH = lwm2m_init("testlwm2mclient", 3, objArray, prv_buffer_send, NULL);
    if (NULL == lwm2mH)
    {
        fprintf(stderr, "lwm2m_init() failed\r\n");
        return -1;
    }

    signal(SIGINT, handle_sigint);

    connP = prv_createConnection("::1", 5684);
    if (connP == NULL)
    {
        fprintf(stderr, "Connection creation failed.\r\n");
        return -1;
    }
    connP->sock = socket;

    memset(&security, 0, sizeof(lwm2m_security_t));

    /*
     * This function add a server to the lwm2m context by passing an identifier, an opaque connection handler and a security
     * context.
     * You can add as many server as your application need and there will be thereby allowed to interact with your object
     */
    result = lwm2m_add_server(lwm2mH, 123, (void *)connP, &security);
    if (result != 0)
    {
        fprintf(stderr, "lwm2m_add_server() failed: 0x%X\r\n", result);
        return -1;
    }

    /*
     * This function register your client to all the servers you added with the precedent one
     */
    result = lwm2m_register(lwm2mH);
    if (result != 0)
    {
        fprintf(stderr, "lwm2m_register() failed: 0x%X\r\n", result);
        return -1;
    }

    /*
     * As you now have your lwm2m context complete you can pass it as an argument to all the command line functions
     * precedently viewed (first point)
     */
    for (i = 0 ; commands[i].name != NULL ; i++)
    {
        commands[i].userData = (void *)lwm2mH;
    }
    fprintf(stdout, "> "); fflush(stdout);

    /*
     * We now enter in a while loop that will handle the communications from the server
     */
    while (0 == g_quit)
    {
        struct timeval tv;
        fd_set readfds;

        FD_ZERO(&readfds);
        FD_SET(socket, &readfds);
        FD_SET(STDIN_FILENO, &readfds);

        tv.tv_sec = 60;
        tv.tv_usec = 0;

        /*
         * This function does two things:
         *  - first it does the work needed by liblwm2m (eg. (re)sending some packets).
         *  - Secondly it adjust the timeout value (default 60s) depending on the state of the transaction
         *    (eg. retransmission) and the time between the next operation
         */
        result = lwm2m_step(lwm2mH, &tv);
        if (result != 0)
        {
            fprintf(stderr, "lwm2m_step() failed: 0x%X\r\n", result);
            return -1;
        }

        /*
         * This part will set up an interruption until an event happen on SDTIN or the socket until "tv" timed out (set
         * with the precedent function)
         */
        result = select(FD_SETSIZE, &readfds, NULL, NULL, &tv);

        if ( result < 0 )
        {
            if (errno != EINTR)
            {
              fprintf(stderr, "Error in select(): %d\r\n", errno);
            }
        }
        else if (result > 0)
        {
            uint8_t buffer[MAX_PACKET_SIZE];
            int numBytes;

            /*
             * If an event happen on the socket
             */
            if (FD_ISSET(socket, &readfds))
            {
                struct sockaddr_storage addr;
                socklen_t addrLen;

                addrLen = sizeof(addr);

                /*
                 * We retrieve the data received
                 */
                numBytes = recvfrom(socket, buffer, MAX_PACKET_SIZE, 0, (struct sockaddr *)&addr, &addrLen);

                if (numBytes == -1)
                {
                    fprintf(stderr, "Error in recvfrom(): %d\r\n", errno);
                }
                else
                {
                    char s[INET6_ADDRSTRLEN];

                    fprintf(stdout, "%d bytes received from [%s]:%hu\r\n",
                            numBytes,
                            inet_ntop(addr.ss_family,
                                      &(((struct sockaddr_in6*)&addr)->sin6_addr),
                                      s,
                                      INET6_ADDRSTRLEN),
                            ntohs(((struct sockaddr_in6*)&addr)->sin6_port));

                    /*
                     * Display it in the STDOUT
                     */
                    prv_output_buffer(buffer, numBytes);

                    if ((connP->addrLen == addrLen)
                     && (memcmp(&(connP->addr), &addr, addrLen) == 0))
                    {
                        /*
                         * Let liblwm2m respond to the query depending on the context
                         */
                        lwm2m_handle_packet(lwm2mH, buffer, numBytes, (void *)connP);
                    }
                }
            }

            /*
             * If the event happened on the SDTIN
             */
            else if (FD_ISSET(STDIN_FILENO, &readfds))
            {
                numBytes = read(STDIN_FILENO, buffer, MAX_PACKET_SIZE);

                if (numBytes > 1)
                {
                    buffer[numBytes - 1] = 0;

                    /*
                     * We call the corresponding callback of the typed command passing it the buffer for further arguments
                     */
                    handle_command(commands, buffer);
                }
                if (g_quit == 0)
                {
                    fprintf(stdout, "\r\n> ");
                    fflush(stdout);
                }
                else
                {
                    fprintf(stdout, "\r\n");
                }
            }
        }
    }

    /*
     * Finally when the loop is left smoothly - asked by user in the command line interface - we unregister our client from it
     */
    if (g_quit == 1)
    {
        lwm2m_close(lwm2mH);
    }
    close(socket);
    free(connP);

    return 0;
}
