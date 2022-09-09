// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <mutex>

#include "arrow/array.h"
#include "arrow/array/concatenate.h"
#include "arrow/dataset/api.h"
#include "arrow/dataset/dataset_internal.h"
#include "arrow/dataset/file_base.h"
#include "arrow/filesystem/localfs.h"
#include "arrow/ipc/api.h"
#include "arrow/c/helpers.h"
#include "arrow/util/iterator.h"
#include "jni/dataset/jni_util.h"
#include "org_apache_arrow_dataset_file_JniWrapper.h"
#include "org_apache_arrow_dataset_jni_JniWrapper.h"
#include "org_apache_arrow_dataset_jni_NativeMemoryPool.h"

#include <iostream>

namespace {

jclass illegal_access_exception_class;
jclass illegal_argument_exception_class;
jclass runtime_exception_class;

jclass java_reservation_listener_class;
jclass native_record_batch_iterator_class;

jmethodID reserve_memory_method;
jmethodID unreserve_memory_method;
jmethodID native_record_batch_iterator_hasNext;
jmethodID native_record_batch_iterator_next;

jlong default_memory_pool_id = -1L;

jint JNI_VERSION = JNI_VERSION_1_6;

class JniPendingException : public std::runtime_error {
 public:
  explicit JniPendingException(const std::string& arg) : runtime_error(arg) {}
};

void ThrowPendingException(const std::string& message) {
  throw JniPendingException(message);
}

template <typename T>
T JniGetOrThrow(arrow::Result<T> result) {
  if (!result.status().ok()) {
    ThrowPendingException(result.status().message());
  }
  return std::move(result).ValueOrDie();
}

void JniAssertOkOrThrow(arrow::Status status) {
  if (!status.ok()) {
    ThrowPendingException(status.message());
  }
}

void JniThrow(std::string message) { ThrowPendingException(message); }

arrow::Result<std::shared_ptr<arrow::dataset::FileFormat>> GetFileFormat(
    jint file_format_id) {
  switch (file_format_id) {
    case 0:
      return std::make_shared<arrow::dataset::ParquetFileFormat>();
    default:
      std::string error_message =
          "illegal file format id: " + std::to_string(file_format_id);
      return arrow::Status::Invalid(error_message);
  }
}

class ReserveFromJava : public arrow::dataset::jni::ReservationListener {
 public:
  ReserveFromJava(JavaVM* vm, jobject java_reservation_listener)
      : vm_(vm), java_reservation_listener_(java_reservation_listener) {}

  arrow::Status OnReservation(int64_t size) override {
    JNIEnv* env;
    if (vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION) != JNI_OK) {
      return arrow::Status::Invalid("JNIEnv was not attached to current thread");
    }
    env->CallObjectMethod(java_reservation_listener_, reserve_memory_method, size);
    RETURN_NOT_OK(arrow::dataset::jni::CheckException(env));
    return arrow::Status::OK();
  }

  arrow::Status OnRelease(int64_t size) override {
    JNIEnv* env;
    if (vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION) != JNI_OK) {
      return arrow::Status::Invalid("JNIEnv was not attached to current thread");
    }
    env->CallObjectMethod(java_reservation_listener_, unreserve_memory_method, size);
    RETURN_NOT_OK(arrow::dataset::jni::CheckException(env));
    return arrow::Status::OK();
  }

  jobject GetJavaReservationListener() { return java_reservation_listener_; }

 private:
  JavaVM* vm_;
  jobject java_reservation_listener_;
};

/// \class DisposableScannerAdaptor
/// \brief An adaptor that iterates over a Scanner instance then returns RecordBatches
/// directly.
///
/// This lessens the complexity of the JNI bridge to make sure it to be easier to
/// maintain. On Java-side, NativeScanner can only produces a single NativeScanTask
/// instance during its whole lifecycle. Each task stands for a DisposableScannerAdaptor
/// instance through JNI bridge.
///
class DisposableScannerAdaptor {
 public:
  DisposableScannerAdaptor(std::shared_ptr<arrow::dataset::Scanner> scanner,
                           arrow::dataset::TaggedRecordBatchIterator batch_itr)
      : scanner_(std::move(scanner)), batch_itr_(std::move(batch_itr)) {}

  static arrow::Result<std::shared_ptr<DisposableScannerAdaptor>> Create(
      std::shared_ptr<arrow::dataset::Scanner> scanner) {
    ARROW_ASSIGN_OR_RAISE(auto batch_itr, scanner->ScanBatches());
    return std::make_shared<DisposableScannerAdaptor>(scanner, std::move(batch_itr));
  }

  arrow::Result<std::shared_ptr<arrow::RecordBatch>> Next() {
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::RecordBatch> batch, NextBatch());
    return batch;
  }

  const std::shared_ptr<arrow::dataset::Scanner>& GetScanner() const { return scanner_; }

 private:
  std::shared_ptr<arrow::dataset::Scanner> scanner_;
  arrow::dataset::TaggedRecordBatchIterator batch_itr_;

  arrow::Result<std::shared_ptr<arrow::RecordBatch>> NextBatch() {
    ARROW_ASSIGN_OR_RAISE(auto batch, batch_itr_.Next())
    return batch.record_batch;
  }
};

/// \brief Simple fragment implementation that is constructed directly
/// from a record batch iterator.
class SimpleIteratorFragment : public arrow::dataset::Fragment {
 public:
  explicit SimpleIteratorFragment(arrow::RecordBatchIterator itr)
      : arrow::dataset::Fragment() {
    itr_ = std::move(itr);
  }

  static arrow::Result<std::shared_ptr<SimpleIteratorFragment>> Make(
      arrow::RecordBatchIterator itr) {
    return std::make_shared<SimpleIteratorFragment>(std::move(itr));
  }

  arrow::Result<arrow::RecordBatchGenerator> ScanBatchesAsync(
      const std::shared_ptr<arrow::dataset::ScanOptions>& options) override {
    struct State {
      State(std::shared_ptr<SimpleIteratorFragment> fragment)
          : fragment(std::move(fragment)) {}

      std::shared_ptr<arrow::RecordBatch> Next() { return cur_rb; }

      bool Finished() {
        arrow::Result<std::shared_ptr<arrow::RecordBatch>> next = fragment->itr_.Next();
        if (IsIterationEnd(next)) {
          cur_rb = nullptr;

          return true;
        } else {
          cur_rb = next.ValueOrDie();
          return false;
        }
      }

      std::shared_ptr<SimpleIteratorFragment> fragment;
      std::shared_ptr<arrow::RecordBatch> cur_rb = nullptr;
    };

    struct Generator {
      Generator(std::shared_ptr<SimpleIteratorFragment> fragment)
          : state(std::make_shared<State>(std::move(fragment))) {}

      arrow::Future<std::shared_ptr<arrow::RecordBatch>> operator()() {
        while (!state->Finished()) {
          auto next = state->Next();
          if (next) {
            return arrow::Future<std::shared_ptr<arrow::RecordBatch>>::MakeFinished(
                std::move(next));
          }
        }
        return arrow::AsyncGeneratorEnd<std::shared_ptr<arrow::RecordBatch>>();
      }

      std::shared_ptr<State> state;
    };
    return Generator(arrow::internal::checked_pointer_cast<SimpleIteratorFragment>(
        shared_from_this()));
  }

  std::string type_name() const override { return "simple_iterator"; }

  arrow::Result<std::shared_ptr<arrow::Schema>> ReadPhysicalSchemaImpl() override {
    return arrow::Status::NotImplemented("No physical schema is readable");
  }

 private:
  arrow::RecordBatchIterator itr_;
  bool used_ = false;
};

/// \brief Create scanner that scans over Java dataset API's components.
///
/// Currently, we use a NativeRecordBatchIterator as the underlying
/// Java object to do scanning. Which means, only one single task will
/// be produced from C++ code.
arrow::Result<std::shared_ptr<arrow::dataset::Scanner>> MakeJavaDatasetScanner(
    JavaVM* vm, jobject iter, std::shared_ptr<arrow::Schema> schema) {
  arrow::RecordBatchIterator itr = arrow::MakeFunctionIterator(
      [vm, iter, schema]() -> arrow::Result<std::shared_ptr<arrow::RecordBatch>> {
        JNIEnv* env;
        if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION) != JNI_OK) {
          return arrow::Status::Invalid("JNIEnv was not attached to current thread");
        }
        if (!env->CallBooleanMethod(iter, native_record_batch_iterator_hasNext)) {
          return nullptr;  // stream ended
        }

        auto bytes =
            (jbyteArray)env->CallObjectMethod(iter, native_record_batch_iterator_next);
        auto byte_array = env->GetByteArrayElements(bytes, 0);
        int64_t memory_address;
        std::memcpy(&memory_address, byte_array, sizeof(int64_t));

        std::shared_ptr<arrow::RecordBatch> rb = JniGetOrThrow(
            arrow::dataset::jni::ImportRecordBatch(env, schema, memory_address));
        // Release the ArrowArray
        auto c_array = reinterpret_cast<struct ArrowArray*>(memory_address);
        ArrowArrayRelease(c_array);
        return rb;
      });

  ARROW_ASSIGN_OR_RAISE(auto fragment, SimpleIteratorFragment::Make(std::move(itr)))

  arrow::dataset::ScannerBuilder scanner_builder(
      std::move(schema), fragment, std::make_shared<arrow::dataset::ScanOptions>());
  // Use default memory pool is enough as native allocation is ideally
  // not being called during scanning Java-based fragments.
  RETURN_NOT_OK(scanner_builder.Pool(arrow::default_memory_pool()));
  return scanner_builder.Finish();
}
}  // namespace

using arrow::dataset::jni::CreateGlobalClassReference;
using arrow::dataset::jni::CreateNativeRef;
using arrow::dataset::jni::FromSchemaByteArray;
using arrow::dataset::jni::GetMethodID;
using arrow::dataset::jni::JStringToCString;
using arrow::dataset::jni::ReleaseNativeRef;
using arrow::dataset::jni::RetrieveNativeInstance;
using arrow::dataset::jni::ToSchemaByteArray;
using arrow::dataset::jni::ToStringVector;

using arrow::dataset::jni::ReservationListenableMemoryPool;
using arrow::dataset::jni::ReservationListener;

#define JNI_METHOD_START try {
// macro ended

#define JNI_METHOD_END(fallback_expr)                 \
  }                                                   \
  catch (JniPendingException & e) {                   \
    env->ThrowNew(runtime_exception_class, e.what()); \
    return fallback_expr;                             \
  }
// macro ended

jint JNI_OnLoad(JavaVM* vm, void* reserved) {
  JNIEnv* env;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION) != JNI_OK) {
    return JNI_ERR;
  }
  JNI_METHOD_START
  illegal_access_exception_class =
      CreateGlobalClassReference(env, "Ljava/lang/IllegalAccessException;");
  illegal_argument_exception_class =
      CreateGlobalClassReference(env, "Ljava/lang/IllegalArgumentException;");
  runtime_exception_class =
      CreateGlobalClassReference(env, "Ljava/lang/RuntimeException;");

  java_reservation_listener_class =
      CreateGlobalClassReference(env,
                                 "Lorg/apache/arrow/"
                                 "dataset/jni/ReservationListener;");
  reserve_memory_method =
      JniGetOrThrow(GetMethodID(env, java_reservation_listener_class, "reserve", "(J)V"));
  unreserve_memory_method = JniGetOrThrow(
      GetMethodID(env, java_reservation_listener_class, "unreserve", "(J)V"));

  native_record_batch_iterator_class =
      CreateGlobalClassReference(env,
                                 "Lorg/apache/arrow/"
                                 "dataset/jni/NativeRecordBatchIterator;");
  native_record_batch_iterator_hasNext = JniGetOrThrow(
      GetMethodID(env, native_record_batch_iterator_class, "hasNext", "()Z"));
  native_record_batch_iterator_next =
      JniGetOrThrow(GetMethodID(env, native_record_batch_iterator_class, "next", "()[B"));

  default_memory_pool_id = reinterpret_cast<jlong>(arrow::default_memory_pool());

  return JNI_VERSION;
  JNI_METHOD_END(JNI_ERR)
}

void JNI_OnUnload(JavaVM* vm, void* reserved) {
  JNIEnv* env;
  vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION);
  env->DeleteGlobalRef(illegal_access_exception_class);
  env->DeleteGlobalRef(illegal_argument_exception_class);
  env->DeleteGlobalRef(runtime_exception_class);
  env->DeleteGlobalRef(java_reservation_listener_class);
  env->DeleteGlobalRef(native_record_batch_iterator_class);

  default_memory_pool_id = -1L;
}

/*
 * Class:     org_apache_arrow_dataset_jni_NativeMemoryPool
 * Method:    getDefaultMemoryPool
 * Signature: ()J
 */
JNIEXPORT jlong JNICALL
Java_org_apache_arrow_dataset_jni_NativeMemoryPool_getDefaultMemoryPool(JNIEnv* env,
                                                                        jclass) {
  JNI_METHOD_START
  return default_memory_pool_id;
  JNI_METHOD_END(-1L)
}

/*
 * Class:     org_apache_arrow_dataset_jni_NativeMemoryPool
 * Method:    createListenableMemoryPool
 * Signature: (Lorg/apache/arrow/memory/ReservationListener;)J
 */
JNIEXPORT jlong JNICALL
Java_org_apache_arrow_dataset_jni_NativeMemoryPool_createListenableMemoryPool(
    JNIEnv* env, jclass, jobject jlistener) {
  JNI_METHOD_START
  jobject jlistener_ref = env->NewGlobalRef(jlistener);
  JavaVM* vm;
  if (env->GetJavaVM(&vm) != JNI_OK) {
    JniThrow("Unable to get JavaVM instance");
  }
  std::shared_ptr<ReservationListener> listener =
      std::make_shared<ReserveFromJava>(vm, jlistener_ref);
  auto memory_pool =
      new ReservationListenableMemoryPool(arrow::default_memory_pool(), listener);
  return reinterpret_cast<jlong>(memory_pool);
  JNI_METHOD_END(-1L)
}

/*
 * Class:     org_apache_arrow_dataset_jni_NativeMemoryPool
 * Method:    releaseMemoryPool
 * Signature: (J)V
 */
JNIEXPORT void JNICALL
Java_org_apache_arrow_dataset_jni_NativeMemoryPool_releaseMemoryPool(
    JNIEnv* env, jclass, jlong memory_pool_id) {
  JNI_METHOD_START
  if (memory_pool_id == default_memory_pool_id) {
    return;
  }
  ReservationListenableMemoryPool* pool =
      reinterpret_cast<ReservationListenableMemoryPool*>(memory_pool_id);
  if (pool == nullptr) {
    return;
  }
  std::shared_ptr<ReserveFromJava> rm =
      std::dynamic_pointer_cast<ReserveFromJava>(pool->get_listener());
  if (rm == nullptr) {
    delete pool;
    return;
  }
  delete pool;
  env->DeleteGlobalRef(rm->GetJavaReservationListener());
  JNI_METHOD_END()
}

/*
 * Class:     org_apache_arrow_dataset_jni_NativeMemoryPool
 * Method:    bytesAllocated
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL Java_org_apache_arrow_dataset_jni_NativeMemoryPool_bytesAllocated(
    JNIEnv* env, jclass, jlong memory_pool_id) {
  JNI_METHOD_START
  arrow::MemoryPool* pool = reinterpret_cast<arrow::MemoryPool*>(memory_pool_id);
  if (pool == nullptr) {
    JniThrow("Memory pool instance not found. It may not exist nor has been closed");
  }
  return pool->bytes_allocated();
  JNI_METHOD_END(-1L)
}

/*
 * Class:     org_apache_arrow_dataset_jni_JniWrapper
 * Method:    closeDatasetFactory
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_org_apache_arrow_dataset_jni_JniWrapper_closeDatasetFactory(
    JNIEnv* env, jobject, jlong id) {
  JNI_METHOD_START
  ReleaseNativeRef<arrow::dataset::DatasetFactory>(id);
  JNI_METHOD_END()
}

/*
 * Class:     org_apache_arrow_dataset_jni_JniWrapper
 * Method:    inspectSchema
 * Signature: (J)[B
 */
JNIEXPORT jbyteArray JNICALL Java_org_apache_arrow_dataset_jni_JniWrapper_inspectSchema(
    JNIEnv* env, jobject, jlong dataset_factor_id) {
  JNI_METHOD_START
  std::shared_ptr<arrow::dataset::DatasetFactory> d =
      RetrieveNativeInstance<arrow::dataset::DatasetFactory>(dataset_factor_id);
  std::shared_ptr<arrow::Schema> schema = JniGetOrThrow(d->Inspect());
  return JniGetOrThrow(ToSchemaByteArray(env, schema));
  JNI_METHOD_END(nullptr)
}

/*
 * Class:     org_apache_arrow_dataset_jni_JniWrapper
 * Method:    createDataset
 * Signature: (J[B)J
 */
JNIEXPORT jlong JNICALL Java_org_apache_arrow_dataset_jni_JniWrapper_createDataset(
    JNIEnv* env, jobject, jlong dataset_factory_id, jbyteArray schema_bytes) {
  JNI_METHOD_START
  std::shared_ptr<arrow::dataset::DatasetFactory> d =
      RetrieveNativeInstance<arrow::dataset::DatasetFactory>(dataset_factory_id);
  std::shared_ptr<arrow::Schema> schema;
  schema = JniGetOrThrow(FromSchemaByteArray(env, schema_bytes));
  std::shared_ptr<arrow::dataset::Dataset> dataset = JniGetOrThrow(d->Finish(schema));
  return CreateNativeRef(dataset);
  JNI_METHOD_END(-1L)
}

/*
 * Class:     org_apache_arrow_dataset_jni_JniWrapper
 * Method:    closeDataset
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_org_apache_arrow_dataset_jni_JniWrapper_closeDataset(
    JNIEnv* env, jobject, jlong id) {
  JNI_METHOD_START
  ReleaseNativeRef<arrow::dataset::Dataset>(id);
  JNI_METHOD_END()
}

/*
 * Class:     org_apache_arrow_dataset_jni_JniWrapper
 * Method:    createScanner
 * Signature: (J[Ljava/lang/String;JJ)J
 */
JNIEXPORT jlong JNICALL Java_org_apache_arrow_dataset_jni_JniWrapper_createScanner(
    JNIEnv* env, jobject, jlong dataset_id, jobjectArray columns, jlong batch_size,
    jlong memory_pool_id) {
  JNI_METHOD_START
  arrow::MemoryPool* pool = reinterpret_cast<arrow::MemoryPool*>(memory_pool_id);
  if (pool == nullptr) {
    JniThrow("Memory pool does not exist or has been closed");
  }
  std::shared_ptr<arrow::dataset::Dataset> dataset =
      RetrieveNativeInstance<arrow::dataset::Dataset>(dataset_id);
  std::shared_ptr<arrow::dataset::ScannerBuilder> scanner_builder =
      JniGetOrThrow(dataset->NewScan());
  JniAssertOkOrThrow(scanner_builder->Pool(pool));
  if (columns != nullptr) {
    std::vector<std::string> column_vector = ToStringVector(env, columns);
    JniAssertOkOrThrow(scanner_builder->Project(column_vector));
  }
  JniAssertOkOrThrow(scanner_builder->BatchSize(batch_size));

  auto scanner = JniGetOrThrow(scanner_builder->Finish());
  std::shared_ptr<DisposableScannerAdaptor> scanner_adaptor =
      JniGetOrThrow(DisposableScannerAdaptor::Create(scanner));
  jlong id = CreateNativeRef(scanner_adaptor);
  return id;
  JNI_METHOD_END(-1L)
}

/*
 * Class:     org_apache_arrow_dataset_jni_JniWrapper
 * Method:    closeScanner
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_org_apache_arrow_dataset_jni_JniWrapper_closeScanner(
    JNIEnv* env, jobject, jlong scanner_id) {
  JNI_METHOD_START
  ReleaseNativeRef<DisposableScannerAdaptor>(scanner_id);
  JNI_METHOD_END()
}

/*
 * Class:     org_apache_arrow_dataset_jni_JniWrapper
 * Method:    getSchemaFromScanner
 * Signature: (J)[B
 */
JNIEXPORT jbyteArray JNICALL
Java_org_apache_arrow_dataset_jni_JniWrapper_getSchemaFromScanner(JNIEnv* env, jobject,
                                                                  jlong scanner_id) {
  JNI_METHOD_START
  std::shared_ptr<arrow::Schema> schema =
      RetrieveNativeInstance<DisposableScannerAdaptor>(scanner_id)
          ->GetScanner()
          ->options()
          ->projected_schema;
  return JniGetOrThrow(ToSchemaByteArray(env, schema));
  JNI_METHOD_END(nullptr)
}

/*
 * Class:     org_apache_arrow_dataset_jni_JniWrapper
 * Method:    nextRecordBatch
 * Signature: (JJ)Z
 */
JNIEXPORT jboolean JNICALL Java_org_apache_arrow_dataset_jni_JniWrapper_nextRecordBatch(
    JNIEnv* env, jobject, jlong scanner_id, jlong struct_array) {
  JNI_METHOD_START
  std::shared_ptr<DisposableScannerAdaptor> scanner_adaptor =
      RetrieveNativeInstance<DisposableScannerAdaptor>(scanner_id);

  std::shared_ptr<arrow::RecordBatch> record_batch =
      JniGetOrThrow(scanner_adaptor->Next());
  if (record_batch == nullptr) {
    return false;  // stream ended
  }
  std::vector<std::shared_ptr<arrow::Array>> offset_zeroed_arrays;
  for (int i = 0; i < record_batch->num_columns(); ++i) {
    // TODO: If the array has an offset then we need to de-offset the array
    // in order for it to be properly consumed on the Java end.
    // This forces a copy, it would be nice to avoid this if Java
    // could consume offset-arrays.  Perhaps at some point in the future
    // using the C data interface.  See ARROW-15275
    //
    // Generally a non-zero offset will occur whenever the scanner batch
    // size is smaller than the batch size of the underlying files.
    std::shared_ptr<arrow::Array> array = record_batch->column(i);
    if (array->offset() == 0) {
      offset_zeroed_arrays.push_back(array);
      continue;
    }
    std::shared_ptr<arrow::Array> offset_zeroed =
        JniGetOrThrow(arrow::Concatenate({array}));
    offset_zeroed_arrays.push_back(offset_zeroed);
  }

  std::shared_ptr<arrow::RecordBatch> offset_zeroed_batch = arrow::RecordBatch::Make(
      record_batch->schema(), record_batch->num_rows(), offset_zeroed_arrays);
  JniAssertOkOrThrow(
      arrow::dataset::jni::ExportRecordBatch(env, offset_zeroed_batch, struct_array));
  return true;
  JNI_METHOD_END(false)
}

/*
 * Class:     org_apache_arrow_dataset_jni_JniWrapper
 * Method:    releaseBuffer
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_org_apache_arrow_dataset_jni_JniWrapper_releaseBuffer(
    JNIEnv* env, jobject, jlong id) {
  JNI_METHOD_START
  ReleaseNativeRef<arrow::Buffer>(id);
  JNI_METHOD_END()
}

/*
 * Class:     org_apache_arrow_dataset_file_JniWrapper
 * Method:    makeFileSystemDatasetFactory
 * Signature: (Ljava/lang/String;II)J
 */
JNIEXPORT jlong JNICALL
Java_org_apache_arrow_dataset_file_JniWrapper_makeFileSystemDatasetFactory(
    JNIEnv* env, jobject, jstring uri, jint file_format_id, jlong start_offset,
    jlong length) {
  JNI_METHOD_START
  std::shared_ptr<arrow::dataset::FileFormat> file_format =
      JniGetOrThrow(GetFileFormat(file_format_id));
  arrow::dataset::FileSystemFactoryOptions options;
  std::shared_ptr<arrow::dataset::DatasetFactory> d =
      JniGetOrThrow(arrow::dataset::FileSystemDatasetFactory::Make(
          JStringToCString(env, uri), start_offset, length, file_format, options));
  return CreateNativeRef(d);
  JNI_METHOD_END(-1L)
}

/*
 * Class:     org_apache_arrow_dataset_file_JniWrapper
 * Method:    writeFromScannerToFile
 * Signature:
 * (Lorg/apache/arrow/dataset/jni/NativeRecordBatchIterator;[BJLjava/lang/String;[Ljava/lang/String;ILjava/lang/String;)V
 */
JNIEXPORT void JNICALL
Java_org_apache_arrow_dataset_file_JniWrapper_writeFromScannerToFile(
    JNIEnv* env, jobject, jobject itr, jbyteArray schema_bytes, jlong file_format_id,
    jstring uri, jobjectArray partition_columns, jint max_partitions,
    jstring base_name_template) {
  JNI_METHOD_START
  JavaVM* vm;
  if (env->GetJavaVM(&vm) != JNI_OK) {
    JniThrow("Unable to get JavaVM instance");
  }
  auto schema = JniGetOrThrow(FromSchemaByteArray(env, schema_bytes));
  auto scanner = JniGetOrThrow(MakeJavaDatasetScanner(vm, itr, schema));
  std::shared_ptr<arrow::dataset::FileFormat> file_format =
      JniGetOrThrow(GetFileFormat(file_format_id));
  arrow::dataset::FileSystemDatasetWriteOptions options;
  std::string output_path;
  auto filesystem = JniGetOrThrow(
      arrow::fs::FileSystemFromUri(JStringToCString(env, uri), &output_path));
  std::vector<std::string> partition_column_vector =
      ToStringVector(env, partition_columns);
  options.file_write_options = file_format->DefaultWriteOptions();
  options.filesystem = filesystem;
  options.base_dir = output_path;
  options.basename_template = JStringToCString(env, base_name_template);
  options.partitioning = std::make_shared<arrow::dataset::HivePartitioning>(
      arrow::dataset::SchemaFromColumnNames(schema, partition_column_vector));
  options.max_partitions = max_partitions;
  JniAssertOkOrThrow(arrow::dataset::FileSystemDataset::Write(options, scanner));
  JNI_METHOD_END()
}
