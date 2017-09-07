// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef C2_VDA_COMPONENT_H_
#define C2_VDA_COMPONENT_H_

#include <map>
#include <unordered_map>

#include "VideoDecodeAcceleratorAdaptor.h"
#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/single_thread_task_runner.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/thread.h"
#include "rect.h"
#include "size.h"
#include "video_codecs.h"
#include "video_decode_accelerator.h"

#include <C2Component.h>
#include <C2Param.h>

namespace android {

C2ENUM(
    ColorFormat, uint32_t,
    kColorFormatYUV420Flexible = 0x7F420888,
)

class C2VDAComponentIntf : public C2ComponentInterface {
public:
    C2VDAComponentIntf(C2String name, node_id id);
    virtual ~C2VDAComponentIntf() {}

    // Impementation of C2ComponentInterface interface
    virtual C2String getName() const override;
    virtual node_id getId() const override;
    virtual status_t query_nb(
            const std::vector<C2Param* const> &stackParams,
            const std::vector<C2Param::Index> &heapParamIndices,
            std::vector<std::unique_ptr<C2Param>>* const heapParams) const override;
    virtual status_t config_nb(
            const std::vector<C2Param* const>& params,
            std::vector<std::unique_ptr<C2SettingResult>>* const failures) override;
    virtual status_t commit_sm(
            const std::vector<C2Param* const>& params,
            std::vector<std::unique_ptr<C2SettingResult>>* const failures) override;
    virtual status_t createTunnel_sm(node_id targetComponent) override;
    virtual status_t releaseTunnel_sm(node_id targetComponent) override;
    virtual std::shared_ptr<C2ParamReflector> getParamReflector() const override;
    virtual status_t getSupportedParams(
            std::vector<std::shared_ptr<C2ParamDescriptor>>* const params) const override;
    virtual status_t getSupportedValues(
            const std::vector<const C2ParamField>& fields,
            std::vector<C2FieldSupportedValues>* const values) const override;

private:
    const C2String kName;
    const node_id kId;
    //TODO: in the future different codec (h264/vp8/vp9) would be different class inherited from a
    //      base class. This static const should be moved to each super class.
    static const uint32_t kInputFormatFourcc;

    class ParamReflector;

    C2Param* getParamByIndex(uint32_t index) const;
    template<class T>
    std::unique_ptr<C2SettingResult> validateVideoSizeConfig(C2Param* c2Param) const;
    template<class T>
    std::unique_ptr<C2SettingResult> validateUint32Config(C2Param* c2Param) const;

    std::shared_ptr<C2ParamReflector> mParamReflector;

    // The following parameters are read-only.

    // The component domain; should be C2DomainVideo.
    C2ComponentDomainInfo mDomainInfo;
    // The color format of video output.
    C2StreamFormatConfig::output mOutputColorFormat;
    // The MIME type of input port.
    std::unique_ptr<C2PortMimeConfig::input> mInputPortMime;
    // The MIME type of output port; should be MEDIA_MIMETYPE_VIDEO_RAW.
    std::unique_ptr<C2PortMimeConfig::output> mOutputPortMime;

    // The following parameters are also writable.

    // The input video codec profile.
    C2StreamFormatConfig::input mInputCodecProfile;
    // Decoded video size for output.
    C2VideoSizeStreamInfo::output mVideoSize;
    // Max video size for video decoder.
    C2MaxVideoSizeHintPortSetting::input mMaxVideoSizeHint;

    std::unordered_map<uint32_t, C2Param*> mParams;
    // C2ParamField is LessThanComparable
    std::map<C2ParamField, C2FieldSupportedValues> mSupportedValues;
    std::vector<std::shared_ptr<C2ParamDescriptor>> mParamDescs;

    media::VideoDecodeAccelerator::SupportedProfiles mSupportedProfiles;
    std::vector<uint32_t> mSupportedCodecProfiles;
    media::Size mMaxVideoSize;
    media::Size mMinVideoSize;

};

class C2VDAComponent
    : public C2Component,
      public VideoDecodeAcceleratorAdaptor::Client,
      public std::enable_shared_from_this<C2VDAComponent> {
public:
    C2VDAComponent(
            C2String name, node_id id, const std::shared_ptr<C2ComponentListener>& listener);
    virtual ~C2VDAComponent() override;

    // Implementation of C2Component interface
    virtual status_t queue_nb(std::list<std::unique_ptr<C2Work>>* const items) override;
    virtual status_t announce_nb(const std::vector<C2WorkOutline>& items) override;
    virtual status_t flush_sm(
            bool flushThrough, std::list<std::unique_ptr<C2Work>>* const flushedWork) override;
    virtual status_t drain_nb(bool drainThrough) override;
    virtual status_t start() override;
    virtual status_t stop() override;
    virtual void reset() override;
    virtual void release() override;
    virtual std::shared_ptr<C2ComponentInterface> intf() override;

    // Implementation of VideDecodeAcceleratorAdaptor::Client interface
    virtual void providePictureBuffers(uint32_t pixelFormat,
                                       uint32_t minNumBuffers,
                                       const media::Size& codedSize) override;
    virtual void dismissPictureBuffer(int32_t pictureBufferId) override;
    virtual void pictureReady(int32_t pictureBufferId, int32_t bitstreamId,
                              const media::Rect& cropRect) override;
    virtual void notifyEndOfBitstreamBuffer(int32_t bitstreamId) override;
    virtual void notifyFlushDone() override;
    virtual void notifyResetDone() override;
    virtual void notifyError(VideoDecodeAcceleratorAdaptor::Result error) override;
private:
    // The state machine enumeration on parent thread.
    enum class State : int32_t {
        // The initial state of component. State will change to LOADED after the component is
        // created.
        UNLOADED,
        // The component is stopped. State will change to RUNNING when start() is called by
        // framework.
        LOADED,
        // The component is running, State will change to LOADED when stop() or reset() is called by
        // framework.
        RUNNING,
        // The component is in error state.
        ERROR,
    };
    // The state machine enumeration on component thread.
    enum class ComponentState : int32_t {
        // This is the initial state until VDA initialization returns successfully.
        UNINITIALIZED,
        // VDA initialization returns successfully. VDA is ready to make progress.
        STARTED,
        // onFlush() is called. VDA is flushing. State will change to STARTED after onFlushDone().
        FLUSHING,
        // onStop() is called. VDA is shutting down. State will change to UNINITIALIZED after
        // onStopDone().
        STOPPING,
        // onError() is called.
        ERROR,
    };

    // Get configured parameters from component interface. This should be called once framework
    // wants to start the component.
    void getParameters();

    // These tasks should be run on the component thread |mThread|.
    void onCreate();
    void onDestroy();
    void onStart(media::VideoCodecProfile profile, base::WaitableEvent* done);
    void onStop(base::WaitableEvent* done);
    void onStopDone();

    // The pointer of component interface.
    const std::shared_ptr<C2VDAComponentIntf> mIntf;
    // The pointer of component listener.
    const std::shared_ptr<C2ComponentListener> mListener;

    // The main component thread.
    base::Thread mThread;
    // The task runner on component thread.
    scoped_refptr<base::SingleThreadTaskRunner> mTaskRunner;

    // The following members should be utilized on component thread |mThread|.

    // The initialization result retrieved from VDA.
    VideoDecodeAcceleratorAdaptor::Result mVDAInitResult;
    // The pointer of VideoDecodeAcceleratorAdaptor.
    std::unique_ptr<VideoDecodeAcceleratorAdaptor> mVDAAdaptor;
    // The done event pointer of stop procedure. It should be restored in onStop() and signaled in
    // onStopDone().
    base::WaitableEvent* mStopDoneEvent;
    // The state machine on component thread.
    ComponentState mComponentState;

    // The following members should be utilized on parent thread.

    // The input codec profile which is configured in component interface.
    media::VideoCodecProfile mCodecProfile;
    // The state machine on parent thread.
    State mState;

    DISALLOW_COPY_AND_ASSIGN(C2VDAComponent);
};

class C2VDAComponentStore : public C2ComponentStore {
public:
    C2VDAComponentStore() {}
    ~C2VDAComponentStore() override {}

    status_t createComponent(C2String name,
                             std::shared_ptr<C2Component>* const component) override;

    status_t createInterface(C2String name,
                             std::shared_ptr<C2ComponentInterface>* const interface) override;

    std::vector<std::unique_ptr<const C2ComponentInfo>> getComponents() override;

    status_t copyBuffer(std::shared_ptr<C2GraphicBuffer> src,
                        std::shared_ptr<C2GraphicBuffer> dst) override;

    status_t query_nb(const std::vector<C2Param* const>& stackParams,
                      const std::vector<C2Param::Index>& heapParamIndices,
                      std::vector<std::unique_ptr<C2Param>>* const heapParams) override;

    status_t config_nb(const std::vector<C2Param* const>& params,
                       std::list<std::unique_ptr<C2SettingResult>>* const failures) override;
};

}  // namespace android

#endif  // C2_VDA_COMPONENT_H_