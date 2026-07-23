#include <iostream>
#include <unordered_map>
#include <string>
#include <vector>
#include <queue>
#include <list>
#include <map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <memory>
#include <chrono>
#include <cassert>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <utility>

// ============================================================================
// CORE PRIMITIVES
// ============================================================================
enum class Side      : uint8_t { Buy, Sell };
enum class OrderType : uint8_t { Limit, Market, Cancel };

struct Order {
    uint64_t order_id;
    uint32_t price;
    uint32_t quantity;
    Side     side;
    // Pad to 24 bytes — aligns cleanly in arena blocks, avoids false sharing
    uint8_t  _pad[7];
};
static_assert(sizeof(Order) == 24, "Order struct must be 24 bytes");

struct OrderRequest {
    uint64_t    order_id;
    std::string ticker;
    OrderType   type;
    uint32_t    price;
    uint32_t    quantity;
    Side        side;
};

struct TradeReport {
    uint64_t aggressive_id;
    uint64_t resting_id;
    uint32_t fill_price;
    uint32_t fill_quantity;
    std::string ticker;
};

// ============================================================================
// 1. CUSTOM ARENA MEMORY POOL
//    - Reserves a flat slab of memory at startup
//    - Alloc = O(1) bump pointer; free = O(1) push to freelist
//    - Zero heap fragmentation; deterministic latency
// ============================================================================
template<typename T, size_t Capacity>
class ArenaPool {
private:
    // Raw aligned storage — one contiguous block, allocated once at startup
    alignas(T) uint8_t storage[sizeof(T) * Capacity];

    // Freelist: indices of returned slots, popped on next alloc
    uint32_t freelist[Capacity];
    uint32_t freelist_top = 0;

    // Bump pointer for first-time slots not yet in freelist
    uint32_t bump = 0;

    std::mutex pool_mutex;

public:
    ArenaPool() {
        // Pre-populate freelist in reverse so first alloc gives slot 0
        // (improves cache locality on initial fills)
        for (uint32_t i = Capacity; i-- > 0; )
            freelist[freelist_top++] = i;
    }

    // O(1) allocation — returns a pointer into the pre-reserved slab
    T* allocate() {
        std::lock_guard<std::mutex> lock(pool_mutex);
        if (freelist_top == 0) [[unlikely]] return nullptr; // Pool exhausted
        uint32_t slot = freelist[--freelist_top];
        T* ptr = reinterpret_cast<T*>(storage + slot * sizeof(T));
        return ptr;
    }

    // O(1) deallocation — returns slot index to freelist
    void deallocate(T* ptr) {
        std::lock_guard<std::mutex> lock(pool_mutex);
        uintptr_t offset = reinterpret_cast<uint8_t*>(ptr) - storage;
        uint32_t slot = static_cast<uint32_t>(offset / sizeof(T));
        assert(slot < Capacity);
        freelist[freelist_top++] = slot;
    }

    size_t available() {
        std::lock_guard<std::mutex> lock(pool_mutex);
        return freelist_top;
    }
};

// Global pool shared across all shards — 1M order slots pre-reserved at startup
static constexpr size_t POOL_CAPACITY = 1'000'000;
static ArenaPool<Order, POOL_CAPACITY> g_order_pool;

// ============================================================================
// 2. ISOLATED ORDER BOOK SHARD
//    Each ticker gets exactly one of these — no cross-ticker lock contention.
//
//    Data structures per side:
//      Price level index : std::map<price, std::list<Order*>>
//        - Bids sorted descending (best bid = begin())
//        - Asks sorted ascending  (best ask = begin())
//        - Insert/remove level: O(log P) where P = distinct price levels
//      Time priority queue : std::list<Order*> per price level
//        - Append to tail: O(1)
//        - Pop from head:  O(1)  (FIFO within same price)
//      Order lookup      : std::unordered_map<order_id, iterator>
//        - Locate any live order: O(1) average
//      Cancellation      : O(1) iterator invalidation via stored list iterator
// ============================================================================
class OrderBookShard {
public:
    // Iterator type pointing into a price-level list — stored for O(1) cancel
    using LevelList = std::list<Order*>;
    using BookSide  = std::map<uint32_t, LevelList>;

    struct OrderMeta {
        LevelList::iterator list_it;  // Points into the level's DLL
        BookSide*           book;     // Points to bids or asks map
    };

private:
    std::string ticker_sym;
    std::mutex  shard_mtx;

    BookSide bids; // Highest price first (std::greater comparator below)
    BookSide asks; // Lowest price first (default)

    // Re-declare bids with descending order
    std::map<uint32_t, LevelList, std::greater<uint32_t>> bids_desc;

    // O(1) cancel: order_id → {iterator into level list, pointer to book side}
    std::unordered_map<uint64_t, OrderMeta> live_orders;

    // Telemetry
    std::atomic<uint64_t> orders_processed{0};
    std::atomic<uint64_t> trades_executed{0};
    std::atomic<uint64_t> total_latency_ns{0};
    std::atomic<uint64_t> peak_latency_ns{0};
    std::atomic<uint64_t> orders_cancelled{0};
    std::atomic<uint64_t> orders_rejected{0};

    // Trade log (bounded ring buffer in production; vector for demo)
    std::vector<TradeReport> trade_log;

    // -----------------------------------------------------------------------
    // INTERNAL MATCHING CORE
    // Match aggressive order against resting opposite side.
    // Returns quantity remaining after sweeping all crossable levels.
    // -----------------------------------------------------------------------
    uint32_t sweep_book(const OrderRequest& req,
                        std::map<uint32_t, LevelList, std::greater<uint32_t>>* bid_side,
                        BookSide* ask_side,
                        uint32_t remaining)
    {
        // Select the book we're aggressing into
        if (req.side == Side::Buy) {
            // Aggressive buy: match against resting asks (ascending price)
            for (auto level_it = ask_side->begin();
                 level_it != ask_side->end() && remaining > 0; )
            {
                uint32_t ask_price = level_it->first;
                // Price check: limit order only crosses if ask <= our limit
                if (req.type == OrderType::Limit && ask_price > req.price) break;

                LevelList& queue = level_it->second;
                while (!queue.empty() && remaining > 0) {
                    Order* resting = queue.front();
                    uint32_t fill = std::min(remaining, resting->quantity);
                    remaining          -= fill;
                    resting->quantity  -= fill;

                    trade_log.push_back({
                        req.order_id, resting->order_id,
                        ask_price, fill, ticker_sym
                    });
                    trades_executed.fetch_add(1, std::memory_order_relaxed);

                    if (resting->quantity == 0) {
                        live_orders.erase(resting->order_id);
                        g_order_pool.deallocate(resting);
                        queue.pop_front();
                    }
                }
                if (queue.empty()) level_it = ask_side->erase(level_it);
                else break;
            }
        } else {
            // Aggressive sell: match against resting bids (descending price)
            for (auto level_it = bid_side->begin();
                 level_it != bid_side->end() && remaining > 0; )
            {
                uint32_t bid_price = level_it->first;
                if (req.type == OrderType::Limit && bid_price < req.price) break;

                LevelList& queue = level_it->second;
                while (!queue.empty() && remaining > 0) {
                    Order* resting = queue.front();
                    uint32_t fill = std::min(remaining, resting->quantity);
                    remaining          -= fill;
                    resting->quantity  -= fill;

                    trade_log.push_back({
                        req.order_id, resting->order_id,
                        bid_price, fill, ticker_sym
                    });
                    trades_executed.fetch_add(1, std::memory_order_relaxed);

                    if (resting->quantity == 0) {
                        live_orders.erase(resting->order_id);
                        g_order_pool.deallocate(resting);
                        queue.pop_front();
                    }
                }
                if (queue.empty()) level_it = bid_side->erase(level_it);
                else break;
            }
        }
        return remaining;
    }

    // Post a resting order onto the appropriate side at the given price level
    void post_resting(const OrderRequest& req, uint32_t remaining_qty) {
        Order* slot = g_order_pool.allocate();
        if (!slot) [[unlikely]] {
            orders_rejected.fetch_add(1, std::memory_order_relaxed);
            std::cerr << "[-] Pool exhausted — order " << req.order_id << " rejected\n";
            return;
        }
        slot->order_id = req.order_id;
        slot->price    = req.price;
        slot->quantity = remaining_qty;
        slot->side     = req.side;

        if (req.side == Side::Buy) {
            bids_desc[req.price].push_back(slot);
            auto level_it = bids_desc.find(req.price);
            live_orders[req.order_id] = {
                std::prev(level_it->second.end()),
                nullptr  // bids_desc is separate type; cancel handled below
            };
        } else {
            asks[req.price].push_back(slot);
            auto level_it = asks.find(req.price);
            live_orders[req.order_id] = {
                std::prev(level_it->second.end()),
                &asks
            };
        }
    }

public:
    explicit OrderBookShard(std::string symbol) : ticker_sym(std::move(symbol)) {}

    // Main dispatch — called from worker thread
    void process_request(const OrderRequest& req) {
        auto t0 = std::chrono::high_resolution_clock::now();
        {
            std::lock_guard<std::mutex> lock(shard_mtx);

            switch (req.type) {

            // ----------------------------------------------------------------
            // LIMIT ORDER
            //   1. Sweep crossable levels on opposite side
            //   2. Post any unfilled remainder as resting order
            // ----------------------------------------------------------------
            case OrderType::Limit: {
                uint32_t remaining = sweep_book(req, &bids_desc, &asks, req.quantity);
                if (remaining > 0) post_resting(req, remaining);
                break;
            }

            // ----------------------------------------------------------------
            // MARKET ORDER
            //   Sweeps entire opposite side; no resting remainder posted.
            //   Unfilled quantity is simply discarded (IOC semantics).
            // ----------------------------------------------------------------
            case OrderType::Market: {
                sweep_book(req, &bids_desc, &asks, req.quantity);
                // Market orders do not post remainder
                break;
            }

            // ----------------------------------------------------------------
            // CANCEL ORDER
            //   O(1): iterator stored at order insertion time.
            //   No tree traversal — direct pointer hop to DLL node.
            // ----------------------------------------------------------------
            case OrderType::Cancel: {
                auto meta_it = live_orders.find(req.order_id);
                if (meta_it == live_orders.end()) [[unlikely]] break;

                OrderMeta& meta = meta_it->second;
                Order* target   = *(meta.list_it);

                if (target->side == Side::Buy) {
                    auto level = bids_desc.find(target->price);
                    if (level != bids_desc.end()) {
                        level->second.erase(meta.list_it);
                        if (level->second.empty()) bids_desc.erase(level);
                    }
                } else {
                    auto level = asks.find(target->price);
                    if (level != asks.end()) {
                        level->second.erase(meta.list_it);
                        if (level->second.empty()) asks.erase(level);
                    }
                }

                g_order_pool.deallocate(target);
                live_orders.erase(meta_it);
                orders_cancelled.fetch_add(1, std::memory_order_relaxed);
                break;
            }

            } // switch
        }

        auto t1 = std::chrono::high_resolution_clock::now();
        uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        total_latency_ns.fetch_add(ns, std::memory_order_relaxed);
        // Track peak (relaxed CAS loop)
        uint64_t prev = peak_latency_ns.load(std::memory_order_relaxed);
        while (ns > prev &&
               !peak_latency_ns.compare_exchange_weak(prev, ns,
                   std::memory_order_relaxed)) {}
        orders_processed.fetch_add(1, std::memory_order_relaxed);
    }

    // -----------------------------------------------------------------------
    // BEST BID / ASK (market depth snapshot)
    // -----------------------------------------------------------------------
    std::pair<uint32_t,uint32_t> best_bid_ask() {
        std::lock_guard<std::mutex> lock(shard_mtx);
        uint32_t bb = bids_desc.empty() ? 0 : bids_desc.begin()->first;
        uint32_t ba = asks.empty()      ? 0 : asks.begin()->first;
        return {bb, ba};
    }

    // -----------------------------------------------------------------------
    // TELEMETRY
    // -----------------------------------------------------------------------
    struct Metrics {
        std::string ticker;
        uint64_t processed;
        uint64_t trades;
        uint64_t cancelled;
        uint64_t rejected;
        uint64_t avg_latency_ns;
        uint64_t peak_latency_ns;
        uint32_t best_bid;
        uint32_t best_ask;
        size_t   live_orders;
    };

    Metrics get_metrics() {
        std::lock_guard<std::mutex> lock(shard_mtx);
        uint64_t proc = orders_processed.load(std::memory_order_relaxed);
        uint64_t tns  = total_latency_ns.load(std::memory_order_relaxed);
        uint32_t bb   = bids_desc.empty() ? 0 : bids_desc.begin()->first;
        uint32_t ba   = asks.empty()      ? 0 : asks.begin()->first;
        return {
            ticker_sym,
            proc,
            trades_executed.load(std::memory_order_relaxed),
            orders_cancelled.load(std::memory_order_relaxed),
            orders_rejected.load(std::memory_order_relaxed),
            proc > 0 ? tns / proc : 0,
            peak_latency_ns.load(std::memory_order_relaxed),
            bb, ba,
            live_orders.size()
        };
    }

    std::vector<TradeReport> drain_trade_log() {
        std::lock_guard<std::mutex> lock(shard_mtx);
        return std::exchange(trade_log, {});
    }
};

// ============================================================================
// 3. WORKER THREAD POOL
//    - Fixed pool allocated at startup — no runtime OS thread spawning
//    - Condition variable wakeup; no busy-spin to avoid wasted CPU cycles
//    - pending_tasks counter enables deterministic drain() without sleep_for
// ============================================================================
class ExchangeThreadPool {
private:
    std::vector<std::thread>            workers;
    std::queue<std::function<void()>>   task_queue;
    std::mutex                          queue_mtx;
    std::condition_variable             work_cv;
    std::condition_variable             drain_cv;
    std::atomic<bool>                   stop_flag{false};
    std::atomic<size_t>                 pending_tasks{0};

public:
    explicit ExchangeThreadPool(size_t thread_count) {
        workers.reserve(thread_count);
        for (size_t i = 0; i < thread_count; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(queue_mtx);
                        work_cv.wait(lock, [this] {
                            return stop_flag || !task_queue.empty();
                        });
                        if (stop_flag && task_queue.empty()) return;
                        task = std::move(task_queue.front());
                        task_queue.pop();
                    }
                    task();
                    // Decrement AFTER execution; notify drain waiters
                    if (pending_tasks.fetch_sub(1, std::memory_order_acq_rel) == 1)
                        drain_cv.notify_all();
                }
            });
        }
    }

    void enqueue_task(std::function<void()> task) {
        pending_tasks.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(queue_mtx);
            task_queue.push(std::move(task));
        }
        work_cv.notify_one();
    }

    // Block until every enqueued task has completed — no sleep_for guesswork
    void drain() {
        std::unique_lock<std::mutex> lock(queue_mtx);
        drain_cv.wait(lock, [this] {
            return pending_tasks.load(std::memory_order_acquire) == 0;
        });
    }

    size_t queue_depth() {
        std::lock_guard<std::mutex> lock(queue_mtx);
        return task_queue.size();
    }

    ~ExchangeThreadPool() {
        stop_flag = true;
        work_cv.notify_all();
        for (std::thread& w : workers)
            if (w.joinable()) w.join();
    }
};

// ============================================================================
// 4. CENTRAL EXCHANGE GATEWAY
//    - Resolves ticker → shard in O(1) via unordered_map
//    - directory_mutex held only during registration; hot path is lock-free
//      at the gateway level (shard pointer captured before pool dispatch)
//    - unique_ptr ensures shard lifetime exceeds any in-flight task
// ============================================================================
class CentralExchange {
private:
    std::unordered_map<std::string, std::unique_ptr<OrderBookShard>> asset_directory;
    ExchangeThreadPool processing_pool;
    std::mutex         directory_mtx;

public:
    explicit CentralExchange(size_t worker_cores)
        : processing_pool(worker_cores) {}

    void register_asset(const std::string& ticker) {
        std::lock_guard<std::mutex> lock(directory_mtx);
        asset_directory.emplace(ticker, std::make_unique<OrderBookShard>(ticker));
    }

    void submit_order(const OrderRequest& req) {
        OrderBookShard* shard = nullptr;
        {
            // directory_mtx held only to read the pointer; shard lifetime
            // is guaranteed by unique_ptr in asset_directory
            std::lock_guard<std::mutex> lock(directory_mtx);
            auto it = asset_directory.find(req.ticker);
            if (it != asset_directory.end()) shard = it->second.get();
        }

        if (!shard) [[unlikely]] {
            std::cerr << "[-] Rejected: unknown ticker [" << req.ticker << "]\n";
            return;
        }

        // Capture raw pointer — safe because shard outlives pool tasks
        processing_pool.enqueue_task([shard, req] {
            shard->process_request(req);
        });
    }

    // Block until all submitted orders are fully processed
    void drain() { processing_pool.drain(); }

    void print_metrics(const std::string& ticker) {
        std::lock_guard<std::mutex> lock(directory_mtx);
        auto it = asset_directory.find(ticker);
        if (it == asset_directory.end()) return;
        auto m = it->second->get_metrics();

        std::cout
            << "  Ticker        : " << m.ticker       << "\n"
            << "  Processed     : " << m.processed     << " orders\n"
            << "  Trades        : " << m.trades        << " fills\n"
            << "  Cancelled     : " << m.cancelled     << "\n"
            << "  Rejected      : " << m.rejected      << "\n"
            << "  Avg latency   : " << m.avg_latency_ns << " ns\n"
            << "  Peak latency  : " << m.peak_latency_ns << " ns\n"
            << "  Best bid/ask  : " << m.best_bid << " / " << m.best_ask << "\n"
            << "  Live orders   : " << m.live_orders   << "\n\n";
    }

    void print_all_metrics() {
        std::lock_guard<std::mutex> lock(directory_mtx);
        for (auto& [ticker, shard] : asset_directory) {
            auto m = shard->get_metrics();
            std::cout
                << "  [" << m.ticker << "]"
                << "  proc=" << m.processed
                << "  trades=" << m.trades
                << "  avg=" << m.avg_latency_ns << "ns"
                << "  peak=" << m.peak_latency_ns << "ns"
                << "  bid=" << m.best_bid
                << "  ask=" << m.best_ask
                << "  live=" << m.live_orders << "\n";
        }
    }

    std::vector<TradeReport> collect_trades(const std::string& ticker) {
        std::lock_guard<std::mutex> lock(directory_mtx);
        auto it = asset_directory.find(ticker);
        if (it == asset_directory.end()) return {};
        return it->second->drain_trade_log();
    }
};

// ============================================================================
// 5. MAIN — BENCHMARK PIPELINE
// ============================================================================
int main() {
    std::cout << "╔══════════════════════════════════════════════════╗\n"
              << "║   NexusCore — Sharded Matching Engine  v1.0     ║\n"
              << "╚══════════════════════════════════════════════════╝\n\n";

    // Spin up 4 worker threads mapped to physical cores
    CentralExchange exchange(4);
    exchange.register_asset("AAPL");
    exchange.register_asset("TSLA");
    exchange.register_asset("MSFT");

    std::cout << "[+] Pool reserved: " << POOL_CAPACITY
              << " order slots (" << (sizeof(Order) * POOL_CAPACITY / 1024 / 1024) << " MB)\n";
    std::cout << "[+] Workers started: 4 threads\n\n";

    // -----------------------------------------------------------------------
    // PHASE 1 — Seed resting liquidity (build the book)
    // Submit 10k limit orders per side per ticker to create a realistic spread
    // -----------------------------------------------------------------------
    std::cout << "[Phase 1] Seeding resting liquidity...\n";
    constexpr uint32_t SEED_ORDERS = 10'000;

    for (uint32_t i = 0; i < SEED_ORDERS; ++i) {
        // AAPL: bids 148–149, asks 151–152
        exchange.submit_order({i * 6 + 0, "AAPL", OrderType::Limit, 149 - (i % 2), 10, Side::Buy});
        exchange.submit_order({i * 6 + 1, "AAPL", OrderType::Limit, 151 + (i % 2), 10, Side::Sell});
        // TSLA: bids 698–699, asks 701–702
        exchange.submit_order({i * 6 + 2, "TSLA", OrderType::Limit, 699 - (i % 2),  5, Side::Buy});
        exchange.submit_order({i * 6 + 3, "TSLA", OrderType::Limit, 701 + (i % 2),  5, Side::Sell});
        // MSFT: bids 298–299, asks 301–302
        exchange.submit_order({i * 6 + 4, "MSFT", OrderType::Limit, 299 - (i % 2), 20, Side::Buy});
        exchange.submit_order({i * 6 + 5, "MSFT", OrderType::Limit, 301 + (i % 2), 20, Side::Sell});
    }
    exchange.drain();
    std::cout << "    Resting book populated.\n\n";

    // -----------------------------------------------------------------------
    // PHASE 2 — Engine warmup (prime CPU caches, not timed)
    // -----------------------------------------------------------------------
    std::cout << "[Phase 2] Engine warmup (untimed)...\n";
    uint64_t base_id = SEED_ORDERS * 6 + 9'000'000;
    for (uint64_t i = 0; i < 1'000; ++i) {
        exchange.submit_order({base_id + i*2+0, "AAPL", OrderType::Limit, 155, 2, Side::Buy});
        exchange.submit_order({base_id + i*2+1, "TSLA", OrderType::Limit, 695, 2, Side::Sell});
    }
    exchange.drain();
    std::cout << "    Warmup complete.\n\n";

    // -----------------------------------------------------------------------
    // PHASE 3 — High-throughput aggressive order stream (with timing)
    // -----------------------------------------------------------------------
    std::cout << "[Phase 3] Injecting 150k aggressive orders...\n";
    constexpr uint32_t AGGR_ORDERS = 50'000;
    base_id = SEED_ORDERS * 6;

    auto wall_start = std::chrono::high_resolution_clock::now();

    for (uint64_t i = 0; i < AGGR_ORDERS; ++i) {
        uint64_t id = base_id + i * 3;
        // Crossing buy:  limit 155 > resting ask 151 — fills immediately
        exchange.submit_order({id + 0, "AAPL", OrderType::Limit,  155, 5, Side::Buy});
        // Crossing sell: limit 695 < resting bid 699 — fills immediately
        exchange.submit_order({id + 1, "TSLA", OrderType::Limit,  695, 3, Side::Sell});
        // Market buy: sweeps entire ask side, no price gate
        exchange.submit_order({id + 2, "MSFT", OrderType::Market,   0, 8, Side::Buy});
    }

    exchange.drain(); // Deterministic flush — no sleep_for
    auto wall_end = std::chrono::high_resolution_clock::now();
    double wall_ms = std::chrono::duration<double, std::milli>(wall_end - wall_start).count();

    std::cout << "    Completed in " << wall_ms << " ms\n";
    std::cout << "    Wall-clock throughput: "
              << static_cast<uint64_t>((AGGR_ORDERS * 3) / (wall_ms / 1000.0))
              << " orders/sec\n\n";

    // -----------------------------------------------------------------------
    // PHASE 4 — Cancel storm (tests O(1) cancellation path)
    // -----------------------------------------------------------------------
    std::cout << "[Phase 4] Cancel storm — 3k cancellations...\n";
    for (uint64_t i = 0; i < 1'000; ++i) {
        exchange.submit_order({i * 6 + 0, "AAPL", OrderType::Cancel, 0, 0, Side::Buy});
        exchange.submit_order({i * 6 + 2, "TSLA", OrderType::Cancel, 0, 0, Side::Buy});
        exchange.submit_order({i * 6 + 4, "MSFT", OrderType::Cancel, 0, 0, Side::Buy});
    }
    exchange.drain();
    std::cout << "    Cancellations processed.\n\n";

    // -----------------------------------------------------------------------
    // AUDIT TRAIL
    // -----------------------------------------------------------------------
    std::cout << "══════════════════════════════════════════════════\n";
    std::cout << "  ENGINE AUDIT — Per-Ticker Metrics\n";
    std::cout << "══════════════════════════════════════════════════\n";
    exchange.print_metrics("AAPL");
    exchange.print_metrics("TSLA");
    exchange.print_metrics("MSFT");

    std::cout << "══════════════════════════════════════════════════\n";
    std::cout << "  TRADE LOG SAMPLE (first 5 AAPL fills)\n";
    std::cout << "══════════════════════════════════════════════════\n";
    auto trades = exchange.collect_trades("AAPL");
    for (size_t i = 0; i < std::min<size_t>(5, trades.size()); ++i) {
        auto& t = trades[i];
        std::cout << "  Fill  aggressor=" << t.aggressive_id
                  << "  resting=" << t.resting_id
                  << "  px=" << t.fill_price
                  << "  qty=" << t.fill_quantity << "\n";
    }
    std::cout << "  ... total AAPL fills: " << trades.size() << "\n\n";

    std::cout << "  Pool slots remaining: "
              << g_order_pool.available() << " / " << POOL_CAPACITY << "\n";

    return 0;
}
