/*
 * SPDX-FileCopyrightText: 2025 Halo Project
 * SPDX-License-Identifier: CC0-1.0
 *
 * Matter Devices - Extended Color Light and Window Covering implementation
 */

#include <string.h>
#include <math.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_err.h"

#include <esp_matter.h>
#include <esp_matter_core.h>
#include <esp_matter_cluster.h>
#include <esp_matter_endpoint.h>
#include <esp_matter_attribute.h>
#include <esp_matter_feature.h>

/* For printing QR code and pairing info */
#include <setup_payload/OnboardingCodesUtil.h>
#include <setup_payload/SetupPayload.h>

/* For Window Covering delegate */
#include <app/clusters/window-covering-server/window-covering-server.h>
#include <app-common/zap-generated/attributes/Accessors.h>

/* For rendering QR code in terminal */
#include "qrcode.h"

#include "matter_devices.h"

static const char *TAG = "matter";

/* IMPORTANT: We store a COPY of the callbacks struct, not a pointer.
 * This avoids dangling pointer issues when the caller's stack frame is destroyed. */
static matter_callbacks_t s_callbacks = {};

/* ============================================================================
   WINDOW COVERING DELEGATE
   ============================================================================
   This delegate handles movement commands from Google Home/Apple HomeKit.
   When the controller sends "open blinds" or "set position to 50%", 
   the SDK calls our HandleMovement() method.
   ============================================================================ */

class HaloBlindsDelegate : public chip::app::Clusters::WindowCovering::Delegate
{
public:
    CHIP_ERROR HandleMovement(chip::app::Clusters::WindowCovering::WindowCoveringType type) override
    {
        if (type != chip::app::Clusters::WindowCovering::WindowCoveringType::Lift) {
            return CHIP_NO_ERROR;  /* We only handle Lift, not Tilt */
        }
        
        /* Get the target position that was set by the SDK */
        chip::EndpointId ep = mEndpoint;
        chip::app::Clusters::WindowCovering::NPercent100ths targetPos;
        chip::app::Clusters::WindowCovering::Attributes::TargetPositionLiftPercent100ths::Get(ep, targetPos);
        
        if (!targetPos.IsNull()) {
            uint16_t pos100ths = targetPos.Value();
            uint8_t percent = (uint8_t)(pos100ths / 100);
            
            ESP_LOGI("matter", ">>> HandleMovement: Blinds to %d%% (via Matter delegate)", percent);
            
            if (s_callbacks.blinds_position) {
                s_callbacks.blinds_position(percent);
            }
        }
        
        return CHIP_NO_ERROR;
    }
    
    CHIP_ERROR HandleStopMotion() override
    {
        ESP_LOGI("matter", ">>> HandleStopMotion: Blinds STOP (via Matter delegate)");
        
        if (s_callbacks.blinds_stop) {
            s_callbacks.blinds_stop();
        }
        
        return CHIP_NO_ERROR;
    }
};

/* Static instance of the delegate */
static HaloBlindsDelegate s_blinds_delegate;

/* ============================================================================
   STATIC STATE
   ============================================================================ */

/* Note: s_callbacks is defined above (copy of struct, not pointer) */
static uint16_t s_light_endpoint_id = 0;
static uint16_t s_blinds_endpoint_id = 0;
static bool s_matter_initialized = false;

/* Current state cache */
static matter_light_state_t s_light_state = {
    .on = true,
    .brightness = 254,
    .hue = 200,         /* Purple-ish default */
    .saturation = 254,
    .color_temp_mireds = 0,
};

static matter_blinds_state_t s_blinds_state = {
    .current_position = 100,    /* Fully open */
    .target_position = 100,
    .is_moving = false,
};

/* ============================================================================
   HELPER: HSV TO RGB CONVERSION
   ============================================================================ */

static void hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v,
                       uint8_t *r, uint8_t *g, uint8_t *b)
{
    /* Matter hue is 0-254 (maps to 0-360 degrees) */
    /* Matter saturation is 0-254 (maps to 0-100%) */
    /* v (value/brightness) is 0-254 */
    
    if (s == 0) {
        /* Achromatic (gray) */
        *r = *g = *b = v;
        return;
    }
    
    float hue = (float)h / 254.0f * 360.0f;
    float sat = (float)s / 254.0f;
    float val = (float)v / 254.0f;
    
    int i = (int)(hue / 60.0f) % 6;
    float f = (hue / 60.0f) - i;
    float p = val * (1.0f - sat);
    float q = val * (1.0f - f * sat);
    float t = val * (1.0f - (1.0f - f) * sat);
    
    float rf, gf, bf;
    switch (i) {
        case 0: rf = val; gf = t;   bf = p;   break;
        case 1: rf = q;   gf = val; bf = p;   break;
        case 2: rf = p;   gf = val; bf = t;   break;
        case 3: rf = p;   gf = q;   bf = val; break;
        case 4: rf = t;   gf = p;   bf = val; break;
        default: rf = val; gf = p;   bf = q;  break;
    }
    
    *r = (uint8_t)(rf * 255.0f);
    *g = (uint8_t)(gf * 255.0f);
    *b = (uint8_t)(bf * 255.0f);
}

/* ============================================================================
   MATTER ATTRIBUTE CALLBACK
   ============================================================================ */

static esp_err_t matter_attribute_update_cb(
    esp_matter::attribute::callback_type_t type,
    uint16_t endpoint_id,
    uint32_t cluster_id,
    uint32_t attribute_id,
    esp_matter_attr_val_t *val,
    void *priv_data)
{
    if (type != esp_matter::attribute::callback_type_t::PRE_UPDATE) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Attribute update: endpoint=%d, cluster=0x%04" PRIx32 ", attr=0x%04" PRIx32,
             endpoint_id, cluster_id, attribute_id);
    
    /* ========== LIGHT ENDPOINT ========== */
    if (endpoint_id == s_light_endpoint_id) {
        
        /* On/Off cluster */
        if (cluster_id == chip::app::Clusters::OnOff::Id) {
            if (attribute_id == chip::app::Clusters::OnOff::Attributes::OnOff::Id) {
                bool on = val->val.b;
                s_light_state.on = on;
                ESP_LOGI(TAG, "Light On/Off: %s", on ? "ON" : "OFF");
                if (s_callbacks.light_on_off) {
                    s_callbacks.light_on_off(on);
                }
            }
        }
        
        /* Level Control cluster (brightness) */
        else if (cluster_id == chip::app::Clusters::LevelControl::Id) {
            if (attribute_id == chip::app::Clusters::LevelControl::Attributes::CurrentLevel::Id) {
                uint8_t level = val->val.u8;
                s_light_state.brightness = level;
                /* Convert Matter level (1-254) to percentage (1-100) */
                uint8_t percent = (uint8_t)((float)level / 254.0f * 100.0f);
                ESP_LOGI(TAG, "Light Brightness: %d%% (level=%d)", percent, level);
                if (s_callbacks.light_brightness) {
                    s_callbacks.light_brightness(percent);
                }
            }
        }
        
        /* Color Control cluster */
        else if (cluster_id == chip::app::Clusters::ColorControl::Id) {
            
            /* Color Mode changed - important to track which mode we're in */
            if (attribute_id == chip::app::Clusters::ColorControl::Attributes::ColorMode::Id) {
                uint8_t mode = val->val.u8;
                ESP_LOGI(TAG, "Light ColorMode changed to: %d (%s)", mode,
                         mode == 0 ? "HS" : mode == 1 ? "XY" : mode == 2 ? "ColorTemp" : "Unknown");
                /* Mode 0 = Hue/Saturation, Mode 2 = Color Temperature */
            }
            
            /* Hue changed (RGB mode) */
            else if (attribute_id == chip::app::Clusters::ColorControl::Attributes::CurrentHue::Id) {
                s_light_state.hue = val->val.u8;
                ESP_LOGI(TAG, "Light Hue: %d", s_light_state.hue);
                
                if (s_callbacks.light_color) {
                    uint8_t r, g, b;
                    hsv_to_rgb(s_light_state.hue, s_light_state.saturation, 
                               s_light_state.brightness, &r, &g, &b);
                    ESP_LOGI(TAG, "Light RGB Color: R=%d G=%d B=%d", r, g, b);
                    s_callbacks.light_color(r, g, b);
                }
            }
            
            /* Saturation changed (RGB mode) */
            else if (attribute_id == chip::app::Clusters::ColorControl::Attributes::CurrentSaturation::Id) {
                s_light_state.saturation = val->val.u8;
                ESP_LOGI(TAG, "Light Saturation: %d", s_light_state.saturation);
                
                if (s_callbacks.light_color) {
                    uint8_t r, g, b;
                    hsv_to_rgb(s_light_state.hue, s_light_state.saturation, 
                               s_light_state.brightness, &r, &g, &b);
                    ESP_LOGI(TAG, "Light RGB Color: R=%d G=%d B=%d", r, g, b);
                    s_callbacks.light_color(r, g, b);
                }
            }
            
            /* Color Temperature changed (White mode for RGBW) */
            else if (attribute_id == chip::app::Clusters::ColorControl::Attributes::ColorTemperatureMireds::Id) {
                uint16_t mireds = val->val.u16;
                s_light_state.color_temp_mireds = (uint8_t)(mireds > 255 ? 255 : mireds);
                
                /* Convert mireds to Kelvin for logging: K = 1,000,000 / mireds */
                uint32_t kelvin = (mireds > 0) ? (1000000 / mireds) : 0;
                ESP_LOGI(TAG, "Light Color Temp: %d mireds (~%luK) - %s", 
                         mireds, (unsigned long)kelvin,
                         mireds < 250 ? "Cool/Daylight" : mireds < 400 ? "Neutral/Warm" : "Very Warm");
                
                if (s_callbacks.light_color_temp) {
                    s_callbacks.light_color_temp(mireds);
                }
            }
        }
    }
    
    /* ========== BLINDS ENDPOINT ========== */
    else if (endpoint_id == s_blinds_endpoint_id) {
        
        /* Window Covering cluster */
        if (cluster_id == chip::app::Clusters::WindowCovering::Id) {
            
            /* Target position (0-10000 in Matter, map to 0-100%) */
            if (attribute_id == chip::app::Clusters::WindowCovering::Attributes::TargetPositionLiftPercent100ths::Id) {
                uint16_t pos_100ths = val->val.u16;
                uint8_t percent = (uint8_t)(pos_100ths / 100);
                s_blinds_state.target_position = percent;
                ESP_LOGI(TAG, "Blinds Target Position: %d%%", percent);
                if (s_callbacks.blinds_position) {
                    s_callbacks.blinds_position(percent);
                }
            }
        }
    }
    
    return ESP_OK;
}

/* ============================================================================
   MATTER EVENT CALLBACK
   ============================================================================ */

static void matter_event_cb(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
        case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════╗");
            ESP_LOGI(TAG, "║  ✅ MATTER COMMISSIONING COMPLETE!                       ║");
            ESP_LOGI(TAG, "║  Device is now paired with your smart home hub.          ║");
            ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════╝");
            ESP_LOGI(TAG, "");
            break;
            
        case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
            ESP_LOGW(TAG, "Matter fabric removed - device unpaired");
            break;
            
        case chip::DeviceLayer::DeviceEventType::kFabricCommitted:
            ESP_LOGI(TAG, "Matter fabric committed");
            break;
            
        default:
            break;
    }
}

/* ============================================================================
   INITIALIZATION
   ============================================================================ */

/* Internal initialization - separated so we can wrap in try/catch */
static esp_err_t matter_devices_init_internal(const matter_callbacks_t *callbacks)
{
    /* IMPORTANT: Copy the struct, don't store pointer!
     * This prevents crashes from dangling pointers if caller's stack is destroyed. */
    if (callbacks) {
        s_callbacks = *callbacks;  /* Copy the entire struct */
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  🏠 INITIALIZING MATTER SMART HOME                       ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    /* Create Matter node - use value initialization instead of memset */
    ESP_LOGI(TAG, "  [1/5] Creating Matter node...");
    esp_matter::node::config_t node_config = {};
    
    esp_matter::node_t *node = esp_matter::node::create(&node_config, 
                                                         matter_attribute_update_cb,
                                                         nullptr);
    if (!node) {
        ESP_LOGE(TAG, "  ❌ Failed to create Matter node");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "  ✓ Matter node created");
    
    /* ========== CREATE LIGHT ENDPOINT ========== */
    ESP_LOGI(TAG, "  [2/5] Creating Extended Color Light endpoint...");
    
    /* Use value initialization */
    esp_matter::endpoint::extended_color_light::config_t light_config = {};
    
    /* Set default values */
    light_config.on_off.on_off = s_light_state.on;
    light_config.level_control.current_level = s_light_state.brightness;
    light_config.color_control.color_mode = 0;  /* HS mode */
    light_config.color_control.enhanced_color_mode = 0;
    
    esp_matter::endpoint_t *light_ep = esp_matter::endpoint::extended_color_light::create(
        node, &light_config, esp_matter::endpoint_flags::ENDPOINT_FLAG_NONE, nullptr);
    
    if (!light_ep) {
        ESP_LOGE(TAG, "  ❌ Failed to create light endpoint");
        return ESP_FAIL;
    }
    
    s_light_endpoint_id = esp_matter::endpoint::get_id(light_ep);
    ESP_LOGI(TAG, "  ✓ Light endpoint created (ID=%d)", s_light_endpoint_id);
    
    /* Add Hue/Saturation feature to ColorControl cluster - required for HS color mode */
    esp_matter::cluster_t *color_cluster = esp_matter::cluster::get(light_ep, chip::app::Clusters::ColorControl::Id);
    if (color_cluster) {
        esp_matter::cluster::color_control::feature::hue_saturation::config_t hs_config = {};
        hs_config.current_hue = s_light_state.hue;
        hs_config.current_saturation = s_light_state.saturation;
        esp_err_t hs_err = esp_matter::cluster::color_control::feature::hue_saturation::add(color_cluster, &hs_config);
        if (hs_err == ESP_OK) {
            ESP_LOGI(TAG, "  ✓ Added Hue/Saturation feature to ColorControl");
        } else {
            ESP_LOGW(TAG, "  ⚠ Failed to add HS feature: %s", esp_err_to_name(hs_err));
        }
    }
    
    /* ========== CREATE BLINDS ENDPOINT ========== */
    ESP_LOGI(TAG, "  [3/5] Creating Window Covering endpoint...");
    
    /* Use value initialization */
    esp_matter::endpoint::window_covering_device::config_t blinds_config = {};
    
    /* Set window covering type and features */
    blinds_config.window_covering.type = 0;  /* Rollershade */
    blinds_config.window_covering.config_status = 0x00;
    blinds_config.window_covering.operational_status = 0x00;
    blinds_config.window_covering.mode = 0;
    
    /* IMPORTANT: Must enable Lift feature - Window Covering requires at least one of Lift/Tilt */
    blinds_config.window_covering.feature_flags = 
        esp_matter::cluster::window_covering::feature::lift::get_id() |
        esp_matter::cluster::window_covering::feature::position_aware_lift::get_id();
    
    ESP_LOGI(TAG, "        Feature flags: 0x%08lX (Lift + PositionAwareLift)", 
             (unsigned long)blinds_config.window_covering.feature_flags);
    
    esp_matter::endpoint_t *blinds_ep = esp_matter::endpoint::window_covering_device::create(
        node, &blinds_config, esp_matter::endpoint_flags::ENDPOINT_FLAG_NONE, nullptr);
    
    if (!blinds_ep) {
        ESP_LOGE(TAG, "  ❌ Failed to create blinds endpoint");
        return ESP_FAIL;
    }
    
    s_blinds_endpoint_id = esp_matter::endpoint::get_id(blinds_ep);
    ESP_LOGI(TAG, "  ✓ Blinds endpoint created (ID=%d)", s_blinds_endpoint_id);
    
    /* Register Window Covering delegate for handling commands */
    s_blinds_delegate.SetEndpoint(s_blinds_endpoint_id);
    chip::app::Clusters::WindowCovering::SetDefaultDelegate(s_blinds_endpoint_id, &s_blinds_delegate);
    ESP_LOGI(TAG, "  ✓ Window Covering delegate registered");
    
    /* Register event callback */
    ESP_LOGI(TAG, "  [4/5] Registering event handlers...");
    chip::DeviceLayer::PlatformMgr().AddEventHandler(matter_event_cb, 0);
    ESP_LOGI(TAG, "  ✓ Event handlers registered");
    
    /* Start Matter - pass the event callback */
    ESP_LOGI(TAG, "  [5/5] Starting Matter stack...");
    esp_err_t err = esp_matter::start(matter_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "  ❌ Failed to start Matter: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "  ✓ Matter stack started");
    
    s_matter_initialized = true;
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "  ✅ Matter initialized successfully!");
    ESP_LOGI(TAG, "     Light endpoint:  %d", s_light_endpoint_id);
    ESP_LOGI(TAG, "     Blinds endpoint: %d", s_blinds_endpoint_id);
    ESP_LOGI(TAG, "");
    
    return ESP_OK;
}

esp_err_t matter_devices_init(const matter_callbacks_t *callbacks)
{
    if (s_matter_initialized) {
        ESP_LOGW(TAG, "Matter already initialized");
        return ESP_OK;
    }
    
    /* NOTE: ESP-IDF has C++ exceptions disabled by default.
     * We ensure proper configuration (e.g., feature flags) to avoid SDK assertions.
     * Any failures return error codes rather than crashing. */
    return matter_devices_init_internal(callbacks);
}

esp_err_t matter_start_commissioning(void)
{
    if (!s_matter_initialized) {
        ESP_LOGE(TAG, "Matter not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  📱 MATTER COMMISSIONING MODE                            ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    /* Get the actual QR code and manual pairing code from the SDK */
    char qr_code[128] = {0};
    char manual_code[32] = {0};
    
    /* Get QR code payload */
    chip::MutableCharSpan qr_span(qr_code, sizeof(qr_code));
    if (GetQRCode(qr_span, chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kOnNetwork)) == CHIP_NO_ERROR) {
        ESP_LOGI(TAG, "  QR Code Payload: %s", qr_code);
        ESP_LOGI(TAG, "");
        
        /* Render QR code directly in the terminal */
        esp_qrcode_config_t qr_config = ESP_QRCODE_CONFIG_DEFAULT();
        ESP_LOGI(TAG, "  📱 Scan this QR code with Google Home app:");
        ESP_LOGI(TAG, "");
        esp_qrcode_generate(&qr_config, qr_code);
        ESP_LOGI(TAG, "");
        
        ESP_LOGI(TAG, "  URL: https://project-chip.github.io/connectedhomeip/qrcode.html?data=%s", qr_code);
        ESP_LOGI(TAG, "");
    } else {
        ESP_LOGW(TAG, "  Could not generate QR code");
    }
    
    /* Get manual pairing code (11 digits) */
    chip::MutableCharSpan manual_span(manual_code, sizeof(manual_code));
    if (GetManualPairingCode(manual_span, chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kOnNetwork)) == CHIP_NO_ERROR) {
        ESP_LOGI(TAG, "  ┌─────────────────────────────────────────────────────────┐");
        ESP_LOGI(TAG, "  │  MANUAL PAIRING CODE (enter in Google Home app):        │");
        ESP_LOGI(TAG, "  │                                                         │");
        ESP_LOGI(TAG, "  │     >>> %s <<<", manual_code);
        ESP_LOGI(TAG, "  │                                                         │");
        ESP_LOGI(TAG, "  └─────────────────────────────────────────────────────────┘");
    } else {
        ESP_LOGW(TAG, "  Could not generate manual pairing code");
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "  Open Google Home → + → Set up device → New device");
    ESP_LOGI(TAG, "  Then select 'Matter device' and enter the code above");
    ESP_LOGI(TAG, "");
    
    return ESP_OK;
}

bool matter_is_commissioned(void)
{
    if (!s_matter_initialized) {
        return false;
    }
    
    return chip::DeviceLayer::ConnectivityMgr().IsWiFiStationConnected() &&
           chip::Server::GetInstance().GetFabricTable().FabricCount() > 0;
}

/* ============================================================================
   STATE UPDATES (from hardware to Matter)
   ============================================================================ */

void matter_update_light_on_off(bool on)
{
    if (!s_matter_initialized) return;
    
    s_light_state.on = on;
    
    esp_matter_attr_val_t val = esp_matter_bool(on);
    esp_matter::attribute::update(s_light_endpoint_id,
                                   chip::app::Clusters::OnOff::Id,
                                   chip::app::Clusters::OnOff::Attributes::OnOff::Id,
                                   &val);
}

void matter_update_light_brightness(uint8_t brightness)
{
    if (!s_matter_initialized) return;
    
    /* Convert 0-100% to Matter level 1-254 */
    uint8_t level = (uint8_t)((float)brightness / 100.0f * 254.0f);
    if (level < 1) level = 1;
    
    s_light_state.brightness = level;
    
    esp_matter_attr_val_t val = esp_matter_uint8(level);
    esp_matter::attribute::update(s_light_endpoint_id,
                                   chip::app::Clusters::LevelControl::Id,
                                   chip::app::Clusters::LevelControl::Attributes::CurrentLevel::Id,
                                   &val);
}

void matter_update_light_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_matter_initialized) return;
    
    /* Convert RGB to HSV for Matter */
    float rf = r / 255.0f;
    float gf = g / 255.0f;
    float bf = b / 255.0f;
    
    float max_val = fmaxf(rf, fmaxf(gf, bf));
    float min_val = fminf(rf, fminf(gf, bf));
    float delta = max_val - min_val;
    
    float h = 0, s = 0;
    
    if (delta > 0) {
        if (max_val == rf) {
            h = 60.0f * fmodf((gf - bf) / delta, 6.0f);
        } else if (max_val == gf) {
            h = 60.0f * ((bf - rf) / delta + 2.0f);
        } else {
            h = 60.0f * ((rf - gf) / delta + 4.0f);
        }
        
        if (h < 0) h += 360.0f;
        s = (max_val > 0) ? (delta / max_val) : 0;
    }
    
    /* Convert to Matter scale (0-254) */
    s_light_state.hue = (uint8_t)(h / 360.0f * 254.0f);
    s_light_state.saturation = (uint8_t)(s * 254.0f);
    
    esp_matter_attr_val_t hue_val = esp_matter_uint8(s_light_state.hue);
    esp_matter::attribute::update(s_light_endpoint_id,
                                   chip::app::Clusters::ColorControl::Id,
                                   chip::app::Clusters::ColorControl::Attributes::CurrentHue::Id,
                                   &hue_val);
    
    esp_matter_attr_val_t sat_val = esp_matter_uint8(s_light_state.saturation);
    esp_matter::attribute::update(s_light_endpoint_id,
                                   chip::app::Clusters::ColorControl::Id,
                                   chip::app::Clusters::ColorControl::Attributes::CurrentSaturation::Id,
                                   &sat_val);
}

void matter_update_blinds_position(uint8_t position, bool is_moving)
{
    if (!s_matter_initialized) return;
    
    s_blinds_state.current_position = position;
    s_blinds_state.is_moving = is_moving;
    
    /* Matter uses 0-10000 for position (percent * 100) */
    uint16_t pos_100ths = position * 100;
    
    esp_matter_attr_val_t pos_val = esp_matter_nullable_uint16(pos_100ths);
    esp_matter::attribute::update(s_blinds_endpoint_id,
                                   chip::app::Clusters::WindowCovering::Id,
                                   chip::app::Clusters::WindowCovering::Attributes::CurrentPositionLiftPercent100ths::Id,
                                   &pos_val);
    
    /* Update operational status */
    uint8_t op_status = is_moving ? 0x01 : 0x00;
    esp_matter_attr_val_t status_val = esp_matter_uint8(op_status);
    esp_matter::attribute::update(s_blinds_endpoint_id,
                                   chip::app::Clusters::WindowCovering::Id,
                                   chip::app::Clusters::WindowCovering::Attributes::OperationalStatus::Id,
                                   &status_val);
}

/* ============================================================================
   FACTORY RESET
   ============================================================================ */

void matter_factory_reset(void)
{
    ESP_LOGW(TAG, "");
    ESP_LOGW(TAG, "╔══════════════════════════════════════════════════════════╗");
    ESP_LOGW(TAG, "║  ⚠️ MATTER FACTORY RESET                                  ║");
    ESP_LOGW(TAG, "║  Device will need to be re-commissioned.                 ║");
    ESP_LOGW(TAG, "╚══════════════════════════════════════════════════════════╝");
    ESP_LOGW(TAG, "");
    
    esp_matter::factory_reset();
}
