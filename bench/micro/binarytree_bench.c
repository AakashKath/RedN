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

#define REDN 1
// #define ONE_SIDED 1

#define SHM_PATH "/ifbw_shm"
#define SHM_F_SIZE 128

char *portno = "12345";
char *client_portno = "11111";
char *server_portno = "22222";

struct mr_context regions[MR_COUNT];

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

uint64_t mr_sizes[MR_COUNT] = {268265456UL, 268265456UL};

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

	client_sock = sockfd;
	worker_sock = worker;

	printf("Setting sockfds [client: %d worker: %d]\n", client_sock, worker_sock);
	
	return;
}

void remove_peer_socket(int sockfd) {
	;
}

void print_seg_data() {
	if(sr0_data && sr0_raddr) {
		printf("------ AFTER ------\n");
		for(int j=0; j<LIST_SIZE; j++) {
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
			printf("sr2_ctrl: raw %lx idx %u opmod %u opcode %u qpn_ds %x fm_ce_se %x sig %u (imm %u)\n", *((uint64_t *)&sr2_ctrl[j]->opmod_idx_opcode), idx2, opmod2, opcode2, ntohl(sr2_ctrl[j]->qpn_ds), ntohl(sr2_ctrl[j]->fm_ce_se), sr2_ctrl[j]->signature, ntohl(sr2_ctrl[j]->imm));
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

void * offload_binarytree(void *iters) {
	;
}

void init_binarytree(addr_t addr) {
	printf("---- Initializing binarytree ----\n");

	struct ll_bucket *bucket = (struct ll_bucket*)addr;

	printf("bucket addr %lu\n", addr);
	for(int i=0; i<10; i++) {
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

void post_get_req_sync(int sockfd, uint32_t key, int response_id) {
	struct timespec start, end;

	addr_t base_addr = mr_local_addr(sockfd, MR_DATA);

	#if REDN
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
	#elif defined(ONE_SIDED)
		volatile struct ll_bucket *bucket = NULL;
		uint32_t wr_id = 0;
		addr_t bucket_addr =  mr_remote_addr(sockfd, MR_DATA);

		time_stats_start(timer);

		for(int i=0; i<LIST_SIZE; i++) {
			printf("read from remote addr %lu\n", bucket_addr);
			wr_id = post_read(sockfd, base_addr, bucket_addr, 19, MR_DATA, MR_DATA);
			IBV_TRIGGER(master_sock, sockfd, 0);
			IBV_AWAIT_WORK_COMPLETION(sockfd, wr_id);
			bucket = (volatile struct ll_bucket *) base_addr;

			printf("key required %u found %u\n", (uint8_t)key, bucket->key[0]);
			if(bucket->key[0] == (uint8_t)key) {
				printf("found key\n");
				wr_id = post_read(sockfd, base_addr + offsetof(struct ll_bucket, value),
						bucket_addr + offsetof(struct ll_bucket, value), 8, MR_DATA, MR_DATA);
				IBV_TRIGGER(master_sock, sockfd, 0);
				IBV_AWAIT_WORK_COMPLETION(sockfd, wr_id);
				break;
			}
			else {
				bucket_addr = ntohll(bucket->next);
				//base_addr += sizeof(struct ll_bucket);
			}

			//XXX remove
			if(i==0) {
				wr_id = post_read(sockfd, base_addr + offsetof(struct ll_bucket, value),
						bucket_addr + offsetof(struct ll_bucket, value), 8, MR_DATA, MR_DATA);
				IBV_TRIGGER(master_sock, sockfd, 0);
				IBV_AWAIT_WORK_COMPLETION(sockfd, wr_id);
				break;
			}
		}

		time_stats_stop(timer);
		time_stats_print(timer, "Run Complete");
	#endif
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
		fprintf(stderr, "usage: %s <peer-address> <iters> [-p <portno>] [-e <sge count>] [-b <batch size>]  (note: run without args to use as server)\n", argv[0]);
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
	master_sock = add_connection(host, portno, SOCK_MASTER, 1, 0);

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
