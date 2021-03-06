/*
Copyright (c) 2016 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/
#include "kernels.h"
#include <math.h>
#include "exp_comp.h"
#include <algorithm>
#include <thread>

/////////////////////////////////////////////////////////////////////////////////////
//! \brief Exposure compensation C reference model. Not used in active stitching lib.
/////////////////////////////////////////////////////////////////////////////////////


extern bool StitchGetEnvironmentVariable(const char * name, char * value, size_t valueSize);

inline vx_uint32 count_nz_mean_single(vx_uint32 *p, uint32_t stride, int width, int height, uint32_t *psum)
{
	vx_uint32 cnt = 0, sum = 0;
	for (int i = 0; i < height; i++){
		for (int j = 0; j < width; j++){
			if (p[j] != 0x80000000){
#if USE_LUMA_VALUES_FOR_GAIN
				sum += (p[j] >> 24), cnt++;
#else
				int r = (p[j] & 0xFF);
				int g = (p[j] & 0xFF00) >> 8;
				int b = (p[j] & 0xFF0000) >> 16;
				//sum += (vx_uint32)std::sqrt((r*r) + (g*g) + (b*b));
				sum += (vx_uint32)((r + g + b) / 3);
				cnt++;
#endif
			}
		}
		p += stride;
	}
	*psum += sum;
	return cnt;
}

inline vx_uint32 count_nz_mean_double(vx_uint32 *p, vx_uint32 *q, uint32_t stride, int width, int height, uint32_t *psum, uint32_t *qsum)
{
	vx_uint32 cnt = 0, sum1 = 0, sum2 = 0;
	for (int i = 0; i < height; i++){
		for (int j = 0; j < width; j++){
			if ((p[j] != 0x80000000) && (q[j] != 0x80000000)){
#if USE_LUMA_VALUES_FOR_GAIN
				sum1 += (p[j] >> 24), cnt++;
				sum2 += (q[j] >> 24);
#else
				int r = (p[j] & 0xFF);
				int g = (p[j] & 0xFF00) >> 8;
				int b = (p[j] & 0xFF0000) >> 16;
				sum1 += (vx_uint32)std::sqrt((r*r) + (g*g) + (b*b));
				//	sum1 += (vx_uint32)((r + g + b) / 3);
				r = (q[j] & 0xFF);
				g = (q[j] & 0xFF00) >> 8;
				b = (q[j] & 0xFF0000) >> 16;
				sum2 += (vx_uint32)std::sqrt((r*r) + (g*g) + (b*b));
				//	sum2 += (vx_uint32)((r + g + b) / 3);
				cnt++;
#endif

			}
		}
		p += stride;
		q += stride;
	}
	*psum += sum1;
	*qsum += sum2;
	return cnt;
}

inline vx_uint32 count_nz_mean_double_32x32(vx_uint32 *p, vx_uint32 *q, uint32_t stride, uint32_t *psum, uint32_t *qsum)
{
	vx_uint32 cnt = 0, sum1 = 0, sum2 = 0;
	for (int i = 0; i < 32; i++){
		for (int j = 0; j < 32; j++){
			if ((p[j] & q[j]) != 0x80000000){
				sum1 += (p[j] >> 24), cnt++;
				sum2 += (q[j] >> 24);
			}
		}
		p += stride;
		q += stride;
	}
	*psum += sum1;
	*qsum += sum2;
	return cnt;
}

inline vx_uint32 count_nz_mean_single_32x32(vx_uint32 *p, vx_uint32 *q, uint32_t stride, uint32_t *psum)
{
	vx_uint32 cnt = 0, sum1 = 0, sum2 = 0;
	for (int i = 0; i < 32; i++){
		for (int j = 0; j < 32; j++){
			if (p[j] != 0x80000000){
				sum1 += (p[j] >> 24), cnt++;
			}
		}
		p += stride;
	}
	*psum += sum1;
	return cnt;
}

inline uint8_t saturate_char(int n)
{
	return (n > 255) ? 255 : (n < 0) ? 0 : n;
}

vx_status Compute_StitchExpCompCalcEntry(vx_rectangle_t *pOverlap_roi, vx_array ExpCompOut, int numCameras)
{
	vx_int32 i;
	ERROR_CHECK_OBJECT(ExpCompOut);
	if (!numCameras)return VX_FAILURE;
	vx_rectangle_t *rect_I;
	vx_int32 x1, y1, x2, y2;
	StitchOverlapPixelEntry ExpOut;
	ERROR_CHECK_STATUS(vxTruncateArray(ExpCompOut, 0));
	for (i = 0; i < numCameras; i++){
		for (int j = i + 1; j < numCameras; j++){
			int ID = (i * numCameras) + j;
			rect_I = &pOverlap_roi[ID];
			x1 = rect_I->start_x, x2 = rect_I->end_x;
			y1 = rect_I->start_y, y2 = rect_I->end_y;
			bool bValid = ((x1 < x2) && (y1 < y2));
			if (bValid)	// intersect is true
			{
				for (int y = y1; y < y2; y += 32){
					for (int x = x1; x < x2; x += 128){
						ExpOut.camId0 = i;
						ExpOut.start_x = x;
						ExpOut.start_y = y;
						ExpOut.end_x = ((x + 127) > x2) ? (x2 - x) : 127;
						ExpOut.end_y = ((y + 31) > y2) ? (y2 - y) : 31;
						ExpOut.camId1 = j;
						ExpOut.camId2 = 0x1F;
						ExpOut.camId3 = 0x1F;
						ExpOut.camId4 = 0x1F;
						ERROR_CHECK_STATUS(vxAddArrayItems(ExpCompOut, 1, &ExpOut, sizeof(ExpOut)));
					}
				}
			}
		}
	}
	return VX_SUCCESS;
}
vx_status Compute_StitchExpCompCalcValidEntry(vx_rectangle_t *pValid_roi, vx_array ExpCompOut, int numCameras, int Img_Height)
{
	StitchExpCompCalcEntry Exp_comp;
	ERROR_CHECK_STATUS(vxTruncateArray(ExpCompOut, 0));
	for (int i = 0; i < numCameras; i++){
		vx_rectangle_t *pRect = pValid_roi + i;
		unsigned int start_y = pRect->start_y;
		unsigned int end_y = pRect->end_y;
		for (unsigned int y = start_y; y < end_y; y += 32){
			for (unsigned int x = pRect->start_x; x < pRect->end_x; x += 128){
				Exp_comp.camId = i;
				Exp_comp.dstX = (x >> 3);
				Exp_comp.dstY = (y >> 1);
				Exp_comp.end_x = ((x + 127) > pRect->end_x) ? (pRect->end_x - x) : 127;
				Exp_comp.end_y = ((y + 31)  > end_y) ? (end_y - y) : 31;
				Exp_comp.start_x = 0;
				Exp_comp.start_y = 0;
				ERROR_CHECK_STATUS(vxAddArrayItems(ExpCompOut, 1, &Exp_comp, sizeof(Exp_comp)));
			}
		}
	}
	return VX_SUCCESS;
}


//! \brief The input validator callback.
static vx_status VX_CALLBACK exposure_compensation_input_validator(vx_node node, vx_uint32 index)
{
	vx_status status = VX_ERROR_INVALID_PARAMETERS;
	// get reference for parameter at specified index
	vx_reference ref = avxGetNodeParamRef(node, index);
	ERROR_CHECK_OBJECT(ref);
	// validate each parameter
	if (index == 0 || index == 1)
	{ // scalar of type VX_TYPE_FLOAT32
		vx_enum type = VX_TYPE_INVALID;
		ERROR_CHECK_STATUS(vxQueryScalar((vx_scalar)ref, VX_SCALAR_ATTRIBUTE_TYPE, &type, sizeof(type)));
		if (type == VX_TYPE_FLOAT32) {
			status = VX_SUCCESS;
		}
		else {
			status = VX_ERROR_INVALID_TYPE;
			vxAddLogEntry((vx_reference)node, status, "ERROR: exposure_compensation scalar type should be an float32\n");
		}
		if (!index){
			vx_float32 alpha = 0;
			ERROR_CHECK_STATUS(vxReadScalarValue((vx_scalar)ref, &alpha));
			if (alpha < 1.0) {
				status = VX_SUCCESS;
			}
			else {
				status = VX_ERROR_INVALID_DIMENSION;
				vxAddLogEntry((vx_reference)node, status, "ERROR: exposure compensation alpha value is not valid\n");
			}
		}
		else
		{
			vx_float32 beta = 0.0;
			ERROR_CHECK_STATUS(vxReadScalarValue((vx_scalar)ref, &beta));
			if (beta >= 1.0) {		// todo: see if there is valid upper range for beta
				status = VX_SUCCESS;
			}
			else {
				status = VX_ERROR_INVALID_DIMENSION;
				vxAddLogEntry((vx_reference)node, status, "ERROR: exposure compensation beta value is not valid\n");
			}
		}
		ERROR_CHECK_STATUS(vxReleaseScalar((vx_scalar *)&ref));
	}
	else if (index == 2)
	{ // array object of RECTANGLE type
		vx_enum itemtype = VX_TYPE_INVALID;
		vx_size capacity = 0;
		ERROR_CHECK_STATUS(vxQueryArray((vx_array)ref, VX_ARRAY_ATTRIBUTE_ITEMTYPE, &itemtype, sizeof(itemtype)));
		ERROR_CHECK_STATUS(vxQueryArray((vx_array)ref, VX_ARRAY_ATTRIBUTE_CAPACITY, &capacity, sizeof(capacity)));
		ERROR_CHECK_STATUS(vxReleaseArray((vx_array *)&ref));
		if (itemtype != VX_TYPE_RECTANGLE) {
			status = VX_ERROR_INVALID_TYPE;
			vxAddLogEntry((vx_reference)node, status, "ERROR: exposure_compensation array type should be an rectangle\n");
		}
		else if (capacity == 0) {
			status = VX_ERROR_INVALID_DIMENSION;
			vxAddLogEntry((vx_reference)node, status, "ERROR: exposure_compensation array capacity should be positive\n");
		}
		else {
			status = VX_SUCCESS;
		}
	}
	else if (index == 3)
	{ // image of format RGBX
		// get num cameras
		vx_array arr = (vx_array)avxGetNodeParamRef(node, 2);
		ERROR_CHECK_OBJECT(arr);
		vx_size capacity = 0;
		ERROR_CHECK_STATUS(vxQueryArray(arr, VX_ARRAY_ATTRIBUTE_CAPACITY, &capacity, sizeof(capacity)));
		ERROR_CHECK_STATUS(vxReleaseArray(&arr));
		vx_uint32 numCameras = (vx_uint32)capacity;
		// check input image format and dimensions
		vx_uint32 input_width = 0, input_height = 0;
		vx_df_image input_format = VX_DF_IMAGE_VIRT;
		ERROR_CHECK_STATUS(vxQueryImage((vx_image)ref, VX_IMAGE_ATTRIBUTE_WIDTH, &input_width, sizeof(input_width)));
		ERROR_CHECK_STATUS(vxQueryImage((vx_image)ref, VX_IMAGE_ATTRIBUTE_HEIGHT, &input_height, sizeof(input_height)));
		ERROR_CHECK_STATUS(vxQueryImage((vx_image)ref, VX_IMAGE_ATTRIBUTE_FORMAT, &input_format, sizeof(input_format)));
		ERROR_CHECK_STATUS(vxReleaseImage((vx_image *)&ref));
		if (input_format != VX_DF_IMAGE_RGBX) {
			status = VX_ERROR_INVALID_TYPE;
			vxAddLogEntry((vx_reference)node, status, "ERROR: exposure_compensation doesn't support input image format: %4.4s\n", &input_format);
		}
		else if ((input_height % numCameras) != 0) {
			status = VX_ERROR_INVALID_DIMENSION;
			vxAddLogEntry((vx_reference)node, status, "ERROR: exposure_compensation invalid input image dimensions: %dx%d (height should be multiple of %d)\n", input_width, input_height, numCameras);
		}
		else {
			status = VX_SUCCESS;
		}
	}
	return status;
}

//! \brief The output validator callback.
static vx_status VX_CALLBACK exposure_compensation_output_validator(vx_node node, vx_uint32 index, vx_meta_format meta)
{
	vx_status status = VX_ERROR_INVALID_PARAMETERS;
	if (index == 4)
	{ // image of format RGBX
		// get image configuration
		vx_image image = (vx_image)avxGetNodeParamRef(node, 3);
		ERROR_CHECK_OBJECT(image);
		vx_uint32 width = 0, height = 0;
		ERROR_CHECK_STATUS(vxQueryImage(image, VX_IMAGE_ATTRIBUTE_WIDTH, &width, sizeof(width)));
		ERROR_CHECK_STATUS(vxQueryImage(image, VX_IMAGE_ATTRIBUTE_HEIGHT, &height, sizeof(height)));
		ERROR_CHECK_STATUS(vxReleaseImage(&image));
		// set output image meta data
		vx_df_image format = VX_DF_IMAGE_RGBX;
		ERROR_CHECK_STATUS(vxSetMetaFormatAttribute(meta, VX_IMAGE_ATTRIBUTE_WIDTH, &width, sizeof(width)));
		ERROR_CHECK_STATUS(vxSetMetaFormatAttribute(meta, VX_IMAGE_ATTRIBUTE_HEIGHT, &height, sizeof(height)));
		ERROR_CHECK_STATUS(vxSetMetaFormatAttribute(meta, VX_IMAGE_ATTRIBUTE_FORMAT, &format, sizeof(format)));
		status = VX_SUCCESS;
	}
	return status;
}

//! \brief The kernel initialize.
static vx_status VX_CALLBACK exposure_compensation_initialize(vx_node node, const vx_reference * parameters, vx_uint32 num)
{
	vx_float32 alpha = 0, beta = 0;
	vx_size size = sizeof(CExpCompensator);
	vx_image img_in, img_out;
	CExpCompensator* exp_comp = new CExpCompensator();
	// calcuate the number of images in the input
	vx_array arr = (vx_array)avxGetNodeParamRef(node, 2);
	img_in = (vx_image)avxGetNodeParamRef(node, 3);
	img_out = (vx_image)avxGetNodeParamRef(node, 4);
	vx_scalar scalar = (vx_scalar)parameters[0];
	ERROR_CHECK_STATUS(vxReadScalarValue(scalar, &alpha));
	scalar = (vx_scalar)parameters[1];
	ERROR_CHECK_STATUS(vxReadScalarValue(scalar, &beta));
	ERROR_CHECK_STATUS(vxSetNodeAttribute(node, VX_NODE_ATTRIBUTE_LOCAL_DATA_SIZE, &size, sizeof(size)));
	ERROR_CHECK_STATUS(vxSetNodeAttribute(node, VX_NODE_ATTRIBUTE_LOCAL_DATA_PTR, &exp_comp, sizeof(exp_comp)))
		ERROR_CHECK_STATUS(exp_comp->Initialize(node, alpha, beta, arr, img_in, img_out));
	return VX_SUCCESS;
}

//! \brief The kernel deinitialize.
static vx_status VX_CALLBACK exposure_compensation_deinitialize(vx_node node, const vx_reference * parameters, vx_uint32 num)
{
	vx_status status = VX_FAILURE;
	vx_size		size;
	if (!vxQueryNode(node, VX_NODE_ATTRIBUTE_LOCAL_DATA_SIZE, &size, sizeof(size)) && (size == sizeof(CExpCompensator)))
	{
		CExpCompensator * exp_comp = nullptr;
		ERROR_CHECK_STATUS(vxQueryNode(node, VX_NODE_ATTRIBUTE_LOCAL_DATA_PTR, &exp_comp, sizeof(exp_comp)));
		if (exp_comp){
			status = exp_comp->DeInitialize();
		}
		delete exp_comp;
	}
	return status;
}

//! \brief The kernel execution.
static vx_status VX_CALLBACK exposure_compensation_kernel(vx_node node, const vx_reference * parameters, vx_uint32 num)
{
	vx_status status = VX_FAILURE;
	vx_size		size;
	if (!vxQueryNode(node, VX_NODE_ATTRIBUTE_LOCAL_DATA_SIZE, &size, sizeof(size)) && (size == sizeof(CExpCompensator)))
	{
		CExpCompensator * exp_comp = nullptr;
		ERROR_CHECK_STATUS(vxQueryNode(node, VX_NODE_ATTRIBUTE_LOCAL_DATA_PTR, &exp_comp, sizeof(exp_comp)));
		if (exp_comp){
			status = exp_comp->Process();
		}
		//	delete exp_comp;
	}
	return status;
}

//! \brief The kernel publisher.
vx_status exposure_compensation_publish(vx_context context)
{
	// add kernel to the context with callbacks
	vx_kernel kernel = vxAddKernel(context, "com.amd.loomsl.exposure_compensation_model",
		AMDOVX_KERNEL_STITCHING_EXPOSURE_COMPENSATION_MODEL,
		exposure_compensation_kernel,
		5,
		exposure_compensation_input_validator,
		exposure_compensation_output_validator,
		exposure_compensation_initialize,
		exposure_compensation_deinitialize);
	ERROR_CHECK_OBJECT(kernel);

	// set kernel parameters
	ERROR_CHECK_STATUS(vxAddParameterToKernel(kernel, 0, VX_INPUT, VX_TYPE_SCALAR, VX_PARAMETER_STATE_REQUIRED));
	ERROR_CHECK_STATUS(vxAddParameterToKernel(kernel, 1, VX_INPUT, VX_TYPE_SCALAR, VX_PARAMETER_STATE_REQUIRED));
	ERROR_CHECK_STATUS(vxAddParameterToKernel(kernel, 2, VX_INPUT, VX_TYPE_ARRAY, VX_PARAMETER_STATE_REQUIRED));
	ERROR_CHECK_STATUS(vxAddParameterToKernel(kernel, 3, VX_INPUT, VX_TYPE_IMAGE, VX_PARAMETER_STATE_REQUIRED));
	ERROR_CHECK_STATUS(vxAddParameterToKernel(kernel, 4, VX_OUTPUT, VX_TYPE_IMAGE, VX_PARAMETER_STATE_REQUIRED));
	// finalize and release kernel object
	ERROR_CHECK_STATUS(vxFinalizeKernel(kernel));
	ERROR_CHECK_STATUS(vxReleaseKernel(&kernel));

	return VX_SUCCESS;
}

CExpCompensator::CExpCompensator()
{
	m_pblockgainInfo = nullptr;
	m_NMat = nullptr;
	m_IMat = nullptr;
	m_AMat = nullptr;
	m_Gains = nullptr;
}

CExpCompensator::~CExpCompensator()
{

}

vx_status CExpCompensator::Initialize(vx_node node, vx_float32 alpha, vx_float32 beta, vx_array valid_roi, vx_image input, vx_image output)
{
	vx_uint32 i, blockgains_bufsize;
	vx_size capacity;
	vx_enum itemtype = VX_TYPE_INVALID;
	ERROR_CHECK_STATUS(vxQueryArray(valid_roi, VX_ARRAY_ATTRIBUTE_CAPACITY, &capacity, sizeof(capacity)));
	ERROR_CHECK_STATUS(vxQueryArray(valid_roi, VX_ARRAY_ATTRIBUTE_ITEMTYPE, &itemtype, sizeof(itemtype)));
	if (!capacity) return VX_ERROR_INVALID_PARAMETERS;
	m_numImages = (vx_uint32)capacity;		// assuming the array has one entry for each image
	if (input != nullptr){
		ERROR_CHECK_STATUS(vxQueryImage(input, VX_IMAGE_ATTRIBUTE_WIDTH, &m_width, sizeof(m_width)));
		ERROR_CHECK_STATUS(vxQueryImage(input, VX_IMAGE_ATTRIBUTE_HEIGHT, &m_height, sizeof(m_height)));
	}
	m_valid_roi = valid_roi;
	m_height /= m_numImages;				//height has to be a multiple of individual eqr height
	m_blockgainsStride = (m_width + 31) >> 5;
	m_pblockgainInfo = new block_gain_info[m_numImages];
	blockgains_bufsize = m_blockgainsStride*((m_height + 31) >> 5);
	m_alpha = alpha, m_beta = beta;
	m_InputImage = input, m_OutputImage = output;

	// initialize ROI based buffers
	vx_size		stride = 0;
	void		*base_array = 0;
	ERROR_CHECK_STATUS(vxAccessArrayRange(m_valid_roi, 0, capacity, &stride, (void **)&base_array, VX_READ_ONLY));
	// calulate the overlapped ROI matrix [numImages][numImages]	// assumed constant for all the frames. new parameters need new initialization
	vx_rectangle_t *rect_I, *rect_J;
	vx_int32 x1, y1, x2, y2;
	for (i = 0; i < m_numImages; i++){
		memset(&m_pRoi_rect[i][0], (int)-1, sizeof(vx_rectangle_t)*m_numImages);	// initialize to invalid
		rect_I = &vxArrayItem(vx_rectangle_t, base_array, i, stride);
		for (vx_uint32 j = i + 1; j < m_numImages; j++)
		{
			rect_J = &vxArrayItem(vx_rectangle_t, base_array, j, stride);
			x1 = std::max(rect_I->start_x, rect_J->start_x);
			y1 = std::max(rect_I->start_y, rect_J->start_y);
			x2 = std::min(rect_I->end_x, rect_J->end_x);
			y2 = std::min(rect_I->end_y, rect_J->end_y);
			if ((x1 < x2) && (y1 < y2))	// intersect is true
			{
				m_pRoi_rect[i][j].start_x = m_pRoi_rect[j][i].start_x = (vx_uint32)x1;
				m_pRoi_rect[i][j].end_x = m_pRoi_rect[j][i].end_x = (vx_uint32)x2;
				m_pRoi_rect[i][j].start_y = m_pRoi_rect[j][i].start_y = (vx_uint32)y1;
				m_pRoi_rect[i][j].end_y = m_pRoi_rect[j][i].end_y = (vx_uint32)y2;
			}
		}
		// allocate blockgains buffer
		m_pblockgainInfo[i].block_gain_buf = new vx_float32[blockgains_bufsize];
		m_pblockgainInfo[i].offset = (rect_I->start_y >> 5)*m_blockgainsStride + (rect_I->start_x >> 5);
		m_pblockgainInfo[i].num_blocks_row = (rect_I->end_x - rect_I->start_x + 31) >> 5;
		m_pblockgainInfo[i].num_blocks_col = (rect_I->end_y - rect_I->start_y + 31) >> 5;
		memcpy(&mValidRect[i], rect_I, sizeof(vx_rectangle_t));
	}
	// allocate N, I A and B arrays
	m_NMat = new vx_uint32*[m_numImages];
	m_IMat = new vx_uint32*[m_numImages];
	m_AMat = new vx_float64*[m_numImages];
	m_Gains = new vx_float32[m_numImages];
	for (i = 0; i < m_numImages; i++){
		m_NMat[i] = new vx_uint32[m_numImages];
		m_IMat[i] = new vx_uint32[m_numImages];
		m_AMat[i] = new vx_float64[m_numImages + 1];	// enough for the augmented matrix [a|b]
		memset(&m_AMat[i][0], 0, (m_numImages + 1)*sizeof(vx_float64));
	}
	memset(&m_Gains[0], 0x00000001, m_numImages*sizeof(vx_float32));
	m_node = node;
	return VX_SUCCESS;

}

vx_status CExpCompensator::DeInitialize()
{
	// free all allocated buffers
	for (int i = 0; i < (int)m_numImages; i++)
	{
		if (m_pblockgainInfo[i].block_gain_buf)
			delete[] m_pblockgainInfo[i].block_gain_buf;
		if (m_NMat[i]) delete[] m_NMat[i];
		if (m_IMat[i]) delete[] m_IMat[i];
		if (m_AMat[i]) delete[] m_AMat[i];
	}
	delete m_pblockgainInfo;
	delete m_NMat;
	delete m_IMat;
	delete m_AMat;
	return VX_SUCCESS;
}

vx_status CExpCompensator::Process()
{
	return CompensateGains();
}

// CPU based implementation for calculating image based gains
vx_status CExpCompensator::CompensateGains()
{
	vx_status status;
	int i;

	// Access full images for reading
	vx_imagepatch_addressing_t addr = { 0 };
	vx_rectangle_t rect;
	rect.start_x = 0;
	rect.start_y = 0;
	rect.end_x = m_width;
	rect.end_y = m_height*m_numImages;
	vx_uint8 * base_ptr = nullptr;
	ERROR_CHECK_STATUS(vxAccessImagePatch(m_InputImage, &rect, 0, &addr, (void **)&base_ptr, VX_READ_ONLY));
	// assume stride_x and stride_y are constant
	m_stride = addr.stride_y;
	m_stride_x = addr.stride_x;

	vx_uint32 nz, ISum, JSum;
	for (i = 0; i < (int)m_numImages; i++){
		m_IMat[i][i] = 0; m_NMat[i][i] = 0;
		for (int j = i+1; j < (int)m_numImages; j++){
			ISum = 0, JSum = 0, nz = 0;
			if (m_pRoi_rect[i][j].start_x != -1)	{ // if intersect
				// find the i,j intersect rect
				if (i == j){
					vx_uint32 *pI = (vx_uint32 *)(base_ptr + (m_height*i + m_pRoi_rect[i][j].start_y)*m_stride + (m_pRoi_rect[i][j].start_x*m_stride_x));
					nz += count_nz_mean_single(pI, (m_stride >> 2), (m_pRoi_rect[i][j].end_x - m_pRoi_rect[i][j].start_x), (m_pRoi_rect[i][j].end_y - m_pRoi_rect[i][j].start_y), &ISum);
				}
				else
				{
					vx_uint32 *pI = (vx_uint32 *)(base_ptr + (m_height*i + m_pRoi_rect[i][j].start_y)*m_stride + (m_pRoi_rect[i][j].start_x*m_stride_x));
					vx_uint32 *pJ = (vx_uint32 *)(base_ptr + (m_height*j + m_pRoi_rect[i][j].start_y)*m_stride + (m_pRoi_rect[i][j].start_x*m_stride_x));
					nz += count_nz_mean_double(pI, pJ, (m_stride >> 2), (m_pRoi_rect[i][j].end_x - m_pRoi_rect[i][j].start_x), (m_pRoi_rect[i][j].end_y - m_pRoi_rect[i][j].start_y), &ISum, &JSum);
				}
			}
			nz = std::max(nz, (vx_uint32)1);
			if (i == j){
				m_IMat[i][j] = 0;// ISum / nz;
				m_NMat[i][j] = 1;// nz;
			}
			else
			{
				m_IMat[i][j] = ISum / nz;
				m_IMat[j][i] = JSum / nz;
				m_NMat[i][j] = m_NMat[j][i] = nz;
			}
		}
	}
	// generate augmented matrix[A/b] for solving gains
	for (int i = 0; i < (int)m_numImages; i++){
		for (int j = 0; j < (int)m_numImages; ++j) {
			m_AMat[i][m_numImages] += m_beta * m_NMat[i][j];		// b matrix
			m_AMat[i][i] += m_beta * m_NMat[i][j];
			if (j == i)			continue;
			m_AMat[i][i] += 2 * m_alpha * m_IMat[i][j] * m_IMat[i][j] * m_NMat[i][j];
			m_AMat[i][j] -= 2 * m_alpha * m_IMat[i][j] * m_IMat[j][i] * m_NMat[i][j];
		}
	}

	//solve the linear equation A*gains_ = B
	solve_gauss(m_AMat, m_Gains, m_numImages);
	// Apply gains to all images
	status = ApplyGains(base_ptr);
	// commit image patch
	if ((status = vxCommitImagePatch(m_InputImage, &rect, 0, &addr, (void *)base_ptr) != VX_SUCCESS)) {
		vxAddLogEntry((vx_reference)m_node, VX_FAILURE, "ERROR Decoder Node: vxCommitImagePatch(READ) failed, status = %d\n", status);
		return VX_FAILURE;
	}
	return VX_SUCCESS;
}

vx_status CExpCompensator::SolveForGains(vx_float32 alpha, vx_float32 beta, vx_uint32 *pIMat, vx_uint32 *pNMat, vx_uint32 num_images, vx_array Gains_arr, vx_uint32 rows, vx_uint32 cols)
{
	unsigned int i, N = rows*cols;
	m_numImages = num_images;
	float *gains = new float[num_images];

	// normalize intensity 
	for (i = 0; i < N; i++){
		if (pNMat[i]){
			pIMat[i] =(vx_uint32) (pIMat[i]*16.0/ pNMat[i]);			// I values are scaled
		}
	}
	// generate augmented matrix[A/b] for solving gains
	m_AMat = new vx_float64*[m_numImages];
	for (i = 0; i < m_numImages; i++){
		m_AMat[i] = new vx_float64[m_numImages + 1];	// enough for the augmented matrix [a|b]
		memset(&m_AMat[i][0], 0, (m_numImages + 1)*sizeof(vx_float64));
	}
	for (i = 0; i < (int)num_images; i++){
		vx_uint32 *pI = pIMat + i*cols;
		vx_uint32 *pN = pNMat + i*cols;
		for (int j = 0; j < (int)num_images; ++j) {
			int N = pN[j] ? pN[j] : 1;
			m_AMat[i][m_numImages] += beta * N;		// b matrix
			m_AMat[i][i] += beta * N;
			if (j == i)			continue;
			m_AMat[i][i] += 2 * alpha * pI[j] * pI[j] * N;
			m_AMat[i][j] -= 2 * alpha * pI[j] * pIMat[j*num_images + i] * N;
		}
	}
	solve_gauss(m_AMat, gains, m_numImages);
	ERROR_CHECK_STATUS(vxTruncateArray(Gains_arr, 0));
	ERROR_CHECK_STATUS(vxAddArrayItems(Gains_arr, m_numImages, gains, sizeof(float)));
	delete gains;
	for (i = 0; i < (int)num_images; i++){
		delete m_AMat[i];
	}
	delete m_AMat;
	return VX_SUCCESS;
}

// solving linear equation of Augmented matrix[A|b] using gaussian elemination method
void CExpCompensator::solve_gauss(vx_float64 **A, vx_float32 *g, int num)
{
	int n = num;

	for (int i = 0; i < n; i++) {
		// Search for maximum in this column
		double maxEl = abs(A[i][i]);
		int maxRow = i;
		for (int k = i + 1; k<n; k++) {
			if (abs(A[k][i]) > maxEl) {
				maxEl = abs(A[k][i]);
				maxRow = k;
			}
		}
		// Swap maximum row with current row (column by column)
		for (int k = i; k < n + 1; k++) {
			double tmp = A[maxRow][k];
			A[maxRow][k] = A[i][k];
			A[i][k] = tmp;
		}
		// Make all rows below this one 0 in current column: scale
		for (int k = i + 1; k < n; k++) {
			double c = -A[k][i] / A[i][i];
			for (int j = i; j < n + 1; j++) {
				if (i == j) {
					A[k][j] = 0;
				}
				else {
					A[k][j] += c * A[i][j];
				}
			}
		}
	}

	// Solve equation Ax=b for an upper triangular matrix A
	for (int i = n - 1; i >= 0; i--) {
		double gain = (A[i][n] / A[i][i]);
		for (int k = i - 1; k >= 0; k--) {
			A[k][n] -= A[k][i] * gain;
		}
		g[i] = (vx_float32)gain;
	}
	return;
}

vx_status CExpCompensator::ApplyGains(void *in_base_addr)
{
	uint32_t num_threads = m_numImages - 1;
	std::thread *thread_apply_gains = new std::thread[num_threads];
	for (int i = 0; i < (int)m_numImages - 1; i++)
	{
		thread_apply_gains[i] = std::thread(&CExpCompensator::applygains_thread_func, this, i, (char *)in_base_addr);
	}
	applygains_thread_func(num_threads, (char *)in_base_addr);
	//Join the threads with the main thread
	for (int i = 0; i < (int)m_numImages - 1; i++) {
		thread_apply_gains[i].join();
	}
	return VX_SUCCESS;
}

vx_status CExpCompensator::applygains_thread_func(vx_int32 img_num, char *in_base_addr)
{
	vx_status status;
	// access image patch output for writing
	vx_imagepatch_addressing_t addr = { 0 };
	vx_rectangle_t rect;
	rect.start_x = 0;
	rect.start_y = m_height*img_num;
	rect.end_x = m_width;
	rect.end_y = rect.start_y + m_height;
	vx_uint8 * base_ptr = nullptr;
	ERROR_CHECK_STATUS(vxAccessImagePatch(m_OutputImage, &rect, 0, &addr, (void **)&base_ptr, VX_WRITE_ONLY));

	vx_int32 width = mValidRect[img_num].end_x - mValidRect[img_num].start_x;
	vx_int32 height = mValidRect[img_num].end_y - mValidRect[img_num].start_y;
	vx_uint32 *pRGB = (vx_uint32 *)(in_base_addr + (img_num*m_height + mValidRect[img_num].start_y)*m_stride + (mValidRect[img_num].start_x*m_stride_x));
	vx_uint32 *pDst = (vx_uint32 *)(base_ptr + mValidRect[img_num].start_y*addr.stride_y + mValidRect[img_num].start_x*addr.stride_x);
	float g_y = m_Gains[img_num];
	float g_r = g_y, g_g = g_y, g_b = g_y;	// todo: check if we need to apply gain factor for RGB
	// todo:: if we do the following code in CPU, need to optimize using SSE
	for (int i = 0; i < height; i++){
		for (int j = 0; j < width; j++){
			// apply gain only to valid pixels : todo: multiply with approapriate gain factors for R, G and B
			if (pRGB[j] != 0x80000000){
				uint8_t *p = (uint8_t *)&pRGB[j];
				uint8_t *d = (uint8_t *)&pDst[j];
				d[0] = saturate_char((int)(p[0] * g_r));
				d[1] = saturate_char((int)(p[1] * g_g));
				d[2] = saturate_char((int)(p[2] * g_b));
				d[3] = saturate_char((int)(p[3] * g_y));
			}
			else
				pDst[j] = pRGB[j];
		}
		pRGB += (m_stride >> 2);
		pDst += (addr.stride_y >> 2);
	}
	// commit image patch
	if ((status = vxCommitImagePatch(m_OutputImage, &rect, 0, &addr, (void *)base_ptr) != VX_SUCCESS)) {
		vxAddLogEntry((vx_reference)m_node, VX_FAILURE, "ERROR Decoder Node: vxCommitImagePatch(WRITE) failed, status = %d\n", status);
		return VX_FAILURE;
	}
	return status;
}