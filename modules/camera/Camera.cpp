/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include <cstdlib>
#include <pthread.h>
#include <hardware/camera3.h>
#include "CameraHAL.h"
#include "Stream.h"

//#define LOG_NDEBUG 0
#define LOG_TAG "Camera"
#include <cutils/log.h>

#define ATRACE_TAG (ATRACE_TAG_CAMERA | ATRACE_TAG_HAL)
#include <cutils/trace.h>
#include "ScopedTrace.h"

#include "Camera.h"

namespace default_camera_hal {

extern "C" {
// Shim passed to the framework to close an opened device.
static int close_device(hw_device_t* dev)
{
    camera3_device_t* cam_dev = reinterpret_cast<camera3_device_t*>(dev);
    Camera* cam = static_cast<Camera*>(cam_dev->priv);
    return cam->close();
}
} // extern "C"

Camera::Camera(int id)
  : mId(id),
    mBusy(false),
    mCallbackOps(NULL),
    mStreams(NULL),
    mNumStreams(0)
{
    pthread_mutex_init(&mMutex,
                       NULL); // No pthread mutex attributes.

    memset(&mDevice, 0, sizeof(mDevice));
    mDevice.common.tag    = HARDWARE_DEVICE_TAG;
    mDevice.common.close  = close_device;
    mDevice.ops           = const_cast<camera3_device_ops_t*>(&sOps);
    mDevice.priv          = this;
}

Camera::~Camera()
{
}

int Camera::open(const hw_module_t *module, hw_device_t **device)
{
    ALOGI("%s:%d: Opening camera device", __func__, mId);
    CAMTRACE_CALL();
    pthread_mutex_lock(&mMutex);
    if (mBusy) {
        pthread_mutex_unlock(&mMutex);
        ALOGE("%s:%d: Error! Camera device already opened", __func__, mId);
        return -EBUSY;
    }

    // TODO: open camera dev nodes, etc
    mBusy = true;
    mDevice.common.module = const_cast<hw_module_t*>(module);
    *device = &mDevice.common;

    pthread_mutex_unlock(&mMutex);
    return 0;
}

int Camera::close()
{
    ALOGI("%s:%d: Closing camera device", __func__, mId);
    CAMTRACE_CALL();
    pthread_mutex_lock(&mMutex);
    if (!mBusy) {
        pthread_mutex_unlock(&mMutex);
        ALOGE("%s:%d: Error! Camera device not open", __func__, mId);
        return -EINVAL;
    }

    // TODO: close camera dev nodes, etc
    mBusy = false;

    pthread_mutex_unlock(&mMutex);
    return 0;
}

int Camera::initialize(const camera3_callback_ops_t *callback_ops)
{
    ALOGV("%s:%d: callback_ops=%p", __func__, mId, callback_ops);
    mCallbackOps = callback_ops;
    return 0;
}

int Camera::configureStreams(camera3_stream_configuration_t *stream_config)
{
    camera3_stream_t *astream;
    Stream **newStreams = NULL;

    CAMTRACE_CALL();
    ALOGV("%s:%d: stream_config=%p", __func__, mId, stream_config);

    if (stream_config == NULL) {
        ALOGE("%s:%d: NULL stream configuration array", __func__, mId);
        return -EINVAL;
    }
    if (stream_config->num_streams == 0) {
        ALOGE("%s:%d: Empty stream configuration array", __func__, mId);
        return -EINVAL;
    }

    // Create new stream array
    newStreams = new Stream*[stream_config->num_streams];
    ALOGV("%s:%d: Number of Streams: %d", __func__, mId,
            stream_config->num_streams);

    pthread_mutex_lock(&mMutex);

    // Mark all current streams unused for now
    for (int i = 0; i < mNumStreams; i++)
        mStreams[i]->mReuse = false;
    // Fill new stream array with reused streams and new streams
    for (int i = 0; i < stream_config->num_streams; i++) {
        astream = stream_config->streams[i];
        if (astream->max_buffers > 0)
            newStreams[i] = reuseStream(astream);
        else
            newStreams[i] = new Stream(mId, astream);

        if (newStreams[i] == NULL) {
            ALOGE("%s:%d: Error processing stream %d", __func__, mId, i);
            goto err_out;
        }
        astream->priv = newStreams[i];
    }

    // Verify the set of streams in aggregate
    if (!isValidStreamSet(newStreams, stream_config->num_streams)) {
        ALOGE("%s:%d: Invalid stream set", __func__, mId);
        goto err_out;
    }

    // Set up all streams (calculate usage/max_buffers for each)
    setupStreams(newStreams, stream_config->num_streams);

    // Destroy all old streams and replace stream array with new one
    destroyStreams(mStreams, mNumStreams);
    mStreams = newStreams;
    mNumStreams = stream_config->num_streams;

    pthread_mutex_unlock(&mMutex);
    return 0;

err_out:
    // Clean up temporary streams, preserve existing mStreams/mNumStreams
    destroyStreams(newStreams, stream_config->num_streams);
    pthread_mutex_unlock(&mMutex);
    return -EINVAL;
}

void Camera::destroyStreams(Stream **streams, int count)
{
    if (streams == NULL)
        return;
    for (int i = 0; i < count; i++) {
        // Only destroy streams that weren't reused
        if (streams[i] != NULL && !streams[i]->mReuse)
            delete streams[i];
    }
    delete [] streams;
}

Stream *Camera::reuseStream(camera3_stream_t *astream)
{
    Stream *priv = reinterpret_cast<Stream*>(astream->priv);
    // Verify the re-used stream's parameters match
    if (!priv->isValidReuseStream(mId, astream)) {
        ALOGE("%s:%d: Mismatched parameter in reused stream", __func__, mId);
        return NULL;
    }
    // Mark stream to be reused
    priv->mReuse = true;
    return priv;
}

bool Camera::isValidStreamSet(Stream **streams, int count)
{
    int inputs = 0;
    int outputs = 0;

    if (streams == NULL) {
        ALOGE("%s:%d: NULL stream configuration streams", __func__, mId);
        return false;
    }
    if (count == 0) {
        ALOGE("%s:%d: Zero count stream configuration streams", __func__, mId);
        return false;
    }
    // Validate there is at most one input stream and at least one output stream
    for (int i = 0; i < count; i++) {
        // A stream may be both input and output (bidirectional)
        if (streams[i]->isInputType())
            inputs++;
        if (streams[i]->isOutputType())
            outputs++;
    }
    if (outputs < 1) {
        ALOGE("%s:%d: Stream config must have >= 1 output", __func__, mId);
        return false;
    }
    if (inputs > 1) {
        ALOGE("%s:%d: Stream config must have <= 1 input", __func__, mId);
        return false;
    }
    // TODO: check for correct number of Bayer/YUV/JPEG/Encoder streams
    return true;
}

void Camera::setupStreams(Stream **streams, int count)
{
    /*
     * This is where the HAL has to decide internally how to handle all of the
     * streams, and then produce usage and max_buffer values for each stream.
     * Note, the stream array has been checked before this point for ALL invalid
     * conditions, so it must find a successful configuration for this stream
     * array.  The HAL may not return an error from this point.
     *
     * In this demo HAL, we just set all streams to be the same dummy values;
     * real implementations will want to avoid USAGE_SW_{READ|WRITE}_OFTEN.
     */
    for (int i = 0; i < count; i++) {
        uint32_t usage = 0;

        if (streams[i]->isOutputType())
            usage |= GRALLOC_USAGE_SW_WRITE_OFTEN |
                     GRALLOC_USAGE_HW_CAMERA_WRITE;
        if (streams[i]->isInputType())
            usage |= GRALLOC_USAGE_SW_READ_OFTEN |
                     GRALLOC_USAGE_HW_CAMERA_READ;

        streams[i]->setUsage(usage);
        streams[i]->setMaxBuffers(1);
    }
}

int Camera::registerStreamBuffers(const camera3_stream_buffer_set_t *buf_set)
{
    ALOGV("%s:%d: buffer_set=%p", __func__, mId, buf_set);
    if (buf_set == NULL) {
        ALOGE("%s:%d: NULL buffer set", __func__, mId);
        return -EINVAL;
    }
    if (buf_set->stream == NULL) {
        ALOGE("%s:%d: NULL stream handle", __func__, mId);
        return -EINVAL;
    }
    Stream *stream = reinterpret_cast<Stream*>(buf_set->stream->priv);
    return stream->registerBuffers(buf_set);
}

const camera_metadata_t* Camera::constructDefaultRequestSettings(int type)
{
    ALOGV("%s:%d: type=%d", __func__, mId, type);
    // TODO: return statically built default request
    return NULL;
}

int Camera::processCaptureRequest(camera3_capture_request_t *request)
{
    ALOGV("%s:%d: request=%p", __func__, mId, request);
    CAMTRACE_CALL();

    if (request == NULL) {
        ALOGE("%s:%d: NULL request recieved", __func__, mId);
        return -EINVAL;
    }

    // TODO: verify request; submit request to hardware
    return 0;
}

void Camera::getMetadataVendorTagOps(vendor_tag_query_ops_t *ops)
{
    ALOGV("%s:%d: ops=%p", __func__, mId, ops);
    // TODO: return vendor tag ops
}

void Camera::dump(int fd)
{
    ALOGV("%s:%d: Dumping to fd %d", __func__, mId, fd);
    // TODO: dprintf all relevant state to fd
}

extern "C" {
// Get handle to camera from device priv data
static Camera *camdev_to_camera(const camera3_device_t *dev)
{
    return reinterpret_cast<Camera*>(dev->priv);
}

static int initialize(const camera3_device_t *dev,
        const camera3_callback_ops_t *callback_ops)
{
    return camdev_to_camera(dev)->initialize(callback_ops);
}

static int configure_streams(const camera3_device_t *dev,
        camera3_stream_configuration_t *stream_list)
{
    return camdev_to_camera(dev)->configureStreams(stream_list);
}

static int register_stream_buffers(const camera3_device_t *dev,
        const camera3_stream_buffer_set_t *buffer_set)
{
    return camdev_to_camera(dev)->registerStreamBuffers(buffer_set);
}

static const camera_metadata_t *construct_default_request_settings(
        const camera3_device_t *dev, int type)
{
    return camdev_to_camera(dev)->constructDefaultRequestSettings(type);
}

static int process_capture_request(const camera3_device_t *dev,
        camera3_capture_request_t *request)
{
    return camdev_to_camera(dev)->processCaptureRequest(request);
}

static void get_metadata_vendor_tag_ops(const camera3_device_t *dev,
        vendor_tag_query_ops_t *ops)
{
    camdev_to_camera(dev)->getMetadataVendorTagOps(ops);
}

static void dump(const camera3_device_t *dev, int fd)
{
    camdev_to_camera(dev)->dump(fd);
}
} // extern "C"

const camera3_device_ops_t Camera::sOps = {
    .initialize              = default_camera_hal::initialize,
    .configure_streams       = default_camera_hal::configure_streams,
    .register_stream_buffers = default_camera_hal::register_stream_buffers,
    .construct_default_request_settings =
            default_camera_hal::construct_default_request_settings,
    .process_capture_request = default_camera_hal::process_capture_request,
    .get_metadata_vendor_tag_ops =
            default_camera_hal::get_metadata_vendor_tag_ops,
    .dump                    = default_camera_hal::dump
};

} // namespace default_camera_hal