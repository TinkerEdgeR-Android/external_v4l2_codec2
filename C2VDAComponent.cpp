// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define LOG_TAG "C2VDAComponent"

#ifdef V4L2_CODEC2_ARC
#include <C2VDAAdaptorProxy.h>
#else
#include <C2VDAAdaptor.h>
#endif

#define __C2_GENERATE_GLOBAL_VARS__
#include <C2VDAAllocatorStore.h>
#include <C2VDAComponent.h>
#include <C2VDAPixelFormat.h>
#include <C2VDASupport.h>  // to getParamReflector from vda store
#include <C2VdaBqBlockPool.h>
#include <C2VdaPooledBlockPool.h>

#include <videodev2_custom.h>

#include <C2AllocatorGralloc.h>
#include <C2ComponentFactory.h>
#include <C2PlatformSupport.h>

#include <base/bind.h>
#include <base/bind_helpers.h>

#include <media/stagefright/MediaDefs.h>
#include <utils/Log.h>
#include <utils/misc.h>

#include <inttypes.h>
#include <string.h>
#include <algorithm>
#include <string>

#define UNUSED(expr)  \
    do {              \
        (void)(expr); \
    } while (0)

namespace android {

namespace {

// Mask against 30 bits to avoid (undefined) wraparound on signed integer.
int32_t frameIndexToBitstreamId(c2_cntr64_t frameIndex) {
    return static_cast<int32_t>(frameIndex.peeku() & 0x3FFFFFFF);
}

// Use basic graphic block pool/allocator as default.
const C2BlockPool::local_id_t kDefaultOutputBlockPool = C2BlockPool::BASIC_GRAPHIC;

const C2String kH264DecoderName = "c2.vda.avc.decoder";
const C2String kVP8DecoderName = "c2.vda.vp8.decoder";
const C2String kVP9DecoderName = "c2.vda.vp9.decoder";
const C2String kH264SecureDecoderName = "c2.vda.avc.decoder.secure";
const C2String kVP8SecureDecoderName = "c2.vda.vp8.decoder.secure";
const C2String kVP9SecureDecoderName = "c2.vda.vp9.decoder.secure";

const uint32_t kDpbOutputBufferExtraCount = 3;  // Use the same number as ACodec.
const int kDequeueRetryDelayUs = 10000;  // Wait time of dequeue buffer retry in microseconds.
const int32_t kAllocateBufferMaxRetries = 10;  // Max retry time for fetchGraphicBlock timeout.
}  // namespace

C2VDAComponent::IntfImpl::IntfImpl(C2String name, const std::shared_ptr<C2ReflectorHelper>& helper)
      : C2InterfaceHelper(helper), mInitStatus(C2_OK) {
    setDerivedInstance(this);

    // TODO(johnylin): use factory function to determine whether V4L2 stream or slice API is.
    uint32_t inputFormatFourcc;
    char inputMime[128];
    if (name == kH264DecoderName || name == kH264SecureDecoderName) {
        strcpy(inputMime, MEDIA_MIMETYPE_VIDEO_AVC);
        inputFormatFourcc = V4L2_PIX_FMT_H264_SLICE;
    } else if (name == kVP8DecoderName || name == kVP8SecureDecoderName) {
        strcpy(inputMime, MEDIA_MIMETYPE_VIDEO_VP8);
        inputFormatFourcc = V4L2_PIX_FMT_VP8_FRAME;
    } else if (name == kVP9DecoderName || name == kVP9SecureDecoderName) {
        strcpy(inputMime, MEDIA_MIMETYPE_VIDEO_VP9);
        inputFormatFourcc = V4L2_PIX_FMT_VP9_FRAME;
    } else {
        ALOGE("Invalid component name: %s", name.c_str());
        mInitStatus = C2_BAD_VALUE;
        return;
    }
    // Get supported profiles from VDA.
    // TODO: re-think the suitable method of getting supported profiles for both pure Android and
    //       ARC++.
    media::VideoDecodeAccelerator::SupportedProfiles supportedProfiles;
#ifdef V4L2_CODEC2_ARC
    supportedProfiles = arc::C2VDAAdaptorProxy::GetSupportedProfiles(inputFormatFourcc);
#else
    supportedProfiles = C2VDAAdaptor::GetSupportedProfiles(inputFormatFourcc);
#endif
    if (supportedProfiles.empty()) {
        ALOGE("No supported profile from input format: %u", inputFormatFourcc);
        mInitStatus = C2_BAD_VALUE;
        return;
    }

    mCodecProfile = supportedProfiles[0].profile;

    auto minSize = supportedProfiles[0].min_resolution;
    auto maxSize = supportedProfiles[0].max_resolution;

    addParameter(
            DefineParam(mInputFormat, C2_PARAMKEY_INPUT_STREAM_BUFFER_TYPE)
                    .withConstValue(new C2StreamBufferTypeSetting::input(0u, C2FormatCompressed))
                    .build());

    addParameter(DefineParam(mOutputFormat, C2_PARAMKEY_OUTPUT_STREAM_BUFFER_TYPE)
                         .withConstValue(new C2StreamBufferTypeSetting::output(0u, C2FormatVideo))
                         .build());

    addParameter(
            DefineParam(mInputMediaType, C2_PARAMKEY_INPUT_MEDIA_TYPE)
                    .withConstValue(AllocSharedString<C2PortMediaTypeSetting::input>(inputMime))
                    .build());

    addParameter(DefineParam(mOutputMediaType, C2_PARAMKEY_OUTPUT_MEDIA_TYPE)
                         .withConstValue(AllocSharedString<C2PortMediaTypeSetting::output>(
                                 MEDIA_MIMETYPE_VIDEO_RAW))
                         .build());

    struct LocalSetter {
        static C2R SizeSetter(bool mayBlock, C2P<C2StreamPictureSizeInfo::output>& videoSize) {
            (void)mayBlock;
            // TODO: maybe apply block limit?
            return videoSize.F(videoSize.v.width)
                    .validatePossible(videoSize.v.width)
                    .plus(videoSize.F(videoSize.v.height).validatePossible(videoSize.v.height));
        }
    };

    addParameter(DefineParam(mSize, C2_PARAMKEY_STREAM_PICTURE_SIZE)
                         .withDefault(new C2StreamPictureSizeInfo::output(0u, 176, 144))
                         .withFields({
                                 C2F(mSize, width).inRange(minSize.width(), maxSize.width(), 16),
                                 C2F(mSize, height).inRange(minSize.height(), maxSize.height(), 16),
                         })
                         .withSetter(LocalSetter::SizeSetter)
                         .build());

    bool secureMode = name.find(".secure") != std::string::npos;
    C2Allocator::id_t inputAllocators[] = {secureMode ? C2VDAAllocatorStore::SECURE_LINEAR
                                                      : C2PlatformAllocatorStore::ION};

    C2Allocator::id_t outputAllocators[] = {C2VDAAllocatorStore::V4L2_BUFFERPOOL};

    C2Allocator::id_t surfaceAllocator = secureMode ? C2VDAAllocatorStore::SECURE_GRAPHIC
                                                    : C2VDAAllocatorStore::V4L2_BUFFERQUEUE;

    addParameter(
            DefineParam(mInputAllocatorIds, C2_PARAMKEY_INPUT_ALLOCATORS)
                    .withConstValue(C2PortAllocatorsTuning::input::AllocShared(inputAllocators))
                    .build());

    addParameter(
            DefineParam(mOutputAllocatorIds, C2_PARAMKEY_OUTPUT_ALLOCATORS)
                    .withConstValue(C2PortAllocatorsTuning::output::AllocShared(outputAllocators))
                    .build());

    addParameter(DefineParam(mOutputSurfaceAllocatorId, C2_PARAMKEY_OUTPUT_SURFACE_ALLOCATOR)
                         .withConstValue(new C2PortSurfaceAllocatorTuning::output(surfaceAllocator))
                         .build());

    C2BlockPool::local_id_t outputBlockPools[] = {kDefaultOutputBlockPool};

    addParameter(
            DefineParam(mOutputBlockPoolIds, C2_PARAMKEY_OUTPUT_BLOCK_POOLS)
                    .withDefault(C2PortBlockPoolsTuning::output::AllocShared(outputBlockPools))
                    .withFields({C2F(mOutputBlockPoolIds, m.values[0]).any(),
                                 C2F(mOutputBlockPoolIds, m.values).inRange(0, 1)})
                    .withSetter(Setter<C2PortBlockPoolsTuning::output>::NonStrictValuesWithNoDeps)
                    .build());
}

////////////////////////////////////////////////////////////////////////////////
#define EXPECT_STATE_OR_RETURN_ON_ERROR(x)                    \
    do {                                                      \
        if (mComponentState == ComponentState::ERROR) return; \
        CHECK_EQ(mComponentState, ComponentState::x);         \
    } while (0)

#define EXPECT_RUNNING_OR_RETURN_ON_ERROR()                       \
    do {                                                          \
        if (mComponentState == ComponentState::ERROR) return;     \
        CHECK_NE(mComponentState, ComponentState::UNINITIALIZED); \
    } while (0)

C2VDAComponent::VideoFormat::VideoFormat(HalPixelFormat pixelFormat, uint32_t minNumBuffers,
                                         media::Size codedSize, media::Rect visibleRect)
      : mPixelFormat(pixelFormat),
        mMinNumBuffers(minNumBuffers),
        mCodedSize(codedSize),
        mVisibleRect(visibleRect) {}

C2VDAComponent::C2VDAComponent(C2String name, c2_node_id_t id,
                               const std::shared_ptr<C2ReflectorHelper>& helper)
      : mIntfImpl(std::make_shared<IntfImpl>(name, helper)),
        mIntf(std::make_shared<SimpleInterface<IntfImpl>>(name.c_str(), id, mIntfImpl)),
        mThread("C2VDAComponentThread"),
        mDequeueThread("C2VDAComponentDequeueThread"),
        mVDAInitResult(VideoDecodeAcceleratorAdaptor::Result::ILLEGAL_STATE),
        mComponentState(ComponentState::UNINITIALIZED),
        mPendingOutputEOS(false),
        mCodecProfile(media::VIDEO_CODEC_PROFILE_UNKNOWN),
        mState(State::UNLOADED),
        mWeakThisFactory(this) {
    // TODO(johnylin): the client may need to know if init is failed.
    if (mIntfImpl->status() != C2_OK) {
        ALOGE("Component interface init failed (err code = %d)", mIntfImpl->status());
        return;
    }

    mSecureMode = name.find(".secure") != std::string::npos;
    if (!mThread.Start()) {
        ALOGE("Component thread failed to start.");
        return;
    }
    mTaskRunner = mThread.task_runner();
    mState.store(State::LOADED);
}

C2VDAComponent::~C2VDAComponent() {
    CHECK_EQ(mState.load(), State::LOADED);

    if (mThread.IsRunning()) {
        mTaskRunner->PostTask(FROM_HERE,
                              ::base::Bind(&C2VDAComponent::onDestroy, ::base::Unretained(this)));
        mThread.Stop();
    }
}

void C2VDAComponent::onDestroy() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onDestroy");
    if (mVDAAdaptor.get()) {
        mVDAAdaptor->destroy();
        mVDAAdaptor.reset(nullptr);
    }
    stopDequeueThread();
}

void C2VDAComponent::onStart(media::VideoCodecProfile profile, ::base::WaitableEvent* done) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onStart");
    CHECK_EQ(mComponentState, ComponentState::UNINITIALIZED);

#ifdef V4L2_CODEC2_ARC
    mVDAAdaptor.reset(new arc::C2VDAAdaptorProxy());
#else
    mVDAAdaptor.reset(new C2VDAAdaptor());
#endif

    mVDAInitResult = mVDAAdaptor->initialize(profile, mSecureMode, this);
    if (mVDAInitResult == VideoDecodeAcceleratorAdaptor::Result::SUCCESS) {
        mComponentState = ComponentState::STARTED;
    }

    done->Signal();
}

void C2VDAComponent::onQueueWork(std::unique_ptr<C2Work> work) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onQueueWork: flags=0x%x, index=%llu, timestamp=%llu", work->input.flags,
          work->input.ordinal.frameIndex.peekull(), work->input.ordinal.timestamp.peekull());
    EXPECT_RUNNING_OR_RETURN_ON_ERROR();

    uint32_t drainMode = NO_DRAIN;
    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        drainMode = DRAIN_COMPONENT_WITH_EOS;
    }
    mQueue.push({std::move(work), drainMode});
    // TODO(johnylin): set a maximum size of mQueue and check if mQueue is already full.

    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VDAComponent::onDequeueWork, ::base::Unretained(this)));
}

void C2VDAComponent::onDequeueWork() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onDequeueWork");
    EXPECT_RUNNING_OR_RETURN_ON_ERROR();
    if (mQueue.empty()) {
        return;
    }
    if (mComponentState == ComponentState::DRAINING ||
        mComponentState == ComponentState::FLUSHING) {
        ALOGV("Temporarily stop dequeueing works since component is draining/flushing.");
        return;
    }
    if (mComponentState != ComponentState::STARTED) {
        ALOGE("Work queue should be empty if the component is not in STARTED state.");
        return;
    }

    // Dequeue a work from mQueue.
    std::unique_ptr<C2Work> work(std::move(mQueue.front().mWork));
    auto drainMode = mQueue.front().mDrainMode;
    mQueue.pop();

    CHECK_LE(work->input.buffers.size(), 1u);
    if (work->input.buffers.empty()) {
        // Client may queue a work with no input buffer for either it's EOS or empty CSD, otherwise
        // every work must have one input buffer.
        CHECK(drainMode != NO_DRAIN || work->input.flags & C2FrameData::FLAG_CODEC_CONFIG);
        // Emplace a nullptr to unify the check for work done.
        ALOGV("Got a work with no input buffer! Emplace a nullptr inside.");
        work->input.buffers.emplace_back(nullptr);
    } else {
        // If input.buffers is not empty, the buffer should have meaningful content inside.
        C2ConstLinearBlock linearBlock = work->input.buffers.front()->data().linearBlocks().front();
        CHECK_GT(linearBlock.size(), 0u);
        // Send input buffer to VDA for decode.
        // Use frameIndex as bitstreamId.
        int32_t bitstreamId = frameIndexToBitstreamId(work->input.ordinal.frameIndex);
        sendInputBufferToAccelerator(linearBlock, bitstreamId);
    }

    CHECK_EQ(work->worklets.size(), 1u);
    work->worklets.front()->output.flags = static_cast<C2FrameData::flags_t>(0);
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.ordinal = work->input.ordinal;

    if (drainMode != NO_DRAIN) {
        mVDAAdaptor->flush();
        mComponentState = ComponentState::DRAINING;
        mPendingOutputEOS = drainMode == DRAIN_COMPONENT_WITH_EOS;
    }

    // Put work to mPendingWorks.
    mPendingWorks.emplace_back(std::move(work));

    if (!mQueue.empty()) {
        mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VDAComponent::onDequeueWork,
                                                      ::base::Unretained(this)));
    }
}

void C2VDAComponent::onInputBufferDone(int32_t bitstreamId) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onInputBufferDone: bitstream id=%d", bitstreamId);
    EXPECT_RUNNING_OR_RETURN_ON_ERROR();

    C2Work* work = getPendingWorkByBitstreamId(bitstreamId);
    if (!work) {
        reportError(C2_CORRUPTED);
        return;
    }

    // When the work is done, the input buffer shall be reset by component.
    work->input.buffers.front().reset();

    reportFinishedWorkIfAny();
}

void C2VDAComponent::onOutputBufferReturned(std::shared_ptr<C2GraphicBlock> block,
                                            uint32_t poolId) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onOutputBufferReturned: pool id=%u", poolId);
    if (mComponentState == ComponentState::UNINITIALIZED) {
        // Output buffer is returned from client after component is stopped. Just let the buffer be
        // released.
        return;
    }

    if (block->width() != static_cast<uint32_t>(mOutputFormat.mCodedSize.width()) ||
        block->height() != static_cast<uint32_t>(mOutputFormat.mCodedSize.height())) {
        // Output buffer is returned after we changed output resolution. Just let the buffer be
        // released.
        ALOGV("Discard obsolete graphic block: pool id=%u", poolId);
        return;
    }

    GraphicBlockInfo* info = getGraphicBlockByPoolId(poolId);
    if (!info) {
        reportError(C2_CORRUPTED);
        return;
    }
    CHECK_EQ(info->mState, GraphicBlockInfo::State::OWNED_BY_CLIENT);
    info->mGraphicBlock = std::move(block);
    info->mState = GraphicBlockInfo::State::OWNED_BY_COMPONENT;

    if (mPendingOutputFormat) {
        tryChangeOutputFormat();
    } else {
        sendOutputBufferToAccelerator(info);
    }
}

void C2VDAComponent::onOutputBufferDone(int32_t pictureBufferId, int32_t bitstreamId) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onOutputBufferDone: picture id=%d, bitstream id=%d", pictureBufferId, bitstreamId);
    EXPECT_RUNNING_OR_RETURN_ON_ERROR();

    C2Work* work = getPendingWorkByBitstreamId(bitstreamId);
    if (!work) {
        reportError(C2_CORRUPTED);
        return;
    }
    GraphicBlockInfo* info = getGraphicBlockById(pictureBufferId);
    if (!info) {
        reportError(C2_CORRUPTED);
        return;
    }
    CHECK_EQ(info->mState, GraphicBlockInfo::State::OWNED_BY_ACCELERATOR);
    // Output buffer will be passed to client soon along with mListener->onWorkDone_nb().
    info->mState = GraphicBlockInfo::State::OWNED_BY_CLIENT;
    mBuffersInClient++;

    // Attach output buffer to the work corresponded to bitstreamId.
    C2ConstGraphicBlock constBlock = info->mGraphicBlock->share(
            C2Rect(mOutputFormat.mVisibleRect.width(), mOutputFormat.mVisibleRect.height()),
            C2Fence());
    MarkBlockPoolDataAsShared(constBlock);
    work->worklets.front()->output.buffers.emplace_back(
            C2Buffer::CreateGraphicBuffer(std::move(constBlock)));
    info->mGraphicBlock.reset();

    reportFinishedWorkIfAny();
}

void C2VDAComponent::onDrain(uint32_t drainMode) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onDrain: mode = %u", drainMode);
    EXPECT_RUNNING_OR_RETURN_ON_ERROR();

    if (!mQueue.empty()) {
        // Mark last queued work as "drain-till-here" by setting drainMode. Do not change drainMode
        // if last work already has one.
        if (mQueue.back().mDrainMode == NO_DRAIN) {
            mQueue.back().mDrainMode = drainMode;
        }
    } else if (!mPendingWorks.empty()) {
        // Neglect drain request if component is not in STARTED mode. Otherwise, enters DRAINING
        // mode and signal VDA flush immediately.
        if (mComponentState == ComponentState::STARTED) {
            mVDAAdaptor->flush();
            mComponentState = ComponentState::DRAINING;
            mPendingOutputEOS = drainMode == DRAIN_COMPONENT_WITH_EOS;
        } else {
            ALOGV("Neglect drain. Component in state: %d", mComponentState);
        }
    } else {
        // Do nothing.
        ALOGV("No buffers in VDA, drain takes no effect.");
    }
}

void C2VDAComponent::onDrainDone() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onDrainDone");
    if (mComponentState == ComponentState::DRAINING) {
        mComponentState = ComponentState::STARTED;
    } else if (mComponentState == ComponentState::STOPPING) {
        // The client signals stop right before VDA notifies drain done. Let stop process goes.
        return;
    } else if (mComponentState != ComponentState::FLUSHING) {
        // It is reasonable to get onDrainDone in FLUSHING, which means flush is already signaled
        // and component should still expect onFlushDone callback from VDA.
        ALOGE("Unexpected state while onDrainDone(). State=%d", mComponentState);
        reportError(C2_BAD_STATE);
        return;
    }

    if (mPendingOutputEOS) {
        // Return EOS work.
        reportEOSWork();
    }
    // mPendingWorks must be empty after draining is finished.
    CHECK(mPendingWorks.empty());

    // Work dequeueing was stopped while component draining. Restart it.
    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VDAComponent::onDequeueWork, ::base::Unretained(this)));
}

void C2VDAComponent::onFlush() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onFlush");
    if (mComponentState == ComponentState::FLUSHING ||
        mComponentState == ComponentState::STOPPING) {
        return;  // Ignore other flush request when component is flushing or stopping.
    }
    EXPECT_RUNNING_OR_RETURN_ON_ERROR();

    mVDAAdaptor->reset();
    // Pop all works in mQueue and put into mAbandonedWorks.
    while (!mQueue.empty()) {
        mAbandonedWorks.emplace_back(std::move(mQueue.front().mWork));
        mQueue.pop();
    }
    mComponentState = ComponentState::FLUSHING;
}

void C2VDAComponent::onStop(::base::WaitableEvent* done) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onStop");
    EXPECT_RUNNING_OR_RETURN_ON_ERROR();

    // Do not request VDA reset again before the previous one is done. If reset is already sent by
    // onFlush(), just regard the following NotifyResetDone callback as for stopping.
    if (mComponentState != ComponentState::FLUSHING) {
        mVDAAdaptor->reset();
    }

    // Pop all works in mQueue and put into mAbandonedWorks.
    while (!mQueue.empty()) {
        mAbandonedWorks.emplace_back(std::move(mQueue.front().mWork));
        mQueue.pop();
    }

    mStopDoneEvent = done;  // restore done event which shoud be signaled in onStopDone().
    mComponentState = ComponentState::STOPPING;
}

void C2VDAComponent::onResetDone() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    if (mComponentState == ComponentState::ERROR) {
        return;
    }
    if (mComponentState == ComponentState::FLUSHING) {
        onFlushDone();
    } else if (mComponentState == ComponentState::STOPPING) {
        onStopDone();
    } else {
        reportError(C2_CORRUPTED);
    }
}

void C2VDAComponent::onFlushDone() {
    ALOGV("onFlushDone");
    reportAbandonedWorks();
    mComponentState = ComponentState::STARTED;

    // Work dequeueing was stopped while component flushing. Restart it.
    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VDAComponent::onDequeueWork, ::base::Unretained(this)));
}

void C2VDAComponent::onStopDone() {
    ALOGV("onStopDone");
    CHECK(mStopDoneEvent);

    // TODO(johnylin): At this moment, there may be C2Buffer still owned by client, do we need to
    // do something for them?
    reportAbandonedWorks();
    mPendingOutputFormat.reset();
    if (mVDAAdaptor.get()) {
        mVDAAdaptor->destroy();
        mVDAAdaptor.reset(nullptr);
    }

    mGraphicBlocks.clear();

    stopDequeueThread();

    mStopDoneEvent->Signal();
    mStopDoneEvent = nullptr;
    mComponentState = ComponentState::UNINITIALIZED;
}

c2_status_t C2VDAComponent::setListener_vb(const std::shared_ptr<C2Component::Listener>& listener,
                                           c2_blocking_t mayBlock) {
    UNUSED(mayBlock);
    // TODO(johnylin): API says this method must be supported in all states, however I'm quite not
    //                 sure what is the use case.
    if (mState.load() != State::LOADED) {
        return C2_BAD_STATE;
    }
    mListener = listener;
    return C2_OK;
}

void C2VDAComponent::sendInputBufferToAccelerator(const C2ConstLinearBlock& input,
                                                  int32_t bitstreamId) {
    ALOGV("sendInputBufferToAccelerator");
    int dupFd = dup(input.handle()->data[0]);
    if (dupFd < 0) {
        ALOGE("Failed to dup(%d) input buffer (bitstreamId=%d), errno=%d", input.handle()->data[0],
              bitstreamId, errno);
        reportError(C2_CORRUPTED);
        return;
    }
    ALOGV("Decode bitstream ID: %d, offset: %u size: %u", bitstreamId, input.offset(),
          input.size());
    mVDAAdaptor->decode(bitstreamId, dupFd, input.offset(), input.size());
}

C2Work* C2VDAComponent::getPendingWorkByBitstreamId(int32_t bitstreamId) {
    auto workIter = std::find_if(mPendingWorks.begin(), mPendingWorks.end(),
                                 [bitstreamId](const std::unique_ptr<C2Work>& w) {
                                     return frameIndexToBitstreamId(w->input.ordinal.frameIndex) ==
                                            bitstreamId;
                                 });

    if (workIter == mPendingWorks.end()) {
        ALOGE("Can't find pending work by bitstream ID: %d", bitstreamId);
        return nullptr;
    }
    return workIter->get();
}

C2VDAComponent::GraphicBlockInfo* C2VDAComponent::getGraphicBlockById(int32_t blockId) {
    if (blockId < 0 || blockId >= static_cast<int32_t>(mGraphicBlocks.size())) {
        ALOGE("getGraphicBlockById failed: id=%d", blockId);
        return nullptr;
    }
    return &mGraphicBlocks[blockId];
}

C2VDAComponent::GraphicBlockInfo* C2VDAComponent::getGraphicBlockByPoolId(uint32_t poolId) {
    auto blockIter = std::find_if(mGraphicBlocks.begin(), mGraphicBlocks.end(),
                                  [poolId](const GraphicBlockInfo& gb) {
                                      return gb.mPoolId == poolId;
                                  });

    if (blockIter == mGraphicBlocks.end()) {
        ALOGE("getGraphicBlockByPoolId failed: poolId=%u", poolId);
        return nullptr;
    }
    return &(*blockIter);
}

void C2VDAComponent::onOutputFormatChanged(std::unique_ptr<VideoFormat> format) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onOutputFormatChanged");
    EXPECT_RUNNING_OR_RETURN_ON_ERROR();

    ALOGV("New output format(pixel_format=0x%x, min_num_buffers=%u, coded_size=%s, crop_rect=%s)",
          static_cast<uint32_t>(format->mPixelFormat), format->mMinNumBuffers,
          format->mCodedSize.ToString().c_str(), format->mVisibleRect.ToString().c_str());

    for (auto& info : mGraphicBlocks) {
        if (info.mState == GraphicBlockInfo::State::OWNED_BY_ACCELERATOR)
            info.mState = GraphicBlockInfo::State::OWNED_BY_COMPONENT;
    }

    CHECK(!mPendingOutputFormat);
    mPendingOutputFormat = std::move(format);
    tryChangeOutputFormat();
}

void C2VDAComponent::tryChangeOutputFormat() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("tryChangeOutputFormat");
    CHECK(mPendingOutputFormat);

    // At this point, all output buffers should not be owned by accelerator. The component is not
    // able to know when a client will release all owned output buffers by now. But it is ok to
    // leave them to client since componenet won't own those buffers anymore.
    // TODO(johnylin): we may also set a parameter for component to keep dequeueing buffers and
    //                 change format only after the component owns most buffers. This may prevent
    //                 too many buffers are still on client's hand while component starts to
    //                 allocate more buffers. However, it leads latency on output format change.
    for (const auto& info : mGraphicBlocks) {
        CHECK(info.mState != GraphicBlockInfo::State::OWNED_BY_ACCELERATOR);
    }

    CHECK_EQ(mPendingOutputFormat->mPixelFormat, HalPixelFormat::YCbCr_420_888);

    mOutputFormat.mPixelFormat = mPendingOutputFormat->mPixelFormat;
    mOutputFormat.mMinNumBuffers = mPendingOutputFormat->mMinNumBuffers;
    mOutputFormat.mCodedSize = mPendingOutputFormat->mCodedSize;

    setOutputFormatCrop(mPendingOutputFormat->mVisibleRect);

    c2_status_t err = allocateBuffersFromBlockAllocator(
            mPendingOutputFormat->mCodedSize,
            static_cast<uint32_t>(mPendingOutputFormat->mPixelFormat));
    if (err != C2_OK) {
        reportError(err);
        return;
    }

    for (auto& info : mGraphicBlocks) {
        sendOutputBufferToAccelerator(&info);
    }
    mPendingOutputFormat.reset();
}

c2_status_t C2VDAComponent::allocateBuffersFromBlockAllocator(const media::Size& size,
                                                              uint32_t pixelFormat) {
    ALOGV("allocateBuffersFromBlockAllocator(%s, 0x%x)", size.ToString().c_str(), pixelFormat);

    stopDequeueThread();

    size_t bufferCount = mOutputFormat.mMinNumBuffers + kDpbOutputBufferExtraCount;

    // Allocate the output buffers.
    mVDAAdaptor->assignPictureBuffers(bufferCount);

    // Get block pool ID configured from the client.
    std::shared_ptr<C2BlockPool> blockPool;
    auto poolId = mIntfImpl->getBlockPoolId();
    ALOGI("Using C2BlockPool ID = %" PRIu64 " for allocating output buffers", poolId);
    auto err = GetCodec2BlockPool(poolId, shared_from_this(), &blockPool);
    if (err != C2_OK) {
        ALOGE("Graphic block allocator is invalid");
        reportError(err);
        return err;
    }

    mGraphicBlocks.clear();

    bool useBufferQueue = blockPool->getAllocatorId() == C2PlatformAllocatorStore::BUFFERQUEUE;
    if (useBufferQueue) {
        ALOGV("Bufferqueue-backed block pool is used.");
        // Set requested buffer count to C2VdaBqBlockPool.
        std::shared_ptr<C2VdaBqBlockPool> bqPool =
                std::static_pointer_cast<C2VdaBqBlockPool>(blockPool);
        if (bqPool) {
            err = bqPool->requestNewBufferSet(static_cast<int32_t>(bufferCount));
            if (err != C2_OK) {
                ALOGE("failed to request new buffer set to block pool: %d", err);
                reportError(err);
                return err;
            }
        } else {
            ALOGE("static_pointer_cast C2VdaBqBlockPool failed...");
            reportError(C2_CORRUPTED);
            return C2_CORRUPTED;
        }
    } else {
        ALOGV("Bufferpool-backed block pool is used.");
        // Set requested buffer count to C2VdaPooledBlockPool.
        std::shared_ptr<C2VdaPooledBlockPool> bpPool =
                std::static_pointer_cast<C2VdaPooledBlockPool>(blockPool);
        if (bpPool) {
            err = bpPool->requestNewBufferSet(static_cast<int32_t>(bufferCount));
            if (err != C2_OK) {
                ALOGE("failed to request new buffer set to block pool: %d", err);
                reportError(err);
                return err;
            }
        } else {
            ALOGE("static_pointer_cast C2VdaPooledBlockPool failed...");
            reportError(C2_CORRUPTED);
            return C2_CORRUPTED;
        }
    }

    for (size_t i = 0; i < bufferCount; ++i) {
        std::shared_ptr<C2GraphicBlock> block;
        C2MemoryUsage usage = {
                mSecureMode ? C2MemoryUsage::READ_PROTECTED : C2MemoryUsage::CPU_READ, 0};

        int32_t retries_left = kAllocateBufferMaxRetries;
        err = C2_NO_INIT;
        while (err != C2_OK) {
            err = blockPool->fetchGraphicBlock(size.width(), size.height(), pixelFormat, usage,
                                               &block);
            if (err == C2_TIMED_OUT && retries_left > 0) {
                ALOGD("allocate buffer timeout, %d retry time(s) left...", retries_left);
                retries_left--;
            } else if (err != C2_OK) {
                mGraphicBlocks.clear();
                ALOGE("failed to allocate buffer: %d", err);
                reportError(err);
                return err;
            }
        }

        uint32_t poolId;
        if (useBufferQueue) {
            err = C2VdaBqBlockPool::getPoolIdFromGraphicBlock(block, &poolId);
        } else {  // use bufferpool
            err = C2VdaPooledBlockPool::getPoolIdFromGraphicBlock(block, &poolId);
        }
        if (err != C2_OK) {
            mGraphicBlocks.clear();
            ALOGE("failed to getPoolIdFromGraphicBlock: %d", err);
            reportError(err);
            return err;
        }
        if (mSecureMode) {
            appendSecureOutputBuffer(std::move(block), poolId);
        } else {
            appendOutputBuffer(std::move(block), poolId);
        }
    }
    mOutputFormat.mMinNumBuffers = bufferCount;

    if (!startDequeueThread(size, pixelFormat, std::move(blockPool))) {
        reportError(C2_CORRUPTED);
        return C2_CORRUPTED;
    }
    return C2_OK;
}

void C2VDAComponent::appendOutputBuffer(std::shared_ptr<C2GraphicBlock> block, uint32_t poolId) {
    GraphicBlockInfo info;
    info.mBlockId = static_cast<int32_t>(mGraphicBlocks.size());
    info.mGraphicBlock = std::move(block);
    info.mPoolId = poolId;

    C2ConstGraphicBlock constBlock = info.mGraphicBlock->share(
            C2Rect(info.mGraphicBlock->width(), info.mGraphicBlock->height()), C2Fence());

    const C2GraphicView& view = constBlock.map().get();
    const uint8_t* const* data = view.data();
    CHECK_NE(data, nullptr);
    const C2PlanarLayout& layout = view.layout();

    ALOGV("allocate graphic buffer: %p, id: %d, size: %dx%d", info.mGraphicBlock->handle(),
          info.mBlockId, info.mGraphicBlock->width(), info.mGraphicBlock->height());

    // get offset from data pointers
    uint32_t offsets[C2PlanarLayout::MAX_NUM_PLANES];
    auto baseAddress = reinterpret_cast<intptr_t>(data[0]);
    for (uint32_t i = 0; i < layout.numPlanes; ++i) {
        auto planeAddress = reinterpret_cast<intptr_t>(data[i]);
        offsets[i] = static_cast<uint32_t>(planeAddress - baseAddress);
    }

    bool crcb = false;
    if (layout.numPlanes == 3 &&
        offsets[C2PlanarLayout::PLANE_U] > offsets[C2PlanarLayout::PLANE_V]) {
        // YCrCb format
        std::swap(offsets[C2PlanarLayout::PLANE_U], offsets[C2PlanarLayout::PLANE_V]);
        crcb = true;
    }

    bool semiplanar = false;
    uint32_t passedNumPlanes = layout.numPlanes;
    if (layout.planes[C2PlanarLayout::PLANE_U].colInc == 2) {  // chroma_step
        // Semi-planar format
        passedNumPlanes--;
        semiplanar = true;
    }

    for (uint32_t i = 0; i < passedNumPlanes; ++i) {
        ALOGV("plane %u: stride: %d, offset: %u", i, layout.planes[i].rowInc, offsets[i]);
    }
    info.mPixelFormat = resolveBufferFormat(crcb, semiplanar);
    ALOGV("HAL pixel format: 0x%x", static_cast<uint32_t>(info.mPixelFormat));

    ::base::ScopedFD passedHandle(dup(info.mGraphicBlock->handle()->data[0]));
    if (!passedHandle.is_valid()) {
        ALOGE("Failed to dup(%d), errno=%d", info.mGraphicBlock->handle()->data[0], errno);
        reportError(C2_CORRUPTED);
        return;
    }
    std::vector<VideoFramePlane> passedPlanes;
    for (uint32_t i = 0; i < passedNumPlanes; ++i) {
        CHECK_GT(layout.planes[i].rowInc, 0);
        passedPlanes.push_back({offsets[i], static_cast<uint32_t>(layout.planes[i].rowInc)});
    }
    info.mHandle = std::move(passedHandle);
    info.mPlanes = std::move(passedPlanes);

    mGraphicBlocks.push_back(std::move(info));
}

void C2VDAComponent::appendSecureOutputBuffer(std::shared_ptr<C2GraphicBlock> block,
                                              uint32_t poolId) {
#ifdef V4L2_CODEC2_ARC
    const C2Handle* const handle = block->handle();
    const int handleFd = handle->data[0];
    ::base::ScopedFD passedHandle(dup(handleFd));
    if (!passedHandle.is_valid()) {
        ALOGE("Failed to dup(%d), errno=%d", handleFd, errno);
        reportError(C2_CORRUPTED);
        return;
    }

    android::HalPixelFormat pixelFormat = getPlatformPixelFormat();
    if (pixelFormat == android::HalPixelFormat::UNKNOWN) {
        ALOGE("Failed to get pixel format on platform.");
        reportError(C2_CORRUPTED);
        return;
    }
    CHECK(pixelFormat == android::HalPixelFormat::YV12 ||
          pixelFormat == android::HalPixelFormat::NV12);
    ALOGV("HAL pixel format: 0x%x", static_cast<uint32_t>(pixelFormat));

    GraphicBlockInfo info;
    info.mBlockId = static_cast<int32_t>(mGraphicBlocks.size());
    info.mGraphicBlock = std::move(block);
    info.mPoolId = poolId;
    info.mHandle = std::move(passedHandle);
    info.mPixelFormat = pixelFormat;
    // In secure mode, since planes are not referred in Chrome side, empty plane is valid.
    info.mPlanes.clear();
    mGraphicBlocks.push_back(std::move(info));
#else
    ALOGE("appendSecureOutputBuffer() is not supported...");
    reportError(C2_OMITTED);
#endif // V4L2_CODEC2_ARC
}

void C2VDAComponent::sendOutputBufferToAccelerator(GraphicBlockInfo* info) {
    ALOGV("sendOutputBufferToAccelerator index=%d", info->mBlockId);
    CHECK_EQ(info->mState, GraphicBlockInfo::State::OWNED_BY_COMPONENT);
    info->mState = GraphicBlockInfo::State::OWNED_BY_ACCELERATOR;

    // is_valid() is true for the first time the buffer is passed to VDA. In that case, VDA needs to
    // import the buffer first.
    if (info->mHandle.is_valid()) {
        mVDAAdaptor->importBufferForPicture(info->mBlockId, info->mPixelFormat,
                                            info->mHandle.release(), info->mPlanes);
    } else {
        mVDAAdaptor->reusePictureBuffer(info->mBlockId);
    }
}

void C2VDAComponent::onVisibleRectChanged(const media::Rect& cropRect) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onVisibleRectChanged");
    EXPECT_RUNNING_OR_RETURN_ON_ERROR();

    // We should make sure there is no pending output format change. That is, the input cropRect is
    // corresponding to current output format.
    CHECK(mPendingOutputFormat == nullptr);
    setOutputFormatCrop(cropRect);
}

void C2VDAComponent::setOutputFormatCrop(const media::Rect& cropRect) {
    ALOGV("setOutputFormatCrop(%dx%d)", cropRect.width(), cropRect.height());
    // This visible rect should be set as crop window for each C2ConstGraphicBlock passed to
    // framework.
    mOutputFormat.mVisibleRect = cropRect;
}

c2_status_t C2VDAComponent::queue_nb(std::list<std::unique_ptr<C2Work>>* const items) {
    if (mState.load() != State::RUNNING) {
        return C2_BAD_STATE;
    }
    while (!items->empty()) {
        mTaskRunner->PostTask(FROM_HERE,
                              ::base::Bind(&C2VDAComponent::onQueueWork, ::base::Unretained(this),
                                           ::base::Passed(&items->front())));
        items->pop_front();
    }
    return C2_OK;
}

c2_status_t C2VDAComponent::announce_nb(const std::vector<C2WorkOutline>& items) {
    UNUSED(items);
    return C2_OMITTED;  // Tunneling is not supported by now
}

c2_status_t C2VDAComponent::flush_sm(flush_mode_t mode,
                                     std::list<std::unique_ptr<C2Work>>* const flushedWork) {
    if (mode != FLUSH_COMPONENT) {
        return C2_OMITTED;  // Tunneling is not supported by now
    }
    if (mState.load() != State::RUNNING) {
        return C2_BAD_STATE;
    }
    mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VDAComponent::onFlush,
                                                  ::base::Unretained(this)));
    // Instead of |flushedWork|, abandoned works will be returned via onWorkDone_nb() callback.
    return C2_OK;
}

c2_status_t C2VDAComponent::drain_nb(drain_mode_t mode) {
    if (mode != DRAIN_COMPONENT_WITH_EOS && mode != DRAIN_COMPONENT_NO_EOS) {
        return C2_OMITTED;  // Tunneling is not supported by now
    }
    if (mState.load() != State::RUNNING) {
        return C2_BAD_STATE;
    }
    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VDAComponent::onDrain, ::base::Unretained(this),
                                       static_cast<uint32_t>(mode)));
    return C2_OK;
}

c2_status_t C2VDAComponent::start() {
    // Use mStartStopLock to block other asynchronously start/stop calls.
    std::lock_guard<std::mutex> lock(mStartStopLock);

    if (mState.load() != State::LOADED) {
        return C2_BAD_STATE;  // start() is only supported when component is in LOADED state.
    }

    mCodecProfile = mIntfImpl->getCodecProfile();
    ALOGI("get parameter: mCodecProfile = %d", static_cast<int>(mCodecProfile));

    ::base::WaitableEvent done(::base::WaitableEvent::ResetPolicy::AUTOMATIC,
                               ::base::WaitableEvent::InitialState::NOT_SIGNALED);
    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VDAComponent::onStart, ::base::Unretained(this),
                                       mCodecProfile, &done));
    done.Wait();
    if (mVDAInitResult != VideoDecodeAcceleratorAdaptor::Result::SUCCESS) {
        ALOGE("Failed to start component due to VDA error: %d", static_cast<int>(mVDAInitResult));
        return C2_CORRUPTED;
    }
    mState.store(State::RUNNING);
    return C2_OK;
}

c2_status_t C2VDAComponent::stop() {
    // Use mStartStopLock to block other asynchronously start/stop calls.
    std::lock_guard<std::mutex> lock(mStartStopLock);

    auto state = mState.load();
    if (!(state == State::RUNNING || state == State::ERROR)) {
        return C2_OK;  // Component is already in stopped state.
    }

    ::base::WaitableEvent done(::base::WaitableEvent::ResetPolicy::AUTOMATIC,
                               ::base::WaitableEvent::InitialState::NOT_SIGNALED);
    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VDAComponent::onStop, ::base::Unretained(this), &done));
    done.Wait();
    mState.store(State::LOADED);
    return C2_OK;
}

c2_status_t C2VDAComponent::reset() {
    return stop();
    // TODO(johnylin): reset is different than stop that it could be called in any state.
    // TODO(johnylin): when reset is called, set ComponentInterface to default values.
}

c2_status_t C2VDAComponent::release() {
    return reset();
}

std::shared_ptr<C2ComponentInterface> C2VDAComponent::intf() {
    return mIntf;
}

void C2VDAComponent::providePictureBuffers(uint32_t minNumBuffers, const media::Size& codedSize) {
    // Always use fexible pixel 420 format YCbCr_420_888 in Android.
    // Uses coded size for crop rect while it is not available.
    auto format = std::make_unique<VideoFormat>(HalPixelFormat::YCbCr_420_888, minNumBuffers,
                                                codedSize, media::Rect(codedSize));

    // Set mRequestedVisibleRect to default.
    mRequestedVisibleRect = media::Rect();

    mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VDAComponent::onOutputFormatChanged,
                                                  ::base::Unretained(this),
                                                  ::base::Passed(&format)));
}

void C2VDAComponent::dismissPictureBuffer(int32_t pictureBufferId) {
    UNUSED(pictureBufferId);
    // no ops
}

void C2VDAComponent::pictureReady(int32_t pictureBufferId, int32_t bitstreamId,
                                  const media::Rect& cropRect) {
    UNUSED(pictureBufferId);
    UNUSED(bitstreamId);

    if (mRequestedVisibleRect != cropRect) {
        mRequestedVisibleRect = cropRect;
        mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VDAComponent::onVisibleRectChanged,
                                                      ::base::Unretained(this), cropRect));
    }

    mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VDAComponent::onOutputBufferDone,
                                                  ::base::Unretained(this),
                                                  pictureBufferId, bitstreamId));
}

void C2VDAComponent::notifyEndOfBitstreamBuffer(int32_t bitstreamId) {
    mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VDAComponent::onInputBufferDone,
                                                  ::base::Unretained(this), bitstreamId));
}

void C2VDAComponent::notifyFlushDone() {
    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VDAComponent::onDrainDone, ::base::Unretained(this)));
}

void C2VDAComponent::notifyResetDone() {
    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VDAComponent::onResetDone, ::base::Unretained(this)));
}

void C2VDAComponent::notifyError(VideoDecodeAcceleratorAdaptor::Result error) {
    ALOGE("Got notifyError from VDA error=%d", error);
    c2_status_t err;
    switch (error) {
    case VideoDecodeAcceleratorAdaptor::Result::ILLEGAL_STATE:
        err = C2_BAD_STATE;
        break;
    case VideoDecodeAcceleratorAdaptor::Result::INVALID_ARGUMENT:
    case VideoDecodeAcceleratorAdaptor::Result::UNREADABLE_INPUT:
        err = C2_BAD_VALUE;
        break;
    case VideoDecodeAcceleratorAdaptor::Result::PLATFORM_FAILURE:
        err = C2_CORRUPTED;
        break;
    case VideoDecodeAcceleratorAdaptor::Result::INSUFFICIENT_RESOURCES:
        err = C2_NO_MEMORY;
        break;
    case VideoDecodeAcceleratorAdaptor::Result::SUCCESS:
        ALOGE("Shouldn't get SUCCESS err code in NotifyError(). Skip it...");
        return;
    }
    reportError(err);
}

void C2VDAComponent::reportFinishedWorkIfAny() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    std::list<std::unique_ptr<C2Work>> finishedWorks;

    // Work should be reported as done if both input and output buffer are returned by VDA.
    // EOS work will not be reported here. reportEOSWork() does it.
    auto iter = mPendingWorks.begin();
    while (iter != mPendingWorks.end()) {
        if (isWorkDone(iter->get())) {
            iter->get()->result = C2_OK;
            iter->get()->workletsProcessed = static_cast<uint32_t>(iter->get()->worklets.size());
            finishedWorks.emplace_back(std::move(*iter));
            iter = mPendingWorks.erase(iter);
        } else {
            ++iter;
        }
    }

    if (!finishedWorks.empty()) {
        mListener->onWorkDone_nb(shared_from_this(), std::move(finishedWorks));
    }
}

bool C2VDAComponent::isWorkDone(const C2Work* work) const {
    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        // This is EOS work and should be processed by reportEOSWork().
        return false;
    }
    if (work->input.buffers.front()) {
        // Input buffer is still owned by VDA.
        return false;
    }
    if (mPendingOutputEOS && mPendingWorks.size() == 1u) {
        // If mPendingOutputEOS is true, the last returned work should be marked EOS flag and
        // returned by reportEOSWork() instead.
        return false;
    }
    if (!(work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) &&
        work->worklets.front()->output.buffers.empty()) {
        // Output buffer is not returned from VDA yet.
        return false;
    }
    return true;  // Output buffer is returned, or it has no related output buffer (CSD work).
}

void C2VDAComponent::reportEOSWork() {
    ALOGV("reportEOSWork");
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    // In this moment all works prior to EOS work should be done and returned to listener.
    if (mPendingWorks.size() != 1u) {  // only EOS work left
        ALOGE("It shouldn't have remaining works in mPendingWorks except EOS work.");
        reportError(C2_CORRUPTED);
        return;
    }

    mPendingOutputEOS = false;

    std::unique_ptr<C2Work> eosWork(std::move(mPendingWorks.front()));
    mPendingWorks.pop_front();
    if (!eosWork->input.buffers.empty()) {
        eosWork->input.buffers.front().reset();
    }
    eosWork->result = C2_OK;
    eosWork->workletsProcessed = static_cast<uint32_t>(eosWork->worklets.size());
    eosWork->worklets.front()->output.flags = C2FrameData::FLAG_END_OF_STREAM;

    std::list<std::unique_ptr<C2Work>> finishedWorks;
    finishedWorks.emplace_back(std::move(eosWork));
    mListener->onWorkDone_nb(shared_from_this(), std::move(finishedWorks));
}

void C2VDAComponent::reportAbandonedWorks() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    std::list<std::unique_ptr<C2Work>> abandonedWorks;

    while (!mPendingWorks.empty()) {
        std::unique_ptr<C2Work> work(std::move(mPendingWorks.front()));
        mPendingWorks.pop_front();

        // TODO: correlate the definition of flushed work result to framework.
        work->result = C2_NOT_FOUND;
        // When the work is abandoned, buffer in input.buffers shall reset by component.
        if (!work->input.buffers.empty()) {
            work->input.buffers.front().reset();
        }
        abandonedWorks.emplace_back(std::move(work));
    }

    for (auto& work : mAbandonedWorks) {
        // TODO: correlate the definition of flushed work result to framework.
        work->result = C2_NOT_FOUND;
        // When the work is abandoned, buffer in input.buffers shall reset by component.
        if (!work->input.buffers.empty()) {
            work->input.buffers.front().reset();
        }
        abandonedWorks.emplace_back(std::move(work));
    }
    mAbandonedWorks.clear();

    // Pending EOS work will be abandoned here due to component flush if any.
    mPendingOutputEOS = false;

    if (!abandonedWorks.empty()) {
        mListener->onWorkDone_nb(shared_from_this(), std::move(abandonedWorks));
    }
}

void C2VDAComponent::reportError(c2_status_t error) {
    mListener->onError_nb(shared_from_this(), static_cast<uint32_t>(error));
}

bool C2VDAComponent::startDequeueThread(const media::Size& size, uint32_t pixelFormat,
                                        std::shared_ptr<C2BlockPool> blockPool) {
    CHECK(!mDequeueThread.IsRunning());
    if (!mDequeueThread.Start()) {
        ALOGE("failed to start dequeue thread!!");
        return false;
    }
    mDequeueLoopStop.store(false);
    mBuffersInClient.store(0u);
    mDequeueThread.task_runner()->PostTask(
            FROM_HERE, ::base::Bind(&C2VDAComponent::dequeueThreadLoop, ::base::Unretained(this),
                                    size, pixelFormat, std::move(blockPool)));
    return true;
}

void C2VDAComponent::stopDequeueThread() {
    if (mDequeueThread.IsRunning()) {
        mDequeueLoopStop.store(true);
        mDequeueThread.Stop();
    }
}

void C2VDAComponent::dequeueThreadLoop(const media::Size& size, uint32_t pixelFormat,
                                       std::shared_ptr<C2BlockPool> blockPool) {
    ALOGV("dequeueThreadLoop starts");
    DCHECK(mDequeueThread.task_runner()->BelongsToCurrentThread());

    while (!mDequeueLoopStop.load()) {
        if (mBuffersInClient.load() == 0) {
            ::usleep(kDequeueRetryDelayUs);  // wait for retry
            continue;
        }
        std::shared_ptr<C2GraphicBlock> block;
        C2MemoryUsage usage = {
                mSecureMode ? C2MemoryUsage::READ_PROTECTED : C2MemoryUsage::CPU_READ, 0};
        auto err = blockPool->fetchGraphicBlock(size.width(), size.height(), pixelFormat, usage,
                                                &block);
        if (err == C2_TIMED_OUT) {
            continue;  // wait for retry
        }
        if (err == C2_OK) {
            uint32_t poolId;
            if (blockPool->getAllocatorId() == C2PlatformAllocatorStore::BUFFERQUEUE) {
                err = C2VdaBqBlockPool::getPoolIdFromGraphicBlock(block, &poolId);
            } else {  // bufferpool
                err = C2VdaPooledBlockPool::getPoolIdFromGraphicBlock(block, &poolId);
            }

            if (err != C2_OK) {
                ALOGE("dequeueThreadLoop got error on getPoolIdFromGraphicBlock: %d", err);
                break;
            }
            mTaskRunner->PostTask(FROM_HERE,
                                  ::base::Bind(&C2VDAComponent::onOutputBufferReturned,
                                               ::base::Unretained(this), std::move(block), poolId));
            mBuffersInClient--;
        } else {
            ALOGE("dequeueThreadLoop got error: %d", err);
            break;
        }
    }
    ALOGV("dequeueThreadLoop terminates");
}

class C2VDAComponentFactory : public C2ComponentFactory {
public:
    C2VDAComponentFactory(C2String decoderName)
          : mDecoderName(decoderName),
            mReflector(std::static_pointer_cast<C2ReflectorHelper>(
                    GetCodec2VDAComponentStore()->getParamReflector())){};

    c2_status_t createComponent(c2_node_id_t id, std::shared_ptr<C2Component>* const component,
                                ComponentDeleter deleter) override {
        UNUSED(deleter);
        *component = std::shared_ptr<C2Component>(new C2VDAComponent(mDecoderName, id, mReflector));
        return C2_OK;
    }
    c2_status_t createInterface(c2_node_id_t id,
                                std::shared_ptr<C2ComponentInterface>* const interface,
                                InterfaceDeleter deleter) override {
        UNUSED(deleter);
        *interface =
                std::shared_ptr<C2ComponentInterface>(new SimpleInterface<C2VDAComponent::IntfImpl>(
                        mDecoderName.c_str(), id,
                        std::make_shared<C2VDAComponent::IntfImpl>(mDecoderName, mReflector)));
        return C2_OK;
    }
    ~C2VDAComponentFactory() override = default;

private:
    const C2String mDecoderName;
    std::shared_ptr<C2ReflectorHelper> mReflector;
};
}  // namespace android

extern "C" ::C2ComponentFactory* CreateC2VDAH264Factory(bool secureMode) {
    ALOGV("in %s (secureMode=%d)", __func__, secureMode);
    return secureMode ? new ::android::C2VDAComponentFactory(android::kH264SecureDecoderName)
                      : new ::android::C2VDAComponentFactory(android::kH264DecoderName);
}

extern "C" void DestroyC2VDAH264Factory(::C2ComponentFactory* factory) {
    ALOGV("in %s", __func__);
    delete factory;
}

extern "C" ::C2ComponentFactory* CreateC2VDAVP8Factory(bool secureMode) {
    ALOGV("in %s (secureMode=%d)", __func__, secureMode);
    return secureMode ? new ::android::C2VDAComponentFactory(android::kVP8SecureDecoderName)
                      : new ::android::C2VDAComponentFactory(android::kVP8DecoderName);
}

extern "C" void DestroyC2VDAVP8Factory(::C2ComponentFactory* factory) {
    ALOGV("in %s", __func__);
    delete factory;
}

extern "C" ::C2ComponentFactory* CreateC2VDAVP9Factory(bool secureMode) {
    ALOGV("in %s (secureMode=%d)", __func__, secureMode);
    return secureMode ? new ::android::C2VDAComponentFactory(android::kVP9SecureDecoderName)
                      : new ::android::C2VDAComponentFactory(android::kVP9DecoderName);
}

extern "C" void DestroyC2VDAVP9Factory(::C2ComponentFactory* factory) {
    ALOGV("in %s", __func__);
    delete factory;
}
