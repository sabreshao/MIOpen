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
#include <miopen/config.h>
#include <miopen/convolution.hpp>
#include <miopen/db.hpp>
#include <miopen/env.hpp>
#include <miopen/util.hpp>
#include <miopen/solver.hpp>
#include <miopen/float_equal.hpp>
#include <miopen/visit_float.hpp>
#include <miopen/check_numerics.hpp>

#if MIOPEN_USE_MIOPENGEMM
#include <miopen/gemm.hpp>
#endif

namespace miopen {

MIOPEN_DECLARE_ENV_VAR(MIOPEN_DEBUG_CONV_DIRECT)

struct AutoEnableProfiling
{
    AutoEnableProfiling(Handle& x) : h(x)
    {
        prev_state = h.IsProfilingEnabled();
        h.EnableProfiling();
    }

    ~AutoEnableProfiling()
    {
        h.EnableProfiling(prev_state);
        h.ResetKernelTime();
    }

    private:
    Handle& h;
    bool prev_state;
};

int ConvolutionDescriptor::FindWinogradKernel(Handle& handle,
                                              const TensorDescriptor& xDesc,
                                              const TensorDescriptor& wDesc,
                                              const TensorDescriptor& yDesc,
                                              WinogradKernelParams& k_p,
                                              KernelInvoke& kernel,
                                              int direction) const
{
    try
    {
        mlo_construct_winograd construct_params(direction);
        construct_params.setStream(&handle);

        construct_params.setOutputDescFromMLDesc(yDesc);
        construct_params.setInputDescFromMLDesc(xDesc);
        construct_params.setWeightDescFromMLDesc(wDesc);

        construct_params.setConvDescr(pad_h, pad_w, u, v, dilation_h, dilation_w);

        mloConstruct(construct_params);
        std::string program_name = construct_params.getKernelFile();
        std::string kernel_name  = construct_params.getKernelName();
        std::string parms        = construct_params.getCompilerOptions();

        std::string network_config;
        construct_params.mloBuildConf_Key(network_config);

        const std::vector<size_t>& vld = construct_params.getLocalWkSize();
        const std::vector<size_t>& vgd = construct_params.getGlobalWkSize();

        std::string algorithm = (direction == 1) ? "miopenConvolutionFwdAlgoWinograd"
                                                 : "miopenConvolutionBwdDataAlgoWinograd";
        kernel =
            handle.AddKernel(algorithm, network_config, program_name, kernel_name, vld, vgd, parms);

        int N, C, H, W, K, n_groups, out_H, out_W, R, S, pad_H, pad_W;
        construct_params.getCompiledInParameters(
            &N, &C, &H, &W, &K, &n_groups, &out_H, &out_W, &R, &S, &pad_H, &pad_W);
        k_p = std::make_tuple(N,
                              C,
                              H,
                              W,
                              K,
                              n_groups,
                              out_H,
                              out_W,
                              R,
                              S,
                              pad_H,
                              pad_W,
                              kernel_name == "sp3AsmConvRxSU");
        return 0;
    }
    catch(miopen::Exception&)
    {
        return -1;
    }
}

int ConvolutionDescriptor::FindDirectKernel(Handle& handle,
                                            const TensorDescriptor& xDesc,
                                            const TensorDescriptor& wDesc,
                                            const TensorDescriptor& yDesc,
                                            ExtraKernelArgs& extraArgs,
                                            std::vector<KernelInvoke>& kernels,
                                            bool exhaustiveSearch,
                                            int direction) const
{

    if(!IsDirectSupported(wDesc) || miopen::IsDisabled(MIOPEN_DEBUG_CONV_DIRECT{}))
        return -1;

    mlo_construct_direct2D construct_params(direction);
    construct_params.doSearch(exhaustiveSearch);
    construct_params.saveSearchRequest(true);

    construct_params.setGeneralCompOptions("");

    construct_params.setStream(&handle);

    construct_params.setOutputDescFromMLDesc(yDesc);
    construct_params.setInputDescFromMLDesc(xDesc);
    construct_params.setWeightDescFromMLDesc(wDesc);

    construct_params.setConvDescr(pad_h, pad_w, u, v, dilation_h, dilation_w);

    if((IsWinograd3x3Supported(handle, direction, wDesc, (direction ? xDesc : yDesc)) &&
        construct_params.mloIsFastBinaryWinograd3x3U()))
    {
        return -1;
    }

    try
    {
        mloConstruct(construct_params);

        std::string program_name = construct_params.getKernelFile();
        std::string kernel_name  = construct_params.getKernelName();
        const std::string& parms = construct_params.getCompilerOptions();

        std::string network_config;
        construct_params.mloBuildConf_Key(network_config);

        const std::vector<size_t>& vld = construct_params.getLocalWkSize();
        const std::vector<size_t>& vgd = construct_params.getGlobalWkSize();

        std::string algorithm = (direction == 1) ? "miopenConvolutionFwdAlgoDirect"
                                                 : "miopenConvolutionBwdDataAlgoDirect";

        {
            int N, C, H, W, K, n_groups;
            construct_params.getCompiledInParameters(&N, &C, &H, &W, &K, &n_groups);
            extraArgs = std::make_tuple(N, C, H, W, K, n_groups);
        }
        // if not 11x11
        if(program_name != "MIOpenConvFwd_LxL_11.cl")
        {

            auto k = handle.AddKernel(
                algorithm, network_config, program_name, kernel_name, vld, vgd, parms);

            kernels.push_back(k);
        }
        else
        {
            const std::vector<mlo_kernel_info>& bwd_wrw_info = construct_params.getKernelsInfo();
            /*
            * get info for all kernels of the layer
            * std::string _kernel_name;
            * std::string _kernel_file;
            * std::string _comp_options;
            * std::vector<size_t> _g_wk;
            * std::vector<size_t> _l_wk;
            */

            if(bwd_wrw_info.size() == 1)
            {
                const mlo_kernel_info& bwd_wrw = bwd_wrw_info[0];

                auto k1 = handle.AddKernel(algorithm,
                                           network_config,
                                           std::get<1>(bwd_wrw),
                                           std::get<0>(bwd_wrw),
                                           std::get<4>(bwd_wrw),
                                           std::get<3>(bwd_wrw),
                                           std::get<2>(bwd_wrw));

                kernels.push_back(k1);
            }
            else
            {
                auto bwd_wrw_main = bwd_wrw_info[0];

                auto k1 = handle.AddKernel(algorithm,
                                           network_config,
                                           std::get<1>(bwd_wrw_main),
                                           std::get<0>(bwd_wrw_main),
                                           std::get<4>(bwd_wrw_main),
                                           std::get<3>(bwd_wrw_main),
                                           std::get<2>(bwd_wrw_main));

                kernels.push_back(k1);

                // second kernel hash
                network_config += "x1";
                // second pass  kernel
                auto bwd_wrw_red = bwd_wrw_info[1];

                auto k2 = handle.AddKernel(algorithm + "_pass2",
                                           network_config,
                                           std::get<1>(bwd_wrw_red),
                                           std::get<0>(bwd_wrw_red),
                                           std::get<4>(bwd_wrw_red),
                                           std::get<3>(bwd_wrw_red),
                                           std::get<2>(bwd_wrw_red));

                kernels.push_back(k2);
            }
        }

        return 0;
    }
    catch(miopen::Exception&)
    {
        return -1;
    }
}

void ConvolutionDescriptor::FindConvFwdAlgorithm(Handle& handle,
                                                 const TensorDescriptor& xDesc,
                                                 ConstData_t x,
                                                 const TensorDescriptor& wDesc,
                                                 ConstData_t w,
                                                 const TensorDescriptor& yDesc,
                                                 ConstData_t y,
                                                 const int requestAlgoCount,
                                                 int* returnedAlgoCount,
                                                 miopenConvAlgoPerf_t* perfResults,
                                                 Data_t workSpace,
                                                 size_t workSpaceSize,
                                                 bool exhaustiveSearch) const
{

    if(x == nullptr || w == nullptr || y == nullptr)
        MIOPEN_THROW(miopenStatusBadParm, "Buffers cannot be NULL");
    if(returnedAlgoCount == nullptr)
        MIOPEN_THROW(miopenStatusBadParm, "returnedAlgoCount cannot be nullptr");
    if(perfResults == nullptr)
        MIOPEN_THROW(miopenStatusBadParm, "perfResults cannot be nullptr");
    if(requestAlgoCount < 1)
        MIOPEN_THROW(miopenStatusBadParm, "requestAlgoCount cannot be < 1");

    AutoEnableProfiling enableProfiling{handle};

    mlo_construct_direct2D find_params(1); // forward
    find_params.setOutputDescFromMLDesc(yDesc);
    find_params.setInputDescFromMLDesc(xDesc);
    find_params.setWeightDescFromMLDesc(wDesc);
    find_params.setConvDescr(pad_h, pad_w, u, v, dilation_h, dilation_w);
    std::string find_config;
    find_params.mloBuildConf_Key(find_config);

    std::unordered_map<std::string, int>::iterator iter = handle.fwd_map.find(find_config);
    if (iter != handle.fwd_map.end())
    {
        if (requestAlgoCount == 1)
        {
            perfResults[0].fwd_algo =
                static_cast<miopenConvFwdAlgorithm_t>(iter->second);
            *returnedAlgoCount = 1;
            return;
        }
    }


    // create a dummy buffer for use as output for the kernel calls
    // because kernels are called purely for timing purposes
    auto tmp_y = handle.Create(yDesc.GetElementSize() * GetTypeSize(yDesc.GetType()));

    // < algorith_name, <time, workspace_size> >
    std::vector<PerfField> perf_db;

    // GEMM based
    int in_n, in_c, in_h, in_w;
    std::tie(in_n, in_c, in_h, in_w) = tien<4>(xDesc.GetLengths());

    int wei_n, wei_h, wei_w;

    int out_h, out_w;
    std::tie(std::ignore, std::ignore, out_h, out_w) = tien<4>(yDesc.GetLengths());

    std::string network_config;

    if(mode == miopenTranspose)
    {
        std::tie(std::ignore, wei_n, wei_h, wei_w) = tien<4>(wDesc.GetLengths());

#if MIOPEN_USE_MIOPENGEMM
        if(xDesc.GetType() == miopenFloat)
        {
            size_t workspace_req = BackwardDataGetWorkSpaceSizeGEMM(handle, wDesc, xDesc);
            float time_gemm      = 0;
            GemmGeometry gg =
                CreateGemmGeometryConvBwdData(xDesc, wDesc, yDesc, true, network_config);

            // 1x1 does not require im2col or workspace
            if(wei_h == 1 && wei_w == 1 && v == 1 && u == 1)
            {
                gg.FindSolution(.003, handle, w, x, tmp_y.get(), false);
                gg.RunGemm(handle, w, x, tmp_y.get(), 0, 0, 0);

                time_gemm = in_n * handle.GetKernelTime();
                perf_db.push_back(PerfField{"miopenConvolutionFwdAlgoGEMM", time_gemm, 0});
            }

            // if not 1x1
            else if(workSpace != nullptr && workSpaceSize >= workspace_req)
            {
                float time_col2im = 0;
                size_t out_offset = 0;

                gg.FindSolution(.003, handle, w, x, workSpace, false);
                gg.RunGemm(handle, w, x, workSpace, 0, 0, 0);

                time_gemm   = in_n * handle.GetKernelTime();
                time_col2im = Col2ImGPU(handle,
                                        workSpace,
                                        in_h,
                                        in_w,
                                        wei_h,
                                        wei_w,
                                        pad_h,
                                        pad_w,
                                        u,
                                        v,
                                        dilation_h,
                                        dilation_w,
                                        wei_n,
                                        out_h,
                                        out_w,
                                        tmp_y.get(),
                                        out_offset);

                time_gemm += in_n * time_col2im;

                perf_db.push_back(
                    PerfField{"miopenConvolutionFwdAlgoGEMM", time_gemm, workspace_req});
            }
        }
#else
        (void)workSpace;     // Suppress warning
        (void)workSpaceSize; // Suppress warning
#endif
    }
    else if(mode == miopenConvolution)
    {
        std::tie(wei_n, std::ignore, wei_h, wei_w) = tien<4>(wDesc.GetLengths());

#if MIOPEN_USE_MIOPENGEMM
        if(xDesc.GetType() == miopenFloat)
        {
            float time_gemm = 0;

            // Use transpose path if input ht and width <= 14 for 1x1_stride=1 convolutions OR for
            // 1x1_stride=2
            if((wei_h == 1 && wei_w == 1 && pad_h == 0 && pad_w == 0 && dilation_h == 1 &&
                dilation_w == 1) &&
               ((in_h <= 14 && in_w <= 14 && u == 1 && v == 1) || (u == 2 && v == 2)))
            {
                size_t workspace_req = ForwardGetWorkSpaceSizeGEMMTranspose(xDesc, yDesc);
                if(workSpace != nullptr && workSpaceSize >= workspace_req)
                {
                    GemmGeometry gg =
                        CreateGemmGeometryConvFwdCNHW(xDesc, wDesc, yDesc, false, network_config);

                    transpose_NCHW2CNHW(
                        handle, in_n, in_c, in_h, in_w, out_h, out_w, x, workSpace, 0, 0, v, u);
                    time_gemm = handle.GetKernelTime();

                    gg.FindSolution(0.03, handle, workSpace, w, tmp_y.get(), false);
                    size_t x_t_size = in_n * in_c * out_h * out_w;
                    gg.RunGemm(handle, workSpace, w, workSpace, 0, 0, x_t_size);
                    time_gemm += handle.GetKernelTime();

                    transpose_CNHW2NCHW(handle,
                                        in_n,
                                        wei_n,
                                        out_h,
                                        out_w,
                                        out_h,
                                        out_w,
                                        workSpace,
                                        tmp_y.get(),
                                        x_t_size,
                                        0,
                                        1,
                                        1);
                    time_gemm += handle.GetKernelTime();

                    perf_db.push_back(
                        PerfField{"miopenConvolutionFwdAlgoGEMM", time_gemm, workspace_req});
                }
            }
            // 1x1_stride=1 with GEMM and zero workspace
            else if(wei_h == 1 && wei_w == 1 && pad_h == 0 && pad_w == 0 && (u == 1 && v == 1) &&
                    dilation_w == 1 && dilation_h == 1)
            {
                GemmGeometry gg =
                    CreateGemmGeometryConvFwd(xDesc, wDesc, yDesc, false, network_config);

                gg.FindSolution(.003, handle, x, w, tmp_y.get(), false);
                gg.RunGemm(handle, x, w, tmp_y.get(), 0, 0, 0);
                time_gemm = in_n * (handle.GetKernelTime());

                perf_db.push_back(PerfField{"miopenConvolutionFwdAlgoGEMM", time_gemm, 0});
            }
            // if not 1x1
            else if(workSpace != nullptr &&
                    workSpaceSize >= ForwardGetWorkSpaceSizeGEMM(handle, wDesc, yDesc))
            {
                GemmGeometry gg =
                    CreateGemmGeometryConvFwd(xDesc, wDesc, yDesc, false, network_config);
                float time_im2col = 0;
                size_t in_offset  = 0;
                time_im2col       = Im2ColGPU(handle,
                                        xDesc.GetElementSize(),
                                        x,
                                        in_offset,
                                        in_c,
                                        in_h,
                                        in_w,
                                        wei_h,
                                        wei_w,
                                        out_h,
                                        out_w,
                                        pad_h,
                                        pad_w,
                                        u,
                                        v,
                                        dilation_h,
                                        dilation_w,
                                        workSpace);

                gg.FindSolution(.003, handle, workSpace, w, tmp_y.get(), false);
                gg.RunGemm(handle, workSpace, w, tmp_y.get(), 0, 0, 0);
                time_gemm = in_n * (time_im2col + handle.GetKernelTime());
                perf_db.push_back(PerfField{"miopenConvolutionFwdAlgoGEMM",
                                            time_gemm,
                                            ForwardGetWorkSpaceSizeGEMM(handle, wDesc, yDesc)});
            }
        }
#else
        (void)workSpace;     // Suppress warning
        (void)workSpaceSize; // Suppress warning
#endif
        if(dilation_h == 1 && dilation_w == 1)
        {
            // Winograd algo
            WinogradKernelParams k_p;
            KernelInvoke kernel_wino;
            if(FindWinogradKernel(handle, xDesc, wDesc, yDesc, k_p, kernel_wino, 1) == 0)
            { // TODO: be more graceful
                // Execute the winograd kernel
                float time_wino  = 0;
                int flags        = 0;
                int reserved     = 0;
                int* return_addr = nullptr;
                bool isRxS;
                int N, C, H, W, K, n_groups, out_H, out_W, R, S, unused;
                std::tie(N, C, H, W, K, n_groups, out_H, out_W, R, S, unused, unused, isRxS) = k_p;
                // clang-format off
                MIOPEN_LOG_I2(" N=" << N << " C=" << C << " H=" << H << " W=" << W << " K=" << K
                    << " n_groups=" << n_groups << " flags=" << flags << " R=" << R << " S=" << S
                    << " pad_h=" << pad_h << " pad_w=" << pad_w << " out_H=" << out_H << " out_W=" << out_W); // clang-format on
                if(isRxS)
                {
                    kernel_wino(N,
                                C,
                                H,
                                W,
                                K,
                                n_groups,
                                flags,
                                reserved,
                                x,
                                w,
                                tmp_y.get(),
                                return_addr,
                                R,
                                S,
                                pad_h,
                                pad_w,
                                out_H,
                                out_W);
                }
                else
                {
                    kernel_wino(
                        N, C, H, W, K, n_groups, flags, reserved, x, w, tmp_y.get(), return_addr);
                }
                time_wino = handle.GetKernelTime();
                perf_db.push_back(PerfField{"miopenConvolutionFwdAlgoWinograd", time_wino, 0});
            }

            // Direct algo
            ExtraKernelArgs eka;
            std::vector<KernelInvoke> kernel_direct;
            if(FindDirectKernel(
                   handle, xDesc, wDesc, yDesc, eka, kernel_direct, exhaustiveSearch, 1) == 0)
            { // Forward

                // Execute the direct kernel
                float time_direct = 0;
                float padding_val = 0;
                visit_float(xDesc.GetType(), [&](auto as_float) {
                    for(auto& k : kernel_direct)
                    {
                        if(k.GetName() == "gcnAsmConv1x1U")
                        {
                            int unused       = 0;
                            int* return_addr = nullptr;
                            int N, C, H, W, K, n_groups;
                            std::tie(N, C, H, W, K, n_groups) = eka;
                            k(N,
                              C,
                              H,
                              W,
                              K,
                              n_groups,
                              unused,
                              unused,
                              x,
                              w,
                              tmp_y.get(),
                              return_addr);
                        }
                        else
                        {
                            k(x, w, tmp_y.get(), as_float(padding_val));
                        }
                        time_direct += handle.GetKernelTime();
                    }
                });

                perf_db.push_back(PerfField{"miopenConvolutionFwdAlgoDirect", time_direct, 0});
            }

            // FFT algo
            std::vector<KernelInvoke> kernels_fft;
            size_t workspace_fft = ForwardGetWorkSpaceSizeFFT(wDesc, xDesc, yDesc);
            if(FindFwdFFTKernel(handle, xDesc, wDesc, yDesc, workspace_fft, kernels_fft) == 0)
            {
                (void)kernels_fft; // not used now, but needed as fft coverage widens
                if(workSpace != nullptr && workSpaceSize >= workspace_fft)
                {
                    float time_fft = ExecuteFwdFFTKernel(handle,
                                                         xDesc,
                                                         x,
                                                         wDesc,
                                                         w,
                                                         yDesc,
                                                         tmp_y.get(),
                                                         workSpace,
                                                         workSpaceSize,
                                                         true);
                    perf_db.push_back(
                        PerfField{"miopenConvolutionFwdAlgoFFT", time_fft, workspace_fft});
                }
            }
        }
    }

    if(perf_db.empty())
        MIOPEN_THROW("Fwd Convolution cannot be executed due to incorrect params");

    // sort the perf_db
    std::sort(begin(perf_db), end(perf_db));

    // update perfResults
    *returnedAlgoCount = std::min(requestAlgoCount, static_cast<int>(perf_db.size()));

    for(int i = 0; i < *returnedAlgoCount; i++)
    {
        perfResults[i].fwd_algo =
            static_cast<miopenConvFwdAlgorithm_t>(FwdAlgoResolver(perf_db[i].name));
        perfResults[i].time   = perf_db[i].time;
        perfResults[i].memory = perf_db[i].workspace;
#ifndef NDEBUG
        std::cout << "algo = " << perfResults[i].fwd_algo << "\n";
        std::cout << "time = " << perfResults[i].time << "\n";
        std::cout << "workspace = " << perfResults[i].memory << "\n";
#endif // !NDEBUG
    }

    handle.fwd_map.insert(std::unordered_map<std::string, int>::value_type(find_config, FwdAlgoResolver(perf_db[0].name)));
}

void ConvolutionDescriptor::ConvolutionForward(Handle& handle,
                                               const void* alpha,
                                               const TensorDescriptor& xDesc,
                                               ConstData_t x,
                                               const TensorDescriptor& wDesc,
                                               ConstData_t w,
                                               miopenConvFwdAlgorithm_t algo,
                                               const void* beta,
                                               const TensorDescriptor& yDesc,
                                               Data_t y,
                                               Data_t workSpace,
                                               size_t workSpaceSize) const
{

    if(x == nullptr || w == nullptr || y == nullptr)
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }
    if(xDesc.GetSize() != yDesc.GetSize() || xDesc.GetSize() != wDesc.GetSize())
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }
    if(xDesc.GetType() != yDesc.GetType() || xDesc.GetType() != wDesc.GetType())
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }
    //    if(xDesc.GetLengths()[1] != wDesc.GetLengths()[1]) {
    //        MIOPEN_THROW(miopenStatusBadParm);
    //    }
    if(xDesc.GetSize() < 3)
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }
    if(!float_equal(*(static_cast<const float*>(alpha)), 1.0) ||
       !float_equal(*(static_cast<const float*>(beta)), 0))
    {
        MIOPEN_THROW(miopenStatusNotImplemented, "Only alpha=1 and beta=0 is supported");
    }

    if(miopen::CheckNumericsEnabled())
    {
        miopen::checkNumericsInput(handle, xDesc, x);
        miopen::checkNumericsInput(handle, wDesc, w);
    }

#ifndef NDEBUG
    std::cout << "workspace passed " << workSpaceSize << "\n";
#endif // !NDEBUG
    if(mode == miopenConvolution)
    {
        if(xDesc.GetLengths()[1] != wDesc.GetLengths()[1])
        {
            MIOPEN_THROW(miopenStatusBadParm);
        }
        switch(algo)
        {
        case miopenConvolutionFwdAlgoDirect:
        {
            // TODO(paul): Replicating code for now.
            mlo_construct_direct2D construct_params(1); // forward
            construct_params.setOutputDescFromMLDesc(yDesc);
            construct_params.setInputDescFromMLDesc(xDesc);
            construct_params.setWeightDescFromMLDesc(wDesc);
            construct_params.setConvDescr(pad_h, pad_w, u, v, dilation_h, dilation_w);
            construct_params.setStream(&handle);

            std::string network_config;
            construct_params.mloBuildConf_Key(network_config);

            std::string algorithm_name = "miopenConvolutionFwdAlgoDirect";
            float padding_val          = 0;
            auto kernel                = handle.GetKernel(algorithm_name, network_config);

            visit_float(xDesc.GetType(), [&](auto as_float) {
                // if not 11x11
                if((kernel.GetName() != "MIOpenCvFwd11x11"))
                {
                    if(kernel.GetName() == "gcnAsmConv1x1U")
                    {
                        int unused       = 0;
                        int* return_addr = nullptr;
                        int N, C, H, W, K, n_groups;
                        construct_params.getCompiledInParameters(&N, &C, &H, &W, &K, &n_groups);
                        kernel(N, C, H, W, K, n_groups, unused, unused, x, w, y, return_addr);
                    }
                    else
                    {
                        kernel(x, w, y, as_float(padding_val));
                    }
                }
                else
                {
                    /// \todo Something unusual is happening here, why? Shall we rework this?
                    ConvolutionContext context;
                    construct_params.mloCopyTo(context);
                    context.n_passes = true;

                    Db db(context.GetPerfDbPath());
                    solver::ConvSolution solution =
                        FindSolution(solver::ConvOclDirectFwd11x11{}, context, db);

                    if(solution.passes == 1)
                    {
                        kernel(x, w, y, as_float(padding_val));
                    }
                    else
                    {
                        // second kernel has
                        network_config += "x1";
                        auto kernel2 = handle.GetKernel(algorithm_name + "_pass2", network_config);

                        handle.ResetKernelTime();
                        kernel(x, w, y, as_float(padding_val));

                        float time0 = handle.GetKernelTime();
                        kernel2(x, w, y, as_float(padding_val));

                        handle.AccumKernelTime(time0);
                    }
                }
            });
        }
        break;

        case miopenConvolutionFwdAlgoWinograd:
        {
            mlo_construct_winograd construct_params(1); // forward
            construct_params.setOutputDescFromMLDesc(yDesc);
            construct_params.setInputDescFromMLDesc(xDesc);
            construct_params.setWeightDescFromMLDesc(wDesc);
            construct_params.setConvDescr(pad_h, pad_w, u, v, dilation_h, dilation_w);

            construct_params.setStream(&handle);

            std::string network_config;
            construct_params.mloBuildConf_Key(network_config);

            std::string algorithm_name = "miopenConvolutionFwdAlgoWinograd";
            auto kernel                = handle.GetKernel(algorithm_name, network_config);

            int flags        = 0;
            int reserved     = 0;
            int* return_addr = nullptr;
            int N, C, H, W, K, n_groups, out_H, out_W, R, S, unused;
            construct_params.getCompiledInParameters(
                &N, &C, &H, &W, &K, &n_groups, &out_H, &out_W, &R, &S, &unused, &unused);
            // clang-format off
            MIOPEN_LOG_I2(" N=" << N << " C=" << C << " H=" << H << " W=" << W << " K=" << K
                << " n_groups=" << n_groups << " flags=" << flags << " R=" << R << " S=" << S
                << " pad_h=" << pad_h << " pad_w=" << pad_w << " out_H=" << out_H << " out_W=" << out_W); // clang-format on
            if(kernel.GetName() == "sp3AsmConvRxSU")
            {
                kernel(N,
                       C,
                       H,
                       W,
                       K,
                       n_groups,
                       flags,
                       reserved,
                       x,
                       w,
                       y,
                       return_addr,
                       R,
                       S,
                       pad_h,
                       pad_w,
                       out_H,
                       out_W);
            }
            else
            {
                kernel(N, C, H, W, K, n_groups, flags, reserved, x, w, y, return_addr);
            }
        }
        break;

        case miopenConvolutionFwdAlgoGEMM:
        {
            int in_n, in_c, in_h, in_w;
            std::tie(in_n, in_c, in_h, in_w) = tien<4>(xDesc.GetLengths());

            int wei_n, wei_h, wei_w;
            std::tie(wei_n, std::ignore, wei_h, wei_w) = tien<4>(wDesc.GetLengths());

            int out_h, out_w;
            std::tie(std::ignore, std::ignore, out_h, out_w) = tien<4>(yDesc.GetLengths());

            std::string network_config;
#if MIOPEN_USE_MIOPENGEMM
            // Use transpose path if input ht and width <= 14 for 1x1_stride=1 convolutions OR for
            // 1x1_stride=2
            if((wei_h == 1 && wei_w == 1 && pad_h == 0 && pad_w == 0 && dilation_h == 1 &&
                dilation_w == 1) &&
               ((in_h <= 14 && in_w <= 14 && u == 1 && v == 1) || (u == 2 && v == 2)))
            {

                assert(workSpace != nullptr &&
                       workSpaceSize >= ForwardGetWorkSpaceSizeGEMMTranspose(xDesc, yDesc));

                CreateGemmGeometryConvFwdCNHW(xDesc, wDesc, yDesc, false, network_config);
                GemmGeometry gg =
                    GetGemmGeometry(handle, "miopenConvolutionFwdAlgoGEMM", network_config);

                float t1 = 0;
                transpose_NCHW2CNHW(
                    handle, in_n, in_c, in_h, in_w, out_h, out_w, x, workSpace, 0, 0, v, u);
                if(handle.IsProfilingEnabled())
                    t1 = handle.GetKernelTime();

                size_t x_t_size = in_n * in_c * out_h * out_w;
                gg.RunGemm(handle, workSpace, w, workSpace, 0, 0, x_t_size);
                if(handle.IsProfilingEnabled())
                    t1 += handle.GetKernelTime();

                transpose_CNHW2NCHW(handle,
                                    in_n,
                                    wei_n,
                                    out_h,
                                    out_w,
                                    out_h,
                                    out_w,
                                    workSpace,
                                    y,
                                    x_t_size,
                                    0,
                                    1,
                                    1);
                if(handle.IsProfilingEnabled())
                    t1 += handle.GetKernelTime();

                if(handle.IsProfilingEnabled())
                {
                    handle.ResetKernelTime();
                    handle.AccumKernelTime(t1);
                }
            }
            else if(wei_h == 1 && wei_w == 1 && pad_h == 0 && pad_w == 0 && (u == 1 && v == 1) &&
                    dilation_w == 1 && dilation_h == 1)
            {
                float time_0 = 0;
                CreateGemmGeometryConvFwd(xDesc, wDesc, yDesc, false, network_config);
                GemmGeometry gg =
                    GetGemmGeometry(handle, "miopenConvolutionFwdAlgoGEMM", network_config);

                for(int i = 0; i < in_n; i++)
                {
                    int out_offset = i * wei_n * out_h * out_w;
                    int in_offset  = i * in_c * in_h * in_w;
                    gg.RunGemm(handle, x, w, y, in_offset, 0, out_offset);
                    if(handle.IsProfilingEnabled())
                    {
                        if(i == in_n - 1)
                            handle.AccumKernelTime(time_0);
                        time_0 += handle.GetKernelTime();
                    }
                }
            }
            else
            {
                assert(workSpace != nullptr &&
                       workSpaceSize >= ForwardGetWorkSpaceSizeGEMM(handle, wDesc, yDesc));

                CreateGemmGeometryConvFwd(xDesc, wDesc, yDesc, false, network_config);
                GemmGeometry gg =
                    GetGemmGeometry(handle, "miopenConvolutionFwdAlgoGEMM", network_config);

                float time_0 = 0;
                float t1     = 0;
                for(int i = 0; i < in_n; i++)
                {
                    int out_offset = i * wei_n * out_h * out_w;
                    if(wei_h != 1 || wei_w != 1 || v != 1 || u != 1)
                    {
                        size_t in_offset = i * in_c * in_h * in_w;
                        Im2ColGPU(handle,
                                  xDesc.GetElementSize(),
                                  x,
                                  in_offset,
                                  in_c,
                                  in_h,
                                  in_w,
                                  wei_h,
                                  wei_w,
                                  out_h,
                                  out_w,
                                  pad_h,
                                  pad_w,
                                  u,
                                  v,
                                  dilation_h,
                                  dilation_w,
                                  workSpace);
                        if(handle.IsProfilingEnabled())
                            t1 = handle.GetKernelTime();

                        gg.RunGemm(handle, workSpace, w, y, 0, 0, out_offset);

                        // Update times for both the kernels
                        if(handle.IsProfilingEnabled())
                        {
                            if(i == in_n - 1)
                                handle.AccumKernelTime(t1 + time_0);
                            else
                                handle.AccumKernelTime(t1);
                            time_0 += handle.GetKernelTime();
                        }
                    }
                }
            }
#else
            MIOPEN_THROW("GEMM is not supported");
#endif
        }
#if MIOPEN_USE_MIOPENGEMM
        break;
#endif
        case miopenConvolutionFwdAlgoFFT:
        {
            size_t workspace_fft = ForwardGetWorkSpaceSizeFFT(wDesc, xDesc, yDesc);
            if(workSpace != nullptr && workSpaceSize >= workspace_fft)
            {
                bool timed  = handle.IsProfilingEnabled();
                float timev = ExecuteFwdFFTKernel(
                    handle, xDesc, x, wDesc, w, yDesc, y, workSpace, workSpaceSize, timed);
                // FIXME: Is workSpaceSize correct here? It seems that workspace_fft is.

                if(timed)
                {
                    handle.ResetKernelTime();
                    handle.AccumKernelTime(timev);
                }
            }
        }
        break;
        }
    }
    else if(mode == miopenTranspose)
    {
        if(xDesc.GetLengths()[1] != wDesc.GetLengths()[0])
        {
            MIOPEN_THROW(miopenStatusBadParm);
        }

        // GEMM based
        int in_n, in_c, in_h, in_w;
        std::tie(in_n, in_c, in_h, in_w) = tien<4>(xDesc.GetLengths());

        int wei_n, wei_h, wei_w;
        std::tie(std::ignore, wei_n, wei_h, wei_w) = tien<4>(wDesc.GetLengths());

        int out_h, out_w;
        std::tie(std::ignore, std::ignore, out_h, out_w) = tien<4>(yDesc.GetLengths());

        if(wei_h != 1 || wei_w != 1 || u != 1 || v != 1)
        {
            assert(workSpace != nullptr &&
                   workSpaceSize >= BackwardDataGetWorkSpaceSizeGEMM(handle, wDesc, xDesc));
        }

        std::string network_config;

#if MIOPEN_USE_MIOPENGEMM
        CreateGemmGeometryConvBwdData(xDesc, wDesc, yDesc, true, network_config);
        GemmGeometry gg =
            GetGemmGeometry(handle, "miopenConvolutionBwdDataAlgoGEMM", network_config);

        float time_0 = 0;
        float t1     = 0;
        for(int i = 0; i < in_n; i++)
        {
            int out_offset = i * wei_n * out_h * out_w;
            if(wei_h != 1 || wei_w != 1 || v != 1 || u != 1)
            {
                size_t in_offset = i * in_c * in_h * in_w;

                gg.RunGemm(handle, w, x, workSpace, 0, in_offset, 0);

                if(handle.IsProfilingEnabled())
                    t1 = handle.GetKernelTime();

                Col2ImGPU(handle,
                          workSpace,
                          in_h,
                          in_w,
                          wei_h,
                          wei_w,
                          pad_h,
                          pad_w,
                          u,
                          v,
                          dilation_h,
                          dilation_w,
                          wei_n,
                          out_h,
                          out_w,
                          y,
                          out_offset);

                // Update times for both the kernels
                if(handle.IsProfilingEnabled())
                {
                    if(i == in_n - 1)
                        handle.AccumKernelTime(t1 + time_0);
                    else
                        handle.AccumKernelTime(t1);
                    time_0 += handle.GetKernelTime();
                }
            }
            else if(wei_h == 1 && wei_w == 1 && v == 1 && u == 1)
            {
                int in_offset = i * in_c * in_h * in_w;
                gg.RunGemm(handle, w, x, y, 0, in_offset, out_offset);
                if(handle.IsProfilingEnabled())
                {
                    if(i == in_n - 1)
                        handle.AccumKernelTime(time_0);
                    time_0 += handle.GetKernelTime();
                }
            }
        }
#else
        MIOPEN_THROW("GEMM is not supported");
#endif
    }

    if(miopen::CheckNumericsEnabled())
    {
        miopen::checkNumericsOutput(handle, yDesc, y);
    }
}

// FindBackwardDataAlgorithm()
//
void ConvolutionDescriptor::FindConvBwdDataAlgorithm(Handle& handle,
                                                     const TensorDescriptor& dyDesc,
                                                     ConstData_t dy,
                                                     const TensorDescriptor& wDesc,
                                                     ConstData_t w,
                                                     const TensorDescriptor& dxDesc,
                                                     ConstData_t dx,
                                                     const int requestAlgoCount,
                                                     int* returnedAlgoCount,
                                                     miopenConvAlgoPerf_t* perfResults,
                                                     Data_t workSpace,
                                                     size_t workSpaceSize,
                                                     bool exhaustiveSearch) const
{

    if(dx == nullptr || w == nullptr || dy == nullptr)
        MIOPEN_THROW(miopenStatusBadParm, "Buffers cannot be NULL");
    if(returnedAlgoCount == nullptr)
        MIOPEN_THROW(miopenStatusBadParm, "returnedAlgoCount cannot be nullptr");
    if(perfResults == nullptr)
        MIOPEN_THROW(miopenStatusBadParm, "perfResults cannot be nullptr");
    if(requestAlgoCount < 1)
        MIOPEN_THROW(miopenStatusBadParm, "requestAlgoCount cannot be < 1");


    mlo_construct_direct2D find_params(0); // forward
    find_params.setOutputDescFromMLDesc(dyDesc);
    find_params.setInputDescFromMLDesc(dxDesc);
    find_params.setWeightDescFromMLDesc(wDesc);
    find_params.setConvDescr(pad_h, pad_w, u, v, dilation_h, dilation_w);
    std::string find_config;
    find_params.mloBuildConf_Key(find_config);

    std::unordered_map<std::string, int>::iterator iter = handle.bwdData_map.find(find_config);
    if (iter != handle.bwdData_map.end())
    {
	    if (requestAlgoCount == 1)
	    {
		    perfResults[0].bwd_data_algo =
			    static_cast<miopenConvBwdDataAlgorithm_t>(iter->second);
		    *returnedAlgoCount = 1;
		    return;
	    }
    }

    // create a dummy buffer for use as output for the kernel calls
    // because kernels are called purely for timing purposes
    auto tmp_dx = handle.Create(dxDesc.GetElementSize() * GetTypeSize(dxDesc.GetType()));

    AutoEnableProfiling enableProfiling{handle};

    // < algorith_name, <time, workspace_size> >
    std::vector<PerfField> perf_db;

    // GEMM based
    int in_n, in_c, in_h, in_w;
    std::tie(in_n, in_c, in_h, in_w) = tien<4>(dxDesc.GetLengths());

    int wei_n, wei_h, wei_w;

    int out_h, out_w;
    std::tie(std::ignore, std::ignore, out_h, out_w) = tien<4>(dyDesc.GetLengths());

    std::string network_config;

    if(mode == miopenTranspose)
    {
        // GEMM based
        std::tie(std::ignore, wei_n, wei_h, wei_w) = tien<4>(wDesc.GetLengths());

#if MIOPEN_USE_MIOPENGEMM
        if(dyDesc.GetType() == miopenFloat)
        {
            size_t workspace_req = ForwardGetWorkSpaceSizeGEMM(handle, wDesc, dxDesc);
            float time_gemm      = 0;
            GemmGeometry gg =
                CreateGemmGeometryTranBwdData(dyDesc, wDesc, dxDesc, true, network_config);

            // 1x1 does not require im2col or workspace
            if(wei_h == 1 && wei_w == 1 && v == 1 && u == 1)
            {
                gg.FindSolution(.003, handle, w, dy, tmp_dx.get(), false);
                gg.RunGemm(handle, w, dy, tmp_dx.get(), 0, 0, 0);

                time_gemm = in_n * handle.GetKernelTime();
                perf_db.push_back(PerfField{"miopenTransposeBwdDataAlgoGEMM", time_gemm, 0});
            }

            // if not 1x1
            else if(workSpace != nullptr && workSpaceSize >= workspace_req)
            {
                float time_im2col = 0;
                size_t out_offset = 0;
                time_im2col       = Im2ColGPU(handle,
                                        dyDesc.GetElementSize(),
                                        dy,
                                        out_offset,
                                        wei_n,
                                        out_h,
                                        out_w,
                                        wei_h,
                                        wei_w,
                                        in_h,
                                        in_w,
                                        pad_h,
                                        pad_w,
                                        u,
                                        v,
                                        dilation_h,
                                        dilation_w,
                                        workSpace);

                gg.FindSolution(.003, handle, w, workSpace, tmp_dx.get(), false);
                gg.RunGemm(handle, w, workSpace, tmp_dx.get(), 0, 0, 0);
                time_gemm = in_n * (time_im2col + handle.GetKernelTime());
                perf_db.push_back(
                    PerfField{"miopenTransposeBwdDataAlgoGEMM", time_gemm, workspace_req});
            }
        }
#else
        (void)workSpace;     // Suppress warning
        (void)workSpaceSize; // Suppress warning
#endif
    }
    else if(mode == miopenConvolution)
    {
        if(dilation_h == 1 && dilation_w == 1)
        {
            // Winograd algo
            WinogradKernelParams k_p;
            KernelInvoke kernel_wino;
            if(FindWinogradKernel(handle, dxDesc, wDesc, dyDesc, k_p, kernel_wino, 0) == 0)
            { // TODO: be more graceful
                float time_wino = 0;
                /// \todo Move Flags into Solution.
                /// Flags:
                ///  - Any combination of flags is allowed.
                ///  - The last two (F_FLIP_DATA_N_C, F_FLIP_OUT_N_K) are for RxS version only.
                ///
                /// Reverse indexing of r, r -> R-1-r if set.
                static const int F_REVERSE_R = 1 << 0;
                /// Reverse indexing of s, s -> S-1-s if set.
                static const int F_REVERSE_S = 1 << 1;
                /// The w ("filter_addr") to be interpreted as float F [C][K][3][3] instead of float
                /// F [K][C][3][3].
                static const int F_FLIP_K_C = 1 << 2;
                /// Causes the dy ("data_addr") to be interpreted as float D [C][N][H][W] with the
                /// following restrictions:
                ///  - Read several stacks, no restrictions when reading single C
                ///  - When reading 2x C, ((N * H * W) <= 2^28)
                /// instead of float D [N][C][H][W] with the following restrictions:
                ///  - Read several stacks, if (H * W) >= 128 not more than 2, distance at most one
                ///    stack, else  (C * H * W) <= 2^23 and it can do 32 stacks, so
                ///    (C * H * W) <= 2^28.
                ///  - Reading 2x C at once not a problem if it can read one.
                // static const int F_FLIP_DATA_N_C = 1 << 3;
                /// Causes the dx ("output_addr") to be interpreted as
                /// float OUT[K][N][out_h][out_w] (no specific restrictions)
                /// instead of float OUT [N][K][out_h][out_w] with the
                /// following restrictions:
                ///  - (K * out_h * out_w) <= 2^28
                // static const int F_FLIP_OUT_N_K = 1 << 4;
                /// <End of Flags>
                // (void)F_FLIP_DATA_N_C;
                // (void)F_FLIP_OUT_N_K;
                int flags        = F_REVERSE_R + F_REVERSE_S + F_FLIP_K_C;
                int reserved     = 0;
                int* return_addr = nullptr;
                int N, C, H, W, K, n_groups, out_H, out_W, R, S, pad_H, pad_W;
                bool isRxS;
                std::tie(N, C, H, W, K, n_groups, out_H, out_W, R, S, pad_H, pad_W, isRxS) = k_p;
                // clang-format off
                MIOPEN_LOG_I2(" N=" << N << " C=" << C << " H=" << H << " W=" << W << " K=" << K
                    << " n_groups=" << n_groups << " flags=" << flags << " R=" << R << " S=" << S
                    << " pad_H=" << pad_H << " pad_W=" << pad_W << " out_H=" << out_H << " out_W=" << out_W); // clang-format on
                if(isRxS)
                {
                    kernel_wino(N,
                                C,
                                H,
                                W,
                                K,
                                n_groups,
                                flags,
                                reserved,
                                dy,
                                w,
                                tmp_dx.get(),
                                return_addr,
                                R,
                                S,
                                pad_H,
                                pad_W,
                                out_H,
                                out_W);
                }
                else
                {
                    kernel_wino(
                        N, C, H, W, K, n_groups, flags, reserved, dy, w, tmp_dx.get(), return_addr);
                }
                time_wino = handle.GetKernelTime();
                perf_db.push_back(PerfField{"miopenConvolutionBwdDataAlgoWinograd", time_wino, 0});
            }

            // Direct algo
            ExtraKernelArgs eka;
            std::vector<KernelInvoke> kernel_direct;
            if(FindDirectKernel(
                   handle, dxDesc, wDesc, dyDesc, eka, kernel_direct, exhaustiveSearch, 0) == 0)
            { // Backward
                float time_direct = 0;
                float padding_val = 0;

                visit_float(dyDesc.GetType(), [&](auto as_float) {
                    for(auto& k : kernel_direct)
                    {
                        if(k.GetName() == "gcnAsmConv1x1U")
                        {
                            int unused       = 0;
                            int* return_addr = nullptr;
                            int N, C, H, W, K, n_groups;
                            std::tie(N, C, H, W, K, n_groups) = eka;
                            k(N,
                              C,
                              H,
                              W,
                              K,
                              n_groups,
                              unused,
                              unused,
                              dy,
                              w,
                              tmp_dx.get(),
                              return_addr);
                        }
                        else
                        {
                            k(dy, w, tmp_dx.get(), as_float(padding_val));
                        }
                        time_direct += handle.GetKernelTime();
                    }
                });

                perf_db.push_back(PerfField{"miopenConvolutionBwdDataAlgoDirect", time_direct, 0});
            }

            // FFT algo
            std::vector<KernelInvoke> kernels_fft;
            size_t workspace_fft = BackwardGetWorkSpaceSizeFFT(wDesc, dyDesc, dxDesc);
            if(FindBwdFFTKernel(handle, dyDesc, wDesc, dxDesc, workspace_fft, kernels_fft) == 0)
            {
                (void)kernels_fft; // not used now, but needed as fft coverage widens
                if(workSpace != nullptr && workSpaceSize >= workspace_fft)
                {
                    float time_fft = ExecuteBwdFFTKernel(handle,
                                                         dyDesc,
                                                         dy,
                                                         wDesc,
                                                         w,
                                                         dxDesc,
                                                         tmp_dx.get(),
                                                         workSpace,
                                                         workSpaceSize,
                                                         true);
                    perf_db.push_back(
                        PerfField{"miopenConvolutionBwdDataAlgoFFT", time_fft, workspace_fft});
                }
            }
        }

        // GEMM based
        std::tie(wei_n, std::ignore, wei_h, wei_w) = tien<4>(wDesc.GetLengths());

#if MIOPEN_USE_MIOPENGEMM
        if(dyDesc.GetType() == miopenFloat)
        {

            float time_gemm = 0;

            // 1x1 does not require col2im or workspace
            if(wei_h == 1 && wei_w == 1 && pad_h == 0 && pad_w == 0 && (u == 2 && v == 2) &&
               dilation_w == 1 && dilation_h == 1 && workSpace != nullptr &&
               workSpaceSize >= BackwardDataGetWorkSpaceSizeGEMMTranspose(dyDesc, dxDesc))
            {

                float zero = 0.f;
                SetTensor(handle, dxDesc, tmp_dx.get(), &zero);
                time_gemm = handle.GetKernelTime();

                GemmGeometry gg =
                    CreateGemmGeometryConvBwdDataCNHW(dyDesc, wDesc, dxDesc, true, network_config);

                transpose_NCHW2CNHW(
                    handle, in_n, wei_n, out_h, out_w, out_h, out_w, dy, workSpace, 0, 0, 1, 1);
                time_gemm += handle.GetKernelTime();

                gg.FindSolution(0.03, handle, w, dy, tmp_dx.get(), false);
                gg.RunGemm(handle, w, workSpace, workSpace, 0, 0, dyDesc.GetElementSize());
                time_gemm += handle.GetKernelTime();

                transpose_CNHW2NCHW(handle,
                                    in_n,
                                    in_c,
                                    out_h,
                                    out_w,
                                    in_h,
                                    in_w,
                                    workSpace,
                                    tmp_dx.get(),
                                    dyDesc.GetElementSize(),
                                    0,
                                    u,
                                    v);
                time_gemm += handle.GetKernelTime();
                perf_db.push_back(
                    PerfField{"miopenConvolutionBwdDataAlgoGEMM",
                              time_gemm,
                              BackwardDataGetWorkSpaceSizeGEMMTranspose(dyDesc, dxDesc)});
            }
            // 1x1_stride=1 convolutions use GEMM and zero workspace
            else if(wei_h == 1 && wei_w == 1 && pad_h == 0 && pad_w == 0 && (u == 1 && v == 1) &&
                    dilation_w == 1 && dilation_h == 1)
            {
                GemmGeometry gg =
                    CreateGemmGeometryConvBwdData(dyDesc, wDesc, dxDesc, true, network_config);

                gg.FindSolution(.003, handle, w, dy, tmp_dx.get(), false);
                gg.RunGemm(handle, w, dy, tmp_dx.get(), 0, 0, 0);

                time_gemm = in_n * handle.GetKernelTime();

                perf_db.push_back(PerfField{"miopenConvolutionBwdDataAlgoGEMM", time_gemm, 0});
            }
            // if not 1x1
            else if(workSpace != nullptr &&
                    workSpaceSize >= BackwardDataGetWorkSpaceSizeGEMM(handle, wDesc, dyDesc))
            {
                GemmGeometry gg =
                    CreateGemmGeometryConvBwdData(dyDesc, wDesc, dxDesc, true, network_config);

                float time_col2im = 0;
                size_t in_offset  = 0;

                gg.FindSolution(.003, handle, w, dy, workSpace, false);
                gg.RunGemm(handle, w, dy, workSpace, 0, 0, 0);

                time_gemm   = in_n * handle.GetKernelTime();
                time_col2im = Col2ImGPU(handle,
                                        workSpace,
                                        out_h,
                                        out_w,
                                        wei_h,
                                        wei_w,
                                        pad_h,
                                        pad_w,
                                        u,
                                        v,
                                        dilation_h,
                                        dilation_w,
                                        in_c,
                                        in_h,
                                        in_w,
                                        tmp_dx.get(),
                                        in_offset);

                time_gemm += in_n * time_col2im;

                perf_db.push_back(
                    PerfField{"miopenConvolutionBwdDataAlgoGEMM",
                              time_gemm,
                              BackwardDataGetWorkSpaceSizeGEMM(handle, wDesc, dyDesc)});
            }
        }
#else
        (void)workSpace;     // Suppress warning
        (void)workSpaceSize; // Suppress warning
#endif
    }

    if(perf_db.empty())
        MIOPEN_THROW(miopenStatusUnknownError, "Backward Data Algo cannot be executed");

    // sort the perf_db
    std::sort(begin(perf_db), end(perf_db));

    // update perfResults
    *returnedAlgoCount = std::min(requestAlgoCount, static_cast<int>(perf_db.size()));

    for(int i = 0; i < *returnedAlgoCount; i++)
    {
        perfResults[i].bwd_data_algo =
            static_cast<miopenConvBwdDataAlgorithm_t>(BwdDataAlgoResolver(perf_db[i].name));
        perfResults[i].time   = perf_db[i].time;
        perfResults[i].memory = perf_db[i].workspace;
    }

    handle.bwdData_map.insert(std::unordered_map<std::string, int>::value_type(find_config, BwdDataAlgoResolver(perf_db[0].name)));
}

// BackwardDataAlgorithm()
void ConvolutionDescriptor::ConvolutionBackwardData(Handle& handle,
                                                    const void* alpha,
                                                    const TensorDescriptor& dyDesc,
                                                    ConstData_t dy,
                                                    const TensorDescriptor& wDesc,
                                                    ConstData_t w,
                                                    miopenConvBwdDataAlgorithm_t algo,
                                                    const void* beta,
                                                    const TensorDescriptor& dxDesc,
                                                    Data_t dx,
                                                    Data_t workSpace,
                                                    size_t workSpaceSize) const
{

    if(dx == nullptr || w == nullptr || dy == nullptr)
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }
    if(dyDesc.GetSize() != dxDesc.GetSize() || dyDesc.GetSize() != wDesc.GetSize())
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }
    if(dyDesc.GetType() != dxDesc.GetType() || dyDesc.GetType() != wDesc.GetType())
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }
    //    if(dyDesc.GetLengths()[1] != wDesc.GetLengths()[0]) {
    //       MIOPEN_THROW(miopenStatusBadParm);
    //    }
    if(dyDesc.GetSize() < 3)
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }
    if(!float_equal(*(static_cast<const float*>(alpha)), 1.0) ||
       !float_equal(*(static_cast<const float*>(beta)), 0))
    {
        MIOPEN_THROW("Only alpha=1 and beta=0 is supported");
    }

    if(miopen::CheckNumericsEnabled())
    {
        miopen::checkNumericsInput(handle, dyDesc, dy);
        miopen::checkNumericsInput(handle, wDesc, w);
        if(!float_equal(*(static_cast<const float*>(beta)), 0))
        {
            miopen::checkNumericsInput(handle, dxDesc, dx);
        }
    }

    if(mode == miopenConvolution)
    {
        if(dyDesc.GetLengths()[1] != wDesc.GetLengths()[0])
        {
            MIOPEN_THROW(miopenStatusBadParm);
        }
        // Launch all kernels and store the perf, workspace limits, etc.
        switch(algo)
        {
        case miopenConvolutionBwdDataAlgoDirect:
        {
            mlo_construct_direct2D construct_params(0); // backward
            {
                construct_params.setOutputDescFromMLDesc(dyDesc);
                construct_params.setInputDescFromMLDesc(dxDesc);
                construct_params.setWeightDescFromMLDesc(wDesc);
                construct_params.setConvDescr(pad_h, pad_w, u, v, dilation_h, dilation_w);
                construct_params.setStream(&handle);
            }

            std::string network_config;
            construct_params.mloBuildConf_Key(network_config);
            auto kernel = handle.GetKernel("miopenConvolutionBwdDataAlgoDirect", network_config);

            visit_float(dyDesc.GetType(), [&](auto as_float) {
                if(kernel.GetName() == "gcnAsmConv1x1U")
                {
                    int unused       = 0;
                    int* return_addr = nullptr;
                    int N, C, H, W, K, n_groups;
                    construct_params.getCompiledInParameters(&N, &C, &H, &W, &K, &n_groups);
                    kernel(N, C, H, W, K, n_groups, unused, unused, dy, w, dx, return_addr);
                }
                else
                {
                    float padding_val = 0;
                    kernel(dy, w, dx, as_float(padding_val));
                }
            });
            break;
        }

        case miopenConvolutionBwdDataAlgoWinograd:
        {
            mlo_construct_winograd construct_params(0); // backward data
            construct_params.setOutputDescFromMLDesc(dyDesc);
            construct_params.setInputDescFromMLDesc(dxDesc);
            construct_params.setWeightDescFromMLDesc(wDesc);
            construct_params.setConvDescr(pad_h, pad_w, u, v, dilation_h, dilation_w);

            construct_params.setStream(&handle);
            std::string network_config;
            construct_params.mloBuildConf_Key(network_config);

            auto kernel = handle.GetKernel("miopenConvolutionBwdDataAlgoWinograd", network_config);
            /// \todo Copied from ConvolutionDescriptor::FindConvBwdDataAlgorithm()
            static const int F_REVERSE_R = 1 << 0;
            static const int F_REVERSE_S = 1 << 1;
            static const int F_FLIP_K_C  = 1 << 2;
            int flags                    = F_REVERSE_R + F_REVERSE_S + F_FLIP_K_C;
            int reserved                 = 0;
            int* return_addr             = nullptr;
            int N, C, H, W, K, n_groups, out_H, out_W, R, S, pad_H, pad_W;
            construct_params.getCompiledInParameters(
                &N, &C, &H, &W, &K, &n_groups, &out_H, &out_W, &R, &S, &pad_H, &pad_W);
            // clang-format off
            MIOPEN_LOG_I2(" N=" << N << " C=" << C << " H=" << H << " W=" << W << " K=" << K
                    << " n_groups=" << n_groups << " flags=" << flags << " R=" << R << " S=" << S
                    << " pad_H=" << pad_H << " pad_W=" << pad_W << " out_H=" << out_H << " out_W=" << out_W); // clang-format on
            if(kernel.GetName() == "sp3AsmConvRxSU")
            {
                kernel(N,
                       C,
                       H,
                       W,
                       K,
                       n_groups,
                       flags,
                       reserved,
                       dy,
                       w,
                       dx,
                       return_addr,
                       R,
                       S,
                       pad_H,
                       pad_W,
                       out_H,
                       out_W);
            }
            else
            {
                kernel(N, C, H, W, K, n_groups, flags, reserved, dy, w, dx, return_addr);
            }
            break;
        }

        case miopenConvolutionBwdDataAlgoGEMM:
        {
            int in_n, in_c, in_h, in_w;
            std::tie(in_n, in_c, in_h, in_w) = tien<4>(dxDesc.GetLengths());

            int wei_n, wei_h, wei_w;
            std::tie(wei_n, std::ignore, wei_h, wei_w) = tien<4>(wDesc.GetLengths());

            int out_h, out_w;
            std::tie(std::ignore, std::ignore, out_h, out_w) = tien<4>(dyDesc.GetLengths());

            std::string network_config;
#if MIOPEN_USE_MIOPENGEMM
            if(wei_h == 1 && wei_w == 1 && pad_h == 0 && pad_w == 0 && (u == 2 && v == 2) &&
               dilation_w == 1 && dilation_h == 1)
            {
                float t1 = 0;
                // Initialization required for upsampling in bwd direction
                float zero = 0.f;
                SetTensor(handle, dxDesc, dx, &zero);
                if(handle.IsProfilingEnabled())
                    t1 = handle.GetKernelTime();

                assert(workSpace != nullptr &&
                       workSpaceSize >= BackwardDataGetWorkSpaceSizeGEMMTranspose(dyDesc, dxDesc));

                CreateGemmGeometryConvBwdDataCNHW(dyDesc, wDesc, dxDesc, true, network_config);
                GemmGeometry gg =
                    GetGemmGeometry(handle, "miopenConvolutionBwdDataAlgoGEMM", network_config);

                transpose_NCHW2CNHW(
                    handle, in_n, wei_n, out_h, out_w, out_h, out_w, dy, workSpace, 0, 0, 1, 1);
                if(handle.IsProfilingEnabled())
                    t1 += handle.GetKernelTime();

                gg.RunGemm(handle, w, workSpace, workSpace, 0, 0, dyDesc.GetElementSize());
                if(handle.IsProfilingEnabled())
                    t1 += handle.GetKernelTime();

                transpose_CNHW2NCHW(handle,
                                    in_n,
                                    in_c,
                                    out_h,
                                    out_w,
                                    in_h,
                                    in_w,
                                    workSpace,
                                    dx,
                                    dyDesc.GetElementSize(),
                                    0,
                                    u,
                                    v);
                if(handle.IsProfilingEnabled())
                    t1 += handle.GetKernelTime();

                if(handle.IsProfilingEnabled())
                {
                    handle.ResetKernelTime();
                    handle.AccumKernelTime(t1);
                }
            }
            // 1x1_stride=1 convolutions use GEMM and zero workspace
            else if(wei_h == 1 && wei_w == 1 && pad_h == 0 && pad_w == 0 && (u == 1 && v == 1) &&
                    dilation_w == 1 && dilation_h == 1)
            {
                CreateGemmGeometryConvBwdData(dyDesc, wDesc, dxDesc, true, network_config);
                GemmGeometry gg =
                    GetGemmGeometry(handle, "miopenConvolutionBwdDataAlgoGEMM", network_config);

                float time_0 = 0;
                for(int i = 0; i < in_n; i++)
                {
                    int out_offset   = i * wei_n * out_h * out_w;
                    size_t in_offset = i * in_c * in_h * in_w;

                    gg.RunGemm(handle, w, dy, dx, 0, out_offset, in_offset);

                    if(handle.IsProfilingEnabled())
                    {
                        if(i == in_n - 1)
                            handle.AccumKernelTime(time_0);
                        time_0 += handle.GetKernelTime();
                    }
                }
            }
            // if not 1x1
            else
            {

                assert(workSpace != nullptr &&
                       workSpaceSize >= BackwardDataGetWorkSpaceSizeGEMM(handle, wDesc, dyDesc));

                CreateGemmGeometryConvBwdData(dyDesc, wDesc, dxDesc, true, network_config);
                GemmGeometry gg =
                    GetGemmGeometry(handle, "miopenConvolutionBwdDataAlgoGEMM", network_config);

                handle.ResetKernelTime();

                float time_0 = 0;
                float t1     = 0;
                for(int i = 0; i < in_n; i++)
                {
                    int out_offset = i * wei_n * out_h * out_w;

                    if(wei_h != 1 || wei_w != 1 || v != 1 || u != 1)
                    {
                        size_t in_offset = i * in_c * in_h * in_w;

                        gg.RunGemm(handle, w, dy, workSpace, 0, out_offset, 0);

                        if(handle.IsProfilingEnabled())
                            t1 = handle.GetKernelTime();

                        Col2ImGPU(handle,
                                  workSpace,
                                  out_h,
                                  out_w,
                                  wei_h,
                                  wei_w,
                                  pad_h,
                                  pad_w,
                                  u,
                                  v,
                                  dilation_h,
                                  dilation_w,
                                  in_c,
                                  in_h,
                                  in_w,
                                  dx,
                                  in_offset);

                        // Update times for both the kernels
                        if(handle.IsProfilingEnabled())
                        {
                            if(i == in_n - 1)
                                handle.AccumKernelTime(t1 + time_0);
                            else
                                handle.AccumKernelTime(t1);
                            time_0 += handle.GetKernelTime();
                        }
                    }
                }
            }
#else
            MIOPEN_THROW("GEMM is not supported");
#endif
        }
#if MIOPEN_USE_MIOPENGEMM
        break;
#endif

        case miopenConvolutionBwdDataAlgoFFT:
        {
            size_t workspace_fft = BackwardGetWorkSpaceSizeFFT(wDesc, dyDesc, dxDesc);
            if(workSpace != nullptr && workSpaceSize >= workspace_fft)
            {
                bool timed  = handle.IsProfilingEnabled();
                float timev = ExecuteBwdFFTKernel(
                    handle, dyDesc, dy, wDesc, w, dxDesc, dx, workSpace, workSpaceSize, timed);

                if(timed)
                {
                    handle.ResetKernelTime();
                    handle.AccumKernelTime(timev);
                }
            }
        }
        break;

        case miopenTransposeBwdDataAlgoGEMM: break;
        }
    }
    else if(mode == miopenTranspose)
    {
        if(dyDesc.GetLengths()[1] != wDesc.GetLengths()[1])
        {
            MIOPEN_THROW(miopenStatusBadParm);
        }

        int in_n, in_c, in_h, in_w;
        std::tie(in_n, in_c, in_h, in_w) = tien<4>(dxDesc.GetLengths());

        int wei_n, wei_h, wei_w;
        std::tie(std::ignore, wei_n, wei_h, wei_w) = tien<4>(wDesc.GetLengths());

        int out_h, out_w;
        std::tie(std::ignore, std::ignore, out_h, out_w) = tien<4>(dyDesc.GetLengths());

        if(wei_h != 1 || wei_w != 1 || u != 1 || v != 1)
        {
            assert(workSpace != nullptr &&
                   workSpaceSize >= ForwardGetWorkSpaceSizeGEMM(handle, wDesc, dxDesc));
        }

        std::string network_config;
#if MIOPEN_USE_MIOPENGEMM
        CreateGemmGeometryTranBwdData(dyDesc, wDesc, dxDesc, true, network_config);
        GemmGeometry gg = GetGemmGeometry(handle, "miopenTransposeBwdDataAlgoGEMM", network_config);

        float time_0 = 0;
        float t1     = 0;
        for(int i = 0; i < in_n; i++)
        {
            int in_offset = i * in_c * in_h * in_w;
            if(wei_h != 1 || wei_w != 1 || v != 1 || u != 1)
            {
                size_t out_offset = i * wei_n * out_h * out_w;
                Im2ColGPU(handle,
                          dyDesc.GetElementSize(),
                          dy,
                          out_offset,
                          wei_n,
                          out_h,
                          out_w,
                          wei_h,
                          wei_w,
                          in_h,
                          in_w,
                          pad_h,
                          pad_w,
                          u,
                          v,
                          dilation_h,
                          dilation_w,
                          workSpace);
                if(handle.IsProfilingEnabled())
                    t1 = handle.GetKernelTime();

                gg.RunGemm(handle, w, workSpace, dx, 0, 0, in_offset);

                // Update times for both the kernels
                if(handle.IsProfilingEnabled())
                {
                    if(i == in_n - 1)
                        handle.AccumKernelTime(t1 + time_0);
                    else
                        handle.AccumKernelTime(t1);
                    time_0 += handle.GetKernelTime();
                }
            }
            else if(wei_h == 1 && wei_w == 1 && v == 1 && u == 1)
            {
                int out_offset = i * wei_n * out_h * out_w;
                gg.RunGemm(handle, w, dy, dx, 0, out_offset, in_offset);
                if(handle.IsProfilingEnabled())
                {
                    if(i == in_n - 1)
                        handle.AccumKernelTime(time_0);
                    time_0 += handle.GetKernelTime();
                }
            }
        }
#else
        MIOPEN_THROW("GEMM is not supported");
#endif
    }
    if(miopen::CheckNumericsEnabled())
    {
        miopen::checkNumericsOutput(handle, dxDesc, dx);
    }
}

// ConvolutionBackwardWeightsGetWorkSpaceSize
// FindBackwardWeightsAlgorithm()
//
void ConvolutionDescriptor::FindConvBwdWeightsAlgorithm(Handle& handle,
                                                        const TensorDescriptor& dyDesc,
                                                        ConstData_t dy,
                                                        const TensorDescriptor& xDesc,
                                                        ConstData_t x,
                                                        const TensorDescriptor& dwDesc,
                                                        ConstData_t dw,
                                                        const int requestAlgoCount,
                                                        int* returnedAlgoCount,
                                                        miopenConvAlgoPerf_t* perfResults,
                                                        Data_t workSpace,
                                                        size_t workSpaceSize,
                                                        bool exhaustiveSearch) const
{

    if(x == nullptr || dw == nullptr || dy == nullptr)
        MIOPEN_THROW(miopenStatusBadParm, "Buffers cannot be NULL");
    if(returnedAlgoCount == nullptr)
        MIOPEN_THROW(miopenStatusBadParm, "returnedAlgoCount cannot be nullptr");
    if(perfResults == nullptr)
        MIOPEN_THROW(miopenStatusBadParm, "perfResults cannot be nullptr");
    if(requestAlgoCount < 1)
        MIOPEN_THROW(miopenStatusBadParm, "requestAlgoCount cannot be < 1");

    mlo_construct_direct2D find_params(0); // forward
    find_params.setOutputDescFromMLDesc(dyDesc);
    find_params.setInputDescFromMLDesc(xDesc);
    find_params.setWeightDescFromMLDesc(dwDesc);
    find_params.setConvDescr(pad_h, pad_w, u, v, dilation_h, dilation_w);
    std::string find_config;
    find_params.mloBuildConf_Key(find_config);

    std::unordered_map<std::string, int>::iterator iter = handle.bwdWeights_map.find(find_config);
    if (iter != handle.bwdWeights_map.end())
    {
	    if (requestAlgoCount == 1)
	    {
		    perfResults[0].bwd_weights_algo =
			    static_cast<miopenConvBwdWeightsAlgorithm_t>(iter->second);
		    *returnedAlgoCount = 1;
		    return;
	    }
    }

    // create a dummy buffer for use as output for the kernel calls
    // because kernels are called purely for timing purposes
    auto tmp_dw = handle.Create(dwDesc.GetElementSize() * GetTypeSize(dwDesc.GetType()));

    AutoEnableProfiling enableProfiling{handle};

    // < algorith_name, <time, workspace_size> >
    std::vector<PerfField> perf_db;

    // GEMM based
    int in_n, in_c, in_h, in_w;
    std::tie(in_n, in_c, in_h, in_w) = tien<4>(xDesc.GetLengths());

    int wei_n, wei_h, wei_w;

    int out_h, out_w;
    std::tie(std::ignore, std::ignore, out_h, out_w) = tien<4>(dyDesc.GetLengths());

    std::string network_config;
    size_t workspace_req = 0;

    if(mode == miopenTranspose)
    {
        std::tie(std::ignore, wei_n, wei_h, wei_w) = tien<4>(dwDesc.GetLengths());

#if MIOPEN_USE_MIOPENGEMM
        if(dyDesc.GetType() == miopenFloat)
        {
            GemmGeometry gg =
                CreateGemmGeometryConvBwdWeights(xDesc, dyDesc, dwDesc, false, network_config);
            workspace_req   = BackwardWeightsGetWorkSpaceSizeGEMM(handle, xDesc, dwDesc);
            float time_gemm = 0;

            // 1x1 does not require im2col or workspace
            if(wei_h == 1 && wei_w == 1 && v == 1 && u == 1)
            {
                gg.FindSolution(.003, handle, dy, x, tmp_dw.get(), false);
                gg.RunGemm(handle, dy, x, tmp_dw.get(), 0, 0, 0);

                time_gemm = in_n * handle.GetKernelTime();
                perf_db.push_back(PerfField{"miopenConvolutionBwdWeightsAlgoGEMM", time_gemm, 0});
            }
            // if not 1x1
            else if(workSpace != nullptr && workSpaceSize >= workspace_req)
            {
                float time_im2col = 0;
                size_t out_offset = 0;
                time_im2col       = Im2ColGPU(handle,
                                        dyDesc.GetElementSize(),
                                        dy,
                                        out_offset,
                                        wei_n,
                                        out_h,
                                        out_w,
                                        wei_h,
                                        wei_w,
                                        in_h,
                                        in_w,
                                        pad_h,
                                        pad_w,
                                        u,
                                        v,
                                        dilation_h,
                                        dilation_w,
                                        workSpace);

                gg.FindSolution(.003, handle, workSpace, x, tmp_dw.get(), false);
                gg.RunGemm(handle, workSpace, x, tmp_dw.get(), 0, 0, 0);
                time_gemm = in_n * (time_im2col + handle.GetKernelTime());
                perf_db.push_back(
                    PerfField{"miopenConvolutionBwdWeightsAlgoGEMM", time_gemm, workspace_req});
            }
        }
#else
        (void)workSpace;     // Suppress warning
        (void)workSpaceSize; // Suppress warning
#endif
    }
    else if(mode == miopenConvolution)
    {
        std::tie(wei_n, std::ignore, wei_h, wei_w) = tien<4>(dwDesc.GetLengths());

#if MIOPEN_USE_MIOPENGEMM
        if(dyDesc.GetType() == miopenFloat)
        {
            GemmGeometry gg =
                CreateGemmGeometryConvBwdWeights(dyDesc, xDesc, dwDesc, false, network_config);
            workspace_req   = BackwardWeightsGetWorkSpaceSizeGEMM(handle, dyDesc, dwDesc);
            float time_gemm = 0;

            // 1x1 does not require im2col or workspace
            if(wei_h == 1 && wei_w == 1 && v == 1 && u == 1)
            {
                gg.FindSolution(.003, handle, x, dy, tmp_dw.get(), false);
                gg.RunGemm(handle, x, dy, tmp_dw.get(), 0, 0, 0);

                time_gemm = in_n * handle.GetKernelTime();
                perf_db.push_back(PerfField{"miopenConvolutionBwdWeightsAlgoGEMM", time_gemm, 0});
            }
            // if not 1x1
            else if(workSpace != nullptr && workSpaceSize >= workspace_req)
            {
                float time_im2col = 0;
                size_t in_offset  = 0;
                time_im2col       = Im2ColGPU(handle,
                                        xDesc.GetElementSize(),
                                        x,
                                        in_offset,
                                        in_c,
                                        in_h,
                                        in_w,
                                        wei_h,
                                        wei_w,
                                        out_h,
                                        out_w,
                                        pad_h,
                                        pad_w,
                                        u,
                                        v,
                                        dilation_h,
                                        dilation_w,
                                        workSpace);

                gg.FindSolution(.003, handle, workSpace, dy, tmp_dw.get(), false);
                gg.RunGemm(handle, workSpace, dy, tmp_dw.get(), 0, 0, 0);
                time_gemm = in_n * (time_im2col + handle.GetKernelTime());
                perf_db.push_back(
                    PerfField{"miopenConvolutionBwdWeightsAlgoGEMM", time_gemm, workspace_req});
            }
        }
#else
        (void)workSpace;     // Suppress warning
        (void)workSpaceSize; // Suppress warning
#endif

        if(dilation_h == 1 && dilation_w == 1)
        {
            if(wei_w >= wei_h && !miopen::IsDisabled(MIOPEN_DEBUG_CONV_DIRECT{}) &&
               IsBwdWeightsDirectSupported(dwDesc))
            {
                mlo_construct_BwdWrW2D construct_params(0); // backward with regards to weights
                construct_params.doSearch(exhaustiveSearch);
                construct_params.setStream(&handle);
                construct_params.setOutputDescFromMLDesc(dyDesc);
                construct_params.setInputDescFromMLDesc(xDesc);
                construct_params.setWeightDescFromMLDesc(dwDesc);
                construct_params.setConvDescr(pad_h, pad_w, u, v, dilation_h, dilation_w);

                if(try_([&] { mloConstruct(construct_params); }, false) == miopenStatusSuccess)
                {
                    construct_params.mloBuildConf_Key(network_config);

                    visit_float(dyDesc.GetType(), [&](auto as_float) {

                        const std::vector<mlo_kernel_info>& bwd_wrw_info =
                            construct_params.getKernelsInfo();
                        /*
                         * get info for all kernels of the layer
                         * std::string _kernel_name;
                         * std::string _kernel_file;
                         * std::string _comp_options;
                         * std::vector<size_t> _g_wk;
                         * std::vector<size_t> _l_wk;
                         */

                        float time_direct = 0;
                        if(bwd_wrw_info.size() == 1)
                        {
                            const mlo_kernel_info& bwd_wrw = bwd_wrw_info[0];
                            auto kernel =
                                handle.AddKernel("miopenConvolutionBwdWeightsAlgoDirect_Main",
                                                 network_config,
                                                 std::get<1>(bwd_wrw),  // _kernel_file
                                                 std::get<0>(bwd_wrw),  // _kernel_name
                                                 std::get<4>(bwd_wrw),  // _l_wk
                                                 std::get<3>(bwd_wrw),  // _g_wk
                                                 std::get<2>(bwd_wrw)); // _comp_options

                            if((std::get<0>(bwd_wrw) == "gcnAsmConv3x3WrW") ||
                               (std::get<0>(bwd_wrw) == "gcnAsmConv1x1WrW"))
                            {
                                int unused       = 0;
                                int* return_addr = nullptr;
                                int N, C, H, W, K, n_groups;
                                construct_params.getCompiledInParameters(
                                    &N, &C, &H, &W, &K, &n_groups);
                                kernel(N,
                                       C,
                                       H,
                                       W,
                                       K,
                                       n_groups,
                                       unused,
                                       unused,
                                       x,
                                       tmp_dw.get(),
                                       dy,
                                       return_addr);
                            }
                            else
                            {
                                float padding_val = 0;
                                kernel(dy, x, tmp_dw.get(), as_float(padding_val));
                            }
                            time_direct = handle.GetKernelTime();
                            perf_db.push_back(
                                PerfField{"miopenConvolutionBwdWeightsAlgoDirect", time_direct, 0});
                        }
                        else
                        {
                            // this pointer needed here as a workaround in gcc 5
                            workspace_req = this->BackwardWeightsGetWorkSpaceSizeDirect(
                                handle, dyDesc, xDesc, dwDesc);

                            if(workSpace != nullptr && workSpaceSize >= workspace_req)
                            {
                                auto bwd_wrw = bwd_wrw_info[0];

                                // bwd stride 2
                                if(std::get<0>(bwd_wrw) == "SubSample")
                                {
                                    auto bwd_wrw_sub = bwd_wrw_info[0];
                                    // subsampling
                                    float time_sub = 0;
                                    time_sub       = SubSampleGPU(
                                        handle, bwd_wrw_sub, network_config, x, workSpace);
                                    time_direct += time_sub;

                                    // second kernel: wrw  kernel
                                    auto bwd_wrw_main = bwd_wrw_info[1];

                                    if((std::get<0>(bwd_wrw_main) == "gcnAsmConv1x1WrW"))
                                    {
                                        auto kernel = handle.AddKernel(
                                            "miopenConvolutionBwdWeightsAlgoDirect_Main",
                                            network_config,
                                            std::get<1>(bwd_wrw_main), // _kernel_file
                                            std::get<0>(bwd_wrw_main), // _kernel_name
                                            std::get<4>(bwd_wrw_main), // _l_wk
                                            std::get<3>(bwd_wrw_main), // _g_wk
                                            std::get<2>(bwd_wrw_main),
                                            1); // _comp_options

                                        int unused       = 0;
                                        int* return_addr = nullptr;
                                        int N, C, H, W, K, n_groups;
                                        construct_params.getCompiledInParameters(
                                            &N, &C, &H, &W, &K, &n_groups);
                                        kernel(N,
                                               C,
                                               H,
                                               W,
                                               K,
                                               n_groups,
                                               unused,
                                               unused,
                                               workSpace,
                                               tmp_dw.get(),
                                               dy,
                                               return_addr);
                                    }
                                    else
                                    {
                                        float padding_val = 0;

                                        handle.AddKernel(
                                            "miopenConvolutionBwdWeightsAlgoDirect_Main",
                                            network_config,
                                            std::get<1>(bwd_wrw_main),
                                            std::get<0>(bwd_wrw_main),
                                            std::get<4>(bwd_wrw_main),
                                            std::get<3>(bwd_wrw_main),
                                            std::get<2>(bwd_wrw_main),
                                            1)(dy, workSpace, tmp_dw.get(), as_float(padding_val));
                                    }
                                    time_direct += handle.GetKernelTime();
                                }
                                else
                                {
                                    auto bwd_wrw_main = bwd_wrw_info[0];

                                    float padding_val = 0;

                                    handle.AddKernel("miopenConvolutionBwdWeightsAlgoDirect_Main",
                                                     network_config,
                                                     std::get<1>(bwd_wrw_main),
                                                     std::get<0>(bwd_wrw_main),
                                                     std::get<4>(bwd_wrw_main),
                                                     std::get<3>(bwd_wrw_main),
                                                     std::get<2>(bwd_wrw_main))(
                                        dy, x, workSpace, as_float(padding_val));

                                    time_direct += handle.GetKernelTime();

                                    // second kernel: reduction  kernel
                                    auto bwd_wrw_red = bwd_wrw_info[1];

                                    handle.AddKernel("miopenConvolutionBwdWeightsAlgoDirect_Main",
                                                     network_config,
                                                     std::get<1>(bwd_wrw_red),
                                                     std::get<0>(bwd_wrw_red),
                                                     std::get<4>(bwd_wrw_red),
                                                     std::get<3>(bwd_wrw_red),
                                                     std::get<2>(bwd_wrw_red),
                                                     1)(workSpace, tmp_dw.get());

                                    time_direct += handle.GetKernelTime();
                                }
                                perf_db.push_back(PerfField{"miopenConvolutionBwdWeightsAlgoDirect",
                                                            time_direct,
                                                            workspace_req});
                            }
                        }
                    });
                }
            }
        }
    }

    if(perf_db.empty())
        MIOPEN_THROW("Bwd Weights Convolution cannot be executed due to incorrect params");

    // sort the perf_db
    std::sort(begin(perf_db), end(perf_db));

    // update perfResults
    *returnedAlgoCount = std::min(requestAlgoCount, static_cast<int>(perf_db.size()));

    for(int i = 0; i < *returnedAlgoCount; i++)
    {
        perfResults[i].bwd_weights_algo =
            static_cast<miopenConvBwdWeightsAlgorithm_t>(BwdWeightsAlgoResolver(perf_db[i].name));
        perfResults[i].time   = perf_db[i].time;
        perfResults[i].memory = perf_db[i].workspace;
    }

    handle.bwdWeights_map.insert(std::unordered_map<std::string, int>::value_type(find_config, BwdWeightsAlgoResolver(perf_db[0].name)));
}

// BackwardWeightsAlgorithm()
void ConvolutionDescriptor::ConvolutionBackwardWeights(Handle& handle,
                                                       const void* alpha,
                                                       const TensorDescriptor& dyDesc,
                                                       ConstData_t dy,
                                                       const TensorDescriptor& xDesc,
                                                       ConstData_t x,
                                                       miopenConvBwdWeightsAlgorithm_t algo,
                                                       const void* beta,
                                                       const TensorDescriptor& dwDesc,
                                                       Data_t dw,
                                                       Data_t workSpace,
                                                       size_t workSpaceSize) const
{
    (void)workSpaceSize; // Suppress warning

    if(x == nullptr || dw == nullptr || dy == nullptr)
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }
    if(dyDesc.GetSize() != dwDesc.GetSize() || dyDesc.GetSize() != xDesc.GetSize())
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }
    if(dyDesc.GetType() != dwDesc.GetType() || dyDesc.GetType() != xDesc.GetType())
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }
    if(dyDesc.GetLengths()[0] != xDesc.GetLengths()[0])
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }
    if(dyDesc.GetSize() < 3)
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }
    if(!float_equal(*(static_cast<const float*>(alpha)), 1.0) ||
       !float_equal(*(static_cast<const float*>(beta)), 0))
    {
        MIOPEN_THROW("Only alpha=1 and beta=0 is supported");
    }

    if(miopen::CheckNumericsEnabled())
    {
        miopen::checkNumericsInput(handle, dyDesc, dy);
        miopen::checkNumericsInput(handle, xDesc, x);
        if(!float_equal(*(static_cast<const float*>(beta)), 0))
        {
            miopen::checkNumericsInput(handle, dwDesc, dw);
        }
    }

    int in_n, in_c, in_h, in_w;
    std::tie(in_n, in_c, in_h, in_w) = tien<4>(xDesc.GetLengths());

    int wei_n, wei_h, wei_w;

    int out_h, out_w;
    std::tie(std::ignore, std::ignore, out_h, out_w) = tien<4>(dyDesc.GetLengths());

    if(mode == miopenConvolution)
    {
        std::tie(wei_n, std::ignore, wei_h, wei_w) = tien<4>(dwDesc.GetLengths());

        switch(algo)
        {
        case miopenConvolutionBwdWeightsAlgoGEMM: {

#if MIOPEN_USE_MIOPENGEMM
            // Zeroing out the output buffer
            float zero = 0.0f;
            SetTensor(handle, dwDesc, dw, &zero);

            std::string network_config;

            if(wei_h != 1 || wei_w != 1 || v != 1 || u != 1)
            {
                assert(workSpace != nullptr &&
                       workSpaceSize >=
                           BackwardWeightsGetWorkSpaceSizeGEMM(handle, dyDesc, dwDesc));
            }

            CreateGemmGeometryConvBwdWeights(dyDesc, xDesc, dwDesc, false, network_config);
            GemmGeometry gg =
                GetGemmGeometry(handle, "miopenConvolutionBwdWeightsAlgoGEMM", network_config);

            handle.ResetKernelTime();
            float time_0 = 0;
            float t1     = 0;
            for(int i = 0; i < in_n; i++)
            {
                int out_offset = i * wei_n * out_h * out_w;
                if(wei_h != 1 || wei_w != 1 || v != 1 || u != 1)
                {
                    size_t in_offset = i * in_c * in_h * in_w;
                    Im2ColGPU(handle,
                              xDesc.GetElementSize(),
                              x,
                              in_offset,
                              in_c,
                              in_h,
                              in_w,
                              wei_h,
                              wei_w,
                              out_h,
                              out_w,
                              pad_h,
                              pad_w,
                              u,
                              v,
                              dilation_h,
                              dilation_w,
                              workSpace);
                    if(handle.IsProfilingEnabled())
                        t1 = handle.GetKernelTime();

                    gg.RunGemm(handle, workSpace, dy, dw, 0, out_offset, 0);

                    // Update times for both the kernels
                    if(handle.IsProfilingEnabled())
                    {
                        if(i == in_n - 1)
                            handle.AccumKernelTime(t1 + time_0);
                        else
                            handle.AccumKernelTime(t1);
                        time_0 += handle.GetKernelTime();
                    }
                }
                else if(wei_h == 1 && wei_w == 1 && v == 1 && u == 1)
                {
                    int in_offset = i * in_c * in_h * in_w;
                    gg.RunGemm(handle, x, dy, dw, in_offset, out_offset, 0);

                    if(handle.IsProfilingEnabled())
                    {
                        if(i == in_n - 1)
                            handle.AccumKernelTime(time_0);
                        time_0 += handle.GetKernelTime();
                    }
                }
            }
#else
            MIOPEN_THROW("GEMM is not supported");
#endif
        }
#if MIOPEN_USE_MIOPENGEMM
        break;
#endif

        case miopenConvolutionBwdWeightsAlgoDirect:
        {
            if(wei_w >= wei_h)
            {
                mlo_construct_BwdWrW2D construct_params(0); // backward with regards to weights
                construct_params.setStream(&handle);
                construct_params.setOutputDescFromMLDesc(dyDesc);
                construct_params.setInputDescFromMLDesc(xDesc);
                construct_params.setWeightDescFromMLDesc(dwDesc);
                construct_params.setConvDescr(pad_h, pad_w, u, v, dilation_h, dilation_w);

                visit_float(dyDesc.GetType(), [&](auto as_float) {

                    std::string network_config;
                    construct_params.mloBuildConf_Key(network_config);

                    auto&& kernels = handle.GetKernels("miopenConvolutionBwdWeightsAlgoDirect_Main",
                                                       network_config);
                    const auto num_kernels = kernels.size();
                    auto p_kernel          = std::begin(kernels);
                    auto kernel            = *p_kernel;

                    handle.ResetKernelTime();

                    if((kernel.GetName() == "gcnAsmConv3x3WrW") ||
                       (kernel.GetName() == "gcnAsmConv1x1WrW"))
                    {
                        int unused       = 0;
                        int* return_addr = nullptr;
                        int N, C, H, W, K, n_groups;
                        construct_params.getCompiledInParameters(&N, &C, &H, &W, &K, &n_groups);
                        kernel(N, C, H, W, K, n_groups, unused, unused, x, dw, dy, return_addr);
                    }
                    else if(num_kernels == 1)
                    {
                        float padding_val = 0;
                        kernel(dy, x, dw, as_float(padding_val));
                    }
                    else
                    {

                        // this pointer needed here as a workaround in gcc 5
                        assert(workSpace != nullptr &&
                               workSpaceSize >= this->BackwardWeightsGetWorkSpaceSizeDirect(
                                                    handle, dyDesc, xDesc, dwDesc));

                        if(kernel.GetName() == "SubSample")
                        {
                            // subsampling kernel
                            kernel(x, workSpace);
                            float time0 = handle.GetKernelTime();

                            // wrw  kernel
                            auto kernel2 = *(p_kernel + 1);
                            if(kernel2.GetName() == "gcnAsmConv1x1WrW")
                            {
                                int unused       = 0;
                                int* return_addr = nullptr;
                                int N, C, H, W, K, n_groups;
                                // H/W are image size after downsampling, parsed from img_h/img_w in
                                // conv_asm_dir_BwdWrW1x1.cpp
                                construct_params.getCompiledInParameters(
                                    &N, &C, &H, &W, &K, &n_groups);
                                kernel2(N,
                                        C,
                                        H,
                                        W,
                                        K,
                                        n_groups,
                                        unused,
                                        unused,
                                        workSpace,
                                        dw,
                                        dy,
                                        return_addr);
                            }
                            else
                            {
                                float padding_val = 0;
                                kernel2(dy, workSpace, dw, as_float(padding_val));
                            }

                            handle.AccumKernelTime(time0);
                        }
                        else
                        {
                            float padding_val = 0;
                            kernel(dy, x, workSpace, as_float(padding_val));

                            float time0 = handle.GetKernelTime();
                            // second kernel has
                            auto kernel2 = *(p_kernel + 1);
                            // reduction  kernel
                            kernel2(workSpace, dw);

                            handle.AccumKernelTime(time0);
                        }
                    }
                });
            }
        }
        break;
        };
    }
    else if(mode == miopenTranspose)
    {
        std::tie(std::ignore, wei_n, wei_h, wei_w) = tien<4>(dwDesc.GetLengths());

        std::string network_config;

        if(wei_h != 1 || wei_w != 1 || v != 1 || u != 1)
        {
            assert(workSpace != nullptr &&
                   workSpaceSize >= BackwardWeightsGetWorkSpaceSizeGEMM(handle, xDesc, dwDesc));
        }

#if MIOPEN_USE_MIOPENGEMM
        CreateGemmGeometryConvBwdWeights(xDesc, dyDesc, dwDesc, false, network_config);
        GemmGeometry gg =
            GetGemmGeometry(handle, "miopenConvolutionBwdWeightsAlgoGEMM", network_config);

        handle.ResetKernelTime();
        float time_0 = 0;
        float t1     = 0;
        for(int i = 0; i < in_n; i++)
        {
            int in_offset = i * in_c * in_h * in_w;
            if(wei_h != 1 || wei_w != 1 || v != 1 || u != 1)
            {
                size_t out_offset = i * wei_n * out_h * out_w;
                Im2ColGPU(handle,
                          dyDesc.GetElementSize(),
                          dy,
                          out_offset,
                          wei_n,
                          out_h,
                          out_w,
                          wei_h,
                          wei_w,
                          in_h,
                          in_w,
                          pad_h,
                          pad_w,
                          u,
                          v,
                          dilation_h,
                          dilation_w,
                          workSpace);

                if(handle.IsProfilingEnabled())
                    t1 = handle.GetKernelTime();

                gg.RunGemm(handle, workSpace, x, dw, 0, in_offset, 0);

                // Update times for both the kernels
                if(handle.IsProfilingEnabled())
                {
                    if(i == in_n - 1)
                        handle.AccumKernelTime(t1 + time_0);
                    else
                        handle.AccumKernelTime(t1);
                    time_0 += handle.GetKernelTime();
                }
            }
            else if(wei_h == 1 && wei_w == 1 && v == 1 && u == 1)
            {
                int out_offset = i * wei_n * out_h * out_w;
                gg.RunGemm(handle, dy, x, dw, out_offset, in_offset, 0);

                if(handle.IsProfilingEnabled())
                {
                    if(i == in_n - 1)
                        handle.AccumKernelTime(time_0);
                    time_0 += handle.GetKernelTime();
                }
            }
        }
#else
        MIOPEN_THROW("GEMM is not supported");
#endif
    }

    if(miopen::CheckNumericsEnabled())
    {
        miopen::checkNumericsOutput(handle, dwDesc, dw);
    }
}

void ConvolutionBackwardBias(Handle& handle,
                             const void* alpha,
                             const TensorDescriptor& dyDesc,
                             ConstData_t dy,
                             const void* beta,
                             const TensorDescriptor& dbDesc,
                             Data_t db)
{
    if(dy == nullptr || db == nullptr)
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }
    if(dyDesc.GetLengths()[1] != dbDesc.GetLengths()[1])
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }
    if(!float_equal(*(static_cast<const float*>(alpha)), 1.0) ||
       !float_equal(*(static_cast<const float*>(beta)), 0))
    {
        MIOPEN_THROW("Only alpha=1 and beta=0 is supported");
    }
    if(miopen::CheckNumericsEnabled())
    {
        miopen::checkNumericsInput(handle, dyDesc, dy);
    }

    int out_n, out_c, out_h, out_w, stride_n, stride_c, stride_h, stride_w;
    std::tie(out_n, out_c, out_h, out_w)             = tien<4>(dyDesc.GetLengths());
    std::tie(stride_n, stride_c, stride_h, stride_w) = tien<4>(dyDesc.GetStrides());
    std::string program_name = "MIOpenConvBwdBias.cl";
    std::string kernel_name  = "MIOpenConvBwdB";

    std::string params;
    size_t lcl_grp_size0 = 256;
    size_t lcl_grp_size1 = 1;
    size_t local_mem_sz  = 256;

    size_t map_size         = out_w * out_h;
    size_t read_unit        = 4;
    size_t map_size_aligned = (map_size + (read_unit - 1)) / read_unit;
    size_t off_pix          = map_size - (map_size / read_unit) * read_unit;

    params = " -DMLO_CONVBWD_GROUP_SZ0=" + std::to_string(lcl_grp_size0);
    params += " -DMLO_CONVBWD_GROUP_SZ1=" + std::to_string(lcl_grp_size1);
    params += " -DMLO_CONVBWDB_LCL_MEMSZ=" + std::to_string(local_mem_sz);
    params += " -DMLO_CONVBWDB_UNITSIZE=" + std::to_string(read_unit);
    params += " -DMLO_OUT_WIDTH=" + std::to_string(out_w);
    params += " -DMLO_OUT_HEIGHT=" + std::to_string(out_h);
    params += " -DMLO_OUT_BATCH_SZ=" + std::to_string(out_n);
    params += " -DMLO_OUT_CHANNEL_STRIDE=" + std::to_string(stride_c);
    params += " -DMLO_OUT_BATCH_STRIDE=" + std::to_string(stride_n);
    params += " -DMLO_WK_SIZE=" + std::to_string(map_size_aligned);
    params += " -DMLO_N_PIX_OFF=" + std::to_string(off_pix);
    if(dyDesc.GetType() == miopenFloat)
    {
        params += " -DMIOPEN_USE_FP16=0 ";
        params += " -DMIOPEN_USE_FP32=1 ";
    }
    else if(dyDesc.GetType() == miopenHalf)
    {
        params += " -DMIOPEN_USE_FP16=1 ";
        params += " -DMIOPEN_USE_FP32=0 ";
    }

    const std::vector<size_t> vld = {lcl_grp_size0, size_t{1}, size_t{1}};
    const std::vector<size_t> vgd = {lcl_grp_size0, static_cast<size_t>(out_c), size_t{1}};

    handle.AddKernel("miopenConvolutionBwdBias", "", program_name, kernel_name, vld, vgd, params)(
        dy, db);

    if(miopen::CheckNumericsEnabled())
    {
        miopen::checkNumericsOutput(handle, dbDesc, db);
    }
}

} // namespace miopen
