#include "motioncam/RawBufferStreamer.h"
#include "motioncam/RawContainer.h"
#include "motioncam/RawBufferManager.h"
#include "motioncam/Logger.h"
#include "motioncam/Util.h"
#include "motioncam/Measure.h"
#include "motioncam/AudioInterface.h"
#include "motioncam/RawImageBuffer.h"
#include "motioncam/RawCameraMetadata.h"

#include <zstd.h>
#include <zstd_errors.h>
#include <tinywav.h>
#include <memory>
#include <vint.h>
#include <vp4.h>
#include <bitpack.h>

#if defined(__APPLE__) || defined(__ANDROID__) || defined(__linux__)
    #include <unistd.h>
    #include <arpa/inet.h>
#elif defined(_WIN32)
    #include <WinSock2.h>
#endif

namespace motioncam {
    const int SoundSampleRateHz       = 48000;
    const int SoundChannelCount       = 2;

#if defined(__APPLE__) || defined(__ANDROID__) || defined(__linux__)
    static inline __attribute__((always_inline))
#endif
    uint16_t RAW10(const uint8_t* data, const int x, const int y, const int stride) {
        const int X = (x >> 2) << 2;
        const int xoffset = (y * stride) + ((10*X) >> 3);

        int p = x - X;
        uint8_t shift = p << 1;
        
        return (((uint16_t) data[xoffset + p]) << 2) | ((((uint16_t) data[xoffset + 4]) >> shift) & 0x03);
    }

#if defined(__APPLE__) || defined(__ANDROID__) || defined(__linux__)
    static inline __attribute__((always_inline))
#endif
    uint16_t RAW12(const uint8_t* data, const int x, const int y, const int stride) {
        const int X = (x >> 1) << 1;
        const int xoffset = (y * stride) + ((12*X) >> 3);

        int p = x - X;
        uint8_t shift = p << 2;
        
        return (((uint16_t) data[xoffset + p]) << 4) | ((((uint16_t) data[xoffset + 2]) >> shift) & 0x0F);
    }

#if defined(__APPLE__) || defined(__ANDROID__) || defined(__linux__)
    static inline __attribute__((always_inline))
#endif
    uint16_t RAW16(const uint8_t* data, const int x, const int y, const int stride) {
        const int offset = (y*stride) + (x*2);
        
        return ((uint16_t) data[offset]) | (((uint16_t)data[offset+1]) << 8);
    }

    RawBufferStreamer::RawBufferStreamer() :
        mRunning(false),
        mAudioFd(-1),
        mCropHeight(0),
        mCropWidth(0),
        mBin(false),
        mEnableCompression(false),
        mWrittenFrames(0),
        mAcceptedFrames(0),
        mWrittenBytes(0)
    {
    }

    RawBufferStreamer::~RawBufferStreamer() {
        stop();
    }

    void RawBufferStreamer::start(const std::vector<int>& fds,
                                  const int& audioFd,
                                  const std::shared_ptr<AudioInterface>& audioInterface,
                                  const bool enableCompression,
                                  const int numThreads,
                                  const RawCameraMetadata& cameraMetadata) {
        stop();
        
        if(fds.empty()) {
            logger::log("No file descriptors found");
            return;
        }
        
        mRunning = true;
        mEnableCompression = enableCompression;
        mWrittenFrames = 0;
        mWrittenBytes = 0;
        mAcceptedFrames = 0;
        
        // Start audio interface
        if(audioInterface && audioFd >= 0) {
            mAudioInterface = audioInterface;
            mAudioFd = audioFd;
            
            mAudioInterface->start(SoundSampleRateHz, SoundChannelCount);
        }

        mStartTime = std::chrono::steady_clock::now();
        
        // Create IO threads with maximum priority
        for(int i = 0; i < fds.size(); i++) {
            auto ioThread = std::unique_ptr<std::thread>(new std::thread(&RawBufferStreamer::doStream, this, fds[i], cameraMetadata, (int)fds.size()));

        #if defined(__APPLE__) || defined(__ANDROID__) || defined(__linux__)
            // Update priority
            sched_param priority{};
            priority.sched_priority = 99;

            pthread_setschedparam(ioThread->native_handle(), SCHED_FIFO, &priority);
        #endif

            mIoThreads.push_back(std::move(ioThread));
        }
                
        // Create process threads
        int processThreads = (std::max)(numThreads, 1);

        for(int i = 0; i < processThreads; i++) {
            auto t = std::unique_ptr<std::thread>(new std::thread(&RawBufferStreamer::doProcess, this));
            
            mProcessThreads.push_back(std::move(t));
        }
    }

    void RawBufferStreamer::add(const std::shared_ptr<RawImageBuffer>& frame) {
        mUnprocessedBuffers.enqueue(frame);
        mAcceptedFrames++;
    }

    void RawBufferStreamer::stop() {
        mRunning = false;

        if(mAudioInterface) {
            mAudioInterface->stop();
            
            // Flush to disk
            uint32_t numFrames = 0;
            auto& buffer = mAudioInterface->getAudioData(numFrames);

            FILE* audioFile = fdopen(mAudioFd, "w");
            if(audioFile != nullptr) {
                TinyWav tw = {nullptr};

                if(tinywav_open_write_f(
                    &tw,
                    mAudioInterface->getChannels(),
                    mAudioInterface->getSampleRate(),
                    TW_INT16,
                    TW_INTERLEAVED,
                    audioFile) == 0)
                {
                    tinywav_write_f(&tw, (void*)buffer.data(), (int)(numFrames));
                }
                
                tinywav_close_write(&tw);
            }
            else {
                #if defined(__APPLE__) || defined(__ANDROID__) || defined(__linux__)
                    close(mAudioFd);
                #endif
            }
        }
        
        mAudioInterface = nullptr;
        mAudioFd = -1;

        for(auto& thread : mProcessThreads) {
            thread->join();
        }
        
        mProcessThreads.clear();

        for(auto& thread : mIoThreads) {
            thread->join();
        }
        
        mIoThreads.clear();
    }

    void RawBufferStreamer::setCropAmount(int width, int height) {
        // Only allow cropping when not running
        if(!mRunning) {
            mCropHeight = height;
            mCropWidth = width;
        }
    }

    void RawBufferStreamer::setBin(bool bin) {
        mBin = bin;
    }

    size_t RawBufferStreamer::cropAndBin_RAW10(RawImageBuffer& buffer,
                                               uint8_t* data,
                                               const int ystart,
                                               const int yend,
                                               const int xstart,
                                               const int xend,
                                               const int binnedWidth,
                                               const bool doCompress) const
    {
        std::vector<uint16_t> row0(binnedWidth);
        std::vector<uint16_t> row1(binnedWidth);
        size_t offset = 0;

        auto* encodeFunc = &bitnzpack16;

        for(int y = ystart; y < yend; y+=4) {
            for(int x = xstart; x < xend; x+=4) {
                int iy = y;

                // Unroll inner loops
                
                {
                    const int ix = x;
                    
                    const int ix_m2 = (std::max)(0, ix - 2);
                    const int ix_p2 = (ix + 2) % buffer.width;
                    
                    const int iy_m2 = (std::max)(0, iy - 2);
                    const int iy_p2 = (iy + 2) % buffer.height;
                    
                    const uint16_t p0 = RAW10(data, ix_m2, iy_m2,  buffer.rowStride);
                    const uint16_t p1 = RAW10(data, ix,    iy_m2,  buffer.rowStride) << 1;
                    const uint16_t p2 = RAW10(data, ix_p2, iy_m2,  buffer.rowStride);
                    
                    const uint16_t p3 = RAW10(data, ix_m2, iy,     buffer.rowStride) << 1;
                    const uint16_t p4 = RAW10(data, ix,    iy,     buffer.rowStride) << 2;
                    const uint16_t p5 = RAW10(data, ix_p2, iy,     buffer.rowStride) << 1;
                    
                    const uint16_t p6 = RAW10(data, ix_m2, iy_p2,  buffer.rowStride);
                    const uint16_t p7 = RAW10(data, ix,    iy_p2,  buffer.rowStride) << 1;
                    const uint16_t p8 = RAW10(data, ix_p2, iy_p2,  buffer.rowStride);

                    const uint16_t out = (p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8) >> 4;
                    
                    const uint16_t X = ((x - xstart) >> 2);
                    row0[X] = out;
                }

                {
                    const int ix = x + 1;

                    const int ix_m2 = (std::max)(0, ix - 2);
                    const int ix_p2 = (ix + 2) % buffer.width;
                    
                    const int iy_m2 = (std::max)(0, iy - 2);
                    const int iy_p2 = (iy + 2) % buffer.height;

                    const uint16_t p0 = RAW10(data, ix_m2, iy_m2,  buffer.rowStride);
                    const uint16_t p1 = RAW10(data, ix,    iy_m2,  buffer.rowStride) << 1;
                    const uint16_t p2 = RAW10(data, ix_p2, iy_m2,  buffer.rowStride);
                    
                    const uint16_t p3 = RAW10(data, ix_m2, iy,     buffer.rowStride) << 1;
                    const uint16_t p4 = RAW10(data, ix,    iy,     buffer.rowStride) << 2;
                    const uint16_t p5 = RAW10(data, ix_p2, iy,     buffer.rowStride) << 1;
                    
                    const uint16_t p6 = RAW10(data, ix_m2, iy_p2,  buffer.rowStride);
                    const uint16_t p7 = RAW10(data, ix,    iy_p2,  buffer.rowStride) << 1;
                    const uint16_t p8 = RAW10(data, ix_p2, iy_p2,  buffer.rowStride);

                    const uint16_t out = (p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8) >> 4;

                    const uint16_t X = ((x - xstart) >> 2) + (binnedWidth >> 1);
                    row0[X] = out;
                }

                // Next row
                iy = y + 1;

                {
                    const int ix = x;
                    
                    const int ix_m2 = (std::max)(0, ix - 2);
                    const int ix_p2 = (ix + 2) % buffer.width;
                    
                    const int iy_m2 = (std::max)(0, iy - 2);
                    const int iy_p2 = (iy + 2) % buffer.height;

                    const uint16_t p0 = RAW10(data, ix_m2, iy_m2,  buffer.rowStride);
                    const uint16_t p1 = RAW10(data, ix,    iy_m2,  buffer.rowStride) << 1;
                    const uint16_t p2 = RAW10(data, ix_p2, iy_m2,  buffer.rowStride);
                    
                    const uint16_t p3 = RAW10(data, ix_m2, iy,     buffer.rowStride) << 1;
                    const uint16_t p4 = RAW10(data, ix,    iy,     buffer.rowStride) << 2;
                    const uint16_t p5 = RAW10(data, ix_p2, iy,     buffer.rowStride) << 1;
                    
                    const uint16_t p6 = RAW10(data, ix_m2, iy_p2,  buffer.rowStride);
                    const uint16_t p7 = RAW10(data, ix,    iy_p2,  buffer.rowStride) << 1;
                    const uint16_t p8 = RAW10(data, ix_p2, iy_p2,  buffer.rowStride);

                    const uint16_t out = (p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8) >> 4;

                    const int X = ((x - xstart) >> 2);
                    row1[X] = out;
                }
                
                {
                    const int ix = x + 1;
                    
                    const int ix_m2 = (std::max)(0, ix - 2);
                    const int ix_p2 = (ix + 2) % buffer.width;
                    
                    const int iy_m2 = (std::max)(0, iy - 2);
                    const int iy_p2 = (iy + 2) % buffer.height;

                    const uint16_t p0 = RAW10(data, ix_m2, iy_m2,  buffer.rowStride);
                    const uint16_t p1 = RAW10(data, ix,    iy_m2,  buffer.rowStride) << 1;
                    const uint16_t p2 = RAW10(data, ix_p2, iy_m2,  buffer.rowStride);
                    
                    const uint16_t p3 = RAW10(data, ix_m2, iy,     buffer.rowStride) << 1;
                    const uint16_t p4 = RAW10(data, ix,    iy,     buffer.rowStride) << 2;
                    const uint16_t p5 = RAW10(data, ix_p2, iy,     buffer.rowStride) << 1;
                    
                    const uint16_t p6 = RAW10(data, ix_m2, iy_p2,  buffer.rowStride);
                    const uint16_t p7 = RAW10(data, ix,    iy_p2,  buffer.rowStride) << 1;
                    const uint16_t p8 = RAW10(data, ix_p2, iy_p2,  buffer.rowStride);

                    const uint16_t out = (p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8) >> 4;

                    const int X = ((x - xstart) >> 2) + (binnedWidth >> 1);

                    row1[X] = out;
                }
            }

            // Compress row?
            if(doCompress) {
                size_t writtenBytes = encodeFunc(row0.data(), row0.size(), data+offset);
                offset += writtenBytes;
            
                writtenBytes = encodeFunc(row1.data(), row1.size(), data+offset);
                offset += writtenBytes;
            }
            else {
                // Pack into RAW10
                const int rowSize_2 = row0.size()/2;
                
                for(int i = 0; i < rowSize_2; i+=2) {
                    const auto p0 = static_cast<uint8_t>( row0[i                 ] >> 2 );
                    const auto p1 = static_cast<uint8_t>( row0[i+rowSize_2       ] >> 2 );
                    const auto p2 = static_cast<uint8_t>( row0[i             + 1 ] >> 2 );
                    const auto p3 = static_cast<uint8_t>( row0[i+rowSize_2   + 1 ] >> 2 );
                    
                    const uint8_t upper =
                        static_cast<uint8_t>( (row0[i]     & 0x03))         |
                        static_cast<uint8_t>( (row0[i + 1] & 0x03) << 2)    |
                        static_cast<uint8_t>( (row0[i + 2] & 0x03) << 4)    |
                        static_cast<uint8_t>( (row0[i + 3] & 0x03) << 6);
                    
                    data[offset    ] = p0;
                    data[offset + 1] = p1;
                    data[offset + 2] = p2;
                    data[offset + 3] = p3;
                    data[offset + 4] = upper;
                    
                    offset += 5;
                }

                for(int i = 0; i < rowSize_2; i+=2) {
                    const auto p0 = static_cast<uint8_t>( row1[i                 ] >> 2 );
                    const auto p1 = static_cast<uint8_t>( row1[i+rowSize_2       ] >> 2 );
                    const auto p2 = static_cast<uint8_t>( row1[i             + 1 ] >> 2 );
                    const auto p3 = static_cast<uint8_t>( row1[i+rowSize_2   + 1 ] >> 2 );

                    const uint8_t upper =
                        static_cast<uint8_t>( (row1[i]     & 0x03))         |
                        static_cast<uint8_t>( (row1[i + 1] & 0x03) << 2)    |
                        static_cast<uint8_t>( (row1[i + 2] & 0x03) << 4)    |
                        static_cast<uint8_t>( (row1[i + 3] & 0x03) << 6);
                    
                    data[offset    ] = p0;
                    data[offset + 1] = p1;
                    data[offset + 2] = p2;
                    data[offset + 3] = p3;
                    data[offset + 4] = upper;
                    
                    offset += 5;
                }
            }
        }
        
        return offset;
    }

    size_t RawBufferStreamer::cropAndBin_RAW12(RawImageBuffer& buffer,
                                               uint8_t* data,
                                               const int ystart,
                                               const int yend,
                                               const int xstart,
                                               const int xend,
                                               const int binnedWidth,
                                               const bool doCompress) const
    {
        std::vector<uint16_t> row0(binnedWidth);
        std::vector<uint16_t> row1(binnedWidth);
        uint32_t offset = 0;
        auto* encodeFunc = &bitnzpack16;

        for(int y = ystart; y < yend; y+=4) {
            for(int x = xstart; x < xend; x+=4) {
                int iy = y;

                // Unroll inner loops
                
                {
                    const int ix = x;
                    
                    const int ix_m2 = (std::max)(0, ix - 2);
                    const int ix_p2 = (ix + 2) % buffer.width;
                    
                    const int iy_m2 = (std::max)(0, iy - 2);
                    const int iy_p2 = (iy + 2) % buffer.height;
                    
                    const uint16_t p0 = RAW12(data, ix_m2, iy_m2,  buffer.rowStride);
                    const uint16_t p1 = RAW12(data, ix,    iy_m2,  buffer.rowStride) << 1;
                    const uint16_t p2 = RAW12(data, ix_p2, iy_m2,  buffer.rowStride);
                    
                    const uint16_t p3 = RAW12(data, ix_m2, iy,     buffer.rowStride) << 1;
                    const uint16_t p4 = RAW12(data, ix,    iy,     buffer.rowStride) << 2;
                    const uint16_t p5 = RAW12(data, ix_p2, iy,     buffer.rowStride) << 1;
                    
                    const uint16_t p6 = RAW12(data, ix_m2, iy_p2,  buffer.rowStride);
                    const uint16_t p7 = RAW12(data, ix,    iy_p2,  buffer.rowStride) << 1;
                    const uint16_t p8 = RAW12(data, ix_p2, iy_p2,  buffer.rowStride);

                    const uint16_t out = (p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8) >> 4;
                    
                    const int X = ((x - xstart) >> 2);
                    row0[X] = out;
                }

                {
                    const int ix = x + 1;

                    const int ix_m2 = (std::max)(0, ix - 2);
                    const int ix_p2 = (ix + 2) % buffer.width;
                    
                    const int iy_m2 = (std::max)(0, iy - 2);
                    const int iy_p2 = (iy + 2) % buffer.height;

                    const uint16_t p0 = RAW12(data, ix_m2, iy_m2,  buffer.rowStride);
                    const uint16_t p1 = RAW12(data, ix,    iy_m2,  buffer.rowStride) << 1;
                    const uint16_t p2 = RAW12(data, ix_p2, iy_m2,  buffer.rowStride);
                    
                    const uint16_t p3 = RAW12(data, ix_m2, iy,     buffer.rowStride) << 1;
                    const uint16_t p4 = RAW12(data, ix,    iy,     buffer.rowStride) << 2;
                    const uint16_t p5 = RAW12(data, ix_p2, iy,     buffer.rowStride) << 1;
                    
                    const uint16_t p6 = RAW12(data, ix_m2, iy_p2,  buffer.rowStride);
                    const uint16_t p7 = RAW12(data, ix,    iy_p2,  buffer.rowStride) << 1;
                    const uint16_t p8 = RAW12(data, ix_p2, iy_p2,  buffer.rowStride);

                    const uint16_t out = (p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8) >> 4;

                    const int X = ((x - xstart) >> 2) + (binnedWidth >> 1);
                    row0[X] = out;
                }

                // Next row
                iy = y + 1;

                {
                    const int ix = x;
                    
                    const int ix_m2 = (std::max)(0, ix - 2);
                    const int ix_p2 = (ix + 2) % buffer.width;
                    
                    const int iy_m2 = (std::max)(0, iy - 2);
                    const int iy_p2 = (iy + 2) % buffer.height;

                    const uint16_t p0 = RAW12(data, ix_m2, iy_m2,  buffer.rowStride);
                    const uint16_t p1 = RAW12(data, ix,    iy_m2,  buffer.rowStride) << 1;
                    const uint16_t p2 = RAW12(data, ix_p2, iy_m2,  buffer.rowStride);
                    
                    const uint16_t p3 = RAW12(data, ix_m2, iy,     buffer.rowStride) << 1;
                    const uint16_t p4 = RAW12(data, ix,    iy,     buffer.rowStride) << 2;
                    const uint16_t p5 = RAW12(data, ix_p2, iy,     buffer.rowStride) << 1;
                    
                    const uint16_t p6 = RAW12(data, ix_m2, iy_p2,  buffer.rowStride);
                    const uint16_t p7 = RAW12(data, ix,    iy_p2,  buffer.rowStride) << 1;
                    const uint16_t p8 = RAW12(data, ix_p2, iy_p2,  buffer.rowStride);

                    const uint16_t out = (p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8) >> 4;

                    const int X = ((x - xstart) >> 2);
                    row1[X] = out;
                }
                
                {
                    const int ix = x + 1;
                    
                    const int ix_m2 = (std::max)(0, ix - 2);
                    const int ix_p2 = (ix + 2) % buffer.width;
                    
                    const int iy_m2 = (std::max)(0, iy - 2);
                    const int iy_p2 = (iy + 2) % buffer.height;

                    const uint16_t p0 = RAW12(data, ix_m2, iy_m2,  buffer.rowStride);
                    const uint16_t p1 = RAW12(data, ix,    iy_m2,  buffer.rowStride) << 1;
                    const uint16_t p2 = RAW12(data, ix_p2, iy_m2,  buffer.rowStride);
                    
                    const uint16_t p3 = RAW12(data, ix_m2, iy,     buffer.rowStride) << 1;
                    const uint16_t p4 = RAW12(data, ix,    iy,     buffer.rowStride) << 2;
                    const uint16_t p5 = RAW12(data, ix_p2, iy,     buffer.rowStride) << 1;
                    
                    const uint16_t p6 = RAW12(data, ix_m2, iy_p2,  buffer.rowStride);
                    const uint16_t p7 = RAW12(data, ix,    iy_p2,  buffer.rowStride) << 1;
                    const uint16_t p8 = RAW12(data, ix_p2, iy_p2,  buffer.rowStride);

                    const uint16_t out = (p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8) >> 4;

                    const int X = ((x - xstart) >> 2) + (binnedWidth >> 1);
                    row1[X] = out;
                }
            }

            if(doCompress) {
                size_t writtenBytes = encodeFunc(row0.data(), row0.size(), data+offset);
                offset += writtenBytes;
                
                writtenBytes = encodeFunc(row1.data(), row1.size(), data+offset);
                offset += writtenBytes;
            }
            else {
                // Pack into RAW12
                const int rowSize_2 = static_cast<int>(row0.size())/2;
                
                for(int i = 0; i < rowSize_2; i++) {
                    const uint16_t p0 = row0[i             ];
                    const uint16_t p1 = row0[i + rowSize_2 ];
                    
                    const uint8_t upper =
                        static_cast<uint8_t>( (p0 & 0x0F)) |
                        static_cast<uint8_t>( (p1 & 0x0F) << 4);

                    data[offset    ] = static_cast<uint8_t>(p0 >> 4);
                    data[offset + 1] = static_cast<uint8_t>(p1 >> 4);
                    data[offset + 2] = upper;
                    
                    offset += 3;
                }

                for(int i = 0; i < rowSize_2; i++) {
                    const uint16_t p0 = row1[i             ];
                    const uint16_t p1 = row1[i + rowSize_2 ];

                    const uint8_t upper =
                        static_cast<uint8_t>( (p0 & 0x0F)) |
                        static_cast<uint8_t>( (p1 & 0x0F) << 4);

                    data[offset    ] = static_cast<uint8_t>(p0 >> 4);
                    data[offset + 1] = static_cast<uint8_t>(p1 >> 4);
                    data[offset + 2] = upper;
                    
                    offset += 3;
                }
            }
        }
        
        return offset;
    }

    size_t RawBufferStreamer::cropAndBin_RAW16(RawImageBuffer& buffer,
                                               uint8_t* data,
                                               const int ystart,
                                               const int yend,
                                               const int xstart,
                                               const int xend,
                                               const int binnedWidth,
                                               const bool doCompress) const
    {
        std::vector<uint16_t> row0(binnedWidth);
        std::vector<uint16_t> row1(binnedWidth);
        uint32_t offset = 0;
        auto* encodeFunc = &bitnzpack16;

        for(int y = ystart; y < yend; y+=4) {
            for(int x = xstart; x < xend; x+=4) {
                int iy = y;

                // Unroll inner loops
                {
                    const int ix = x;
                    
                    const int ix_m2 = (std::max)(0, ix - 2);
                    const int ix_p2 = (ix + 2) % buffer.width;
                    
                    const int iy_m2 = (std::max)(0, iy - 2);
                    const int iy_p2 = (iy + 2) % buffer.height;
                    
                    const uint16_t p0 = RAW16(data, ix_m2, iy_m2,  buffer.rowStride);
                    const uint16_t p1 = RAW16(data, ix,    iy_m2,  buffer.rowStride) << 1;
                    const uint16_t p2 = RAW16(data, ix_p2, iy_m2,  buffer.rowStride);
                    
                    const uint16_t p3 = RAW16(data, ix_m2, iy,     buffer.rowStride) << 1;
                    const uint16_t p4 = RAW16(data, ix,    iy,     buffer.rowStride) << 2;
                    const uint16_t p5 = RAW16(data, ix_p2, iy,     buffer.rowStride) << 1;
                    
                    const uint16_t p6 = RAW16(data, ix_m2, iy_p2,  buffer.rowStride);
                    const uint16_t p7 = RAW16(data, ix,    iy_p2,  buffer.rowStride) << 1;
                    const uint16_t p8 = RAW16(data, ix_p2, iy_p2,  buffer.rowStride);

                    const uint16_t out = (p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8) >> 4;
                    
                    const int X = ((x - xstart) >> 2);
                    row0[X] = out;
                }

                {
                    const int ix = x + 1;

                    const int ix_m2 = (std::max)(0, ix - 2);
                    const int ix_p2 = (ix + 2) % buffer.width;
                    
                    const int iy_m2 = (std::max)(0, iy - 2);
                    const int iy_p2 = (iy + 2) % buffer.height;

                    const uint16_t p0 = RAW16(data, ix_m2, iy_m2,  buffer.rowStride);
                    const uint16_t p1 = RAW16(data, ix,    iy_m2,  buffer.rowStride) << 1;
                    const uint16_t p2 = RAW16(data, ix_p2, iy_m2,  buffer.rowStride);
                    
                    const uint16_t p3 = RAW16(data, ix_m2, iy,     buffer.rowStride) << 1;
                    const uint16_t p4 = RAW16(data, ix,    iy,     buffer.rowStride) << 2;
                    const uint16_t p5 = RAW16(data, ix_p2, iy,     buffer.rowStride) << 1;
                    
                    const uint16_t p6 = RAW16(data, ix_m2, iy_p2,  buffer.rowStride);
                    const uint16_t p7 = RAW16(data, ix,    iy_p2,  buffer.rowStride) << 1;
                    const uint16_t p8 = RAW16(data, ix_p2, iy_p2,  buffer.rowStride);

                    const uint16_t out = (p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8) >> 4;

                    const int X = ((x - xstart) >> 2) + (binnedWidth >> 1);
                    row0[X] = out;
                }

                // Next row
                iy = y + 1;

                {
                    const int ix = x;
                    
                    const int ix_m2 = (std::max)(0, ix - 2);
                    const int ix_p2 = (ix + 2) % buffer.width;
                    
                    const int iy_m2 = (std::max)(0, iy - 2);
                    const int iy_p2 = (iy + 2) % buffer.height;

                    const uint16_t p0 = RAW16(data, ix_m2, iy_m2,  buffer.rowStride);
                    const uint16_t p1 = RAW16(data, ix,    iy_m2,  buffer.rowStride) << 1;
                    const uint16_t p2 = RAW16(data, ix_p2, iy_m2,  buffer.rowStride);
                    
                    const uint16_t p3 = RAW16(data, ix_m2, iy,     buffer.rowStride) << 1;
                    const uint16_t p4 = RAW16(data, ix,    iy,     buffer.rowStride) << 2;
                    const uint16_t p5 = RAW16(data, ix_p2, iy,     buffer.rowStride) << 1;
                    
                    const uint16_t p6 = RAW16(data, ix_m2, iy_p2,  buffer.rowStride);
                    const uint16_t p7 = RAW16(data, ix,    iy_p2,  buffer.rowStride) << 1;
                    const uint16_t p8 = RAW16(data, ix_p2, iy_p2,  buffer.rowStride);

                    const uint16_t out = (p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8) >> 4;

                    const int X = ((x - xstart) >> 2);
                    row1[X] = out;
                }
                
                {
                    const int ix = x + 1;
                    
                    const int ix_m2 = (std::max)(0, ix - 2);
                    const int ix_p2 = (ix + 2) % buffer.width;
                    
                    const int iy_m2 = (std::max)(0, iy - 2);
                    const int iy_p2 = (iy + 2) % buffer.height;

                    const uint16_t p0 = RAW16(data, ix_m2, iy_m2,  buffer.rowStride);
                    const uint16_t p1 = RAW16(data, ix,    iy_m2,  buffer.rowStride) << 1;
                    const uint16_t p2 = RAW16(data, ix_p2, iy_m2,  buffer.rowStride);
                    
                    const uint16_t p3 = RAW16(data, ix_m2, iy,     buffer.rowStride) << 1;
                    const uint16_t p4 = RAW16(data, ix,    iy,     buffer.rowStride) << 2;
                    const uint16_t p5 = RAW16(data, ix_p2, iy,     buffer.rowStride) << 1;
                    
                    const uint16_t p6 = RAW16(data, ix_m2, iy_p2,  buffer.rowStride);
                    const uint16_t p7 = RAW16(data, ix,    iy_p2,  buffer.rowStride) << 1;
                    const uint16_t p8 = RAW16(data, ix_p2, iy_p2,  buffer.rowStride);

                    const uint16_t out = (p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8) >> 4;

                    const int X = ((x - xstart) >> 2) + (binnedWidth >> 1);
                    row1[X] = out;
                }
            }

            if(doCompress) {
                size_t writtenBytes = encodeFunc(row0.data(), row0.size(), data+offset);
                offset += writtenBytes;
                
                writtenBytes = encodeFunc(row1.data(), row1.size(), data+offset);
                offset += writtenBytes;
            }
            else {
                // Pack into RAW12
                const int rowSize_2 = static_cast<int>(row0.size())/2;
                                
                for(int i = 0; i < rowSize_2; i++) {
                    const uint16_t p0 = row0[i             ];
                    const uint16_t p1 = row0[i + rowSize_2 ];
                    
                    const uint8_t upper =
                        static_cast<uint8_t>( (p0 & 0x0F)) |
                        static_cast<uint8_t>( (p1 & 0x0F) << 4);

                    data[offset    ] = static_cast<uint8_t>(p0 >> 4);
                    data[offset + 1] = static_cast<uint8_t>(p1 >> 4);
                    data[offset + 2] = upper;
                    
                    offset += 3;
                }

                for(int i = 0; i < rowSize_2; i++) {
                    const uint16_t p0 = row1[i             ];
                    const uint16_t p1 = row1[i + rowSize_2 ];

                    const uint8_t upper =
                        static_cast<uint8_t>( (p0 & 0x0F)) |
                        static_cast<uint8_t>( (p1 & 0x0F) << 4);

                    data[offset    ] = static_cast<uint8_t>(p0 >> 4);
                    data[offset + 1] = static_cast<uint8_t>(p1 >> 4);
                    data[offset + 2] = upper;
                    
                    offset += 3;
                }
            }
        }
        
        return offset;
    }

    void RawBufferStreamer::cropAndBin(RawImageBuffer& buffer) const {
//        Measure m("cropAndBin");
        
        const int horizontalCrop = static_cast<const int>(4 * (lround(0.5f * (mCropWidth/100.0 * buffer.width)) / 4));

        // Even vertical crop to match bayer pattern
        const int verticalCrop   = static_cast<const int>(2 * (lround(0.5f * (mCropHeight/100.0 * buffer.height)) / 2));

        const int croppedWidth  = static_cast<const int>(buffer.width - horizontalCrop*2);
        const int croppedHeight = static_cast<const int>(buffer.height - verticalCrop*2);
        
        const int ystart = verticalCrop;
        const int yend = buffer.height - ystart;

        const int xstart = horizontalCrop;
        const int xend = buffer.width - horizontalCrop;

        std::vector<uint16_t> row0(croppedWidth / 2);
        std::vector<uint16_t> row1(croppedWidth / 2);

        auto data = buffer.data->lock(true);
        size_t end;

        if(buffer.pixelFormat == PixelFormat::RAW10) {
            end = cropAndBin_RAW10(buffer, data, ystart, yend, xstart, xend, croppedWidth / 2, mEnableCompression);
        }
        else if(buffer.pixelFormat == PixelFormat::RAW12) {
            end = cropAndBin_RAW12(buffer, data, ystart, yend, xstart, xend, croppedWidth / 2, mEnableCompression);
        }
        else if(buffer.pixelFormat == PixelFormat::RAW16) {
            end = cropAndBin_RAW16(buffer, data, ystart, yend, xstart, xend, croppedWidth / 2, mEnableCompression);
        }
        else {
            // Not supported
            buffer.data->unlock();
            return;
        }

        buffer.data->unlock();

        buffer.width = croppedWidth / 2;
        buffer.height = croppedHeight / 2;
        buffer.isBinned = true;
        
        if(mEnableCompression) {
            buffer.pixelFormat = PixelFormat::RAW16;
            buffer.isCompressed = true;
            buffer.compressionType = CompressionType::BITNZPACK_2;
            buffer.rowStride = 2 * buffer.width;
        }
        else {
            buffer.rowStride = buffer.pixelFormat == PixelFormat::RAW10 ? 10*buffer.width/8 : 12*buffer.width/8;
            buffer.isCompressed = false;
            buffer.compressionType = CompressionType::UNCOMPRESSED;
            
            // Repacked from RAW16 -> RAW12
            if(buffer.pixelFormat == PixelFormat::RAW16)
                buffer.pixelFormat = PixelFormat::RAW12;
        }
        
        // Update valid range
        buffer.data->setValidRange(0, end);
    }

    void RawBufferStreamer::cropAndCompress(RawImageBuffer& buffer) const {
        //Measure m("crop");

        const int horizontalCrop = static_cast<const int>(4 * (lround(0.5 * (mCropWidth/100.0 * buffer.width)) / 4));

        // Even vertical crop to match bayer pattern
        const int verticalCrop   = static_cast<const int>(2 * (lround(0.5 * (mCropHeight/100.0 * buffer.height)) / 2));

        const int croppedWidth  = static_cast<const int>(buffer.width - horizontalCrop*2);
        const int croppedHeight = static_cast<const int>(buffer.height - verticalCrop*2);
        
        auto data = buffer.data->lock(true);

        const int xstart = horizontalCrop;
        const int xend = buffer.width - xstart;

        const int ystart = verticalCrop;
        const int yend = buffer.height - ystart;

        const int croppedWidth_2 = croppedWidth>>1;

        std::vector<uint16_t> row(croppedWidth);
        size_t offset = 0;
        
        auto* encodeFunc = &bitnzpack16;
        auto compressionType = CompressionType::BITNZPACK_2;

        // TODO: Unsafely assuming here that the compressed size is always smaller than the input.

        
        
        if(buffer.pixelFormat == PixelFormat::RAW10) {
            for(int y = ystart; y < yend; y++) {
                for(int x = xstart; x < xend; x+=2) {
                    const uint16_t p0 = RAW10(data, x,   y, buffer.rowStride);
                    const uint16_t p1 = RAW10(data, x+1, y, buffer.rowStride);

                    int X = (x - xstart) >> 1;

                    row[X]                  = p0;
                    row[croppedWidth_2 + X] = p1;
                }
                
                size_t writtenBytes = encodeFunc(row.data(), row.size(), data+offset);
                offset += writtenBytes;
            }
        }
        else if(buffer.pixelFormat == PixelFormat::RAW12) {
            for(int y = ystart; y < yend; y++) {
                for(int x = xstart; x < xend; x+=2) {
                    const uint16_t p0 = RAW12(data, x,   y, buffer.rowStride);
                    const uint16_t p1 = RAW12(data, x+1, y, buffer.rowStride);
                    
                    int X = (x - xstart) >> 1;
                    
                    row[X]                  = p0;
                    row[croppedWidth_2 + X] = p1;
                }

                size_t writtenBytes = encodeFunc(row.data(), row.size(), data+offset);
                offset += writtenBytes;
            }
        }
        else if(buffer.pixelFormat == PixelFormat::RAW16) {
            for(int y = ystart; y < yend; y++) {
                for(int x = xstart; x < xend; x+=2) {
                    const uint16_t p0 = RAW16(data, x,   y, buffer.rowStride);
                    const uint16_t p1 = RAW16(data, x+1, y, buffer.rowStride);
                    
                    int X = (x - xstart) >> 1;
                    
                    row[X]                  = p0;
                    row[croppedWidth_2 + X] = p1;
                }

                size_t writtenBytes = encodeFunc(row.data(), row.size(), data+offset);
                offset += writtenBytes;
            }
        }
        else {
            // Not supported
            buffer.data->unlock();
            return;
        }

        buffer.data->unlock();

        // Update buffer
        buffer.pixelFormat = PixelFormat::RAW16;
        buffer.rowStride = croppedWidth*2;
        buffer.width = croppedWidth;
        buffer.height = croppedHeight;
        buffer.isCompressed = true;
        buffer.compressionType = compressionType;

        buffer.data->setValidRange(0, offset);
    }

    void RawBufferStreamer::crop(RawImageBuffer& buffer) const {
        // Nothing to do
        if(mCropWidth  == 0   &&
           mCropHeight == 0   &&
           buffer.pixelFormat != PixelFormat::RAW16) // Always crop when RAW16 so we can pack to RAW10
        {
            return;
        }
        
        const int horizontalCrop = static_cast<const int>(4 * (lround(0.5f * (mCropWidth/100.0 * buffer.width)) / 4));

        // Even vertical crop to match bayer pattern
        const int verticalCrop   = static_cast<const int>(2 * (lround(0.5f * (mCropHeight/100.0 * buffer.height)) / 2));

        const int croppedWidth  = static_cast<const int>(buffer.width - horizontalCrop*2);
        const int croppedHeight = static_cast<const int>(buffer.height - verticalCrop*2);
        
        auto data = buffer.data->lock(true);

        const int ystart = verticalCrop;
        const int yend = buffer.height - ystart;

        int croppedRowStride = 0;

        if(buffer.pixelFormat == PixelFormat::RAW10) {
            croppedRowStride = 10*croppedWidth/8;
            
            for(int y = ystart; y < yend; y++) {
                const int srcOffset = buffer.rowStride * y;
                const int dstOffset = croppedRowStride * (y - ystart);

                const int xstart = 10*horizontalCrop/8;

                std::memmove(data+dstOffset, data+srcOffset+xstart, croppedRowStride);
            }
        }
        else if(buffer.pixelFormat == PixelFormat::RAW12) {
            croppedRowStride = 12*croppedWidth/8;
            
            for(int y = ystart; y < yend; y++) {
                const int srcOffset = buffer.rowStride * y;
                const int dstOffset = croppedRowStride * (y - ystart);

                const int xstart = 10*horizontalCrop/8;

                std::memmove(data+dstOffset, data+srcOffset+xstart, croppedRowStride);
            }
        }
        else if(buffer.pixelFormat == PixelFormat::RAW16) {
            // Pack into RAW12
            croppedRowStride = 12*croppedWidth/8;
            uint32_t dstOffset = 0;

            for(int y = ystart; y < yend; y++) {
                for(int x = horizontalCrop; x < buffer.width - horizontalCrop; x+=2) {
                    const uint16_t p0 = RAW16(data, x,   y, buffer.rowStride);
                    const uint16_t p1 = RAW16(data, x+1, y, buffer.rowStride);

                    const uint8_t upper =
                        static_cast<uint8_t>( (p0 & 0x0F)) |
                        static_cast<uint8_t>( (p1 & 0x0F) << 4);

                    data[dstOffset    ] = static_cast<uint8_t>(p0 >> 4);
                    data[dstOffset + 1] = static_cast<uint8_t>(p1 >> 4);
                    data[dstOffset + 2] = upper;

                    dstOffset += 3;
                }
            }
            
            buffer.pixelFormat = PixelFormat::RAW12;
        }
        else {
            // Not supported
            buffer.data->unlock();
            return;
        }

        buffer.data->unlock();

        // Update buffer
        buffer.rowStride = croppedRowStride;
        buffer.width = croppedWidth;
        buffer.height = croppedHeight;
        buffer.isCompressed = false;
        buffer.compressionType = CompressionType::UNCOMPRESSED;

        buffer.data->setValidRange(0, buffer.rowStride * buffer.height);
    }

    void RawBufferStreamer::processBuffer(const std::shared_ptr<RawImageBuffer>& buffer) const {
        if(mBin)
            cropAndBin(*buffer);
        else {
            if(mEnableCompression)
                cropAndCompress(*buffer);
            else
                crop(*buffer);
        }
    }

    void RawBufferStreamer::doProcess() {
        std::shared_ptr<RawImageBuffer> buffer;
        
        while(mRunning) {
            if(!mUnprocessedBuffers.wait_dequeue_timed(buffer, std::chrono::milliseconds(67))) {
                continue;
            }
            
            processBuffer(buffer);
            
            // Add to the ready list
            mReadyBuffers.enqueue(buffer);
        }

    }

    void RawBufferStreamer::doStream(const int fd, const RawCameraMetadata& cameraMetadata, const int numContainers) {
        std::shared_ptr<RawImageBuffer> buffer;
        size_t start, end;

        auto container = RawContainer::Create(fd, cameraMetadata, numContainers);

        while(mRunning) {
            if(!mReadyBuffers.wait_dequeue_timed(buffer, std::chrono::milliseconds(100))) {
                continue;
            }

            container->add(*buffer, true);

            start = end = 0;
            buffer->data->getValidRange(start, end);

            // Return the buffer after it has been written
            RawBufferManager::get().discardBuffer(buffer);

            mWrittenBytes += (end - start);
            mWrittenFrames++;
        }

        //
        // Flush buffers
        //

        // Ready buffers
        while(mReadyBuffers.try_dequeue(buffer)) {
            container->add(*buffer, true);
            
            buffer->data->getValidRange(start, end);
            mWrittenBytes += (end - start);
            mWrittenFrames++;

            RawBufferManager::get().discardBuffer(buffer);
        }

        // Unprocessed buffers
        while(mUnprocessedBuffers.try_dequeue(buffer)) {
            processBuffer(buffer);
            
            container->add(*buffer, true);

            buffer->data->getValidRange(start, end);
            mWrittenBytes += (end - start);
            mWrittenFrames++;

            RawBufferManager::get().discardBuffer(buffer);
        }

        container->commit();
    }

    bool RawBufferStreamer::isRunning() const {
        return mRunning;
    }

    float RawBufferStreamer::estimateFps() const {
        auto now = std::chrono::steady_clock::now();
        float durationSecs = std::chrono::duration <float>(now - mStartTime).count();
        
        return mAcceptedFrames / (1e-5f + durationSecs);
    }

    size_t RawBufferStreamer::writenOutputBytes() const {
        return mWrittenBytes;
    }
}
