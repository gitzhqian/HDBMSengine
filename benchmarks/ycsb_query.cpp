#include <algorithm>
#include "query.h"
#include "ycsb_query.h"
#include "mem_alloc.h"
#include "wl.h"
#include "ycsb.h"
#include "table.h"
#include "benchmark_common.h"

uint64_t ycsb_query::the_n = 0;
double ycsb_query::denom = 0;

void ycsb_query::init(uint64_t thd_id, workload * h_wl, Query_thd * query_thd, std::vector<uint64_t> &insert_keys, UInt32 qid) {
	_query_thd = query_thd;
#if OLAP_ENABLE == true
    if (thd_id == (g_thread_cnt - 1)){
        requests = (ycsb_request *)  mem_allocator.alloc(sizeof(ycsb_request) * g_req_per_query_ap, thd_id);
    }else{
        requests = (ycsb_request *)  mem_allocator.alloc(sizeof(ycsb_request) * g_req_per_query, thd_id);
    }
#else
    requests = (ycsb_request *)  mem_allocator.alloc(sizeof(ycsb_request) * g_req_per_query, thd_id);
#endif

	part_to_access = (uint64_t *)  mem_allocator.alloc(sizeof(uint64_t) * g_part_per_txn, thd_id);
	zeta_2_theta = zeta(2, g_zipf_theta);
	assert(the_n != 0);
	assert(denom != 0);
	gen_requests(thd_id, h_wl, insert_keys, qid);
}

void 
ycsb_query::calculateDenom()
{
	assert(the_n == 0);
	uint64_t table_size = g_synth_table_size / g_virtual_part_cnt;
	the_n = table_size - 1;
	denom = zeta(the_n, g_zipf_theta);
}

// The following algorithm comes from the paper:
// Quickly generating billion-record synthetic databases
// However, it seems there is a small bug. 
// The original paper says zeta(theta, 2.0). But I guess it should be 
// zeta(2.0, theta).
double ycsb_query::zeta(uint64_t n, double theta) {
	double sum = 0;
	for (uint64_t i = 1; i <= n; i++) 
		sum += pow(1.0 / i, theta);
	return sum;
}

uint64_t ycsb_query::zipf(uint64_t n, double theta) {
	assert(this->the_n == n);
	assert(theta == g_zipf_theta);
	double alpha = 1 / (1 - theta);
	double zetan = denom;
	double eta = (1 - pow(2.0 / n, 1 - theta)) / 
		(1 - zeta_2_theta / zetan);
	double u; 
	drand48_r(&_query_thd->buffer, &u);
	double uz = u * zetan;
	if (uz < 1) return 1;
	if (uz < 1 + pow(0.5, theta)) return 2;
	return 1 + (uint64_t)(n * pow(eta*u -eta + 1, alpha));
}

void ycsb_query::gen_requests(uint64_t thd_id, workload * h_wl, std::vector<uint64_t> &insert_keys, UInt32 qid) {
#if CC_ALG == HSTORE
	assert(g_virtual_part_cnt == g_part_cnt);
#endif
	int access_cnt = 0;
	set<uint64_t> all_keys;
	part_num = 0;
	double r = 0;
	int64_t rint64 = 0;
	drand48_r(&_query_thd->buffer, &r);
	lrand48_r(&_query_thd->buffer, &rint64);
	if (r < g_perc_multi_part) {
		for (UInt32 i = 0; i < g_part_per_txn; i++) {
			if (i == 0 && FIRST_PART_LOCAL)
				part_to_access[part_num] = thd_id % g_virtual_part_cnt;
			else {
				part_to_access[part_num] = rint64 % g_virtual_part_cnt;
			}
			UInt32 j;
			for (j = 0; j < part_num; j++) 
				if ( part_to_access[part_num] == part_to_access[j] )
					break;
			if (j == part_num)
				part_num ++;
		}
	} else {
		part_num = 1;
		if (FIRST_PART_LOCAL)
			part_to_access[0] = thd_id % g_part_cnt;
		else
			part_to_access[0] = rint64 % g_part_cnt;
	}

	int rid = 0;

    //auto table_size_ = h_wl->tables["MAIN_TABLE"]->get_table_size();
    //ZipfDistribution zipf_s(table_size_ - 1, g_zipf_theta);
    //test for OLAP$OLTP simulating
    //gen all RD request for last thread
#if OLAP_ENABLE == true
    if (thd_id == (g_thread_cnt - 1)){
        for (UInt32 tmp = 0; tmp < g_req_per_query_ap; tmp ++) {
            double r;
            drand48_r(&_query_thd->buffer, &r);
            ycsb_request * req = &requests[rid];
            req->rtype = RO;

            // the request will access part_id.
            uint64_t ith = tmp * part_num / g_req_per_query;
            uint64_t part_id =  part_to_access[ ith ];
            uint64_t table_size = g_synth_table_size / g_virtual_part_cnt;
            uint64_t row_id = zipf(table_size - 1, g_zipf_theta);
            assert(row_id < table_size);
            uint64_t primary_key = row_id  ;
            req->key = primary_key;
            int64_t rint64;
            lrand48_r(&_query_thd->buffer, &rint64);
            req->value = rint64 % (1<<8);
            // Make sure a single row is not accessed twice
            all_keys.insert(req->key);

            rid ++;
        }

        request_cnt = rid;
    }else{
#endif
        for (UInt32 tmp = 0; tmp < g_req_per_query; tmp ++) {
            double r;
            drand48_r(&_query_thd->buffer, &r);
            ycsb_request * req = &requests[rid];
            if (g_read_perc == 1){
                req->rtype = RO;
            }else{
                if (r < g_read_perc) {
                    req->rtype = RD;
                } else if (r >= g_read_perc && r <= g_write_perc + g_read_perc) {
                    req->rtype = WR;
                } else if(r >= (g_write_perc + g_read_perc) && r <= (g_write_perc + g_read_perc + g_scan_perc)) {
                    req->rtype = SCAN;
                    req->scan_len = SCAN_LEN;
                } else{
                    req->rtype = INS;
                }
            }

            // the request will access part_id.
            uint64_t ith = tmp * part_num / g_req_per_query;
            uint64_t part_id =  part_to_access[ ith ];
            uint64_t table_size = g_synth_table_size / g_virtual_part_cnt;
            uint64_t row_id = zipf(table_size - 1, g_zipf_theta);
            assert(row_id < table_size);
            uint64_t primary_key = row_id * g_virtual_part_cnt + part_id;
            req->key = primary_key;
            int64_t rint64;
            lrand48_r(&_query_thd->buffer, &rint64);
            req->value = rint64 % (1<<8);
            // Make sure a single row is not accessed twice
            if (req->rtype == RD || req->rtype == RO || req->rtype == WR) {
                if (all_keys.find(req->key) == all_keys.end()) {
                    all_keys.insert(req->key);
                    access_cnt ++;
                } else {
                    continue;
                }
            } else if(req->rtype == INS){
//            req->key = h_wl->tables["MAIN_TABLE"]->get_next_row_id();
//            uint64_t insert_k = req->key + h_wl->tables["MAIN_TABLE"]->get_next_row_id();
                uint64_t insert_k = insert_keys[qid];
                req->key = insert_k;
                all_keys.insert(req->key);
                // all_keys.insert(row_id);
                access_cnt ++;
                g_key_order = false;
            } else {
                bool conflict = false;
                //row_id = zipf_s.GetNextNumber();
                if (all_keys.find(req->key) == all_keys.end()) {
                    all_keys.insert(req->key);
                    access_cnt ++;
                } else {
                    continue;
                }
            }
            rid ++;
        }

        request_cnt = rid;
        // Sort the requests in key order.
        if (g_key_order) {
            for (int i = request_cnt - 1; i > 0; i--)
                for (int j = 0; j < i; j ++)
                    if (requests[j].key > requests[j + 1].key) {
                        ycsb_request tmp = requests[j];
                        requests[j] = requests[j + 1];
                        requests[j + 1] = tmp;
                    }
            for (UInt32 i = 0; i < request_cnt - 1; i++){
                assert(requests[i].key < requests[i + 1].key);
            }

        }
#if OLAP_ENABLE == true
    }
#endif

}


