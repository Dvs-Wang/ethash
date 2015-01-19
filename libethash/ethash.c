/*
  This file is part of cpp-ethereum.

  cpp-ethereum is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  cpp-ethereum is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file dash.cpp
* @author Tim Hughes <tim@ethdev.org>
* @date 2015
*/

#include <stdio.h>
#include <assert.h>
#include "ethash.h"
#include "blum_blum_shub.h"

#ifdef WITH_CRYPTOPP
#include "sha3_cryptopp.h"
#else
#include "sha3.h"
#endif // WITH_CRYPTOPP

// compile time settings
#define NODE_WORDS 8
#define PAGE_WORDS 512
#define PAGE_NODES (PAGE_WORDS / NODE_WORDS)

typedef union node {
    uint8_t bytes[NODE_WORDS * 8];
    uint64_t words[NODE_WORDS];
} node;


// todo
static void ethash_compute_cache_nodes(node *nodes, ethash_params const *params) {
    assert((params->cache_size % sizeof(node)) == 0);
    const size_t num_nodes = params->cache_size / sizeof(node);


    sha3_512(nodes[0].bytes, params->seed, 32);

    for (unsigned i = 1; i != num_nodes; ++i) {
        sha3_512(nodes[i].bytes, nodes[i - 1].bytes, 64);
    }

    for (unsigned i = 0; i != num_nodes; ++i) {
        // todo: endianness
        unsigned const idx = (unsigned int const) (nodes[i].words[0] % num_nodes);
        sha3_512(nodes[i].bytes, nodes[idx].bytes, 64);
    }
}

void ethash_compute_cache_data(ethash_cache *cache, ethash_params const *params) {
    node *nodes = (node *) cache->mem;
    ethash_compute_cache_nodes(nodes, params);

    // todo, endian
    uint32_t rng_seed = make_seed(nodes->words[0], SAFE_PRIME);
    init_power_table_mod_prime1(cache->rng_table, rng_seed);
}

static void ethash_compute_full_node(
        node *const ret,
        const unsigned node_index,
        ethash_params const *params,
        node const *nodes,
        uint32_t const *rng_table
) {
    size_t num_parent_nodes = params->cache_size / sizeof(node);
    assert(node_index >= num_parent_nodes);

    for (unsigned w = 0; w != NODE_WORDS; ++w) {
        ret->words[w] = 0;
    }

    uint32_t rand = quick_bbs(rng_table, node_index * 2);
    for (unsigned i = 0; i != params->k; ++i) {

        size_t parent_index = rand % num_parent_nodes;
        rand = cube_mod_safe_prime2(rand);

        for (unsigned w = 0; w != NODE_WORDS; ++w) {
            ret->words[w] ^= nodes[parent_index].words[w];
        }
    }
}

void ethash_compute_full_data(void *mem, ethash_params const *params) {
    assert((params->full_size % (sizeof(uint64_t) * PAGE_WORDS)) == 0);
    assert((params->full_size % sizeof(node)) == 0);
    node *nodes = (node *) mem;

    // compute cache nodes first
    ethash_cache cache;
    ethash_cache_init(&cache, mem);
    ethash_compute_cache_data(&cache, params);

    // now compute full nodes
    for (unsigned n = params->cache_size / sizeof(node); n != params->full_size / sizeof(node); ++n) {
        ethash_compute_full_node(&nodes[n], n, params, nodes, cache.rng_table);
    }
}

static void ethash_hash(uint8_t ret[32], void const *mem, uint32_t const *rng_table, ethash_params const *params, uint64_t nonce) {
    // for simplicity the cache and dag must be whole number of pages
    assert((params->cache_size % PAGE_WORDS) == 0);
    assert((params->full_size % PAGE_WORDS) == 0);
    node const *nodes = (node const *) mem;

    // pack seed and nonce together, todo, endian
    struct {
        uint8_t seed[32];
        uint64_t nonce;
    } init;
    memcpy(init.seed, params->seed, sizeof(params->seed));
    init.nonce = nonce;

    // compute sha3-256 hash and replicate across mix
    uint64_t mix[PAGE_WORDS];
    sha3_256((uint8_t *const) mix, (uint8_t const *) &init, sizeof(init));
    for (unsigned w = 4; w != PAGE_WORDS; ++w) {
        mix[w] = mix[w & 3];
    }

    // seed for RNG variant
    uint32_t rand = (uint32_t) mix[0];

    unsigned const page_size = sizeof(uint64_t) * PAGE_WORDS;
    unsigned const page_reads = params->hash_read_size / page_size;
    unsigned num_full_pages = params->full_size / page_size;
    unsigned num_cache_pages = params->cache_size / page_size;
    unsigned num_pages = rng_table ? num_cache_pages : num_full_pages;

    for (unsigned i = 0; i != page_reads; ++i) {
        // most architectures are little-endian now, we can speed things up
        // by specifying the byte order as little endian for this part.
        uint64_t mixi = mix[i % PAGE_WORDS];

        unsigned index = rand % num_full_pages;
        rand = cube_mod_safe_prime1(rand);

        node tmp_page[PAGE_NODES];
        node const *page;
        if (index >= num_pages) {
            page = tmp_page;

            for (unsigned n = 0; n != PAGE_NODES; ++n) {
                ethash_compute_full_node(&tmp_page[n], index * PAGE_NODES + n, params, nodes, rng_table);
            }
        }
        else {
            page = nodes + PAGE_NODES * index;
        }

        for (unsigned w = 0; w != PAGE_WORDS; ++w) {
            mix[w] ^= page->words[w];
        }
    }

    uint8_t tmp[32];
    sha3_256((uint8_t *const) &tmp, (uint8_t const *) mix, sizeof(mix));
    sha3_256(ret, (uint8_t const *) &tmp, sizeof(tmp));
}

void ethash_full(uint8_t ret[32], void const *full_mem, ethash_params const *params, uint64_t nonce) {
    ethash_hash(ret, (node const *) full_mem, NULL, params, nonce);
}

void ethash_light(uint8_t ret[32], ethash_cache const *cache, ethash_params const *params, uint64_t nonce) {
    ethash_hash(ret, (node const *) cache->mem, cache->rng_table, params, nonce);
}
