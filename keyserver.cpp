#include <vector>
#include <deque>
#include <map>
#include <stdint.h>
#include <random>
#include <future>
#include <mutex>
#include <algorithm>
#include <stdlib.h>

#define USE_EPOLL !__APPLE__

#if USE_EPOLL
#include <sys/epoll.h>
#endif

#include <libwebsockets.h>
#define CMAKE_BUILD
extern "C" {
#include <private-libwebsockets.h> // derp
}

#define NO_VOTE 0xffff
#define UNVOTE_DELAY (60*5)
#define BATCH_SIZE 10
#define WORKERS 4
#define MAX_POPULARITY 6

struct player_data {
	libwebsocket *wsi;
	bool ready;
	bool magic;
	int fd;
	uint64_t pid;
	size_t voting_players_idx;
	uint16_t vote;
	uint64_t vote_frame;
};

enum {
	REQ_GIMME_SINCE = 0,
	REQ_SET_VOTE = 1,
	REQ_SET_RUNNING = 2,
};

enum {
	RES_HERES_YOUR_STUFF = 0,
	RES_BATCH = 1,
};

struct popularity_packet {
	uint16_t vote;
	uint32_t popularity;
} __attribute__((packed));

struct batch_packet {
	uint8_t type;
	uint64_t frame;
	uint32_t num_players;
	uint8_t num_inputs;
} __attribute__((packed));
 
static libwebsocket_context *g_lws_ctx;

static std::default_random_engine g_rand;

static bool g_running;

static uint64_t g_next_pid;

static uint64_t g_frame;
static uint64_t g_next_frame_time;

static std::map<uint16_t, size_t> g_popularity;

static std::vector<player_data> g_players;
static size_t g_num_players;
static std::mutex g_conn_mtx;

// oh, why not
static std::vector<player_data *> g_voting_players;
static size_t g_voting_players_count;

static std::vector<uint16_t> g_history;
static int g_history_fd;

struct player_data *g_mr_headless;

#if USE_EPOLL
static int g_epoll_fd;
#endif

#define please(x) do { if((x) == -1) fprintf(stderr, "%s failed: %s\n", #x, strerror(errno)); } while(0)

static uint8_t *get_packet(size_t size) {
	return (uint8_t *) malloc(LWS_SEND_BUFFER_PRE_PADDING + size + LWS_SEND_BUFFER_POST_PADDING) + LWS_SEND_BUFFER_PRE_PADDING;
}

static void un_packet(uint8_t *packet) {
	free(packet - LWS_SEND_BUFFER_PRE_PADDING);
}

static uint64_t get_time() {
	struct timeval tv = {0};
	please(gettimeofday(&tv, NULL));
	return ((uint64_t) tv.tv_sec) * 1000000 + tv.tv_usec;
}

static void set_running(bool running) {
	printf("running <- %d\n", running);
	if(running && !g_running) {
		g_next_frame_time = get_time();
	}
	g_running = running;
}


static void set_vote(player_data *pl, uint16_t vote) {
	if(vote != NO_VOTE)
		vote &= ~4; // no select for you
	if(vote == pl->vote)
		return;
	if(pl->vote != NO_VOTE) {
		if(--g_popularity[pl->vote] == 0)
			g_popularity.erase(pl->vote);
	}
	if(vote != NO_VOTE) {
		g_popularity[vote]++;
	}
	if(vote == NO_VOTE) {
		player_data *opl = g_voting_players.back();
		g_voting_players.pop_back();
		opl->voting_players_idx = pl->voting_players_idx;
		g_voting_players[opl->voting_players_idx] = opl;
	} else if(pl->vote == NO_VOTE) {
		pl->voting_players_idx = g_voting_players.size();
		g_voting_players.push_back(pl);
	}
	pl->vote = vote;
	pl->vote_frame = g_frame;
}

static uint16_t get_input() {
	if(g_voting_players.size() == 0)
		return 0;
	std::uniform_int_distribution<size_t> distr(0, g_voting_players.size() - 1);
	size_t lucky = distr(g_rand);
	return g_voting_players[lucky]->vote;
}

static void kill_player(player_data *pl) {
	if(pl == g_mr_headless) {
		g_mr_headless = NULL;
		set_running(false);
	}
	if(pl->wsi) {
		set_vote(pl, NO_VOTE);
		pl->wsi = NULL;
		if(!pl->magic)
			g_num_players--;
	}
}

static inline void pre_write(player_data *pl) {
	// derp
	if(pl->magic)
		fcntl(pl->fd, F_SETFL, 0);
}
static inline void post_write(player_data *pl) {
	if(pl->magic)
		fcntl(pl->fd, F_SETFL, O_NONBLOCK);
}

static void scattershot_packet(void *packet, size_t len) {
	// In the /extremely/ unlikely case that this becomes TPP-level popular,
	// this is actually going to be eating most of the CPU just popping between
	// userland and kernelland and poking TCP queues.  Whatever.  If I double
	// the batch rate it will almost certainly be OK, so...
	std::future<void> f[WORKERS];
	std::mutex blargh_lock;
	std::vector<player_data *> blargh;
	for(int i = 0; i < WORKERS; i++) {
		f[i] = std::async(std::launch::async, [i, len, packet, &blargh_lock, &blargh] {
			uint8_t *mypacket = get_packet(len);
			memcpy(mypacket, packet, len);
			for(size_t j = i; j < g_players.size(); j += WORKERS) {
				player_data *pl = &g_players[j];
				libwebsocket *wsi = pl->wsi;
				if(!wsi || !pl->ready)
					continue;
				// Note: Docs say not to do this outside of
				// LWS_CALLBACK_SERVER_WRITEABLE or on multiple threads.  But
				// it doesn't actually matter (if you don't mind sometimes
				// rudely disconnecting people).
				pre_write(pl);
				int ret = libwebsocket_write(wsi, (unsigned char *) mypacket, len, LWS_WRITE_BINARY);
				post_write(pl);
				if(ret != len) {
					printf("polvio'ing %p because %d/%zu\n", pl, ret, len);
					std::lock_guard<std::mutex> lk(blargh_lock);
					blargh.push_back(pl);
				}
			}
			un_packet(mypacket);
		});
	}
	for(int i = 0; i < WORKERS; i++)
		f[i].wait();
	for(player_data *pl : blargh) {
		libwebsocket_close_and_free_session(g_lws_ctx, pl->wsi, LWS_CLOSE_STATUS_POLICY_VIOLATION);
		kill_player(pl);
	}
}

static void add_to_history(uint16_t input) {
	g_history.push_back(input);
	write(g_history_fd, &input, sizeof(input));
}

static void do_frame() {
	uint16_t input = get_input();
	add_to_history(input);
	g_frame++;

	if(g_frame % 60 == 0)
		printf("f %llu\n", g_frame);

	if(g_frame % BATCH_SIZE == 0) {
		size_t num_inputs = BATCH_SIZE;
		// actually, add the WS bullshit here
		size_t len =
			sizeof(batch_packet) +
			num_inputs * sizeof(uint16_t) +
			std::min(g_popularity.size(), (size_t) MAX_POPULARITY) * sizeof(popularity_packet);
		uint8_t *packet = get_packet(len);
		batch_packet *bp = (batch_packet *) packet;
		bp->type = RES_BATCH;
		bp->frame = g_frame - num_inputs;
		bp->num_players = g_num_players;
		bp->num_inputs = num_inputs;
		auto inputs = (uint16_t *) &bp[1];
		memcpy(inputs, g_history.data() + (g_frame - num_inputs), num_inputs * sizeof(uint16_t));
		auto popcnt_data = (popularity_packet *) &inputs[num_inputs];
		size_t i = 0;
		for(auto it = g_popularity.rbegin(); it != g_popularity.rend() && i < MAX_POPULARITY; ++it, i++)
			popcnt_data[i] = {it->first, (uint32_t) it->second};
		scattershot_packet(packet, len);
		un_packet(packet);
	}

}

static int keyserver_callback(struct libwebsocket_context *context, struct libwebsocket *wsi, enum libwebsocket_callback_reasons reason, void *user, void *in, size_t len) {
#define BAD() do { fprintf(stderr, "bad on line %d\n", __LINE__); return -1; } while(0)
#if USE_EPOLL
	libwebsocket_pollargs *pa = (libwebsocket_pollargs *) in;
#endif
	switch(reason) {
#if USE_EPOLL
	case LWS_CALLBACK_ADD_POLL_FD:
		please(epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, pa->fd, (epoll_event[]) {{translate_pa_mode(pa->events), {.ptr = pa}}}));
		break;
	case LWS_CALLBACK_CHANGE_MODE_POLL_FD:
		please(epoll_ctl(g_epoll_fd, EPOLL_CTL_MOD, pa->fd, (epoll_event[]) {{translate_pa_mode(pa->events), {.ptr = pa}}}));
		break;
	case LWS_CALLBACK_CHANGE_MODE_POLL_FD:
		please(epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, pa->fd, NULL));
		break;
#endif
	case LWS_CALLBACK_ESTABLISHED: {
		int fd = wsi->sock;
		if(fd >= g_players.size())
			g_players.resize(fd + 1);
		struct sockaddr_in sin;
		socklen_t len = sizeof(sin);
		bool is_magic = getpeername(fd, (struct sockaddr *) &sin, &len) != -1 &&
			ntohl(sin.sin_addr.s_addr) == 0x7f000001;
		player_data *pl = &g_players[fd];
		*pl = player_data();
		pl->wsi = wsi;
		pl->fd = fd;
		pl->pid = g_next_pid++;
		pl->magic = is_magic;
		pl->vote = NO_VOTE;
		if(!is_magic)
			g_num_players++;
		break;
	}
	case LWS_CALLBACK_RECEIVE: {
		player_data *pl = &g_players.at(wsi->sock);
		auto buf = (uint8_t *) in;
		if(len < 1)
			BAD();
		int req = buf[0];
		if(req == REQ_GIMME_SINCE) {
			if(len != 9)
				BAD();
			auto since = *(uint64_t *) (buf + 1);
			if(since > g_frame || (g_frame - since > 60*30 && !pl->magic))
				BAD();
			uint64_t frames = g_frame - since;

			size_t outlen = 1 + frames * sizeof(uint16_t);
			uint8_t *packet = get_packet(outlen);
			packet[0] = RES_HERES_YOUR_STUFF;
			memcpy(&packet[1], g_history.data() + since, frames * sizeof(uint16_t));
			pre_write(pl);
			int ret = libwebsocket_write(wsi, packet, outlen, LWS_WRITE_BINARY);
			post_write(pl);
			if(ret != outlen)
				BAD();
			un_packet(packet);
			pl->ready = true;
		} else if(req == REQ_SET_VOTE) {
			if(len != 3)
				BAD();
			set_vote(pl, *(uint16_t *) (buf + 1));
		} else if(req == REQ_SET_RUNNING && pl->magic) {
			if(len != 2)
				BAD();
			set_running(buf[1]);
			g_mr_headless = pl;
		} else {
			BAD();
		}
		break;
	}
	case LWS_CALLBACK_CLOSED: {
		player_data *pl = &g_players.at(wsi->sock);
		kill_player(pl);
		break;
	}
	default:
		break;
	}
	return 0;
}

static void serve() {
#if USE_EPOLL
	struct epoll_event events[16];
	int ready = epoll_wait(g_epoll_fd, events, sizeof(events)/sizeof(*events), 4) == -1;
	if(ready == -1) {
		perror("epoll_wait");
		exit(1);
	}
	for(int i = 0; i < ready; i++) {
		auto pa = (libwebsocket_pollargs *) events[i].data.ptr;
		please(libwebsocket_service_fd(g_lws_ctx, pa));
	}
#else
	please(libwebsocket_service(g_lws_ctx, 4));
#endif
}

int main() {
	g_rand = std::default_random_engine(std::random_device()());
	g_history_fd = open("saves/history.bin", O_RDWR | O_CREAT | O_APPEND, 0644);
	if(g_history_fd == -1) {
		perror("open history");
		exit(1);
	}
	uint16_t frames[10000];
	while(1) {
		ssize_t ret = read(g_history_fd, frames, sizeof(frames));
		if(ret == -1) {
			perror("read");
			exit(1);
		}
		if(ret == 0) break;
		int n = ret / sizeof(*frames);
		g_history.insert(g_history.end(), std::begin(frames), std::begin(frames) + n);
		g_frame += n;
	}

	printf("starting from frame %lld\n", g_frame);

	libwebsocket_protocols pi;
	memset(&pi, 0, sizeof(pi));
	pi.name = "keyserver";
	pi.callback = keyserver_callback;
	pi.per_session_data_size = 0;
	pi.rx_buffer_size = 4096*4;

	lws_context_creation_info ci;
	memset(&ci, 0, sizeof(ci));
	ci.port = 4321;
	ci.iface = NULL;
	ci.gid = -1;
	ci.uid = -1;
	ci.protocols = &pi;

	g_lws_ctx = libwebsocket_create_context(&ci);


#if USE_EPOLL
	g_epoll_fd = epoll_create(1);
#endif
	while(1) {
		serve();
		if(g_running) {
			uint64_t now = get_time();
			if(now >= g_next_frame_time) {
				do_frame();
				if(now - g_next_frame_time >= 300000) {
					// we've fallen behind...
					g_next_frame_time = now;
				}
				g_next_frame_time += (1000000/60);
			}
		}
	}
}
