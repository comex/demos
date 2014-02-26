#include <vector>
#include <deque>
#include <unordered_map>
#include <stdint.h>
#include <random>
#include <future>
#include <mutex>
#include <algorithm>
#include <stdlib.h>
#include <sys/mman.h>

#define USE_EPOLL !__APPLE__

#if USE_EPOLL
#include <sys/epoll.h>
#endif

#include <libwebsockets.h>
#define CMAKE_BUILD
#include <private-libwebsockets.h> // derp

#define NO_VOTE 0xffff
#define UNVOTE_DELAY (60*5)
#define BATCH_SIZE 10
#define WORKERS 4
#define MAX_POPULARITY 10

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

struct queued_event {
	uint64_t frame;
	int fd;
	uint64_t pid;
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
	uint8_t num_inputs;
} __attribute__((packed));

static libwebsocket_context *g_lws_ctx;

static std::default_random_engine g_rand;

static bool g_running;

static uint64_t g_next_pid;

static uint64_t g_frame;
static uint64_t g_next_frame_time;

static std::unordered_map<uint16_t, size_t> g_popularity;

static std::vector<player_data> g_players;
static size_t g_num_players;
static std::mutex g_conn_mtx;

// oh, why not
static std::vector<player_data *> g_voting_players;
static size_t g_voting_players_count;

static std::deque<queued_event> g_events;

static int g_history_fd;
static void *g_history_file;
static size_t g_history_file_size;
static uint64_t *g_history_count;
static uint16_t *g_history_frames;

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
	if(running && !g_running) {
		g_next_frame_time = get_time();
	}
	g_running = running;
}


static void disconnect_player(player_data *pl) {
	close(pl->fd);
}


static void set_vote(player_data *pl, uint16_t vote) {
	if(vote == pl->vote)
		return;
	if(pl->vote != NO_VOTE) {
		if(--g_popularity[pl->vote] == 0)
			g_popularity.erase(pl->vote);
	}
	if(vote == NO_VOTE) {
		std::swap(g_voting_players[pl->voting_players_idx], g_voting_players[--g_voting_players_count]);
		g_voting_players.resize(g_voting_players_count);
	} else {
		g_voting_players.push_back(pl);
		g_popularity[vote]++;
	}
	pl->vote = vote;
	pl->vote_frame = g_frame;
	if(vote == 0) { // release
		g_events.push_back(queued_event{g_frame + UNVOTE_DELAY, pl->fd, pl->pid});
	}
}

static uint16_t get_input() {
	if(g_voting_players_count == 0)
		return 0;
	std::uniform_int_distribution<size_t> distr(0, g_voting_players_count - 1);
	size_t lucky = distr(g_rand);
	return g_voting_players[lucky]->vote;
}

static void inc_frame() {
	g_frame++;
	while(g_events.size() > 0 && g_frame >= g_events.front().frame) {
		queued_event *ev = &g_events.front();
		if(ev->fd < g_players.size()) {
			player_data *pl = &g_players[ev->fd];
			if(pl->pid == ev->pid && g_frame == pl->vote_frame + UNVOTE_DELAY) {
				set_vote(pl, NO_VOTE);
			}
		}
		g_events.pop_front();
	}
}

static void scattershot_packet(void *packet, size_t len) {
	// In the /extremely/ unlikely case that this becomes TPP-level popular,
	// this is actually going to be eating most of the CPU just popping between
	// userland and kernelland and poking TCP queues.  Whatever.  If I double
	// the batch rate it will almost certainly be OK, so...
	std::future<void> f[4];
	for(int i = 0; i < WORKERS; i++) {
		f[i] = std::async(std::launch::async, [=] {
			for(size_t j = 0; j < g_players.size(); j += WORKERS) {
				player_data *pl = &g_players[j];
				libwebsocket *wsi = pl->wsi;
				if(!wsi || !pl->ready)
					continue;
				// Note: Docs say not to do this outside of
				// LWS_CALLBACK_SERVER_WRITEABLE or on multiple threads.  But
				// it doesn't actually matter (if you don't mind sometimes
				// rudely disconnecting people).
				int ret = libwebsocket_write(wsi, (unsigned char *) packet, len, LWS_WRITE_BINARY);
				if(ret != len)
					disconnect_player(pl);
			}
		});
	}
	for(int i = 0; i < WORKERS; i++)
		f[i].wait();
}

static void add_to_history(uint16_t input) {
	if(sizeof(*g_history_count) + g_frame * sizeof(*g_history_frames) >= g_history_file_size) {
		if(g_history_file_size)
			munmap(g_history_file, g_history_file_size);
		g_history_file_size = (g_history_file_size + 0x1000) * 2;
		if(ftruncate(g_history_fd, g_history_file_size) == -1) {
			perror("ftruncate");
			exit(1);
		}
		if((g_history_file = mmap(NULL, g_history_file_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_history_fd, 0)) == MAP_FAILED) {
			perror("mmap");
			exit(1);
		}
		g_history_count = (uint64_t *) g_history_file;
		g_history_frames = (uint16_t *) (g_history_count + 1);
	}
	*g_history_count = g_frame;
	g_history_frames[g_frame - 1] = input;
}

static void do_frame() {
	inc_frame();
	uint16_t input = get_input();
	add_to_history(input);

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
		bp->frame = g_frame - num_inputs + 1;
		bp->num_inputs = num_inputs;
		auto inputs = (uint16_t *) &bp[1];
		memcpy(inputs, g_history_frames + g_frame - num_inputs, num_inputs * sizeof(uint16_t));
		size_t i = 0;
		auto popcnt_data = (popularity_packet *) &inputs[num_inputs];
		for(auto it : g_popularity) {
			popcnt_data[i++] = {it.first, (uint32_t) it.second};
			if(i == MAX_POPULARITY) break;
		}
		scattershot_packet(packet, len);
		un_packet(packet);
	}

}

static int keyserver_callback(struct libwebsocket_context *context, struct libwebsocket *wsi, enum libwebsocket_callback_reasons reason, void *user, void *in, size_t len) {
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
		g_players[fd] = (player_data) {
			.wsi = wsi,
			.fd = fd,
			.pid = g_next_pid++,
			.magic = is_magic,
		};
		if(!is_magic)
			g_num_players++;
		break;
	}
	case LWS_CALLBACK_RECEIVE: {
		player_data *pl = &g_players.at(wsi->sock);
		auto buf = (uint8_t *) in;
		if(len < 1)
			return -1;
		int req = buf[0];
		if(req == REQ_GIMME_SINCE) {
			if(len != 9)
				return -1;
			auto since = *(uint64_t *) (buf + 1);
			if(since < 1 || since > g_frame || (g_frame - since > 60*30 && !pl->magic))
				return -1;
			uint64_t frames = g_frame - (since - 1);

			size_t outlen = 1 + frames * sizeof(uint16_t);
			uint8_t *packet = get_packet(outlen);
			packet[0] = RES_HERES_YOUR_STUFF;
			memcpy(&packet[1], g_history_frames + (since - 1), frames * sizeof(uint16_t));
			int ret = libwebsocket_write(wsi, packet, outlen, LWS_WRITE_BINARY);
			if(ret != outlen)
				disconnect_player(pl);
			un_packet(packet);
		} else if(req == REQ_SET_VOTE) {
			if(len != 3)
				return -1;
			set_vote(pl, *(uint16_t *) (buf + 1));
		} else if(req == REQ_SET_RUNNING && pl->magic) {
			if(len != 2)
				return -1;
			set_running(buf[1]);
		} else {
			return -1;
		}
		break;
	}
	case LWS_CALLBACK_CLOSED: {
		player_data *pl = &g_players.at(wsi->sock);
		pl->wsi = NULL;
		if(!pl->magic)
			g_num_players--;
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
	g_history_fd = open("history.bin", O_RDWR | O_CREAT);
	if(g_history_fd == -1) {
		perror("open history");
		exit(1);
	}
	g_lws_ctx = libwebsocket_create_context((lws_context_creation_info[]) {{
		.port = 4321,
		.iface = NULL,
		.protocols = (libwebsocket_protocols[]) {
			{
				.name = "keyserver",
				.callback = keyserver_callback,
				.per_session_data_size = 0,
				.rx_buffer_size = 16
			},
		},
		.gid = -1,
		.uid = -1
	}});
#if USE_EPOLL
	g_epoll_fd = epoll_create(1);
#endif
	while(1) {
		serve();
		if(g_running) {
			uint16_t now = get_time();
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
