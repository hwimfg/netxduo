/* This test concentrates on DTLS connections.  */

#include   "nx_api.h"
#include   "nx_secure_dtls_api.h"
#include   "test_ca_cert.c"
#include   "test_device_cert.c"

extern VOID    test_control_return(UINT status);


#if !defined(NX_SECURE_TLS_CLIENT_DISABLED) && !defined(NX_SECURE_TLS_SERVER_DISABLED) && defined(NX_SECURE_ENABLE_DTLS)
#define NUM_PACKETS                 24
#define PACKET_SIZE                 1536
#define PACKET_POOL_SIZE            (NUM_PACKETS * (PACKET_SIZE + sizeof(NX_PACKET)))
#define THREAD_STACK_SIZE           1024
#define ARP_CACHE_SIZE              1024
#define BUFFER_SIZE                 64
#define METADATA_SIZE               16000
#define CERT_BUFFER_SIZE            (2048 + sizeof(NX_SECURE_X509_CERT))
#define PSK                         "simple_psk"
#define PSK_IDENTITY                "psk_indentity"
#define PSK_HINT                    "psk_hint"
#define SERVER_PORT                 4433

/* Number of DTLS sessions to apply to DTLS server. */
#define NUM_SERVER_SESSIONS         2


/* Define the ThreadX and NetX object control blocks...  */

static TX_THREAD                server_thread;
static NX_PACKET_POOL           server_pool;
static NX_IP                    server_ip;
static TX_THREAD                client_thread[2];
static NX_PACKET_POOL           client_pool[2];
static NX_IP                    client_ip[2];
static UINT                     error_counter;

static NX_UDP_SOCKET            client_socket[2];
static NX_SECURE_DTLS_SESSION   dtls_client_session[2];
static NX_SECURE_X509_CERT      client_trusted_ca[2];
static NX_SECURE_DTLS_SERVER    dtls_server;
static NX_SECURE_X509_CERT      server_local_certificate;
extern const NX_SECURE_TLS_CRYPTO
                                nx_crypto_tls_ciphers;

static ULONG                    server_pool_memory[PACKET_POOL_SIZE / sizeof(ULONG)];
static ULONG                    client_pool_memory[2][PACKET_POOL_SIZE / sizeof(ULONG)];
static ULONG                    server_thread_stack[THREAD_STACK_SIZE / sizeof(ULONG)];
static ULONG                    client_thread_stack[2][THREAD_STACK_SIZE / sizeof(ULONG)];
static ULONG                    server_ip_stack[THREAD_STACK_SIZE / sizeof(ULONG)];
static ULONG                    client_ip_stack[2][THREAD_STACK_SIZE / sizeof(ULONG)];
static ULONG                    server_arp_cache[ARP_CACHE_SIZE];
static ULONG                    client_arp_cache[2][ARP_CACHE_SIZE];
static UCHAR                    server_metadata[METADATA_SIZE * NUM_SERVER_SESSIONS];
static UCHAR                    client_metadata[2][METADATA_SIZE];
static UCHAR                    client_cert_buffer[2][CERT_BUFFER_SIZE];

static UCHAR                    request_buffer[2][BUFFER_SIZE];
static UCHAR                    response_buffer[BUFFER_SIZE];
static UCHAR                    server_tls_packet_buffer[4000 * NUM_SERVER_SESSIONS];
static UCHAR                    client_tls_packet_buffer[2][4000];

/* Session buffer for DTLS server. Must be equal to the size of NX_SECURE_DTLS_SESSION times the
   number of desired DTLS sessions. */
static UCHAR                    server_session_buffer[sizeof(NX_SECURE_DTLS_SESSION) * NUM_SERVER_SESSIONS];

/* Define thread prototypes.  */

static VOID    server_thread_entry(ULONG thread_input);
static VOID    client_thread_0_entry(ULONG thread_input);
static VOID    client_thread_1_entry(ULONG thread_input);
extern VOID    _nx_ram_network_driver_1500(struct NX_IP_DRIVER_STRUCT *driver_req);

static TX_SEMAPHORE            semaphore_connect;
static TX_SEMAPHORE            semaphore_receive;


/* Define what the initial system looks like.  */

#define ERROR_COUNTER() __ERROR_COUNTER(__FILE__, __LINE__)

static VOID    __ERROR_COUNTER(UCHAR *file, UINT line)
{
    printf("Error on line %d in %s\n", line, file);
    error_counter++;
}

#ifdef CTEST
void test_application_define(void *first_unused_memory);
void test_application_define(void *first_unused_memory)
#else
VOID    nx_secure_dtls_multiple_sessions_retransmit_test_application_define(void *first_unused_memory)
#endif
{
UINT     status;
CHAR    *pointer;


    error_counter = 0;


    /* Setup the working pointer.  */
    pointer =  (CHAR *) first_unused_memory;

    /* Create the server thread.  */
    tx_thread_create(&server_thread, "server thread", server_thread_entry, 0,
                     server_thread_stack, sizeof(server_thread_stack),
                     7, 7, TX_NO_TIME_SLICE, TX_AUTO_START);

    /* Create the client thread.  */
    tx_thread_create(&client_thread[0], "client thread 0", client_thread_0_entry, 0,
                     client_thread_stack[0], sizeof(client_thread_stack[0]),
                     8, 8, TX_NO_TIME_SLICE, TX_AUTO_START);
    tx_thread_create(&client_thread[1], "client thread 1", client_thread_1_entry, 0,
                     client_thread_stack[1], sizeof(client_thread_stack[1]),
                     8, 8, TX_NO_TIME_SLICE, TX_AUTO_START);

    tx_semaphore_create(&semaphore_connect, "semaphore connect", 0);
    tx_semaphore_create(&semaphore_receive, "semaphore receive", 0);
    
    /* Initialize the NetX system.  */
    nx_system_initialize();

    /* Create a packet pool.  */
    status =  nx_packet_pool_create(&server_pool, "Server Packet Pool", PACKET_SIZE,
                                    server_pool_memory, PACKET_POOL_SIZE);
    if (status)
    {
        ERROR_COUNTER();
    }
    status =  nx_packet_pool_create(&client_pool[0], "Client 0 Packet Pool", PACKET_SIZE,
                                    client_pool_memory[0], PACKET_POOL_SIZE);
    if (status)
    {
        ERROR_COUNTER();
    }
    status =  nx_packet_pool_create(&client_pool[1], "Client 1 Packet Pool", PACKET_SIZE,
                                    client_pool_memory[1], PACKET_POOL_SIZE);
    if (status)
    {
        ERROR_COUNTER();
    }

    /* Create an IP instance.  */
    status = nx_ip_create(&server_ip, "Server IP Instance", IP_ADDRESS(1, 2, 3, 4), 0xFFFFFF00UL,
                          &server_pool, _nx_ram_network_driver_1500,
                          server_ip_stack, sizeof(server_ip_stack), 1);
    if (status)
    {
        ERROR_COUNTER();
    }
    status = nx_ip_create(&client_ip[0], "Client 0 IP Instance", IP_ADDRESS(1, 2, 3, 5), 0xFFFFFF00UL,
                          &client_pool[0], _nx_ram_network_driver_1500,
                          client_ip_stack[0], sizeof(client_ip_stack[0]), 1);
    if (status)
    {
        ERROR_COUNTER();
    }
    status = nx_ip_create(&client_ip[1], "Client 1 IP Instance", IP_ADDRESS(1, 2, 3, 6), 0xFFFFFF00UL,
                          &client_pool[1], _nx_ram_network_driver_1500,
                          client_ip_stack[1], sizeof(client_ip_stack[1]), 1);
    if (status)
    {
        ERROR_COUNTER();
    }

    /* Enable ARP and supply ARP cache memory for IP Instance 0.  */
    status =  nx_arp_enable(&server_ip, (VOID *)server_arp_cache, sizeof(server_arp_cache));
    if (status)
    {
        ERROR_COUNTER();
    }
    status =  nx_arp_enable(&client_ip[0], (VOID *)client_arp_cache[0], sizeof(client_arp_cache[0]));
    if (status)
    {
        ERROR_COUNTER();
    }
    status =  nx_arp_enable(&client_ip[1], (VOID *)client_arp_cache[1], sizeof(client_arp_cache[1]));
    if (status)
    {
        ERROR_COUNTER();
    }

    /* Enable UDP traffic.  */
    status =  nx_udp_enable(&server_ip);
    if (status)
    {
        ERROR_COUNTER();
    }
    status =  nx_udp_enable(&client_ip[0]);
    if (status)
    {
        ERROR_COUNTER();
    }
    status =  nx_udp_enable(&client_ip[1]);
    if (status)
    {
        ERROR_COUNTER();
    }

    nx_secure_tls_initialize();
    nx_secure_dtls_initialize();
}

static VOID client_dtls_setup(NX_SECURE_DTLS_SESSION *dtls_session_ptr, UINT index)
{
UINT status;

    status = nx_secure_dtls_session_create(dtls_session_ptr,
                                           &nx_crypto_tls_ciphers,
                                           client_metadata[index],
                                           sizeof(client_metadata[index]),
                                           client_tls_packet_buffer[index], sizeof(client_tls_packet_buffer[index]),
                                           1, client_cert_buffer[index], sizeof(client_cert_buffer[index]));
    if (status)
    {
        ERROR_COUNTER();
    }

    status = nx_secure_x509_certificate_initialize(&client_trusted_ca[index],
                                                   test_ca_cert_der,
                                                   test_ca_cert_der_len, NX_NULL, 0, NULL, 0, NX_SECURE_X509_KEY_TYPE_NONE);
    if (status)
    {
        ERROR_COUNTER();
    }

    status = nx_secure_dtls_session_trusted_certificate_add(dtls_session_ptr, &client_trusted_ca[index], 1);
    if (status)
    {
        ERROR_COUNTER();
    }

#if defined(NX_SECURE_ENABLE_PSK_CIPHERSUITES) || defined(NX_SECURE_ENABLE_ECJPAKE_CIPHERSUITE)
    /* For PSK ciphersuites, add a PSK and identity hint.  */
    nx_secure_dtls_psk_add(dtls_session_ptr, PSK, strlen(PSK),
                         PSK_IDENTITY, strlen(PSK_IDENTITY), PSK_HINT, strlen(PSK_HINT));
#endif
}


/* Notification flags for DTLS server connect/receive. */
static UINT server_connect_count = 0;
static UINT server_receive_count = 0;
static NX_SECURE_DTLS_SESSION *connect_session[8];
static NX_SECURE_DTLS_SESSION *receive_session[8];

/* Connect notify callback for DTLS server - notifies the application thread that
   a DTLS connection is ready to kickoff a handshake. */
static UINT server_connect_notify(NX_SECURE_DTLS_SESSION *dtls_session, NXD_ADDRESS *ip_address, UINT port)
{
    connect_session[server_connect_count  & 0x07] = dtls_session;
    server_connect_count++;
    tx_semaphore_put(&semaphore_connect);
    return(NX_SUCCESS);
}

/* Receive notify callback for DTLS server - notifies the application thread that
   we have received a DTLS record over an established DTLS session. */
static UINT server_receive_notify(NX_SECURE_DTLS_SESSION *dtls_session)
{
    receive_session[server_receive_count & 0x07] = dtls_session;
    server_receive_count++;
    tx_semaphore_put(&semaphore_receive);
    return(NX_SUCCESS);
}


static VOID server_dtls_setup(NX_SECURE_DTLS_SERVER *dtls_server_ptr)
{
UINT status;

    status = nx_secure_dtls_server_create(dtls_server_ptr, &server_ip, SERVER_PORT, NX_IP_PERIODIC_RATE,
                                          server_session_buffer, sizeof(server_session_buffer),
                                          &nx_crypto_tls_ciphers, server_metadata, sizeof(server_metadata),
                                          server_tls_packet_buffer, sizeof(server_tls_packet_buffer),
                                          server_connect_notify, server_receive_notify);
    if (status)
    {
        ERROR_COUNTER();
    }

    memset(&server_local_certificate, 0, sizeof(server_local_certificate));
    status = nx_secure_x509_certificate_initialize(&server_local_certificate,
                                                   test_device_cert_der, test_device_cert_der_len,
                                                   NX_NULL, 0, test_device_cert_key_der,
                                                   test_device_cert_key_der_len, NX_SECURE_X509_KEY_TYPE_RSA_PKCS1_DER);
    if (status)
    {
        ERROR_COUNTER();
    }

    status = nx_secure_dtls_server_local_certificate_add(dtls_server_ptr, &server_local_certificate, 1);
    if (status)
    {
        ERROR_COUNTER();
    }
}

static UINT packet_drop_list[][5] =
{
    {NX_SECURE_TLS_CLIENT_HELLO, NX_SECURE_TLS_CLIENT_KEY_EXCHANGE, NX_SECURE_TLS_CLIENT_HELLO,},
    {NX_SECURE_TLS_HELLO_VERIFY_REQUEST, NX_SECURE_TLS_SERVER_HELLO,},
    {NX_SECURE_TLS_HELLO_VERIFY_REQUEST, NX_SECURE_TLS_SERVER_HELLO,},
};

static UINT packet_drop_num[3] = {3, 2, 2};

static UINT packet_drop_count[3] = {0};

extern VOID _nx_udp_packet_receive(NX_IP *ip_ptr, NX_PACKET *packet_ptr);
static VOID test_udp_packet_receive(NX_IP *ip_ptr, NX_PACKET *packet_ptr)
{

UCHAR *data;
UINT *drop_count;
UINT *drop_list;
UINT drop_num;
UINT index;

    data = packet_ptr -> nx_packet_prepend_ptr;

    if (ip_ptr == &server_ip)
    {
        index = 0;
    }
    else if (ip_ptr == &client_ip[0])
    {
        index = 1;
    }
    else
    {
        index = 2;
    }

    drop_list = packet_drop_list[index];
    drop_count = &packet_drop_count[index];
    drop_num = packet_drop_num[index];

    if (*drop_count < drop_num)
    {

        if ((data[8] == NX_SECURE_TLS_HANDSHAKE) && (data[21] == drop_list[*drop_count]))
        {
            (*drop_count)++;
            nx_packet_release(packet_ptr);
            return;
        }
    }
    
    _nx_udp_packet_receive(ip_ptr, packet_ptr);
}

static void server_thread_entry(ULONG thread_input)
{
UINT status;
INT  i, j;
ULONG response_length;
NX_PACKET *packet_ptr;

    /* Print out test information banner.  */
    printf("NetX Secure Test:   DTLS Multiple Sessions Retransmit Test.............");

    server_dtls_setup(&dtls_server);

    server_ip.nx_ip_udp_packet_receive = test_udp_packet_receive;

    /* Start DTLS server - we have multiple sessions that will start below. */
    status = nx_secure_dtls_server_start(&dtls_server);
    if (status)
    {
        printf("Error in starting DTLS server: 0x%02X\n", status);
        ERROR_COUNTER();
    }

    for (i = 0; i < NUM_SERVER_SESSIONS; i++)
    {
        /* Wait for connection attempt. */
        tx_semaphore_get(&semaphore_connect, NX_IP_PERIODIC_RATE);

        status = nx_secure_dtls_server_session_start(connect_session[i], NX_WAIT_FOREVER);

        if (status)
        {
            printf("Error in establishing DTLS server session at index %d: 0x%02X\n", i, status);
            ERROR_COUNTER();
        }
    }


    /* Receive the records.  */
    for (i = 0; i < NUM_SERVER_SESSIONS; i++)
    {

        /* Wait for records to be received. */
        tx_semaphore_get(&semaphore_receive, NX_IP_PERIODIC_RATE);

        status = nx_secure_dtls_session_receive(receive_session[i], &packet_ptr, NX_IP_PERIODIC_RATE);
        if(status == NX_SECURE_TLS_CLOSE_NOTIFY_RECEIVED)
        {
            /* Session ended. Close this seerver. */
            status = nx_secure_dtls_session_end(receive_session[i], NX_WAIT_FOREVER);
            if(status)
            {
                ERROR_COUNTER();
            }
        }
        else if (status)
        {
            printf("Error in DTLS server session receive at index %d: 0x%02X\n", i, status);
            ERROR_COUNTER();
        }        
        else
        {
            /* Look for matching IP addresses - use this to determine the buffers to use. */
            for (j = 0; j < NUM_SERVER_SESSIONS; j++)
            {
                if (receive_session[i] -> nx_secure_dtls_remote_ip_address.nxd_ip_address.v4 == client_ip[j].nx_ip_interface[0].nx_interface_ip_address)
                {
                    break;
                }
            }

            memset(response_buffer, 0, sizeof(response_buffer));
            nx_packet_data_retrieve(packet_ptr, response_buffer, &response_length);
            nx_packet_release(packet_ptr);

            if ((response_length != sizeof(request_buffer[j])) ||
                memcmp(request_buffer[j], response_buffer, response_length))
            {
                printf("Received data did not match expected in DTLS Server: %s\n", __FILE__);
                ERROR_COUNTER();
            }
        }
    }

    for (i = 0; i < NUM_SERVER_SESSIONS; i++)
    {
        if(connect_session[i] -> nx_secure_dtls_tls_session.nx_secure_tls_local_session_active)
        {
            status = nx_secure_dtls_session_end(connect_session[i], NX_WAIT_FOREVER);
            if(status)
            {
                ERROR_COUNTER();
            }
        }
    }

    tx_thread_resume(&client_thread[0]);
    tx_thread_resume(&client_thread[1]);

    /* Shutdown DTLS server. */
    nx_secure_dtls_server_stop(&dtls_server);

    /* Delete server. */
    nx_secure_dtls_server_delete(&dtls_server);

    if (error_counter)
    {
        printf("ERROR!\n");
        test_control_return(1);
    }
    else
    {
        printf("SUCCESS!\n");
        test_control_return(0);
    }
}

static void client_thread_0_entry(ULONG thread_input)
{
UINT i;
UINT status;
NX_PACKET *packet_ptr;
NXD_ADDRESS server_address;

    for (i = 0; i < sizeof(request_buffer[0]); i++)
    {
        request_buffer[0][i] = i;
    }

    server_address.nxd_ip_version = NX_IP_VERSION_V4;
    server_address.nxd_ip_address.v4 = IP_ADDRESS(1, 2, 3, 4);

    client_ip[0].nx_ip_udp_packet_receive = test_udp_packet_receive;

    /* Create UDP socket. */
    status = nx_udp_socket_create(&client_ip[0], &client_socket[0], "Client socket 0", NX_IP_NORMAL,
                                  NX_DONT_FRAGMENT, 0x80, 5);
    if (status)
    {
        ERROR_COUNTER();
    }

    status = nx_udp_socket_bind(&client_socket[0], NX_ANY_PORT, NX_NO_WAIT);
    if (status)
    {
        ERROR_COUNTER();
    }

    client_dtls_setup(&dtls_client_session[0], 0);

    /* Start DTLS session. */
    status = nx_secure_dtls_client_session_start(&dtls_client_session[0], &client_socket[0], &server_address, SERVER_PORT, NX_WAIT_FOREVER);
    if (status)
    {
        printf("Error in starting client session 0 with error %x\n", status);
        ERROR_COUNTER();
    }

    /* Prepare packet to send. */
    status = nx_packet_allocate(&client_pool[0], &packet_ptr, NX_UDP_PACKET, NX_NO_WAIT);
    if (status)
    {
        ERROR_COUNTER();
    }

    packet_ptr -> nx_packet_prepend_ptr += NX_SECURE_DTLS_RECORD_HEADER_SIZE;
    packet_ptr -> nx_packet_append_ptr = packet_ptr -> nx_packet_prepend_ptr;

    status = nx_packet_data_append(packet_ptr, request_buffer[0], sizeof(request_buffer[0]),
                                   &client_pool[0], NX_NO_WAIT);
    if (status)
    {
        ERROR_COUNTER();
    }

    /* Send the packet. */
    status = nx_secure_dtls_client_session_send(&dtls_client_session[0], packet_ptr);
    if (status)
    {
        ERROR_COUNTER();
    }


    status = nx_secure_dtls_session_end(&dtls_client_session[0], NX_WAIT_FOREVER);
    if(status)
    {
        ERROR_COUNTER();
    }

    tx_thread_suspend(&client_thread[0]);

    nx_secure_dtls_session_delete(&dtls_client_session[0]);

    nx_udp_socket_unbind(&client_socket[0]);

    nx_udp_socket_delete(&client_socket[0]);
}

static void client_thread_1_entry(ULONG thread_input)
{
UINT i;
UINT status;
NX_PACKET *packet_ptr;
NXD_ADDRESS server_address;

    for (i = 0; i < sizeof(request_buffer[1]); i++)
    {
        request_buffer[1][i] = (i << 1);
    }

    server_address.nxd_ip_version = NX_IP_VERSION_V4;
    server_address.nxd_ip_address.v4 = IP_ADDRESS(1, 2, 3, 4);

    client_ip[1].nx_ip_udp_packet_receive = test_udp_packet_receive;

    /* Create UDP socket. */
    status = nx_udp_socket_create(&client_ip[1], &client_socket[1], "Client socket 1", NX_IP_NORMAL,
                                  NX_DONT_FRAGMENT, 0x80, 5);
    if (status)
    {
        ERROR_COUNTER();
    }

    status = nx_udp_socket_bind(&client_socket[1], NX_ANY_PORT, NX_NO_WAIT);
    if (status)
    {
        ERROR_COUNTER();
    }

    client_dtls_setup(&dtls_client_session[1], 1);

    /* Start DTLS session. */
    status = nx_secure_dtls_client_session_start(&dtls_client_session[1], &client_socket[1], &server_address, SERVER_PORT, NX_WAIT_FOREVER);
    if (status)
    {
        printf("Error in starting client session 1 with error %x\n", status);
        ERROR_COUNTER();
    }

    /* Prepare packet to send. */
    status = nx_packet_allocate(&client_pool[1], &packet_ptr, NX_UDP_PACKET, NX_NO_WAIT);
    if (status)
    {
        ERROR_COUNTER();
    }

    packet_ptr -> nx_packet_prepend_ptr += NX_SECURE_DTLS_RECORD_HEADER_SIZE;
    packet_ptr -> nx_packet_append_ptr = packet_ptr -> nx_packet_prepend_ptr;

    status = nx_packet_data_append(packet_ptr, request_buffer[1], sizeof(request_buffer[1]),
                                   &client_pool[1], NX_NO_WAIT);
    if (status)
    {
        ERROR_COUNTER();
    }

    /* Send the packet. */
    status = nx_secure_dtls_client_session_send(&dtls_client_session[1], packet_ptr);
    if (status)
    {
        ERROR_COUNTER();
    }


    status = nx_secure_dtls_session_end(&dtls_client_session[1], NX_WAIT_FOREVER);
    if(status)
    {
        ERROR_COUNTER();
    }

    tx_thread_suspend(&client_thread[1]);

    nx_secure_dtls_session_delete(&dtls_client_session[1]);

    nx_udp_socket_unbind(&client_socket[1]);

    nx_udp_socket_delete(&client_socket[1]);
}
#else
#ifdef CTEST
void test_application_define(void *first_unused_memory);
void test_application_define(void *first_unused_memory)
#else
VOID    nx_secure_dtls_multiple_sessions_retransmit_test_application_define(void *first_unused_memory)
#endif
{

    /* Print out test information banner.  */
    printf("NetX Secure Test:   DTLS Multiple Sessions Retransmit Test.............N/A\n");
    test_control_return(3);
}
#endif
