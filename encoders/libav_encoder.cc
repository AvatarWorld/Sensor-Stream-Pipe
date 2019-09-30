//
// Created by amourao on 26-06-2019.
//

#include "libav_encoder.h"
#include "../utils/logger.h"

LibAvEncoder::LibAvEncoder(std::string codec_parameters_file, unsigned int _fps) {
  codec_parameters_ = YAML::LoadFile(codec_parameters_file);
  fps_ = _fps;
  av_register_all();

  ready_ = false;
  codec_params_struct_ = NULL;
  total_frame_counter_ = 0;

  stream_id_ = RandomString(16);
}

LibAvEncoder::LibAvEncoder(YAML::Node &_codec_parameters, unsigned int _fps) {
  codec_parameters_ = _codec_parameters;
  fps_ = _fps;
  av_register_all();

  ready_ = false;
  codec_params_struct_ = NULL;
  total_frame_counter_ = 0;
}

std::vector<unsigned char> LibAvEncoder::CurrentFrameBytes() {
  return std::vector<unsigned char>(
      buffer_packet_.front()->data, buffer_packet_.front()->data + buffer_packet_.front()->size);
}

LibAvEncoder::~LibAvEncoder() {
  av_packet_free(&packet_av_);
  av_frame_free(&frame_av_);
  avcodec_free_context(&av_codec_context_);
  avcodec_parameters_free(&av_codec_parameters_);
  sws_freeContext(sws_context_);
  delete codec_params_struct_;

  while (!buffer_fs_.empty()) {
    FrameStruct *q_element = buffer_fs_.front();
    delete q_element;
    buffer_fs_.pop();
  }

  while (!buffer_packet_.empty()) {
    AVPacket *q_element = buffer_packet_.front();
    av_packet_free(&q_element);
    buffer_fs_.pop();
  }
}

void LibAvEncoder::NextPacket() {
  if (!buffer_fs_.empty()) {
    FrameStruct *f = buffer_fs_.front();
    buffer_fs_.pop();
    f->frame.clear();
    delete f;
  }
  if (!buffer_packet_.empty()) {
    AVPacket *p = buffer_packet_.front();
    buffer_packet_.pop();
    delete[] p->data;
    delete p;
  }
}

void LibAvEncoder::PrepareFrame() {
  FrameStruct *f = buffer_fs_.front();
  if (f->frame_data_type == 2) {
    uint8_t *in_data[1] = {&f->frame[8]};
    int in_linesize[1] = {4 * frame_av_->width};

    sws_scale(sws_context_, (const uint8_t *const *)in_data, in_linesize, 0,
              frame_av_->height, frame_av_->data, frame_av_->linesize);

  } else if (f->frame_data_type == 3) {
    if (frame_av_->format == AV_PIX_FMT_GRAY12LE) {
      int i = 0;
      uint8_t *data = &f->frame[8];
      // memcpy(frame_av_->data[0], data, frame_av_->height * frame_av_->width * 2);

      for (int y = 0; y < frame_av_->height; y++) {
        for (int x = 0; x < frame_av_->width; x++) {

          ushort value = data[i + 1] << 8 | data[i];
          if (value >= MAX_DEPTH_VALUE_12_BITS) {
            frame_av_->data[0][i] = 13;
            frame_av_->data[0][i + 1] = 255;
          } else {
            frame_av_->data[0][i] = data[i];
            frame_av_->data[0][i + 1] = data[i + 1];
          }
          i += 2;
        }
      }

    } else if (frame_av_->format ==
               AV_PIX_FMT_GRAY16BE) { // PNG GRAY16 TO gray12le
      int i = 0;
      uint8_t *data = &f->frame[8];
      for (int y = 0; y < frame_av_->height; y++) {
        for (int x = 0; x < frame_av_->width; x++) {
          frame_av_->data[0][i] = data[i + 1];
          frame_av_->data[0][i + 1] = data[i];
          i += 2;
        }
      }
    } else {
      int i = 0;
      uint8_t *data = &f->frame[8];
      float coeff = (float)MAX_DEPTH_VALUE_8_BITS / MAX_DEPTH_VALUE_12_BITS;
      for (int y = 0; y < frame_av_->height; y++) {
        for (int x = 0; x < frame_av_->width; x++) {
          unsigned int lower = data[i * 2];
          unsigned int upper = data[i * 2 + 1];
          ushort value = upper << 8 | lower;

          frame_av_->data[0][i++] =
              std::min((unsigned int)(value * coeff), (unsigned int)MAX_DEPTH_VALUE_8_BITS - 1);
        }
      }

      memset(&frame_av_->data[1][0], 128, frame_av_->height * frame_av_->width / 4);
      memset(&frame_av_->data[2][0], 128, frame_av_->height * frame_av_->width / 4);
    }

  } else if (f->frame_data_type == 0) {

    AVFrame *frame_av_O = av_frame_alloc();

    std::vector<unsigned char> frameData = f->frame;

    image_decoder_.ImageBufferToAVFrame(f, frame_av_O);

    if (frame_av_O->format == frame_av_->format) {
      av_frame_copy(frame_av_, frame_av_O);

    } else if (frame_av_O->format == AV_PIX_FMT_GRAY16BE &&
               frame_av_->format ==
                   AV_PIX_FMT_GRAY12LE) { // PNG GRAY16 TO gray12le
      int i = 0;
      for (int y = 0; y < frame_av_O->height; y++) {
        for (int x = 0; x < frame_av_O->width; x++) {
          ushort lower = frame_av_O->data[0][y * frame_av_O->linesize[0] + x * 2];
          ushort upper = frame_av_O->data[0][y * frame_av_O->linesize[0] + x * 2 + 1];
          ushort value = lower << 8 | upper;

          // TODO: check what is happening!
          if (value >= MAX_DEPTH_VALUE_12_BITS) {
            frame_av_->data[0][i++] = 255;
            frame_av_->data[0][i++] = 255;
          } else {
            frame_av_->data[0][i++] = upper;
            frame_av_->data[0][i++] = lower;
          }
        }
      }
    } else if (frame_av_O->format == AV_PIX_FMT_GRAY16BE &&
               (frame_av_->format == AV_PIX_FMT_YUV420P ||
                frame_av_->format == AV_PIX_FMT_YUV422P ||
                frame_av_->format == AV_PIX_FMT_YUV444P)) { // PNG GRAY16 TO YUV
      // TODO: remove redundant call
      sws_scale(sws_context_, (const uint8_t *const *)frame_av_O->data,
                frame_av_O->linesize, 0, frame_av_O->height, frame_av_->data,
                frame_av_->linesize);

      int i = 0;
      float coeff = (float)MAX_DEPTH_VALUE_8_BITS / MAX_DEPTH_VALUE_12_BITS;
      for (int y = 0; y < frame_av_O->height; y++) {
        for (int x = 0; x < frame_av_O->width; x++) {
          unsigned int lower = frame_av_O->data[0][y * frame_av_O->linesize[0] + x * 2];
          unsigned int upper = frame_av_O->data[0][y * frame_av_O->linesize[0] + x * 2 + 1];
          ushort value = lower << 8 | upper;

          frame_av_->data[0][i++] = std::min((ushort)(value * coeff), (ushort)255);
        }
      }

      /*
      for (unsigned int y = 0; y < frame_av_->height; y++) {
          for (unsigned int x = 0; x < frame_av_->width; x++) {
              frame_av_->data[1][y * frame_av_->linesize[1] + x] = 128;
              frame_av_->data[2][y * frame_av_->linesize[2] + x] = 128;
          }
      }*/
    } else {
      // YUV to YUV
      sws_scale(sws_context_, (const uint8_t *const *)frame_av_O->data,
                frame_av_O->linesize, 0, frame_av_O->height, frame_av_->data,
                frame_av_->linesize);
    }

    av_frame_free(&frame_av_O);
  }

}

void LibAvEncoder::EncodeA(AVCodecContext *enc_ctx, AVFrame *frame,
                           AVPacket *pkt) {
  int ret;
  /* send the frame to the encoder */
  ret = avcodec_send_frame(enc_ctx, frame);
  if (ret < 0) {
    spdlog::error("Error sending a frame for encoding.");
    exit(1);
  }

  ret = avcodec_receive_packet(enc_ctx, pkt);
  if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
    return;
  else if (ret < 0) {
    spdlog::error("Error during encoding.");
    exit(1);
  }

  AVPacket *new_packet = new AVPacket(*packet_av_);
  buffer_packet_.push(new_packet);
  buffer_packet_.back()->data = reinterpret_cast<uint8_t *>(
      new uint64_t[(packet_av_->size + FF_INPUT_BUFFER_PADDING_SIZE) /
                       sizeof(uint64_t) +
                   1]);
  memcpy(buffer_packet_.back()->data, packet_av_->data, packet_av_->size);
}

void LibAvEncoder::Encode() {

  PrepareFrame();

  frame_av_->pts = total_frame_counter_++;

  EncodeA(av_codec_context_, frame_av_, packet_av_);

  buffer_fs_.front()->timestamps.push_back(CurrentTimeMs());
}

void LibAvEncoder::Init(FrameStruct *fs) {
  int ret;

  spdlog::info("Codec information:\n {}", codec_parameters_);

  av_codec_ = avcodec_find_encoder_by_name(
      codec_parameters_["codec_name"].as<std::string>().c_str());
  av_codec_context_ = avcodec_alloc_context3(av_codec_);
  packet_av_ = av_packet_alloc();
  av_codec_parameters_ = avcodec_parameters_alloc();

  int width = 0, height = 0, pxl_format = 0;

  if (fs->frame_data_type == 0 || fs->frame_data_type == 1) {
    AVFrame *frame_av_O = av_frame_alloc();

    image_decoder_.ImageBufferToAVFrame(fs, frame_av_O);

    width = frame_av_O->width;
    height = frame_av_O->height;
    pxl_format = frame_av_O->format;

    av_frame_free(&frame_av_O);
  } else if (fs->frame_data_type == 2) {
    memcpy(&width, &fs->frame[0], sizeof(int));
    memcpy(&height, &fs->frame[4], sizeof(int));
    pxl_format = AV_PIX_FMT_BGRA;
  } else if (fs->frame_data_type == 3) {
    memcpy(&width, &fs->frame[0], sizeof(int));
    memcpy(&height, &fs->frame[4], sizeof(int));
    pxl_format = AV_PIX_FMT_GRAY16LE;
  }

  av_codec_context_->width = width;
  av_codec_context_->height = height;
  /* frames per second */
  av_codec_context_->time_base = (AVRational){1, (int)fps_};
  av_codec_context_->framerate = (AVRational){(int)fps_, 1};
  av_codec_context_->gop_size = 0;

  av_codec_context_->bit_rate_tolerance = 0;
  av_codec_context_->rc_max_rate = 0;
  av_codec_context_->rc_buffer_size = 0;
  av_codec_context_->max_b_frames = 0;
  av_codec_context_->delay = 0;

  /* emit one intra frame every ten frames
   * check frame pict_type before passing frame
   * to encoder, if frame->pict_type is AV_PICTURE_TYPE_I
   * then gop_size is ignored and the output of encoder
   * will always be I frame irrespective to gop_size
   */

  // TDOO: check delay:
  // https://ffmpeg.org/pipermail/libav-user/2014-December/007672.html
  // 1
  av_codec_context_->bit_rate = codec_parameters_["bit_rate"].as<int>(); // 1

  // fmpeg -h encoder=hevc
  // libx264:                 yuv420p yuvj420p yuv422p yuvj422p yuv444p yuvj444p
  // nv12 nv16 nv21 libx264rgb:              bgr0 bgr24 rgb24 libx265: yuv420p
  // yuv422p yuv444p gbrp yuv420p10le yuv422p10le yuv444p10le gbrp10le
  // yuv420p12le yuv422p12le yuv444p12le gbrp12le gray gray10le gray12le
  // hevc_nvenc (gpu x265):   yuv420p nv12 p010le yuv444p yuv444p16le bgr0 rgb0
  // cuda nv12 p010le nvenc_h264 (gpu x264):   yuv420p nv12 p010le yuv444p
  // yuv444p16le bgr0 rgb0 cuda

  av_codec_context_->pix_fmt =
      av_get_pix_fmt(codec_parameters_["pix_fmt"].as<std::string>().c_str());

  YAML::Node codec_parameters_options = codec_parameters_["options"];

  for (YAML::const_iterator it = codec_parameters_options.begin();
       it != codec_parameters_options.end(); ++it) {
    av_opt_set(av_codec_context_->priv_data,
               it->first.as<std::string>().c_str(),
               it->second.as<std::string>().c_str(), AV_OPT_SEARCH_CHILDREN);
  }

  av_opt_set(av_codec_context_->priv_data, "tune", "delay", 0);
  av_opt_set(av_codec_context_->priv_data, "tune", "zerolatency", 1);
  av_opt_set(av_codec_context_->priv_data, "rcParams", "zeroReorderDelay",
             1);

  av_codec_parameters_->codec_type = AVMEDIA_TYPE_VIDEO;

  av_codec_parameters_->codec_id = av_codec_->id;
  av_codec_parameters_->codec_tag = av_codec_context_->codec_tag;
  av_codec_parameters_->bit_rate = av_codec_context_->bit_rate;
  av_codec_parameters_->bits_per_coded_sample =
      av_codec_context_->bits_per_coded_sample;
  av_codec_parameters_->bits_per_raw_sample =
      av_codec_context_->bits_per_raw_sample;
  av_codec_parameters_->profile = av_codec_context_->level;
  av_codec_parameters_->width = av_codec_context_->width;
  av_codec_parameters_->height = av_codec_context_->height;

  av_codec_parameters_->color_space = av_codec_context_->colorspace;
  av_codec_parameters_->sample_rate = av_codec_context_->sample_rate;

  ret = avcodec_open2(av_codec_context_, av_codec_, NULL);
  if (ret < 0) {
    spdlog::error("Could not open codec: {}", av_err2str(ret));
    exit(1);
  }

  frame_av_ = av_frame_alloc();
  if (!frame_av_) {
    spdlog::error("Could not allocate video frame.");
    exit(1);
  }

  frame_av_->format = av_codec_context_->pix_fmt;
  frame_av_->width = av_codec_context_->width;
  frame_av_->height = av_codec_context_->height;

  frame_av_->pts = 0;
  frame_av_->pkt_dts = 0;

  ret = av_frame_get_buffer(frame_av_, 30);
  if (ret < 0) {
    spdlog::error("Could not allocate the video frame data.");
    exit(1);
  }

  sws_context_ = sws_getContext(
      width, height, (AVPixelFormat)pxl_format, frame_av_->width, frame_av_->height,
      (AVPixelFormat)frame_av_->format, SWS_BILINEAR, NULL, NULL, NULL);
}

CodecParamsStruct *LibAvEncoder::GetCodecParamsStruct() {

  if (codec_params_struct_ == NULL) {

    void *extra_data_pointer = av_codec_parameters_->extradata;
    size_t extra_data_size = av_codec_parameters_->extradata_size;
    size_t data_size = sizeof(*av_codec_parameters_);

    std::vector<unsigned char> data_buffer(data_size);
    std::vector<unsigned char> extra_data_buffer(extra_data_size);

    memcpy(&data_buffer[0], av_codec_parameters_, data_size);
    memcpy(&extra_data_buffer[0], extra_data_pointer, extra_data_size);
    codec_params_struct_ = new CodecParamsStruct(0, data_buffer, extra_data_buffer);
  }

  return codec_params_struct_;
}

FrameStruct *LibAvEncoder::CurrentFrameEncoded() {
  FrameStruct *f = new FrameStruct(*buffer_fs_.front());

  f->message_type = 0;
  f->frame_data_type = 1;
  f->codec_data = *GetCodecParamsStruct();
  f->frame = CurrentFrameBytes();
  f->frame_id = total_frame_counter_;
  f->stream_id = stream_id_;

  return f;
}

FrameStruct *LibAvEncoder::CurrentFrameOriginal() {
  if (buffer_fs_.empty())
    return nullptr;
  return buffer_fs_.front();
}

void LibAvEncoder::AddFrameStruct(FrameStruct *fs) {
  if (!ready_) {
    ready_ = true;
    Init(fs);
  }
  fs->timestamps.push_back(CurrentTimeMs());
  buffer_fs_.push(fs);
  Encode();
}

unsigned int LibAvEncoder::GetFps() { return fps_; }

bool LibAvEncoder::HasNextPacket() { return !buffer_packet_.empty(); }
