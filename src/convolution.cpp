/*******************************************************************************
 *
 * MIT License
 *
 * Copyright (c) 2017 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *******************************************************************************/
#include <miopen/convolution.hpp>
#include <miopen/solver.hpp>
#include <miopen/env.hpp>
#include <miopen/errors.hpp>

MIOPEN_DECLARE_ENV_VAR(MIOPEN_DEBUG_CONV_DIRECT)

namespace miopen {

ConvolutionDescriptor::ConvolutionDescriptor(
    int p_pad_h, int p_pad_w, int p_u, int p_v, int p_dilation_h, int p_dilation_w)
    : mode(miopenConvolution),
      paddingMode(miopenPaddingDefault),
      pad_h(p_pad_h),
      pad_w(p_pad_w),
      u(p_u),
      v(p_v),
      dilation_h(p_dilation_h),
      dilation_w(p_dilation_w)
{
    if(pad_h < 0 || pad_w < 0 || u <= 0 || v <= 0 || dilation_h <= 0 || dilation_w <= 0 ||
       (dilation_h != dilation_w))
    {
        MIOPEN_THROW(miopenStatusBadParm,
                     "Invalid parameters, check usage. MIOPEN expects padding "
                     ">= 0, stride >= 1, dilation >= 1 and the same dilation "
                     "factor for horizontal and vertical direction");
    }
}

ConvolutionDescriptor::ConvolutionDescriptor(miopenConvolutionMode_t c_mode,
                                             miopenPaddingMode_t p_mode,
                                             int p_pad_h,
                                             int p_pad_w,
                                             int p_u,
                                             int p_v,
                                             int p_dilation_h,
                                             int p_dilation_w)
    : mode(c_mode),
      paddingMode(p_mode),
      pad_h(p_pad_h),
      pad_w(p_pad_w),
      u(p_u),
      v(p_v),
      dilation_h(p_dilation_h),
      dilation_w(p_dilation_w)
{
    if(pad_h < 0 || pad_w < 0 || u <= 0 || v <= 0 || dilation_h <= 0 || dilation_w <= 0 ||
       (dilation_h != dilation_w))
    {
        MIOPEN_THROW(miopenStatusBadParm,
                     "Invalid parameters, check usage. MIOPEN expects padding "
                     ">= 0, stride >= 1, dilation >= 1 and the same dilation "
                     "factor for horizontal and vertical direction");
    }
    if(!(mode == miopenConvolution || mode == miopenTranspose))
    {
        MIOPEN_THROW(miopenStatusBadParm, "Convolution mode not supported");
    }
    if(!(paddingMode == miopenPaddingSame || paddingMode == miopenPaddingValid ||
         paddingMode == miopenPaddingDefault))
    {
        MIOPEN_THROW(miopenStatusBadParm, "Padding mode not supported");
    }
}

std::tuple<std::size_t, std::size_t, std::size_t, std::size_t>
ConvolutionDescriptor::GetForwardOutputDim(const TensorDescriptor& inputTensorDesc,
                                           const TensorDescriptor& filterDesc) const
{
    assert(inputTensorDesc.GetLengths().size() == 4);
    assert(filterDesc.GetLengths().size() == 4);

    if(inputTensorDesc.GetType() != filterDesc.GetType())
    {
        MIOPEN_THROW(miopenStatusBadParm, "Types do not match for the filter");
    }

    // We use signed integers here to avoid possible underflows
    std::ptrdiff_t input_n;
    std::ptrdiff_t input_c;
    std::ptrdiff_t input_h;
    std::ptrdiff_t input_w;

    std::tie(input_n, input_c, input_h, input_w) = miopen::tien<4>(inputTensorDesc.GetLengths());

    std::ptrdiff_t filter_k;
    std::ptrdiff_t filter_c;
    std::ptrdiff_t filter_h;
    std::ptrdiff_t filter_w;

    std::tie(filter_k, filter_c, filter_h, filter_w) = miopen::tien<4>(filterDesc.GetLengths());

    if(mode == miopenConvolution)
    {
        if(input_c != filter_c)
        {
            MIOPEN_THROW(miopenStatusBadParm, "Channels do not match for the filter");
        }
    }
    else if(mode == miopenTranspose)
    {
        if(input_c != filter_k)
        {
            MIOPEN_THROW(miopenStatusBadParm, "Channels do not match for the filter");
        }
    }

    std::ptrdiff_t output_c;
    std::ptrdiff_t output_h;
    std::ptrdiff_t output_w;

    if(paddingMode == miopenPaddingDefault)
    {
        if(mode == miopenTranspose)
        {
            output_c = filter_c;
            output_h = std::max<std::ptrdiff_t>(
                1, u * (input_h - 1) + 1 + dilation_h * (filter_h - 1.0) - 2 * pad_h);
            output_w = std::max<std::ptrdiff_t>(
                1, v * (input_w - 1) + 1 + dilation_w * (filter_w - 1.0) - 2 * pad_w);
        }
        else if(mode == miopenConvolution)
        {
            output_c = filter_k;
            output_h = std::max<std::ptrdiff_t>(
                1, (input_h - (1 + dilation_h * (filter_h - 1)) + 2 * pad_h) / u + 1);
            output_w = std::max<std::ptrdiff_t>(
                1, (input_w - (1 + dilation_w * (filter_w - 1)) + 2 * pad_w) / v + 1);
        }
    }
    else if(paddingMode == miopenPaddingSame)
    {
        output_c = filter_k;
        output_h = std::ceil(static_cast<double>(input_h) / u);
        output_w = std::ceil(static_cast<double>(input_w) / v);
    }
    else if(paddingMode == miopenPaddingValid)
    {
        output_c = filter_k;
        output_h = std::ceil(static_cast<double>(input_h - filter_h + 1) / u);
        output_w = std::ceil(static_cast<double>(input_w - filter_w + 1) / v);
    }
    else
        MIOPEN_THROW(miopenStatusInvalidValue, "Invalid Padding Mode!");

    return std::make_tuple(input_n, output_c, output_h, output_w);

    //	return std::make_tuple(
    //		input_n,
    //		filter_k,
    //		std::max(1, (input_h - filter_h + 2*pad_h) / u + 1),
    //		std::max(1, (input_w - filter_w + 2*pad_w) / v + 1)
    //	);
}

size_t ConvolutionDescriptor::ForwardGetWorkSpaceSizeGEMM(Handle& handle,
                                                          const TensorDescriptor& wDesc,
                                                          const TensorDescriptor& yDesc) const
{
    int out_h, out_w;
    std::tie(std::ignore, std::ignore, out_h, out_w) = miopen::tien<4>(yDesc.GetLengths());

    int wei_c, wei_h, wei_w;
    std::tie(std::ignore, wei_c, wei_h, wei_w) = miopen::tien<4>(wDesc.GetLengths());

    size_t workspace_size = wei_c * wei_h * wei_w * out_h * out_w * GetTypeSize(yDesc.GetType());

    // No workspace is needed for 1x1_stride=1 convolutions
    if(wei_h == 1 && wei_w == 1 && u == 1 && v == 1 && pad_h == 0 && pad_w == 0)
    {
        return 0;
    }

    // gfx803 devices have 4gb-6gb memory
    if(workspace_size > (1 << 30) && handle.GetDeviceName() == "gfx803")
    {
        workspace_size = 0;
    }

    return workspace_size;
}

size_t
ConvolutionDescriptor::ForwardGetWorkSpaceSizeGEMMTranspose(const TensorDescriptor& xDesc,
                                                            const TensorDescriptor& yDesc) const
{

    int in_n, in_c;
    std::tie(in_n, in_c, std::ignore, std::ignore) = tien<4>(xDesc.GetLengths());

    int out_h, out_w;
    std::tie(std::ignore, std::ignore, out_h, out_w) = tien<4>(yDesc.GetLengths());

    size_t x_t_size = in_n * in_c * out_h * out_w * GetTypeSize(xDesc.GetType());

    size_t y_t_size = yDesc.GetElementSize() * GetTypeSize(yDesc.GetType());

    return x_t_size + y_t_size;
}

// FIXME: This seems to duplicate
// mlo_construct_direct2D::mloIsCorrectBinaryWinograd3x3U()
// functionality thus violating the One Definition Rule.
bool ConvolutionDescriptor::IsWinograd3x3Supported(Handle& handle,
                                                   bool direction,
                                                   const TensorDescriptor& wDesc,
                                                   const TensorDescriptor& xDesc) const
{
    if(miopen::IsDisabled(MIOPEN_DEBUG_AMD_ROCM_PRECOMPILED_BINARIES{}))
    {
        // Support for MIOPEN_DEBUG_AMD_ASM_KERNELS_PERF_FILTERING is not copypasted here.
        // Right now this does not matter as there is none perf filtering for Winograd
        return false;
    }

    const auto device_name       = handle.GetDeviceName();
    const auto max_compute_units = handle.GetMaxComputeUnits();

    int _batch_sz, _n_inputs, _in_height, _in_width;
    int _n_outputs, _kernel_size0, _kernel_size1;
    int _n_outputs_w, _n_inputs_w;

    // Assumed rocm_meta_version::AMDHSA_1_0 or newer.
    if(!(device_name == "gfx803" || device_name == "gfx900" || device_name == "gfx906"))
    {
        return false;
    }
    const auto device_is_gfx8 = (device_name.find("gfx8") != std::string::npos);

    std::tie(_batch_sz, _n_inputs, _in_height, _in_width)             = tien<4>(xDesc.GetLengths());
    std::tie(_n_outputs_w, _n_inputs_w, _kernel_size0, _kernel_size1) = tien<4>(wDesc.GetLengths());

    _n_outputs = direction ? _n_outputs_w : _n_inputs_w;
    return pad_h == 1 && pad_w == 1 && _kernel_size0 == 3 && _kernel_size1 == 3 && u == 1 &&
           v == 1 && _batch_sz < std::pow(2, 16) && _n_inputs < std::pow(2, 16) &&
           _n_outputs < std::pow(2, 16) && _in_height < std::pow(2, 16) &&
           _in_width < std::pow(2, 16) && max_compute_units < std::pow(2, 16) &&
           (_n_inputs * _in_height * _in_width) <= std::pow(2, 28) &&
           (_n_outputs * _in_height * _in_width) <= std::pow(2, 28) &&
           (_n_inputs * _kernel_size0 * _kernel_size1) <= std::pow(2, 28) &&
           (_n_outputs * _kernel_size0 * _kernel_size1) <= std::pow(2, 28) && _n_inputs % 2 == 0 &&
           _n_inputs >= (device_is_gfx8 ? 16 : 18) && (GetTypeSize(wDesc.GetType()) == 4) &&
           (GetTypeSize(xDesc.GetType()) == 4);
}

bool ConvolutionDescriptor::IsBwdWeightsDirectSupported(const TensorDescriptor& wDesc) const
{
    int k, c, _kernel_size0, _kernel_size1;
    std::tie(k, c, _kernel_size0, _kernel_size1) = tien<4>(wDesc.GetLengths());

    bool supported_filters =
        ((_kernel_size0 == 1 && _kernel_size1 == 1) || (_kernel_size0 == 3 && _kernel_size1 == 3) ||
         (_kernel_size0 == 5 && _kernel_size1 == 5) || (_kernel_size0 == 7 && _kernel_size1 == 7) ||
         (_kernel_size0 == 9 && _kernel_size1 == 9) ||
         (_kernel_size0 == 11 && _kernel_size1 == 11) ||
         (_kernel_size0 == 5 && _kernel_size1 == 10 && u == 2 && v == 2 && pad_h == 0 &&
          pad_w == 0) ||
         (_kernel_size0 == 5 && _kernel_size1 == 20 && u == 2 && v == 2 && pad_h == 0 &&
          pad_w == 0));

    bool workarounds =
        //        (((_kernel_size0 == 1 && _kernel_size1 == 1 && ((c & 0xF) > 0 || (k & 0xF) > 0)))
        //        ||
        ((_kernel_size0 == 1 && _kernel_size1 == 1 && (u > 2 || v > 2)) ||
         (_kernel_size0 == 3 && _kernel_size1 == 3 && (u > 2 || v > 2)) ||
         (_kernel_size0 % 2 == 0 && _kernel_size1 % 2 == 0));

    return (supported_filters && !workarounds);
}

bool ConvolutionDescriptor::IsDirectSupported(const TensorDescriptor& wDesc) const
{

    int k, c, _kernel_size0, _kernel_size1;
    std::tie(k, c, _kernel_size0, _kernel_size1) = tien<4>(wDesc.GetLengths());

    bool supported_filters =
        ((_kernel_size0 == 1 && _kernel_size1 == 1) || (_kernel_size0 == 3 && _kernel_size1 == 3) ||
         (_kernel_size0 == 5 && _kernel_size1 == 5) || (_kernel_size0 == 7 && _kernel_size1 == 7) ||
         (_kernel_size0 == 9 && _kernel_size1 == 9) ||
         (_kernel_size0 == 11 && _kernel_size1 == 11) ||
         (_kernel_size0 == 5 && _kernel_size1 == 10 && u == 2 && v == 2 && pad_h == 0 &&
          pad_w == 0) ||
         (_kernel_size0 == 5 && _kernel_size1 == 20 && u == 2 && v == 2 && pad_h == 0 &&
          pad_w == 0));

    bool workarounds = ((_kernel_size0 == 3 && _kernel_size1 == 3 && (u > 2 || v > 2)) ||
                        (_kernel_size0 == 1 && _kernel_size1 == 1 && (pad_h > 0 || pad_w > 0)) ||
                        (_kernel_size0 % 2 == 0 && _kernel_size1 % 2 == 0));

    return (supported_filters && !workarounds);
}

size_t ConvolutionDescriptor::ForwardGetWorkSpaceSize(Handle& handle,
                                                      const TensorDescriptor& wDesc,
                                                      const TensorDescriptor& xDesc,
                                                      const TensorDescriptor& yDesc) const
{
    MIOPEN_LOG_I2("");
    mlo_construct_direct2D find_params(1); // forward
    size_t wsize = 0;
    find_params.setOutputDescFromMLDesc(yDesc);
    find_params.setInputDescFromMLDesc(xDesc);
    find_params.setWeightDescFromMLDesc(wDesc);
    std::string find_config;
    find_params.mloBuildConf_Key(find_config);

    std::unordered_map<std::string, size_t>::iterator iter = handle.fwd_size_map.find(find_config);
    if (iter != handle.fwd_size_map.end())
    {
        return iter->second;
    }


    if(mode == miopenTranspose)
        wsize = BackwardDataGetWorkSpaceSizeGEMM(handle, wDesc, xDesc);
    else
    {
        int wei_h, wei_w;
        std::tie(std::ignore, std::ignore, wei_h, wei_w) = tien<4>(wDesc.GetLengths());
        int in_h, in_w;
        std::tie(std::ignore, std::ignore, in_h, in_w) = tien<4>(xDesc.GetLengths());

        const size_t direct_workspace =
            ForwardBackwardDataGetWorkSpaceSizeDirect(handle, xDesc, yDesc, wDesc, 1);

        if(dilation_w > 1 || dilation_h > 1)
            wsize = std::max(ForwardGetWorkSpaceSizeGEMM(handle, wDesc, yDesc), direct_workspace);

        // Use transpose path if input ht and width <= 14 for 1x1_stride=1 convolutions OR for
        // 1x1_stride=2
        else if((wei_h == 1 && wei_w == 1 && pad_h == 0 && pad_w == 0 && dilation_h == 1 &&
            dilation_w == 1) &&
           ((in_h <= 14 && in_w <= 14 && u == 1 && v == 1) || (u == 2 && v == 2)))
        {
            wsize = std::max(ForwardGetWorkSpaceSizeGEMMTranspose(xDesc, yDesc), direct_workspace);
        }

        // Check if Winograd is available
        // If Winograd is present, there is no advantage in letting
        // the user run another algorithm as those both slower and
        // use more workspace.
        else if(IsWinograd3x3Supported(handle, true, wDesc, xDesc))
        {
            wsize = 0;
        }
        else
        {
            size_t workspace_size_gemm = ForwardGetWorkSpaceSizeGEMM(handle, wDesc, yDesc);
            size_t workspace_size_fft  = ForwardGetWorkSpaceSizeFFT(wDesc, xDesc, yDesc);

            wsize = std::max(std::max(workspace_size_fft, workspace_size_gemm), direct_workspace);
        }
    }
    handle.fwd_size_map.insert(std::unordered_map<std::string, size_t>::value_type(find_config, wsize));
    return wsize;
}

size_t ConvolutionDescriptor::BackwardDataGetWorkSpaceSize(Handle& handle,
                                                           const TensorDescriptor& wDesc,
                                                           const TensorDescriptor& dyDesc,
                                                           const TensorDescriptor& dxDesc) const
{
    MIOPEN_LOG_I2("");
    size_t wsize = 0;
    mlo_construct_direct2D find_params(0); // forward
    find_params.setOutputDescFromMLDesc(dyDesc);
    find_params.setInputDescFromMLDesc(dxDesc);
    find_params.setWeightDescFromMLDesc(wDesc);
    find_params.setConvDescr(pad_h, pad_w, u, v, dilation_h, dilation_w);
    std::string find_config;
    find_params.mloBuildConf_Key(find_config);
    std::unordered_map<std::string, size_t>::iterator iter = handle.bwdData_size_map.find(find_config);
    if (iter != handle.bwdData_size_map.end())
    {
	    return iter->second;
    }

    if(mode == miopenTranspose)
        wsize = ForwardGetWorkSpaceSizeGEMM(handle, wDesc, dxDesc);
    else
    {
        int wei_h, wei_w;
        std::tie(std::ignore, std::ignore, wei_h, wei_w) = tien<4>(wDesc.GetLengths());

        const size_t direct_workspace =
            ForwardBackwardDataGetWorkSpaceSizeDirect(handle, dxDesc, dyDesc, wDesc, 0);

        if(dilation_w > 1 || dilation_h > 1)
            wsize = std::max(BackwardDataGetWorkSpaceSizeGEMM(handle, wDesc, dyDesc),
                            direct_workspace);

        else if(wei_h == 1 && wei_w == 1 && pad_h == 0 && pad_w == 0 && (u == 2 && v == 2) &&
           dilation_w == 1 && dilation_h == 1)
        {
            size_t gemm_trans = BackwardDataGetWorkSpaceSizeGEMMTranspose(dyDesc, dxDesc);
            wsize = std::max(gemm_trans, direct_workspace);
        }
        // Check if Winograd is available
        // If Winograd is present, there is no advantage in letting
        // the user run another algorithm as those both slower and
        // use more workspace.
        else if(IsWinograd3x3Supported(handle, false, wDesc, dyDesc))
        {
            wsize = 0;
        }
        else
        {
            size_t workspace_size_gemm = BackwardDataGetWorkSpaceSizeGEMM(handle, wDesc, dyDesc);
            size_t workspace_size_fft  = BackwardGetWorkSpaceSizeFFT(wDesc, dyDesc, dxDesc);

            wsize = std::max(std::max(workspace_size_fft, workspace_size_gemm), direct_workspace);
        }
    }
    handle.bwdData_size_map.insert(std::unordered_map<std::string, size_t>::value_type(find_config, wsize));
    return wsize;
}

// weights_n = output_c
// weights_c = input_c
// weights_h = 2*pad_h + input_h - u*(output_h - 1)
// weights_w = 2*pad_w + input_w - v*(output_w - 1)
std::tuple<std::size_t, std::size_t, std::size_t, std::size_t>
ConvolutionDescriptor::GetBackwardsWeightsDim(const TensorDescriptor& inputTensorDesc,
                                              const TensorDescriptor& outputTensorDesc) const
{
    assert(inputTensorDesc.GetLengths().size() == 4);
    assert(outputTensorDesc.GetLengths().size() == 4);

    if(inputTensorDesc.GetType() != outputTensorDesc.GetType())
    {
        MIOPEN_THROW(miopenStatusBadParm, "Types do not match for the filter");
    }

    std::size_t input_n;
    std::size_t input_c;
    std::size_t input_h;
    std::size_t input_w;

    std::tie(input_n, input_c, input_h, input_w) = miopen::tien<4>(inputTensorDesc.GetLengths());

    std::size_t output_n;
    std::size_t output_c;
    std::size_t output_h;
    std::size_t output_w;

    std::tie(output_n, output_c, output_h, output_w) =
        miopen::tien<4>(outputTensorDesc.GetLengths());

    // if(input_c != filter_c) {
    // 	MIOPEN_THROW(miopenStatusBadParm, "Channels do not match for the filter");
    // }

    return std::make_tuple(output_c,
                           input_c,
                           2 * pad_h + input_h - u * (output_h - 1),
                           2 * pad_w + input_w - v * (output_w - 1));
}

std::tuple<std::size_t, std::size_t, std::size_t, std::size_t>
ConvolutionDescriptor::GetBackwardOutputDim(const TensorDescriptor& outputTensorDesc,
                                            const TensorDescriptor& filterDesc) const
{
    assert(outputTensorDesc.GetLengths().size() == 4);
    assert(filterDesc.GetLengths().size() == 4);

    if(outputTensorDesc.GetType() != filterDesc.GetType())
    {
        MIOPEN_THROW(miopenStatusBadParm, "Types do not match for the filter");
    }

    std::size_t output_n;
    std::size_t output_c;
    std::size_t output_h;
    std::size_t output_w;

    std::tie(output_n, output_c, output_h, output_w) =
        miopen::tien<4>(outputTensorDesc.GetLengths());

    std::size_t filter_k;
    std::size_t filter_c;
    std::size_t filter_h;
    std::size_t filter_w;

    std::tie(filter_k, filter_c, filter_h, filter_w) = miopen::tien<4>(filterDesc.GetLengths());

    if(output_c != filter_k)
    {
        MIOPEN_THROW(miopenStatusBadParm, "Channels do not match for the filter");
    }

    return std::make_tuple(output_n,
                           filter_c,
                           u * (output_h - 1) - 2 * pad_h + filter_h,
                           v * (output_w - 1) - 2 * pad_w + filter_w);
}

TensorDescriptor
ConvolutionDescriptor::GetForwardOutputTensor(const TensorDescriptor& inputTensorDesc,
                                              const TensorDescriptor& filterDesc) const
{
    auto dims = this->GetForwardOutputDim(inputTensorDesc, filterDesc);
    return TensorDescriptor(
        inputTensorDesc.GetType(),
        {std::get<0>(dims), std::get<1>(dims), std::get<2>(dims), std::get<3>(dims)});
}

TensorDescriptor
ConvolutionDescriptor::GetBackwardOutputTensor(const TensorDescriptor& outputTensorDesc,
                                               const TensorDescriptor& filterDesc) const
{
    auto dims = this->GetBackwardOutputDim(outputTensorDesc, filterDesc);
    return TensorDescriptor(
        outputTensorDesc.GetType(),
        {std::get<0>(dims), std::get<1>(dims), std::get<2>(dims), std::get<3>(dims)});
}

TensorDescriptor
ConvolutionDescriptor::GetBackwardWeightsTensor(const TensorDescriptor& inputTensorDesc,
                                                const TensorDescriptor& outputTensorDesc) const
{
    auto dims = this->GetBackwardsWeightsDim(inputTensorDesc, outputTensorDesc);
    return TensorDescriptor(
        outputTensorDesc.GetType(),
        {std::get<0>(dims), std::get<1>(dims), std::get<2>(dims), std::get<3>(dims)});
}

size_t ConvolutionDescriptor::BackwardDataGetWorkSpaceSizeGEMM(Handle& handle,
                                                               const TensorDescriptor& wDesc,
                                                               const TensorDescriptor& dyDesc) const
{
    int out_h, out_w;
    std::tie(std::ignore, std::ignore, out_h, out_w) = miopen::tien<4>(dyDesc.GetLengths());
    int wei_c, wei_h, wei_w;
    std::tie(std::ignore, wei_c, wei_h, wei_w) = miopen::tien<4>(wDesc.GetLengths());
    size_t gemm_size = wei_c * wei_h * wei_w * out_h * out_w * GetTypeSize(dyDesc.GetType());

    // No workspace is needed for 1x1_stride=1 convolutions
    if(wei_h == 1 && wei_w == 1 && u == 1 && v == 1 && pad_h == 0 && pad_w == 0)
    {
        return 0;
    }

    // gfx803 devices have limited memory
    // TODO: be graceful, need to ensure we can execute a config on the GPU
    // what if both the algos require > (1 << 30) memory
    if(handle.GetDeviceName() == "gfx803" && gemm_size > (1 << 30))
        gemm_size = 0;

    return gemm_size;
}

size_t ConvolutionDescriptor::BackwardDataGetWorkSpaceSizeGEMMTranspose(
    const TensorDescriptor& dyDesc, const TensorDescriptor& dxDesc) const
{
    int in_n, in_c;
    std::tie(in_n, in_c, std::ignore, std::ignore) = tien<4>(dxDesc.GetLengths());

    int out_h, out_w;
    std::tie(std::ignore, std::ignore, out_h, out_w) = tien<4>(dyDesc.GetLengths());

    size_t dx_t_size = in_n * in_c * out_h * out_w * GetTypeSize(dxDesc.GetType());

    size_t dy_t_size = dyDesc.GetElementSize() * GetTypeSize(dyDesc.GetType());

    return dx_t_size + dy_t_size;
}

size_t ConvolutionDescriptor::BackwardWeightsGetWorkSpaceSizeGEMM(
    Handle& handle, const TensorDescriptor& dyDesc, const TensorDescriptor& dwDesc) const
{
    int out_h, out_w;
    std::tie(std::ignore, std::ignore, out_h, out_w) = miopen::tien<4>(dyDesc.GetLengths());
    int wei_c, wei_h, wei_w;
    std::tie(std::ignore, wei_c, wei_h, wei_w) = miopen::tien<4>(dwDesc.GetLengths());
    size_t gemm_size = wei_c * wei_h * wei_w * out_h * out_w * GetTypeSize(dyDesc.GetType());

    // gfx803 devices have limited memory
    // TODO: be graceful, need to ensure we can execute a config on the GPU
    // what if both the algos require > (1 << 30) memory
    if(handle.GetDeviceName() == "gfx803" && gemm_size > (1 << 30))
        gemm_size = 0;

    return gemm_size;
}

size_t ConvolutionDescriptor::ForwardBackwardDataGetWorkSpaceSizeDirect(
    Handle& handle,
    const TensorDescriptor& xDesc,
    const TensorDescriptor& yDesc,
    const TensorDescriptor& wDesc,
    int direction) const // 1: Forward, 0: BackwardData
{

    if(!IsDirectSupported(wDesc) || miopen::IsDisabled(MIOPEN_DEBUG_CONV_DIRECT{}))
        return 0;

    mlo_construct_direct2D construct_params(direction);
    construct_params.setDoSearch(false);
    construct_params.setStream(&handle);
    construct_params.setOutputDescFromMLDesc(yDesc);
    construct_params.setInputDescFromMLDesc(xDesc);
    construct_params.setWeightDescFromMLDesc(wDesc);
    construct_params.setConvDescr(pad_h, pad_w, u, v, dilation_h, dilation_w);
    construct_params.setWorkaroundDisableSearchEnforce(true);

    try
    {
        const auto ss = FindAllSolutions(construct_params);
        size_t sz     = 0;
        for(const auto& solution : ss)
        {
            if(sz < solution.workspce_sz)
            {
                MIOPEN_LOG_I2(sz << " < " << solution.workspce_sz);
                sz = solution.workspce_sz;
            }
        }
        return sz;
    }
    catch(const miopen::Exception&)
    {
        return 0;
    }
}

size_t
ConvolutionDescriptor::BackwardWeightsGetWorkSpaceSizeDirect(Handle& handle,
                                                             const TensorDescriptor& dyDesc,
                                                             const TensorDescriptor& xDesc,
                                                             const TensorDescriptor& dwDesc) const
{
    mlo_construct_BwdWrW2D construct_params(0); // backward with regards to weights
    construct_params.setDoSearch(false);
    construct_params.setStream(&handle);
    construct_params.setOutputDescFromMLDesc(dyDesc);
    construct_params.setInputDescFromMLDesc(xDesc);
    construct_params.setWeightDescFromMLDesc(dwDesc);
    construct_params.setConvDescr(pad_h, pad_w, u, v, dilation_h, dilation_w);
    construct_params.setWorkaroundDisableSearchEnforce(true);

    try
    {
        const auto ss = FindAllSolutions(construct_params);
        size_t sz     = 0;
        for(const auto& solution : ss)
        {
            if(sz < solution.workspce_sz)
            {
                MIOPEN_LOG_I2(sz << " < " << solution.workspce_sz);
                sz = solution.workspce_sz;
            }
        }
        return sz;
    }
    catch(const miopen::Exception&)
    {
        return 0;
    }
}

size_t ConvolutionDescriptor::ConvolutionBackwardWeightsGetWorkSpaceSize(
    Handle& handle,
    const TensorDescriptor& dyDesc,
    const TensorDescriptor& xDesc,
    const TensorDescriptor& dwDesc) const
{
    MIOPEN_LOG_I2("");
    size_t wsize = 0;
    mlo_construct_direct2D find_params(0); // forward
    find_params.setOutputDescFromMLDesc(dyDesc);
    find_params.setInputDescFromMLDesc(xDesc);
    find_params.setWeightDescFromMLDesc(dwDesc);
    std::string find_config;
    find_params.mloBuildConf_Key(find_config);
    std::unordered_map<std::string, size_t>::iterator iter = handle.bwdWeights_size_map.find(find_config);
    if (iter != handle.bwdWeights_size_map.end())
    {
        return iter->second;
    }

    if(mode == miopenTranspose)
        wsize = BackwardWeightsGetWorkSpaceSizeGEMM(handle, xDesc, dwDesc);
    else
        wsize = std::max(BackwardWeightsGetWorkSpaceSizeDirect(handle, dyDesc, xDesc, dwDesc),
                    BackwardWeightsGetWorkSpaceSizeGEMM(handle, dyDesc, dwDesc));

    handle.bwdWeights_size_map.insert(std::unordered_map<std::string, size_t>::value_type(find_config, wsize));
    return wsize;
}

std::ostream& operator<<(std::ostream& stream, const ConvolutionDescriptor& c)
{
    stream << c.pad_h << ", ";
    stream << c.pad_w << ", ";
    stream << c.u << ", ";
    stream << c.v << ", ";
    stream << c.dilation_h << ", ";
    stream << c.dilation_w << ", ";
    return stream;
}

} // namespace miopen
