#include <time.h>
#include <math.h>
#include <signal.h>
#include <assert.h> 
#include <sys/mman.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <inttypes.h>
#include "time_stat.h"
#include "agent.h"
# include "binarytree_bench.h"

struct time_stats *timer;

int isClient = 0;
int n_hash_req = 0;
int psync = 0;

#define OFFLOAD_COUNT 5000
static pthread_t offload_thread;
#define TREE_SIZE 10

// Pass -redn as arg to use RedN
int REDN = 0;
int ONE_SIDED = 1;

#define SHM_PATH "/ifbw_shm"
#define SHM_F_SIZE 128

int batch_size = 1;	//default - batching disabled
int sge_count = 1;	//default - 1 scatter/gather element
int use_cas = 0;	//default - compare_and_swap disabled

char *portno = "12345";
char *client_portno = "11111";
char *server_portno = "22222";

char *intf = "ib0";

int master_sock = 0;
int client_sock = 2;
int worker_sock = 3;

volatile sig_atomic_t stop = 0;

char host[NI_MAXHOST];

enum region_type {
	MR_DATA = 0,
	MR_BUFFER,
	MR_COUNT
};

enum sock_type {
	SOCK_MASTER = 2,
	SOCK_CLIENT,
	SOCK_WORKER
};

struct mr_context regions[MR_COUNT];

uint64_t mr_sizes[MR_COUNT] = {268265456UL, 268265456UL};


#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

#define IS_ALIGNED(x, a) (((x) & ((__typeof__(x))(a) - 1)) == 0)

#ifdef __cplusplus
	#define ALIGN(x, a)  ALIGN_MASK((x), ((__typeof__(x))(a) - 1))
#else
	#define ALIGN(x, a)  ALIGN_MASK((x), ((typeof(x))(a) - 1))
#endif

#define ALIGN_MASK(x, mask)	(((x) + (mask)) & ~(mask))

#if __BIG_ENDIAN__
    #define htonll(x)   (x)
    #define ntohll(x)   (x)
#else
    #define htonll(x)   ((((uint64_t)htonl(x&0xFFFFFFFF)) << 32) + htonl(x >> 32))
    #define ntohll(x)   ((((uint64_t)ntohl(x&0xFFFFFFFF)) << 32) + ntohl(x >> 32))
#endif

struct wqe_ctrl_seg *sr0_ctrl[TREE_SIZE] = {NULL};
struct mlx5_wqe_raddr_seg * sr0_raddr[TREE_SIZE] = {NULL};
struct mlx5_wqe_data_seg * sr0_data[TREE_SIZE*3] = { NULL };
struct wqe_ctrl_seg *sr1_ctrl[TREE_SIZE] = {NULL};
struct mlx5_wqe_data_seg * sr1_data[TREE_SIZE] = {NULL};
struct mlx5_wqe_raddr_seg * sr1_raddr[TREE_SIZE] = {NULL};
struct mlx5_wqe_atomic_seg * sr1_atomic[TREE_SIZE] = {NULL};
struct wqe_ctrl_seg *sr2_ctrl[TREE_SIZE] = {NULL};
struct mlx5_wqe_data_seg * sr2_data[TREE_SIZE] = {NULL};
struct mlx5_wqe_raddr_seg * sr2_raddr[TREE_SIZE] = {NULL};
int sr0_wrid[TREE_SIZE], sr1_wrid[TREE_SIZE], sr2_wrid[TREE_SIZE];

struct timespec start;

void add_peer_socket(int sockfd) {
	int sock_type = rc_connection_meta(sockfd);

	printf("ADDING PEER SOCKET %d (type: %d)\n", sockfd, sock_type);

	if(isClient || sock_type != SOCK_CLIENT) { //XXX do not post receives on master socket
		for(int i=0; i<100; i++) {
			IBV_RECEIVE_IMM(sockfd);
		}
		return;
	}

	int worker = add_connection(host, portno, 0, 1, IBV_EXP_QP_CREATE_MANAGED_SEND);

	printf("Setting sockfds [client: %d worker: %d]\n", sockfd, worker);
	
	return;
}

void remove_peer_socket(int sockfd) {
	;
}

void print_seg_data() {
	if(sr0_data && sr0_raddr) {
		printf("------ AFTER ------\n");
		for(int j=0; j<TREE_SIZE; j++) {
			printf("sr0_data[0]: addr %lu length %u\n", be64toh(sr0_data[j+0]->addr), ntohl(sr0_data[j+0]->byte_count));
			printf("sr0_data[1]: addr %lu length %u\n", be64toh(sr0_data[j+1]->addr), ntohl(sr0_data[j+1]->byte_count));
			printf("sr0_data[2]: addr %lu length %u\n", be64toh(sr0_data[j+2]->addr), ntohl(sr0_data[j+2]->byte_count));
			printf("sr0_raddr: raddr %lu\n", ntohll(sr0_raddr[j]->raddr));
			printf("sr1_atomic: compare %lx (original: %lx) swap_add %lx (original: %lx)\n",
					be64toh(sr1_atomic[j]->compare), sr1_atomic[j]->compare, be64toh(sr1_atomic[j]->swap_add), sr1_atomic[j]->swap_add);
			printf("sr1_raddr: raddr %lu\n", ntohll(sr1_raddr[j]->raddr));

			uint32_t sr2_meta = ntohl(sr2_ctrl[j]->opmod_idx_opcode);
			uint16_t idx2 =  ((sr2_meta >> 8) & (UINT_MAX));
			uint8_t opmod2 = ((sr2_meta >> 24) & (UINT_MAX));
			uint8_t opcode2 = (sr2_meta & USHRT_MAX);

			printf("&sr2_ctrl->opmod_idx_opcode %lu\n", (uintptr_t)&sr2_ctrl[j]->opmod_idx_opcode);
			printf("sr2_ctrl: raw %lx idx %u opmod %u opcode %u qpn_ds %x fm_ce_se %x sig %u (imm %u)\n",
					*((uint64_t *)&sr2_ctrl[j]->opmod_idx_opcode), idx2, opmod2, opcode2, ntohl(sr2_ctrl[j]->qpn_ds),
					ntohl(sr2_ctrl[j]->fm_ce_se), sr2_ctrl[j]->signature, ntohl(sr2_ctrl[j]->imm));
			printf("sr2_data: addr %lu length %u\n", be64toh(sr2_data[j]->addr), ntohl(sr2_data[j]->byte_count));
			printf("*sr2_data->addr = %lu\n", *((uint64_t *)be64toh(sr2_data[j]->addr)));
			printf("sr2_raddr: raddr %lu\n", be64toh(sr2_raddr[j]->raddr));
		}
	}
}

void test_callback(struct app_context *msg) {
	if(!isClient) {	

		//printf("posting receive imm\n");
		int sock_type = rc_connection_meta(msg->sockfd);

		//XXX do not post receives on master lock socket
		if(sock_type != SOCK_CLIENT)
			IBV_RECEIVE_IMM(msg->sockfd);

		if(sock_type == SOCK_CLIENT)
			n_hash_req++;

		print_seg_data();
	}
	printf("Received response with id %d\n", msg->id);
}

uint32_t post_dummy_write(int sockfd, int iosize, int imm) {
	// send response
	int src_mr = 0;
	int dst_mr = 0;

	uint64_t remote_base_addr = mr_remote_addr(sockfd, dst_mr);
	uint64_t local_base_addr = mr_local_addr(sockfd, src_mr);
	rdma_meta_t *meta =  (rdma_meta_t *) malloc(sizeof(rdma_meta_t)
			+ 1 * sizeof(struct ibv_sge));

	meta->addr = remote_base_addr;
	meta->length = iosize;
	meta->sge_count = 1;
	meta->sge_entries[0].addr = local_base_addr;
	meta->sge_entries[0].length = iosize;
	meta->imm = imm;
	meta->next = NULL;

	if(imm)
		return IBV_WRAPPER_RDMA_WRITE_WITH_IMM_ASYNC(sockfd, meta, src_mr, dst_mr);
	else
		return IBV_WRAPPER_RDMA_WRITE_ASYNC(sockfd, meta, src_mr, dst_mr);
}

uint32_t post_binarytree_read(int sockfd, uint32_t lkey1, uint32_t lkey2, uint32_t rkey) {
	int src_mr = 0;
	int dst_mr = 0;
	int signaled = 1;

	uint64_t remote_base_addr = mr_remote_addr(sockfd, dst_mr);
	uint64_t local_base_addr = mr_local_addr(sockfd, src_mr);

	struct ibv_send_wr *wr = malloc(sizeof(struct ibv_send_wr));
	memset (wr, 0, sizeof (struct ibv_send_wr));

	struct ibv_sge *sge = malloc(sizeof(struct ibv_sge) * 3);
	memset(sge, 0, sizeof(struct ibv_sge) * 3);

	sge[0].addr = local_base_addr;
	sge[0].length = 3;
	sge[0].lkey = lkey2;
	sge[1].addr = local_base_addr;
	sge[1].length = 8;
	sge[1].lkey = lkey2;
	sge[2].addr = local_base_addr;
	sge[2].length = 8;
	sge[2].lkey = lkey1;

	/* prepare the send work request */
	wr->next = NULL;
	wr->wr_id = IBV_NEXT_WR_ID(sockfd);
	wr->sg_list = sge;
	wr->num_sge = 3;
	wr->wr.rdma.remote_addr = remote_base_addr;
	wr->wr.rdma.rkey = rkey;

	wr->opcode = IBV_WR_RDMA_READ;

	if(signaled)
		wr->send_flags = IBV_SEND_SIGNALED;

	return IBV_POST_ASYNC(sockfd, wr);
}

void * offload_binarytree(void *iters) {
	int count = *((int *)iters);
	int master = master_sock;
	int client = client_sock;
	int worker = worker_sock;

	uint64_t base_data_addr = mr_local_addr(worker, MR_DATA);
	uint64_t base_buffer_addr = mr_local_addr(worker, MR_BUFFER);

	printf("performing binary tree offload [client: %d worker: %d]\n", client, worker);

	int count_1 = 11;
	int count_2 = 4;

	for(int k=0; k<count; k++) {
		if(k == 0)
			IBV_WAIT_TILL(worker, client, 4);
		else
			IBV_WAIT_EXPLICIT(worker, client, 1);

		IBV_TRIGGER_EXPLICIT(worker, worker, count_1);

		for(int j=0; j<TREE_SIZE; j++) {
			printf("remote start: %lu end: %lu\n", mr_remote_addr(worker, MR_DATA), mr_remote_addr(worker, MR_DATA) + mr_sizes[MR_DATA]);
			sr0_wrid[j] = post_binarytree_read(worker, mr_local_key(worker, mr_get_sq_idx(worker)),
					mr_local_key(client, mr_get_sq_idx(client)), mr_remote_key(worker, MR_DATA));

			sr1_wrid[j] = IBV_CAS_ASYNC(worker, base_buffer_addr, base_buffer_addr, 0, 1, mr_remote_key(master, MR_BUFFER), mr_local_key(client, mr_get_sq_idx(client)), 1);

			if(k == 0 && j == 0)
				IBV_WAIT_TILL(worker, worker, 3);
			else {
				IBV_WAIT_EXPLICIT(worker, worker, 2);
			}

			IBV_TRIGGER_EXPLICIT(worker, client, count_2);

			if(j < TREE_SIZE - 2)
				IBV_TRIGGER_EXPLICIT(worker, worker, count_1+6);
			else if(j == TREE_SIZE - 2) {
				if(k == count - 1)
					IBV_TRIGGER_EXPLICIT(worker, worker, count_1+5);
				else
					IBV_TRIGGER_EXPLICIT(worker, worker, count_1+6);
			}

			printf("j %d TREE_SIZE %d k %d count %d\n", j, TREE_SIZE, k, count);
			if(j == TREE_SIZE - 1 && k < count - 1) {
				count_1 += 2;
				IBV_TRIGGER_EXPLICIT(worker, worker, count_1);
			}

			sr2_wrid[j] = post_dummy_write(client, 8, k + j + 1);

			// find READ WR
			sr0_ctrl[j] = IBV_FIND_WQE(worker, sr0_wrid[j]);

			if(!sr0_ctrl[j]) {
				printf("Failed to find sr1 seg\n");
				pause();
			}

			uint32_t sr0_meta = ntohl(sr0_ctrl[j]->opmod_idx_opcode);
			uint16_t idx0 =  ((sr0_meta >> 8) & (UINT_MAX));
			uint8_t opmod0 = ((sr0_meta >> 24) & (UINT_MAX));
			uint8_t opcode0 = (sr0_meta & USHRT_MAX);

			printf("sr0 (READ) segment will be posted to idx #%u\n", idx0);


			// find CAS WR
			sr1_ctrl[j] = IBV_FIND_WQE(worker, sr1_wrid[j]);

			if(!sr1_ctrl[j]) {
				printf("Failed to find sr0 seg\n");
				pause();
			}

			uint32_t sr1_meta = ntohl(sr1_ctrl[j]->opmod_idx_opcode);
			uint16_t idx1 =  ((sr1_meta >> 8) & (UINT_MAX));
			uint8_t opmod1 = ((sr1_meta >> 24) & (UINT_MAX));
			uint8_t opcode1 = (sr1_meta & USHRT_MAX);

			printf("sr1 (CAS) segment will be posted to idx #%u\n", idx1);

			// find WRITE WR
			sr2_ctrl[j] = IBV_FIND_WQE(client, sr2_wrid[j]);

			if(!sr2_ctrl[j]) {
				printf("Failed to find sr2 seg\n");
				pause();
			}

			uint32_t sr2_meta = ntohl(sr2_ctrl[j]->opmod_idx_opcode);
			uint16_t idx2 =  ((sr2_meta >> 8) & (UINT_MAX));
			uint8_t opmod2 = ((sr2_meta >> 24) & (UINT_MAX));
			uint8_t opcode2 = (sr2_meta & USHRT_MAX);

			printf("sr2 (WRITE) segment will be posted to idx #%u\n", idx2);

			void *seg0 = ((void*)sr0_ctrl[j]) + sizeof(struct mlx5_wqe_ctrl_seg) + sizeof(struct mlx5_wqe_raddr_seg);

			// need to modify 3 sges
			for(int i=0; i<3; i++) {
				sr0_data[j*3+i] = (struct mlx5_wqe_data_seg *) (seg0 + i * sizeof(struct mlx5_wqe_data_seg));
			}

			seg0 = ((void*)sr0_ctrl[j]) + sizeof(struct mlx5_wqe_ctrl_seg);

			sr0_raddr[j] = (struct mlx5_wqe_raddr_seg *) seg0;


			void *seg1 = ((void*)sr1_ctrl[j]) + sizeof(struct mlx5_wqe_ctrl_seg) +
				sizeof(struct mlx5_wqe_atomic_seg) + sizeof(struct mlx5_wqe_raddr_seg);

			sr1_data[j] = (struct mlx5_wqe_data_seg *) seg1;

			seg1 = ((void*)sr1_ctrl[j]) + sizeof(struct mlx5_wqe_ctrl_seg);

			sr1_raddr[j] = (struct mlx5_wqe_raddr_seg *) seg1;

			seg1 = ((void*)sr1_ctrl[j]) + sizeof(struct mlx5_wqe_ctrl_seg) + sizeof(struct mlx5_wqe_raddr_seg);

			sr1_atomic[j] = (struct mlx5_wqe_atomic_seg *) seg1; 


			void *seg2 = ((void*)sr2_ctrl[j]) + sizeof(struct mlx5_wqe_ctrl_seg);

			sr2_raddr[j] = (struct mlx5_wqe_raddr_seg *) seg2;

			seg2 = ((void*)sr2_ctrl[j]) + sizeof(struct mlx5_wqe_ctrl_seg) + sizeof(struct mlx5_wqe_raddr_seg);

			sr2_data[j] = (struct mlx5_wqe_data_seg *) seg2;

			sr0_data[j*3+0]->addr = htobe64(((uintptr_t) (&sr2_ctrl[j]->qpn_ds)));
			sr0_data[j*3+1]->addr = htobe64(((uintptr_t) (&sr2_data[j]->addr)));

			if(j>0)
				sr0_data[(j-1)*3+2]->addr = htobe64(((uintptr_t) (&sr0_raddr[j]->raddr)));

			//XXX point last READ SG to somehwere unused. for now let it modify itself
			if(j == TREE_SIZE-1)
				sr0_data[j*3+2]->addr = htobe64(((uintptr_t) (&sr0_raddr[j]->raddr))); 

			//XXX might increase latency
			//sr_ctrl[j]->fm_ce_se = htonl(0);
			//sr0_ctrl[j]->fm_ce_se = htonl(0);
			sr2_ctrl[j]->fm_ce_se = htonl(0);
			sr2_ctrl[j]->opmod_idx_opcode = sr2_ctrl[j]->opmod_idx_opcode | 0x09000000; //SEND

			sr1_atomic[j]->swap_add =  htobe64(*((uint64_t *)&sr2_ctrl[j]->opmod_idx_opcode));

			sr2_ctrl[j]->qpn_ds = htonl((0 << 8) | 3);

			sr2_ctrl[j]->opmod_idx_opcode = sr2_ctrl[j]->opmod_idx_opcode & 0x00FFFFFF; //NOOP

			sr2_ctrl[j]->imm = htonl(k+1);

			sr1_raddr[j]->raddr = htobe64((uintptr_t) &sr2_ctrl[j]->opmod_idx_opcode);
			sr1_atomic[j]->compare = htobe64(*((uint64_t *)&sr2_ctrl[j]->opmod_idx_opcode));

			printf("------ BEFORE ------\n");
			printf("sr0_data[0]: addr %lu length %u\n", be64toh(sr0_data[j+0]->addr), ntohl(sr0_data[j+0]->byte_count));
			printf("sr0_data[1]: addr %lu length %u\n", be64toh(sr0_data[j+1]->addr), ntohl(sr0_data[j+1]->byte_count));
			printf("sr0_data[2]: addr %lu length %u\n", be64toh(sr0_data[j+2]->addr), ntohl(sr0_data[j+2]->byte_count));
			printf("sr0_raddr: raddr %lu\n", ntohll(sr0_raddr[j]->raddr));
			printf("sr1_atomic: compare %lx (original: %lx) swap_add %lx (original: %lx)\n",
					be64toh(sr1_atomic[j]->compare), sr1_atomic[j]->compare, be64toh(sr1_atomic[j]->swap_add), sr1_atomic[j]->swap_add);
			printf("sr1_raddr: raddr %lu\n", ntohll(sr1_raddr[j]->raddr));

			sr2_meta = ntohl(sr2_ctrl[j]->opmod_idx_opcode);
			idx2 =  ((sr2_meta >> 8) & (UINT_MAX));
			opmod2 = ((sr2_meta >> 24) & (UINT_MAX));
			opcode2 = (sr2_meta & USHRT_MAX);

			printf("&sr2_ctrl->opmod_idx_opcode %lu\n", (uintptr_t)&sr2_ctrl[j]->opmod_idx_opcode);
			printf("sr2_ctrl: raw %lx idx %u opmod %u opcode %u qpn_ds %x fm_ce_se %x sig %u (imm %u)\n", *((uint64_t *)&sr2_ctrl[j]->opmod_idx_opcode), idx2, opmod2, opcode2, ntohl(sr2_ctrl[j]->qpn_ds), ntohl(sr2_ctrl[j]->fm_ce_se), sr2_ctrl[j]->signature, ntohl(sr2_ctrl[j]->imm));
			printf("sr2_data: addr %lu length %u\n", be64toh(sr2_data[j]->addr), ntohl(sr2_data[j]->byte_count));
			printf("*sr2_data->addr = %lu\n", *((uint64_t *)be64toh(sr2_data[j]->addr)));
			printf("sr2_raddr: raddr %lu\n", be64toh(sr2_raddr[j]->raddr));

			count_1 += 6;
			count_2 += 1;

			if(k == 0)
				IBV_TRIGGER(master, worker, 5); // trigger first two wrs


			assert(TREE_SIZE <= 16);

			// set up RECV for client inputs
			struct rdma_metadata *recv_meta =  (struct rdma_metadata *)
				calloc(1, sizeof(struct rdma_metadata) + TREE_SIZE * sizeof(struct ibv_sge));

			for(int l=0; l<TREE_SIZE; l++) {
				recv_meta->sge_entries[l].addr = ((uintptr_t)&sr1_atomic[l]->compare)+1;
				recv_meta->sge_entries[l].length = 3;
			}
			recv_meta->length = 3*TREE_SIZE;
			recv_meta->sge_count = TREE_SIZE;

			IBV_RECEIVE_SG(client, recv_meta, mr_local_key(worker, mr_get_sq_idx(worker)));
		}

		// rate limit
		while(k - n_hash_req > 100)
			ibw_cpu_relax();
	}
}

void init_binarytree(addr_t addr) {
	printf("---- Initializing binarytree ----\n");

	struct bt_bucket *bucket = (struct bt_bucket*)addr;

	printf("bucket addr %lu\n", addr);
	for(int i=0; i<TREE_SIZE; i++) {
		bucket[i].key[0] = i + 1000;
		bucket[i].key[1] = 0;
		bucket[i].key[2] = 0;
		bucket[i].addr = htobe64((uintptr_t) &bucket[i].value[0]);
		bucket[i].value[0] = 5555 + i;

		if(i > 0) {
			if(i%2 == 0)
				bucket[(i-1)/2].right = htobe64((uintptr_t) &bucket[i]);
			else
				bucket[(i-1)/2].left = htobe64((uintptr_t) &bucket[i]);
		}

		printf("bucket[%d] key=%u addr=%lu\n", i, *((uint32_t *)bucket[i].key), be64toh(bucket[i].addr));
	}
}

uint32_t post_read(int sockfd, uint64_t src, uint64_t dst, int iosize, int src_mr, int dst_mr) {
	uint64_t remote_base_addr = mr_remote_addr(sockfd, dst_mr);
	uint64_t local_base_addr = mr_local_addr(sockfd, src_mr);
	rdma_meta_t *meta =  (rdma_meta_t *) malloc(sizeof(rdma_meta_t)
			+ 1 * sizeof(struct ibv_sge));

	meta->addr = dst;
	meta->length = iosize;
	meta->sge_count = 1;
	meta->sge_entries[0].addr = src;
	meta->sge_entries[0].length = iosize;
	meta->next = NULL;

	printf("reading from dst %lu to src %lu\n", dst, src);

	return IBV_WRAPPER_RDMA_READ_ASYNC(sockfd, meta, src_mr, dst_mr);
}

uint32_t post_get_req_async(int sockfd, uint32_t key, uint32_t imm) {
	struct rdma_metadata *send_meta =  (struct rdma_metadata *)
		calloc(1, sizeof(struct rdma_metadata) + TREE_SIZE * sizeof(struct ibv_sge));

	printf("--> Send GET [key %u]\n", key);

	//post_dummy_imm(master, 0);

	addr_t base_addr = mr_local_addr(sockfd, MR_BUFFER);
	uint8_t *param1 = (uint8_t *) base_addr; //key
	uint64_t *param2 = (uint64_t *) (base_addr + 4); //addr

	for(int i=0; i<TREE_SIZE; i++) {
		param1[i*3+0] = 0;
		param1[i*3+1] = 0;
		param1[i*3+2] = key;
	}

	send_meta->sge_entries[0].addr = (uintptr_t) param1;
	send_meta->sge_entries[0].length = 3*TREE_SIZE;
	send_meta->length = 3*TREE_SIZE;
	send_meta->sge_count = TREE_SIZE;
	send_meta->addr = 0;
	send_meta->imm = imm;
	return IBV_WRAPPER_SEND_WITH_IMM_ASYNC(sockfd, send_meta, MR_BUFFER, 0);
}

void post_get_req_sync(int sockfd, uint32_t key, int response_id) {
	struct timespec start, end;

	addr_t base_addr = mr_local_addr(sockfd, MR_DATA);

	if(REDN) {
		volatile uint64_t *res = (volatile uint64_t *) (base_addr);

		uint32_t wr_id = post_get_req_async(sockfd, key, response_id);

		time_stats_start(timer);

		IBV_TRIGGER(master_sock, sockfd, 0);

		// while(*res != (5555 + key - 1000)) {
			//printf("res %lu\n", *res);
			//sleep(1);
		// }

		time_stats_stop(timer);
		time_stats_print(timer, "Run Complete");

		//reset
		*res = 0;
	}
	else if(ONE_SIDED) {
		volatile struct bt_bucket *bucket = NULL;
		uint32_t wr_id = 0;
		addr_t bucket_addr;
		addr_t queue[TREE_SIZE];
		uint32_t key_to_find;

		time_stats_start(timer);
		for (int j=0; j<TREE_SIZE; j++) {
			key_to_find = key + j;
			memset(queue, -1, TREE_SIZE * sizeof(addr_t));
			queue[0] = mr_remote_addr(sockfd, MR_DATA);
			int key_found = 0;

			for (int i=0; i<TREE_SIZE; i++) {
				bucket_addr = queue[i];
				if (bucket_addr == 0) {
					printf("reached leaf node. Trying other branches...\n");
					continue;
				}
				if (bucket_addr == -1) {
					printf("reached end of tree.");
					break;
				}

				printf("read from remote addr %lu\n", bucket_addr);
				wr_id = post_read(sockfd, base_addr, bucket_addr, 27, MR_DATA, MR_DATA);
				IBV_TRIGGER(master_sock, sockfd, 0);
				IBV_AWAIT_WORK_COMPLETION(sockfd, wr_id);
				bucket = (volatile struct bt_bucket *) base_addr;

				printf("key required %u found %u\n", (uint8_t)key_to_find, bucket->key[0]);
				if(bucket->key[0] == (uint8_t)key_to_find) {
					key_found = 1;
					break;
				}
				else {
					if(2*i+1 < TREE_SIZE)
						queue[2*i+1] = ntohll(bucket->left);
					if(2*i+2 < TREE_SIZE)
						queue[2*i+2] = ntohll(bucket->right);
				}
			}
			if(!key_found) {
				printf("\n---------- didn't find required key %u ----------\n\n", key_to_find);
			} else {
				printf("\n------------------ found key %u ------------------\n\n", key_to_find);
				wr_id = post_read(sockfd, base_addr + offsetof(struct bt_bucket, value),
						bucket_addr + offsetof(struct bt_bucket, value), 8, MR_DATA, MR_DATA);
				IBV_TRIGGER(master_sock, sockfd, 0);
				IBV_AWAIT_WORK_COMPLETION(sockfd, wr_id);
			}
		}

		time_stats_stop(timer);
		time_stats_print(timer, "Run Complete");
	}
}

/* Returns new argc */
static int adjust_args(int i, char *argv[], int argc, unsigned del) {
	if (i >= 0) {
		for (int j = i + del; j < argc; j++, i++)
			argv[i] = argv[j];
		argv[i] = NULL;
		return argc - del;
	}
   return argc;
}

int process_opt_args(int argc, char *argv[]) {
	int dash_d = -1;
	restart:
	for (int i = 0; i < argc; i++) {
		if (strncmp("-b", argv[i], 2) == 0) {
			batch_size = atoi(argv[i+1]);
			dash_d = i;
			argc = adjust_args(dash_d, argv, argc, 2);
			goto restart;
		}
		else if (strncmp("-e", argv[i], 2) == 0) {
			sge_count = atoi(argv[i+1]);
			dash_d = i;
			argc = adjust_args(dash_d, argv, argc, 2);
			goto restart;
		}
		else if (strncmp("-p", argv[i], 2) == 0) {
			portno = argv[i+1];
			dash_d = i;
			argc = adjust_args(dash_d, argv, argc, 2);
			goto restart;
		}
		else if (strncmp("-i", argv[i], 2) == 0) {
			intf = argv[i+1];
			dash_d = i;
			argc = adjust_args(dash_d, argv, argc, 2);
			goto restart;
		}
		else if (strncmp("-s", argv[i], 2) == 0) {
			psync = 1;
			dash_d = i;
			argc = adjust_args(dash_d, argv, argc, 1);
			goto restart;
		}
		else if (strncmp("-cas", argv[i], 4) == 0) {
			use_cas = 1;
			dash_d = i;
			argc = adjust_args(dash_d, argv, argc, 1);
			goto restart;
		}
		else if (strncmp("-redn", argv[i], 5) == 0) {
			REDN = 1;
			ONE_SIDED = 0;
			dash_d = i;
			argc = adjust_args(dash_d, argv, argc, 1);
			goto restart;
		}
	}

   return argc;
}

void* create_shm(int *fd, int *res) {
	void * addr;
	*fd = shm_open(SHM_PATH, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		exit(-1);
	}

	*res = ftruncate(*fd, SHM_F_SIZE);
	if (res < 0)
	{
		exit(-1);
	}

	addr = mmap(NULL, SHM_F_SIZE, PROT_WRITE, MAP_SHARED, *fd, 0);
	if (addr == MAP_FAILED){
		exit(-1);
	}

	return addr;
}

void destroy_shm(void *addr) {
	int ret, fd;
	ret = munmap(addr, SHM_F_SIZE);
	if (ret < 0)
	{
		exit(-1);
	}

	fd = shm_unlink(SHM_PATH);
	if (fd < 0) {
		exit(-1);
	}
}

int get_ipaddress(char* ip, char* intf){
	struct ifaddrs *interfaces = NULL;
	struct ifaddrs *temp_addr = NULL;
	char *ipAddress = NULL;
	int success = 0;
	int ret = -1;
	// retrieve the current interfaces - returns 0 on success
	success = getifaddrs(&interfaces);
	if (success == 0) {
		// Loop through linked list of interfaces
		temp_addr = interfaces;
		while(temp_addr != NULL) {
			if(temp_addr->ifa_addr->sa_family == AF_INET) {
				// Check if interface is en0 which is the wifi connection on the iPhone
				if(strcmp(temp_addr->ifa_name, intf)==0){
					ipAddress=inet_ntoa(((struct sockaddr_in*)temp_addr->ifa_addr)->sin_addr);
					strcpy(ip, ipAddress);
					ret = 0;
				}
			}
			temp_addr = temp_addr->ifa_next;
		}
	}

	// Free memory
	freeifaddrs(interfaces);

	return ret;
}

int main(int argc, char **argv) {
	void *ptr;
	int shm_fd;
	int shm_ret;
	int iters;
	int ret;
	int offload_count = OFFLOAD_COUNT;
	int response_id = 1;

    timer = (struct time_stats*) malloc(sizeof(struct time_stats));
	int *shm_proc = (int*)create_shm(&shm_fd, &shm_ret);

	argc = process_opt_args(argc, argv);

	if(psync) {
		printf("Setting shm_proc to zero\n");
		*shm_proc = 0;
		return 0;
	}

	if (argc != 3 && argc != 1) {
		fprintf(stderr, "usage: %s <peer-address> <iters> [-p <portno>] [-e <sge count>] [-b <batch size>] [-redn] (note: run without args to use as server)\n", argv[0]);
		return 1;
	}

	if(argc > 1) {
        isClient = 1;
    }

	if(isClient) {
	
		iters = atoi(argv[2]);

		if(iters > OFFLOAD_COUNT) {
			fprintf(stderr, "iters should be less than or equal to OFFLOAD_COUNT = %d\n", OFFLOAD_COUNT);
			return 1;
		}
		time_stats_init(timer, iters);
		portno = client_portno;
	}
	else
		portno = server_portno;

	// allocate dram region
	for(int i=0; i<MR_COUNT; i++) {
		printf("Mapping dram memory: size %lu pagesize %lu\n", mr_sizes[i], sysconf(_SC_PAGESIZE));
		ret = posix_memalign(&ptr, sysconf(_SC_PAGESIZE), mr_sizes[i]);
		regions[i].type = i;
		regions[i].addr = (uintptr_t) ptr;
		regions[i].length = mr_sizes[i];
		if(ret) {
			printf("Failed to map space for dram memory region\n");
			return 1;
		}
	}

	if(get_ipaddress(host, intf)) {
		printf("Failed to find IP on interface %s\n", intf);
		return EXIT_FAILURE;
	}

    init_rdma_agent(portno, regions, MR_COUNT, 256, add_peer_socket, remove_peer_socket, test_callback);

	// Run in server mode
	if(!isClient) {

		master_sock = add_connection(host, portno, SOCK_MASTER, 1, 0);

		init_binarytree(regions[MR_DATA].addr);

		while(!(rc_ready(client_sock)) && !(rc_ready(worker_sock)) && !stop) {
			asm("");
		}

		sleep(5);

		pthread_create(&offload_thread, NULL, offload_binarytree, &offload_count);

		sleep(5);

		while(!stop) {
			sleep(3);
		}

		free((void *)regions[0].addr);
		return 0;
	}

	master_sock = add_connection(host, portno, SOCK_MASTER, 1, 0);

	while(!(rc_ready(master_sock)) && !stop) {
		asm("");
	}

	// Run in client mode
 	client_sock = add_connection(argv[1], server_portno, SOCK_CLIENT, 1, IBV_EXP_QP_CREATE_MANAGED_SEND);

	sleep(10);

	printf("Starting benchmark ...\n");

	for(int i=0; i<iters; i++) {
		IBV_RECEIVE_IMM(client_sock);
		post_get_req_sync(client_sock, 1000, response_id);
		response_id++;
		usleep(5000);
	}

	time_stats_print(timer, "Run Complete");
	sleep(1);

    return 0;
}
