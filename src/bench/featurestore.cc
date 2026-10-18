// featurestore.cc - a feature-store multi-get workload on the Inline table vs Boost and Abseil.
// Entities are 64-bit IDs (hashed, as feature stores usually carry them); the value is a 64-bit row offset
// into a feature/embedding store that is fetched separately.  A request is a multi-get of k IDs, k ~ lognormal
// (median KMED, sigma KSIG, clipped to [1, 512]); IDs are Zipf(s) over N entities, absent with probability 1-h.
// Requests arrive as a Poisson process at LAMBDA requests/s.  Service times are measured on the real machine;
// arrivals and queueing are simulated in virtual time (single server, FIFO), so the reported latency is the
// response time a request sees: queueing + batch accumulation + service.
//   engines:  inline-batch     the multi-get itself is the batch (find_batch<32> over its k IDs)
//             inline-coalesce  consecutive requests coalesced up to BMAX IDs or TAU us, then one find_batch
//             inline-sync, boost, abseil   a scalar find per ID
//
// Version 2.  Changes against the first version:
//   * ids_per_s_closed is measured with the SAME batching policy the engine uses under load.  For the coalescing
//     engine that is closed-loop service of consecutive requests packed into batches of >= BMAX IDs; the first
//     version measured it per request, which is why its "capacity" (130 M/s) contradicted its utilisation (0.79 at
//     156 M offered/s).  The per-request capacity is still reported as ids_per_s_closed_req for every engine.
//   * every run is repeated --reps times with a fresh trace and fresh arrivals (seed+rep); one CSV row per rep.
//   * service-time diagnostics per row: p50/p99/max of the measured service samples, the number of samples above
//     100 us (OS preemption shows up here), the timer overhead, the number of requests, and the virtual horizon.
//   * --requests, --bmax are parameters; the QoS call is macOS-only; Linux builds pin with taskset outside.
//   No request is dropped, deduplicated or reordered inside a batch; completed = offered in every row.
//   CSV: config,rep,engine,lambda_req_s,tau_us,offered_ids_s,util,p50_us,p95_us,p99_us,p999_us,mean_us,
//        ids_per_s_closed,ids_per_s_closed_req,svc_p50_us,svc_p99_us,svc_max_us,svc_over100us,svc_samples,
//        timer_ns,requests,mean_k,horizon_us
#include "swiss_table_chunkv.hpp"
#include <absl/container/flat_hash_map.h>
#include <boost/unordered/unordered_flat_map.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <random>
#include <chrono>
#include <algorithm>
#include <memory>
#include <functional>
#include <string>
#ifdef __APPLE__
#include <pthread.h>
#include <sys/qos.h>
#endif

static inline uint64_t splitmix64(uint64_t x){ x+=0x9e3779b97f4a7c15ULL; x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL; x=(x^(x>>27))*0x94d049bb133111ebULL; return x^(x>>31); }
static inline uint64_t row_offset(uint64_t key){ return (key * 2654435761ULL + 1) & ~0x3FULL; }   // a 64-byte-aligned offset, never 0
using Clock = std::chrono::steady_clock;
static inline double now_us(){ return std::chrono::duration<double, std::micro>(Clock::now().time_since_epoch()).count(); }

struct Zipf {   // rejection-inversion (Hörmann & Derflinger), ranks 1..n
    double s, n, hx1, hn, sfix;
    static double h1(double x){ return std::fabs(x) > 1e-8 ? std::log1p(x) / x : 1 - 0.5 * x; }
    static double h2(double x){ return std::fabs(x) > 1e-8 ? std::expm1(x) / x : 1 + 0.5 * x; }
    double hI(double x) const { double lx = std::log(x); return h2((1 - s) * lx) * lx; }
    double h(double x) const { return std::exp(-s * std::log(x)); }
    double hIinv(double x) const { double t = x * (1 - s); if (t < -1) t = -1; return std::exp(h1(t) * x); }
    Zipf(uint64_t n_, double s_) : s(s_), n((double)n_) { hx1 = hI(1.5) - 1; hn = hI(n + 0.5); sfix = 2 - hIinv(hI(2.5) - h(2)); }
    template<class R> uint64_t operator()(R& r) const { std::uniform_real_distribution<double> U(0, 1);
        for (;;) { double u = hn + U(r) * (hx1 - hn), x = hIinv(u); uint64_t k = (uint64_t)(x + 0.5); if (k < 1) k = 1; else if ((double)k > n) k = (uint64_t)n;
                   if (k - x <= sfix || u >= hI((double)k + 0.5) - h((double)k)) return k; } }
};

struct Trace { std::vector<uint64_t> ids; std::vector<size_t> off; std::vector<uint64_t> expect; size_t R; };
static Trace make_trace(size_t R, uint64_t N, double zipf_s, double hit, double kmed, double ksig, uint64_t seed){
    std::mt19937_64 rng(seed); std::lognormal_distribution<double> K(std::log(kmed), ksig); std::uniform_real_distribution<double> U(0, 1);
    Zipf z(N, zipf_s); Trace t; t.R = R; t.off.reserve(R + 1); t.off.push_back(0);
    for (size_t i = 0; i < R; i++) { size_t k = (size_t)std::clamp(std::llround(K(rng)), 1LL, 512LL);
        for (size_t j = 0; j < k; j++) { bool present = U(rng) < hit; uint64_t id = present ? splitmix64(z(rng) - 1) : splitmix64(N + (rng() % (3 * N)));
            t.ids.push_back(id); t.expect.push_back(present ? row_offset(id) : 0); }
        t.off.push_back(t.ids.size()); }
    return t;
}
struct Stats { double p50, p95, p99, p999, mean, max; size_t n, over100; };
static Stats quantiles(std::vector<double>& v){ std::sort(v.begin(), v.end()); auto q = [&](double p){ return v[std::min(v.size() - 1, (size_t)(p * v.size()))]; };
    double m = 0; size_t o = 0; for (double x : v) { m += x; o += x > 100.0; } return {q(0.5), q(0.95), q(0.99), q(0.999), m / v.size(), v.back(), v.size(), o}; }

// service(lo, hi): looks up ids[lo, hi) into out[lo, hi) and returns wall-clock service time in microseconds
using Service = std::function<double(size_t, size_t)>;

struct SimOut { Stats resp; Stats svc; double util, horizon; };

// Poisson arrivals at lambda req/s; per-request engine: FIFO single server.
static SimOut simulate_per_request(const Trace& t, Service svc, double lambda, std::mt19937_64& rng){
    std::exponential_distribution<double> E(lambda / 1e6); std::vector<double> lat(t.R), sv; sv.reserve(t.R); double arr = 0, free_at = 0, busy = 0;
    for (size_t i = 0; i < t.R; i++) { arr += E(rng); double start = std::max(free_at, arr); double s = svc(t.off[i], t.off[i + 1]); busy += s; sv.push_back(s); free_at = start + s; lat[i] = free_at - arr; }
    return {quantiles(lat), quantiles(sv), busy / free_at, free_at};
}
// Coalescing engine: a batch closes when it holds >= bmax ids or when its oldest request has waited tau us (or the server was busy that long).
static SimOut simulate_coalesce(const Trace& t, Service svc, double lambda, double tau, size_t bmax, std::mt19937_64& rng){
    std::exponential_distribution<double> E(lambda / 1e6); std::vector<double> arr(t.R); double a = 0; for (size_t i = 0; i < t.R; i++) { a += E(rng); arr[i] = a; }
    std::vector<double> lat(t.R), sv; double free_at = 0, busy = 0; size_t i = 0;
    while (i < t.R) {
        double close = arr[i] + tau; size_t j = i, ids = 0;
        while (j < t.R && ids < bmax && arr[j] <= std::max(free_at, close)) { ids += t.off[j + 1] - t.off[j]; j++; }
        double start = std::max(free_at, ids >= bmax ? arr[j - 1] : std::min(close, arr[j - 1] > close ? arr[j - 1] : close));
        if (j == t.R && arr[j - 1] < close) start = std::max(free_at, arr[j - 1]);      // tail: nothing more will arrive
        double s = svc(t.off[i], t.off[j]); busy += s; sv.push_back(s); free_at = start + s;
        for (size_t r = i; r < j; r++) lat[r] = free_at - arr[r];
        i = j;
    }
    return {quantiles(lat), quantiles(sv), busy / free_at, free_at};
}

int main(int argc, char** argv){
    int LOG2N = 26; size_t R = 300000; double zipf_s = 0.99, hit = 0.95, kmed = 32, ksig = 1.0; std::string lambdas = "250000,500000,1000000,2000000,3000000", taus = "20,100"; std::string cfg = "default"; uint64_t seed = 42; int reps = 5; size_t bmax = 256;
    for (int i = 1; i < argc; i++) { std::string a = argv[i]; auto v = [&]{ return std::string(argv[++i]); };
        if (a == "--logn") LOG2N = atoi(v().c_str()); else if (a == "--requests") R = strtoull(v().c_str(), 0, 10); else if (a == "--zipf") zipf_s = atof(v().c_str());
        else if (a == "--hit") hit = atof(v().c_str()); else if (a == "--kmed") kmed = atof(v().c_str()); else if (a == "--ksig") ksig = atof(v().c_str());
        else if (a == "--lambdas") lambdas = v(); else if (a == "--taus") taus = v(); else if (a == "--config") cfg = v(); else if (a == "--seed") seed = strtoull(v().c_str(), 0, 10);
        else if (a == "--reps") reps = atoi(v().c_str()); else if (a == "--bmax") bmax = strtoull(v().c_str(), 0, 10);
        else { std::fprintf(stderr, "unknown arg %s\n", a.c_str()); return 1; } }
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    const uint64_t N = 1ull << LOG2N;
    auto split = [](const std::string& s){ std::vector<double> v; size_t a = 0; while (a < s.size()) { size_t b = s.find(',', a); if (b == std::string::npos) b = s.size(); v.push_back(atof(s.substr(a, b - a).c_str())); a = b + 1; } return v; };
    std::vector<double> LAM = split(lambdas), TAU = split(taus);
    // timer overhead: median of 1e5 back-to-back readings
    double timer_ns; { std::vector<double> d(100000); for (auto& x : d) { double a = now_us(), b = now_us(); x = (b - a) * 1e3; } std::sort(d.begin(), d.end()); timer_ns = d[d.size() / 2]; }
    std::fprintf(stderr, "config=%s N=2^%d requests=%zu reps=%d bmax=%zu zipf=%.2f hit=%.2f kmed=%.0f ksig=%.1f timer=%.0f ns\n", cfg.c_str(), LOG2N, R, reps, bmax, zipf_s, hit, kmed, ksig, timer_ns);
    std::printf("config,rep,engine,lambda_req_s,tau_us,offered_ids_s,util,p50_us,p95_us,p99_us,p999_us,mean_us,ids_per_s_closed,ids_per_s_closed_req,svc_p50_us,svc_p99_us,svc_max_us,svc_over100us,svc_samples,timer_ns,requests,mean_k,horizon_us\n");

    // the three tables are built once; every rep draws a fresh trace and fresh arrivals
    std::vector<uint64_t> keys(N); for (uint64_t i = 0; i < N; i++) keys[i] = splitmix64(i);
    auto inl = std::make_unique<SwissTableChunkV<uint64_t,uint64_t>>(size_t(N/0.7)+1); for (uint64_t k : keys) inl->insert(k, row_offset(k));
    auto bo = std::make_unique<boost::unordered_flat_map<uint64_t,uint64_t>>(); bo->reserve(N); for (uint64_t k : keys) (*bo)[k] = row_offset(k);
    auto ab = std::make_unique<absl::flat_hash_map<uint64_t,uint64_t>>(); ab->reserve(N); for (uint64_t k : keys) (*ab)[k] = row_offset(k);
    keys.clear(); keys.shrink_to_fit();

    for (int rep = 0; rep < reps; rep++) {
        Trace t = make_trace(R, N, zipf_s, hit, kmed, ksig, seed + 1000 * rep);
        double mean_k = (double)t.ids.size() / R;
        std::vector<uint64_t> out(t.ids.size());
        auto validate = [&](const char* eng){ for (size_t i = 0; i < t.ids.size(); i++) if (out[i] != t.expect[i]) { std::fprintf(stderr, "VALIDATION FAILED %s at %zu\n", eng, i); std::exit(1); } };
        auto closed_req = [&](Service svc){ double t0 = now_us(); for (size_t i = 0; i < t.R; i++) svc(t.off[i], t.off[i + 1]); return t.ids.size() / (now_us() - t0) * 1e6; };
        auto closed_coal = [&](Service svc){ double t0 = now_us(); size_t i = 0; while (i < t.R) { size_t j = i, ids = 0; while (j < t.R && ids < bmax) { ids += t.off[j + 1] - t.off[j]; j++; } svc(t.off[i], t.off[j]); i = j; } return t.ids.size() / (now_us() - t0) * 1e6; };
        auto emit = [&](const char* eng, double lam, double tau, const SimOut& o, double closed_ips, double closed_req_ips){
            std::printf("%s,%d,%s,%.0f,%.0f,%.0f,%.3f,%.2f,%.2f,%.2f,%.2f,%.2f,%.0f,%.0f,%.2f,%.2f,%.2f,%zu,%zu,%.0f,%zu,%.2f,%.0f\n", cfg.c_str(), rep, eng, lam, tau, lam * mean_k, o.util,
                o.resp.p50, o.resp.p95, o.resp.p99, o.resp.p999, o.resp.mean, closed_ips, closed_req_ips, o.svc.p50, o.svc.p99, o.svc.max, o.svc.over100, o.svc.n, timer_ns, t.R, mean_k, o.horizon); std::fflush(stdout); };
        auto run_engine = [&](const char* eng, Service svc, bool coalesce){
            std::mt19937_64 rng(seed + 1 + 1000 * rep); svc(0, t.ids.size()); validate(eng);              // warm-up over the whole trace, validated
            double creq = closed_req(svc); double ccoal = coalesce ? closed_coal(svc) : creq;
            for (double lam : LAM) {
                if (!coalesce) emit(eng, lam, 0, simulate_per_request(t, svc, lam, rng), creq, creq);
                else for (double tau : TAU) emit(eng, lam, tau, simulate_coalesce(t, svc, lam, tau, bmax, rng), ccoal, creq);
            }
        };
        Service batch = [&](size_t lo, size_t hi){ double t0 = now_us(); inl->find_batch<32>(t.ids.data() + lo, hi - lo, out.data() + lo); return now_us() - t0; };
        Service isync = [&](size_t lo, size_t hi){ double t0 = now_us(); for (size_t i = lo; i < hi; i++) { auto* p = inl->find_sync(t.ids[i]); out[i] = p ? *p : 0; } return now_us() - t0; };
        Service bsync = [&](size_t lo, size_t hi){ double t0 = now_us(); for (size_t i = lo; i < hi; i++) { auto it = bo->find(t.ids[i]); out[i] = it == bo->end() ? 0 : it->second; } return now_us() - t0; };
        Service async = [&](size_t lo, size_t hi){ double t0 = now_us(); for (size_t i = lo; i < hi; i++) { auto it = ab->find(t.ids[i]); out[i] = it == ab->end() ? 0 : it->second; } return now_us() - t0; };
        run_engine("inline-batch", batch, false); run_engine("inline-coalesce", batch, true); run_engine("inline-sync", isync, false);
        run_engine("boost", bsync, false); run_engine("abseil", async, false);
    }
    return 0;
}
