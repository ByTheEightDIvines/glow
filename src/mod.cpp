// Master Sword Glow
//
// A soft, BotW-style glow on the Master Sword while it is drawn: a bright core over the blade and a
// wide soft halo around it, both drawn as camera-facing ribbons with additive blending, plus a
// coloured light cast into the world.
//
// Rules:
//  - Only while the Master Sword is in Link's hand (unsheathed) and actually being drawn.
//  - Never while the vanilla Light Sword (Sol) glow is active: that glow is left untouched, and this
//    one fades out while it plays.
//  - Colour comes from the Hero of Legend "Blade glow" dropdown. Hero of Legend (a Luau mod) publishes
//    an empty marker file on the virtual disc - /res/hol_swordglow_gold.bin or
//    /res/hol_swordglow_cyan.bin - and this mod checks for it a few times a second. No marker = off.

#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphBase/J3DPacket.h"
#include "JSystem/J3DGraphBase/J3DShape.h"
#include "JSystem/J3DGraphBase/J3DSys.h"
#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "dolphin/dvd.h"
#include "dolphin/gx.h"
#include "helpers/gx_helper.h"
#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/hook.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/interp.h"
#include "mods/svc/log.h"
#include "mods/svc/ui.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(InterpService, svc_interp);

DEFINE_HOOK(&daAlink_c::setLightningSwordEffect, LinkSolEffect);
DEFINE_HOOK(&daAlink_c::setLight, LinkSetLight);
DEFINE_HOOK(&daAlink_c::modelDraw, LinkModelDraw);

namespace {

// ---------------------------------------------------------------------------------------------
// Tunables (base values; the mod's settings panel scales them in percent)

// Blade in the sword model's own space: the blade runs along +X from the guard to the tip.
constexpr float kBladeStart = 10.0f;
constexpr float kBladeEnd = 117.0f;
constexpr float kBladeCenterY = 0.5f;

constexpr float kCoreWidth = 5.6f;   // half-width of the bright core at the guard
constexpr float kHaloWidth = 18.0f;  // half-width of the soft halo at the guard
constexpr float kTipTaper = 0.7f;    // width at the tip relative to the guard
constexpr float kHaloCap = 0.4f;     // how far the halo runs past the tip/guard, relative to its width
constexpr float kCoreCap = 0.5f;
constexpr float kCoreBias = 2.5f;    // pull the core toward the camera so it sits over the blade
constexpr float kHaloBias = 1.5f;

constexpr float kCoreAlpha = 0.75f;
constexpr float kLightScale = 0.3f;  // world light relative to vanilla's glowing-sword light
constexpr float kHaloAlpha = 0.42f;

constexpr float kFadeIn = 0.15f;   // per game tick
constexpr float kFadeOut = 0.25f;

enum GlowColor { GLOW_OFF = 0, GLOW_GOLD = 1, GLOW_CYAN = 2 };

struct Palette {
    GXColor core;
    GXColor halo;
    GXColor light;
};

const Palette kPalettes[3] = {
    {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
    // Gold: the light is exactly the colour vanilla uses for the glowing sword.
    {{255, 240, 190, 255}, {255, 190, 70, 255}, {150, 174, 141, 255}},
    // Cyan: blue light of similar strength.
    {{190, 245, 255, 255}, {40, 190, 255, 255}, {80, 170, 215, 255}},
};

// ---------------------------------------------------------------------------------------------
// State

int g_color = GLOW_OFF;        // from the Hero of Legend marker
float g_fade = 0.0f;           // 0..1, advanced once per game tick
float g_lightPower = 0.0f;     // chased like vanilla's sword light
uint32_t g_tick = 0;
uint32_t g_enteredTick = UINT32_MAX;  // the packet is queued at most once per tick
int g_pollCounter = 0;

ConfigVarHandle g_cvHaloSize = 0;
ConfigVarHandle g_cvHaloBright = 0;
ConfigVarHandle g_cvCoreBright = 0;
ConfigVarHandle g_cvLight = 0;

float percent(ConfigVarHandle var) {
    int64_t v = 100;
    if (var != 0) {
        svc_config->get_int(mod_ctx, var, &v);
    }
    return static_cast<float>(v) / 100.0f;
}

// ---------------------------------------------------------------------------------------------
// Glow texture: a soft capsule. U runs along the blade (0-0.25 and 0.75-1 are the rounded ends,
// the middle stretches over the blade), V runs across it. Stored as I8, so alpha = intensity.

constexpr int kTexW = 64;
constexpr int kTexH = 32;
alignas(32) uint8_t g_texData[kTexW * kTexH];
TGXTexObj g_texObj;
bool g_texReady = false;

float glow_falloff(float u, float v) {
    float du = 0.0f;
    if (u < 0.25f) du = (0.25f - u) / 0.25f;
    else if (u > 0.75f) du = (u - 0.75f) / 0.25f;
    const float dv = std::fabs(v - 0.5f) / 0.5f;
    const float d = std::sqrt(du * du + dv * dv);
    if (d >= 1.0f) return 0.0f;
    const float t = 1.0f - d;
    return t * t * (0.35f + 0.65f * t);  // bright centre, long soft tail
}

void build_texture() {
    // I8 is stored in 8x4 tiles.
    int i = 0;
    for (int ty = 0; ty < kTexH; ty += 4) {
        for (int tx = 0; tx < kTexW; tx += 8) {
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 8; x++) {
                    const float u = (tx + x + 0.5f) / kTexW;
                    const float v = (ty + y + 0.5f) / kTexH;
                    float f = glow_falloff(u, v);
                    if (tx + x == 0 || tx + x == kTexW - 1 || ty + y == 0 || ty + y == kTexH - 1) {
                        f = 0.0f;  // clean edges under CLAMP
                    }
                    g_texData[i++] = static_cast<uint8_t>(f * 255.0f + 0.5f);
                }
            }
        }
    }
}

void ensure_texture() {
    if (g_texReady) return;
    GXInitTexObj(&g_texObj, g_texData, kTexW, kTexH, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObjLOD(&g_texObj, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    g_texReady = true;
}

// ---------------------------------------------------------------------------------------------
// Small vector helpers

struct V3 {
    float x, y, z;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float len(V3 a) { return std::sqrt(dot(a, a)); }
inline V3 norm(V3 a) {
    const float l = len(a);
    return l > 1e-6f ? a * (1.0f / l) : V3{0, 0, 0};
}
inline V3 xform(const float (*m)[4], V3 p) {
    return {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3],
        m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
        m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3]};
}

// ---------------------------------------------------------------------------------------------
// Draw packet (translucent, z-sorted with the rest of the scene)

class GlowPacket : public J3DPacket {
public:
    J3DModel* mSword = nullptr;
    void draw() override;
};
GlowPacket g_packet;

void put_vertex(V3 p, float s, float t) {
    GXPosition3f32(p.x, p.y, p.z);
    GXTexCoord2f32(s, t);
}

// One camera-facing capsule ribbon from a to b. wa/wb: half-widths at each end.
void draw_ribbon(V3 a, V3 b, float wa, float wb, float cap, V3 cam, float bias) {
    const V3 dir = norm(b - a);
    const V3 mid = (a + b) * 0.5f;
    const V3 toCam = norm(cam - mid);
    V3 side = cross(dir, toCam);
    if (len(side) < 1e-3f) {
        // Looking straight down the blade: any direction across the view works.
        side = cross(toCam, std::fabs(toCam.y) < 0.9f ? V3{0, 1, 0} : V3{1, 0, 0});
    }
    side = norm(side);
    const V3 off = toCam * bias;
    const V3 a0 = a - dir * (wa * cap) + off, a1 = a + off, b1 = b + off, b0 = b + dir * (wb * cap) + off;
    const V3 sa = side * wa, sb = side * wb;

    GXBegin(GX_QUADS, GX_VTXFMT0, 12);
    // guard cap
    put_vertex(a0 - sa, 0.0f, 0.0f);
    put_vertex(a0 + sa, 0.0f, 1.0f);
    put_vertex(a1 + sa, 0.25f, 1.0f);
    put_vertex(a1 - sa, 0.25f, 0.0f);
    // blade
    put_vertex(a1 - sa, 0.25f, 0.0f);
    put_vertex(a1 + sa, 0.25f, 1.0f);
    put_vertex(b1 + sb, 0.75f, 1.0f);
    put_vertex(b1 - sb, 0.75f, 0.0f);
    // tip cap
    put_vertex(b1 - sb, 0.75f, 0.0f);
    put_vertex(b1 + sb, 0.75f, 1.0f);
    put_vertex(b0 + sb, 1.0f, 1.0f);
    put_vertex(b0 - sb, 1.0f, 0.0f);
    GXEnd();
}

void set_layer_color(GXColor c, float alpha) {
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    c.a = static_cast<u8>(alpha * 255.0f);
    GXSetTevColor(GX_TEVREG0, c);
}

void GlowPacket::draw() {
    if (mSword == nullptr || g_fade <= 0.0f || g_color == GLOW_OFF) return;

    // Sword matrix: use the frame-interpolated one when running above the game's tick rate.
    MtxP simMtx = mSword->getAnmMtx(0);
    Mtx interp;
    const float (*m)[4] = simMtx;
    if (svc_interp != nullptr && svc_interp->lookup_replacement_mtx(simMtx, interp)) {
        m = interp;
    }

    const V3 a = xform(m, {kBladeStart, kBladeCenterY, 0.0f});
    const V3 b = xform(m, {kBladeEnd, kBladeCenterY, 0.0f});

    // Camera position from the view matrix (inverse of a rigid transform).
    MtxP view = j3dSys.getViewMtx();
    const V3 t = {view[0][3], view[1][3], view[2][3]};
    const V3 cam = {-(view[0][0] * t.x + view[1][0] * t.y + view[2][0] * t.z),
        -(view[0][1] * t.x + view[1][1] * t.y + view[2][1] * t.z),
        -(view[0][2] * t.x + view[1][2] * t.y + view[2][2] * t.z)};

    // Scale from the sword's own matrix (Link is not always unit scale).
    const float scale = len(V3{m[0][0], m[1][0], m[2][0]});

    const Palette& pal = kPalettes[g_color];
    const float pulse = 0.93f + 0.07f * std::sin(static_cast<float>(g_tick) * 0.21f);
    const float haloSize = percent(g_cvHaloSize);

    ensure_texture();
    j3dSys.reinitGX();
    GXSetNumIndStages(0);
    GXLoadTexObj(&g_texObj, GX_TEXMAP0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXSetNumChans(0);
    GXSetNumTexGens(1);
    GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GXSetNumTevStages(1);
    GXSetTevDirect(GX_TEVSTAGE0);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_A0, GX_CA_TEXA, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXLoadPosMtxImm(view, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_SET);  // additive
    GXSetCullMode(GX_CULL_NONE);
    GXSetZMode(GX_ENABLE, GX_LEQUAL, GX_DISABLE);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);

    const float fade = g_fade * pulse;

    // Halo first, then the core on top.
    const float hw = kHaloWidth * haloSize * scale;
    set_layer_color(pal.halo, kHaloAlpha * percent(g_cvHaloBright) * fade);
    draw_ribbon(a, b, hw, hw * kTipTaper, kHaloCap, cam, kHaloBias * scale);

    const float cw = kCoreWidth * scale;
    set_layer_color(pal.core, kCoreAlpha * percent(g_cvCoreBright) * fade);
    draw_ribbon(a, b, cw, cw * kTipTaper, kCoreCap, cam, kCoreBias * scale);

    J3DShape::resetVcdVatCache();
}

// ---------------------------------------------------------------------------------------------
// Game hooks

bool sword_in_hand(daAlink_c* link) {
    return link->mEquipItem == 0x103 && !link->checkWolf() && daPy_py_c::checkMasterSwordEquip();
}

bool sol_active(daAlink_c* link) {
    return link->checkNoResetFlg3(daPy_py_c::FLG3_UNK_100000) != 0;
}

void approach(float& v, float target, float up, float down) {
    if (v < target) {
        v = v + up > target ? target : v + up;
    } else if (v > target) {
        v = v - down < target ? target : v - down;
    }
}

// Runs once per game tick, right after the vanilla Sol effect update.
void on_sol_effect_post(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    if (link == nullptr) return;
    g_tick++;

    if (!sword_in_hand(link)) {
        g_fade = 0.0f;  // sheathed or another item: off at once, never on the scabbard
        return;
    }
    const bool want = g_color != GLOW_OFF && !sol_active(link);
    approach(g_fade, want ? 1.0f : 0.0f, kFadeIn, kFadeOut);
}

// Runs after vanilla's own light update. Vanilla's lantern and Sol light take priority.
void on_set_light_post(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    if (link == nullptr || link->checkWolf()) return;

    const daAlinkHIO_huLight_c1* light_m = &link->mpHIO->mItem.mLanternPL.m;
    const bool vanillaLight = link->checkNoResetFlg1(daPy_py_c::FLG1_UNK_80) != 0;

    float target = 0.0f;
    if (!vanillaLight && g_fade > 0.0f && g_color != GLOW_OFF) {
        target = (link->mCutType == 0 ? 0.4f : 1.5f) * kLightScale * g_fade * percent(g_cvLight);
    }
    const float step = light_m->mPower * 0.2f;
    approach(g_lightPower, target, step, step);

    if (vanillaLight || g_lightPower <= 0.0f || g_color == GLOW_OFF || light_m->mPower <= 0.0f) {
        return;
    }

    cXyz pos = (link->mSwordTopPos + link->field_0x3498) * 0.5f;
    GXColor color = kPalettes[g_color].light;
    dKy_WolfEyeLight_set(&pos, light_m->mXAngle, cM_sht2d(-link->shape_angle.y),
        (light_m->mWidth * g_lightPower) / light_m->mPower, &color, g_lightPower,
        light_m->mAngleAttenuationType, light_m->mDistanceAttenuationType);
}

// Queue the glow whenever the sword model itself is queued for drawing (so it hides whenever the
// game hides the sword: first person, hidden-player cutscenes, and so on).
void on_model_draw_post(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    J3DModel* model = mods::arg<J3DModel*>(args, 1);
    const int hidden = mods::arg<int>(args, 2);
    if (link == nullptr || model == nullptr || hidden != 0) return;
    if (model != link->mSwordModel || g_fade <= 0.0f || g_color == GLOW_OFF) return;
    if (!sword_in_hand(link)) return;

    if (g_enteredTick == g_tick) return;
    g_enteredTick = g_tick;
    g_packet.mSword = model;
    const V3 mid = xform(model->getAnmMtx(0), {(kBladeStart + kBladeEnd) * 0.5f, kBladeCenterY, 0.0f});
    cXyz sortPos(mid.x, mid.y, mid.z);
    dComIfGd_entryZSortXluList(&g_packet, sortPos);
}

// ---------------------------------------------------------------------------------------------
// Hero of Legend marker

void poll_marker() {
    int color = GLOW_OFF;
    if (DVDConvertPathToEntrynum("/res/hol_swordglow_gold.bin") >= 0) {
        color = GLOW_GOLD;
    } else if (DVDConvertPathToEntrynum("/res/hol_swordglow_cyan.bin") >= 0) {
        color = GLOW_CYAN;
    }
    if (color != g_color) {
        g_color = color;
        static const char* names[] = {"off", "gold", "cyan"};
        char msg[64];
        std::snprintf(msg, sizeof(msg), "Master Sword glow: %s", names[color]);
        svc_log->info(mod_ctx, msg);
    }
}

// ---------------------------------------------------------------------------------------------
// Settings panel (fine tuning; colour and on/off live in Hero of Legend)

ModResult register_percent(const char* name, ConfigVarHandle& out) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_INT;
    desc.default_int = 100;
    return svc_config->register_var(mod_ctx, &desc, &out);
}

void add_percent_control(UiElementHandle panel, const char* label, const char* help, ConfigVarHandle var,
    int64_t maxValue) {
    UiControlDesc c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_NUMBER;
    c.label = label;
    c.help_rml = help;
    c.binding = UI_BINDING_CONFIG_VAR;
    c.config_var = var;
    c.min = 0;
    c.max = maxValue;
    c.step = 10;
    c.suffix = "%";
    svc_ui->pane_add_control(mod_ctx, panel, &c, nullptr);
}

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    svc_ui->pane_add_text(mod_ctx, panel,
        "Turn the glow on and pick Gold or Cyan in Hero of Legend > Blade glow.", nullptr);
    add_percent_control(panel, "Halo size", "<p>Width of the soft halo around the blade.</p>",
        g_cvHaloSize, 300);
    add_percent_control(panel, "Halo brightness", "<p>Strength of the soft halo.</p>",
        g_cvHaloBright, 300);
    add_percent_control(panel, "Blade brightness", "<p>Strength of the bright core over the blade.</p>",
        g_cvCoreBright, 300);
    add_percent_control(panel, "Light strength", "<p>How much light the sword casts on the world.</p>",
        g_cvLight, 300);
    return MOD_OK;
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    build_texture();

    if (register_percent("halo_size", g_cvHaloSize) != MOD_OK ||
        register_percent("halo_brightness", g_cvHaloBright) != MOD_OK ||
        register_percent("blade_brightness", g_cvCoreBright) != MOD_OK ||
        register_percent("light_strength", g_cvLight) != MOD_OK)
    {
        return mods::set_error(error, MOD_ERROR, "failed to register settings");
    }

    if (mods::hook::add_post<LinkSolEffect>(on_sol_effect_post) != MOD_OK ||
        mods::hook::add_post<LinkSetLight>(on_set_light_post) != MOD_OK ||
        mods::hook::add_post<LinkModelDraw>(on_model_draw_post) != MOD_OK)
    {
        return mods::set_error(error, MOD_ERROR, "failed to hook Link");
    }

    UiModsPanelDesc panelDesc = UI_MODS_PANEL_DESC_INIT;
    panelDesc.build = build_panel;
    svc_ui->register_mods_panel(mod_ctx, &panelDesc);

    poll_marker();
    svc_log->info(mod_ctx, "Master Sword Glow ready");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    if (++g_pollCounter >= 15) {
        g_pollCounter = 0;
        poll_marker();
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    g_color = GLOW_OFF;
    g_fade = 0.0f;
    if (g_texReady) {
#ifdef TARGET_PC
        g_texObj.reset();
#endif
        g_texReady = false;
    }
    return MOD_OK;
}
}
