/*
* Copyright (c) 2011, Code Aurora Forum. All rights reserved.
* Copyright (C) 2011 Sony Ericsson Mobile Communications AB
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are
* met:
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above
*       copyright notice, this list of conditions and the following
*       disclaimer in the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of Code Aurora Forum, Inc. nor the names of its
*       contributors may be used to endorse or promote products derived
*       from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
* WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
* ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
* BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
* WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
* OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
* IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "config.h"

#if ENABLE(ACCELERATED_SCROLLING)
#define LOG_TAG "renderer"
#define LOG_NDEBUG 0

#include "Renderer.h"

#include "BackingStore.h"
#include "CurrentTime.h"
#if ENABLE(GPU_ACCELERATED_SCROLLING)
#include "Renderer2D.h"
#endif
#include "SkBitmap.h"
#include "SkCanvas.h"
#include "SkDevice.h"
#include "SkDrawFilter.h"
#include "SkPaint.h"
#include "SkPaintFlagsDrawFilter.h"
#include "SkPicture.h"
#include "SkPoint.h"
#include "SkProxyCanvas.h"
#include "SkShader.h"
#include "SkTypeface.h"
#include "SkXfermode.h"
#include <cutils/log.h>
#include <cutils/properties.h>
#include <dlfcn.h>
#include <stdio.h>
#include <wtf/PassOwnPtr.h>
#include <wtf/Threading.h>

#if ENABLE(GPU_ACCELERATED_SCROLLING)
#include <surfaceflinger/ISurfaceComposer.h>
#include <surfaceflinger/SurfaceComposerClient.h>
#include <surfaceflinger/IGraphicBufferAlloc.h>
#include <ui/GraphicBuffer.h>
#include <ui/GraphicBufferMapper.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#endif // GPU_ACCELERATED_SCROLLING

#define DO_LOG_PERF
#define DO_LOG_RENDER

#undef LOG_PERF
#ifdef DO_LOG_PERF
#define LOG_PERF(...) ((s_logPerf)? SLOGV(__VA_ARGS__):(void)0)
#else
#define LOG_PERF(...) ((void)0)
#endif

#undef LOG_RENDER
#ifdef DO_LOG_RENDER
#define LOG_RENDER(...) ((s_log)? SLOGV(__VA_ARGS__):(void)0)
#else
#define LOG_RENDER(...) ((void)0)
#endif

static int s_log = 0;
static int s_logPerf = 0;

using namespace android;

namespace RendererImplNamespace {
#if ENABLE(GPU_ACCELERATED_SCROLLING)
static void checkGlError(const char* op) {
    for (GLint error = glGetError(); error; error
        = glGetError()) {
            SLOGE("%s() returns glerror (0x%x)", op, error);
    }
}
#endif


// RendererImpl class
// This is the implementation of the Renderer class.  Users of this class will only see it
// through the Renderer interface.  All implementation details are hidden in this RendererImpl
// class.
class RendererImpl : public Renderer, public WebTech::IBackingStore::IUpdater {
    struct RenderTask {
        enum RenderQuality {
            LOW,
            HIGH
        };

        RenderTask()
            : valid(false)
        {
        }
        SkColor viewColor;
        WebCore::Color pageBackgroundColor;
        bool invertColor;
        WebTech::IBackingStore::UpdateRegion requestArea; // requested valid region in content space.
        float contentScale;
        SkIPoint contentOrigin; // (x,y) in content space, for the point at (0,0) in the viewport
        int viewportWidth;
        int viewportHeight;
        RenderQuality quality;
        SkBitmap::Config config;
        int newContent;
        bool valid;
        bool canAbort;
        bool useGL;
        bool needGLSync;
        bool needRedraw;
        bool scrolling;
        bool zooming;
    };

    struct RendererConfig {
        RendererConfig()
            : enableSpeculativeUpdate(true)
            , allowSplit(true)
            , splitSize(50)
            , enablePartialUpdate(true)
            , enablePartialRender(false)
            , enableDraw(true)
            , enableZoom(true)
            , enableFPSDisplay(false)
            , interactiveZoomDelayUpdate(10)
            , interactiveScrollStart(2)
            , interactiveScrollEnd(2)
        {
        }

        static RendererConfig* getConfig();

        void init()
        {
            char pval[PROPERTY_VALUE_MAX];
            char ival[PROPERTY_VALUE_MAX];
            property_get("persist.debug.tbs.log", pval, s_log? "1" : "0");
            s_log = atoi(pval);
            property_get("debug.tbs.log", pval, s_log? "1" : "0");
            s_log = atoi(pval);
            property_get("persist.debug.tbs.perf", pval, s_logPerf? "1" : "0");
            s_logPerf = atoi(pval);
            property_get("debug.tbs.perf", pval, s_logPerf? "1" : "0");
            s_logPerf = atoi(pval);
            property_get("persist.debug.tbs.fps", pval, enableFPSDisplay? "1" : "0");
            enableFPSDisplay = atoi(pval);
            property_get("debug.tbs.fps", pval, enableFPSDisplay? "1" : "0");
            enableFPSDisplay = atoi(pval);
            property_get("persist.debug.tbs.enable", pval, enableDraw? "1" : "0");
            enableDraw = atoi(pval)? true : false;
            property_get("debug.tbs.enable", pval, enableDraw? "1" : "0");
            enableDraw = atoi(pval)? true : false;
            property_get("persist.debug.tbs.speculative", pval, enableSpeculativeUpdate? "1" : "0");
            enableSpeculativeUpdate = atoi(pval)? true : false;
            property_get("debug.tbs.speculative", pval, enableSpeculativeUpdate? "1" : "0");
            enableSpeculativeUpdate = atoi(pval)? true : false;
            property_get("persist.debug.tbs.partialupdate", pval, enablePartialUpdate? "1" : "0");
            enablePartialUpdate = atoi(pval)? true : false;
            property_get("debug.tbs.partialupdate", pval, enablePartialUpdate? "1" : "0");
            enablePartialUpdate = atoi(pval)? true : false;
            property_get("persist.debug.tbs.partialrender", pval, enablePartialRender? "1" : "0");
            enablePartialRender = atoi(pval)? true : false;
            property_get("debug.tbs.partialrender", pval, enablePartialRender? "1" : "0");
            enablePartialRender = atoi(pval)? true : false;
            property_get("persist.debug.tbs.zoom", pval, enableZoom? "1" : "0");
            enableZoom = atoi(pval)? true : false;
            property_get("debug.tbs.zoom", pval, enableZoom? "1" : "0");
            enableZoom = atoi(pval)? true : false;
            property_get("persist.debug.tbs.split", pval, allowSplit? "1" : "0");
            allowSplit = atoi(pval)? true : false;
            property_get("debug.tbs.split", pval, allowSplit? "1" : "0");
            allowSplit = atoi(pval)? true : false;
            sprintf(ival, "%d", interactiveZoomDelayUpdate);
            property_get("persist.debug.tbs.zoomdelay", pval, ival);
            interactiveZoomDelayUpdate = atoi(pval);
            sprintf(ival, "%d", interactiveZoomDelayUpdate);
            property_get("debug.tbs.zoomdelay", pval, ival);
            interactiveZoomDelayUpdate = atoi(pval);
            print();
        }

        void print()
        {
            LOG_RENDER("TBS properties:");
            LOG_RENDER("    enableDraw = %d", enableDraw? 1 : 0);
            LOG_RENDER("    enableSpeculativeUpdate = %d", enableSpeculativeUpdate? 1 : 0);
            LOG_RENDER("    enablePartialUpdate = %d", enablePartialUpdate? 1 : 0);
            LOG_RENDER("    enablePartialRender = %d", enablePartialRender? 1 : 0);
            LOG_RENDER("    enableZoom = %d", enableZoom? 1 : 0);
            LOG_RENDER("    allowSplit = %d", allowSplit? 1 : 0);
            LOG_RENDER("    fpsDisplay = %d", enableFPSDisplay? 1 : 0);
            LOG_RENDER("    zoomDelay = %d", interactiveZoomDelayUpdate);
        }

        bool enableSpeculativeUpdate; // allow speculative update
        bool allowSplit; // allow the PictureSet to be split into smaller pieces for better performance
        int splitSize; // each block is 50 pages
        bool enablePartialUpdate; // allow backing store to perform partial update on new PictureSet instead of redrawing everything
        bool enablePartialRender;
        bool enableDraw; // enable the backing store
        bool enableZoom; // allow fast zooming using the backing store
        bool enableFPSDisplay; // display FPS
        int interactiveZoomDelayUpdate; // num of frames after an interactive zoom before doing a fullscreen update
        int interactiveScrollStart;
        int interactiveScrollEnd;
    };

#if ENABLE(GPU_ACCELERATED_SCROLLING)

#define NUM_GL_RESOURCE_RENDERERS 2


    class GLResourceUsers {
    public:
        GLResourceUsers();
        ~GLResourceUsers();
        static GLResourceUsers* getGLResourceUsers();
        void getGLResource(RendererImpl* renderer);
        void releaseGLResource(RendererImpl* renderer);
        void incrementTimestamp()
        {
            ++m_curTime;
            if (m_curTime >= 0xffffffff) {
                m_curTime = 0;
                for (int i=0; i < NUM_GL_RESOURCE_RENDERERS; ++i)
                    m_timeStamp[i] = m_curTime;
            }
        }
    private:
        bool m_used[NUM_GL_RESOURCE_RENDERERS];
        RendererImpl* m_renderers[NUM_GL_RESOURCE_RENDERERS];
        unsigned int m_timeStamp[NUM_GL_RESOURCE_RENDERERS];
        unsigned int m_curTime;
    };
#endif // ENABLE(GPU_ACCELERATED_SCROLLING)

    // implementation of WebTech::IBackingStore::IBuffer interface.
    class BackingStoreBuffer : public WebTech::IBackingStore::IBuffer {
    public:
        BackingStoreBuffer(int w, int h, int bpp
#if ENABLE(GPU_ACCELERATED_SCROLLING)
            , bool textureMemory
#endif // GPU_ACCELERATED_SCROLLING
            )
            : m_refCount(1)
#if ENABLE(GPU_ACCELERATED_SCROLLING)
            , m_eglImage(0)
            , m_eglDisplay(0)
            , m_textureName(0)
            , m_context(0)
            , m_tid(0)
            , m_next(0)
#endif // GPU_ACCELERATED_SCROLLING
        {
#if ENABLE(GPU_ACCELERATED_SCROLLING)
            LOG_RENDER("BackingStoreBuffer constructor.  texture = %s", textureMemory? "true" : "false");
#endif // GPU_ACCELERATED_SCROLLING
            SkBitmap::Config config = bppToConfig(bpp);
            int stride = 0;

#if ENABLE(GPU_ACCELERATED_SCROLLING)
            if (textureMemory) {
                sp<ISurfaceComposer> composer = (ComposerService::getComposerService());
                if (composer == 0) {
                    SLOGE("BackingStoreBuffer: cannot create composer");
                }
                m_graphicBufferAlloc = composer->createGraphicBufferAlloc();
                if (m_graphicBufferAlloc == 0) {
                    SLOGE("BackingStoreBuffer: cannot create GraphicBufferAlloc");
                }
                else {
                    int format = bppToGraphicBufferFormat(bpp);
                    LOG_RENDER("creating GraphicBuffer w = %d, h = %d", w, h);
                    m_graphicBuffer = m_graphicBufferAlloc->createGraphicBuffer(w, h, format,
                        GraphicBuffer::USAGE_HW_TEXTURE |
                        GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN);
                    if (m_graphicBuffer == 0) {
                        SLOGE("BackingStoreBuffer: Error: cannot allocate GraphicBuffer. num buffers remaining: %d, in delete list: %d, num renderers: %d", s_numBuffers, numBuffersInDeleteList(), s_numRenderers);
                    }
                    if (m_graphicBuffer != 0) {
                        stride = m_graphicBuffer->getStride() * bpp;
                        LOG_RENDER("GraphicBuffer width = %d, stride = %d", m_graphicBuffer->getWidth(), stride);
                    }
                }
            }
#endif // GPU_ACCELERATED_SCROLLING

            m_bitmap.setConfig(config, w, h, stride);

#if ENABLE(GPU_ACCELERATED_SCROLLING)
            if (m_graphicBuffer == 0)
#endif // GPU_ACCELERATED_SCROLLING
                m_bitmap.allocPixels();

            ++s_numBuffers;

            static unsigned int s_id = 0;
            m_id = ++s_id;
            if (s_id <= 0)
                s_id = 1;

            LOG_RENDER("created BackingStoreBuffer - number of buffers: %d", s_numBuffers);
        }

        virtual ~BackingStoreBuffer()
        {
#if ENABLE(GPU_ACCELERATED_SCROLLING)
            pid_t tid = gettid();
            if ((m_textureName || m_eglImage) && m_tid != 0 && tid != m_tid) {
                SLOGE("~BackingStoreBuffer not called in UI thread");
            }
            if (m_textureName) {
                EGLContext ctx = eglGetCurrentContext();
                if (ctx != m_context) {
                    SLOGE("~BackingStoreBuffer context changed");
                }
                glDeleteTextures(1, &m_textureName);
            }
            if (m_eglImage) {
                eglDestroyImageKHR(m_eglDisplay, m_eglImage);
            }
            if (m_graphicBufferAlloc != 0)
                m_graphicBufferAlloc->freeAllGraphicBuffersExcept(-1);

            if (s_log) {
                int gbCount = 0, gballocCount = 0;
                if (m_graphicBuffer != 0)
                    gbCount = m_graphicBuffer->getStrongCount();
                if (m_graphicBufferAlloc != 0)
                    gballocCount = m_graphicBufferAlloc->getStrongCount();

                SLOGV("~BackingStoreBuffer - graphicBuffer refcount = %d.  allocator ref count = %d", gbCount, gballocCount);
            }
#endif // GPU_ACCELERATED_SCROLLING

            --s_numBuffers;
            LOG_RENDER("~BackingStoreBuffer - number of buffers left: %d", s_numBuffers);
        }

        void addRef()
        {
#if ENABLE(GPU_ACCELERATED_SCROLLING)
            // Normally only TBS's working thread change the buffer's reference count.
            // But in GPU mode, the UI thread also add reference to the buffer because
            // the GPU may be using the buffer.  So only if GPU is used this function need
            // to be thread safe.
            MutexLocker locker(s_mutex);
#endif
            ++m_refCount;
        }

        int getRefCount()
        {
            return m_refCount;
        }

        virtual int width()
        {
            return m_bitmap.width();
        }

        virtual int height()
        {
            return m_bitmap.height();
        }

        virtual void release()
        {
#if ENABLE(GPU_ACCELERATED_SCROLLING)
            MutexLocker locker(s_mutex);
#endif
            --m_refCount;
            if (m_refCount<=0) {
#if ENABLE(GPU_ACCELERATED_SCROLLING)
                pid_t tid = gettid();
                EGLContext ctx = eglGetCurrentContext();
                if ((m_textureName || m_eglImage)
                    && ((m_tid != 0 && tid != m_tid) || (m_context != 0 && m_context != ctx))) {
                    // We're not in the UI thread. Add this instance of RendererImpl
                    // to the delete list so that it can be deleted later on in the UI thread.
                    BackingStoreBuffer* next = s_deleteList;
                    s_deleteList = this;
                    m_next = next;
                } else {
                    delete this;
                    processDeleteList();
                }
#else
                delete this;
#endif
            }
        }

#if ENABLE(GPU_ACCELERATED_SCROLLING)
        //Must be called in UI thread
        static void processDeleteList()
        {
            LOG_RENDER("BackingStoreBuffer::processDeleteList");
            BackingStoreBuffer* next = s_deleteList;
            s_deleteList = 0;
            while(next) {
                BackingStoreBuffer* buf = next;
                next = next->m_next;
                LOG_RENDER("deleting BackingStoreBuffer %p", buf);
                delete buf;
            }
        }

        static int numBuffersInDeleteList()
        {
            BackingStoreBuffer* next = s_deleteList;
            int num = 0;
            while(next) {
                ++num;
                next = next->m_next;
            }
            return num;
        }
#endif
        const SkBitmap& getBitmap()
        {
            return m_bitmap;
        }

        unsigned int getID()
        {
            return m_id;
        }

        void lock(WebTech::IBackingStore::UpdateRegion* lockRect = 0)
        {
#if ENABLE(GPU_ACCELERATED_SCROLLING)
            if (m_graphicBuffer != 0) {
                void* buf = NULL;
                LOG_RENDER("locking GraphicBuffer");
                status_t stat;
                if (lockRect) {
                    android::Rect arect((lockRect->x1 > 0)? lockRect->x1 : 0,
                        (lockRect->y1 > 0)? lockRect->y1 : 0,
                        (lockRect->x2 <= m_bitmap.width())? lockRect->x2 : m_bitmap.width(),
                        (lockRect->y2 <= m_bitmap.height())? lockRect->y2 : m_bitmap.height());
                    stat = m_graphicBuffer->lock(GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN, arect, (void**)(&buf));
                } else
                    stat = m_graphicBuffer->lock(GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN, (void**)(&buf));
                if (stat != NO_ERROR) {
                    SLOGE("BackingStoreBuffer: GraphicBuffer::lock() failed (error = %d) num buffers remaining: %d, in delete list: %d, num renderers: %d", stat, s_numBuffers, numBuffersInDeleteList(), s_numRenderers);
                    buf = 0;
                }
                m_bitmap.setPixels(buf);
            }
#endif // GPU_ACCELERATED_SCROLLING
        }

        void unlock()
        {
#if ENABLE(GPU_ACCELERATED_SCROLLING)
            if (m_graphicBuffer != 0) {
                LOG_RENDER("unlocking GraphicBuffer");
                m_graphicBuffer->unlock();
            }
#endif // GPU_ACCELERATED_SCROLLING
        }

#if ENABLE(GPU_ACCELERATED_SCROLLING)
        //Must be called in UI thread
        GLuint bindTexture()
        {
            LOG_RENDER("bindTexture");
            {
                MutexLocker locker(s_mutex);
                processDeleteList();
            }

            if (failed())
                return 0;

            if (!m_eglImage && m_graphicBuffer != 0) {
                EGLDisplay display = eglGetCurrentDisplay();
                EGLClientBuffer clientBuffer = (EGLClientBuffer)m_graphicBuffer->getNativeBuffer();
                m_eglImage = eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                    clientBuffer, 0);
                if (!m_eglImage) {
                    SLOGE("BackingStoreBuffer: eglCreateImageKHR failed.  num buffers remaining: %d, in delete list: %d, num renderers: %d", s_numBuffers, numBuffersInDeleteList(), s_numRenderers);
                }
                m_eglDisplay = display;
            }

            if (!m_textureName) {
                m_tid = gettid();
                m_context = eglGetCurrentContext();
                glGenTextures(1, &m_textureName);
                glBindTexture(GL_TEXTURE_2D, m_textureName);checkGlError("glBindTexture");
                glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)m_eglImage);checkGlError("glEGLImageTargetTexture2DOES");
            }
            return m_textureName;
        }
#endif
        bool failed()
        {
            return
#if ENABLE(GPU_ACCELERATED_SCROLLING)
                (m_graphicBuffer == 0) &&
#endif // GPU_ACCELERATED_SCROLLING
                (!m_bitmap.getPixels());
        }

    private:
        static SkBitmap::Config bppToConfig(int bpp)
        {
            if (bpp == 2)
                return SkBitmap::kRGB_565_Config;
            return SkBitmap::kARGB_8888_Config;
        }

#if ENABLE(GPU_ACCELERATED_SCROLLING)
        static int bppToGraphicBufferFormat(int bpp)
        {
            if (bpp == 2)
                return HAL_PIXEL_FORMAT_RGB_565;
            return HAL_PIXEL_FORMAT_RGBA_8888;
        }
#endif

        int m_refCount;
        SkBitmap m_bitmap;
        static int s_numBuffers;
        unsigned int m_id;
#if ENABLE(GPU_ACCELERATED_SCROLLING)
        sp<IGraphicBufferAlloc> m_graphicBufferAlloc;
        static WTF::Mutex s_mutex;
        sp<GraphicBuffer> m_graphicBuffer;
        EGLImageKHR m_eglImage;
        EGLDisplay m_eglDisplay;
        GLuint m_textureName;
        EGLContext m_context;
        pid_t m_tid;
        BackingStoreBuffer* m_next;
        static BackingStoreBuffer* s_deleteList;
#endif // GPU_ACCELERATED_SCROLLING
    };

    // ContentData - data that must be guarded in mutex.  New content can be set in a webcore thread (webkit main thread),
    // but used in the UI thread.  Any data that can be changed in the webcore thread is encapsulated in
    // this class.
    class ContentData {
    public:
        ContentData()
            : m_incomingPicture(0)
            , m_picture(0)
            , m_contentWidth(0)
            , m_contentHeight(0)
            , m_numIncomingContent(0)
            , m_numIncomingPicture(0)
            , m_incomingLoading(false)
            , m_loading(false)
            , m_incomingInvalidateAll(false)
            , m_invalidateAll(false)
        {
        }

        ~ContentData()
        {
            if (m_incomingPicture)
                m_incomingPicture->unref();
            if (m_picture)
                m_picture->unref();
        }

        // returns number of times content has changed
        int numContentChanged()
        {
            MutexLocker locker(m_mutex);
            return m_numIncomingContent;
        }

        // returns number of times picture has changed
        int numPictureChanged()
        {
            MutexLocker locker(m_mutex);
            return m_numIncomingPicture;
        }

        // if numContentChanged() above returns non zero, changeToNewContent() can be called to
        // switch the currently active content (which may be used by IBackingStore) to the new content.
        // A copy of the reference of the new data is copied for access by the UI thread.  The content
        // previously used by the UI thread is released.
        void changeToNewContent()
        {
            MutexLocker locker(m_mutex);
            if (m_numIncomingContent > 0) {
                m_content = m_incomingContent.release();
                if (m_content) {
                    m_contentWidth = m_content->width();
                    m_contentHeight = m_content->height();
                } else {
                    m_contentWidth = 0;
                    m_contentHeight = 0;
                }
            }
            if (m_numIncomingPicture > 0) {
                if (m_picture)
                    m_picture->unref();
                m_picture = m_incomingPicture;
                m_incomingPicture = 0;
            }

            if (m_incomingContentInvalidRegion && !m_contentInvalidRegion) {
                m_contentInvalidRegion = m_incomingContentInvalidRegion.release();
            }
            else if (m_incomingContentInvalidRegion && m_contentInvalidRegion) {
                m_contentInvalidRegion->op(*m_incomingContentInvalidRegion, SkRegion::kUnion_Op);
                m_incomingContentInvalidRegion.clear();
            }

            m_numIncomingContent = 0;
            m_numIncomingPicture = 0;

            m_loading = m_incomingLoading;
            m_invalidateAll |= m_incomingInvalidateAll;
            m_incomingInvalidateAll = false;
        }

        // can be called from webcore thread (webkit main thread) when setting to null content.
        void onClearContent()
        {
            MutexLocker locker(m_mutex);
            m_incomingContent.clear();
            m_incomingContentInvalidRegion.clear();

            ++m_numIncomingContent;

            m_loading = m_incomingLoading = false;
            m_incomingInvalidateAll = true;
        }

        // can be called from webcore thread (webkit main thread) when new content is available.
        bool onNewContent(const PictureSet& content, SkRegion* region, bool* loading)
        {
            MutexLocker locker(m_mutex);
            int num = content.size();
            LOG_RENDER("new content size = %d x %d.  %d pictures", content.width(), content.height(), num);
            if (region) {
                LOG_RENDER("        - region={%d,%d,r=%d,b=%d}.", region->getBounds().fLeft,
                    region->getBounds().fTop, region->getBounds().fRight,
                    region->getBounds().fBottom);
            }
            m_incomingContent.clear();
            m_incomingContent = new PictureSet(content);

            bool invalidateAll = !region || !RendererConfig::getConfig()->enablePartialUpdate;
            invalidateAll |= ((m_contentWidth != content.width()) || (m_contentHeight != content.height()));

            if (region ) {
                SkIRect rect;
                rect.set(0, 0, content.width(), content.height());
                if (region->contains(rect))
                    invalidateAll = true;
            }
            if (!invalidateAll) {
                LOG_RENDER("setContent. invalidate region");
                if (!m_incomingContentInvalidRegion)
                    m_incomingContentInvalidRegion = new SkRegion(*region);
                else
                    m_incomingContentInvalidRegion->op(*region, SkRegion::kUnion_Op);
            } else {
                LOG_RENDER("setContent. invalidate All");
                m_incomingContentInvalidRegion.clear();
                m_incomingInvalidateAll = true;
            }
            ++m_numIncomingContent;
            if (loading)
                m_incomingLoading = *loading;
            return invalidateAll;
        }

        bool onNewPicture(const SkPicture* picture, WebCore::IntRect& rect)
        {
            MutexLocker locker(m_mutex);
            if (picture)
                LOG_RENDER("new picture size = %d x %d.", picture->width(), picture->height());
            LOG_RENDER("        - rect: x=%d, y=%d, w=%d, h=%d.", rect.x(), rect.y(), rect.width(), rect.height());
            LOG_RENDER("        - last rect: x=%d, y=%d, w=%d, h=%d.", m_lastPictureRect.x(), m_lastPictureRect.y(), m_lastPictureRect.width(), m_lastPictureRect.height());

            if (m_incomingPicture)
                m_incomingPicture->unref();
            m_incomingPicture = 0;
            if (picture)
                m_incomingPicture = new SkPicture(*picture);

            WebCore::IntRect largeRect = rect;
            largeRect.inflateX(rect.width());
            largeRect.inflateY(rect.height());

            WebCore::IntRect urect = unionRect(largeRect, m_lastPictureRect);
            bool invalidate = false;
            if (!urect.isEmpty()) {
                ++m_numIncomingPicture;
#if 1
                if (!m_incomingInvalidateAll) {
                    SkIRect irect;
                    irect = (SkIRect)urect;
                    SkRegion region(irect);
                    LOG_RENDER("        - union rect: x=%d, y=%d, w=%d, h=%d.", urect.x(), urect.y(), urect.width(), urect.height());

                    if (!m_incomingContentInvalidRegion)
                        m_incomingContentInvalidRegion = new SkRegion(region);
                    else
                        m_incomingContentInvalidRegion->op(region, SkRegion::kUnion_Op);
                }
#else
                m_incomingInvalidateAll = true;
#endif
            }

            m_lastPictureRect = largeRect;

            return invalidate;
        }

        WTF::Mutex m_mutex; // for guarding access from UI thread and webcore thread (webkit main thread).
        OwnPtr<PictureSet> m_incomingContent;
        OwnPtr<PictureSet> m_content;
        SkPicture* m_incomingPicture;
        SkPicture* m_picture;
        OwnPtr<SkRegion> m_incomingContentInvalidRegion;
        OwnPtr<SkRegion> m_contentInvalidRegion;
        int m_contentWidth;
        int m_contentHeight;
        int m_numIncomingContent;
        int m_numIncomingPicture;
        bool m_incomingLoading;
        bool m_loading;
        bool m_incomingInvalidateAll;
        bool m_invalidateAll;
        WebCore::IntRect m_lastPictureRect;
    };

public:
    RendererImpl()
        : m_backingStore(0)
        , m_lastScale(1.0f)
        , m_numTimesScaleUnchanged(0)
        , m_loading(false)
        , m_doPartialRender(false)
#if ENABLE(GPU_ACCELERATED_SCROLLING)
        , m_syncObject(EGL_NO_SYNC_KHR)
#endif
        , m_bufferInUse(0)
#if ENABLE(GPU_ACCELERATED_SCROLLING)
        , m_2DRenderer(0)
        , m_FPSRenderer(0)
        , m_tid(0)
        , m_context(0)
        , m_next(0)
#endif
        , m_numTimesScrollChanged(0)
        , m_numTimesScrollUnchanged(0)
        , m_interactiveScrolling(false)
        , m_alphaBlending(false)
    {
        RendererConfig::getConfig()->init();

        char libraryName[PROPERTY_VALUE_MAX];
        property_get(WebTech::BackingStoreLibraryNameProperty, libraryName, WebTech::BackingStoreLibraryName);
        m_library = dlopen(libraryName, RTLD_LAZY);
        if (!m_library)
            LOG_RENDER("Failed to open acceleration library %s", libraryName);
        else {
            m_createBackingStore = (WebTech::CreateBackingStore_t) dlsym(m_library, WebTech::CreateBackingStoreFuncName);
            m_getBackingStoreVersion = (WebTech::GetBackingStoreVersion_t) dlsym(m_library, WebTech::GetBackingStoreVersionFuncName);
#if ENABLE(GPU_ACCELERATED_SCROLLING)
            m_createRenderer2D = (WebTech::CreateRenderer2D_t) dlsym(m_library, WebTech::CreateRenderer2DFuncName);
#endif
            if (!m_createBackingStore || !m_getBackingStoreVersion
#if ENABLE(GPU_ACCELERATED_SCROLLING)
                || !m_createRenderer2D
#endif
                ) {
                dlclose(m_library);
                m_library = 0;
                LOG_RENDER("Failed to find acceleration routines in library %s", libraryName);
            }
            else
                LOG_RENDER("Loaded acceleration library %s", libraryName);
        }

        if (m_library) {
            const char* version = m_getBackingStoreVersion();
            LOG_RENDER("creating WebTech BackingStore - version %s", version);
            int vlen = strlen(WEBTECH_BACKINGSTORE_VERSION);
            int cmp = strncmp(version, WEBTECH_BACKINGSTORE_VERSION, vlen);
            if (cmp)
                LOG_RENDER("WARNING:  WebTech BackingStore version is wrong. Looking for version %s", WEBTECH_BACKINGSTORE_VERSION);

            m_backingStore = m_createBackingStore(static_cast<WebTech::IBackingStore::IUpdater*>(this));
            if (m_backingStore) {
                m_backingStore->setParam(WebTech::IBackingStore::LOG_DEBUG, s_log);
                m_backingStore->setParam(WebTech::IBackingStore::LOG_PERFORMANCE, s_logPerf);
                m_backingStore->setParam(WebTech::IBackingStore::ALLOW_SPECULATIVE_UPDATE, (RendererConfig::getConfig()->enableSpeculativeUpdate)? 1 : 0);
            }

#if ENABLE(GPU_ACCELERATED_SCROLLING)
            if (!m_2DRenderer)
                m_2DRenderer = m_createRenderer2D();
            if (RendererConfig::getConfig()->enableFPSDisplay) {
                m_FPSRenderer = m_createRenderer2D();
                m_FPSRenderer->setCacheSize(1000000);
            }
            m_tid = gettid();
#endif // GPU_ACCELERATED_SCROLLING
        }

        m_enabled = RendererConfig::getConfig()->enableDraw && m_backingStore;
        m_doPartialRender = RendererConfig::getConfig()->enablePartialRender;

        ++s_numRenderers;
        LOG_RENDER("created RendererImpl - number of renderers: %d", s_numRenderers);
    }

    ~RendererImpl()
    {
#if ENABLE(GPU_ACCELERATED_SCROLLING)
        pid_t tid = gettid();
        EGLContext ctx = eglGetCurrentContext();
        if ((m_tid != 0 && tid != m_tid) || (m_context != 0 && m_context != ctx)) {
            SLOGE("~RendererImpl not called in UI thread or context is changed");
        }
        GLResourceUsers::getGLResourceUsers()->releaseGLResource(this);
#endif
        if (m_backingStore)
            m_backingStore->release();
#if ENABLE(GPU_ACCELERATED_SCROLLING)
        glBindTexture(GL_TEXTURE_2D, 0);
        if (m_syncObject != EGL_NO_SYNC_KHR)
            eglDestroySyncKHR(m_syncDisplay, m_syncObject);
#endif
        if (m_bufferInUse) {
#if ENABLE(GPU_ACCELERATED_SCROLLING)
            glFinish();
#endif
            m_bufferInUse->release();
        }
#if ENABLE(GPU_ACCELERATED_SCROLLING)
        if (m_2DRenderer)
            m_2DRenderer->release();
        if (m_FPSRenderer)
            m_FPSRenderer->release();
#endif
        if (m_library)
            dlclose(m_library);

        --s_numRenderers;
        LOG_RENDER("~RendererImpl - number of renderers left: %d", s_numRenderers);
    }

    virtual void release()
    {
#if ENABLE(GPU_ACCELERATED_SCROLLING)
        pid_t tid = gettid();
        EGLContext ctx = eglGetCurrentContext();
        if ((m_tid != 0 && tid != m_tid) || (m_context != 0 && m_context != ctx)) {
            // We're not in the UI thread. Add this instance of RendererImpl
            // to the delete list so that it can be deleted later on in the UI thread.
            RendererImpl* next = s_deleteList;
            s_deleteList = this; // This instance as the head of the list
            m_next = next; // point to the previous head.
        } else
#endif
            delete this;
    }

    virtual void enable(bool e)
    {
        if (e)
            m_enabled = RendererConfig::getConfig()->enableDraw && m_backingStore;
        else
            m_enabled = false;
    }

    virtual bool enabled()
    {
        return m_enabled;
    }

    // can be called from webcore thread (webkit main thread).
    virtual void setContent(const PictureSet& content, SkRegion* region, bool loading)
    {
        bool invalidate = m_contentData.onNewContent(content, region, &loading);
    }

    // can be called from webcore thread (webkit main thread).
    virtual void clearContent()
    {
        LOG_RENDER("client clearContent");
        m_contentData.onClearContent();
        if (!m_doPartialRender && m_backingStore)
            m_backingStore->invalidate();
    }

    // can be called from webcore thread (webkit main thread).
    virtual void pause()
    {
        LOG_RENDER("client pause");
        if (m_backingStore)
            m_backingStore->cleanup();
    }

#if ENABLE(GPU_ACCELERATED_SCROLLING)
    void releaseGLMemory()
    {
        LOG_RENDER("releaseGLMemory");
        glBindTexture(GL_TEXTURE_2D, 0);
        if (m_bufferInUse) {
            glFinish();
            m_bufferInUse->release();
            m_bufferInUse = 0;
        }
        if (m_backingStore)
            m_backingStore->cleanup();
    }
#endif // ENABLE(GPU_ACCELERATED_SCROLLING)

    virtual void finish()
    {
        LOG_RENDER("client finish");
        if (m_backingStore)
            m_backingStore->finish();
    }

    // called in the UI thread.
    virtual bool drawContent(SkCanvas* canvas, SkColor color, bool invertColor, PictureSet& content, bool& splitContent)
    {
        if (!m_enabled)
            return false;
        LOG_RENDER("drawContent");

        if (m_request.valid && m_request.useGL) {
            LOG_RENDER("drawContent called for a GL Renderer.  abort.");
            return false;
        }

#ifdef DO_LOG_RENDER
        if (s_log) {
            const SkMatrix& matrix = canvas->getTotalMatrix();
            SkScalar tx = matrix.getTranslateX();
            SkScalar ty = matrix.getTranslateY();
            SkScalar sx = matrix.getScaleX();
            const SkRegion& clip = canvas->getTotalClip();
            SkIRect clipBound = clip.getBounds();
            int isRect = (clip.isRect())?1:0;
            SLOGV("drawContent tx=%f, ty=%f, scale=%f", tx, ty, sx);
            SLOGV("  clip %d, %d to %d, %d.  isRect=%d", clipBound.fLeft, clipBound.fTop, clipBound.fRight, clipBound.fBottom, isRect);
        }
#endif

        splitContent = false;

        RenderTask request;
        generateRequest(canvas, color, invertColor, request);

        bool drawn = onDraw(request, canvas);

        // The following is the code to allow splitting of the PictureSet
        // for situations where a huge PictureSet is slow to render.  This
        // requires a new setDrawTimes() function in the PictureSet class.
        // It can be enabled later when the changes in PictureSet is done.
#if DO_CONTENT_SPLIT
        if (RendererConfig::getConfig()->allowSplit) {
            int split = suggestContentSplitting(content, request);
            if (split) {
                LOG_RENDER("renderer client triggers content splitting %d", split);
                content.setDrawTimes((uint32_t)(100 << (split - 1)));
                splitContent = true;
            }
        }
#endif
        return drawn;
    }

#if ENABLE(GPU_ACCELERATED_SCROLLING)

    bool drawContentGL(PictureSet& content, WebCore::IntRect& viewRect, SkRect& contentRect, float scale, WebCore::Color color)
    {
        if (!m_enabled)
            return false;
        LOG_RENDER("drawContentGL");

        processDeleteList();

        GLResourceUsers::getGLResourceUsers()->getGLResource(this);

#ifdef DO_LOG_RENDER
        if (s_log) {
            SLOGV("  viewRect (%d, %d) w = %d, h = %d.", viewRect.x(), viewRect.y(), viewRect.width(), viewRect.height());
            SLOGV("  contentRect (%f, %f) w = %f, h = %f.", contentRect.fLeft, contentRect.fTop, contentRect.width(), contentRect.height());
            SLOGV("  scale = %f", scale);
        }
#endif
        RenderTask request;
        generateRequestGL(viewRect, contentRect, scale, color, request);

        EGLDisplay display = eglGetCurrentDisplay();
        if (m_syncObject != EGL_NO_SYNC_KHR) {
            EGLint status = eglClientWaitSyncKHR(m_syncDisplay, m_syncObject, 0, 1000000);

            if (status == EGL_TIMEOUT_EXPIRED_KHR)
                LOG_RENDER("Sync timeout");

            eglDestroySyncKHR(m_syncDisplay, m_syncObject);
            m_syncObject = EGL_NO_SYNC_KHR;
        }

        onDraw(request, 0);

        m_syncObject = eglCreateSyncKHR(display, EGL_SYNC_FENCE_KHR, 0);
        m_syncDisplay = display;

        return request.needRedraw;
    }

    virtual void displayFPS(int ix, int iy, int iwidth, int iheight)
    {
        if (RendererConfig::getConfig()->enableFPSDisplay && m_FPSRenderer) {
            m_FPSRenderer->init(ix, iy, iwidth, iheight);
            char str[4];
            SkCanvas canvas;
            SkBitmap    bitmap;
            int bw = 25;
            int bh = 25;
            bitmap.setConfig(SkBitmap::kARGB_8888_Config, bw, bh);
            bitmap.allocPixels();
            bitmap.eraseColor(SK_ColorWHITE);
            canvas.setBitmapDevice(bitmap);
            SkPaint paint;
            paint.setAntiAlias(true);
            paint.setTextSize(SkIntToScalar(15));
            paint.setColor(SK_ColorBLACK);

            static double gPrevDur = 0;
            static nsecs_t gNow = 0;
            nsecs_t t = systemTime();
            nsecs_t dt = t - gNow;
            gNow = t;
            double fdur = dt /1000000000.0;
            fdur = (gPrevDur + fdur) / 2;
            gPrevDur = fdur;

            int dur = (int)(10.0 / fdur);
            str[3] = (char)('0' + dur % 10); dur /= 10;
            str[2] = (char)('0' + dur % 10); dur /= 10;
            str[1] = (char)('0' + dur % 10); dur /= 10;
            str[0] = (char)('0' + dur % 10);

            canvas.drawText(str, 4, SkIntToScalar(1), SkIntToScalar(15), paint);
            bitmap.lockPixels();
            void* ptr = bitmap.getPixels();
            static unsigned int s_key = 0;
            m_FPSRenderer->drawImage(ptr, bw, bh, 0, s_key, true, 0, 0, bw, bh, 0, 0, bw, bh);
            bitmap.unlockPixels();
        }
    }
#endif // ENABLE(GPU_ACCELERATED_SCROLLING)

    virtual void setAlphaBlending(bool set)
    {
        m_alphaBlending = set;
    }

    ////////////////////////////// IBackingStore::IUpdater methods ///////////////////////
    virtual void inPlaceScroll(WebTech::IBackingStore::IBuffer* buffer, int x, int y, int w, int h, int dx, int dy)
    {
        if (!buffer)
            return;

        BackingStoreBuffer* bitmap = static_cast<BackingStoreBuffer*>(buffer);
        if (!bitmap)
            return;

        bitmap->lock();
        if (bitmap->failed()) {
            LOG_RENDER("inPlaceScroll: buffer not valid");
            return;
        }
        int pitch = bitmap->getBitmap().rowBytes();
        int bpp = bitmap->getBitmap().bytesPerPixel();
        int ny = h;
        int nx = w * bpp;
        char* src = static_cast<char*>(bitmap->getBitmap().getPixels());
        src = src + x * bpp + y * pitch;
        int dptr = pitch;

        if (dy>0) {
            src = src + (h-1) * pitch;
            dptr = -dptr;
        }

        char* dst = src + dx*bpp + dy*pitch;

        if (!dy) {
            for (int i = 0; i < ny; ++i) {
                memmove(dst, src, nx);
                dst += dptr;
                src += dptr;
            }
        } else {
            for (int i = 0; i < ny; ++i) {
                memcpy(dst, src, nx);
                dst += dptr;
                src += dptr;
            }
        }
        bitmap->unlock();
    }

    virtual WebTech::IBackingStore::IBuffer* createBuffer(int w, int h)
    {
        LOG_RENDER("RendererImpl::createBuffer");
        BackingStoreBuffer* buffer;

        buffer = new BackingStoreBuffer(w, h, SkBitmap::ComputeBytesPerPixel(m_request.config)
#if ENABLE(GPU_ACCELERATED_SCROLLING)
            , true
#endif
        );

        if (!buffer || buffer->failed()) {
            SLOGV("failed to allocate buffer for backing store");
            if (buffer)
                buffer->release();
            return 0;
        }
        return static_cast<WebTech::IBackingStore::IBuffer*>(buffer);
    }

    virtual void renderToBackingStoreRegion(WebTech::IBackingStore::IBuffer* buffer, int bufferX, int bufferY, WebTech::IBackingStore::UpdateRegion& region, WebTech::IBackingStore::UpdateQuality quality, float scale, bool existingRegion)
    {
        if (!m_contentData.m_content && !m_contentData.m_picture) {
            LOG_RENDER("renderToBackingStoreRegion: no content to draw");
            return;
        }

        if (!buffer) {
            LOG_RENDER("renderToBackingStoreRegion: no buffer to draw to");
            return;
        }
        BackingStoreBuffer* bitmap = static_cast<BackingStoreBuffer*>(buffer);
        if (!bitmap)
            return;
        WebTech::IBackingStore::UpdateRegion lockRect;
        lockRect.x1 = bufferX;
        lockRect.y1 = bufferY;
        lockRect.x2 = bufferX + region.x2 - region.x1;
        lockRect.y2 = bufferY + region.y2 - region.y1;
        bitmap->lock(&lockRect);

        if (bitmap->failed()) {
            LOG_RENDER("renderToBackingStoreRegion: buffer not valid");
            return;
        }

        LOG_RENDER("renderToBackingStoreRegion. out(%d, %d), area=(%d, %d) to (%d, %d) size=(%d, %d). scale = %f",
            bufferX, bufferY, region.x1, region.y1, region.x2, region.y2, region.x2 - region.x1, region.y2 - region.y1, scale);
        SkCanvas srcCanvas(bitmap->getBitmap());
        SkCanvas* canvas = static_cast<SkCanvas*>(&srcCanvas);
        SkRect clipRect;
        clipRect.set(bufferX, bufferY, bufferX + region.x2 - region.x1, bufferY + region.y2 - region.y1);
        canvas->clipRect(clipRect, SkRegion::kReplace_Op);

        SkScalar s = scale;
        SkScalar dx = -static_cast<SkScalar>(region.x1) + bufferX;
        SkScalar dy = -static_cast<SkScalar>(region.y1) + bufferY;
        canvas->translate(dx, dy);
        canvas->scale(s, s);

        uint32_t removeFlags, addFlags;

        removeFlags = SkPaint::kFilterBitmap_Flag | SkPaint::kDither_Flag;
        addFlags = 0;
        SkPaintFlagsDrawFilter filterLo(removeFlags, addFlags);

        if (existingRegion) {
            SkRegion* clip = m_contentData.m_contentInvalidRegion.get();
            SkRegion* transformedClip = transformContentClip(*clip, s, dx, dy);

            if (transformedClip) {
                canvas->clipRegion(*transformedClip);
                delete transformedClip;
                const SkRegion& totalClip = canvas->getTotalClip();
                if (totalClip.isEmpty()) {
                    LOG_RENDER("renderToBackingStoreRegion exiting because outside clip region");
                    bitmap->unlock();
                    return;
                }
            }
        }

        int sc = 0;
        if (!m_alphaBlending) {
            sc = canvas->save(SkCanvas::kClip_SaveFlag);

            if (m_contentData.m_content) {
                clipRect.set(0, 0, m_contentData.m_content->width(), m_contentData.m_content->height());
                canvas->clipRect(clipRect, SkRegion::kDifference_Op);
            }
        }
        canvas->drawColor(m_request.viewColor, SkXfermode::kSrc_Mode);
        if (!m_alphaBlending)
            canvas->restoreToCount(sc);

        if (m_contentData.m_content) {
            m_contentData.m_content->draw(canvas);
        }

        if (m_contentData.m_picture && m_contentData.m_picture->width() > 0) {
            LOG_RENDER("rendering picture %p\n", m_contentData.m_picture);
            canvas->drawPicture(*(m_contentData.m_picture));
        }
        bitmap->unlock();
    }

private:

#if ENABLE(GPU_ACCELERATED_SCROLLING)
    //Must be called in UI thread
    static void processDeleteList()
    {
        LOG_RENDER("RendererImpl::processDeleteList");
        RendererImpl* next = s_deleteList;
        s_deleteList = 0;
        while(next) {
            RendererImpl* renderer = next;
            next = next->m_next;
            LOG_RENDER("deleting RendererImpl %p", renderer);
            delete renderer;
        }
    }
#endif

    SkRegion* transformContentClip(SkRegion& rgn, float scale, float dx, float dy)
    {
        SkRegion* clip = new SkRegion;
        SkRegion::Iterator iter(rgn);
        int num = 0;
        while (!iter.done()) {
            SkIRect rect = iter.rect();
            rect.fLeft = static_cast<int32_t>(floor(rect.fLeft*scale + dx));
            rect.fTop = static_cast<int32_t>(floor(rect.fTop*scale + dy));
            rect.fRight = static_cast<int32_t>(ceil(rect.fRight*scale + dx));
            rect.fBottom = static_cast<int32_t>(ceil(rect.fBottom*scale + dy));
            clip->op(rect, SkRegion::kUnion_Op);
            iter.next();
            ++num;
        }
        LOG_RENDER("scaleContentClip - created clip region of %d rectangles", num);
        return clip;
    }

    bool detectInteractiveZoom(RenderTask& request)
    {
        bool ret = false;

        if (RendererConfig::getConfig()->enableZoom && !m_request.useGL && m_request.valid && (request.contentScale != m_request.contentScale) && request.quality == RenderTask::LOW) {
                LOG_RENDER("Renderer client detected interactive zoom");
                request.zooming = true;
                ret = true;
        } else if (m_request.useGL || !RendererConfig::getConfig()->enableZoom) {
            if (request.contentScale != m_lastScale)
                m_numTimesScaleUnchanged = 0;
            else if (m_numTimesScaleUnchanged < RendererConfig::getConfig()->interactiveZoomDelayUpdate)
                    ++m_numTimesScaleUnchanged;

            if (m_request.valid && (m_numTimesScaleUnchanged < RendererConfig::getConfig()->interactiveZoomDelayUpdate)) {
                LOG_RENDER("Renderer client detected interactive zoom");
                request.needRedraw = true;
                request.zooming = true;
                ret = true;
            }
        }
        m_lastScale = request.contentScale;
        return ret;
    }

    bool detectInteractiveScroll(RenderTask& request)
    {
        if (!m_request.useGL) {
            //SW relies on low quality mode to indicate interactive zoom/pan.
            //And there's no mechanism for forcing rerender.  So don't risk
            //rendering in scroll mode when it's may be the last frame update.
            request.scrolling = m_interactiveScrolling = (request.quality == RenderTask::LOW);
            return m_interactiveScrolling;
        }

        if (!m_interactiveScrolling) {
            if (request.contentOrigin.fX != m_request.contentOrigin.fX
                || request.contentOrigin.fY != m_request.contentOrigin.fY)
                ++m_numTimesScrollChanged;
            else
                m_numTimesScrollChanged = 0;
            if (m_numTimesScrollChanged >= RendererConfig::getConfig()->interactiveScrollStart) {
                m_interactiveScrolling = true;
                m_numTimesScrollChanged = RendererConfig::getConfig()->interactiveScrollStart;
            }
            m_numTimesScrollUnchanged = 0;
        } else {
            if (request.contentOrigin.fX == m_request.contentOrigin.fX
                && request.contentOrigin.fY == m_request.contentOrigin.fY)
                ++m_numTimesScrollUnchanged;
            else
                m_numTimesScrollUnchanged = 0;

            if (m_numTimesScrollUnchanged >= RendererConfig::getConfig()->interactiveScrollEnd) {
                m_interactiveScrolling = false;
                m_numTimesScrollUnchanged = RendererConfig::getConfig()->interactiveScrollEnd;
            }
            m_numTimesScrollChanged = 0;
        }
        request.scrolling = m_interactiveScrolling;
        return m_interactiveScrolling;
    }

    void generateRequest(SkCanvas* canvas, SkColor viewColor, bool invertColor, RenderTask& task)
    {
        const SkRegion& clip = canvas->getTotalClip();
        const SkMatrix & matrix = canvas->getTotalMatrix();
        SkRegion* region = new SkRegion(clip);

        SkIRect clipBound = clip.getBounds();

        SkDevice* device = canvas->getDevice();
        const SkBitmap& bitmap = device->accessBitmap(true);

        SkDrawFilter* f = canvas->getDrawFilter();
        bool filterBitmap = true;
        if (f) {
            SkPaint tmpPaint;
            tmpPaint.setFilterBitmap(true);
            f->filter(canvas, &tmpPaint, SkDrawFilter::kBitmap_Type);
            filterBitmap = tmpPaint.isFilterBitmap();
        }

        task.requestArea.x1 = -matrix.getTranslateX() + clipBound.fLeft;
        task.requestArea.y1 = -matrix.getTranslateY() + clipBound.fTop;
        task.requestArea.x2 = -matrix.getTranslateX()+ clipBound.fRight;
        task.requestArea.y2 = -matrix.getTranslateY() + clipBound.fBottom;
        task.contentScale = matrix.getScaleX();
        task.contentOrigin.fX = -matrix.getTranslateX();
        task.contentOrigin.fY = -matrix.getTranslateY();
        task.viewportWidth = bitmap.width();
        task.viewportHeight = bitmap.height();
        task.config = bitmap.getConfig();
        task.viewColor = viewColor;
        task.pageBackgroundColor = WebCore::Color::white;
        task.invertColor = invertColor;
        task.quality = (filterBitmap)? RenderTask::HIGH : RenderTask::LOW;
        task.valid = true;
        task.newContent = 0;
        task.canAbort = true;
        task.useGL = false;
        task.needGLSync = false;
        task.needRedraw = false;
        task.scrolling = false;
        task.zooming = false;
    }

#if ENABLE(GPU_ACCELERATED_SCROLLING)
    void generateRequestGL(WebCore::IntRect& viewRect, SkRect& contentRect, float scale, WebCore::Color pageBackgroundColor, RenderTask& task)
    {
        EGLDisplay display = eglGetCurrentDisplay();
        EGLSurface surface = eglGetCurrentSurface(EGL_DRAW);

        int viewportWidth, viewportHeight, viewX, viewY;
        EGLint value;
        eglQuerySurface(display, surface, EGL_WIDTH, &value);
        viewportWidth = static_cast<int>(value);
        eglQuerySurface(display, surface, EGL_HEIGHT, &value);
        viewportHeight = static_cast<int>(value);

        LOG_RENDER("generateRequestGL viewport size = (%d x %d)", viewportWidth, viewportHeight);

        SkRect scaledContentRect;
        scaledContentRect.fLeft = contentRect.fLeft * scale;
        scaledContentRect.fRight = contentRect.fRight * scale;
        scaledContentRect.fTop = contentRect.fTop * scale;
        scaledContentRect.fBottom = contentRect.fBottom * scale;

        // viewRect is specified with (0,0) being the lower left corner.  We invert it
        // so that it's consistent with the other coordinates (with (0,0) being the upper left corner)
        SkIRect iviewRect;
        iviewRect.set(viewRect.x(), (viewportHeight - viewRect.bottom()), viewRect.right(), (viewportHeight - viewRect.y()));


        // make sure viewRect is within bound of the output surface
        if (iviewRect.fLeft < 0) {
            int newWidth = iviewRect.fRight - 0;
            float ratio = (float)newWidth/iviewRect.width();
            iviewRect.fLeft = 0;
            scaledContentRect.fLeft = scaledContentRect.fRight - scaledContentRect.width() * ratio;
        }
        if (iviewRect.fRight > viewportWidth) {
            int newWidth = viewportWidth - iviewRect.fLeft;
            float ratio = (float)newWidth/iviewRect.width();
            iviewRect.fRight = viewportWidth;
            scaledContentRect.fRight = scaledContentRect.fLeft + scaledContentRect.width() * ratio;
        }
        if (iviewRect.fTop < 0) {
            int newHeight = iviewRect.fBottom - 0;
            float ratio = (float)newHeight/iviewRect.height();
            iviewRect.fTop = 0;
            scaledContentRect.fTop = scaledContentRect.fBottom - scaledContentRect.height() * ratio;
        }
        if (iviewRect.fBottom > viewportHeight) {
            int newHeight = viewportHeight - iviewRect.fTop;
            float ratio = (float)newHeight/iviewRect.height();
            iviewRect.fBottom = viewportHeight;
            scaledContentRect.fBottom = scaledContentRect.fTop + scaledContentRect.height() * ratio;
        }

        task.requestArea.x1 = scaledContentRect.fLeft;
        task.requestArea.y1 = scaledContentRect.fTop;
        task.requestArea.x2 = scaledContentRect.fRight;
        task.requestArea.y2 = scaledContentRect.fBottom;
        if ((task.requestArea.x2 - task.requestArea.x1) > viewportWidth)
            task.requestArea.x2 = task.requestArea.x1 + viewportWidth;
        if ((task.requestArea.y2 - task.requestArea.y1) > viewportHeight)
            task.requestArea.y2 = task.requestArea.y1 + viewportHeight;
        task.contentScale = scale;
        task.contentOrigin.fX = scaledContentRect.fLeft - iviewRect.fLeft;
        task.contentOrigin.fY = scaledContentRect.fTop - iviewRect.fTop;
        task.viewportWidth = viewportWidth;
        task.viewportHeight = viewportHeight;
        task.config = SkBitmap::kARGB_8888_Config;
        task.viewColor = SK_ColorWHITE;
        task.pageBackgroundColor = pageBackgroundColor;
        task.invertColor = false;
        task.quality = RenderTask::HIGH;
        task.valid = true;
        task.newContent = 0;
        task.canAbort = false;
        task.useGL = true;
        task.needGLSync = false;
        task.needRedraw = false;
        task.scrolling = false;
        task.zooming = false;
    }
#endif // ENABLE(GPU_ACCELERATED_SCROLLING)

    bool onDraw(RenderTask& request, SkCanvas* canvas)
    {
        double startTime =  WTF::currentTimeMS();

        bool drawn = false;
        bool shouldUpdate = true;
        bool abort = false;

        bool interactiveZoom = detectInteractiveZoom(request);
        if (interactiveZoom && (!m_request.valid || !RendererConfig::getConfig()->enableZoom) && m_request.canAbort) {
            shouldUpdate = false;
            if (m_backingStore)
                m_backingStore->invalidate();
        }
        detectInteractiveScroll(request);

        bool result = false;
        handleNewContent(request);

        if (shouldUpdate) {
            bool changingBuffer = m_request.valid && (request.config != m_request.config
                           || request.viewportWidth != m_request.viewportWidth
                           || request.viewportHeight != m_request.viewportHeight);

            result = RenderRequest((interactiveZoom && !changingBuffer)? m_request : request);
        }

        if (result && !drawn)
            drawn = drawResult(canvas, request);

        if (!drawn)
            finish();

        double endTime =  WTF::currentTimeMS();
        double elapsedTime = endTime - startTime;
        double frameElapsedTime = startTime - m_lastFrameTime;
        LOG_PERF("drawContent (%d, %d) %s %s %s took %f msec. %f since last frame.",
            request.contentOrigin.fX, request.contentOrigin.fY,
            (request.newContent)? "(with new content)" : "",
            (m_loading)? "(loading)" : "",
            (drawn)? "" : "aborted and",
            elapsedTime,
            frameElapsedTime);
        m_lastFrameTime = startTime;

        return drawn;
    }

    void handleNewContent(RenderTask& task)
    {
        task.newContent = false;
        if (m_contentData.numContentChanged() > 0 || m_contentData.numPictureChanged() > 0) {
            if (task.scrolling) {
                task.needRedraw = true;
                return;
            }
            task.newContent = (m_contentData.numContentChanged() + m_contentData.numPictureChanged());
            if (m_backingStore)
                m_backingStore->finish();
            if (task.useGL)
                task.needGLSync = true;
            m_contentData.changeToNewContent();
            if (m_contentData.m_invalidateAll) {
                if (m_backingStore)
                    m_backingStore->invalidate();
                m_contentData.m_invalidateAll = false;
            }
        }
    }

    bool RenderRequest(RenderTask& request)
    {
        bool result = false;

        if (!m_backingStore)
            return false;

        if (m_backingStore->checkError()) {
            m_backingStore->release();
            m_backingStore = 0;
            return false;
        }

        m_backingStore->setParam(WebTech::IBackingStore::ALLOW_INPLACE_SCROLL, request.useGL? 0 : 1);
        m_backingStore->setParam(WebTech::IBackingStore::ALLOW_PARTIAL_RENDER, (m_doPartialRender && (request.zooming || request.scrolling))? 1 : 0);
        m_backingStore->setParam(WebTech::IBackingStore::QUALITY, request.scrolling? 0 : (request.quality == RenderTask::HIGH? 1 : 0));
        m_backingStore->setParam(WebTech::IBackingStore::ALLOW_ABORT, request.canAbort? 1 : 0);
        m_backingStore->setParam(WebTech::IBackingStore::ALLOW_DELAYED_CLEANUP, request.useGL? 0 : 1);

        // see if we need to invalidate
        if (request.contentScale != m_request.contentScale
            || request.config != m_request.config
            || request.viewportWidth != m_request.viewportWidth
            || request.viewportHeight != m_request.viewportHeight) {
            m_backingStore->invalidate();
            if (request.useGL)
                request.needGLSync = true;
            m_contentData.m_contentInvalidRegion.clear();
        }

        if (m_loading != m_contentData.m_loading) {
            m_loading = m_contentData.m_loading;
            m_backingStore->setParam(WebTech::IBackingStore::PRIORITY, (m_loading)? -1 : 0);
        }

#if ENABLE(GPU_ACCELERATED_SCROLLING)
        if (request.useGL && request.needGLSync) {
            LOG_RENDER("RenderRequest glFinish");
            glFinish();
        }
#endif // #if ENABLE(GPU_ACCELERATED_SCROLLING)

        m_request = request;
        result = m_backingStore->update(&(request.requestArea),
                                (m_contentData.m_contentInvalidRegion)? WebTech::IBackingStore::UPDATE_ALL : WebTech::IBackingStore::UPDATE_EXPOSED_ONLY,
                                request.contentOrigin.fX, request.contentOrigin.fY,
                                request.viewportWidth, request.viewportHeight,
                                static_cast<int>(ceil(m_contentData.m_contentWidth * request.contentScale)),
                                static_cast<int>(ceil(m_contentData.m_contentHeight * request.contentScale)),
                                request.contentScale,
                                request.newContent
                                );

        if (result) {
            m_contentData.m_contentInvalidRegion.clear();
        }

        return result;
    };

    // draw sub-region in backing store onto output
    void drawAreaToOutput(SkCanvas* srcCanvas,
        int outWidth, int outHeight, int outPitch, void* outPixels, SkBitmap::Config outConfig,
        float scale,
        SkPaint& paint,
        WebTech::IBackingStore::IDrawRegionIterator* iter,
        bool noClipping)
    {
        SkIPoint o;
        o.fX = iter->outX();
        o.fY = iter->outY();
        SkIPoint i;
        i.fX = iter->inX();
        i.fY = iter->inY();
        int width = iter->width();
        int height = iter->height();

        if (!iter->buffer()) {
            LOG_RENDER("drawAreaToOutput: no buffer");
            return;
        }
        BackingStoreBuffer* bufferImpl = static_cast<BackingStoreBuffer*>(iter->buffer());
        const SkBitmap& backingStoreBitmap = bufferImpl->getBitmap();

        SkBitmap bitmap;
        int inPitch = backingStoreBitmap.rowBytes();
        SkBitmap::Config inConfig = backingStoreBitmap.getConfig();
        bitmap.setConfig(inConfig, width, height, inPitch);
        char* pixels = static_cast<char*>(backingStoreBitmap.getPixels());
        int bpp = backingStoreBitmap.bytesPerPixel();
        pixels = pixels + i.fY * inPitch + i.fX * bpp;
        bitmap.setPixels(static_cast<void*>(pixels));

        // do memcpy instead of using SkCanvas to draw if possible
        if (!m_alphaBlending && noClipping && scale == 1.0f && outConfig == inConfig) {
            if (!i.fX && !o.fX && width == outWidth && height == outHeight && outPitch == inPitch) {
                    char* optr = static_cast<char*>(outPixels);
                    optr = optr + o.fY*outPitch;
                    char* iptr = static_cast<char*>(pixels);
                    memcpy(static_cast<void*>(optr), static_cast<void*>(iptr), height*outPitch);
            } else {
                int w = width * bpp;
                int h = height;
                char* optr = static_cast<char*>(outPixels);
                optr = optr + o.fY*outPitch + o.fX*bpp;
                char* iptr = static_cast<char*>(pixels);
                for (int y = 0; y < h; ++y) {
                    memcpy(static_cast<void*>(optr), static_cast<void*>(iptr), w);
                    optr += outPitch;
                    iptr += inPitch;
                }
            }
        } else {
            SkRect rect;
            rect.set(o.fX, o.fY, o.fX + bitmap.width(), o.fY + bitmap.height());
            srcCanvas->drawBitmapRect(bitmap, 0, rect, &paint);
        }
    }

#if ENABLE(GPU_ACCELERATED_SCROLLING)
    void drawAreaToOutputGL(RenderTask& request, float scale, WebTech::IBackingStore::IDrawRegionIterator* iter, WebTech::IBackingStore::UpdateRegion& areaAvailable)
    {
        LOG_RENDER("RendererImpl::drawAreaToOutputGL");

        SkIPoint o;
        o.fX = iter->outX();
        o.fY = iter->outY();
        SkIPoint i;
        i.fX = iter->inX();
        i.fY = iter->inY();
        int width = areaAvailable.x2 - areaAvailable.x1;
        int height = areaAvailable.y2 - areaAvailable.y1;

        if (!iter->buffer()) {
            LOG_RENDER("drawAreaToOutputGL: no buffer");
            return;
        }
        BackingStoreBuffer* bufferImpl = static_cast<BackingStoreBuffer*>(iter->buffer());

        GLuint texture = bufferImpl->bindTexture();

        if (m_bufferInUse != bufferImpl) {
            bufferImpl->addRef();
            if (m_bufferInUse) {
                LOG_RENDER("drawAreaToOutputGL glFinish");
                glFinish();
                m_bufferInUse->release();
            }
            m_bufferInUse = bufferImpl;
        }

        if (!texture)
            return;

        m_context = eglGetCurrentContext();

        m_2DRenderer->init(0, 0, request.viewportWidth, request.viewportHeight);

        LOG_RENDER("    draw bitmap (size= %d x %d) input area (%d, %d) - (%d, %d) to output area (%d, %d) - (%d, %d)",
            bufferImpl->getBitmap().width(), bufferImpl->getBitmap().height(),
            i.fX, i.fY, i.fX + width, i.fY + height,
            (int)(o.fX * scale), (int)(o.fY * scale), (int)((o.fX + width) * scale), (int)((o.fY + height) * scale));

        m_2DRenderer->drawTexture(texture,
            bufferImpl->getBitmap().width(), bufferImpl->getBitmap().height(),
            i.fX, i.fY, i.fX + width, i.fY + height,
            (int)(o.fX * scale), (int)(o.fY * scale), (int)((o.fX + width) * scale), (int)((o.fY + height) * scale),
            (scale != 1.0f), m_alphaBlending);
    }
#endif // ENABLE(GPU_ACCELERATED_SCROLLING)

    // draw valid region received from render thread to output.  The regions can be broken down
    // into sub-regions
    bool drawResult(SkCanvas* srcCanvas, RenderTask& request)
    {
        bool ret = m_doPartialRender;
        if (!m_backingStore)
            return ret;

        WebTech::IBackingStore::UpdateRegion areaToDraw = request.requestArea;
        SkIPoint contentOrigin = request.contentOrigin;
        float deltaScale = 1.0f;
        if (m_request.contentScale != request.contentScale) {
            if (request.canAbort && request.quality >= 1) {
                LOG_RENDER("Renderer client can't zoom result in high quality.  should wait.");
                return false;
            }
            if (request.canAbort && !RendererConfig::getConfig()->enableZoom) {
                LOG_RENDER("Renderer client aborted due to changing zoom");
                return false;
            }
            deltaScale = m_request.contentScale / request.contentScale;
            areaToDraw.x1 *= deltaScale;
            areaToDraw.y1 *= deltaScale;
            areaToDraw.x2 *= deltaScale;
            areaToDraw.y2 *= deltaScale;
            contentOrigin.fX *= deltaScale;
            contentOrigin.fY *= deltaScale;
        }

        LOG_RENDER("drawResult.  delta scale = %f", deltaScale);

        WebTech::IBackingStore::UpdateRegion areaAvailable;
        WebTech::IBackingStore::RegionAvailability availability = m_backingStore->canDrawRegion(areaToDraw, areaAvailable);
        bool allDrawn;
        if (m_doPartialRender)
            allDrawn = (availability >= WebTech::IBackingStore::FULLY_AVAILABLE);
        else
            allDrawn = (availability == WebTech::IBackingStore::FULLY_AVAILABLE);

        request.needRedraw |= (availability != WebTech::IBackingStore::FULLY_AVAILABLE);

        int requestSize = (areaToDraw.x2 - areaToDraw.x1) * (areaToDraw.y2 - areaToDraw.y1);
        int availSize = (areaAvailable.x2 - areaAvailable.x1) * (areaAvailable.y2 - areaAvailable.y1);
        float ratio = 0.0f;
        if (requestSize != 0)
            ratio = (float)availSize / (float)requestSize;
        LOG_RENDER("drawing viewport area (%d, %d) to (%d, %d).  avail (%d, %d) to (%d, %d). ratio = %f. All valid in backing store: %d",
            areaToDraw.x1, areaToDraw.y1, areaToDraw.x2, areaToDraw.y2,
            areaAvailable.x1, areaAvailable.y1, areaAvailable.x2, areaAvailable.y2,
            ratio,
            (allDrawn)?1:0);
        if (request.canAbort && ratio < 0.2)
            allDrawn = false;

        if (!allDrawn && request.canAbort)
            return false;

        if (!request.useGL)
            drawResultSW(srcCanvas, request, areaAvailable, deltaScale, contentOrigin, ret);
#if ENABLE(GPU_ACCELERATED_SCROLLING)
        else
            drawResultGL(request, areaAvailable, deltaScale, contentOrigin, (availability == WebTech::IBackingStore::FULLY_AVAILABLE)? false : true, ret);
#endif // ENABLE(GPU_ACCELERATED_SCROLLING)
        return ret;

    }

    void drawResultSW(SkCanvas* srcCanvas, RenderTask& request, WebTech::IBackingStore::UpdateRegion& areaAvailable, float deltaScale, SkIPoint& contentOrigin, bool& ret)
    {
        bool simpleClip = false;
        const SkRegion& clip = srcCanvas->getTotalClip();
        SkIRect clipBound = clip.getBounds();
        if (clip.isRect())
            simpleClip = true;

        SkPaint paint;
        paint.setFilterBitmap(false);
        paint.setDither(false);
        paint.setAntiAlias(false);
        paint.setColor(0xffffff);
        paint.setAlpha(255);
        paint.setXfermodeMode(SkXfermode::kSrcOver_Mode);

        srcCanvas->save();
        srcCanvas->setDrawFilter(0);

        srcCanvas->resetMatrix();

        srcCanvas->scale(1.0f / deltaScale, 1.0f / deltaScale);

        SkDevice* device = srcCanvas->getDevice();
        const SkBitmap& bitmap = device->accessBitmap(true);

        int outWidth = bitmap.width();
        int outHeight = bitmap.height();
        void* outPixels = bitmap.getPixels();
        int outPitch = bitmap.rowBytes();
        SkBitmap::Config outConfig = bitmap.getConfig();

        WebTech::IBackingStore::IDrawRegionIterator* iter = m_backingStore->beginDrawRegion(areaAvailable, contentOrigin.fX, contentOrigin.fY);
        if (iter) {
            do {
                drawAreaToOutput(srcCanvas, outWidth, outHeight, outPitch, outPixels, outConfig,
                            1.0f / deltaScale, paint, iter, simpleClip);
            } while (iter->next());
            iter->release();
            ret = true;
        } else
            ret = m_doPartialRender;

        srcCanvas->restore();

    }

#if ENABLE(GPU_ACCELERATED_SCROLLING)
    void drawResultGL(RenderTask& request, WebTech::IBackingStore::UpdateRegion& areaAvailable, float deltaScale, SkIPoint& contentOrigin, bool clear, bool& ret)
    {
        if (clear) {
            glClearColor((float)request.pageBackgroundColor.red() / 255.0,
                (float)request.pageBackgroundColor.green() / 255.0,
                (float)request.pageBackgroundColor.blue() / 255.0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        WebTech::IBackingStore::IDrawRegionIterator* iter = m_backingStore->beginDrawRegion(areaAvailable, contentOrigin.fX, contentOrigin.fY);
        if (iter) {
                drawAreaToOutputGL(request, 1.0f / deltaScale, iter, areaAvailable);
            iter->release();
            ret = true;
        } else
            ret = m_doPartialRender;
    }
#endif // ENABLE(GPU_ACCELERATED_SCROLLING)

    int suggestContentSplitting(PictureSet& content, RenderTask& request)
    {
        unsigned int numBlocks = (content.height() / (request.viewportHeight * RendererConfig::getConfig()->splitSize));
        if (numBlocks>content.size()) {
            numBlocks /= content.size();
            int numSplit = 0;
            while (numBlocks>1) {
                ++numSplit;
                numBlocks = numBlocks >> 1;
            }
            LOG_RENDER("suggestContentSplitting: content length=%d.  num pictures=%d.  num split=%d",
                content.height(), content.size(), numSplit);
            return numSplit;
        }
        return 0;
    }

public:
    static int s_numRenderers;

private:
    WebTech::IBackingStore* m_backingStore;
    bool m_enabled;
    ContentData m_contentData;
    RenderTask m_request;
    float m_lastScale;
    int m_numTimesScaleUnchanged;
    bool m_loading;
    bool m_doPartialRender;
#if ENABLE(GPU_ACCELERATED_SCROLLING)
    EGLSyncKHR m_syncObject;
    EGLDisplay m_syncDisplay;
#endif // GPU_ACCELERATED_SCROLLING
    BackingStoreBuffer* m_bufferInUse;
    void* m_library;
    WebTech::CreateBackingStore_t m_createBackingStore;
    WebTech::GetBackingStoreVersion_t m_getBackingStoreVersion;
#if ENABLE(GPU_ACCELERATED_SCROLLING)
    WebTech::CreateRenderer2D_t m_createRenderer2D;
    WebTech::IRenderer2D* m_2DRenderer;
    WebTech::IRenderer2D* m_FPSRenderer;
    pid_t m_tid;
    EGLContext m_context;
    RendererImpl* m_next;
    static RendererImpl* s_deleteList;
#endif
    double m_lastFrameTime;
    int m_numTimesScrollChanged;
    int m_numTimesScrollUnchanged;
    bool m_interactiveScrolling;
    bool m_alphaBlending;
};

RendererImpl::RendererConfig* RendererImpl::RendererConfig::getConfig()
{
static RendererImpl::RendererConfig s_config;
    return &s_config;
}

#if ENABLE(GPU_ACCELERATED_SCROLLING)
////////////////////////// GLResourceUsers implementation///////////////////
RendererImpl::GLResourceUsers::GLResourceUsers()
    : m_curTime(0)
{
    for (int i=0; i < NUM_GL_RESOURCE_RENDERERS; ++i) {
        m_used[i] = false;
        m_renderers[i] = 0;
    }
}

RendererImpl::GLResourceUsers::~GLResourceUsers()
{

}

RendererImpl::GLResourceUsers* RendererImpl::GLResourceUsers::getGLResourceUsers()
{
static RendererImpl::GLResourceUsers s_GLResourceUsers;
    return &s_GLResourceUsers;
}

void RendererImpl::GLResourceUsers::getGLResource(RendererImpl* renderer)
{
    int i;
    for (int i=0; i < NUM_GL_RESOURCE_RENDERERS; ++i) {
        if (m_used[i] && m_renderers[i] == renderer) {
            incrementTimestamp();
            m_timeStamp[i] = m_curTime;
            return;
        }
    }
    LOG_RENDER("GLResourceUsers::getGLResource");
    int unused = -1;
    unsigned int minTimestamp = 0xffffffff;
    for (i=0; i < NUM_GL_RESOURCE_RENDERERS; ++i) {
        if (!m_used[i]) {
            unused = i;
            break;
        }
        if (m_timeStamp[i] < minTimestamp) {
            minTimestamp = m_timeStamp[i];
            unused = i;
        }
    }

    if (m_used[unused] && m_renderers[unused])
        m_renderers[unused]->releaseGLMemory();

    m_used[unused] = false;

    incrementTimestamp();

    m_renderers[unused] = renderer;
    if (renderer)
        m_used[unused] = true;
    m_timeStamp[unused] = m_curTime;
}

void RendererImpl::GLResourceUsers::releaseGLResource(RendererImpl* renderer)
{
    LOG_RENDER("GLResourceUsers::releaseGLResource");
    for (int i=0; i < NUM_GL_RESOURCE_RENDERERS; ++i) {
        if (m_used[i] && m_renderers[i] == renderer) {
            m_used[i] = false;
            m_renderers[i] = 0;
            break;
        }
    }
}

#endif // ENABLE(GPU_ACCELERATED_SCROLLING)

int RendererImpl::BackingStoreBuffer::s_numBuffers = 0;
int RendererImpl::s_numRenderers = 0;
#if ENABLE(GPU_ACCELERATED_SCROLLING)
WTF::Mutex RendererImpl::BackingStoreBuffer::s_mutex;
RendererImpl::BackingStoreBuffer* RendererImpl::BackingStoreBuffer::s_deleteList = 0;
RendererImpl* RendererImpl::s_deleteList = 0;
#endif // ENABLE(GPU_ACCELERATED_SCROLLING)

} // namespace RendererImplNamespace

using namespace RendererImplNamespace;
namespace android {
Renderer* android::Renderer::createRenderer()
{
    return static_cast<Renderer*>(new RendererImpl());
}

}

#endif // ACCELERATED_SCROLLING
