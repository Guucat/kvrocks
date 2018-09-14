#include "t_list.h"
#include <vector>

rocksdb::Status RedisList::GetMetadata(Slice key, ListMetadata *metadata) {
  return RedisDB::GetMetadata(kRedisList, key, metadata);
}

rocksdb::Status RedisList::Size(Slice key, uint32_t *ret) {
  *ret = 0;
  ListMetadata metadata;
  rocksdb::Status s = GetMetadata(key, &metadata);
  if (!s.ok() && !s.IsNotFound()) return s;
  *ret = metadata.size;
  return rocksdb::Status::OK();
}

rocksdb::Status RedisList::Push(Slice key, std::vector<Slice> elems, bool left, int *ret) {
  return push(key, elems, true, left, ret);
}

rocksdb::Status RedisList::PushX(Slice key, std::vector<Slice> elems, bool left, int *ret) {
  return push(key, elems, false, left, ret);
}

rocksdb::Status RedisList::push(Slice key, std::vector<Slice> elems, bool create_if_missing, bool left, int *ret) {
  ListMetadata metadata;
  rocksdb::Status s = GetMetadata(key, &metadata);
  if (!s.ok()
      && !(create_if_missing && s.IsNotFound())) {
    return s;
  }

  char buf[8];
  uint64_t index = left ? metadata.head - 1 : metadata.tail;
  rocksdb::WriteBatch batch;
  for (auto elem : elems) {
    EncodeFixed64(buf, index);
    Slice sub_key = InternalKey(key, Slice(buf, 8), metadata.version).Encode();
    batch.Put(sub_key, elem);
    left ? --index : ++index;
  }
  if (left) {
    metadata.head -= elems.size();
  } else {
    metadata.tail += elems.size();
  }
  std::string bytes;
  metadata.size += elems.size();
  metadata.Encode(&bytes);
  batch.Put(metadata_cf_handle_, key, bytes);
  *ret = metadata.size;
  return db_->Write(rocksdb::WriteOptions(), &batch);
}

rocksdb::Status RedisList::Pop(Slice key, std::string *elem, bool left) {
  ListMetadata metadata;
  rocksdb::Status s = GetMetadata(key, &metadata);
  if (!s.ok()) return s;

  uint64_t index = left ? metadata.head : metadata.tail-1;
  std::string buf;
  PutFixed64(&buf, index);
  Slice sub_key = InternalKey(key, buf, metadata.version).Encode();
  s = db_->Get(rocksdb::ReadOptions(), sub_key, elem);
  if (!s.ok()) {
    // FIXME: should be always exists??
    return s;
  }
  rocksdb::WriteBatch batch;
  batch.Delete(sub_key);
  if (metadata.size == 1) {
    batch.Delete(metadata_cf_handle_, key);
  } else {
    std::string bytes;
    metadata.size -= 1;
    left ? ++metadata.head : --metadata.tail;
    metadata.Encode(&bytes);
    batch.Put(metadata_cf_handle_, key, bytes);
  }
  return db_->Write(rocksdb::WriteOptions(), &batch);
}

rocksdb::Status RedisList::Index(Slice key, int index, std::string *elem) {
  ListMetadata metadata;
  rocksdb::Status s = GetMetadata(key, &metadata);
  if (!s.ok()) return s;

  if (index < 0) index += metadata.size;
  if (index < 0 || index >= metadata.size) return rocksdb::Status::OK();

  std::string buf;
  PutFixed64(&buf, metadata.head + index);
  Slice sub_key = InternalKey(key, buf, metadata.version).Encode();
  return db_->Get(rocksdb::ReadOptions(), sub_key, elem);
}

// The offset can also be negative, -1 is the last element, -2 the penultimate
// Out of range indexes will not produce an error.
// If start is larger than the end of the list, an empty list is returned.
// If stop is larger than the actual end of the list,
// Redis will treat it like the last element of the list.
rocksdb::Status RedisList::Range(Slice key, int start, int stop, std::vector<std::string> *elems) {
  elems->clear();
  ListMetadata metadata;
  rocksdb::Status s = GetMetadata(key, &metadata);
  if (!s.ok()) return s.IsNotFound() ? rocksdb::Status::OK() : s;

  if (start < 0) start = metadata.size + start;
  if (stop < 0) stop = metadata.size + stop;
  if (start < 0 || stop < 0 || start >= stop) return rocksdb::Status::OK();

  std::string buf;
  PutFixed64(&buf, metadata.head + start);
  Slice start_key = InternalKey(key, buf, metadata.version).Encode();
  Slice prefix = InternalKey(key, "", metadata.version).Encode();

  rocksdb::ReadOptions opts;
  opts.fill_cache = false;
  auto iter = db_->NewIterator(opts);
  for (iter->Seek(start_key);
       iter->Valid() && iter->key().starts_with(prefix);
       iter->Next()) {
    InternalKey ikey(iter->key());
    Slice sub_key = ikey.GetSubKey();
    uint64_t index;
    GetFixed64(&sub_key, &index);
    // index should be always >= start
    if (index > metadata.head+stop) break;
    elems->push_back(iter->value().ToString());
  }
  return rocksdb::Status::OK();
}

rocksdb::Status RedisList::Set(Slice key, int index, Slice elem) {
  ListMetadata metadata;
  rocksdb::Status s = GetMetadata(key, &metadata);
  if (!s.ok()) return s;
  if (index < 0) index = metadata.size + index;
  if (index < 0 || index >= metadata.size) {
    return rocksdb::Status::InvalidArgument("index out of range");
  }

  std::string buf, value;
  PutFixed64(&buf, metadata.head+index);
  Slice sub_key = InternalKey(key, buf, metadata.version).Encode();
  s = db_->Get(rocksdb::ReadOptions(), sub_key, &value);
  if (!s.ok()) {
    return s;
  }
  if (value == elem) return rocksdb::Status::OK();
  return db_->Put(rocksdb::WriteOptions(), sub_key, elem);
}

rocksdb::Status RedisList::RPopLPush(Slice src, Slice dst, std::string *elem) {
  rocksdb::Status s = Pop(src, elem, false);
  if (!s.ok()) return s;

  int ret;
  std::vector<Slice> elems;
  elems.emplace_back(*elem);
  s = Push(dst, elems, true, &ret);
  return s;
}

// Caution: trim the big list may block the server
rocksdb::Status RedisList::Trim(Slice key, int start, int stop) {
  ListMetadata metadata;
  rocksdb::Status s = GetMetadata(key, &metadata);
  if (!s.ok()) return s.IsNotFound() ? rocksdb::Status::OK() : s;

  if (start < 0) start = metadata.size + start;
  if (stop < 0) stop = metadata.size > -1 * stop ? metadata.size+stop : metadata.size;
  // the result will be empty list when start > stop,
  // or start is larger than the end of list
  if (start < 0 || start > stop) {
    return db_->Delete(rocksdb::WriteOptions(), metadata_cf_handle_, key);
  }
  // TODO: copy the alive elems when it's much less then the list size
  std::string buf;
  rocksdb::WriteBatch batch;
  uint64_t left_index = metadata.head + start;
  for (uint64_t i = metadata.head; i < left_index; i++) {
    PutFixed64(&buf, i);
    Slice sub_key = InternalKey(key, buf, metadata.version).Encode();
    batch.Delete(sub_key);
    metadata.head++;
  }
  uint64_t right_index = metadata.head+stop+1;
  for (uint64_t i = right_index; i < metadata.tail; i++) {
    Slice sub_key = InternalKey(key, buf, metadata.version).Encode();
    batch.Delete(sub_key);
    metadata.tail--;
  }
  metadata.size = uint32_t(stop - start + 1);
  std::string bytes;
  metadata.Encode(&bytes);
  batch.Put(metadata_cf_handle_, key, bytes);
  return db_->Write(rocksdb::WriteOptions(), &batch);
}