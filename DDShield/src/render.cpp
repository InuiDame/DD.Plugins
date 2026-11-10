#include "render.h"
#include "config.h"
#include "loc.h"
#include "shield.h"
#include "game.h"

// OpenGL entry points are declared locally: the build does not rely on GL
// headers being installed.
typedef unsigned int  GLenum;
typedef unsigned int  GLuint;
typedef int           GLint;
typedef int           GLsizei;
typedef float         GLfloat;
typedef char          GLchar;
typedef ptrdiff_t     GLsizeiptr;

#define GL_FALSE 0
#define GL_TRIANGLES 0x0004
#define GL_BLEND 0x0BE2
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_SCISSOR_TEST 0x0C11
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_ARRAY_BUFFER 0x8892
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_FRAMEBUFFER 0x8D40
#define GL_VIEWPORT 0x0BA2
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_UNPACK_ALIGNMENT 0x0CF5

namespace dds {
namespace render {

namespace {

typedef void (*PFNGLGETINTEGERV)(GLenum, GLint*);
typedef void (*PFNGLVIEWPORT)(GLint, GLint, GLsizei, GLsizei);
typedef void (*PFNGLDISABLE)(GLenum);
typedef void (*PFNGLENABLE)(GLenum);
typedef void (*PFNGLBLENDFUNC)(GLenum, GLenum);
typedef void (*PFNGLUSEPROGRAM)(GLuint);
typedef void (*PFNGLGENVERTEXARRAYS)(GLsizei, GLuint*);
typedef void (*PFNGLBINDVERTEXARRAY)(GLuint);
typedef void (*PFNGLGENBUFFERS)(GLsizei, GLuint*);
typedef void (*PFNGLBINDBUFFER)(GLenum, GLuint);
typedef void (*PFNGLBUFFERDATA)(GLenum, GLsizeiptr, const void*, GLenum);
typedef void (*PFNGLVERTEXATTRIBPOINTER)(GLuint, GLint, GLenum, unsigned char, GLsizei, const void*);
typedef void (*PFNGLENABLEVERTEXATTRIBARRAY)(GLuint);
typedef void (*PFNGLDRAWARRAYS)(GLenum, GLint, GLsizei);
typedef GLuint (*PFNGLCREATESHADER)(GLenum);
typedef void (*PFNGLSHADERSOURCE)(GLuint, GLsizei, const GLchar* const*, const GLint*);
typedef void (*PFNGLCOMPILESHADER)(GLuint);
typedef void (*PFNGLGETSHADERIV)(GLuint, GLenum, GLint*);
typedef void (*PFNGLGETSHADERINFOLOG)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef GLuint (*PFNGLCREATEPROGRAM)();
typedef void (*PFNGLATTACHSHADER)(GLuint, GLuint);
typedef void (*PFNGLLINKPROGRAM)(GLuint);
typedef void (*PFNGLGETPROGRAMIV)(GLuint, GLenum, GLint*);
typedef void (*PFNGLDELETESHADER)(GLuint);
typedef void (*PFNGLBINDFRAMEBUFFER)(GLenum, GLuint);
typedef void (*PFNGLGENBUFFERSPROC)(GLsizei, GLuint*);
typedef void (*PFNGLGENTEXTURES)(GLsizei, GLuint*);
typedef void (*PFNGLBINDTEXTURE)(GLenum, GLuint);
typedef void (*PFNGLTEXIMAGE2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
typedef void (*PFNGLTEXPARAMETERI)(GLenum, GLenum, GLint);
typedef void (*PFNGLACTIVETEXTURE)(GLenum);
typedef void (*PFNGLUNIFORM1I)(GLint, GLint);
typedef void (*PFNGLPIXELSTOREI)(GLenum, GLint);
typedef GLint (*PFNGLGETUNIFORMLOCATION)(GLuint, const GLchar*);

struct GL {
    PFNGLGETINTEGERV GetIntegerv = nullptr;
    PFNGLVIEWPORT Viewport = nullptr;
    PFNGLDISABLE Disable = nullptr;
    PFNGLENABLE Enable = nullptr;
    PFNGLBLENDFUNC BlendFunc = nullptr;
    PFNGLUSEPROGRAM UseProgram = nullptr;
    PFNGLGENVERTEXARRAYS GenVertexArrays = nullptr;
    PFNGLBINDVERTEXARRAY BindVertexArray = nullptr;
    PFNGLGENBUFFERS GenBuffers = nullptr;
    PFNGLBINDBUFFER BindBuffer = nullptr;
    PFNGLBUFFERDATA BufferData = nullptr;
    PFNGLVERTEXATTRIBPOINTER VertexAttribPointer = nullptr;
    PFNGLENABLEVERTEXATTRIBARRAY EnableVertexAttribArray = nullptr;
    PFNGLDRAWARRAYS DrawArrays = nullptr;
    PFNGLCREATESHADER CreateShader = nullptr;
    PFNGLSHADERSOURCE ShaderSource = nullptr;
    PFNGLCOMPILESHADER CompileShader = nullptr;
    PFNGLGETSHADERIV GetShaderiv = nullptr;
    PFNGLGETSHADERINFOLOG GetShaderInfoLog = nullptr;
    PFNGLCREATEPROGRAM CreateProgram = nullptr;
    PFNGLATTACHSHADER AttachShader = nullptr;
    PFNGLLINKPROGRAM LinkProgram = nullptr;
    PFNGLGETPROGRAMIV GetProgramiv = nullptr;
    PFNGLDELETESHADER DeleteShader = nullptr;
    PFNGLBINDFRAMEBUFFER BindFramebuffer = nullptr;
    PFNGLGENTEXTURES GenTextures = nullptr;
    PFNGLBINDTEXTURE BindTexture = nullptr;
    PFNGLTEXIMAGE2D TexImage2D = nullptr;
    PFNGLTEXPARAMETERI TexParameteri = nullptr;
    PFNGLACTIVETEXTURE ActiveTexture = nullptr;
    PFNGLUNIFORM1I Uniform1i = nullptr;
    PFNGLPIXELSTOREI PixelStorei = nullptr;
    PFNGLGETUNIFORMLOCATION GetUniformLocation = nullptr;
} gl;

struct Vertex { float x, y, u, v, r, g, b, a; };

struct TextTex { GLuint id = 0; int w = 0; int h = 0; };

bool  g_ready = false;
bool  g_failed = false;
GLuint g_program = 0, g_vao = 0, g_vbo = 0, g_white = 0;
GLint g_locTex = -1;
std::unordered_map<std::wstring, TextTex> g_textCache;
std::wstring g_configPath;

const char* kVert =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n"
    "layout(location=1) in vec2 aUV;\n"
    "layout(location=2) in vec4 aColor;\n"
    "out vec2 vUV; out vec4 vColor;\n"
    "void main(){ vUV=aUV; vColor=aColor; gl_Position=vec4(aPos,0.0,1.0); }\n";

const char* kFrag =
    "#version 330 core\n"
    "uniform sampler2D uTex;\n"
    "in vec2 vUV; in vec4 vColor; out vec4 oColor;\n"
    "void main(){ oColor = texture(uTex, vUV) * vColor; }\n";

template <typename T>
bool Load(T& fn, const char* name, HMODULE mod, void* (WINAPI* getProc)(const char*)) {
    fn = (T)GetProcAddress(mod, name);
    if (!fn && getProc) fn = (T)getProc(name);
    if (!fn) WARN("render: missing GL entry '{}'", name);
    return fn != nullptr;
}

bool CompileShader(GLenum type, const char* src, GLuint& out) {
    out = gl.CreateShader(type);
    gl.ShaderSource(out, 1, &src, nullptr);
    gl.CompileShader(out);
    GLint ok = 0;
    gl.GetShaderiv(out, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        gl.GetShaderInfoLog(out, sizeof(log) - 1, nullptr, log);
        WARN("render: shader compile failed: {}", log);
        return false;
    }
    return true;
}

// ---- GDI text texture -----------------------------------------------------
TextTex* GetTextTexture(const std::wstring& text, int heightPx) {
    auto key = text;
    auto it = g_textCache.find(key);
    if (it != g_textCache.end()) return &it->second;

    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return nullptr;
    HFONT font = CreateFontW(-heightPx, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    HGDIOBJ oldFont = SelectObject(dc, font);

    SIZE size = { 0, 0 };
    GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &size);
    int w = size.cx + 4;
    int h = size.cy + 4;
    if (w < 8) w = 8;
    if (h < 8) h = 8;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    memset(bits, 0, (size_t)w * h * 4);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    TextOutW(dc, 2, 2, text.c_str(), (int)text.size());

    // BGRA -> RGBA with alpha = luminance.
    auto* px = (uint8_t*)bits;
    for (int i = 0; i < w * h; ++i) {
        uint8_t b = px[i * 4 + 0], g = px[i * 4 + 1], r = px[i * 4 + 2];
        uint8_t lum = (uint8_t)std::max({ b, g, r });
        px[i * 4 + 0] = 255;
        px[i * 4 + 1] = 255;
        px[i * 4 + 2] = 255;
        px[i * 4 + 3] = lum;
    }

    TextTex tex;
    tex.w = w;
    tex.h = h;
    gl.GenTextures(1, &tex.id);
    gl.BindTexture(GL_TEXTURE_2D, tex.id);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, bits);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.BindTexture(GL_TEXTURE_2D, 0);

    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    SelectObject(dc, oldFont);
    DeleteObject(font);
    DeleteDC(dc);

    auto res = g_textCache.emplace(key, tex);
    return &res.first->second;
}

void DrawBatch(GLuint tex, const std::vector<Vertex>& verts) {
    if (verts.empty()) return;
    gl.BindBuffer(GL_ARRAY_BUFFER, g_vbo);
    gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts.size() * sizeof(Vertex)), verts.data(), GL_DYNAMIC_DRAW);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, 0, sizeof(Vertex), (const void*)0);
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(1, 2, GL_FLOAT, 0, sizeof(Vertex), (const void*)(2 * sizeof(float)));
    gl.EnableVertexAttribArray(2);
    gl.VertexAttribPointer(2, 4, GL_FLOAT, 0, sizeof(Vertex), (const void*)(4 * sizeof(float)));
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, tex);
    gl.DrawArrays(GL_TRIANGLES, 0, (GLsizei)verts.size());
}

struct Box { float x, y, w, h; };

void AddQuad(std::vector<Vertex>& v, float fbW, float fbH,
             float x, float y, float w, float h, float r, float g, float b, float a) {
    float x0 = 2.0f * x / fbW - 1.0f;
    float x1 = 2.0f * (x + w) / fbW - 1.0f;
    float y0 = 1.0f - 2.0f * y / fbH;
    float y1 = 1.0f - 2.0f * (y + h) / fbH;
    v.push_back({ x0, y0, 0, 0, r, g, b, a });
    v.push_back({ x1, y0, 0, 0, r, g, b, a });
    v.push_back({ x1, y1, 0, 0, r, g, b, a });
    v.push_back({ x0, y0, 0, 0, r, g, b, a });
    v.push_back({ x1, y1, 0, 0, r, g, b, a });
    v.push_back({ x0, y1, 0, 0, r, g, b, a });
}

void AddTextQuad(std::vector<Vertex>& v, float fbW, float fbH,
                 float x, float y, float w, float h, float r, float g, float b, float a) {
    float x0 = 2.0f * x / fbW - 1.0f;
    float x1 = 2.0f * (x + w) / fbW - 1.0f;
    float y0 = 1.0f - 2.0f * y / fbH;
    float y1 = 1.0f - 2.0f * (y + h) / fbH;
    v.push_back({ x0, y0, 0, 0, r, g, b, a });
    v.push_back({ x1, y0, 1, 0, r, g, b, a });
    v.push_back({ x1, y1, 1, 1, r, g, b, a });
    v.push_back({ x0, y0, 0, 0, r, g, b, a });
    v.push_back({ x1, y1, 1, 1, r, g, b, a });
    v.push_back({ x0, y1, 0, 1, r, g, b, a });
}

} // namespace

bool Init() {
    if (g_ready) return true;
    if (g_failed) return false;

    HMODULE mod = LoadLibraryA("opengl32.dll");
    if (!mod) { g_failed = true; return false; }
    auto getProc = (void* (WINAPI*)(const char*))GetProcAddress(mod, "wglGetProcAddress");

    bool ok = true;
    ok &= Load(gl.GetIntegerv, "glGetIntegerv", mod, getProc);
    ok &= Load(gl.Viewport, "glViewport", mod, getProc);
    ok &= Load(gl.Disable, "glDisable", mod, getProc);
    ok &= Load(gl.Enable, "glEnable", mod, getProc);
    ok &= Load(gl.BlendFunc, "glBlendFunc", mod, getProc);
    ok &= Load(gl.UseProgram, "glUseProgram", mod, getProc);
    ok &= Load(gl.GenVertexArrays, "glGenVertexArrays", mod, getProc);
    ok &= Load(gl.BindVertexArray, "glBindVertexArray", mod, getProc);
    ok &= Load(gl.GenBuffers, "glGenBuffers", mod, getProc);
    ok &= Load(gl.BindBuffer, "glBindBuffer", mod, getProc);
    ok &= Load(gl.BufferData, "glBufferData", mod, getProc);
    ok &= Load(gl.VertexAttribPointer, "glVertexAttribPointer", mod, getProc);
    ok &= Load(gl.EnableVertexAttribArray, "glEnableVertexAttribArray", mod, getProc);
    ok &= Load(gl.DrawArrays, "glDrawArrays", mod, getProc);
    ok &= Load(gl.CreateShader, "glCreateShader", mod, getProc);
    ok &= Load(gl.ShaderSource, "glShaderSource", mod, getProc);
    ok &= Load(gl.CompileShader, "glCompileShader", mod, getProc);
    ok &= Load(gl.GetShaderiv, "glGetShaderiv", mod, getProc);
    ok &= Load(gl.GetShaderInfoLog, "glGetShaderInfoLog", mod, getProc);
    ok &= Load(gl.CreateProgram, "glCreateProgram", mod, getProc);
    ok &= Load(gl.AttachShader, "glAttachShader", mod, getProc);
    ok &= Load(gl.LinkProgram, "glLinkProgram", mod, getProc);
    ok &= Load(gl.GetProgramiv, "glGetProgramiv", mod, getProc);
    ok &= Load(gl.DeleteShader, "glDeleteShader", mod, getProc);
    ok &= Load(gl.BindFramebuffer, "glBindFramebuffer", mod, getProc);
    ok &= Load(gl.GenTextures, "glGenTextures", mod, getProc);
    ok &= Load(gl.BindTexture, "glBindTexture", mod, getProc);
    ok &= Load(gl.TexImage2D, "glTexImage2D", mod, getProc);
    ok &= Load(gl.TexParameteri, "glTexParameteri", mod, getProc);
    ok &= Load(gl.ActiveTexture, "glActiveTexture", mod, getProc);
    ok &= Load(gl.Uniform1i, "glUniform1i", mod, getProc);
    ok &= Load(gl.PixelStorei, "glPixelStorei", mod, getProc);
    ok &= Load(gl.GetUniformLocation, "glGetUniformLocation", mod, getProc);
    if (!ok) { g_failed = true; return false; }

    GLuint vs = 0, fs = 0;
    if (!CompileShader(GL_VERTEX_SHADER, kVert, vs)) { g_failed = true; return false; }
    if (!CompileShader(GL_FRAGMENT_SHADER, kFrag, fs)) { g_failed = true; return false; }
    g_program = gl.CreateProgram();
    gl.AttachShader(g_program, vs);
    gl.AttachShader(g_program, fs);
    gl.LinkProgram(g_program);
    GLint linked = 0;
    gl.GetProgramiv(g_program, GL_LINK_STATUS, &linked);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    if (!linked) { g_failed = true; return false; }
    g_locTex = gl.GetUniformLocation(g_program, "uTex");

    gl.GenVertexArrays(1, &g_vao);
    gl.GenBuffers(1, &g_vbo);
    gl.GenTextures(1, &g_white);
    uint8_t white[4] = { 255, 255, 255, 255 };
    gl.BindTexture(GL_TEXTURE_2D, g_white);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.BindTexture(GL_TEXTURE_2D, 0);

    g_ready = true;
    INFO("render: overlay ready");
    return true;
}

bool Ready() { return g_ready; }

void SetConfigPath(const std::wstring& path) { g_configPath = path; }

static int (__cdecl* g_showCursor)(int) = nullptr;

void OnSwap() {
    if (!g_cfg.enabled || !g_cfg.barShow) return;
    if (!g_ready && !Init()) return;
    if (g_menuOpen) return;   // the pause menu must cover the bars
    if (g_cfg.hideInPanels && g_panelOpen) return;   // hero sheet / other UI panels
    if (g_cfg.hideActing && ShieldActingNow()) return;   // some actor is mid-animation

    if (!g_showCursor) {
        HMODULE sdl = GetModuleHandleW(L"SDL2.dll");
        if (sdl) g_showCursor = (int (__cdecl*)(int))GetProcAddress(sdl, "SDL_ShowCursor");
    }
    if (g_showCursor) {
        int shown = g_showCursor(-1);   // SDL_QUERY
        static int lastShown = -999;
        if (shown != lastShown) {
            lastShown = shown;
            INFO("cursor shown state: {}", shown);
        }
        if (g_cfg.hideWhenCursor && shown == 1) return;
    }

    GLint vp[4] = { 0, 0, 1, 1 };
    gl.GetIntegerv(GL_VIEWPORT, vp);
    float fbW = (float)vp[2];
    float fbH = (float)vp[3];
    if (fbW <= 0 || fbH <= 0) return;

    // Darkest Dungeon lays its UI out in uiWidth x uiHeight and scales it into
    // the framebuffer.  16:9 -> uniform scale; other aspect ratios are either
    // letterboxed (default) or stretched (barStretch = 1).
    float uiW = g_cfg.uiWidth > 0 ? g_cfg.uiWidth : 1920.0f;
    float uiH = g_cfg.uiHeight > 0 ? g_cfg.uiHeight : 1080.0f;
    float sx = 1.0f, sy = 1.0f, ox = 0.0f, oy = 0.0f;
    if (g_cfg.autoScale) {
        if (g_cfg.barStretch) {
            sx = fbW / uiW;
            sy = fbH / uiH;
        } else {
            float u = std::min(fbW / uiW, fbH / uiH);
            sx = sy = u;
            ox = (fbW - uiW * u) * 0.5f;
            oy = (fbH - uiH * u) * 0.5f;
        }
    }
    sx *= g_cfg.barScaleMul;
    sy *= g_cfg.barScaleMul;

    {
        static DWORD lastLog = 0;
        if (g_cfg.debugLog && GetTickCount() - lastLog > 5000) {
            lastLog = GetTickCount();
            DEBUG("framebuffer {:.0f}x{:.0f} uiScale=({:.4f},{:.4f}) offset=({:.1f},{:.1f})",
                fbW, fbH, sx, sy, ox, oy);
        }
    }
    ox *= g_cfg.barScaleMul;
    oy *= g_cfg.barScaleMul;

    // status_bars layout: +0x20 = health_bar_offset.x (usually 50), +0x24 = .y
    float layoutOffX = 0.0f, layoutOffY = 0.0f;
    if (g_cfg.barUseLayoutOffset) {
        const float* layout = (const float*)(ModuleBase() + 0x10AA768);
        layoutOffX = layout[8];
        layoutOffY = layout[9];
    }

    // Mouse position in framebuffer pixels.
    POINT mp = { 0, 0 };
    GetCursorPos(&mp);
    HWND hwnd = WindowFromPoint(mp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    float mx = -1, my = -1;
    if (pid == GetCurrentProcessId()) {
        ScreenToClient(hwnd, &mp);
        RECT rc = {};
        GetClientRect(hwnd, &rc);
        float cw = (float)(rc.right - rc.left);
        float ch = (float)(rc.bottom - rc.top);
        if (cw > 0 && ch > 0) {
            mx = mp.x * fbW / cw;
            my = mp.y * fbH / ch;
        }
    }

    std::vector<Vertex> solids;
    Box hoverBox = { 0, 0, 0, 0 };
    ShieldState* hovered = nullptr;

    DWORD nowMs = GetTickCount();
    for (auto& kv : Shields()) {
        ShieldState& s = kv.second;
        if (!s.geo.valid || s.max <= 0.0f) continue;
        // The game stops calling the status-bar routine while bars are hidden
        // (map transition, fade-out, death).  Mirror that: only draw actors
        // whose bar was updated in the last few frames.
        if (s.lastSeen == 0 || nowMs - s.lastSeen > (DWORD)g_cfg.barHoldMs) continue;
        if (s.cur <= 0.0f && !g_cfg.barAlwaysShow) continue;   // hide when empty

        float w = s.geo.width * g_cfg.barWidthScale * sx;
        if (w <= 1.0f) continue;
        float h = g_cfg.barHeight * sy;
        float left = ox + (s.geo.x + layoutOffX) * sx + g_cfg.barXOffset;
        float top = oy + (s.geo.y + layoutOffY - g_cfg.barGap - g_cfg.barHeight) * sy + g_cfg.barYOffset;

        if (g_cfg.barBorder)
            AddQuad(solids, fbW, fbH, left - 1, top - 1, w + 2, h + 2,
                    g_cfg.borderR, g_cfg.borderG, g_cfg.borderB, g_cfg.borderA);
        AddQuad(solids, fbW, fbH, left, top, w, h,
                g_cfg.backR, g_cfg.backG, g_cfg.backB, g_cfg.backA);
        float ratio = Clamp(s.cur / s.max, 0.0f, 1.0f);
        if (ratio > 0.0f)
            AddQuad(solids, fbW, fbH, left, top, w * ratio, h,
                    g_cfg.fillR, g_cfg.fillG, g_cfg.fillB, g_cfg.fillA);

        if (g_cfg.tooltip && mx >= left - 2 && mx <= left + w + 2 &&
            my >= top - 4 && my <= top + h + 4) {
            hoverBox = { left, top, w, h };
            hovered = &s;
        }
    }

    std::vector<Vertex> textDraw;
    TextTex* textTex = nullptr;
    float textX = 0, textY = 0, textW = 0, textH = 0;

    if (hovered) {
        wchar_t buf[128];
        std::wstring name = loc::Get("str_shield_name", L"SHIELD");
        swprintf_s(buf, L"%s  %.0f / %.0f", name.c_str(), hovered->cur, hovered->max);
        TextTex* t = GetTextTexture(buf, (int)std::max(12.0f, 16.0f * sy));
        if (t) {
            textTex = t;
            float pad = 4.0f;
            float bx = hoverBox.x;
            float by = hoverBox.y - t->h - 2 * pad - 2;
            if (by < 0) by = hoverBox.y + hoverBox.h + 2;
            float bw = (float)t->w + 2 * pad;
            float bh = (float)t->h + 2 * pad;
            AddQuad(solids, fbW, fbH, bx, by, bw, bh, 0.02f, 0.03f, 0.05f, 0.92f);
            AddQuad(solids, fbW, fbH, bx, by, bw, 1.0f, g_cfg.borderR, g_cfg.borderG, g_cfg.borderB, 0.9f);
            AddQuad(solids, fbW, fbH, bx, by + bh - 1.0f, bw, 1.0f, g_cfg.borderR, g_cfg.borderG, g_cfg.borderB, 0.9f);
            textX = bx + pad;
            textY = by + pad;
            textW = (float)t->w;
            textH = (float)t->h;
        }
    }

    if (solids.empty() && !textTex) return;

    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_CULL_FACE);
    gl.Disable(GL_SCISSOR_TEST);
    gl.Enable(GL_BLEND);
    gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl.UseProgram(g_program);
    gl.Uniform1i(g_locTex, 0);
    gl.BindVertexArray(g_vao);

    DrawBatch(g_white, solids);
    if (textTex) {
        textDraw.clear();
        AddTextQuad(textDraw, fbW, fbH, textX, textY, textW, textH, 1, 1, 1, 1);
        DrawBatch(textTex->id, textDraw);
    }

    gl.BindVertexArray(0);
    gl.UseProgram(0);
}

} // namespace render
} // namespace dds




