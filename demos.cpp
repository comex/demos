#include <vector>
#include <deque>
#include <unordered_map>
#include <stdint.h>
#include <random>
#include <future>
#include <mutex>

#define NO_VOTE 0xffffffff
#define UNVOTE_DELAY (60*5)
#define BATCH_SIZE 10
#define WORKERS 4

struct player_data {
	libwebsocket *wsi;
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

__attribute__((packed))
struct popularity_packet {
	uint16_t vote;
	uint32_t popularity;
};

__attribute__((packed))
struct batch_packet {
	uint8_t type;
	uint64_t frame;
	uint8_t num_inputs;
	uint16_t inputs[BATCH_SIZE];
	popularity_packet popcnt_data[0];
};

static std::default_random_engine g_rand;

static uint64_t g_next_pid;

static uint64_t g_frame;

static std::unordered_map<uint16_t, size_t> g_popularity;

static std::vector<player_data> g_players;
static std::mutex g_conn_mtx;

// oh, why not
static std::vector<player_data *> g_voting_players;
static size_t g_voting_players_count;

static std::deque<queued_event> g_events;

static uint16_t g_current_batch[BATCH_SIZE];
static int g_current_batch_idx;

static void init() {
	g_rand = std::default_random_engine(std::random_device()());
}

static void set_vote(player_data *pl, uint32_t vote) {
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

static uint32_t get_input() {
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

static void scattershot_packet(std::vector<uint8_t>& packet) {
	// In the /extremely/ unlikely case that this becomes TPP-level popular,
	// this is actually going to be eating most of the CPU just popping between
	// userland and kernelland and poking TCP queues.  Whatever.  If I double
	// the batch rate it will almost certainly be OK, so...
	for(int i = 0; i < WORKERS; i++) {
		std::async(std::launch::async, [=] {
			std::lock_guard lk(g_conn_mtx);
			for(size_t j = 0; j < g_players.size(); j += WORKERS) {
				player_data *pl = &g_players[j];
				libwebsocket *wsi = pl->wsi;
				if(!wsi || wsi->state != WSI_STATE_ESTABLISHED)
					continue;
				// just do the send raw here
				// if buffer is full, fuck that, disconnect them

			}
		});
	}
}


static void do_frame() {
	inc_frame();
	uint32_t input = get_input();
	g_current_batch[g_current_batch_idx++] = input;
	if(g_current_batch_idx == BATCH_SIZE) {
		std::vector<uint8_t> packet;
		// actually, add the WS bullshit here
		packet.resize(sizeof(batch_packet) + g_popularity.size() * sizeof(uint16_t));
		batch_packet *bp = (batch_packet *) packet.data();
		bp->type = 0;
		bp->frame = g_frame - BATCH_SIZE + 1;
		bp->num_inputs = BATCH_SIZE;
		memcpy(bp->inputs, g_current_batch, sizeof(bp->inputs));
		size_t i = 0;
		for(auto it : g_popularity) {
			bp->popcnt_data[i++] = {it.first, it.second};
		}
		distribute_packet(packet);

		g_current_batch_idx = 0;
	}

}

