#include "backend.h"
#include "rendervulkan.hpp"
#include "wlserver.hpp"
#include "refresh_rate.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

extern int g_nPreferredOutputWidth;
extern int g_nPreferredOutputHeight;

namespace gamescope
{
    namespace
    {
        constexpr uint32_t k_AHardwareBufferBlobMagic = 0x47423031u; // "GB01"
        constexpr uint32_t k_AckBase = 0xabc00000u;
        constexpr int k_MaxReceivedFds = 16;

        const char *GetAndroidSocketName()
        {
            const char *pszSocket = std::getenv( "GAMESCOPE_ANDROID_SOCKET" );
            return pszSocket && *pszSocket ? pszSocket : "steam-arm-gamescope";
        }

        void MakeAbstractAddress( sockaddr_un *pAddr, socklen_t *pLen, const char *pszName )
        {
            std::memset( pAddr, 0, sizeof( *pAddr ) );
            pAddr->sun_family = AF_UNIX;
            pAddr->sun_path[0] = '\0';
            std::snprintf( pAddr->sun_path + 1, sizeof( pAddr->sun_path ) - 1, "%s", pszName );
            *pLen = offsetof( sockaddr_un, sun_path ) + 1 + std::strlen( pszName );
        }

        uint64_t GetFdSize( int nFd )
        {
            off_t nEnd = lseek( nFd, 0, SEEK_END );
            if ( nEnd > 0 )
            {
                lseek( nFd, 0, SEEK_SET );
                return uint64_t( nEnd );
            }

            struct stat st = {};
            if ( fstat( nFd, &st ) == 0 && st.st_size > 0 )
                return uint64_t( st.st_size );

            return 0;
        }

        void CloseFds( int *pFds, int nFdCount )
        {
            for ( int i = 0; i < nFdCount; i++ )
            {
                if ( pFds[i] >= 0 )
                    close( pFds[i] );
                pFds[i] = -1;
            }
        }

        struct AndroidHardwareBufferFrame
        {
            uint32_t uWidth = 0;
            uint32_t uHeight = 0;
            uint32_t uStridePixels = 0;
            uint32_t uFormat = 0;
            uint64_t ulUsage = 0;
            uint64_t ulFdSize = 0;
            uint32_t uFrameId = 0;
            int nFd = -1;
        };
    }

    class CAndroidBridge
    {
    public:
        ~CAndroidBridge()
        {
            Close();
        }

        bool Init()
        {
            m_pszSocketName = GetAndroidSocketName();

            sockaddr_un addr = {};
            socklen_t len = 0;
            MakeAbstractAddress( &addr, &len, m_pszSocketName );

            m_nListenFd = socket( AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0 );
            if ( m_nListenFd < 0 )
            {
                std::fprintf( stderr, "gamescope android backend: socket @%s failed: %s\n", m_pszSocketName, std::strerror( errno ) );
                return false;
            }

            if ( bind( m_nListenFd, reinterpret_cast<sockaddr *>( &addr ), len ) < 0 )
            {
                std::fprintf( stderr, "gamescope android backend: bind @%s failed: %s\n", m_pszSocketName, std::strerror( errno ) );
                Close();
                return false;
            }

            if ( listen( m_nListenFd, 4 ) < 0 )
            {
                std::fprintf( stderr, "gamescope android backend: listen @%s failed: %s\n", m_pszSocketName, std::strerror( errno ) );
                Close();
                return false;
            }

            std::fprintf( stderr, "gamescope android backend: listening for AHardwareBuffer bridge @%s\n", m_pszSocketName );
            return true;
        }

        std::optional<AndroidHardwareBufferFrame> ReceiveFrame()
        {
            if ( !EnsureConnection() )
                return std::nullopt;

            std::array<unsigned char, 4096> payload = {};
            std::array<int, k_MaxReceivedFds> fds = {};
            fds.fill( -1 );
            int nFdCount = 0;

            char control[CMSG_SPACE( sizeof( int ) * k_MaxReceivedFds )];
            std::memset( control, 0, sizeof( control ) );

            iovec iov = {
                .iov_base = payload.data(),
                .iov_len = payload.size(),
            };
            msghdr msg = {
                .msg_iov = &iov,
                .msg_iovlen = 1,
                .msg_control = control,
                .msg_controllen = sizeof( control ),
            };

            ssize_t nPayload = recvmsg( m_nConnectionFd, &msg, 0 );
            if ( nPayload == 0 )
            {
                std::fprintf( stderr, "gamescope android backend: bridge disconnected\n" );
                CloseConnection();
                return std::nullopt;
            }
            if ( nPayload < 0 )
            {
                std::fprintf( stderr, "gamescope android backend: recvmsg failed: %s\n", std::strerror( errno ) );
                CloseConnection();
                return std::nullopt;
            }

            for ( cmsghdr *cmsg = CMSG_FIRSTHDR( &msg ); cmsg; cmsg = CMSG_NXTHDR( &msg, cmsg ) )
            {
                if ( cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS )
                {
                    size_t nBytes = cmsg->cmsg_len - CMSG_LEN( 0 );
                    nFdCount = int( nBytes / sizeof( int ) );
                    if ( nFdCount > k_MaxReceivedFds )
                        nFdCount = k_MaxReceivedFds;
                    std::memcpy( fds.data(), CMSG_DATA( cmsg ), nFdCount * sizeof( int ) );
                }
            }

            if ( nPayload < 32 || nFdCount < 1 )
            {
                std::fprintf( stderr, "gamescope android backend: invalid AHB payload bytes=%zd fds=%d\n", nPayload, nFdCount );
                CloseFds( fds.data(), nFdCount );
                return std::nullopt;
            }

            const int32_t *pWords = reinterpret_cast<const int32_t *>( payload.data() );
            if ( uint32_t( pWords[0] ) != k_AHardwareBufferBlobMagic )
            {
                std::fprintf( stderr, "gamescope android backend: unexpected AHB payload magic=0x%x\n", uint32_t( pWords[0] ) );
                CloseFds( fds.data(), nFdCount );
                return std::nullopt;
            }

            AndroidHardwareBufferFrame frame = {};
            frame.uWidth = uint32_t( pWords[1] );
            frame.uHeight = uint32_t( pWords[2] );
            frame.uStridePixels = uint32_t( pWords[3] );
            frame.uFormat = uint32_t( pWords[5] );
            frame.ulUsage = uint32_t( pWords[6] );
            if ( nPayload >= 40 )
                frame.ulUsage |= uint64_t( uint32_t( pWords[9] ) ) << 32;
            frame.ulFdSize = GetFdSize( fds[0] );
            frame.uFrameId = m_uNextReceivedFrame++;
            frame.nFd = fds[0];
            fds[0] = -1;

            CloseFds( fds.data(), nFdCount );

            if ( frame.uWidth == 0 || frame.uHeight == 0 || frame.uStridePixels == 0 )
            {
                std::fprintf( stderr, "gamescope android backend: invalid AHB dimensions %ux%u stride=%u\n",
                    frame.uWidth, frame.uHeight, frame.uStridePixels );
                if ( frame.nFd >= 0 )
                    close( frame.nFd );
                return std::nullopt;
            }

            if ( frame.uFrameId < 5 || frame.uFrameId % 60 == 0 )
            {
                std::fprintf( stderr,
                    "gamescope android backend: received AHB frame=%u %ux%u stride=%u format=%u usage=0x%llx fdsize=%llu\n",
                    frame.uFrameId, frame.uWidth, frame.uHeight, frame.uStridePixels, frame.uFormat,
                    (unsigned long long)frame.ulUsage, (unsigned long long)frame.ulFdSize );
            }

            return frame;
        }

        bool SendAck( uint32_t uFrameId )
        {
            if ( m_nConnectionFd < 0 )
                return false;

            uint32_t uAck = k_AckBase | ( uFrameId & 0x000fffffu );
            ssize_t nSent = send( m_nConnectionFd, &uAck, sizeof( uAck ), MSG_NOSIGNAL );
            if ( nSent != ssize_t( sizeof( uAck ) ) )
            {
                std::fprintf( stderr, "gamescope android backend: ack failed: %s\n", std::strerror( errno ) );
                CloseConnection();
                return false;
            }
            return true;
        }

    private:
        bool EnsureConnection()
        {
            if ( m_nConnectionFd >= 0 )
                return true;
            if ( m_nListenFd < 0 )
                return false;

            std::fprintf( stderr, "gamescope android backend: waiting for Android bridge connection @%s\n", m_pszSocketName );
            m_nConnectionFd = accept4( m_nListenFd, nullptr, nullptr, SOCK_CLOEXEC );
            if ( m_nConnectionFd < 0 )
            {
                std::fprintf( stderr, "gamescope android backend: accept failed: %s\n", std::strerror( errno ) );
                return false;
            }

            std::fprintf( stderr, "gamescope android backend: Android bridge connected\n" );
            return true;
        }

        void CloseConnection()
        {
            if ( m_nConnectionFd >= 0 )
            {
                close( m_nConnectionFd );
                m_nConnectionFd = -1;
            }
        }

        void Close()
        {
            CloseConnection();
            if ( m_nListenFd >= 0 )
            {
                close( m_nListenFd );
                m_nListenFd = -1;
            }
        }

        const char *m_pszSocketName = "steam-arm-gamescope";
        int m_nListenFd = -1;
        int m_nConnectionFd = -1;
        uint32_t m_uNextReceivedFrame = 0;
    };

    // Android/AHardwareBuffer backend.
    //
    // This starts from the Headless backend shape: no DRM/KMS device and no
    // Vulkan WSI swapchain, but it initializes gamescope's Vulkan renderer and
    // nested Wayland session. Present() receives Android-owned AHardwareBuffers
    // over an abstract AF_UNIX socket, imports each buffer's dma-buf into Turnip,
    // composites into it, waits for completion, and acks Android so the app can
    // submit the buffer to SurfaceControl.
    class CAndroidConnector final : public CBaseBackendConnector
    {
    public:
        bool InitBridge()
        {
            return m_Bridge.Init();
        }

        gamescope::GamescopeScreenType GetScreenType() const override
        {
            return GAMESCOPE_SCREEN_TYPE_INTERNAL;
        }

        GamescopePanelOrientation GetCurrentOrientation() const override
        {
            return GAMESCOPE_PANEL_ORIENTATION_0;
        }

        bool SupportsHDR() const override { return false; }
        bool IsHDRActive() const override { return false; }
        const BackendConnectorHDRInfo &GetHDRInfo() const override { return m_HDRInfo; }
        bool IsVRRActive() const override { return false; }

        std::span<const BackendMode> GetModes() const override
        {
            return std::span<const BackendMode>{};
        }

        bool SupportsVRR() const override { return false; }
        std::span<const uint8_t> GetRawEDID() const override { return std::span<const uint8_t>{}; }
        std::span<const uint32_t> GetValidDynamicRefreshRates() const override { return std::span<const uint32_t>{}; }

        void GetNativeColorimetry(
            bool bHDR10,
            displaycolorimetry_t *displayColorimetry,
            EOTF *displayEOTF,
            displaycolorimetry_t *outputEncodingColorimetry,
            EOTF *outputEncodingEOTF ) const override
        {
            *displayColorimetry = displaycolorimetry_709;
            *displayEOTF = EOTF_Gamma22;
            *outputEncodingColorimetry = displaycolorimetry_709;
            *outputEncodingEOTF = EOTF_Gamma22;
        }

        const char *GetName() const override { return "Android"; }
        const char *GetMake() const override { return "Gamescope"; }
        const char *GetModel() const override { return "Android AHardwareBuffer Bridge"; }

        int Present( const FrameInfo_t *pFrameInfo, bool bAsync ) override
        {
            m_PresentFeedback.m_uQueuedPresents++;

            std::optional<AndroidHardwareBufferFrame> oFrame = m_Bridge.ReceiveFrame();
            if ( !oFrame )
            {
                m_PresentFeedback.m_uCompletedPresents++;
                return -EAGAIN;
            }

            AndroidHardwareBufferFrame frame = *oFrame;

            wlr_dmabuf_attributes dmaBuf = {};
            dmaBuf.width = int32_t( frame.uWidth );
            dmaBuf.height = int32_t( frame.uHeight );
            dmaBuf.format = VulkanFormatToDRM( VK_FORMAT_R8G8B8A8_UNORM );
            dmaBuf.modifier = DRM_FORMAT_MOD_INVALID;
            dmaBuf.n_planes = 1;
            dmaBuf.offset[0] = 0;
            dmaBuf.stride[0] = frame.uStridePixels * 4;
            dmaBuf.fd[0] = frame.nFd;

            CVulkanTexture::createFlags textureFlags;
            textureFlags.bSampled = true;
            textureFlags.bStorage = true;
            textureFlags.bTransferSrc = true;
            textureFlags.bTransferDst = true;
            textureFlags.bOutputImage = true;
            textureFlags.bLinear = true;

            OwningRc<CVulkanTexture> pOutputTexture = new CVulkanTexture();
            bool bImported = pOutputTexture->BInit( frame.uWidth, frame.uHeight, 1u, dmaBuf.format, textureFlags, &dmaBuf );

            // CVulkanTexture duplicates the imported fd before handing it to Vulkan.
            // Close our received copy regardless of import success.
            close( frame.nFd );
            frame.nFd = -1;

            if ( !bImported )
            {
                std::fprintf( stderr, "gamescope android backend: failed to import AHB frame %u as output texture\n", frame.uFrameId );
                m_PresentFeedback.m_uCompletedPresents++;
                return -EINVAL;
            }

            std::optional<uint64_t> oCompositeSeq = vulkan_composite( const_cast<FrameInfo_t *>( pFrameInfo ), nullptr, false, pOutputTexture );
            if ( !oCompositeSeq )
            {
                std::fprintf( stderr, "gamescope android backend: vulkan_composite failed for AHB frame %u\n", frame.uFrameId );
                m_PresentFeedback.m_uCompletedPresents++;
                return -EINVAL;
            }

            vulkan_wait( *oCompositeSeq, false );

            if ( !m_Bridge.SendAck( frame.uFrameId ) )
            {
                m_PresentFeedback.m_uCompletedPresents++;
                return -EPIPE;
            }

            if ( frame.uFrameId < 5 || frame.uFrameId % 60 == 0 )
            {
                std::fprintf( stderr, "gamescope android backend: composited and acked AHB frame=%u seq=%llu\n",
                    frame.uFrameId, (unsigned long long)*oCompositeSeq );
            }

            m_PresentFeedback.m_uCompletedPresents++;
            return 0;
        }

    private:
        CAndroidBridge m_Bridge;
        BackendConnectorHDRInfo m_HDRInfo{};
    };

    class CAndroidBackend final : public CBaseBackend
    {
    public:
        bool Init() override
        {
            g_nOutputWidth = g_nPreferredOutputWidth;
            g_nOutputHeight = g_nPreferredOutputHeight;
            g_nOutputRefresh = g_nNestedRefresh;

            if ( g_nOutputHeight == 0 )
            {
                if ( g_nOutputWidth != 0 )
                {
                    std::fprintf( stderr, "Cannot specify -W without -H\n" );
                    return false;
                }
                g_nOutputHeight = 720;
            }
            if ( g_nOutputWidth == 0 )
                g_nOutputWidth = g_nOutputHeight * 16 / 9;
            if ( g_nOutputRefresh == 0 )
                g_nOutputRefresh = ConvertHztomHz( 60 );

            std::fprintf( stderr, "gamescope android backend: initializing %dx%d@%d mHz\n",
                g_nOutputWidth, g_nOutputHeight, g_nOutputRefresh );

            if ( !m_Connector.InitBridge() )
                return false;

            if ( !vulkan_init( vulkan_get_instance(), VK_NULL_HANDLE ) )
                return false;

            if ( !wlsession_init() )
            {
                std::fprintf( stderr, "Failed to initialize Wayland session\n" );
                return false;
            }

            return true;
        }

        bool PostInit() override { return true; }

        std::span<const char *const> GetInstanceExtensions() const override
        {
            return std::span<const char *const>{};
        }

        std::span<const char *const> GetDeviceExtensions( VkPhysicalDevice pVkPhysicalDevice ) const override
        {
            return std::span<const char *const>{};
        }

        VkImageLayout GetPresentLayout() const override
        {
            return VK_IMAGE_LAYOUT_GENERAL;
        }

        void GetPreferredOutputFormat( uint32_t *pPrimaryPlaneFormat, uint32_t *pOverlayPlaneFormat ) const override
        {
            // Android prototype currently uses RGBA8 AHardwareBuffers. Gamescope's
            // DRM fourcc mapping for VK_FORMAT_R8G8B8A8_UNORM is ABGR8888.
            *pPrimaryPlaneFormat = VulkanFormatToDRM( VK_FORMAT_R8G8B8A8_UNORM );
            *pOverlayPlaneFormat = VulkanFormatToDRM( VK_FORMAT_R8G8B8A8_UNORM );
        }

        bool ValidPhysicalDevice( VkPhysicalDevice pVkPhysicalDevice ) const override
        {
            // KGSL Turnip does not expose VK_EXT_physical_device_drm. Accept the
            // selected Vulkan device here; Android presentation is handled by the
            // bridge, not DRM device matching.
            return true;
        }

        void DirtyState( bool bForce, bool bForceModeset ) override {}
        bool PollState() override { return false; }

        std::shared_ptr<BackendBlob> CreateBackendBlob( const std::type_info &type, std::span<const uint8_t> data ) override
        {
            return std::make_shared<BackendBlob>( data );
        }

        OwningRc<IBackendFb> ImportDmabufToBackend( wlr_dmabuf_attributes *pDmaBuf ) override
        {
            return new CBaseBackendFb();
        }

        bool UsesModifiers() const override { return false; }
        std::span<const uint64_t> GetSupportedModifiers( uint32_t uDrmFormat ) const override
        {
            static constexpr uint64_t s_Modifiers[] = { DRM_FORMAT_MOD_INVALID };
            return std::span<const uint64_t>{ s_Modifiers };
        }

        IBackendConnector *GetCurrentConnector() override { return &m_Connector; }
        IBackendConnector *GetConnector( GamescopeScreenType eScreenType ) override
        {
            return eScreenType == GAMESCOPE_SCREEN_TYPE_INTERNAL ? &m_Connector : nullptr;
        }

        bool SupportsPlaneHardwareCursor() const override { return false; }
        bool SupportsTearing() const override { return false; }
        bool UsesVulkanSwapchain() const override { return false; }
        bool IsSessionBased() const override { return false; }
        bool SupportsExplicitSync() const override { return true; }
        bool IsPaused() const override { return false; }
        bool IsVisible() const override { return true; }
        glm::uvec2 CursorSurfaceSize( glm::uvec2 uvecSize ) const override { return uvecSize; }

    protected:
        void OnBackendBlobDestroyed( BackendBlob *pBlob ) override {}

    private:
        CAndroidConnector m_Connector;
    };

    template <>
    bool IBackend::Set<CAndroidBackend>()
    {
        return Set( new CAndroidBackend{} );
    }
}
