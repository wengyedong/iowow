#include "iwkv.h"
#include "iwutils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

char *g_program;

typedef struct BMCTX BMCTX;

typedef bool (bench_method(BMCTX *bmctx));

#define RND_DATA_SZ 1048576
char RND_DATA[RND_DATA_SZ];

struct BM {
  bool initiated;
  int argc;
  char **argv;
  int param_num;
  int param_num_reads;
  int param_value_size;
  char *param_benchmarks;
  char *param_report;
  void (*env_setup)(void);
  void *(*db_open)(BMCTX *ctx);
  bool (*db_close)(BMCTX *ctx);
  bool (*db_put)(BMCTX *ctx, const IWKV_val *key, const IWKV_val *val, bool sync);
  bool (*db_get)(BMCTX *ctx, const IWKV_val *key, IWKV_val *val, bool *found);
  bool (*db_cursor_to_key)(BMCTX *ctx, const IWKV_val *key, IWKV_val *val, bool *found);
  bool (*db_del)(BMCTX *ctx, const IWKV_val *key, bool *found);
  bool (*db_read_seq)(BMCTX *ctx, bool reverse);
} bm;

struct BMCTX {
  bool success;
  bool freshdb;
  char *name;
  int num;
  int value_size;
  uint64_t start_ms;
  uint64_t end_ms;
  void *db;
  void *extra;
  bench_method *method;
  int rnd_data_pos;
};

static void _bmctx_dispose(BMCTX *ctx) {
  free(ctx->name);
  free(ctx);
}

static const char *_bmctx_rndbuf_nextptr(BMCTX *ctx, int len) {
  assert(len <= RND_DATA_SZ);
  if (ctx->rnd_data_pos + len > RND_DATA_SZ) {
    ctx->rnd_data_pos = 0;
  }
  const char *ret = RND_DATA + ctx->rnd_data_pos;
  ctx->rnd_data_pos += len;
  return ret;
}

static bool _bm_check() {
  if (!bm.env_setup) {
    fprintf(stderr, "print_env function is not set\n");
    return false;
  }
  if (!bm.db_open) {
    fprintf(stderr, "db_open function is not set\n");
    return false;
  }
  if (!bm.db_close) {
    fprintf(stderr, "db_close function is not set\n");
    return false;
  }
  if (!bm.db_put) {
    fprintf(stderr, "db_put function is not set\n");
    return false;
  }
  if (!bm.db_get) {
    fprintf(stderr, "db_get function is not set\n");
    return false;
  }
  if (!bm.db_del) {
    fprintf(stderr, "db_del function is not set\n");
    return false;
  }
  if (!bm.db_read_seq) {
    fprintf(stderr, "db_read_seq function is not set\n");
    return false;
  }
  if (!bm.db_cursor_to_key) {
    fprintf(stderr, "db_cursor_to_key function is not set\n");
    return false;
  }
  return true;
}

static void _bm_help() {
  fprintf(stderr, "\n");
  fprintf(stderr, "Usage %s [options]\n\n", g_program);
  fprintf(stderr, "Options:\n");
  fprintf(stderr, "  -h Show this help\n");
  fprintf(stderr, "  -n <num> Number of stored records\n");
  fprintf(stderr, "  -r <num> Number of records to read (equals to number of stored records if not specified )\n");
  fprintf(stderr, "  -vz <size> Size of a single record value in bytes\n");
  fprintf(stderr, "  -b <comma separated benchmarks to run>\n\n");
  fprintf(stderr, "  -r <CSV report file> Default: report.csv\n\n");
  fprintf(stderr, "Available benchmarks:\n");
  fprintf(stderr, "  fillseq        write N values in sequential key order in async mode\n");
  fprintf(stderr, "  fillrandom     write N values in random key order in async mode\n");
  fprintf(stderr, "  overwrite      overwrite N values in random key order in async mode\n");
  fprintf(stderr, "  fillsync       write N/10 values in random key order in sync mode\n");
  fprintf(stderr, "  fill100K       write N/100 100K values in random order in async mode\n");
  fprintf(stderr, "  deleteseq      delete N keys in sequential order\n");
  fprintf(stderr, "  deleterandom   delete N keys in random order\n");
  fprintf(stderr, "  readseq        read N times sequentially\n");
  fprintf(stderr, "  readreverse    read N times in reverse order\n");
  fprintf(stderr, "  readrandom     read N times in random order\n");
  fprintf(stderr, "  readmissing    read N missing keys in random order\n");
  fprintf(stderr, "  readhot        read N times in random order from 1%% section of DB\n");
  fprintf(stderr, "  seekrandom     N random seeks\n");
  fprintf(stderr, "\n");
}

static bool _bm_init(int argc, char *argv[]) {
  if (bm.initiated) {
    fprintf(stderr, "Benchmark already initialized\n");
    return false;
  }
  bm.argc = argc;
  bm.argv = argv;
  bm.param_num = 1000000; // 1M records
  bm.param_num_reads = -1; // Same as param_num
  bm.param_value_size = 100; // 100 byte per value
  bm.param_benchmarks =  "fillrandom,"
                         "fillsync,"
                         "fillseq,"
                         "overwrite,"
                         "readrandom,"
                         "readseq,"
                         "readreverse,"
                         "readhot,"
                         "readmissing,"
                         "deleteseq,"
                         "fillseq,"
                         "deleterandom,"
                         "fill100K";

  bm.param_report = "report.csv";
#ifndef NDEBUG
  fprintf(stdout, "WARNING: Assertions are enabled, benchmarks can be slow\n");
#endif
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "-h")) {
      _bm_help();
      return false;
    }
    if (!strcmp(argv[i], "-n")) {
      if (++i >= argc) {
        fprintf(stderr, "'-n <num records>' option has no value\n");
        return false;
      }
      bm.param_num = atoi(argv[i]);
      if (bm.param_num <= 1000) {
        fprintf(stderr, "'-n <num records>' invalid option value\n");
        return false;
      }
    } else if (!strcmp(argv[i], "-r")) {
      if (++i >= argc) {
        fprintf(stderr, "'-r <num read records>' option has no value\n");
        return false;
      }
      bm.param_num_reads = atoi(argv[i]);
    } else if (!strcmp(argv[i], "-vz")) {
      if (++i >= argc) {
        fprintf(stderr, "'-vz <value size>' option has not value\n");
        return false;
      }
      bm.param_value_size = atoi(argv[i]);
      if (bm.param_value_size <= 0) {
        fprintf(stderr, "'-vz <value size>' invalid option value\n");
        return false;
      }
    } else if (!strcmp(argv[i], "-b")) {
      if (++i >= argc) {
        fprintf(stderr, "'-b <benchmarks>' options has no value\n");
        return false;
      }
      bm.param_benchmarks = argv[i];
    } else if (!strcmp(argv[i], "-r")) {
      if (++i >= argc) {
        fprintf(stderr, "'-r <CSV report file>' options has no value\n");
        return false;
      }
      bm.param_report = argv[i];
    }
  }
  if (bm.param_num_reads < 0) {
    bm.param_num_reads = bm.param_num;
  }
  if (!_bm_check()) {
    fprintf(stderr, "Benchmark `bm` structure is not configured properly\n");
    return false;
  }

  fprintf(stderr,
          "\n num records: %d\n read num records: %d\n value size: %d\n benchmarks: %s\n report: %s\n\n",
          bm.param_num, bm.param_num_reads, bm.param_value_size, bm.param_benchmarks, bm.param_report);

  // Fill up random data array
  for (int i = 0; i < RND_DATA_SZ; ++i) {
    RND_DATA[i] = ' ' + iwu_rand_range(95); // ascii space ... ~
  }
  return true;
}

static void _bm_run(BMCTX *ctx) {
  assert(ctx->method);
  int64_t llv;
  ctx->db = bm.db_open(ctx);
  if (!ctx->db) {
    ctx->success = false;
    return;
  }
  iwp_current_time_ms(&llv);
  ctx->start_ms = llv;
  ctx->success = ctx->method(ctx);
  if (!bm.db_close(ctx)) {
    ctx->success = false;
  }
  iwp_current_time_ms(&llv);
  ctx->end_ms = llv;
}

static bool _do_write(BMCTX *ctx, bool seq, bool sync) {
  char kbuf[100];
  IWKV_val key, val;
  key.data = kbuf;
  for (int i = 0; i < ctx->num; ++i) {
    const int k = seq ? i : iwu_rand_range(bm.param_num);
    snprintf(key.data, sizeof(kbuf), "%016d", k);
    key.size = strlen(key.data);
    val.data = (void *) _bmctx_rndbuf_nextptr(ctx, ctx->value_size);
    val.size = ctx->value_size;
    if (!bm.db_put(ctx, &key, &val, sync)) {
      return false;
    }
  }
  return true;
}

static bool _do_delete(BMCTX *ctx, bool seq) {
  char kbuf[100];
  bool found;
  IWKV_val key;
  key.data = kbuf;
  for (int i = 0; i < ctx->num; ++i) {
    const int k = seq ? i : iwu_rand_range(bm.param_num);
    snprintf(key.data, sizeof(kbuf), "%016d", k);
    key.size = strlen(key.data);
    if (!bm.db_del(ctx, &key, &found)) {
      return false;
    }
  }
  return true;
}

static bool _do_read_seq(BMCTX *ctx) {
  return bm.db_read_seq(ctx, false);
}

static bool _do_read_reverse(BMCTX *ctx) {
  return bm.db_read_seq(ctx, true);
}

static bool _do_read_random(BMCTX *ctx) {
  char kbuf[100];
  IWKV_val key, val;
  bool found;
  key.data = kbuf;
  for (int i = 0; i < bm.param_num_reads; ++i) {
    const int k = iwu_rand_range(bm.param_num);
    snprintf(key.data, sizeof(kbuf), "%016d", k);
    key.size = strlen(key.data);
    val.data = 0;
    val.size = 0;
    if (!bm.db_get(ctx, &key, &val, &found)) {
      iwkv_val_dispose(&val);
      return false;
    }
    iwkv_val_dispose(&val);
  }
  return true;
}

static bool _do_read_missing(BMCTX *ctx) {
  char kbuf[100];
  IWKV_val key, val;
  bool found;
  key.data = kbuf;
  for (int i = 0; i < bm.param_num_reads; ++i) {
    const int k = iwu_rand_range(bm.param_num);
    snprintf(key.data, sizeof(kbuf), "%016d.", k);
    key.size = strlen(key.data);
    val.data = 0;
    val.size = 0;
    if (!bm.db_get(ctx, &key, &val, &found)) {
      iwkv_val_dispose(&val);
      return false;
    }
    iwkv_val_dispose(&val);
    if (found) {
      return false;
    }
  }
  return true;
}

static bool _do_read_hot(BMCTX *ctx) {
  char kbuf[100];
  IWKV_val key, val;
  bool found;
  key.data = kbuf;
  const int range = (bm.param_num + 99) / 100;
  for (int i = 0; i < bm.param_num_reads; ++i) {
    const int k = iwu_rand_range(range);
    snprintf(key.data, sizeof(kbuf), "%016d", k);
    key.size = strlen(key.data);
    val.data = 0;
    val.size = 0;
    if (!bm.db_get(ctx, &key, &val, &found)) {
      iwkv_val_dispose(&val);
      return false;
    }
    iwkv_val_dispose(&val);
  }
  return true;
}

static bool _do_seek_random(BMCTX *ctx) {
  char kbuf[100];
  IWKV_val key, val;
  bool found;
  key.data = kbuf;
  for (int i = 0; i < bm.param_num_reads; ++i) {
    const int k = iwu_rand_range(bm.param_num);
    snprintf(key.data, sizeof(kbuf), "%016d", k);
    val.data = 0;
    val.size = 0;
    if (!bm.db_cursor_to_key(ctx, &key, &val, &found)) {
      iwkv_val_dispose(&val);
      return false;
    }
    iwkv_val_dispose(&val);
  }
  return true;
}

static bool _bm_fillseq(BMCTX *ctx) {
  if (!ctx->freshdb) return false;
  return _do_write(ctx, true, false);
}

static bool _bm_fillrandom(BMCTX *ctx) {
  if (!ctx->freshdb) return false;
  return _do_write(ctx, false, false);
}

static bool _bm_overwrite(BMCTX *ctx) {
  if (ctx->freshdb) return false;
  return _do_write(ctx, false, false);
}

static bool _bm_fillsync(BMCTX *ctx) {
  if (!ctx->freshdb) return false;
  ctx->num /= 10;
  return _do_write(ctx, false, true);
}

static bool _bm_fill100K(BMCTX *ctx) {
  if (!ctx->freshdb) return false;
  ctx->num /= 100;
  ctx->value_size = 100 * 1000;
  return _do_write(ctx, false, false);
}

static bool _bm_deleteseq(BMCTX *ctx) {
  if (ctx->freshdb) return false;
  return _do_delete(ctx, true);
}

static bool _bm_deleterandom(BMCTX *ctx) {
  if (ctx->freshdb) return false;
  return _do_delete(ctx, false);
}

static bool _bm_readseq(BMCTX *ctx) {
  if (ctx->freshdb) return false;
  return _do_read_seq(ctx);
}

static bool _bm_readreverse(BMCTX *ctx) {
  if (ctx->freshdb) return false;
  return _do_read_reverse(ctx);
}

static bool _bm_readrandom(BMCTX *ctx) {
  if (ctx->freshdb) return false;
  return _do_read_random(ctx);
}

static bool _bm_readmissing(BMCTX *ctx) {
  if (ctx->freshdb) return false;
  return _do_read_missing(ctx);
}

static bool _bm_readhot(BMCTX *ctx) {
  if (ctx->freshdb) return false;
  return _do_read_hot(ctx);
}

static bool _bm_seekrandom(BMCTX *ctx) {
  if (ctx->freshdb) return false;
  return _do_seek_random(ctx);
}

static BMCTX *_bmctx_create(const char *name) {
  bench_method *method = 0;
  bool freshdb = false;
  if (!strcmp(name, "fillseq")) {
    freshdb = true;
    method = _bm_fillseq;
  } else if (!strcmp(name, "fillrandom")) {
    freshdb = true;
    method = _bm_fillrandom;
  } else if (!strcmp(name, "overwrite")) {
    method = _bm_overwrite;
  } else if (!strcmp(name, "fillsync")) {
    freshdb = true;
    method = _bm_fillsync;
  } else if (!strcmp(name, "fill100K")) {
    freshdb = true;
    method = _bm_fill100K;
  } else if (!strcmp(name, "deleteseq")) {
    method = _bm_deleteseq;
  } else if (!strcmp(name, "deleterandom")) {
    method = _bm_deleterandom;
  } else if (!strcmp(name, "readseq")) {
    method = _bm_readseq;
  } else if (!strcmp(name, "readreverse")) {
    method = _bm_readreverse;
  } else if (!strcmp(name, "readrandom")) {
    method = _bm_readrandom;
  } else if (!strcmp(name, "readmissing")) {
    method = _bm_readmissing;
  } else if (!strcmp(name, "readhot")) {
    method = _bm_readhot;
  } else if (!strcmp(name, "seekrandom")) {
    method = _bm_seekrandom;
  } else {
    fprintf(stderr, "Unknown benchmark: '%s'\n", name);
    return 0;
  }
  BMCTX *bmctx = calloc(1, sizeof(*bmctx));
  bmctx->name = strdup(name);
  bmctx->method = method;
  bmctx->num = bm.param_num;
  bmctx->value_size = bm.param_value_size;
  bmctx->freshdb = freshdb;
  return bmctx;
}

static bool bm_bench_run(int argc, char *argv[]) {
  if (!_bm_init(argc, argv)) {
    fprintf(stderr, "Benchmark cannot be initialized\n");
    exit(1);
  }
  bm.env_setup();
  int c = 0;
  const char *ptr = bm.param_benchmarks;
  char bname[100];
  bool bmres = true;
  while (bmres) {
    if (*ptr == ',' || *ptr == '\0' || c >= 99) {
      bname[c] = '\0';
      BMCTX *ctx = _bmctx_create(bname);
      c = 0;
      if (ctx) {
        printf("Starting benchmark: '%s'\n", bname);
        _bm_run(ctx);
        bmres = ctx->success;
        if (!bmres) {
          fprintf(stderr, "Failed to run benchmark: %s\n", bname);
        } else {
          printf("Done '%s' in %ld\n", bname, (ctx->end_ms - ctx->start_ms));
        }
        _bmctx_dispose(ctx);
      }
      if (*ptr == '\0' || !bmres) {
        break;
      }
    } else if (!isspace(*ptr)) {
      bname[c++] = *ptr;
    }
    ptr += 1;
  }
  return bmres;
}