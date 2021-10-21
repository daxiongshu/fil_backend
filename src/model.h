/*
 * Copyright (c) 2021, NVIDIA CORPORATION.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <forest_model.h>
#include <names.h>
#include <shared_state.h>
#include <tl_model.h>

#include <filesystem>
#include <optional>
#include <rapids_triton/batch/batch.hpp>        // rapids::Batch
#include <rapids_triton/build_control.hpp>      // rapids::IS_GPU_BUILD
#include <rapids_triton/memory/types.hpp>       // rapids::MemoryType
#include <rapids_triton/model/model.hpp>        // rapids::Model
#include <rapids_triton/tensor/tensor.hpp>      // rapids::Tensor
#include <rapids_triton/triton/deployment.hpp>  // rapids::DeploymentType
#include <rapids_triton/triton/device.hpp>      // rapids::device_id_t

namespace triton {
namespace backend {
namespace NAMESPACE {

struct RapidsModel : rapids::Model<RapidsSharedState> {
  RapidsModel(std::shared_ptr<RapidsSharedState> shared_state,
              rapids::device_id_t device_id, cudaStream_t default_stream,
              rapids::DeploymentType deployment_type,
              std::string const& filepath)
      : rapids::Model<RapidsSharedState>(shared_state, device_id,
                                         default_stream, deployment_type,
                                         filepath) {}

  void predict(rapids::Batch& batch) const {
    /* Get I/O Tensors */
    auto input = get_input<float>(batch, "input__0");
    auto output = get_output<float>(batch, "output__0");
    auto samples = input.shape()[0];

    /* Cache pointer to shared state */
    auto shared_state = get_shared_state();

    /* Create non-owning buffer pointing to same memory provided by I/O
     * tensors. If we determine that it is possible and worthwhile to transfer
     * inputs to device for processing, new buffers will be allocated on-device
     * in place of these. */
    auto input_buffer = rapids::Buffer<float const>(
        input.data(), input.size(), input.mem_type(), input.device(),
        input.stream());
    auto output_buffer = rapids::Buffer<float>(
        output.data(), output.size(), output.mem_type(), output.device(),
        output.stream());

    /* Determine if it is possible and worthwhile to copy data to device
     * before performing inference */
    if constexpr (rapids::IS_GPU_BUILD) {  // Do we support GPU ops at all?
      if (gpu_model.has_value()) {         // Is model loaded on device?
        // Is incoming data on host but large enough to justify copying?
        if (input.mem_type() == rapids::HostMemory &&
            samples > shared_state->transfer_threshold()) {
          // Create new buffer on-device to store input data
          input_buffer = rapids::Buffer<float const>(
              input_buffer, rapids::DeviceMemory, get_device_id());
        }
      }

      // Are input data and output data in different places?
      if (input_buffer.mem_type() != output_buffer.mem_type()) {
        // Create output buffer in correct  location
        output_buffer = rapids::Buffer<float>(
            output.size(), input_buffer.mem_type(), get_device_id(),
            get_stream());
      }
    }

    /* Perform inference */
    if (input_buffer.mem_type() == rapids::DeviceMemory) {
      gpu_model.value().predict(
          output_buffer, input_buffer, samples, shared_state->predict_proba());
    } else {
      cpu_model.predict(
          output_buffer, input_buffer, samples, shared_state->predict_proba());
    }

    /* If the output buffer we used for prediction is in a different place from
     * our original output tensor, copy back to the output tensor's memory */
    if (output_buffer.mem_type() != output.mem_type()) {
      rapids::copy(output.buffer(), output_buffer);
    }

    output.finalize();
  }

 private:
  auto model_file()
  {
    auto path = std::filesystem::path(get_filepath());
    switch (get_shared_state()->model_format()) {
      case SerializationFormat::xgboost:
        path /= "xgboost.model";
        break;
      case SerializationFormat::xgboost_json:
        path /= "xgboost.json";
        break;
      case SerializationFormat::lightgbm:
        path /= "model.txt";
        break;
      case SerializationFormat::treelite:
        path /= "checkpoint.tl";
        break;
    }
    return path;
  }

 public:
  void load()
  {
    auto shared_state = get_shared_state();

    // Cache location (GPU/host) preference for incoming input data
    if constexpr (rapids::IS_GPU_BUILD) {
      if (get_deployment_type() == rapids::GPUDeployment) {
        if (shared_state->transfer_threshold() == 0) {
          // If the transfer threshold is 0, we always want input on device
          preferred_mem_type_ = rapids::DeviceMemory;
        } else {
          // If the transfer threshold is non-zero, we'll take the input
          // however it comes and then transfer it to device if it exceeds the
          // given threshold
          preferred_mem_type_ = std::nullopt;
        }
      } else {
        // If we're deployed on-host, we want data in host memory
        preferred_mem_type_ = rapids::HostMemory;
      }
    } else {
      preferred_mem_type_ = rapids::HostMemory;
    }

    // Load model via Treelite
    auto tl_model = std::make_shared<TreeliteModel>(
        model_file(), shared_state->model_format(),
        shared_state->treelite_params());

    if constexpr (rapids::IS_GPU_BUILD) {
      if (get_deployment_type() == rapids::GPUDeployment) {
        gpu_model.emplace(get_device_id(), get_stream(), tl_model);
      }
    }
    cpu_model = ForestModel<rapids::HostMemory>(tl_model);
  }

  std::optional<rapids::MemoryType> preferred_mem_type(
      rapids::Batch& batch) const
  {
    return preferred_mem_type_;
  }

 private:
  std::optional<rapids::MemoryType> preferred_mem_type_{};
  std::size_t num_classes_{};
  ForestModel<rapids::HostMemory> cpu_model;
  std::optional<ForestModel<rapids::DeviceMemory>> gpu_model{};
};

}  // namespace NAMESPACE
}  // namespace backend
}  // namespace triton