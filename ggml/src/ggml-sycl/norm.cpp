#include "norm.hpp"
#include "ggml-sycl/common.hpp"
#include "ggml-sycl/presets.hpp"

static void norm_f32(const float* x, float* dst, const int ncols, const int64_t stride_row, const int64_t stride_channel,
        const int64_t stride_sample, const float eps, const sycl::nd_item<3>& item_ct1, sycl::float2* s_sum, int block_size) {

    const int nrows = item_ct1.get_group_range(2);
    const int nchannels = item_ct1.get_group_range(1);

    const int nthreads = item_ct1.get_local_range(2);
    const int sample  = item_ct1.get_group(0);
    const int channel = item_ct1.get_group(1);
    const int row     = item_ct1.get_group(2);

    const int tid = item_ct1.get_local_id(2);
    const int nwarps = nthreads / WARP_SIZE;

    const auto strided_offset = calculate_offset<3>({stride_sample, stride_channel, stride_row}, {sample, channel, row});
    const auto packed_offset = calculate_offset<3>({nchannels * nrows * ncols, nrows * ncols, ncols}, {sample, channel, row});

    x += strided_offset;
    dst += packed_offset;

    sycl::float2 mean_var = sycl::float2(0.f, 0.f);

    for (int col = tid; col < ncols; col += block_size) {
        const float xi = x[col];
        mean_var.x() += xi;
        mean_var.y() += xi * xi;
    }

    // sum up partial sums
    mean_var = warp_reduce_sum(mean_var, item_ct1);
    if  (block_size > WARP_SIZE) {
        const auto sub_group = item_ct1.get_sub_group();
        const auto sg_id = sub_group.get_group_linear_id();
        const auto wi_in_sg = sub_group.get_local_linear_id();
        if (wi_in_sg == 0) {
            s_sum[sg_id] = mean_var;
        }
        item_ct1.barrier(sycl::access::fence_space::local_space);
        mean_var = 0.f;
        const size_t nreduce = ceil_div(nwarps, WARP_SIZE);
        for (size_t i = 0; i < nreduce; i += 1)
        {
            mean_var += s_sum[wi_in_sg + i * WARP_SIZE];
        }
        mean_var = warp_reduce_sum(mean_var, item_ct1);
    }

    const float mean = mean_var.x() / ncols;
    const float var = mean_var.y() / ncols - mean * mean;
    const float inv_std = sycl::rsqrt(var + eps);

    for (int col = tid; col < ncols; col += block_size) {
        dst[col] = (x[col] - mean) * inv_std;
    }
}

static void group_norm_f32(const float* x, float* dst, const int group_size, const int ne_elements, const float eps,
    const sycl::nd_item<3>& item_ct1, float* s_sum, int block_size) {
    int start = item_ct1.get_group(2) * group_size;
    int end = start + group_size;
    const int nthreads = item_ct1.get_local_range(2);
    const int nwarps = nthreads / WARP_SIZE;
    start += item_ct1.get_local_id(2);
    size_t nreduce = nwarps / WARP_SIZE;

    if (end >= ne_elements) {
        end = ne_elements;
    }

    float tmp = 0.0f; // partial sum for thread in warp

    for (int j = start; j < end; j += block_size) {
        tmp += x[j];
    }

    tmp = warp_reduce_sum(tmp, item_ct1);
    if (block_size > WARP_SIZE) {

        int warp_id = item_ct1.get_local_id(2) / WARP_SIZE;
        int lane_id = item_ct1.get_local_id(2) % WARP_SIZE;
        if (lane_id == 0) {
            s_sum[warp_id] = tmp;
        }
        /*
        DPCT1118:1: SYCL group functions and algorithms must be encountered in
        converged control flow. You may need to adjust the code.
        */
        /*
        DPCT1065:54: Consider replacing sycl::nd_item::barrier() with
        sycl::nd_item::barrier(sycl::access::fence_space::local_space) for
        better performance if there is no access to global memory.
        */
        item_ct1.barrier();
        tmp = 0.f;
        for (size_t i = 0; i < nreduce; i += 1)
        {
            tmp += s_sum[lane_id + i * WARP_SIZE];
        }
        tmp = warp_reduce_sum(tmp, item_ct1);
    }

    float mean = tmp / group_size;
    tmp = 0.0f;

    for (int j = start; j < end; j += block_size) {
        float xi = x[j] - mean;
        dst[j] = xi;
        tmp += xi * xi;
    }

    tmp = warp_reduce_sum(tmp, item_ct1);
    if (block_size > WARP_SIZE) {

        int warp_id = item_ct1.get_local_id(2) / WARP_SIZE;
        int lane_id = item_ct1.get_local_id(2) % WARP_SIZE;
        if (lane_id == 0) {
            s_sum[warp_id] = tmp;
        }
        /*
        DPCT1118:2: SYCL group functions and algorithms must be encountered in
        converged control flow. You may need to adjust the code.
        */
        /*
        DPCT1065:55: Consider replacing sycl::nd_item::barrier() with
        sycl::nd_item::barrier(sycl::access::fence_space::local_space) for
        better performance if there is no access to global memory.
        */
        item_ct1.barrier();
        tmp = 0.f;
        for (size_t i = 0; i < nreduce; i += 1)
        {
            tmp += s_sum[lane_id + i * WARP_SIZE];
        }
        tmp = warp_reduce_sum(tmp, item_ct1);
    }

    float variance = tmp / group_size;
    float scale = sycl::rsqrt(variance + eps);
    for (int j = start; j < end; j += block_size) {
        dst[j] *= scale;
    }
}

static void rms_norm_f32(const float* x, float* dst, const int ncols, const int64_t stride_row, const int64_t stride_channel,
        const int64_t stride_sample, const float eps, const sycl::nd_item<3>& item_ct1, float* s_sum, int block_size) {

    const int nrows = item_ct1.get_group_range(2);
    const int nchannels = item_ct1.get_group_range(1);

    const int sample  = item_ct1.get_group(0);
    const int channel = item_ct1.get_group(1);
    const int row     = item_ct1.get_group(2);

    const int nthreads = item_ct1.get_local_range(2);

    const int tid = item_ct1.get_local_id(2);
    const int nwarps = nthreads / WARP_SIZE;

    const auto strided_offset = calculate_offset<3>({stride_sample, stride_channel, stride_row}, {sample, channel, row});
    const auto packed_offset = calculate_offset<3>({nchannels * nrows * ncols, nrows * ncols, ncols}, {sample, channel, row});

    x   += strided_offset;
    dst += packed_offset;


    float tmp = 0.0f; // partial sum for thread in warp

    for (int col = tid; col < ncols; col += block_size) {
        const float xi = x[col];
        tmp += xi * xi;
    }

    // sum up partial sums
    tmp = warp_reduce_sum(tmp, item_ct1);
    if (block_size > WARP_SIZE) {
        const auto sub_group = item_ct1.get_sub_group();
        const auto sg_id = sub_group.get_group_linear_id();
        const auto wi_in_sg = sub_group.get_local_linear_id();
        if (wi_in_sg == 0) {
            s_sum[sg_id] = tmp;
        }

        item_ct1.barrier(sycl::access::fence_space::local_space);
        const size_t nreduce = ceil_div(nwarps, WARP_SIZE);
        tmp = 0.f;
        for (size_t i = 0; i < nreduce; i += 1)
        {
            tmp += s_sum[wi_in_sg + i * WARP_SIZE];
        }
        tmp = warp_reduce_sum(tmp, item_ct1);
    }

    const float mean = tmp / ncols;
    const float scale = sycl::rsqrt(mean + eps);

    for (int col = tid; col < ncols; col += block_size) {
        dst[col] = scale * x[col];
    }
}

static void l2_norm_f32(const float* x, float* dst, const int ncols, const float eps,
    const sycl::nd_item<3>& item_ct1, float* s_sum, int block_size) {
    const int row = item_ct1.get_group(2) * item_ct1.get_local_range(1) +
        item_ct1.get_local_id(1);
    const int tid = item_ct1.get_local_id(2);
    const int nthreads = item_ct1.get_local_range(2);
    const int nwarps = nthreads / WARP_SIZE;
    float tmp = 0.0f; // partial sum for thread in warp

    for (int col = tid; col < ncols; col += block_size) {
        const float xi = x[row * ncols + col];
        tmp += xi * xi;
    }

    // sum up partial sums
    tmp = warp_reduce_sum(tmp, item_ct1);
    if (block_size > WARP_SIZE) {

        int warp_id = item_ct1.get_local_id(2) / WARP_SIZE;
        int lane_id = item_ct1.get_local_id(2) % WARP_SIZE;
        if (lane_id == 0) {
            s_sum[warp_id] = tmp;
        }
        /*
        DPCT1118:3: SYCL group functions and algorithms must be encountered in
        converged control flow. You may need to adjust the code.
        */
        item_ct1.barrier(sycl::access::fence_space::local_space);
        size_t nreduce = nwarps / WARP_SIZE;
        tmp = 0.f;
        for (size_t i = 0; i < nreduce; i += 1)
        {
            tmp += s_sum[lane_id + i * WARP_SIZE];
        }
        tmp = warp_reduce_sum(tmp, item_ct1);
    }

    const float scale = sycl::rsqrt(sycl::max(tmp, eps * eps));

    for (int col = tid; col < ncols; col += block_size) {
        dst[row * ncols + col] = scale * x[row * ncols + col];
    }
}

static void norm_f32_sycl(const float * x, float * dst, const int ncols, const int nrows, const int nchannels, const int nsamples,
        const int64_t stride_row, const int64_t stride_channel, const int64_t stride_sample,
        const float eps, queue_ptr stream, int device) {

    const sycl::range<3> global_dims(nsamples, nchannels, nrows);
    GGML_ASSERT(ncols % WARP_SIZE == 0);
    if (ncols < 1024) {
        const sycl::range<3> block_dims(1, 1, WARP_SIZE);
        sycl_launch(stream, [&](sycl::handler & cgh) {
            sycl_parallel_for(cgh, sycl::nd_range<3>(global_dims * block_dims, block_dims),
                              [=](sycl::nd_item<3> item_ct1) [[sycl::reqd_sub_group_size(WARP_SIZE)]] {
                                  norm_f32(x, dst, ncols, stride_row, stride_channel, stride_sample, eps, item_ct1,
                                           nullptr, WARP_SIZE);
                              });
        });
    }
    else {
        const int work_group_size = ggml_sycl_info().max_work_group_sizes[device];
        assert(work_group_size % (WARP_SIZE * WARP_SIZE) == 0);
        const sycl::range<3> block_dims(1, 1, work_group_size);
        /*
        DPCT1049:17: The work-group size passed to the SYCL kernel may exceed
        the limit. To get the device limit, query
        info::device::max_work_group_size. Adjust the work-group size if needed.
        */
        sycl_launch(stream, [&](sycl::handler & cgh) {
            sycl::local_accessor<sycl::float2, 1> s_sum_acc_ct1(
                            sycl::range<1>(work_group_size / WARP_SIZE), cgh);
            sycl_parallel_for(cgh, sycl::nd_range<3>(global_dims * block_dims, block_dims),
                              [=](sycl::nd_item<3> item_ct1) [[sycl::reqd_sub_group_size(WARP_SIZE)]] {
                                  norm_f32(x, dst, ncols, stride_row, stride_channel, stride_sample, eps, item_ct1,
                                           get_pointer(s_sum_acc_ct1), work_group_size);
                              });
        });
    }
}

static void group_norm_f32_sycl(const float* x, float* dst,
    const int num_groups, const float eps, const int group_size,
    const int ne_elements, queue_ptr stream, int device) {
    if (group_size < 1024) {
        const sycl::range<3> block_dims(1, 1, WARP_SIZE);
        sycl_launch(stream, [&](sycl::handler & cgh) {
            const float eps_ct4 = eps;
            sycl_parallel_for(cgh, sycl::nd_range<3>(sycl::range<3>(1, 1, num_groups) * block_dims, block_dims),
                              [=](sycl::nd_item<3> item_ct1) [[sycl::reqd_sub_group_size(WARP_SIZE)]] {
                                  group_norm_f32(x, dst, group_size, ne_elements, eps_ct4, item_ct1, nullptr,
                                                 WARP_SIZE);
                              });
        });
    }
    else {
        const int work_group_size = ggml_sycl_info().max_work_group_sizes[device];
        assert(work_group_size % (WARP_SIZE * WARP_SIZE) == 0);
        const sycl::range<3> block_dims(1, 1, work_group_size);
        /*
        DPCT1049:18: The work-group size passed to the SYCL kernel may exceed
        the limit. To get the device limit, query
        info::device::max_work_group_size. Adjust the work-group size if needed.
        */

        sycl_launch(stream, [&](sycl::handler & cgh) {
            sycl::local_accessor<float, 1> s_sum_acc_ct1(sycl::range<1>(work_group_size / WARP_SIZE),
                cgh);

            const float eps_ct4 = eps;

            sycl_parallel_for(cgh, sycl::nd_range<3>(sycl::range<3>(1, 1, num_groups) * block_dims, block_dims),
                              [=](sycl::nd_item<3> item_ct1) [[sycl::reqd_sub_group_size(WARP_SIZE)]] {
                                  group_norm_f32(x, dst, group_size, ne_elements, eps_ct4, item_ct1,
                                                 get_pointer(s_sum_acc_ct1), work_group_size);
                              });
        });
    }
}

static void rms_norm_f32_sycl(const float* x, float* dst, const int ncols, const int nrows, const int nchannels, const int nsamples,
        const int64_t stride_row, const int64_t stride_channel, const int64_t stride_sample, const float eps, queue_ptr stream, int device) {
    GGML_ASSERT(ncols % WARP_SIZE == 0);
    // printf("%s ncols=%d, nrows=%d, WARP_SIZE=%d\n", __func__, ncols, nrows, WARP_SIZE);

    const sycl::range<3> global_dims(nsamples, nchannels, nrows);
    if (ncols < 1024) {
        const sycl::range<3> block_dims(1, 1, WARP_SIZE);
        sycl_launch(stream, [&](sycl::handler & cgh) {
            sycl_parallel_for(cgh, sycl::nd_range<3>(global_dims * block_dims, block_dims),
                              [=](sycl::nd_item<3> item_ct1) [[sycl::reqd_sub_group_size(WARP_SIZE)]] {
                                  rms_norm_f32(x, dst, ncols, stride_row, stride_channel, stride_sample, eps, item_ct1,
                                               nullptr, WARP_SIZE);
                              });
        });
    }
    else {
        const int work_group_size = ggml_sycl_info().max_work_group_sizes[device];
        assert(work_group_size % (WARP_SIZE * WARP_SIZE) == 0);
        const sycl::range<3> block_dims(1, 1, work_group_size);
        /*
        DPCT1049:19: The work-group size passed to the SYCL kernel may exceed
        the limit. To get the device limit, query
        info::device::max_work_group_size. Adjust the work-group size if needed.
        */
        sycl_launch(stream, [&](sycl::handler & cgh) {
            sycl::local_accessor<float, 1> s_sum_acc_ct1(sycl::range<1>(work_group_size / WARP_SIZE),
                cgh);
            sycl_parallel_for(cgh, sycl::nd_range<3>(global_dims * block_dims, block_dims),
                              [=](sycl::nd_item<3> item_ct1) [[sycl::reqd_sub_group_size(WARP_SIZE)]] {
                                  rms_norm_f32(x, dst, ncols, stride_row, stride_channel, stride_sample, eps, item_ct1,
                                               get_pointer(s_sum_acc_ct1), work_group_size);
                              });
        });
    }
}

static void l2_norm_f32_sycl(const float* x, float* dst, const int ncols,
    const int nrows, const float eps,
    queue_ptr stream, int device) {
    GGML_ASSERT(ncols % WARP_SIZE == 0);
    // printf("%s ncols=%d, nrows=%d, WARP_SIZE=%d\n", __func__, ncols, nrows, WARP_SIZE);
    if (ncols < 1024) {
        const sycl::range<3> block_dims(1, 1, WARP_SIZE);
        sycl_launch(stream, [&](sycl::handler & cgh) {
            sycl_parallel_for(cgh, sycl::nd_range<3>(sycl::range<3>(1, 1, nrows) * block_dims, block_dims),
                              [=](sycl::nd_item<3> item_ct1) [[sycl::reqd_sub_group_size(WARP_SIZE)]] {
                                  l2_norm_f32(x, dst, ncols, eps, item_ct1, nullptr, WARP_SIZE);
                              });
        });
    }
    else {
        const int work_group_size = ggml_sycl_info().max_work_group_sizes[device];
        assert(work_group_size % (WARP_SIZE * WARP_SIZE) == 0);
        const sycl::range<3> block_dims(1, 1, work_group_size);
        /*
        DPCT1049:19: The work-group size passed to the SYCL kernel may exceed
        the limit. To get the device limit, query
        info::device::max_work_group_size. Adjust the work-group size if needed.
        */
        sycl_launch(stream, [&](sycl::handler & cgh) {
            sycl::local_accessor<float, 1> s_sum_acc_ct1(sycl::range<1>(work_group_size / WARP_SIZE),
                cgh);
            sycl_parallel_for(cgh, sycl::nd_range<3>(sycl::range<3>(1, 1, nrows) * block_dims, block_dims),
                              [=](sycl::nd_item<3> item_ct1) [[sycl::reqd_sub_group_size(WARP_SIZE)]] {
                                  l2_norm_f32(x, dst, ncols, eps, item_ct1, get_pointer(s_sum_acc_ct1),
                                              work_group_size);
                              });
        });
    }
}

void ggml_sycl_op_norm(ggml_backend_sycl_context& ctx, ggml_tensor* dst) {
    const ggml_tensor * src0 = dst->src[0];

    GGML_ASSERT(dst->src[0]->type == GGML_TYPE_F32);
    GGML_ASSERT(dst->type == GGML_TYPE_F32);

    GGML_TENSOR_UNARY_OP_LOCALS
    dpct::queue_ptr main_stream = ctx.stream();
    SYCL_CHECK(ggml_sycl_set_device(ctx.device));
    const float * src0_dd = static_cast<const float *>(dst->src[0]->data);
    float *       dst_dd  = static_cast<float *>(dst->data);

    float eps;
    memcpy(&eps, dst->op_params, sizeof(float));
    GGML_ASSERT(eps >= 0.0f);
    const size_t ts0 = ggml_type_size(src0->type);
    GGML_ASSERT(nb00 == ts0);
    const int64_t s01 = nb01 / ts0;
    const int64_t s02 = nb02 / ts0;
    const int64_t s03 = nb03 / ts0;

    norm_f32_sycl(src0_dd, dst_dd, ne00, ne01, ne02, ne03, s01, s02, s03, eps, main_stream, ctx.device);
}

void ggml_sycl_op_group_norm(ggml_backend_sycl_context& ctx, ggml_tensor* dst) {

    GGML_ASSERT(dst->src[0]->type == GGML_TYPE_F32);
    GGML_ASSERT(dst->type == GGML_TYPE_F32);

    int num_groups = dst->op_params[0];
    dpct::queue_ptr main_stream = ctx.stream();
    SYCL_CHECK(ggml_sycl_set_device(ctx.device));

    const float * src0_dd = static_cast<const float *>(dst->src[0]->data);
    float *       dst_dd  = static_cast<float *>(dst->data);

    float eps;
    memcpy(&eps, dst->op_params + 1, sizeof(float));

    int group_size = dst->src[0]->ne[0] * dst->src[0]->ne[1] * ((dst->src[0]->ne[2] + num_groups - 1) / num_groups);
    group_norm_f32_sycl(src0_dd, dst_dd, num_groups, eps, group_size, dst->src[0]->ne[0] * dst->src[0]->ne[1] * dst->src[0]->ne[2], main_stream, ctx.device);
}

void ggml_sycl_op_rms_norm(ggml_backend_sycl_context & ctx, ggml_tensor * dst) {

    const ggml_tensor * src0 = dst->src[0];
    GGML_ASSERT(dst->src[0]->type == GGML_TYPE_F32);
    GGML_ASSERT(dst->type == GGML_TYPE_F32);

    dpct::queue_ptr main_stream = ctx.stream();
    SYCL_CHECK(ggml_sycl_set_device(ctx.device));

    const float * src0_dd = static_cast<const float *>(dst->src[0]->data);
    float *       dst_dd  = static_cast<float *>(dst->data);

    float eps;
    memcpy(&eps, dst->op_params, sizeof(float));

    GGML_TENSOR_UNARY_OP_LOCALS
    const size_t ts0 = ggml_type_size(src0->type);
    GGML_ASSERT(nb00 == ts0);
    const int64_t s01 = nb01 / ts0;
    const int64_t s02 = nb02 / ts0;
    const int64_t s03 = nb03 / ts0;
    rms_norm_f32_sycl(src0_dd, dst_dd, ne00, ne01, ne02, ne03, s01, s02, s03, eps, main_stream, ctx.device);
}

void ggml_sycl_op_l2_norm(ggml_backend_sycl_context& ctx, ggml_tensor* dst) {

    GGML_ASSERT(dst->src[0]->type == GGML_TYPE_F32);
    GGML_ASSERT(dst->type == GGML_TYPE_F32);

    dpct::queue_ptr main_stream = ctx.stream();
    SYCL_CHECK(ggml_sycl_set_device(ctx.device));

    const int64_t ne00 = dst->src[0]->ne[0];
    const int64_t nrows = ggml_nrows(dst->src[0]);
    const float * src0_dd = static_cast<const float *>(dst->src[0]->data);
    float * dst_dd = static_cast<float *>(dst->data);

    float eps;
    memcpy(&eps, dst->op_params, sizeof(float));

    l2_norm_f32_sycl(src0_dd, dst_dd, ne00, nrows, eps, main_stream, ctx.device);

}
