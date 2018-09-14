#include <gtest/gtest.h>

#include "test_base.h"
#include "t_hash.h"
class RedisHashTest : public TestBase {
protected:
  explicit RedisHashTest() : TestBase() {
    hash = new RedisHash(storage_);
  }
  ~RedisHashTest() {
    delete hash;
  }
  void SetUp() override {
    key_ = "test_hash->key";
    fields_ = {"test-hash-key-1", "test-hash-key-2", "test-hash-key-3"};
    values_  = {"hash-test-value-1", "hash-test-value-2", "hash-test-value-3"};
  }
  void TearDown() override {
  }

protected:
  RedisHash *hash;
};

TEST_F(RedisHashTest, GetAndSet) {
  int ret;
  for (int i = 0; i < fields_.size(); i++) {
    rocksdb::Status s = hash->Set(key_, fields_[i], values_[i], &ret);
    EXPECT_TRUE(s.ok() && ret == 1);
  }
  for (int i = 0; i < fields_.size(); i++) {
    std::string got;
    rocksdb::Status s = hash->Get(key_, fields_[i], &got);
    EXPECT_EQ(values_[i], got);
  }
  rocksdb::Status s = hash->Delete(key_, fields_, &ret);
  EXPECT_TRUE(s.ok() && fields_.size() == ret);
  hash->Del(key_);
}

TEST_F(RedisHashTest, MGetAndMSet) {
  int ret;
  std::vector<FieldValue> fvs;
  for (int i = 0; i < fields_.size(); i++) {
    fvs.emplace_back(FieldValue{fields_[i].ToString(), values_[i].ToString()});
  }
  rocksdb::Status s = hash->MSet(key_, fvs, &ret);
  EXPECT_TRUE(s.ok() && fvs.size()==ret);
  s = hash->MSet(key_, fvs, &ret);
  EXPECT_TRUE(s.ok() && ret == 0);
  s = hash->Delete(key_, fields_, &ret);
  EXPECT_TRUE(s.ok() && fields_.size() == ret);
  hash->Del(key_);
}

TEST_F(RedisHashTest, SetNX) {
  int ret;
  Slice field("foo");
  rocksdb::Status s = hash->Set(key_, field, "bar", &ret);
  EXPECT_TRUE(s.ok() && ret == 1);
  s = hash->Set(key_, field, "bar", &ret);
  EXPECT_TRUE(s.ok() && ret == 0);
  std::vector<Slice> fields = {field};
  s = hash->Delete(key_, fields, &ret);
  EXPECT_EQ(fields.size(), ret);
  hash->Del(key_);
}

TEST_F(RedisHashTest, HGetAll) {
  int ret;
  for (int i = 0; i < fields_.size(); i++) {
    rocksdb::Status s = hash->Set(key_, fields_[i], values_[i], &ret);
    EXPECT_TRUE(s.ok() && ret == 1);
  }
  std::vector<FieldValue> fvs;
  rocksdb::Status s = hash->GetAll(key_, &fvs);
  EXPECT_TRUE(s.ok() && fvs.size() == fields_.size());
  s = hash->Delete(key_, fields_, &ret);
  EXPECT_TRUE(s.ok() && fields_.size() == ret);
  hash->Del(key_);
}

TEST_F(RedisHashTest, HIncr) {
  long long value;
  Slice field("hash-incrby-invalid-field");
  for (int i = 0; i < 32; i++) {
    rocksdb::Status s = hash->IncrBy(key_, field, 1, &value);
    EXPECT_TRUE(s.ok());
  }
  std::string bytes;
  hash->Get(key_, field, &bytes);
  value = std::stoll(bytes);
  EXPECT_EQ(32, value);
  hash->Del(key_);
}

TEST_F(RedisHashTest, HIncrInvalid) {
  int ret;
  long long value;
  Slice field("hash-incrby-invalid-field");
  rocksdb::Status s = hash->IncrBy(key_, field, 1, &value);
  EXPECT_TRUE(s.ok() && value == 1);

  s = hash->IncrBy(key_, field, LLONG_MAX, &value);
  EXPECT_TRUE(s.IsInvalidArgument());
  hash->Set(key_, field, "abc", &ret);
  s = hash->IncrBy(key_, field, 1, &value);
  EXPECT_TRUE(s.IsInvalidArgument());

  hash->Set(key_, field, "-1", &ret);
  s = hash->IncrBy(key_, field, -1, &value);
  EXPECT_TRUE(s.ok());
  s = hash->IncrBy(key_, field, LLONG_MIN, &value);
  EXPECT_TRUE(s.IsInvalidArgument());

  hash->Del(key_);
}

TEST_F(RedisHashTest, HIncrByFloat) {
  float value;
  Slice field("hash-incrbyfloat-invalid-field");
  for (int i = 0; i < 32; i++) {
    rocksdb::Status s = hash->IncrByFloat(key_, field, 1.2, &value);
    EXPECT_TRUE(s.ok());
  }
  std::string bytes;
  hash->Get(key_, field, &bytes);
  value = std::stof(bytes);
  EXPECT_FLOAT_EQ(32*1.2, value);
  hash->Del(key_);
}