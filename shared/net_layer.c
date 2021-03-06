#include "thread.h"
#include "limits.h"
#include "mutex.h"
#include "irq.h"
#include "utlist.h"
#include "byteorder.h"
#include "crc.h"
#include "net_layer.h"
#include "string.h"
#include "portail.h"

/**
 * UDP checksums are not calculated. It's not possible as IP packets are
 * terminated in the xport and the checksum is based on IP headers.
 * Checksum is checked at IP termination, then all other transports
 * (UART and radio) have their own CRC checks.
 */


typedef struct radio_pkt_t
{
    uint8_t users;                  ///< number of threads still processing it
    struct radio_pkt_t *rcv_queue_next;
    uint8_t rssi;                   ///< Radio Signal Strength Indication
    radio_packet_length_t length;   ///< Length of payload
    uint8_t *data;                  ///< Payload
}
radio_pkt_t;


#define MAX_SOCKETS (3)

typedef struct
{
    int id;
    bool blocking;
    uint type;
    uint port;
    kernel_pid_t dest_pid;
    radio_pkt_t *rcv_queue;
}
sock_t;

static sock_t sockets[MAX_SOCKETS];
static uint socket_nextid = 0;
static uint next_rand_port = UINT_MAX;

static struct
{
  uint radio_buffer_full;
  uint radio_send_fail;
  uint unhandled_packet;
  uint out_of_buffer;
}
stats;

/**
 * cc110x drivers and transceiver are so tightly coupled that transceiver.h
 * and radio/types.h cannot be avoided.
 */

#define RCV_BUF_SIZE (3)
/**
 * all three are linked and accessed using the same index `rcv_buffer_pos`.
 * freedom determined using `users` property of netl_rcv_t
 */
static radio_pkt_t    pkt_rcv_buffer[RCV_BUF_SIZE];
static uint8_t        data_buffer[RCV_BUF_SIZE * CC1100_MAX_DATA_LENGTH];
// static netl_rcv_msg_t rvc_msg_buffer[RCV_BUF_SIZE];
static volatile uint8_t rcv_buffer_pos = 0;

kernel_pid_t net_layer_pid = KERNEL_PID_UNDEF;


#define ENABLE_DEBUG    (0)
#include "debug.h"

static sock_t *new_socket(void)
{
    for (int i = 0; i < MAX_SOCKETS; ++i)
    {
        if (sockets[i].id == SOCK_UNDEF)
        {
            sock_t *sock = &sockets[i];
            sock->id = socket_nextid++;
            sock->blocking = true;
            sock->rcv_queue = NULL;
            return sock;
        }
    }
    return NULL;
}

static sock_t *get_socket(int id)
{
    for (int i = 0; i < MAX_SOCKETS; ++i)
    {
        if (sockets[i].id == id)
        {
            return &sockets[i];
        }
    }
    return NULL;
}

static sock_t *get_socket_port(uint port)
{
    for (int i = 0; i < MAX_SOCKETS; ++i)
    {
        if (sockets[i].port == port)
        {
            return &sockets[i];
        }
    }
    return NULL;
}

static uint get_rand_port(void)
{
    if (next_rand_port < UINT_MAX/2)
    {
        next_rand_port = UINT_MAX;
    }
    return next_rand_port--;
}

static bool sock_is_bound(sock_t *sock)
{
    return sock->port && sock->dest_pid != KERNEL_PID_UNDEF;
}

static void rcv_queue_add(sock_t *sock, radio_pkt_t *pkt)
{
    LL_APPEND2(sock->rcv_queue, pkt, rcv_queue_next);
}

static radio_pkt_t *rcv_queue_pop(sock_t *sock)
{
    unsigned irq_state = disableIRQ();

    radio_pkt_t *pkt = sock->rcv_queue;
    if (pkt != NULL)
    {
        LL_DELETE2(sock->rcv_queue, pkt, rcv_queue_next);
    }

    restoreIRQ(irq_state);

    return pkt;
}

int netl_socket(uint type)
{
    sock_t *sock = new_socket();
    if (sock != NULL)
    {
        sock->type = type;
        return sock->id;
    }
    return -1;
}

/* for now capture all udp ports to caller */
uint8_t netl_bind(int s, uint port)
{
    sock_t *sock = get_socket(s);

    if (sock == NULL) {
        return -1;
    }

    sock_t *existing = get_socket_port(port);

    if (existing != NULL)
    {
        if (sock->id != existing->id)
        {
            return -1;
        }
    }
    else {
        /* bind! */
        sock->port = port;
        sock->dest_pid = thread_getpid();
    }
    return 0;
}

uint8_t netl_unbind(int s)
{
    sock_t *sock = get_socket(s);

    if (sock == NULL) {
        return -1;
    }

    if (sock_is_bound(sock))
    {
        sock->port = 0;
        sock->dest_pid = KERNEL_PID_UNDEF;
    }
    return 0;
}

int netl_close(int s)
{
    sock_t *sock = get_socket(s);

    if (sock == NULL) {
        return -1;
    }

    sock->id = SOCK_UNDEF;
    netl_unbind(s);

    return 0;
}

int netl_recv(int s, uint8_t *dest, uint maxlen, frominfo_t *info)
{
    sock_t *sock = get_socket(s);

    if (sock == NULL) {
        return -1;
    }

    radio_pkt_t *pkt = rcv_queue_pop(sock);

    if (pkt == NULL)
    {
        if (sock->blocking)
        {
            msg_t m;

            while (1) {
                msg_receive(&m);

                if (m.type != NETL_RCV_MSG_TYPE) {
                    continue;
                }

                if (sock->id != m.content.value) {
                    continue;
                }

                pkt = rcv_queue_pop(sock);
                break;
             }
        }
        else {
            return 0;
        }
    }

    udp_hdr_t *udph = (udp_hdr_t *) pkt->data;
    char *data;
    uint length;

    if (sock->type == SOCK_RAW)
    {
        length = pkt->length;
        data = (char *) pkt->data;
    }
    else {
        length = udph->length;
        data = (char *) udph + sizeof(udp_hdr_t);
    }

    if (length <= maxlen)
    {
        memcpy(dest, data, length);
        if (info != NULL) {
            info->src_port = udph->src_port;
            info->rssi = pkt->rssi;
        }
        return length;
    }
    pkt->users--;

    return 0;
}

static uint8_t _netl_send(sock_t *sock, uint8_t *data, uint8_t size)
{
    static cc110x_packet_t cc110x_pkt;

    cc110x_pkt.length = size + CC1100_HEADER_LENGTH;
    cc110x_pkt.address = CC1100_BROADCAST_ADDRESS;
    cc110x_pkt.flags = 0;
    memcpy(cc110x_pkt.data, data, size);

    int8_t sent_len = cc110x_send(&cc110x_pkt);

    if (sent_len <= 0) {
        stats.radio_send_fail += 1;
    }

    return sent_len;
}

static char send_to_buf[PORTAIL_MAX_DATA_SIZE];
static mutex_t send_to_buf_mutex = MUTEX_INIT;

int netl_send_to(int s, uint8_t *data, uint8_t size, uint dstport)
{
    sock_t *sock = get_socket(s);

    if (sock == NULL) {
        return -1;
    }

    if (sizeof(send_to_buf) - sizeof(udp_hdr_t) < size)
    {
        DEBUG("netl_send_to(): data too large\n");
        return -1;
    }

    /* concurrent call should be rare, rather save space with just one buf slot */
    mutex_lock(&send_to_buf_mutex);
    udp_hdr_t *udph = (udp_hdr_t *) send_to_buf;
    udph->dst_port = HTONS(dstport);
    if (sock_is_bound(sock)) {
        udph->src_port = HTONS(sock->port);
    } else {
        udph->src_port = HTONS(get_rand_port());
    }
    // write crc
    udph->length = HTONS(size);
    memcpy((char *) udph + sizeof(udp_hdr_t), data, size);

    int ret = _netl_send(sock, (uint8_t *) udph, size + sizeof(udp_hdr_t));

    mutex_unlock(&send_to_buf_mutex);

    return ret;
}

int netl_send(int s, uint8_t *data, uint8_t size)
{
    sock_t *sock = get_socket(s);

    if (sock == NULL) {
        return -1;
    }

    if (PORTAIL_MAX_DATA_SIZE < size)
    {
        DEBUG("netl_send_to(): data too large\n");
        return -1;
    }

    return _netl_send(sock, data, size);
}

int netl_set_nonblock(int s)
{
    sock_t *sock = get_socket(s);

    if (sock == NULL) {
        return -1;
    }

    sock->blocking = false;

    return 0;
}

/**
 * @return  0 or -1 on out of buffer
 */
static int8_t incr_rcv_buffer_pos(void)
{
    uint8_t i;
    for (i = 0;
         i < RCV_BUF_SIZE && pkt_rcv_buffer[rcv_buffer_pos].users;
         i++)
    {
        if (++rcv_buffer_pos == RCV_BUF_SIZE) {
            rcv_buffer_pos = 0;
        }
    }
    if (i >= RCV_BUF_SIZE) {
        return -1;
    }
    return 0;
}

static void handle_radio_packet(radio_pkt_t *pkt)
{
    msg_t m;

    bool pkt_handled = false;
    udp_hdr_t *udph = (udp_hdr_t *) pkt->data;

    /* for all sockets */
    for (int i = 0; i < MAX_SOCKETS; ++i)
    {
        sock_t *sock = &sockets[i];

        /* if has a pid registered */
        if (sock->dest_pid != KERNEL_PID_UNDEF)
        {
            bool has_matching_port = sock->port && sock->port == NTOHS(udph->dst_port);
            bool has_no_port_but_is_raw = !sock->port && sock->type == SOCK_RAW;

            /* if socket should receive the msg */
            if (has_matching_port || has_no_port_but_is_raw)
            {
                m.type = NETL_RCV_MSG_TYPE;
                m.content.value = sock->id;

                int ret = msg_try_send(&m, sock->dest_pid);
                if (ret == 1) {
                    /* successfully delivered, netl_recv will have to decr this */

                    pkt->users += 1;
                    rcv_queue_add(sock, pkt);
                    pkt_handled = true;
                }
            }
        }
    }
    if (!pkt_handled)
    {
        DEBUG("net_layer: radio packet dropped");
        stats.unhandled_packet += 1;
    }
}

static void receive_cc110x_packet(radio_pkt_t *trans_p, uint8_t rx_buffer_pos)
{
    DEBUG("net_layer: Handling CC1100 packet\n");
    /* disable interrupts while copying packet */
    dINT();
    cc110x_packet_t *p = &cc110x_rx_buffer[rx_buffer_pos].packet;

    trans_p->rssi = cc110x_rx_buffer[rx_buffer_pos].rssi;
    trans_p->length = p->length - CC1100_HEADER_LENGTH;
    trans_p->data = &data_buffer[rcv_buffer_pos * CC1100_MAX_DATA_LENGTH];
    memcpy(trans_p->data, p->data, CC1100_MAX_DATA_LENGTH);
    eINT();

    DEBUG("net_layer: Packet %p (%p) was from %hu to %hu, size: %u\n", trans_p, trans_p->data, trans_p->src, trans_p->dst, trans_p->length);
}

#define MSG_BUFFER_SIZE (8)
static msg_t msg_buffer[MSG_BUFFER_SIZE];

static void *rx_thread(void *arg)
{
    msg_t m;

    msg_init_queue(msg_buffer, MSG_BUFFER_SIZE);

    while(1)
    {
        msg_receive(&m);

        if (m.type == CC1100_PKT_RCV_MSG_TYPE)
        {
            int err = incr_rcv_buffer_pos();
            if (!err)
            {
                uint8_t pos = m.content.value;
                radio_pkt_t *pkt = &pkt_rcv_buffer[rcv_buffer_pos];

                receive_cc110x_packet(pkt, pos);
                handle_radio_packet(pkt);
            }
            else {
                DEBUG("net_layer: receive buffer full, packet dropped");
                stats.radio_buffer_full += 1;
            }
        }
    }
    UNREACHABLE();
}

static char stack_buffer[THREAD_STACKSIZE_DEFAULT];
static bool did_init = false;

void netl_init(void)
{
    if (!did_init)
    {
        /* initialize all socket IDs to null value */
        for (int i = 0; i < MAX_SOCKETS; ++i)
        {
            sockets[i].id = SOCK_UNDEF;
        }
        did_init = true;
    }

    if (net_layer_pid == KERNEL_PID_UNDEF)
    {
        net_layer_pid = thread_create(
          stack_buffer,
          sizeof(stack_buffer),
          THREAD_PRIORITY_MAIN - 2,
          0,
          rx_thread,
          NULL,
          "net_layer"
        );
    }

    cc110x_init(net_layer_pid);
}