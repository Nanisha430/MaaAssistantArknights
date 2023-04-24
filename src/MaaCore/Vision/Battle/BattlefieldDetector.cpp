#include "BattlefieldDetector.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "Utils/NoWarningCV.h"
#include <onnxruntime/core/session/onnxruntime_cxx_api.h>

#include "Config/Miscellaneous/TilePack.h"
#include "Config/OnnxSessions.h"
#include "Config/TaskData.h"
#include "Utils/Logger.hpp"

MAA_VISION_NS_BEGIN

BattlefieldDetector::ResultOpt BattlefieldDetector::analyze() const
{
    LogTraceFunction;

    Result result { .object_of_interest = m_object_of_interest };

    if (m_object_of_interest.operators) {
        result.operators = operator_analyze();
    }

    return result;
}

std::vector<BattlefieldDetector::OperatorResult> BattlefieldDetector::operator_analyze() const
{
    const double x_scale = 640.0 / m_image.cols;
    const double y_scale = 640.0 / m_image.rows;

    cv::Mat image;
    cv::resize(m_image, image, cv::Size(), x_scale, y_scale, cv::INTER_AREA);
    std::vector<float> input = image_to_tensor(image);

    auto memory_info = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);
    constexpr int64_t batch_size = 1;
    std::array<int64_t, 4> input_shape { batch_size, image.channels(), image.cols, image.rows };

    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(memory_info, input.data(), input.size(),
                                                              input_shape.data(), input_shape.size());

    auto& session = OnnxSessions::get_instance().get("operators_det");

    Ort::AllocatorWithDefaultOptions allocator;
    std::vector input_names = { session.GetInputNameAllocated(0, allocator).get() };
    std::vector output_names = { session.GetOutputNameAllocated(0, allocator).get() };

    Ort::RunOptions run_options;
    auto output_tensors = session.Run(run_options, input_names.data(), &input_tensor, input_names.size(),
                                      output_names.data(), output_names.size());

    const float* raw_output = output_tensors[0].GetTensorData<float>();
    // output_shape is { 1, 5, 8400 }
    std::vector<int64_t> output_shape = output_tensors[0].GetTensorTypeAndShapeInfo().GetShape();

    // yolov8 的 onnx 输出和前面的 v5, v7 等似乎不太一样，目前网上 yolov8 的 demo 较少，文档也没找到
    // 这里的输出解析是我跟着数据推测的：
    // center_x0, center_x1, ..... center_x8399
    // center_y0, center_y1, ..... center_y8399
    // w0, w1, ..... w8399
    // h0, h1, ..... h8399
    // conf0, conf1, ..... conf8399
    // 如果后面要做多分类，可能得再看下怎么改（我也不知道shape会变成啥样）
    std::vector<std::vector<float>> output(output_shape[1]);
    for (int64_t i = 0; i < output_shape[1]; i++) {
        output[i] = std::vector<float>(raw_output + i * output_shape[2], raw_output + (i + 1) * output_shape[2]);
    }

#ifdef ASST_DEBUG

    const int draw_offset_y = static_cast<int>(m_image.rows * -0.15);
    const int draw_offset_h = static_cast<int>(m_image.rows * 0.13);

#endif

    std::vector<OperatorResult> all_results;
    const auto& conf_vec = output.back();
    for (size_t i = 0; i < conf_vec.size(); ++i) {
        float score = conf_vec[i];
        constexpr float Threshold = 0.3f;
        if (score < Threshold) {
            continue;
        }

        int center_x = static_cast<int>(output[0][i] / x_scale);
        int center_y = static_cast<int>(output[1][i] / y_scale);
        int w = static_cast<int>(output[2][i] / x_scale);
        int h = static_cast<int>(output[3][i] / y_scale);

        int x = center_x - w / 2;
        int y = center_y - h / 2;
        Rect rect { x, y, w, h };

        OperatorResult box { OperatorResult::Cls::Operator, rect, score };

#ifdef ASST_DEBUG

        Rect& draw_rect = box.draw_rect;
        draw_rect.y += draw_offset_y;
        draw_rect.height += draw_offset_h;
        cv::rectangle(m_image_draw, make_rect<cv::Rect>(draw_rect), cv::Scalar(0, 0, 255), 5);
        cv::putText(m_image_draw, std::to_string(box.score), cv::Point(draw_rect.x, draw_rect.y - 10),
                    cv::FONT_HERSHEY_PLAIN, 1.2, cv::Scalar(0, 0, 255), 2);
#endif

        all_results.emplace_back(std::move(box));
    }

    // NMS
    constexpr double NmsThreshold = 0.7f;
    std::sort(all_results.begin(), all_results.end(),
              [](const OperatorResult& a, const OperatorResult& b) { return a.score > b.score; });

    std::vector<OperatorResult> nms_results;
    for (size_t i = 0; i < all_results.size(); ++i) {
        const auto& box = all_results[i];
        if (box.score < 0.1f) {
            continue;
        }
        nms_results.emplace_back(box);
        for (size_t j = i + 1; j < all_results.size(); ++j) {
            auto& box2 = all_results[j];
            if (box2.score < 0.1f) {
                continue;
            }
            int iou_area = (make_rect<cv::Rect>(box.rect) & make_rect<cv::Rect>(box2.rect)).area();
            if (iou_area > NmsThreshold * box2.rect.area()) {
                box2.score = 0;
            }
        }
    }

    return nms_results;
}

MAA_VISION_NS_END