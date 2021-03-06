/*!
 * Copyright (c) 2017 by Contributors
 * \file roi_align.cc
 * \brief roi align operator
 * \author Xilai Li
*/
#include "./roi_align-inl.h"
#include <mshadow/base.h>
#include <mshadow/tensor.h>
#include <mshadow/packet-inl.h>
#include <mshadow/dot_engine-inl.h>
#include <cassert>
#include <iostream>

using std::max;
using std::min;
using std::floor;
using std::ceil;
//using std::cout;
//using std::endl;

namespace mshadow {
template<typename Dtype>
inline void ROIAlignForward(const Tensor<cpu, 4, Dtype> &out,
                           const Tensor<cpu, 4, Dtype> &data,
                           const Tensor<cpu, 2, Dtype> &bbox,
                           const Tensor<cpu, 4, Dtype> &max_idx,
                           const float spatial_scale_) {
  // cout << "Using .cc operator" << endl;
  const Dtype *bottom_data = data.dptr_;
  const Dtype *bottom_rois = bbox.dptr_;
  Dtype *top_data = out.dptr_;
  Dtype *argmax_data = max_idx.dptr_;
  const int channels_ = data.size(1);
  const int height_ = data.size(2);
  const int width_ = data.size(3);
  const int pooled_height_ = out.size(2);
  const int pooled_width_ = out.size(3);
  //cout << "pooled_size: " << pooled_height_ << " " << pooled_width_ << endl;

  const int num_rois = bbox.size(0);
  const int batch_size = data.size(0);
  const int data_size = data.size(1) * data.size(2) * data.size(3);
  // For each ROI R = [batch_index x1 y1 x2 y2]: max pool over R
  for (int n = 0; n < num_rois; ++n) {
    int roi_batch_ind = bottom_rois[0];
    float roi_start_w = bottom_rois[1] * spatial_scale_;
    float roi_start_h = bottom_rois[2] * spatial_scale_;
    float roi_end_w = bottom_rois[3] * spatial_scale_;
    float roi_end_h = bottom_rois[4] * spatial_scale_;
    //cout << "roi " << roi_start_w << " " << roi_start_h << " " << roi_end_w << " " << roi_end_h << endl;
    assert(roi_batch_ind >= 0);
    assert(roi_batch_ind < batch_size);

    // force malformed ROIs to be 1 * 1
    float roi_height = fmaxf(roi_end_h - roi_start_h, 0);
    //float roi_height = fmaxf(roi_end_h - roi_start_h + 1, 0);
    float roi_width = fmaxf(roi_end_w - roi_start_w, 0);
    //float roi_width = fmaxf(roi_end_w - roi_start_w + 1, 0);
    float bin_size_h = roi_height / (pooled_height_ - 1);
    float bin_size_w = roi_width / (pooled_width_ - 1);
    //cout << "bin size: " << bin_size_h << " " << bin_size_w << endl;

    const Dtype* batch_data = bottom_data + data_size * roi_batch_ind;

    for (int c = 0; c < channels_; ++c) {
      for (int ph = 0; ph < pooled_height_; ++ph) {
        for (int pw = 0; pw < pooled_width_; ++pw) {
          // Compute pooling region for this output unit:
          //  start (included) = floor(ph * roi_height / pooled_height_)
          //  end (excluded) = ceil((ph + 1) * roi_height / pooled_height_)

          float h_ = float(ph) * bin_size_h + roi_start_h;
          float w_ = float(pw) * bin_size_w + roi_start_w;
          int hstart = fminf(floor(h_), height_-2);
          int wstart = fminf(floor(w_), width_-2);

          const int pool_index = ph * pooled_width_ + pw; 
          if (h_<0 || h_>=height_ || w_<0 || w_>=width_) {
              top_data[pool_index] = 0;
          } else {
              float  h_ratio = h_ - (float)(hstart);
              float  w_ratio = w_ - (float)(wstart);
              int upleft = hstart * width_ + wstart;
              int upright = upleft + 1;
              int downleft = upleft + width_;
              int downright = downleft + 1;

              //cout << "batch data: " << batch_data[upleft] << " " << batch_data[upright] << " " << batch_data[downleft] << " " << batch_data[downright] << endl;
              top_data[pool_index] = batch_data[upleft]*(1.-h_ratio)*(1.-w_ratio)
                                   + batch_data[upright]*(1.-h_ratio)*w_ratio
                                   + batch_data[downleft]*h_ratio*(1.-w_ratio)
                                   + batch_data[downright]*h_ratio*w_ratio;
          }
        }
      }
      // Increment all data pointers by one channel
      batch_data += data.size(2) * data.size(3);
      top_data += out.size(2) * out.size(3);
      argmax_data += max_idx.size(2) * max_idx.size(3);
    }
    // Increment ROI data pointer
    bottom_rois += bbox.size(1);
  }

  return;
}

template<typename Dtype>
inline void ROIAlignBackwardAcc(const Tensor<cpu, 4, Dtype> &in_grad,
                               const Tensor<cpu, 4, Dtype> &out_grad,
                               const Tensor<cpu, 2, Dtype> &bbox,
                               const Tensor<cpu, 4, Dtype> &max_idx,
                               const float spatial_scale_) {
  const Dtype *top_diff = out_grad.dptr_;
  const Dtype *bottom_rois = bbox.dptr_;
  Dtype *bottom_diff = in_grad.dptr_;
  Dtype *argmax_data = max_idx.dptr_;

  const int batch_size_ = in_grad.size(0);
  const int channels_ = in_grad.size(1);
  const int height_ = in_grad.size(2);
  const int width_ = in_grad.size(3);
  const int pooled_height_ = out_grad.size(2);
  const int pooled_width_ = out_grad.size(3);

  const int num_rois = bbox.size(0);

  const int data_size = in_grad.size(1) * in_grad.size(2) * in_grad.size(3);
  // For each ROI R = [batch_index x1 y1 x2 y2]: max pool over R
  for (int n = 0; n < num_rois; ++n) {
    int roi_batch_ind = bottom_rois[0];
    float roi_start_w = bottom_rois[1] * spatial_scale_;
    float roi_start_h = bottom_rois[2] * spatial_scale_;
    float roi_end_w = bottom_rois[3] * spatial_scale_;
    float roi_end_h = bottom_rois[4] * spatial_scale_;
    assert(roi_batch_ind >= 0);
    assert(roi_batch_ind < batch_size);

    // force malformed ROIs to be 1 * 1
    float roi_height = fmaxf(roi_end_h - roi_start_h, 0);
    //float roi_height = fmaxf(roi_end_h - roi_start_h + 1, 0);
    float roi_width = fmaxf(roi_end_w - roi_start_w, 0);
    //float roi_width = fmaxf(roi_end_w - roi_start_w + 1, 0);
    float bin_size_h = roi_height / (pooled_height_ - 1);
    float bin_size_w = roi_width / (pooled_width_ - 1);

    Dtype* batch_data = bottom_diff + data_size * roi_batch_ind;

    for (int c = 0; c < channels_; ++c) {
      for (int ph = 0; ph < pooled_height_; ++ph) {
        for (int pw = 0; pw < pooled_width_; ++pw) {
          // Compute pooling region for this output unit:
          //  start (included) = floor(ph * roi_height / pooled_height_)
          //  end (excluded) = ceil((ph + 1) * roi_height / pooled_height_)
          float h_ = float(ph) * bin_size_h + roi_start_h;
          float w_ = float(pw) * bin_size_w + roi_start_w;
          int hstart = fminf(floor(h_), height_-2);
          int wstart = fminf(floor(w_), width_-2);

          const int pool_index = ph * pooled_width_ + pw; 
          if (h_>=0 && h_<height_ && w_>=0 && w_<width_) {
              float h_ratio = h_ - (float)(hstart);
              float w_ratio = w_ - (float)(wstart);
              int upleft = hstart * width_ + wstart;
              int upright = upleft + 1;
              int downleft = upleft + width_;
              int downright = downleft + 1;

              batch_data[upleft] += top_diff[pool_index]*(1.-h_ratio)*(1.-w_ratio);
              batch_data[upright] += top_diff[pool_index]*(1.-h_ratio)*w_ratio;
              batch_data[downleft] += top_diff[pool_index]*h_ratio*(1.-w_ratio);
              batch_data[downright] += top_diff[pool_index]*h_ratio*w_ratio;
          }
        }
      }
      // Increment all data pointers by one channel
      batch_data += in_grad.size(2) * in_grad.size(3);
      top_diff += out_grad.size(2) * out_grad.size(3);
      argmax_data += max_idx.size(2) * max_idx.size(3);
    }
    // Increment ROI data pointer
    bottom_rois += bbox.size(1);
  }

  return;
}
}  // namespace mshadow

namespace mxnet {
namespace op {

template<>
Operator *CreateOp<cpu>(ROIAlignParam param, int dtype) {
  Operator* op = NULL;
  MSHADOW_REAL_TYPE_SWITCH(dtype, DType, {
    op = new ROIAlignOp<cpu, DType>(param);
  });
  return op;
}

Operator *ROIAlignProp::CreateOperatorEx(Context ctx, std::vector<TShape> *in_shape,
                                           std::vector<int> *in_type) const {
  std::vector<TShape> out_shape, aux_shape;
  std::vector<int> out_type, aux_type;
  CHECK(InferType(in_type, &out_type, &aux_type));
  CHECK(InferShape(in_shape, &out_shape, &aux_shape));
  DO_BIND_DISPATCH(CreateOp, param_, in_type->at(0));
}

DMLC_REGISTER_PARAMETER(ROIAlignParam);

MXNET_REGISTER_OP_PROPERTY(ROIAlign, ROIAlignProp)
.describe(R"code(Performs region of interest(ROI) pooling on the input array.

ROI pooling is a variant of a max pooling layer, in which the output size is fixed and
region of interest is a parameter. Its purpose is to perform max pooling on the inputs
of non-uniform sizes to obtain fixed-size feature maps. ROI pooling is a neural-net
layer mostly used in training a `Fast R-CNN` network for object detection.

This operator takes a 4D feature map as an input array and region proposals as `rois`,
then it pools over sub-regions of input and produces a fixed-sized output array
regardless of the ROI size.

To crop the feature map accordingly, you can resize the bounding box coordinates
by changing the parameters `rois` and `spatial_scale`.

The cropped feature maps are pooled by standard max pooling operation to a fixed size output
indicated by a `pooled_size` parameter. batch_size will change to the number of region
bounding boxes after `ROIAlign`.

The size of each region of interest doesn't have to be perfectly divisible by
the number of pooling sections(`pooled_size`).

Example::

  x = [[[[  0.,   1.,   2.,   3.,   4.,   5.],
         [  6.,   7.,   8.,   9.,  10.,  11.],
         [ 12.,  13.,  14.,  15.,  16.,  17.],
         [ 18.,  19.,  20.,  21.,  22.,  23.],
         [ 24.,  25.,  26.,  27.,  28.,  29.],
         [ 30.,  31.,  32.,  33.,  34.,  35.],
         [ 36.,  37.,  38.,  39.,  40.,  41.],
         [ 42.,  43.,  44.,  45.,  46.,  47.]]]]

  // region of interest i.e. bounding box coordinates.
  y = [[0,0,0,4,4]]

  // returns array of shape (2,2) according to the given roi with max pooling.
  ROIAlign(x, y, (2,2), 1.0) = [[[[ 14.,  16.],
                                    [ 26.,  28.]]]]

  // region of interest is changed due to the change in `spacial_scale` parameter.
  ROIAlign(x, y, (2,2), 0.7) = [[[[  7.,   9.],
                                    [ 19.,  21.]]]]

)code" ADD_FILELINE)
.add_argument("data", "NDArray-or-Symbol", "The input array to the pooling operator, "
                                            " a 4D Feature maps ")
.add_argument("rois", "NDArray-or-Symbol", "Bounding box coordinates, a 2D array of "
"[[batch_index, x1, y1, x2, y2]], where (x1, y1) and (x2, y2) are top left and bottom right "
"corners of designated region of interest. `batch_index` indicates the index of corresponding "
"image in the input array")
.add_arguments(ROIAlignParam::__FIELDS__());
}  // namespace op
}  // namespace mxnet
