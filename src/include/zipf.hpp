#pragma once
// Générateur Zipf - échantillonnage par rejection-inversion (Hörmann & Derflinger,
// 1996), tel qu'implémenté dans Apache Commons RNG. O(1) par tirage, aucune table
// O(N) : adapté à des tables de centaines de millions d'éléments.
//
//   rang k in [1,N] tiré avec P(k) ∝ 1 / k^s   (s = exposant ; s=0 -> uniforme)
//   Plus s est grand, plus le "hot set" est concentré (ex. YCSB : s=0.99).

#include <cstdint>
#include <cmath>
#include <random>

class ZipfGen {
    double exponent_;
    double n_;
    double hx1_;       // hIntegral(1.5) - 1
    double hN_;        // hIntegral(N + 0.5)
    double s_;         // constante de rejet

    static double helper1(double x) { return std::fabs(x) > 1e-8 ? std::log1p(x) / x : 1.0 - 0.5 * x; }
    static double helper2(double x) { return std::fabs(x) > 1e-8 ? std::expm1(x) / x : 1.0 + 0.5 * x; }
    double hIntegral(double x) const { double lx = std::log(x); return helper2((1.0 - exponent_) * lx) * lx; }
    double h(double x) const { return std::exp(-exponent_ * std::log(x)); }
    double hIntegralInverse(double x) const {
        double t = x * (1.0 - exponent_);
        if (t < -1.0) t = -1.0;
        return std::exp(helper1(t) * x);
    }

public:
    ZipfGen(uint64_t n, double exponent) : exponent_(exponent), n_((double)n) {
        hx1_ = hIntegral(1.5) - 1.0;
        hN_  = hIntegral(n_ + 0.5);
        s_   = 2.0 - hIntegralInverse(hIntegral(2.5) - h(2.0));
    }

    // Renvoie un rang dans [1, N].
    template <class Rng>
    uint64_t sample(Rng& rng) const {
        std::uniform_real_distribution<double> U(0.0, 1.0);
        for (;;) {
            double u = hN_ + U(rng) * (hx1_ - hN_); // uniforme dans [hx1_, hN_]
            double x = hIntegralInverse(u);
            uint64_t k = (uint64_t)(x + 0.5);
            if (k < 1) k = 1; else if ((double)k > n_) k = (uint64_t)n_;
            if (k - x <= s_ || u >= hIntegral((double)k + 0.5) - h((double)k))
                return k;
        }
    }
};
