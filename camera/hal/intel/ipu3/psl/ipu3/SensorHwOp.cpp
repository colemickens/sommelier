/*
 * Copyright (C) 2017-2018 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "SensorHwOp"

#include "SensorHwOp.h"
#include "LogHelper.h"

namespace cros {
namespace intel {

/**
 * SENSOR TYPE OPERATIONS
 */

/**
 * Base sensor class operation
 * also SMIAPP sensor class operation
 */

SensorHwOp::SensorHwOp(std::shared_ptr<cros::V4L2Subdevice> pixelArraySubdev):
   pPixelArraySubdev(pixelArraySubdev),
   pPixelRate(0),
   pHorzBlank(0),
   pVertBlank(0),
   pCropWidth(0),
   pCropHeight(0),
   pSensorFTWidth(0),
   pSensorFTHeight(0),
   pHBlankReadOnly(false),
   pVBlankReadOnly(false),
   pUseDiscreteDG(false)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL1, LOG_TAG);
}

SensorHwOp::~SensorHwOp()
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL1, LOG_TAG);
}

/************* V4L2 wrapper *******************/
/**
 * retrieving crop values and format from pixel array driver.
 *
 * \param[OUT] width: width of the crop
 * \param[OUT] height: height of the crop
 * \param[OUT] code: pixel format
 *
 * \return IOCTL return value.
 */
int SensorHwOp::getActivePixelArraySize(int &width,
                                        int &height,
                                        int &code)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2, LOG_TAG);
    int status = BAD_VALUE;

    status = pPixelArraySubdev->GetPadFormat(0, &width, &height, &code);

    return status;
}

/**
 * Update the class members used to calculate blanking
 *
 * \return IOCTL return value.
 */
status_t SensorHwOp::updateMembers()
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2, LOG_TAG);
    int status = BAD_VALUE;
    int code = 0;
    v4l2_queryctrl control;

    status = getActivePixelArraySize(pCropWidth, pCropHeight, code);
    if (status != NO_ERROR) {
        LOGE("Error getting  PA size");
        return UNKNOWN_ERROR;
    }

    status = updateFrameTimings();
    if (status != NO_ERROR) {
        LOGE("Error updating frame timings");
        return UNKNOWN_ERROR;
    }

    control.id = V4L2_CID_HBLANK;
    status = pPixelArraySubdev->QueryControl(&control);
    if (status == NO_ERROR && (control.flags & V4L2_CTRL_FLAG_READ_ONLY)) {
        pHBlankReadOnly = true;
        LOG1("HBLANK is readonly");
    }

    control.id = V4L2_CID_VBLANK;
    status = pPixelArraySubdev->QueryControl(&control);
    if (status == NO_ERROR && (control.flags & V4L2_CTRL_FLAG_READ_ONLY)) {
        pVBlankReadOnly = true;
        LOG1("VBLANK is readonly");
    }

    return status;
}

/**
 * retrieving output size and format from sensor
 * after possible cropping, binning and scaling
 *
 * \param[OUT] width: width of the crop
 * \param[OUT] height: height of the crop
 * \param[OUT] code: pixel format
 *
 * \return IOCTL return value.
 */
int SensorHwOp::getSensorOutputSize(int &width, int &height, int &code)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2, LOG_TAG);
    int status = BAD_VALUE;

    status = pPixelArraySubdev->GetPadFormat(0, &width, &height, &code);

    return status;
}

/**
 * retrieving pixel rate from pixel array
 *
 * \param[OUT] pixel_rate: pixel rate value
 *
 * \return IOCTL return value.
 */
int SensorHwOp::getPixelRate(int &pixel_rate)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2, LOG_TAG);

    return pPixelArraySubdev->GetControl(V4L2_CID_PIXEL_RATE, &pixel_rate);
}

/**
 * retrieving link frequency from scaler subdev
 *
 * \param[OUT] link_freq: link frequency value
 *
 * \return IOCTL return value.
 */
int SensorHwOp::getLinkFreq(int &link_freq)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2, LOG_TAG);

    return pPixelArraySubdev->GetControl(V4L2_CID_LINK_FREQ, &link_freq);
}

/**
 * retrieving pixel clock from scaler subdev
 *
 * \param[OUT] pixel_clock: pixel clock value
 *
 * \return IOCTL return value.
 */
int SensorHwOp::getPixelClock(int64_t &pixel_clock)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2, LOG_TAG);
    int ret = BAD_VALUE;
    int link_freq = 0;
    v4l2_querymenu menu;
    CLEAR(menu);

    ret = pPixelArraySubdev->GetControl(V4L2_CID_LINK_FREQ, &link_freq);
    if (ret != NO_ERROR)
        return ret;

    menu.id = V4L2_CID_LINK_FREQ;
    menu.index = link_freq;
    ret = pPixelArraySubdev->QueryMenu(&menu);
    if (ret != NO_ERROR)
        return ret;

    pixel_clock = menu.value;
    LOG1("pixel clock set to %lld", menu.value);
    return ret;
}

/**
 * set exposure value to sensor driver
 *
 * \param[IN] coarse_exposure: coarse exposure value
 * \param[IN] fine_exposure: fine exposure value
 *
 * return IOCTL return value.
 */
int SensorHwOp::setExposure(int coarse_exposure, int fine_exposure)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2, LOG_TAG);
    UNUSED(fine_exposure);
    int ret = BAD_VALUE;

    ret = pPixelArraySubdev->SetControl(V4L2_CID_EXPOSURE, coarse_exposure);
    // TODO: Need fine_exposure whenever supported on kernel.
    return ret;
}

/**
 * get exposure value from sensor driver
 *
 * \param[OUT] coarse_exposure: coarse exposure value
 * \param[OUT] fine_exposure: fine exposure value
 *
 * TODO: V4L2 does not support FINE_EXPOSURE setting
 *
 * \return IOCTL return value.
 */
int SensorHwOp::getExposure(int &coarse_exposure, int &fine_exposure)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2, LOG_TAG);
    int ret = BAD_VALUE;

    ret = pPixelArraySubdev->GetControl(V4L2_CID_EXPOSURE, &coarse_exposure);
    // TODO: Need fine exposure whenever supported in kernel.
    fine_exposure = -1;

    return ret;
}

/**
 * get exposure range value from sensor driver
 *
 * \param[OUT] coarse_exposure: exposure min value
 * \param[OUT] fine_exposure: exposure max value
 * \param[OUT] exposure_step: step of exposure
 *
 * TODO: V4L2 does not support FINE_EXPOSURE setting
 *
 * \return IOCTL return value.
 */
int SensorHwOp::getExposureRange(int &exposure_min, int &exposure_max, int &exposure_step)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2, LOG_TAG);
    int ret = BAD_VALUE;
    v4l2_queryctrl exposure;
    CLEAR(exposure);

    exposure.id = V4L2_CID_EXPOSURE;

    ret = pPixelArraySubdev->QueryControl(&exposure);
    if (ret != NO_ERROR) {
        LOGE("Couldn't get exposure Range");
        return ret;
    }

    exposure_min = exposure.minimum;
    exposure_max = exposure.maximum;
    exposure_step = exposure.step;

    return ret;
}

/**
 * set analog and digital gain to sensor driver
 *
 * \param[IN] analog_gain: analog gain value
 * \param[IN] digital_gain: digital gain value
 *
 * \return IOCTL return value.
 */
int SensorHwOp::setGains(int analog_gain, int digital_gain)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2, LOG_TAG);
    int ret = BAD_VALUE;

    ret = pPixelArraySubdev->SetControl(V4L2_CID_ANALOGUE_GAIN, analog_gain);
    if (pUseDiscreteDG || digital_gain != 0) {
        ret = pPixelArraySubdev->SetControl(V4L2_CID_DIGITAL_GAIN, digital_gain);
    }
    return ret;
}

/**
 * get analog and digital gain from sensor driver
 *
 * \param[OUT] analog_gain: analog gain value
 * \param[OUT] digital_gain: digital gain value
 *
 * TODO: V4L2 does not support DIGITAL_GAIN setting
 *
 * return IOCTL return value.
 */
int SensorHwOp::getGains(int &analog_gain, int &digital_gain)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2, LOG_TAG);
    int ret = BAD_VALUE;

    ret = pPixelArraySubdev->GetControl(V4L2_CID_ANALOGUE_GAIN, &analog_gain);
    // XXX: Need digital_gain whenever defined in V4L2.
    digital_gain = -1;

    return ret;
}

/**
 * This function is using Hblank and Vblank, because it s supported
 * by both CRL and SMIAPP drivers
 *
 * \param[IN] llp: Line Length in Pixel value
 * \param[IN] fll: Frame Length in Line value
 *
 * \return IOCTL return value.
 */
status_t SensorHwOp::setFrameDuration(unsigned int llp, unsigned int fll)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2, LOG_TAG);
    status_t status = OK;
    status_t statusH = OK;
    status_t statusV = OK;
    int horzBlank, vertBlank;

    /* only calculate when not 0 */
    if (llp && !pHBlankReadOnly) {
        horzBlank = llp - pCropWidth;
        statusH = pPixelArraySubdev->SetControl(V4L2_CID_HBLANK, horzBlank);
        if (statusH != OK)
            LOGE("Failed to set hblank");
    }

    if (fll && !pVBlankReadOnly) {
        vertBlank = fll - pCropHeight;
        statusV = pPixelArraySubdev->SetControl(V4L2_CID_VBLANK, vertBlank);
        if (statusV != OK)
            LOGE("Failed to set vblank");
    }

    if (statusH != OK || statusV != OK)
        status = UNKNOWN_ERROR;

    return status;
}

/**
 * This function is using Hblank and Vblank, because it s supported
 * by both CRL and SMIAPP drivers
 *
 * \param[OUT] llp: Line Length in Pixel value
 * \param[OUT] fll: Frame Length in Line value
 *
 * \return IOCTL return value.
 */
status_t SensorHwOp::getMinimumFrameDuration(unsigned int &llp, unsigned int &fll)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2, LOG_TAG);
    status_t status = OK;
    status_t statusH = OK;
    status_t statusV = OK;
    int horzBlank, vertBlank;

    // Get query control for sensor mode to determine min value
    v4l2_queryctrl sensorModeControl;
    CLEAR(sensorModeControl);
    sensorModeControl.id = V4L2_CID_HBLANK;
    statusH = pPixelArraySubdev->QueryControl(&sensorModeControl);
    horzBlank = sensorModeControl.minimum;
    LOG2("%s, QueryControl statusH: %d, horzBlank: %d", __FUNCTION__, statusH, horzBlank);
    if (statusH == OK)
        llp = horzBlank + pCropWidth;
    else
        LOGE("failed to get hblank");

    CLEAR(sensorModeControl);
    sensorModeControl.id = V4L2_CID_VBLANK;
    statusV = pPixelArraySubdev->QueryControl(&sensorModeControl);
    vertBlank = sensorModeControl.minimum;
    LOG2("%s, QueryControl statusV: %d, vertBlank, %d", __FUNCTION__, statusV, vertBlank);
    if (statusV == OK)
        fll = vertBlank + pCropHeight;
    else
        LOGE("failed to get vblank");

    if (statusH != OK || statusV != OK)
        status = UNKNOWN_ERROR;

    pHorzBlank = horzBlank;
    pVertBlank = vertBlank;
    return status;
}

/**
 * This function is used to get vblank from private member
 *
 * \param[OUT] vblank: local vblank value
 *
 * \return OK
 */
int SensorHwOp::getVBlank(unsigned int &vblank)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2, LOG_TAG);
    vblank = pVertBlank;

    return OK;
}

/**
 * This function is used to get hblank from private member
 *
 * \param[OUT] hblank: local hblank value
 *
 * \return OK
 */
int SensorHwOp::getHBlank(unsigned int &hblank)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2, LOG_TAG);
    hblank = pHorzBlank;

    return OK;
}

/**
 * This function is used to get the aperture from driver
 *
 * \param[OUT] aperture: aperture from driver
 *
 * \return IOCTL return value
 */
int SensorHwOp::getAperture(int &aperture)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2, LOG_TAG);
    return pPixelArraySubdev->GetControl(V4L2_CID_IRIS_ABSOLUTE, &aperture);
}

/**
 * This function is empty for base class
 *
 * \return OK.
 */
status_t SensorHwOp::updateFrameTimings()
{
    return OK;
}

/**
 * Set the sensor frame timings from the xml
 *
 * \param[IN] width: Frame timing width
 * \param[IN] height: Frame timing height
 *
 * \return OK
 */
status_t SensorHwOp::setSensorFT(int width, int height)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2, LOG_TAG);

    pSensorFTWidth = width;
    pSensorFTHeight = height;

    LOG2("@%s: Setting sensor FT %d %d", __FUNCTION__, pSensorFTWidth,
         pSensorFTHeight);

    return OK;
}

/**
 * retrieving test pattern mode from sensor driver
 *
 * \param[OUT] mode: sensor test pattern mode
 *
 * \return IOCTL return value.
 */
int SensorHwOp::getTestPattern(int *mode)
{
    return pPixelArraySubdev->GetControl(V4L2_CID_TEST_PATTERN, mode);
}

/**
 * set test pattern mode to sensor driver
 *
 * \param[IN] mode: test pattern mode
 *
 * \return IOCTL return value.
 */
int SensorHwOp::setTestPattern(int mode)
{
    return pPixelArraySubdev->SetControl(V4L2_CID_TEST_PATTERN, mode);
}

/**
* create a map for discrete digital gain
*
* \param[OUT] discreteDG: use discrete digital gain
*
* \return IOCTL return value.
*/
int SensorHwOp::createDiscreteDgMap(bool *use_discrete_digital_gain)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2, LOG_TAG);
    status_t status = OK;
    v4l2_queryctrl dgControl;
    v4l2_querymenu dgMenu;
    CLEAR(dgControl);
    CLEAR(dgMenu);

    *use_discrete_digital_gain = false;

    dgControl.id = V4L2_CID_DIGITAL_GAIN;
    status = pPixelArraySubdev->QueryControl(&dgControl);
    if (status) {
        LOGE("failed to get the max index of discrete DG table");
        return status;
    }

    dgMenu.id = V4L2_CID_DIGITAL_GAIN;
    if (dgControl.type == V4L2_CTRL_TYPE_INTEGER_MENU && dgControl.maximum > 0) {
        /* discrete DG has been enabled, WA should work for the sensor */
        for (dgMenu.index = 0; dgMenu.index <= dgControl.maximum; dgMenu.index++) {
            status = pPixelArraySubdev->QueryMenu(&dgMenu);
            if (status == OK) {
                LOG2("insert discrete DG val: %d %lld", dgMenu.index, dgMenu.value);
                pDgMap.emplace(dgMenu.value, dgMenu.index);
            } else {
                LOGE("failed to get discrete DG table");
                pUseDiscreteDG = false;

                return status;
            }
        }

        pUseDiscreteDG = true;
        *use_discrete_digital_gain = true;
    }

    return status;
}

/**
* get a suitable discrete digital gain
*
* \param[OUT] original digital gain: the current DG value
*
* \param[OUT] digital_gain_idx: index of the current DG in the DG menu
*/
void SensorHwOp::getDiscreteDg(unsigned short *digital_gain, unsigned short *digital_gain_idx)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2, LOG_TAG);

    *digital_gain_idx = 0;

    if (pUseDiscreteDG) {
        auto it = pDgMap.upper_bound((int64_t)*digital_gain - 1);
        if (it != pDgMap.end()) {
            *digital_gain = (unsigned short)it->first;
            *digital_gain_idx = (unsigned short)it->second;
            LOG2("%s new discreteDG:%d, discreteDgIdx:%d", __FUNCTION__,
                 *digital_gain, *digital_gain_idx);
        } else {
            LOGE("%s no suitable discrete DG", __FUNCTION__);
        }
    }
}


/*
 * -- end SensorHwOp
 */

}// namespace intel
}// namespace cros
