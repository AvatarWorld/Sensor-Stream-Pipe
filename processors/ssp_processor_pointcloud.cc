//
// Created by amourao on 26-06-2019.
//

#include <chrono>
#include <iostream>
#include <thread>
#include <unistd.h>

#include <k4a/k4a.h>
#include <opencv2/imgproc.hpp>
#include <zmq.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include "../utils/logger.h"

#include "../clients/ssp_coordinator_types.h"
#include "../readers/network_reader.h"
#include "../utils/kinect_utils.h"

std::mutex mutex_;
std::condition_variable cond_var_;
zmq::context_t context_(1);
bool ready = false;
bool leave = false;
NetworkReader *reader;

struct color_point_t {
  int16_t xyz[3];
  uint8_t rgb[3];
};

void TranformationHelpersWritePointCloud(const k4a_image_t point_cloud_image,
                                         const k4a_image_t color_image,
                                         const char *file_name) {
  std::vector<color_point_t> points;

  int width = k4a_image_get_width_pixels(point_cloud_image);
  int height = k4a_image_get_height_pixels(color_image);

  int16_t *point_cloud_image_data =
      (int16_t *)(void *)k4a_image_get_buffer(point_cloud_image);
  uint8_t *color_image_data = k4a_image_get_buffer(color_image);

  for (int i = 0; i < width * height; i++) {
    color_point_t point;
    point.xyz[0] = point_cloud_image_data[3 * i + 0];
    point.xyz[1] = point_cloud_image_data[3 * i + 1];
    point.xyz[2] = point_cloud_image_data[3 * i + 2];
    if (point.xyz[2] == 0) {
      continue;
    }

    point.rgb[0] = color_image_data[4 * i + 0];
    point.rgb[1] = color_image_data[4 * i + 1];
    point.rgb[2] = color_image_data[4 * i + 2];
    uint8_t alpha = color_image_data[4 * i + 3];

    if (point.rgb[0] == 0 && point.rgb[1] == 0 && point.rgb[2] == 0 &&
        alpha == 0) {
      continue;
    }

    points.push_back(point);
  }

#define PLY_START_HEADER "ply"
#define PLY_END_HEADER "end_header"
#define PLY_ASCII "format ascii 1.0"
#define PLY_ELEMENT_VERTEX "element vertex"

  // save to the ply file
  std::ofstream ofs(file_name); // text mode first
  ofs << PLY_START_HEADER << std::endl;
  ofs << PLY_ASCII << std::endl;
  ofs << PLY_ELEMENT_VERTEX << " " << points.size() << std::endl;
  ofs << "property float x" << std::endl;
  ofs << "property float y" << std::endl;
  ofs << "property float z" << std::endl;
  ofs << "property uchar red" << std::endl;
  ofs << "property uchar green" << std::endl;
  ofs << "property uchar blue" << std::endl;
  ofs << PLY_END_HEADER << std::endl;
  ofs.close();

  std::stringstream ss;
  for (size_t i = 0; i < points.size(); ++i) {
    // image data is BGR
    ss << (float)points[i].xyz[0] << " " << (float)points[i].xyz[1] << " "
       << (float)points[i].xyz[2];
    ss << " " << (float)points[i].rgb[2] << " " << (float)points[i].rgb[1]
       << " " << (float)points[i].rgb[0];
    ss << std::endl;
  }
  std::ofstream ofs_text(file_name, std::ios::out | std::ios::app);
  ofs_text.write(ss.str().c_str(), (std::streamsize)ss.str().length());
}

int worker(bool write_to_disk, std::string &write_pattern) {

  spdlog::set_level(spdlog::level::debug);
  av_log_set_level(AV_LOG_QUIET);

  srand(time(NULL) * getpid());

  zmq::context_t context(1);
  // std::thread processor_thread(processor_communicator, std::ref(context));

  try {

    reader->init();

    k4a::calibration sensor_calibration;
    bool calibration_set = false;
    k4abt::tracker tracker;

    std::unordered_map<std::string, IDecoder *> decoders;

    int i = 0, j = 0;
    while (reader->HasNextFrame()) {
      reader->NextFrame();
      std::vector<FrameStruct> f_list = reader->GetCurrentFrame();
      for (FrameStruct f : f_list) {
        std::string decoder_id = f.stream_id + std::to_string(f.sensor_id);

        if (f.camera_calibration_data.type == 0 && calibration_set == false) {

          const k4a_depth_mode_t d = static_cast<const k4a_depth_mode_t>(
              f.camera_calibration_data.extra_data[0]);
          const k4a_color_resolution_t r =
              static_cast<const k4a_color_resolution_t>(
                  f.camera_calibration_data.extra_data[1]);

          sensor_calibration = k4a::calibration::get_from_raw(
              reinterpret_cast<char *>(&f.camera_calibration_data.data[0]),
              f.camera_calibration_data.data.size(), d, r);
          calibration_set = true;
        }
        if (write_to_disk) {
          if (f_list.size() == 3 && f.frame_type == 0) {
            cv::Mat img;
            bool imgChanged = FrameStructToMat(f, img, decoders);

            std::string j_str = std::to_string(j);

            std::string j_formatted =
                std::string(6 - j_str.length(), '0') + j_str;
            std::string output = write_pattern + j_formatted + ".color.png";
            if (imgChanged) {
              cv::imwrite(output, img);
            }
          }
          if (f_list.size() == 3 && f.frame_type == 1) {
            cv::Mat img;
            bool imgChanged = FrameStructToMat(f, img, decoders);

            std::string j_str = std::to_string(j++);

            std::string j_formatted =
                std::string(6 - j_str.length(), '0') + j_str;

            std::string output = write_pattern + j_formatted + ".depth.png";
            if (imgChanged) {
              cv::imwrite(output, img);
            }
          }
        }
      }

      k4a::transformation transformation =
          k4a_transformation_create(&sensor_calibration);
      k4a::capture sensor_capture = k4a::capture::create();

      try {

        FrameStructToK4A(f_list, sensor_capture, decoders);

        k4a::image depth_image = sensor_capture.get_depth_image();
        k4a::image color_image = sensor_capture.get_color_image();

        k4a::image point_cloud = k4a::image::create(
            K4A_IMAGE_FORMAT_CUSTOM, depth_image.get_width_pixels(),
            depth_image.get_height_pixels(),
            depth_image.get_width_pixels() * 3 * (int)sizeof(uint16_t));

        k4a::image transformed_color = k4a::image::create(
            K4A_IMAGE_FORMAT_COLOR_BGRA32, depth_image.get_width_pixels(),
            depth_image.get_height_pixels(),
            depth_image.get_width_pixels() * 4 * (int)sizeof(uint8_t));

        transformation.color_image_to_depth_camera(depth_image, color_image,
                                                   &transformed_color);

        transformation.depth_image_to_point_cloud(
            depth_image, K4A_CALIBRATION_TYPE_DEPTH, &point_cloud);

        std::string i_str = std::to_string(i++);

        std::string i_formatted = std::string(6 - i_str.length(), '0') + i_str;

        std::string output = write_pattern + i_formatted + ".pointcloud.ply";
        TranformationHelpersWritePointCloud(
            point_cloud.handle(), transformed_color.handle(), output.c_str());

      } catch (std::exception &e) {
        spdlog::error(e.what());
      }
    }
  } catch (std::exception &e) {
    spdlog::error(e.what());
  }

  return 0;
}

int main(int argc, char *argv[]) {
  spdlog::set_level(spdlog::level::debug);

  srand(time(NULL) * getpid());

  av_log_set_level(AV_LOG_QUIET);

  if (argc < 2) {
    std::cerr << "Usage: ssp_client_pointcloud <port> (<dest_folder>) (<log "
                 "level>) (<log file>)"
              << std::endl;
    return 1;
  }
  std::string log_level = "debug";
  std::string log_file = "";
  bool write_to_disk = false;
  std::string write_pattern = "";

  if (argc > 2) {
    write_to_disk = true;
    write_pattern = argv[2];
  }

  if (argc > 3)
    log_level = argv[3];
  if (argc > 4)
    log_file = argv[4];

  int port = std::stoi(argv[1]);

  reader = new NetworkReader(port);

  std::string coor_host = "127.0.0.1";
  int coor_port = 9999;

  std::string coor_host_port = coor_host + ":" + std::to_string(coor_port);

  std::string error_msg;
  int error = 1;

  int SIZE = 256 * 256;
  zmq::message_t in_request(SIZE);
  std::string processor_id = RandomString(16);

  std::string yaml_config_file = argv[1];

  zmq::socket_t coor_socket(context_, ZMQ_REQ);

  std::string connect_msg =
      std::string(1, char(SSP_MESSAGE_CONNECT)) +
      std::string(1, char(SSP_CONNECTION_TYPE_PROCESSOR)) + coor_host + ":" +
      std::to_string(coor_port) + " " + processor_id + " " +
      std::string(1, char(SSP_FRAME_SOURCE_KINECT_DK)) + " " +
      std::string(1, char(SSP_EXCHANGE_DATA_TYPE_VECTOR_POINTCLOUD)) + " ";
  zmq::message_t conn_request(connect_msg.c_str(), connect_msg.size());
  zmq::message_t dummy_request(std::string(1, char(SSP_MESSAGE_DUMMY)).c_str(),
                               1);

  spdlog::info("Connecting to coordinator at " + coor_host_port);

  coor_socket.connect("tcp://" + coor_host_port);

  coor_socket.send(conn_request);
  spdlog::info("Waiting to coordinator");
  coor_socket.recv(&in_request);
  spdlog::info("Coordinator responded");
  coor_socket.send(dummy_request);

  FrameSourceType type;
  std::string metadata;

  spdlog::info("Connected to coordinator " + coor_host_port);

  std::string connect_msg_rsp((char *)in_request.data(), in_request.size());

  spdlog::info("Coordinator answer " + connect_msg_rsp);

  std::thread worker_thread(worker, write_to_disk, std::ref(write_pattern));

  while (!leave) {
    spdlog::info("Waiting for request");
    coor_socket.recv(&in_request);
    std::string msg_rsp((char *)in_request.data(), in_request.size());
    char msg_type = msg_rsp.substr(0, 1).c_str()[0];

    switch (msg_type) {
    case SSP_MESSAGE_CONNECT: {
      char conn_type = msg_rsp.substr(1, 1).c_str()[0];
      std::string data = msg_rsp.substr(3, msg_rsp.size() - 3);
      std::string delimitor = " ";
      std::vector<std::string> sdata = SplitString(data, delimitor);
      std::string host = sdata.at(0);
      std::string id = sdata.at(1);

      reader->AddFilter(id);

      dummy_request =
          zmq::message_t(std::string(1, char(SSP_MESSAGE_DUMMY)).c_str(), 1);
      coor_socket.send(dummy_request);
      break;
    }
    case SSP_MESSAGE_DISCONNECT: {
      std::string dummy_filter = "STOP ";
      reader->ResetFilter();

      dummy_request =
          zmq::message_t(std::string(1, char(SSP_MESSAGE_DUMMY)).c_str(), 1);
      coor_socket.send(dummy_request);
      break;
    }
    default: {
      spdlog::info("Invalid " + std::to_string(msg_type) + " request.");
      dummy_request =
          zmq::message_t(std::string(1, char(SSP_MESSAGE_DUMMY)).c_str(), 1);
      coor_socket.send(dummy_request);
      break;
    }
    }
  }

  worker_thread.join();
  context_.close();

  return 0;
}
