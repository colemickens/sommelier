/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <base/at_exit.h>
#include <base/command_line.h>
#include <base/files/file_util.h>
#include <base/memory/shared_memory.h>
#include <base/threading/thread.h>
#include <gtest/gtest.h>
#include <libyuv.h>

#include "common/jpeg/jpeg_decode_accelerator_impl.h"
#include "cros-camera/future.h"

namespace cros {

namespace tests {
// Environment to create test data for all test cases.
class JpegDecodeTestEnvironment;
JpegDecodeTestEnvironment* g_env;

namespace {
// Download test image URI.
const char* kDownloadTestImageURI1 =
    "https://storage.googleapis.com/chromeos-localmirror/distfiles/"
    "peach_pi-1280x720.jpg";
const char* kDownloadTestImageURI2 =
    "https://storage.googleapis.com/chromeos-localmirror/distfiles/"
    "field-1280x720.jpg";

// Default test image file.
const char kDefaultJpegFilename1[] = "peach_pi-1280x720.jpg";
const char kDefaultJpegFilename2[] = "field-1280x720.jpg";
// Threshold for mean absolute difference of hardware and software decode.
// Absolute difference is to calculate the difference between each pixel in two
// images. This is used for measuring of the similarity of two images.
const double kDecodeSimilarityThreshold = 1.0;
// Bytes per pixel for YUV420 format
const double kYUV420_BytesFactor = 6.0 / 4;

const int kMaxDecoderNumber = 3;
}  // namespace

struct Frame {
  // The input content of the tested Jpeg file.
  // It will be loaded after call LoadFrame.
  std::string data_str;
  int width;
  int height;

  // Mapped memory of input file.
  std::unique_ptr<base::SharedMemory> in_shm;
  // Mapped memory of output buffer from hardware decoder.
  std::unique_ptr<base::SharedMemory> hw_out_shm;
  // Mapped memory of output buffer from software decoder.
  std::unique_ptr<base::SharedMemory> sw_out_shm;
};

class JpegDecodeAcceleratorTest : public ::testing::Test {
 public:
  JpegDecodeAcceleratorTest() {
    for (size_t i = 0; i < kMaxDecoderNumber; i++) {
      jpeg_decoder_[i] = std::make_unique<JpegDecodeAcceleratorImpl>();
    }
  }

  ~JpegDecodeAcceleratorTest() {}
  void SetUp() {}

  void TearDown() {}

  void LoadFrame(const char* jpeg_filename, Frame* frame);
  void PrepareMemory(Frame* frame);
  bool GetSoftwareDecodeResult(Frame* frame);
  double GetMeanAbsoluteDifference(Frame* frame);
  void DecodeTest(Frame* frame, size_t decoder_id);
  void DecodeTestAsync(Frame* frame, DecodeCallback callback);
  void DecodeSyncCallback(base::Callback<void(int)> callback,
                          int32_t buffer_id,
                          int error);
  void ResetJDAChannel();

 protected:
  std::unique_ptr<JpegDecodeAcceleratorImpl> jpeg_decoder_[kMaxDecoderNumber];

  Frame jpeg_frame1_;
  Frame jpeg_frame2_;

 private:
  void ResetJDAChannelOnIpcThread(scoped_refptr<cros::Future<void>> future);

  DISALLOW_COPY_AND_ASSIGN(JpegDecodeAcceleratorTest);
};

class JpegDecodeTestEnvironment : public ::testing::Environment {
 public:
  explicit JpegDecodeTestEnvironment(const char* jpeg_filename1,
                                     const char* jpeg_filename2) {
    jpeg_filename1_ = jpeg_filename1 ? jpeg_filename1 : kDefaultJpegFilename1;
    jpeg_filename2_ = jpeg_filename2 ? jpeg_filename2 : kDefaultJpegFilename2;
  }

  const char* jpeg_filename1_;
  const char* jpeg_filename2_;
};

void JpegDecodeAcceleratorTest::LoadFrame(const char* jpeg_filename,
                                          Frame* frame) {
  base::FilePath jpeg_filepath = base::FilePath(jpeg_filename);

  if (!PathExists(jpeg_filepath)) {
    LOG(ERROR) << "There is no test image file: " << jpeg_filepath.value();
    LOG(ERROR) << "You may download one from " << kDownloadTestImageURI1;
    LOG(ERROR) << " Or from " << kDownloadTestImageURI2;
    return;
  }

  LOG(INFO) << "Read file:" << jpeg_filepath.value();
  ASSERT_TRUE(base::ReadFileToString(jpeg_filepath, &frame->data_str));
  EXPECT_EQ(
      libyuv::MJPGSize(reinterpret_cast<const uint8_t*>(frame->data_str.data()),
                       frame->data_str.size(), &frame->width, &frame->height),
      0);

  VLOG(1) << "GetWidth() = " << frame->width
          << ",GetHeight() = " << frame->height;
}

void JpegDecodeAcceleratorTest::PrepareMemory(Frame* frame) {
  size_t input_size = frame->data_str.size();
  // Prepare enought size of YUV420 format.
  size_t output_size = frame->width * frame->height * kYUV420_BytesFactor;
  if (!frame->in_shm.get() || input_size > frame->in_shm->mapped_size()) {
    frame->in_shm.reset(new base::SharedMemory);
    LOG_ASSERT(frame->in_shm->CreateAndMapAnonymous(input_size));
  }
  memcpy(frame->in_shm->memory(), frame->data_str.data(), input_size);

  if (!frame->hw_out_shm.get() ||
      output_size > frame->hw_out_shm->mapped_size()) {
    frame->hw_out_shm.reset(new base::SharedMemory);
    LOG_ASSERT(frame->hw_out_shm->CreateAndMapAnonymous(output_size));
  }
  memset(frame->hw_out_shm->memory(), 0, output_size);

  if (!frame->sw_out_shm.get() ||
      output_size > frame->sw_out_shm->mapped_size()) {
    frame->sw_out_shm.reset(new base::SharedMemory);
    LOG_ASSERT(frame->sw_out_shm->CreateAndMapAnonymous(output_size));
  }
  memset(frame->sw_out_shm->memory(), 0, output_size);
}

double JpegDecodeAcceleratorTest::GetMeanAbsoluteDifference(Frame* frame) {
  double total_difference = 0;
  int output_size = frame->width * frame->height * kYUV420_BytesFactor;
  uint8_t* hw_ptr = static_cast<uint8_t*>(frame->hw_out_shm->memory());
  uint8_t* sw_ptr = static_cast<uint8_t*>(frame->sw_out_shm->memory());
  for (size_t i = 0; i < output_size; i++)
    total_difference += std::abs(hw_ptr[i] - sw_ptr[i]);
  return total_difference / output_size;
}

bool JpegDecodeAcceleratorTest::GetSoftwareDecodeResult(Frame* frame) {
  uint8_t* yplane = static_cast<uint8_t*>(frame->sw_out_shm->memory());
  uint8_t* uplane = yplane + frame->width * frame->height;
  uint8_t* vplane = uplane + frame->width * frame->height / 4;
  int yplane_stride = frame->width;
  int uv_plane_stride = yplane_stride / 2;

  if (libyuv::ConvertToI420(static_cast<uint8_t*>(frame->in_shm->memory()),
                            frame->data_str.size(), yplane, yplane_stride,
                            uplane, uv_plane_stride, vplane, uv_plane_stride, 0,
                            0, frame->width, frame->height, frame->width,
                            frame->height, libyuv::kRotate0,
                            libyuv::FOURCC_MJPG) != 0) {
    LOG(ERROR) << "Software decode failed.";
    return false;
  }
  return true;
}

void JpegDecodeAcceleratorTest::DecodeTest(Frame* frame, size_t decoder_id) {
  base::SharedMemoryHandle input_handle;
  base::SharedMemoryHandle output_handle;
  int input_fd, output_fd;
  JpegDecodeAccelerator::Error error;

  // Clear HW Decode results.
  memset(frame->hw_out_shm->memory(), 0, frame->hw_out_shm->mapped_size());

  input_handle = frame->in_shm->handle();
  output_handle = frame->hw_out_shm->handle();
  input_fd = base::SharedMemory::GetFdFromSharedMemoryHandle(input_handle);
  output_fd = base::SharedMemory::GetFdFromSharedMemoryHandle(output_handle);
  VLOG(1) << "input fd " << input_fd << " output fd " << output_fd;

  // Pretend the shared memory as DMA buffer.
  // Since we all use mmap to get the user space address.
  error = jpeg_decoder_[decoder_id]->DecodeSync(
      input_fd, frame->in_shm->mapped_size(), frame->width, frame->height,
      output_fd, frame->hw_out_shm->mapped_size());
  EXPECT_EQ(error, JpegDecodeAccelerator::Error::NO_ERRORS);
  if (error == JpegDecodeAccelerator::Error::NO_ERRORS) {
    double difference = GetMeanAbsoluteDifference(frame);
    EXPECT_LE(difference, kDecodeSimilarityThreshold);
  }
}

void JpegDecodeAcceleratorTest::DecodeTestAsync(Frame* frame,
                                                DecodeCallback callback) {
  base::SharedMemoryHandle input_handle;
  base::SharedMemoryHandle output_handle;
  int input_fd, output_fd;

  // Clear HW Decode results.
  memset(frame->hw_out_shm->memory(), 0, frame->hw_out_shm->mapped_size());

  input_handle = frame->in_shm->handle();
  output_handle = frame->hw_out_shm->handle();
  input_fd = base::SharedMemory::GetFdFromSharedMemoryHandle(input_handle);
  output_fd = base::SharedMemory::GetFdFromSharedMemoryHandle(output_handle);
  VLOG(1) << "input fd " << input_fd << " output fd " << output_fd;

  jpeg_decoder_[0]->Decode(input_fd, frame->in_shm->mapped_size(), frame->width,
                           frame->height, output_fd,
                           frame->hw_out_shm->mapped_size(), callback);
}

void JpegDecodeAcceleratorTest::DecodeSyncCallback(
    base::Callback<void(int)> callback, int32_t buffer_id, int error) {
  callback.Run(error);
}

void JpegDecodeAcceleratorTest::ResetJDAChannel() {
  jpeg_decoder_[0]->TestResetJDAChannel();
}

TEST_F(JpegDecodeAcceleratorTest, InitTest) {
  for (size_t i = 0; i < kMaxDecoderNumber; i++) {
    EXPECT_TRUE(jpeg_decoder_[i]->Start());
  }
}

TEST_F(JpegDecodeAcceleratorTest, DecodeTest) {
  EXPECT_TRUE(jpeg_decoder_[0]->Start());
  LoadFrame(g_env->jpeg_filename1_, &jpeg_frame1_);
  PrepareMemory(&jpeg_frame1_);

  EXPECT_TRUE(GetSoftwareDecodeResult(&jpeg_frame1_));

  DecodeTest(&jpeg_frame1_, 0);
}

TEST_F(JpegDecodeAcceleratorTest, MultiDecodesTest) {
  for (size_t i = 0; i < kMaxDecoderNumber; i++) {
    EXPECT_TRUE(jpeg_decoder_[i]->Start());
  }

  LoadFrame(g_env->jpeg_filename1_, &jpeg_frame1_);
  PrepareMemory(&jpeg_frame1_);

  LoadFrame(g_env->jpeg_filename2_, &jpeg_frame2_);
  PrepareMemory(&jpeg_frame2_);

  EXPECT_TRUE(GetSoftwareDecodeResult(&jpeg_frame1_));
  EXPECT_TRUE(GetSoftwareDecodeResult(&jpeg_frame2_));

  DecodeTest(&jpeg_frame1_, 0);
  DecodeTest(&jpeg_frame2_, 1);
}

TEST_F(JpegDecodeAcceleratorTest, DecodeFailTest) {
  base::SharedMemoryHandle input_handle;
  base::SharedMemoryHandle output_handle;
  int input_fd, output_fd;
  JpegDecodeAccelerator::Error error;

  LoadFrame(g_env->jpeg_filename1_, &jpeg_frame1_);
  PrepareMemory(&jpeg_frame1_);

  // Corrupt jpeg content
  memset(jpeg_frame1_.in_shm->memory(), 0, jpeg_frame1_.in_shm->mapped_size());
  input_handle = jpeg_frame1_.in_shm->handle();
  output_handle = jpeg_frame1_.hw_out_shm->handle();
  input_fd = base::SharedMemory::GetFdFromSharedMemoryHandle(input_handle);
  output_fd = base::SharedMemory::GetFdFromSharedMemoryHandle(output_handle);
  VLOG(1) << "input fd " << input_fd << " output fd " << output_fd;

  EXPECT_TRUE(jpeg_decoder_[0]->Start());
  error = jpeg_decoder_[0]->DecodeSync(
      input_fd, jpeg_frame1_.in_shm->mapped_size(), jpeg_frame1_.width,
      jpeg_frame1_.height, output_fd, jpeg_frame1_.hw_out_shm->mapped_size());

  EXPECT_EQ(error, JpegDecodeAccelerator::Error::PARSE_JPEG_FAILED);
}

TEST_F(JpegDecodeAcceleratorTest, Decode60Images) {
  LoadFrame(g_env->jpeg_filename1_, &jpeg_frame1_);
  PrepareMemory(&jpeg_frame1_);
  EXPECT_TRUE(GetSoftwareDecodeResult(&jpeg_frame1_));

  EXPECT_TRUE(jpeg_decoder_[0]->Start());
  for (size_t i = 0; i < 60; i++) {
    DecodeTest(&jpeg_frame1_, 0);
  }
}

TEST_F(JpegDecodeAcceleratorTest, DecodeAsync) {
  LoadFrame(g_env->jpeg_filename1_, &jpeg_frame1_);
  PrepareMemory(&jpeg_frame1_);
  EXPECT_TRUE(GetSoftwareDecodeResult(&jpeg_frame1_));

  auto future1 = cros::Future<int>::Create(nullptr);

  EXPECT_TRUE(jpeg_decoder_[0]->Start());

  DecodeTestAsync(
      &jpeg_frame1_,
      base::Bind(&JpegDecodeAcceleratorTest::DecodeSyncCallback,
                 base::Unretained(this), cros::GetFutureCallback(future1)));

  EXPECT_TRUE(future1->Wait());
  EXPECT_EQ(future1->Get(),
            static_cast<int>(JpegDecodeAccelerator::Error::NO_ERRORS));

  double difference = GetMeanAbsoluteDifference(&jpeg_frame1_);
  EXPECT_LE(difference, kDecodeSimilarityThreshold);
}

TEST_F(JpegDecodeAcceleratorTest, DecodeAsync2) {
  EXPECT_TRUE(jpeg_decoder_[0]->Start());

  LoadFrame(g_env->jpeg_filename1_, &jpeg_frame1_);
  PrepareMemory(&jpeg_frame1_);
  EXPECT_TRUE(GetSoftwareDecodeResult(&jpeg_frame1_));

  LoadFrame(g_env->jpeg_filename2_, &jpeg_frame2_);
  PrepareMemory(&jpeg_frame2_);
  EXPECT_TRUE(GetSoftwareDecodeResult(&jpeg_frame2_));

  auto future1 = cros::Future<int>::Create(nullptr);
  auto future2 = cros::Future<int>::Create(nullptr);

  DecodeTestAsync(
      &jpeg_frame1_,
      base::Bind(&JpegDecodeAcceleratorTest::DecodeSyncCallback,
                 base::Unretained(this), cros::GetFutureCallback(future1)));

  DecodeTestAsync(
      &jpeg_frame2_,
      base::Bind(&JpegDecodeAcceleratorTest::DecodeSyncCallback,
                 base::Unretained(this), cros::GetFutureCallback(future2)));
  EXPECT_TRUE(future2->Wait());
  EXPECT_EQ(future2->Get(),
            static_cast<int>(JpegDecodeAccelerator::Error::NO_ERRORS));

  double difference = GetMeanAbsoluteDifference(&jpeg_frame1_);
  EXPECT_LE(difference, kDecodeSimilarityThreshold);

  difference = GetMeanAbsoluteDifference(&jpeg_frame2_);
  EXPECT_LE(difference, kDecodeSimilarityThreshold);
}

TEST_F(JpegDecodeAcceleratorTest, Decode6000Images) {
  LoadFrame(g_env->jpeg_filename1_, &jpeg_frame1_);
  PrepareMemory(&jpeg_frame1_);
  EXPECT_TRUE(GetSoftwareDecodeResult(&jpeg_frame1_));

  for (size_t i = 0; i < kMaxDecoderNumber; i++) {
    EXPECT_TRUE(jpeg_decoder_[i]->Start());
  }
  for (size_t i = 0; i < 6000; i++) {
    DecodeTest(&jpeg_frame1_, i % kMaxDecoderNumber);
  }
}

TEST_F(JpegDecodeAcceleratorTest, LostMojoChannel) {
  EXPECT_TRUE(jpeg_decoder_[0]->Start());
  LoadFrame(g_env->jpeg_filename1_, &jpeg_frame1_);
  PrepareMemory(&jpeg_frame1_);

  EXPECT_TRUE(GetSoftwareDecodeResult(&jpeg_frame1_));

  DecodeTest(&jpeg_frame1_, 0);

  ResetJDAChannel();
  // The channel is broken now, use wrong parameters here.
  // It shouldn't be INVALID_ARGUMENT error.
  JpegDecodeAccelerator::Error error =
      jpeg_decoder_[0]->DecodeSync(0, 0, 0, 0, 0, 0);
  EXPECT_EQ(error, JpegDecodeAccelerator::Error::TRY_START_AGAIN);

  // Call start again and test decode jpeg.
  EXPECT_TRUE(jpeg_decoder_[0]->Start());
  DecodeTest(&jpeg_frame1_, 0);
}

}  // namespace tests
}  // namespace cros

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  base::CommandLine::Init(argc, argv);
  base::AtExitManager exit_manager;

  // Needed to enable VLOG through --vmodule.
  logging::LoggingSettings settings;
  settings.logging_dest = logging::LOG_TO_SYSTEM_DEBUG_LOG;
  LOG_ASSERT(logging::InitLogging(settings));

  const base::CommandLine* cmd_line = base::CommandLine::ForCurrentProcess();
  DCHECK(cmd_line);

  const char* jpeg_filename1 = nullptr;
  const char* jpeg_filename2 = nullptr;
  base::CommandLine::SwitchMap switches = cmd_line->GetSwitches();
  for (base::CommandLine::SwitchMap::const_iterator it = switches.begin();
       it != switches.end(); ++it) {
    if (it->first == "jpeg_filename1") {
      jpeg_filename1 = it->second.c_str();
      continue;
    }
    if (it->first == "jpeg_filename2") {
      jpeg_filename2 = it->second.c_str();
      continue;
    }
    if (it->first == "v" || it->first == "vmodule")
      continue;
    if (it->first == "h" || it->first == "help")
      continue;
    LOG(ERROR) << "Unexpected switch: " << it->first << ":" << it->second;
    return -EINVAL;
  }

  cros::tests::g_env =
      reinterpret_cast<cros::tests::JpegDecodeTestEnvironment*>(
          testing::AddGlobalTestEnvironment(
              new cros::tests::JpegDecodeTestEnvironment(jpeg_filename1,
                                                         jpeg_filename2)));

  return RUN_ALL_TESTS();
}
