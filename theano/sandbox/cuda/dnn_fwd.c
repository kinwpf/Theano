#section support_code_struct

int
APPLY_SPECIFIC(conv_fwd)(CudaNdarray *input, CudaNdarray *kerns,
                         CudaNdarray *om, cudnnConvolutionDescriptor_t desc,
                         float alpha, float beta, CudaNdarray **output) {

  cudnnStatus_t err = CUDNN_STATUS_SUCCESS;
  if (CudaNdarray_HOST_DIMS(input)[1] != CudaNdarray_HOST_DIMS(kerns)[1]) {
    PyErr_SetString(PyExc_ValueError,
                    "GpuDnnConv images and kernel must have the same stack size\n");
    return 1;
  }

  if (c_set_tensorNd(input, APPLY_SPECIFIC(input)) == -1)
    return 1;
  if (c_set_filterNd(kerns, APPLY_SPECIFIC(kerns)) == -1)
    return 1;

  int nb_dim = CudaNdarray_NDIM(input);

#ifdef CONV_INPLACE
  Py_XDECREF(*output);
  *output = om;
  Py_INCREF(*output);
#else
  if (CudaNdarray_prep_output(output, nb_dim, CudaNdarray_HOST_DIMS(om)) != 0)
    return 1;
  if (beta != 0.0 && CudaNdarray_CopyFromCudaNdarray(*output, om))
    return 1;
#endif

   if (c_set_tensorNd(*output, APPLY_SPECIFIC(output)) == -1)
     return 1;

  {
    size_t worksize;
    void *workspace;
    cudnnConvolutionFwdAlgo_t chosen_algo;


    if (CHOOSE_ALGO)
    {

      // A new convolution implementation should be selected, based either on
      // timing or heuristics if in one of the two following cases :
      // - The implementation should only be chosen during the first execution
      //   of an apply node and this is the first execution of the apply node.
      // - The implementation should be chosen as often as necessary and the
      //   shapes of the inputs differ from the last time an implementation
      //   was chosen.
      bool reuse_previous_algo;
      if (CHOOSE_ALGO_ONCE)
      {
        // Only choose a new implementation of none has been chosen before.
        reuse_previous_algo = APPLY_SPECIFIC(previous_algo_set);
      }
      else
      {
        // Reuse the previous implementation if the inputs and the kernels
        // have the same shapes as they had when the previous implementation
        // was selected
        bool same_shapes = true;
        for (int i = 0; (i < nb_dim) && same_shapes; i++)
        {
          same_shapes &= (CudaNdarray_HOST_DIMS(input)[i] ==
                          APPLY_SPECIFIC(previous_input_shape)[i]);
          same_shapes &= (CudaNdarray_HOST_DIMS(kerns)[i] ==
                          APPLY_SPECIFIC(previous_kerns_shape)[i]);
        }
        reuse_previous_algo = same_shapes;
      }

      // If the previously choosen implementation can't be reused, select a
      // new one based on the shapes of the current inputs
      if (!reuse_previous_algo)
      {

        // Obtain a convolution algorithm appropriate for the input and kernel
        // shapes. Either by choosing one according to heuristics or by making
        // CuDNN time every implementation and choose the best one.
        if (CHOOSE_ALGO_TIME)
        {
#if defined(CUDNN_VERSION) && CUDNN_VERSION >= 3000
          // Time the different implementations to choose the best one
          int requestedCount = 1;
          int count;
          cudnnConvolutionFwdAlgoPerf_t choosen_algo_perf;
          err = cudnnFindConvolutionForwardAlgorithm(_handle,
                                                     APPLY_SPECIFIC(input),
                                                     APPLY_SPECIFIC(kerns),
                                                     desc,
                                                     APPLY_SPECIFIC(output),
                                                     requestedCount,
                                                     &count,
                                                     &choosen_algo_perf);
          if (err != CUDNN_STATUS_SUCCESS) {
            PyErr_Format(PyExc_RuntimeError,
                         "GpuDnnConv: error selecting convolution algo: %s",
                         cudnnGetErrorString(err));
            return 1;
          }

          chosen_algo = choosen_algo_perf.algo;
#endif
        }
        else
        {
          // The implementation should be chosen using heuristics based on the
          // input shapes and the amount of memory available.

          // Get the amount of available memory
          size_t free = 0, total = 0;
          cudaError_t err2 = cudaMemGetInfo(&free, &total);
          if (err2 != cudaSuccess){
            cudaGetLastError();
            fprintf(stderr,
                    "Error when trying to find the memory information"
                    " on the GPU: %s\n", cudaGetErrorString(err2));
            return 1;
          }

          // Use heuristics to choose the implementation
          err = cudnnGetConvolutionForwardAlgorithm(_handle,
                                                    APPLY_SPECIFIC(input),
                                                    APPLY_SPECIFIC(kerns),
                                                    desc,
                                                    APPLY_SPECIFIC(output),
                                                    CUDNN_CONVOLUTION_FWD_SPECIFY_WORKSPACE_LIMIT,
                                                    free,
                                                    &chosen_algo);

          if (err != CUDNN_STATUS_SUCCESS) {
            PyErr_Format(PyExc_RuntimeError,
                         "GpuDnnConv: error selecting convolution algo: %s",
                         cudnnGetErrorString(err));
            return 1;
          }
        }

        // Store the shapes of the inputs and kernels as well as the chosen
        // algorithm for future use.
        APPLY_SPECIFIC(previous_algo) = chosen_algo;
        APPLY_SPECIFIC(previous_algo_set) = true;
        for (int i = 0; i < nb_dim; i++)
        {
            APPLY_SPECIFIC(previous_input_shape)[i] =
                                            CudaNdarray_HOST_DIMS(input)[i];
            APPLY_SPECIFIC(previous_kerns_shape)[i] =
                                            CudaNdarray_HOST_DIMS(kerns)[i];
        }
      }
      else
      {
          // Reuse the previously chosen convolution implementation
          chosen_algo = APPLY_SPECIFIC(previous_algo);
      }
    }
    else
    {
      chosen_algo = CONV_ALGO;
    }

#if defined(CUDNN_VERSION) && CUDNN_VERSION >= 3000
    // The FFT implementation (only in V3 and onward) does not support strides,
    // 1x1 filters or inputs with a spatial dimension larger than 1024.
    // If the chosen implementation is FFT, validate that it can be used
    // on the current data and default on a safe implementation if it
    // can't.
    // Following code is 2d-specific, but it is fine as ftt is defined only for
    // 2d-filters
    if (chosen_algo == CUDNN_CONVOLUTION_FWD_ALGO_FFT && nb_dim == 4)
    {

      // Extract the properties of the convolution descriptor
      int pad_h, pad_w, stride_v, stride_h, upscale_x, upscale_y;
      cudnnConvolutionMode_t mode;
      err = cudnnGetConvolution2dDescriptor(desc, &pad_h, &pad_w,
                                            &stride_v, &stride_h,
                                            &upscale_x, &upscale_y,
                                            &mode);

      if (err != CUDNN_STATUS_SUCCESS) {
        PyErr_Format(PyExc_RuntimeError,
                     "GpuDnnConv: error getting convolution properties: %s",
                     cudnnGetErrorString(err));
        return 1;
      }

      // Extract the spatial size of the filters
      int filter_h = CudaNdarray_HOST_DIMS(kerns)[2];
      int filter_w = CudaNdarray_HOST_DIMS(kerns)[3];

      // Extract the spatial size of the input
      int input_h = CudaNdarray_HOST_DIMS(input)[2];
      int input_w = CudaNdarray_HOST_DIMS(input)[3];

      // Ensure that the selected implementation supports the requested
      // convolution. Fall back to a safe implementation otherwise.
      if (stride_v != 1 || stride_h != 1 || input_h > 1024 ||
          input_w > 1024 || (filter_h == 1 && filter_w == 1))
      {
        chosen_algo = CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM;
      }
    }
#endif

#if defined(CUDNN_VERSION) && CUDNN_VERSION < 3000
    // In versions before V3, CuDNN did not support kernels larger than the
    // inputs in any spatial dimension, even if padding was used such that the
    // padded inputs were larger than the kernels. If the kernels are larger
    // then the inputs, raise an error message.

    bool shape_mismatch = false;
    for (int i=2; i < nb_dim; i++){
        shape_mismatch = shape_mismatch || (CudaNdarray_HOST_DIMS(kerns)[i] >
                                            CudaNdarray_HOST_DIMS(input)[i]);
    }

    if (shape_mismatch){
      PyErr_Format(PyExc_RuntimeError,
                   "GpuDnnConv: the current version of CuDNN does not support "
                   "kernels larger than the inputs in any spatial dimension, "
                   "even if the inputs are padded such that the padded inputs "
                   "are larger than the kernels. Update your installation of "
                   "CuDNN to V3 or more recent to solve the issue.");
      return 1;
    }
#endif

    err = cudnnGetConvolutionForwardWorkspaceSize(_handle,
                                                  APPLY_SPECIFIC(input),
                                                  APPLY_SPECIFIC(kerns),
                                                  desc,
                                                  APPLY_SPECIFIC(output),
                                                  chosen_algo,
                                                  &worksize);
    if (err != CUDNN_STATUS_SUCCESS) {
      PyErr_Format(PyExc_RuntimeError,
                   "GpuDnnConv: error getting worksize: %s",
                   cudnnGetErrorString(err));
      return 1;
    }
    workspace = get_work_mem(worksize);
    if (workspace == NULL && worksize != 0)
      return 1;

    err = cudnnConvolutionForward(
      _handle,
      (void *)&alpha,
      APPLY_SPECIFIC(input), CudaNdarray_DEV_DATA(input),
      APPLY_SPECIFIC(kerns), CudaNdarray_DEV_DATA(kerns),
      desc,
      chosen_algo,
      workspace, worksize,
      (void *)&beta,
      APPLY_SPECIFIC(output), CudaNdarray_DEV_DATA(*output));
  }
  if (err != CUDNN_STATUS_SUCCESS) {
    PyErr_Format(PyExc_RuntimeError, "GpuDnnConv: error doing operation: %s",
		 cudnnGetErrorString(err));
    return 1;
  }
  return 0;
}
