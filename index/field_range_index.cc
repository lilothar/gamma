/**
 * Copyright 2019 The Gamma Authors.
 *
 * This source code is licensed under the Apache License, Version 2.0 license
 * found in the LICENSE file in the root directory of this source tree.
 */

#include "field_range_index.h"

#include <math.h>
#include <string.h>

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <sstream>
#include <typeinfo>

#include "bitmap.h"
#include "log.h"

#ifdef __APPLE__
#include "threadskv8.h"
#else
#include "threadskv10h.h"
#endif

#include "utils.h"

using std::string;
using std::vector;

#define BM_OPERATE_TYPE long

namespace tig_gamma {

class Node {
 public:
  Node() {
    size_ = 0;
    data_dense_ = nullptr;
    data_sparse_ = nullptr;
    min_ = std::numeric_limits<int>::max();
    min_aligned_ = std::numeric_limits<int>::max();
    max_ = -1;
    max_aligned_ = -1;
    capacity_ = 0;
    type_ = Sparse;
    n_extend_ = 0;
  }

  ~Node() {
    if (type_ == Dense) {
      if (data_dense_) {
        free(data_dense_);
        data_dense_ = nullptr;
      }
    } else {
      if (data_sparse_) {
        free(data_sparse_);
        data_sparse_ = nullptr;
      }
    }
  }

  typedef enum NodeType { Dense, Sparse } NodeType;

  int AddDense(int val, ResourceQueue *res_q) {
    int op_len = sizeof(BM_OPERATE_TYPE) * 8;

    if (size_ == 0) {
      min_ = val;
      max_ = val;
      min_aligned_ = (val / op_len) * op_len;
      max_aligned_ = (val / op_len + 1) * op_len - 1;
      int bytes_count = -1;

      if (bitmap::create(data_dense_, bytes_count,
                         max_aligned_ - min_aligned_ + 1) != 0) {
        LOG(ERROR) << "Cannot create bitmap!";
        return -1;
      }
      bitmap::set(data_dense_, val - min_aligned_);
      ++size_;
      return 0;
    }

    if (val < min_aligned_) {
      char *data = nullptr;
      int min_aligned = (val / op_len) * op_len;

      // LOG(INFO) << "Dense lower min_aligned_ [" << min_aligned_ << "],
      // min_aligned ["
      //           << min_aligned << "] max_aligned_ [" << max_aligned_ << "]";

      int bytes_count = -1;
      if (bitmap::create(data, bytes_count, max_aligned_ - min_aligned + 1) !=
          0) {
        LOG(ERROR) << "Cannot create bitmap!";
        return -1;
      }

      BM_OPERATE_TYPE *op_data_dst = (BM_OPERATE_TYPE *)data;
      BM_OPERATE_TYPE *op_data_ori = (BM_OPERATE_TYPE *)data_dense_;

      for (int i = 0; i < (max_aligned_ - min_aligned_ + 1) / op_len; ++i) {
        op_data_dst[i + (min_aligned_ - min_aligned) / op_len] = op_data_ori[i];
      }

      bitmap::set(data, val - min_aligned);
      auto old_data = data_dense_;
      data_dense_ = data;
      min_ = val;
      min_aligned_ = min_aligned;
      ResourceToRecovery *res = new ResourceToRecovery(old_data);
      bool ret_q = res_q->enqueue(res);
      if (not ret_q) {
        LOG(ERROR) << "Enqueue failed!";
      }
    } else if (val > max_aligned_) {
      // double k = 1 + exp(-1 * n_extend_ + 1);
      char *data = nullptr;
      // 2X spare space to speed up insert
      int max_aligned = (val / op_len + 1) * op_len * 2 - 1;

      // LOG(INFO) << "Dense upper min_aligned_ [" << min_aligned_ << "],
      // max_aligned ["
      //           << max_aligned << "] max_aligned_ [" << max_aligned_ << "]
      //           size ["
      //           << size_ << "] val [" << val << "] min [" << min_ << "] max
      //           [" << max_ << "]";

      int bytes_count = -1;
      if (bitmap::create(data, bytes_count, max_aligned - min_aligned_ + 1) !=
          0) {
        LOG(ERROR) << "Cannot create bitmap!";
        return -1;
      }

      BM_OPERATE_TYPE *op_data_dst = (BM_OPERATE_TYPE *)data;
      BM_OPERATE_TYPE *op_data_ori = (BM_OPERATE_TYPE *)data_dense_;

      for (int i = 0; i < (max_aligned_ - min_aligned_ + 1) / op_len; ++i) {
        op_data_dst[i] = op_data_ori[i];
      }

      bitmap::set(data, val - min_aligned_);
      auto old_data = data_dense_;
      data_dense_ = data;
      max_ = val;
      max_aligned_ = max_aligned;
      ResourceToRecovery *res = new ResourceToRecovery(old_data);
      bool ret_q = res_q->enqueue(res);
      if (not ret_q) {
        LOG(ERROR) << "Enqueue failed!";
      }
    } else {
      bitmap::set(data_dense_, val - min_aligned_);
      min_ = std::min(min_, val);
      max_ = std::max(max_, val);
    }

    ++size_;
    return 0;
  }

  int AddSparse(int val, ResourceQueue *res_q) {
    int op_len = sizeof(BM_OPERATE_TYPE) * 8;
    min_ = std::min(min_, val);
    max_ = std::max(max_, val);
    if (val < min_aligned_) {
      min_aligned_ = (val / op_len) * op_len;
    }
    if (val > max_aligned_) {
      max_aligned_ = (val / op_len + 1) * op_len - 1;
    }

    if (capacity_ == 0) {
      capacity_ = 1;
      data_sparse_ = (int *)malloc(capacity_ * sizeof(int));
    } else if (size_ >= capacity_) {
      capacity_ *= 2;
      // LOG(INFO) << "Sparse capacity [" << capacity_ << "]";
      int *data = (int *)malloc(capacity_ * sizeof(int));
      for (int i = 0; i < size_; ++i) {
        data[i] = data_sparse_[i];
      }

      int *old_data = data_sparse_;
      data_sparse_ = data;
      ResourceToRecovery *res = new ResourceToRecovery(old_data);
      bool ret_q = res_q->enqueue(res);
      if (not ret_q) {
        LOG(ERROR) << "Enqueue failed!";
      }
    }
    data_sparse_[size_] = val;

    ++size_;
    return 0;
  }

  int Add(int val, ResourceQueue *res_q) {
    int offset = max_ - min_;
    double density = (size_ * 1.) / offset;

    if (type_ == Dense) {
      if (offset > 100000) {
        if (density < 0.08) {
          ConvertToSparse(res_q);
          return AddSparse(val, res_q);
        }
      }
      return AddDense(val, res_q);
    } else {
      if (offset > 100000) {
        if (density > 0.1) {
          ConvertToDense(res_q);
          return AddDense(val, res_q);
        }
      }
      return AddSparse(val, res_q);
    }
  }

  int ConvertToSparse(ResourceQueue *res_q) {
    data_sparse_ = (int *)malloc(size_ * sizeof(int));
    int offset = max_aligned_ - min_aligned_ + 1;
    int idx = 0;
    for (int i = 0; i < offset; ++i) {
      if (bitmap::test(data_dense_, i)) {
        if (idx >= size_) {
          LOG(WARNING) << "idx [" << idx << "] size [" << size_ << "] i [" << i
                       << "] offset [" << offset << "]";
          break;
        }
        data_sparse_[idx] = i + min_aligned_;
        ++idx;
      }
    }

    if (size_ != idx) {
      LOG(ERROR) << "size [" << size_ << "] idx [" << idx << "] max_aligned_ ["
                 << max_aligned_ << "] min_aligned_ [" << min_aligned_
                 << "] max [" << max_ << "] min [" << min_ << "]";
    }
    ResourceToRecovery *res = new ResourceToRecovery(data_dense_);
    bool ret_q = res_q->enqueue(res);
    if (not ret_q) {
      LOG(ERROR) << "Enqueue failed!";
    }
    capacity_ = size_;
    type_ = Sparse;
    data_dense_ = nullptr;
    return 0;
  }

  int ConvertToDense(ResourceQueue *res_q) {
    int bytes_count = -1;
    if (bitmap::create(data_dense_, bytes_count,
                       max_aligned_ - min_aligned_ + 1) != 0) {
      LOG(ERROR) << "Cannot create bitmap!";
      return -1;
    }

    for (int i = 0; i < size_; ++i) {
      int val = data_sparse_[i];
      if (val < min_aligned_ || val > max_aligned_) {
        LOG(WARNING) << "val [" << val << "] size [" << size_ << "] i [" << i
                     << "]";
        continue;
      }
      bitmap::set(data_dense_, val - min_aligned_);
    }

    ResourceToRecovery *res = new ResourceToRecovery(data_sparse_);
    bool ret_q = res_q->enqueue(res);
    if (not ret_q) {
      LOG(ERROR) << "Enqueue failed!";
    }
    type_ = Dense;
    data_sparse_ = nullptr;
    return 0;
  }

  int DeleteDense(int val, ResourceQueue *res_q) {
    int pos = val - min_aligned_;
    if (pos < 0 || val > max_aligned_) {
      LOG(ERROR) << "Cannot delete [" << val << "]";
      return -1;
    }
    --size_;
    bitmap::unset(data_dense_, pos);
    return 0;
  }

  int DeleteSparse(int val, ResourceQueue *res_q) {
    int i = 0;
    for (; i < size_; ++i) {
      if (data_sparse_[i] == val) {
        break;
      }
    }

    if (i == size_) {
      LOG(ERROR) << "Cannot delete [" << val << "]";
      return -1;
    }
    for (int j = i; j < size_ - 1; ++j) {
      data_sparse_[j] = data_sparse_[j + 1];
    }

    --size_;
    return 0;
  }

  int Delete(int val, ResourceQueue *res_q) {
    if (type_ == Dense) {
      return DeleteDense(val, res_q);
    } else {
      return DeleteSparse(val, res_q);
    }
  }

  int Min() { return min_; }
  int Max() { return max_; }

  int MinAligned() { return min_aligned_; }
  int MaxAligned() { return max_aligned_; }

  int Size() { return size_; }
  NodeType Type() { return type_; }

  char *DataDense() { return data_dense_; }
  int *DataSparse() { return data_sparse_; }

  // for debug
  void MemorySize(long &dense, long &sparse) {
    if (type_ == Dense) {
      dense += (max_aligned_ - min_aligned_) / 8;
    } else {
      sparse += capacity_ * sizeof(int);
    }
  }

 private:
  int min_;
  int max_;
  int min_aligned_;
  int max_aligned_;

  NodeType type_;
  int capacity_;  // for sparse node
  int size_;
  char *data_dense_;
  int *data_sparse_;

  int n_extend_;
};

typedef struct BTreeParameters {
  uint mainleafxtra;
  uint maxleaves;
  uint poolsize;
  uint leafxtra;
  uint mainpool;
  uint mainbits;
  uint bits;
  const char *kDelim;
} BTreeParameters;

class FieldRangeIndex {
 public:
  FieldRangeIndex(std::string &path, int field_idx, enum DataType field_type,
                  BTreeParameters &bt_param);
  ~FieldRangeIndex();

  int Add(unsigned char *key, uint key_len, int value, ResourceQueue *res_q);

  int Delete(unsigned char *key, uint key_len, int value, ResourceQueue *res_q);

  int Search(const string &low, const string &high, RangeQueryResult *result);

  int Search(const string &tags, RangeQueryResult *result);

  bool IsNumeric() { return is_numeric_; }

  char *Delim() { return kDelim_; }

  // for debug
  long ScanMemory(long &dense, long &sparse);

 private:
  BtMgr *main_mgr_;
#ifndef __APPLE__
  BtMgr *cache_mgr_;
#endif
  bool is_numeric_;
  char *kDelim_;
  std::string path_;
};

FieldRangeIndex::FieldRangeIndex(std::string &path, int field_idx,
                                 enum DataType field_type,
                                 BTreeParameters &bt_param)
    : path_(path) {
  string cache_file =
      path + string("/cache_") + std::to_string(field_idx) + ".dis";
  string main_file =
      path + string("/main_") + std::to_string(field_idx) + ".dis";

  remove(cache_file.c_str());
  remove(main_file.c_str());

#ifdef __APPLE__
  main_mgr_ = bt_mgr(const_cast<char *>(main_file.c_str()), bt_param.mainbits,
                     bt_param.poolsize);
#else
  cache_mgr_ = bt_mgr(const_cast<char *>(cache_file.c_str()), bt_param.bits,
                      bt_param.leafxtra, bt_param.poolsize);
  cache_mgr_->maxleaves = bt_param.maxleaves;
  main_mgr_ = bt_mgr(const_cast<char *>(main_file.c_str()), bt_param.mainbits,
                     bt_param.mainleafxtra, bt_param.mainpool);
  main_mgr_->maxleaves = bt_param.maxleaves;
#endif

  if (field_type == DataType::STRING) {
    is_numeric_ = false;
  } else {
    is_numeric_ = true;
  }
  kDelim_ = const_cast<char *>(bt_param.kDelim);
}

FieldRangeIndex::~FieldRangeIndex() {
#ifdef __APPLE__
  BtDb *bt = bt_open(main_mgr_);
  BtPageSet set[1];
  uid next, page_no = LEAF_page;  // start on first page of leaves
  int cnt = 0;

  do {
    if (set->latch = bt_pinlatch(bt, page_no, 1)) {
      set->page = bt_mappage(bt, set->latch);
    } else {
      LOG(ERROR) << "unable to obtain latch";
      return;
    }
    bt_lockpage(bt, BtLockRead, set->latch);
    next = bt_getid(set->page->right);

    for (uint slot = 0; slot++ < set->page->cnt;)
      if (next || slot < set->page->cnt)
        if (!slotptr(set->page, slot)->dead) {
          BtKey *ptr = keyptr(set->page, slot);
          unsigned char len = ptr->len;

          if (slotptr(set->page, slot)->type == Duplicate) len -= BtId;

          // fwrite(ptr->key, len, 1, stdout);
          BtVal *val = valptr(set->page, slot);
          Node *p_node = nullptr;
          memcpy(&p_node, val->value, sizeof(Node *));
          delete p_node;
          // fwrite(val->value, val->len, 1, stdout);
          cnt++;
        }

    bt_unlockpage(bt, BtLockRead, set->latch);
    bt_unpinlatch(set->latch);
  } while (page_no = next);
#else
  BtDb *bt = bt_open(cache_mgr_, main_mgr_);

  if (bt_startkey(bt, nullptr, 0) == 0) {
    while (bt_nextkey(bt)) {
      if (bt->phase == 1) {
        Node *p_node = nullptr;
        memcpy(&p_node, bt->mainval->value, sizeof(Node *));
        delete p_node;
      }
    }
  }

  bt_unlockpage(BtLockRead, bt->cacheset->latch, __LINE__);
  bt_unpinlatch(bt->cacheset->latch);

  bt_unlockpage(BtLockRead, bt->mainset->latch, __LINE__);
  bt_unpinlatch(bt->mainset->latch);
#endif

  bt_close(bt);

#ifndef __APPLE__
  if (cache_mgr_) {
    bt_mgrclose(cache_mgr_);
    cache_mgr_ = nullptr;
  }
#endif
  if (main_mgr_) {
    bt_mgrclose(main_mgr_);
    main_mgr_ = nullptr;
  }
}

static int ReverseEndian(const unsigned char *in, unsigned char *out,
                         uint len) {
  for (uint i = 0; i < len; ++i) {
    out[i] = in[len - i - 1];
  }

  unsigned char N = 0x80;
  out[0] += N;
  return 0;
}

int FieldRangeIndex::Add(unsigned char *key, uint key_len, int value,
                         ResourceQueue *res_q) {
#ifdef __APPLE__
  BtDb *bt = bt_open(main_mgr_);
#else
  BtDb *bt = bt_open(cache_mgr_, main_mgr_);
#endif
  unsigned char key2[key_len];

  std::function<void(unsigned char *, uint)> InsertToBt =
      [&](unsigned char *key_to_add, uint key_len) {
        Node *p_node = nullptr;
        int ret = bt_findkey(bt, key_to_add, key_len, (unsigned char *)&p_node,
                             sizeof(Node *));

        if (ret < 0) {
          p_node = new Node;
#ifdef __APPLE__
          BTERR bterr = bt_insertkey(bt, key_to_add, key_len, 0,
                                     static_cast<void *>(&p_node),
                                     sizeof(Node *), Unique);
          if (bterr) {
            LOG(ERROR) << "Error " << bt->err;
          }
#else
          BTERR bterr = bt_insertkey(bt->main, key_to_add, key_len, 0,
                                     static_cast<void *>(&p_node),
                                     sizeof(Node *), Unique);
          if (bterr) {
            LOG(ERROR) << "Error " << bt->mgr->err;
          }
#endif
        }
        p_node->Add(value, res_q);
      };

  if (is_numeric_) {
    ReverseEndian(key, key2, key_len);
    InsertToBt(key2, key_len);
  } else {
    char key_s[key_len + 1];
    memcpy(key_s, key, key_len);
    key_s[key_len] = 0;

    char *p, *k;
    k = strtok_r(key_s, kDelim_, &p);
    while (k != nullptr) {
      InsertToBt(reinterpret_cast<unsigned char *>(k), strlen(k));
      k = strtok_r(NULL, kDelim_, &p);
    }
  }

  bt_close(bt);

  return 0;
}

int FieldRangeIndex::Delete(unsigned char *key, uint key_len, int value,
                            ResourceQueue *res_q) {
#ifdef __APPLE__
  BtDb *bt = bt_open(main_mgr_);
#else
  BtDb *bt = bt_open(cache_mgr_, main_mgr_);
#endif
  unsigned char key2[key_len];

  std::function<void(unsigned char *, uint)> DeleteFromBt =
      [&](unsigned char *key_to_add, uint key_len) {
        Node *p_node = nullptr;
        int ret = bt_findkey(bt, key_to_add, key_len, (unsigned char *)&p_node,
                             sizeof(Node *));

        if (ret < 0) {
          LOG(ERROR) << "Cannot find field [" << key << "]";
          return;
        }
        p_node->Delete(value, res_q);
      };

  if (is_numeric_) {
    ReverseEndian(key, key2, key_len);
    DeleteFromBt(key2, key_len);
  } else {
    char key_s[key_len + 1];
    memcpy(key_s, key, key_len);
    key_s[key_len] = 0;

    char *p, *k;
    k = strtok_r(key_s, kDelim_, &p);
    while (k != nullptr) {
      DeleteFromBt(reinterpret_cast<unsigned char *>(k), strlen(k));
      k = strtok_r(NULL, kDelim_, &p);
    }
  }

  bt_close(bt);

  return 0;
}

int FieldRangeIndex::Search(const string &lower, const string &upper,
                            RangeQueryResult *result) {
  if (!is_numeric_) {
    return Search(lower, result);
  }

#ifdef DEBUG
  double start = utils::getmillisecs();
#endif
#ifdef __APPLE__
  BtDb *bt = bt_open(main_mgr_);
#else
  BtDb *bt = bt_open(cache_mgr_, main_mgr_);
#endif
  unsigned char key_l[lower.length()];
  unsigned char key_u[upper.length()];
  ReverseEndian(reinterpret_cast<const unsigned char *>(lower.data()), key_l,
                lower.length());
  ReverseEndian(reinterpret_cast<const unsigned char *>(upper.data()), key_u,
                upper.length());

  std::vector<Node *> lists;

  int min_doc = std::numeric_limits<int>::max();
  int min_aligned = std::numeric_limits<int>::max();
  int max_doc = 0;
  int max_aligned = 0;
#ifdef __APPLE__
  uint slot = bt_startkey(bt, key_l, lower.length());
  while (slot) {
    BtKey *key = bt_key(bt, slot);
    BtVal *val = bt_val(bt, slot);

    if (keycmp(key, key_u, upper.length()) > 0) {
      break;
    }
    Node *p_node = nullptr;
    memcpy(&p_node, val->value, sizeof(Node *));
    lists.push_back(p_node);

    min_doc = std::min(min_doc, p_node->Min());
    min_aligned = std::min(min_aligned, p_node->MinAligned());
    max_doc = std::max(max_doc, p_node->Max());
    max_aligned = std::max(max_aligned, p_node->MaxAligned());

    slot = bt_nextkey(bt, slot);
  }
#else
  if (bt_startkey(bt, key_l, lower.length()) == 0) {
    while (bt_nextkey(bt)) {
      if (bt->phase == 1) {
        if (keycmp(bt->mainkey, key_u, upper.length()) > 0) {
          break;
        }
        Node *p_node = nullptr;
        memcpy(&p_node, bt->mainval->value, sizeof(Node *));
        lists.push_back(p_node);

        min_doc = std::min(min_doc, p_node->Min());
        min_aligned = std::min(min_aligned, p_node->MinAligned());
        max_doc = std::max(max_doc, p_node->Max());
        max_aligned = std::max(max_aligned, p_node->MaxAligned());
      }
    }
  }

  bt_unlockpage(BtLockRead, bt->cacheset->latch, __LINE__);
  bt_unpinlatch(bt->cacheset->latch);

  bt_unlockpage(BtLockRead, bt->mainset->latch, __LINE__);
  bt_unpinlatch(bt->mainset->latch);
#endif
  bt_close(bt);

#ifdef DEBUG
  double search_bt = utils::getmillisecs();
#endif
  if (max_doc - min_doc + 1 <= 0) {
    return 0;
  }

  result->SetRange(min_aligned, max_aligned);
  result->Resize();
#ifdef DEBUG
  double end_resize = utils::getmillisecs();
#endif

  auto &bitmap = result->Ref();
  int list_size = lists.size();

  int total = 0;

  int op_len = sizeof(BM_OPERATE_TYPE) * 8;
  for (int i = 0; i < list_size; ++i) {
    Node *list = lists[i];
    Node::NodeType node_type = list->Type();
    if (node_type == Node::NodeType::Dense) {
      char *data = list->DataDense();
      int min = list->MinAligned();
      int max = list->MaxAligned();

      if (min < min_aligned || max > max_aligned) {
        continue;
      }

      total += list->Size();

      BM_OPERATE_TYPE *op_data_dst = (BM_OPERATE_TYPE *)bitmap;
      BM_OPERATE_TYPE *op_data_ori = (BM_OPERATE_TYPE *)data;
      int offset = (min - min_aligned) / op_len;
      for (int j = 0; j < (max - min + 1) / op_len; ++j) {
        op_data_dst[j + offset] |= op_data_ori[j];
      }
    } else {
      int *data = list->DataSparse();
      int min = list->Min();
      int max = list->Max();
      int size = list->Size();

      if (min < min_doc || max > max_doc) {
        continue;
      }

      total += list->Size();

      for (int j = 0; j < size; ++j) {
        bitmap::set(bitmap, data[j] - min_aligned);
      }
    }
  }

  result->SetDocNum(total);

#ifdef DEBUG
  double end = utils::getmillisecs();
  LOG(INFO) << "bt cost [" << search_bt - start << "], resize cost ["
            << end_resize - search_bt << "], assemble result ["
            << end - end_resize << "], total [" << end - start << "]";
#endif
  return max_doc - min_doc + 1;
}

int FieldRangeIndex::Search(const string &tags, RangeQueryResult *result) {
  std::vector<string> items = utils::split(tags, kDelim_);
  Node *nodes[items.size()];
  int op_len = sizeof(BM_OPERATE_TYPE) * 8;
#ifdef DEBUG
  double begin = utils::getmillisecs();
#endif

  for (size_t i = 0; i < items.size(); ++i) {
    nodes[i] = nullptr;
    string item = items[i];
    const unsigned char *key_tag =
        reinterpret_cast<const unsigned char *>(item.data());

    Node *p_node = nullptr;
#ifdef __APPLE__
    BtDb *bt = bt_open(main_mgr_);
#else
    BtDb *bt = bt_open(cache_mgr_, main_mgr_);
#endif
    int ret =
        bt_findkey(bt, const_cast<unsigned char *>(key_tag), item.length(),
                   (unsigned char *)&p_node, sizeof(Node *));
    bt_close(bt);

    if (ret < 0) {
      LOG(ERROR) << "find node failed, key=" << item;
      continue;
    }
    if (p_node == nullptr) {
      LOG(ERROR) << "node is nullptr, key=" << item;
      continue;
    }
    nodes[i] = p_node;
  }
#ifdef DEBUG
  double fend = utils::getmillisecs();
#endif

  int min_doc = std::numeric_limits<int>::max();
  int max_doc = 0;
  for (size_t i = 0; i < items.size(); ++i) {
    if (nodes[i] == nullptr || nodes[i]->Size() <= 0) continue;
    min_doc = std::min(min_doc, nodes[i]->MinAligned());
    max_doc = std::max(max_doc, nodes[i]->MaxAligned());
  }

  if (max_doc - min_doc + 1 <= 0) {
    return 0;
  }

  int total = 0;
  result->SetRange(min_doc, max_doc);
  result->Resize();
  char *&bitmap = result->Ref();
#ifdef DEBUG
  double mbegin = utils::getmillisecs();
#endif
  for (size_t i = 0; i < items.size(); i++) {
    Node *p_node = nodes[i];
    if (p_node == nullptr) continue;
    int min_aligned = p_node->MinAligned();
    int max_aligned = p_node->MaxAligned();
    if (p_node->Type() == Node::NodeType::Dense) {
      char *data = p_node->DataDense();
      BM_OPERATE_TYPE *op_data_dst = (BM_OPERATE_TYPE *)bitmap;
      BM_OPERATE_TYPE *op_data_ori = (BM_OPERATE_TYPE *)data;

      int offset = (min_aligned - min_doc) / op_len;
      for (int j = 0; j < (max_aligned - min_aligned + 1) / op_len; ++j) {
        op_data_dst[j + offset] |= op_data_ori[j];
      }
    } else {
      int *data = p_node->DataSparse();
      int size = p_node->Size();
      for (int j = 0; j < size; ++j) {
        bitmap::set(bitmap, data[j] - min_aligned);
      }
    }
    total += p_node->Size();
  }
  result->SetDocNum(total);

#ifdef DEBUG
  double mend = utils::getmillisecs();
  LOG(INFO) << "total cost=" << mend - begin << ", find cost=" << fend - begin
            << ", merge cost=" << mend - mbegin << ", total num=" << total;
#endif
  return total;
}

long FieldRangeIndex::ScanMemory(long &dense, long &sparse) {
  long total = 0;
#ifdef __APPLE__
  BtDb *bt = bt_open(main_mgr_);

  uint slot = bt_startkey(bt, nullptr, 0);
  while (slot) {
    BtVal *val = bt_val(bt, slot);
    Node *p_node = nullptr;
    memcpy(&p_node, val->value, sizeof(Node *));
    p_node->MemorySize(dense, sparse);

    total += sizeof(Node);
    slot = bt_nextkey(bt, slot);
  }
#else
  BtDb *bt = bt_open(cache_mgr_, main_mgr_);

  if (bt_startkey(bt, nullptr, 0) == 0) {
    while (bt_nextkey(bt)) {
      if (bt->phase == 1) {
        Node *p_node = nullptr;
        memcpy(&p_node, bt->mainval->value, sizeof(Node *));
        p_node->MemorySize(dense, sparse);

        total += sizeof(Node);
      }
    }
  }

  bt_unlockpage(BtLockRead, bt->cacheset->latch, __LINE__);
  bt_unpinlatch(bt->cacheset->latch);

  bt_unlockpage(BtLockRead, bt->mainset->latch, __LINE__);
  bt_unpinlatch(bt->mainset->latch);
#endif

  bt_close(bt);

  return total;
}

MultiFieldsRangeIndex::MultiFieldsRangeIndex(std::string &path,
                                             Profile *profile)
    : path_(path) {
  profile_ = profile;
  fields_.resize(profile->FieldsNum());
  std::fill(fields_.begin(), fields_.end(), nullptr);

  b_recovery_running_ = true;
  b_operate_running_ = true;
  b_running_ = true;
  resource_recovery_q_ = new ResourceQueue;
  {
    auto func_recovery =
        std::bind(&MultiFieldsRangeIndex::ResourceRecoveryWorker, this);
    std::thread t(func_recovery);
    t.detach();
  }
  field_operate_q_ = new FieldOperateQueue;
  {
    auto func_operate =
        std::bind(&MultiFieldsRangeIndex::FieldOperateWorker, this);
    std::thread t(func_operate);
    t.detach();
  }
}

MultiFieldsRangeIndex::~MultiFieldsRangeIndex() {
  b_running_ = false;
  for (size_t i = 0; i < fields_.size(); i++) {
    if (fields_[i]) {
      delete fields_[i];
      fields_[i] = nullptr;
    }
  }

  while (b_recovery_running_) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  delete resource_recovery_q_;
  resource_recovery_q_ = nullptr;

  delete field_operate_q_;
  field_operate_q_ = nullptr;
}

void MultiFieldsRangeIndex::ResourceRecoveryWorker() {
  bool ret = false;
  while (b_running_ || b_operate_running_ || ret) {
    ResourceToRecovery *res = nullptr;
    ret = resource_recovery_q_->wait_dequeue_timed(res, 1000);
    if (not ret) {
      continue;
    }
    auto deadline = res->Deadline();
    auto now = std::chrono::system_clock::now();
    if (now < deadline) {
      std::this_thread::sleep_for(deadline - now);
    }
    delete res;
  }
  LOG(INFO) << "ResourceRecoveryWorker exited!";
  b_recovery_running_ = false;
}

void MultiFieldsRangeIndex::FieldOperateWorker() {
  bool ret = false;
  while (b_running_ || ret) {
    FieldOperate *field_op = nullptr;
    ret = field_operate_q_->wait_dequeue_timed(field_op, 1000);
    if (not ret) {
      continue;
    }

    int doc_id = field_op->doc_id;
    int field_id = field_op->field_id;

    auto op = field_op->type;
    if (op == FieldOperate::ADD) {
      AddDoc(doc_id, field_id);
    } else {
      DeleteDoc(doc_id, field_id);
    }

    delete field_op;
  }
  LOG(INFO) << "FieldOperateWorker exited!";
  b_operate_running_ = false;
}

int MultiFieldsRangeIndex::Add(int docid, int field) {
  FieldRangeIndex *index = fields_[field];
  if (index == nullptr) {
    return 0;
  }
  FieldOperate *field_op = new FieldOperate(FieldOperate::ADD, docid, field);

  bool ret = field_operate_q_->enqueue(field_op);

  if (not ret) {
    LOG(ERROR) << "Add failed!";
    return -1;
  }

  return 0;
}

int MultiFieldsRangeIndex::Delete(int docid, int field) {
  FieldRangeIndex *index = fields_[field];
  if (index == nullptr) {
    return 0;
  }
  FieldOperate *field_op = new FieldOperate(FieldOperate::DELETE, docid, field);

  bool ret = field_operate_q_->enqueue(field_op);

  if (not ret) {
    LOG(ERROR) << "Delete failed!";
    return -1;
  }

  return 0;
}

int MultiFieldsRangeIndex::AddDoc(int docid, int field) {
  FieldRangeIndex *index = fields_[field];
  if (index == nullptr) {
    return 0;
  }

  unsigned char *key;
  int key_len = 0;
  profile_->GetFieldRawValue(docid, field, &key, key_len);
  index->Add(key, key_len, docid, resource_recovery_q_);

  return 0;
}

int MultiFieldsRangeIndex::DeleteDoc(int docid, int field) {
  FieldRangeIndex *index = fields_[field];
  if (index == nullptr) {
    return 0;
  }

  unsigned char *key;
  int key_len = 0;
  profile_->GetFieldRawValue(docid, field, &key, key_len);
  index->Delete(key, key_len, docid, resource_recovery_q_);

  return 0;
}

int MultiFieldsRangeIndex::Search(const std::vector<FilterInfo> &origin_filters,
                                  MultiRangeQueryResults *out) {
  out->Clear();

  std::vector<FilterInfo> filters;

  for (const auto &filter : origin_filters) {
    FieldRangeIndex *index = fields_[filter.field];
    if (index == nullptr || filter.field < 0) {
      return -1;
    }
    if (not index->IsNumeric() && (filter.is_union == 0)) {
      // type is string and operator is "and", split this filter
      std::vector<string> items =
          utils::split(filter.lower_value, index->Delim());
      for (string &item : items) {
        FilterInfo f = filter;
        f.lower_value = item;
        filters.push_back(f);
      }
      continue;
    }
    filters.push_back(filter);
  }

  int fsize = filters.size();

  if (1 == fsize) {
    auto &filter = filters[0];
    RangeQueryResult *result = new RangeQueryResult;
    FieldRangeIndex *index = fields_[filter.field];

    int retval = index->Search(filter.lower_value, filter.upper_value, result);
    if (retval > 0) {
      out->Add(result);
    }
    // result->Output();
    return retval;
  }

  RangeQueryResult *results = new RangeQueryResult[fsize];
  utils::ScopeDeleter<RangeQueryResult> del(results);

  int valuable_result = -1;
  // record the shortest docid list
  int shortest_idx = -1, shortest = std::numeric_limits<int>::max();

  for (int i = 0; i < fsize; ++i) {
    auto &filter = filters[i];

    FieldRangeIndex *index = fields_[filter.field];
    if (index == nullptr || filter.field < 0) {
      continue;
    }

    int retval = index->Search(filter.lower_value, filter.upper_value,
                               &(results[valuable_result + 1]));
    if (retval < 0) {
      ;
    } else if (retval == 0) {
      return 0;  // no intersection
    } else {
      valuable_result += 1;

      if (shortest > retval) {
        shortest = retval;
        shortest_idx = valuable_result;
      }
    }
  }

  if (valuable_result < 0) {
    return -1;  // universal set
  }

  RangeQueryResult *tmp = new RangeQueryResult;
  int count = Intersect(results, valuable_result, shortest_idx, tmp);
  if (count > 0) {
    out->Add(tmp);
  } else {
    delete tmp;
  }

  return count;
}

int MultiFieldsRangeIndex::Intersect(RangeQueryResult *results, int j,
                                     int shortest_idx, RangeQueryResult *out) {
  assert(results != nullptr && j >= 0);

  // I want to build a smaller bitmap ...
  int min_doc = results[0].MinAligned();
  int max_doc = results[0].MaxAligned();

  int total = results[0].Size();

  // results[0].Output();

  for (int i = 1; i <= j; i++) {
    RangeQueryResult &r = results[i];

    // the maximum of the minimum(s)
    if (r.MinAligned() > min_doc) {
      min_doc = r.MinAligned();
    }
    // the minimum of the maximum(s)
    if (r.MaxAligned() < max_doc) {
      max_doc = r.MaxAligned();
    }
  }

  if (max_doc - min_doc + 1 <= 0) {
    return 0;
  }
  out->SetRange(min_doc, max_doc);
  out->Resize();

  out->SetDocNum(total);

  char *&bitmap = out->Ref();

  int op_len = sizeof(BM_OPERATE_TYPE) * 8;

  BM_OPERATE_TYPE *op_data_dst = (BM_OPERATE_TYPE *)bitmap;

  // calculate the intersection with the shortest doc chain.
  {
    char *data = results[shortest_idx].Ref();

    BM_OPERATE_TYPE *op_data_ori = (BM_OPERATE_TYPE *)data;
    for (int j = 0; j < (max_doc - min_doc + 1) / op_len; ++j) {
      op_data_dst[j] = op_data_ori[j];
    }
  }

  for (int i = 0; i < j; ++i) {
    if (i == shortest_idx) {
      continue;
    }
    char *data = results[i].Ref();
    BM_OPERATE_TYPE *op_data_ori = (BM_OPERATE_TYPE *)data;
    int min = results[i].MinAligned();
    int max = results[i].MaxAligned();

    if (min < min_doc) {
      int offset = (min_doc - min) / op_len;
      if (max > max_doc) {
        for (int k = 0; k < (max_doc - min_doc + 1) / op_len; ++k) {
          op_data_dst[k] &= op_data_ori[k + offset];
        }
      } else {
        for (int k = 0; k < (max - min_doc + 1) / op_len; ++k) {
          op_data_dst[k] &= op_data_ori[k + offset];
        }
      }
    } else {
      int offset = (min - min_doc) / op_len;
      if (max > max_doc) {
        for (int k = 0; k < (max_doc - min_doc + 1) / op_len; ++k) {
          op_data_dst[k + offset] &= op_data_ori[k];
        }
      } else {
        for (int k = 0; k < (max - min_doc + 1) / op_len; ++k) {
          op_data_dst[k + offset] &= op_data_ori[k];
        }
      }
    }
  }

  return total;
}

int MultiFieldsRangeIndex::AddField(int field, enum DataType field_type) {
  BTreeParameters bt_param;
  bt_param.mainleafxtra = 0;
  bt_param.maxleaves = 1000000;
  bt_param.poolsize = 500;
  bt_param.leafxtra = 0;
  bt_param.mainpool = 500;
  bt_param.mainbits = 16;
  bt_param.bits = 16;
  bt_param.kDelim = "\001";

  FieldRangeIndex *index =
      new FieldRangeIndex(path_, field, field_type, bt_param);
  fields_[field] = index;
  return 0;
}

long MultiFieldsRangeIndex::MemorySize(long &dense, long &sparse) {
  long total = 0;
  for (const auto &field : fields_) {
    if (field == nullptr) continue;

    total += field->ScanMemory(dense, sparse);
    total += sizeof(FieldRangeIndex);
  }
  return total;
}

}  // namespace tig_gamma
