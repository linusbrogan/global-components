/*
 * Copyright 2019, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * This software may be distributed and modified according to the terms of
 * the GNU General Public License version 2. Note that NO WARRANTY is provided.
 * See "LICENSE_GPLv2.txt" for details.
 *
 * @TAG(DATA61_GPL)
 */

#include <autoconf.h>

#include <camkes.h>
#include <camkes/dma.h>
#include <camkes/io.h>
#include <camkes/irq.h>

#include <platsupport/io.h>
#include <platsupport/irq.h>
#include <vka/vka.h>
#include <simple/simple.h>
#include <simple/simple_helpers.h>
#include <allocman/allocman.h>
#include <allocman/bootstrap.h>
#include <allocman/vka.h>
#include <sel4utils/vspace.h>
#include <ethdrivers/raw.h>
#include <ethdrivers/intel.h>
#include <sel4utils/sel4_zf_logif.h>

#include "ethdriver.h"
#include "plat.h"

#define RX_BUFS 256

#define CLIENT_RX_BUFS 128
#define CLIENT_TX_BUFS 128

#define BUF_SIZE 2048

#define BRK_VIRTUAL_SIZE 400000000

reservation_t muslc_brk_reservation;
void *muslc_brk_reservation_start;
vspace_t  *muslc_this_vspace;
static sel4utils_res_t muslc_brk_reservation_memory;
static allocman_t *allocman;
static char allocator_mempool[8388608];
static simple_t camkes_simple;
static vka_t vka;
static vspace_t vspace;
static sel4utils_alloc_data_t vspace_data;
static struct eth_driver eth_driver;

static ps_irq_ops_t irq_ops;

void camkes_make_simple(simple_t *simple);

typedef struct eth_buf {
    void *buf;
    uintptr_t phys;
} eth_buf_t;

typedef struct rx_frame {
    eth_buf_t *buf; // Clients share a pool of RX frames
    int len;
    int client;
} rx_frame_t;

typedef struct tx_frame {
    eth_buf_t buf; // Each client has a pool of TX frames
    int len;
    int client;
} tx_frame_t;

typedef struct client {
    /* this flag indicates whether we or not we need to notify the client
     * if new data is received. We only notify once the client observes
     * the last packet */
    int should_notify;

    /* keeps track of the head of the queue */
    int pending_rx_head;
    /* keeps track of the tail of the queue */
    int pending_rx_tail;
    /*
     * this is a cyclic queue of RX buffers pending to be read by a client,
     * the head represents the first buffer in the queue, and the tail the last
     */
    rx_frame_t pending_rx[CLIENT_RX_BUFS];

    /* keeps track of how many TX buffers are in use */
    int num_tx;
    /* the allocated TX buffers for the client */
    tx_frame_t tx_mem[CLIENT_TX_BUFS];
    /*
     * this represents the pool of buffers that can be used for TX,
     * this array is a sliding array in that num_tx acts a pointer to
     * separate between buffers that are in use and buffers that are
     * not in use. E.g. 'o' = free, 'x' = in use
     *  -------------------------------------
     *  | o | o | o | o | o | o | x | x | x |
     *  -------------------------------------
     *                          ^
     *                        num_tx
     */
    tx_frame_t *pending_tx[CLIENT_TX_BUFS];

    /* mac address for this client */
    uint8_t mac[6];

    /* Badge for this client */
    seL4_Word client_id;

    /* dataport for this client */
    void *dataport;
} client_t;

static int num_clients = 0;
static client_t *clients = NULL;

static int num_rx_bufs;
static eth_buf_t rx_bufs[RX_BUFS];
static eth_buf_t *rx_buf_pool[RX_BUFS];

static int done_init = 0;

/* Functions provided by the Ethdriver template */
void client_emit(unsigned int client_id);
unsigned int client_get_sender_id(void);
unsigned int client_num_badges(void);
unsigned int client_enumerate_badge(unsigned int i);
void *client_buf(unsigned int client_id);
void client_get_mac(unsigned int client_id, uint8_t *mac);

static void init_system(void)
{
    int error;

    /* Camkes adds nothing to our address space, so this array is empty */
    void *existing_frames[] = {
        NULL
    };
    camkes_make_simple(&camkes_simple);

    /* Initialize allocator */
    allocman = bootstrap_use_current_1level(
                   simple_get_cnode(&camkes_simple),
                   simple_get_cnode_size_bits(&camkes_simple),
                   simple_last_valid_cap(&camkes_simple) + 1,
                   BIT(simple_get_cnode_size_bits(&camkes_simple)),
                   sizeof(allocator_mempool), allocator_mempool
               );
    assert(allocman);
    error = allocman_add_simple_untypeds(allocman, &camkes_simple);
    allocman_make_vka(&vka, allocman);

    /* Initialize the vspace */
    error = sel4utils_bootstrap_vspace(&vspace, &vspace_data,
                                       simple_get_init_cap(&camkes_simple, seL4_CapInitThreadPD), &vka, NULL, NULL, existing_frames);
    assert(!error);

    sel4utils_reserve_range_no_alloc(&vspace, &muslc_brk_reservation_memory, BRK_VIRTUAL_SIZE, seL4_AllRights, 1,
                                     &muslc_brk_reservation_start);
    muslc_this_vspace = &vspace;
    muslc_brk_reservation = (reservation_t) {
        .res = &muslc_brk_reservation_memory
    };
}

static void eth_tx_complete(void *iface, void *cookie)
{
    tx_frame_t *buf = (tx_frame_t *)cookie;
    client_t *client = &clients[buf->client];
    client->pending_tx[client->num_tx] = buf;
    client->num_tx++;
}

static uintptr_t eth_allocate_rx_buf(void *iface, size_t buf_size, void **cookie)
{
    if (buf_size > BUF_SIZE) {
        return 0;
    }
    if (num_rx_bufs == 0) {
        return 0;
    }
    num_rx_bufs--;
    *cookie = rx_buf_pool[num_rx_bufs];
    return rx_buf_pool[num_rx_bufs]->phys;
}

static client_t *detect_client(void *buf, unsigned int len)
{
    if (len < 6) {
        return NULL;
    }
    for (int i = 0; i < num_clients; i++) {
        if (memcmp(clients[i].mac, buf, 6) == 0) {
            return &clients[i];
        }
    }
    return NULL;
}

static int is_broadcast(void *buf, unsigned int len)
{
    static uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    if (len < 6) {
        return 0;
    }
    if (memcmp(buf, broadcast, 6) == 0) {
        return 1;
    }
    return 0;
}

static int is_multicast(void *buf, unsigned int len)
{
    // the dest address is in the IP header (16 bytes in), which is located after the
    // ethernet header. the dest address itself is a standard 4byte IP address
    const int eth_header_len = 14;
    const int ip_hdr_dest_offset = 16;
    if (len < eth_header_len + ip_hdr_dest_offset + 4) {
        return 0;
    }
    // read out a copy of the IP address so that it is correctly aligned
    uint32_t addr;
    memcpy(&addr, ((uintptr_t)buf) + eth_header_len + ip_hdr_dest_offset, 4);
    // multicast addresses start with bit pattern 1110, which after accounting for
    // network byte ordering is 0xe0
    if ((addr & 0xf0) == 0xe0) {
        return 1;
    }
    return 0;
}

static void give_client_buf(client_t *client, eth_buf_t *buf, unsigned int len)
{
    client->pending_rx[client->pending_rx_head] = (rx_frame_t) {
        buf, len, 0
    };
    client->pending_rx_head = (client->pending_rx_head + 1) % CLIENT_RX_BUFS;
    if (client->should_notify) {
        client_emit(client->client_id);
        client->should_notify = 0;
    }
}

static void eth_rx_complete(void *iface, unsigned int num_bufs, void **cookies, unsigned int *lens)
{
    /* insert filtering here. currently everything just goes to one client */
    if (num_bufs != 1) {
        goto error;
    }
    eth_buf_t *curr_buf = cookies[0];
    client_t *client = detect_client(curr_buf->buf, lens[0]);
    if (!client) {
        if (is_broadcast(curr_buf->buf, lens[0]) || is_multicast(curr_buf->buf, lens[0])) {
            /* in a broadcast duplicate this buffer for every other client, we will fallthrough
             * to give the buffer to client 0 at the end */
            for (int i = 1; i < num_clients; i++) {
                client = &clients[i];
                if ((client->pending_rx_head + 1) % CLIENT_RX_BUFS != client->pending_rx_tail) {
                    void *cookie;
                    uintptr_t phys = eth_allocate_rx_buf(iface, lens[0], &cookie);
                    eth_buf_t *new_buf = cookie;
                    if (phys) {
                        memcpy(new_buf->buf, curr_buf->buf, lens[0]);
                        give_client_buf(client, new_buf, lens[0]);
                    }
                }
            }
        } else {
            goto error;
        }
        client = &clients[0];
    }
    if ((client->pending_rx_head + 1) % CLIENT_RX_BUFS == client->pending_rx_tail) {
        goto error;
    }
    give_client_buf(client, curr_buf, lens[0]);
    return;
error:
    /* abort and put all the bufs back */
    for (int i = 0; i < num_bufs; i++) {
        eth_buf_t *returned_buf = cookies[i];
        rx_buf_pool[num_rx_bufs] = returned_buf;
        num_rx_bufs++;
    }
}

static struct raw_iface_callbacks ethdriver_callbacks = {
    .tx_complete = eth_tx_complete,
    .rx_complete = eth_rx_complete,
    .allocate_rx_buf = eth_allocate_rx_buf
};

/** If eth frames have been received by the driver, copy a single frame from
 * the driver's buffer (rx_bufs), into the dataport of the caller of this
 * function.
 *
 * @param[out] len The size in bytes of the eth frame.
 * @return  If there are no frames available to be consumed, returns negative.
 *          If there was an error or the component hasn't been initialized yet,
 *          returns negative.
 *          If there was only one frame available to be consumed, returns 0.
 *          If there are other frames to be consumed even after the one that
 *          will be returned by the current invocation, returns 1 (i.e, "there
 *          is more data.").
 */
int client_rx(int *len)
{
    int UNUSED err;
    if (!done_init) {
        return -1;
    }
    int ret;
    int id = client_get_sender_id();
    client_t *client = NULL;
    for (int i = 0; i < num_clients; i++) {
        if (clients[i].client_id == id) {
            client = &clients[i];
        }
    }
    assert(client);
    void *packet = client->dataport;
    err = ethdriver_lock();
    if (client->pending_rx_head == client->pending_rx_tail) {
        client->should_notify = 1;
        err = ethdriver_unlock();
        return -1;
    }
    rx_frame_t rx = client->pending_rx[client->pending_rx_tail];
    client->pending_rx_tail = (client->pending_rx_tail + 1) % CLIENT_RX_BUFS;
    memcpy(packet, rx.buf->buf, rx.len);
    *len = rx.len;
    if (client->pending_rx_tail == client->pending_rx_head) {
        client->should_notify = 1;
        ret = 0;
    } else {
        ret = 1;
    }
    rx_buf_pool[num_rx_bufs] = rx.buf;
    num_rx_bufs++;
    err = ethdriver_unlock();
    return ret;
}

int client_tx(int len)
{
    int UNUSED error;
    if (!done_init) {
        return -1;
    }
    if (len > BUF_SIZE) {
        len = BUF_SIZE;
    }
    if (len < 12) {
        return -1;
    }
    int err = ETHIF_TX_COMPLETE;
    int id = client_get_sender_id();
    client_t *client = NULL;
    for (int i = 0; i < num_clients; i++) {
        if (clients[i].client_id == id) {
            client = &clients[i];
        }
    }
    assert(client);
    void *packet = client->dataport;
    error = ethdriver_lock();
    /* silently drop packets */
    if (client->num_tx != 0) {
        client->num_tx --;
        tx_frame_t *tx_buf = client->pending_tx[client->num_tx];
        /* copy the packet over */
        memcpy(tx_buf->buf.buf, packet, len);
        memcpy(tx_buf->buf.buf + 6, client->mac, 6);
        /* queue up transmit */
        err = eth_driver.i_fn.raw_tx(&eth_driver, 1, (uintptr_t *) & (tx_buf->buf.phys), (unsigned int *)&len, tx_buf);
        if (err != ETHIF_TX_ENQUEUED) {
            /* Free the internal tx buffer in case tx fails. Up to the client to retry the trasmission */
            client->num_tx++;
        }
    }
    error = ethdriver_unlock();

    return err;
}

void client_mac(uint8_t *b1, uint8_t *b2, uint8_t *b3, uint8_t *b4, uint8_t *b5, uint8_t *b6)
{
    int UNUSED error;
    int id = client_get_sender_id();
    client_t *client = NULL;
    for (int i = 0; i < num_clients; i++) {
        if (clients[i].client_id == id) {
            client = &clients[i];
        }
    }
    assert(client);
    assert(done_init);
    error = ethdriver_lock();
    *b1 = client->mac[0];
    *b2 = client->mac[1];
    *b3 = client->mac[2];
    *b4 = client->mac[3];
    *b5 = client->mac[4];
    *b6 = client->mac[5];
    error = ethdriver_unlock();
}

void eth_irq_handle(void *data, ps_irq_acknowledge_fn_t acknowledge_fn, void *ack_data)
{
    int error;

    error = ethdriver_lock();
    ZF_LOGF_IF(error, "Failed to obtain lock for Ethdriver");

    ps_irq_t *irq = data;

    if (irq && irq->type == PS_INTERRUPT) {
        /*
         * Sabre, ZYNQ doesn't care about the number being passed in,
         * however Beaglebone does
         */
        eth_driver.i_fn.raw_handleIRQ(&eth_driver, irq->irq.number);
    } else {
        /*
         * Other platforms which use different interrupt types
         * do not care about the interrupt number at the moment
         */
        eth_driver.i_fn.raw_handleIRQ(&eth_driver, 0);
    }

    error = acknowledge_fn(irq);
    ZF_LOGF_IF(error, "Failed to acknowledge IRQ");

    error = ethdriver_unlock();
    ZF_LOGF_IF(error, "Failed to release lock for Ethdriver");
}

void post_init(void)
{
    int error = 0;
    error = ethdriver_lock();
    /* initialize seL4 allocators and give us a half sane environment */
    init_system();

    ps_io_ops_t io_ops = {0};

    error = camkes_irq_ops(&irq_ops);
    ZF_LOGF_IF(error, "Failed to initialise IRQ ops");

    eth_driver.cb_cookie = NULL;
    eth_driver.i_cb = ethdriver_callbacks;

    /* initialise the driver */
    error = ethif_preinit(&vka, &camkes_simple, &vspace, &io_ops);
    ZF_LOGF_IF(error, "Failed to setup the initialisation of the ethernet interface");

    /* preallocate buffers */
    for (int i = 0; i < RX_BUFS; i++) {
        void *buf = ps_dma_alloc(&io_ops.dma_manager, BUF_SIZE, 4, 1, PS_MEM_NORMAL);
        assert(buf);
        memset(buf, 0, BUF_SIZE);
        uintptr_t phys = ps_dma_pin(&io_ops.dma_manager, buf, BUF_SIZE);
        rx_bufs[num_rx_bufs] = (eth_buf_t) {
            .buf = buf, .phys = phys
        };
        rx_buf_pool[num_rx_bufs] = &(rx_bufs[num_rx_bufs]);
        num_rx_bufs++;
    }
    num_clients = client_num_badges();
    clients = calloc(num_clients, sizeof(client_t));
    for (int client = 0; client < num_clients; client++) {
        clients[client].should_notify = 1;
        clients[client].client_id = client_enumerate_badge(client);
        clients[client].dataport = client_buf(clients[client].client_id);
        client_get_mac(clients[client].client_id, clients[client].mac);
        for (int i = 0; i < CLIENT_TX_BUFS; i++) {
            void *buf = ps_dma_alloc(&io_ops.dma_manager, BUF_SIZE, 4, 1, PS_MEM_NORMAL);
            assert(buf);
            memset(buf, 0, BUF_SIZE);
            uintptr_t phys = ps_dma_pin(&io_ops.dma_manager, buf, BUF_SIZE);
            tx_frame_t *tx_buf = &clients[client].tx_mem[clients[client].num_tx];
            *tx_buf = (tx_frame_t) {
                .len = BUF_SIZE, .client = client
            };
            tx_buf->buf = (eth_buf_t) {
                .buf = buf, .phys = phys
            };
            clients[client].pending_tx[clients[client].num_tx] = tx_buf;
            clients[client].num_tx++;
        }
    }

    error = ethif_init(&eth_driver, &io_ops, &irq_ops);
    ZF_LOGF_IF(error, "Failed to initialise the ethernet device");

    done_init = 1;

    error = ethdriver_unlock();
}
