#include "spdk/env.h"
#include "spdk/nvme.h"
#include "spdk/nvme_spec.h"
#include <cstdint>
#include <vector>
using std::vector;

vector<spdk_nvme_ctrlr *> ctrlr_list;
vector<spdk_nvme_ns *> ns_list;

constexpr int64_t kPageSize = 4096;
constexpr int64_t kDataLength = kPageSize * 26;

bool finished = false;

bool probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
              struct spdk_nvme_ctrlr_opts *opts) {
  return true;
}

void attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
               struct spdk_nvme_ctrlr *ctrlr,
               const struct spdk_nvme_ctrlr_opts *opts) {
  printf("attach type %s, addr %s\n", trid->trstring, trid->traddr);
  int cnt = spdk_nvme_ctrlr_get_num_ns(ctrlr);
  for (int i = 1; i <= cnt; i++) {
    spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(ctrlr, i);
    if (!spdk_nvme_ns_is_active(ns)) {
      continue;
    }
    uint64_t ns_size = spdk_nvme_ns_get_size(ns);
    uint64_t sector_size = spdk_nvme_ns_get_sector_size(ns);
    uint64_t sector_num = spdk_nvme_ns_get_num_sectors(ns);
    printf("Namespace ID: %d size: %.2f GB, %.2fGiB, sector size %lu, sector "
           "num %lu\n",
           spdk_nvme_ns_get_id(ns), ns_size / 1000.0 / 1000.0 / 1000.0,
           ns_size / 1024.0 / 1024.0 / 1024.0, sector_size, sector_num);
    ctrlr_list.push_back(ctrlr);
    ns_list.push_back(ns);
  }
}

void server_spdk_cmd_cb(void *arg, const struct spdk_nvme_cpl *cpl) {
  if (cpl == nullptr || !spdk_nvme_cpl_is_success(cpl)) {
    printf("error %s\n", spdk_nvme_cpl_get_status_type_string(&cpl->status));
  }
  printf("stage %s success\n", (char *)arg);
  finished = true;
}

int main() {
  // init default opts
  spdk_env_opts opts;
  spdk_env_opts_init(&opts);
  // init env
  int ret = spdk_env_init(&opts);
  if (ret < 0) {
    printf("Unable to initialize SPDK env\n");
    return 1;
  }

  ret = spdk_nvme_probe(nullptr, nullptr, probe_cb, attach_cb, nullptr);
  if (ret < 0) {
    printf("spdk nvme probe failed\n");
    return 1;
  }
  if (ctrlr_list.empty()) {
    printf("no spdk device\n");
    return 1;
  }

  for (int i = 0; i < ns_list.size(); i++) {
    char *write_buf =
        (char *)spdk_malloc(kDataLength, kPageSize, nullptr,
                            SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
    char *read_buf =
        (char *)spdk_malloc(kDataLength, kPageSize, nullptr,
                            SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
    for (int i = 0; i < 26; i++) {
      memset(write_buf + i * kPageSize, 'a' + i, kPageSize);
    }

    spdk_nvme_io_qpair_opts qp_opts;
    spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr_list[i], &qp_opts,
                                              sizeof(qp_opts));
    qp_opts.io_queue_requests = 256 * 256;
    qp_opts.io_queue_size = 256;
    spdk_nvme_qpair *ssd_qp = spdk_nvme_ctrlr_alloc_io_qpair(
        ctrlr_list[i], &qp_opts, sizeof(qp_opts));
    if (ssd_qp == nullptr) {
      printf("ssd_qp is nullptr\n");
      return 1;
    }

    spdk_nvme_ns_cmd_write(ns_list[i], ssd_qp, write_buf, 0,
                           kDataLength /
                               spdk_nvme_ns_get_sector_size(ns_list[i]),
                           server_spdk_cmd_cb, (void *)"write", 0);
    finished = false;
    while (!finished) {
      spdk_nvme_qpair_process_completions(ssd_qp, 0);
    }
    spdk_nvme_ns_cmd_read(ns_list[i], ssd_qp, read_buf, 0,
                          kDataLength /
                              spdk_nvme_ns_get_sector_size(ns_list[i]),
                          server_spdk_cmd_cb, (void *)"read", 0);
    finished = false;
    while (!finished) {
      spdk_nvme_qpair_process_completions(ssd_qp, 0);
    }

    printf("Namespace ID: %d, memcmp %d\n", spdk_nvme_ns_get_id(ns_list[i]),
           memcmp(write_buf, read_buf, kDataLength));
    spdk_free(write_buf);
    spdk_free(read_buf);
    spdk_nvme_ctrlr_free_io_qpair(ssd_qp);
  }

  for (auto &x : ctrlr_list) {
    spdk_nvme_detach(x);
  }
  spdk_env_fini();
  return 0;
}