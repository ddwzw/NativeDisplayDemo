#define LOG_TAG "DisplayDemo"

#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include <binder/IServiceManager.h>
#include <hardware/gralloc.h>
#include <ui/GraphicBuffer.h>
#include <utils/Log.h>
#include <gui/BLASTBufferQueue.h>
#include <gui/IGraphicBufferProducer.h>
#include <gui/Surface.h>
#include <gui/SurfaceControl.h>
#include <system/window.h>
#include <utils/RefBase.h>
#include <android-base/properties.h>
#include <gui/ISurfaceComposerClient.h>
#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>
#include <ui/DisplayState.h>


using namespace android;

bool mQuit = false;

/*
 Android 系统支持多种显示设备，比如说，输出到手机屏幕，或者通过WiFi 投射到电视屏幕。Android用 DisplayDevice 类来表示这样的设备。
 不是所有的 Layer 都会输出到所有的Display, 比如说，我们可以只将Video Layer投射到电视， 而非整个屏幕。
 LayerStack 就是为此设计，LayerStack 是一个Display 对象的一个数值，
 而类Layer里成员State结构体也有成员变量mLayerStack， 只有两者的mLayerStack 值相同，Layer才会被输出到给该Display设备。
 所以LayerStack 决定了每个Display设备上可以显示的Layer数目。   
 */
int mLayerStack = 0;

void fillRGBA8Buffer(uint8_t* img, int width, int height, int stride, int r, int g, int b) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint8_t* pixel = img + (4 * (y*stride + x));
            pixel[0] = r;
            pixel[1] = g;
            pixel[2] = b;
            pixel[3] = 0;
        }
    }
}


int main(int argc, char ** argv) {

    // 建立 App 到 SurfaceFlinger 的 Binder 通信通道
    sp<SurfaceComposerClient> surfaceComposerClient = new SurfaceComposerClient;
    status_t err = surfaceComposerClient->initCheck();
    if (err != OK) {
        ALOGD("SurfaceComposerClient::initCheck error: %#x\n", err);
        return -1;
    }


    // 获取到显示设备的 ID
    // 返回的是一个 vector，因为存在多屏或者投屏等情况
    const std::vector<PhysicalDisplayId> ids = SurfaceComposerClient::getPhysicalDisplayIds();
    if (ids.empty()) {
        ALOGE("Failed to get ID for any displays\n");
        return -1;
    }   

    //displayToken 是屏幕的索引
    sp<IBinder> displayToken = nullptr;
    
    // 示例仅考虑只有一个屏幕的情况
    displayToken = SurfaceComposerClient::getPhysicalDisplayToken(ids.front());
    
    // 获取屏幕相关参数
    ui::DisplayMode displayMode;
    err = SurfaceComposerClient::getActiveDisplayMode(displayToken, &displayMode);
    if (err != OK) {
        ALOGD("SurfaceComposerClient::getActiveDisplayMode error: %#x\n", err);
        return -1;
    }
        

    ui::DisplayState displayState;
    err = SurfaceComposerClient::getDisplayState(displayToken, &displayState);
    if (err != OK) {
        ALOGD("SurfaceComposerClient::getDisplayState error: %#x\n", err);
        return -1;
    }
    
    ui::Size resolution = displayMode.resolution; 
    //resolution = limitSurfaceSize(resolution.width, resolution.height);

    // 创建 SurfaceControl 对象
    // 会远程调用到 SurfaceFlinger 进程中，Surfaceflinger 中会创建一个 Layer 对象
    String8 name("displaydemo");
    sp<SurfaceControl> surfaceControl = surfaceComposerClient->createSurface(name, resolution.getWidth(), 
                                                                             resolution.getHeight(), PIXEL_FORMAT_RGBA_8888,
                                                                             ISurfaceComposerClient::eFXSurfaceBufferState,
                                                                             /*parent*/ nullptr);


    // 配置 Layer 对象
    SurfaceComposerClient::Transaction{}
            .setLayer(surfaceControl, std::numeric_limits<int32_t>::max())
            .show(surfaceControl)
            .setBackgroundColor(surfaceControl, half3{0, 0, 0}, 1.0f, ui::Dataspace::UNKNOWN) // black background
            .setAlpha(surfaceControl, 1.0f)
            .setLayerStack(surfaceControl, displayState.layerStack)
            .apply();


    // 初始化一个 BLASTBufferQueue 对象，传入了前面获取到的 surfaceControl
    // BLASTBufferQueue 是帧缓存的大管家
    sp<BLASTBufferQueue> mBlastBufferQueue = new BLASTBufferQueue("DemoBLASTBufferQueue", surfaceControl, 
                                             resolution.getWidth(), resolution.getHeight(),
                                             PIXEL_FORMAT_RGBA_8888);
    // 获取到 GraphicBuffer 的生产者并完成初始化。
    sp<IGraphicBufferProducer> igbProducer;
    igbProducer = mBlastBufferQueue->getIGraphicBufferProducer();
    igbProducer->setMaxDequeuedBufferCount(2);
    IGraphicBufferProducer::QueueBufferOutput qbOutput;
    igbProducer->connect(new StubProducerListener, NATIVE_WINDOW_API_CPU, false, &qbOutput);

    int countFrame = 0;
    while(!mQuit) {
        int slot;
        sp<Fence> fence;
        sp<GraphicBuffer> buf;
        
        // 1. dequeue buffer
        igbProducer->dequeueBuffer(&slot, &fence, resolution.getWidth(), resolution.getHeight(),
                                              PIXEL_FORMAT_RGBA_8888, GRALLOC_USAGE_SW_WRITE_OFTEN,
                                              nullptr, nullptr);
        igbProducer->requestBuffer(slot, &buf);

        int waitResult = fence->waitForever("dequeueBuffer_EmptyNative");
        if (waitResult != OK) {
            ALOGE("dequeueBuffer_EmptyNative: Fence::wait returned an error: %d", waitResult);
            break;
        }
        
        // 2. fill the buffer with color
        uint8_t* img = nullptr;
        err = buf->lock(GRALLOC_USAGE_SW_WRITE_OFTEN, (void**)(&img));
        if (err != NO_ERROR) {
            ALOGE("error: lock failed: %s (%d)", strerror(-err), -err);
            break;
        }

        countFrame = (countFrame+1)%3;
        ALOGD("countFrame: %d\n", countFrame);
        fillRGBA8Buffer(img, resolution.getWidth(), resolution.getHeight(), buf->getStride(),
                        countFrame == 0 ? 255 : 0,
                        countFrame == 1 ? 255 : 0,
                        countFrame == 2 ? 255 : 0);
        countFrame++;
        err = buf->unlock();
        if (err != NO_ERROR) {
            ALOGE("error: unlock failed: %s (%d)", strerror(-err), -err);
            break;
        }
        
        // 3. queue the buffer to display
        IGraphicBufferProducer::QueueBufferOutput qbOutput;
        IGraphicBufferProducer::QueueBufferInput input(systemTime(), true /* autotimestamp */,
                                                       HAL_DATASPACE_UNKNOWN, {},
                                                       NATIVE_WINDOW_SCALING_MODE_FREEZE, 0,
                                                       Fence::NO_FENCE);
        igbProducer->queueBuffer(slot, input, &qbOutput);

        sleep(1);
    }
    return 0;
}