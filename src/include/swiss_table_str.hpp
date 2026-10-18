#pragma once
#ifndef HT_MAXLOAD
#define HT_MAXLOAD 0.8
#endif
// =============================================================================
//  SwissTableStr - clés STRING (std::string), test de généralité (#4)
// =============================================================================
//  La clé est stockée HORS-LIGNE (keystore) ; le chunk ne contient que le
//  fingerprint (octet de contrôle) + un index 32 bits vers la clé. Un lookup :
//    stage 0  charge le chunk (1 ligne), SIMD-match le fingerprint ;
//    stage 1  sur match, déréférence keystore[idx] (une 2e ligne, FROIDE) et
//             compare la chaîne - ce déréférencement est le défaut supplémentaire
//             que TOUTE map paie sur des clés non triviales ; le moteur AMAC le
//             RECOUVRE entre requêtes indépendantes (comme la ligne du chunk).
//  But : montrer que le co-design (chunk + AMAC) se généralise aux clés string.
#include "chunk_simd.hpp"
#include <cstdint>
#include <vector>
#include <string>
#include <bit>
#include <functional>

template<typename Value>
class SwissTableStr {
    static constexpr int    SLOTS    = 7;
    static constexpr int    LANEMASK = 0x7F;
    static constexpr int8_t EMPTY    = -128;
    static constexpr uint64_t PHI    = 0x9E3779B97F4A7C15ULL;
    static constexpr uint32_t NONE   = 0xFFFFFFFFu;

    struct alignas(64) Chunk { int8_t ctrl[8]; uint32_t idx[SLOTS]; }; // 8 + 28 = 36 -> 64
    std::vector<Chunk> chunks;
    std::vector<std::string> keystore;   // clés hors-ligne
    std::vector<Value>       valstore;
    size_t num_chunks, size; int index_shift;

    static uint64_t hash(const std::string& k){ return (uint64_t)std::hash<std::string>{}(k) * PHI; }
    inline void route(uint64_t h,size_t&c,int8_t&sig)const{ c=h>>index_shift; sig=(int8_t)((h>>(index_shift-7))&0x7F); }
    static Chunk empty_chunk(){ Chunk c; for(int i=0;i<8;++i)c.ctrl[i]=EMPTY; for(int i=0;i<SLOTS;++i)c.idx[i]=NONE; return c; }

    void rehash(size_t nc){
        nc=std::bit_ceil(nc); if(nc<2)nc=2;
        std::vector<Chunk> old=std::move(chunks);
        num_chunks=nc; index_shift=64-std::countr_zero(num_chunks);
        chunks.assign(num_chunks,empty_chunk());
        for(auto&ch:old) for(int b=0;b<SLOTS;++b) if(ch.ctrl[b]!=EMPTY) place(ch.idx[b]);
    }
    // (ré)insère un index déjà présent dans keystore
    void place(uint32_t ki){
        uint64_t h=hash(keystore[ki]); int8_t sig; size_t c; route(h,c,sig);
        while(true){ Chunk&ch=chunks[c];
            __m128i ctrl=_mm_loadu_si128((const __m128i*)ch.ctrl);
            int m=_mm_movemask_epi8(ctrl)&LANEMASK;   // EMPTY (bit de signe)
            if(m){ int b=std::countr_zero((unsigned)m); ch.ctrl[b]=sig; ch.idx[b]=ki; return; }
            c=(c+1)&(num_chunks-1);
        }
    }
public:
    SwissTableStr(size_t cap=16):num_chunks(0),size(0){ rehash(cap/SLOTS+2); }
    size_t count() const { return size; }

    void insert(const std::string& key, const Value& val){
        if(size>=num_chunks*SLOTS*HT_MAXLOAD) rehash(num_chunks*2);
        uint32_t ki=(uint32_t)keystore.size(); keystore.push_back(key); valstore.push_back(val);
        place(ki); size++;
    }

    // Recherche par LOT, AMAC 2-étages (chunk -> keystore), recouvre le déréf clé.
    template<int DEPTH>
    void find_batch(const std::string* qs, size_t n, Value* out){
        struct Lane { size_t qi; size_t c; int mask; int8_t sig; uint8_t stage; uint32_t cand; bool active; };
        Lane L[DEPTH]; size_t next=0; int active=0;
        auto load=[&](int l){
            if(next>=n){ L[l].active=false; return; }
            size_t qi=next++; uint64_t h=hash(qs[qi]); int8_t sig; size_t c; route(h,c,sig);
            L[l]=Lane{qi,c,0,sig,0,0,true}; __builtin_prefetch(&chunks[c],0,1); ++active;
        };
        for(int l=0;l<DEPTH;++l) load(l);
        while(active>0){
            for(int l=0;l<DEPTH;++l){
                if(!L[l].active) continue;
                if(L[l].stage==0){                          // SCAN chunk
                    Chunk&ch=chunks[L[l].c];
                    __m128i ctrl=_mm_loadu_si128((const __m128i*)ch.ctrl);
                    int sm=_mm_movemask_epi8(_mm_cmpeq_epi8(ctrl,_mm_set1_epi8(L[l].sig)))&LANEMASK;
                    if(sm){ int b=std::countr_zero((unsigned)sm); L[l].mask=sm&(sm-1);
                        L[l].cand=ch.idx[b]; __builtin_prefetch(keystore[L[l].cand].data(),0,1);
                        __builtin_prefetch(&keystore[L[l].cand],0,1); L[l].stage=1; continue; }
                    int em=_mm_movemask_epi8(_mm_cmpeq_epi8(ctrl,_mm_set1_epi8(EMPTY)))&LANEMASK;
                    if(em){ out[L[l].qi]=Value{}; --active; load(l); }          // absent
                    else { L[l].c=(L[l].c+1)&(num_chunks-1); __builtin_prefetch(&chunks[L[l].c],0,1); }
                } else {                                     // VERIFY out-of-line key
                    if(keystore[L[l].cand]==qs[L[l].qi]){ out[L[l].qi]=valstore[L[l].cand]; --active; load(l); }
                    else if(L[l].mask){ int b=std::countr_zero((unsigned)L[l].mask); L[l].mask&=L[l].mask-1;
                        Chunk&ch=chunks[L[l].c]; L[l].cand=ch.idx[b]; __builtin_prefetch(keystore[L[l].cand].data(),0,1); }
                    else { L[l].stage=0; L[l].c=(L[l].c+1)&(num_chunks-1); __builtin_prefetch(&chunks[L[l].c],0,1); }
                }
            }
        }
    }
};
