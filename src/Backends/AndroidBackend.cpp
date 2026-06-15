#include "backend.h"
#include "rendervulkan.hpp"
#include "wlserver.hpp"
#include "refresh_rate.h"

#include <cstdio>

extern int g_nPreferredOutputWidth;
extern int g_nPreferredOutputHeight;

namespace gamescope
{
    // Initial Android/AHardwareBuffer backend skeleton.
    //
    // This intentionally starts from the Headless backend shape: no DRM/KMS
    // device, no Vulkan WSI swapchain, but it still initializes gamescope's
    // Vulkan renderer and nested Wayland session. Follow-up patches will replace
    // Present() with AHardwareBuffer bridge acquisition/import and direct
    // vulkan_composite(..., pOutputOverride = imported_android_buffer).
    class CAndroidConnector final : public CBaseBackendConnector
    {
    public:
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
            std::fprintf( stderr, "gamescope android backend: Present layerCount=%d async=%d\n",
                pFrameInfo ? pFrameInfo->layerCount : -1,
                bAsync ? 1 : 0 );
            m_PresentFeedback.m_uCompletedPresents++;
            return 0;
        }

    private:
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
