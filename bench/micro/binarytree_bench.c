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
// #include "agent.h"
# include "binarytree_bench.h"

struct time_stats *timer;

int isClient = 0;
int n_hash_req = 0;
int psync = 0;

#define OFFLOAD_COUNT 5000
static pthread_t offload_thread;

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
	;
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
