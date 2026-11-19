#ifndef LOCAL_CROP_DIALOG_H_
#define LOCAL_CROP_DIALOG_H_

/* An independent window to adjust the part of the pages that is shown.
   Briss-style: preview the page, run the pixel based margin detection, then
   drag the edges until the crop is right. Applying it sets the same fractional
   crop on every page (DisplayModel::SetUserCrop), i.e. it changes the view, not
   the PDF file.

   Included once, near the end of the include block of SumatraPDF.cpp: the
   upstream headers it needs (base/Pixmap.h, DisplayModel.h, EngineBase.h,
   MainWindow.h) have no include guards, so it must not include them itself. */

#include "BrissCrop.h"

// Defined in DisplayModel.cpp. Applies per-page crop rectangles given as
// fractions of each page (a null rect in the array means "whole page").
void DisplayModelApplyPageCrops(DisplayModel* dm, const RectF* fractions, int count);
#include "PageCacheTrim.h"

namespace cropdlg {

// A command id that no entry in Commands.h uses; wired up in MenuDefs.cpp and
// in the command switch in SumatraPDF.cpp.
constexpr int kCmdId = 9001;

constexpr int kMinCropPt = 20;   // smallest crop side, in page points
constexpr int kHitSlopPx = 8;    // how close to an edge counts as grabbing it

// Whole-document analysis, the way Briss does it: render every page small,
// measure where its content is, group the pages by size and crop each group to
// the box that still contains the content of *all* its pages.
// Analysis detail: about 2.3 pt per pixel for a normal page, which is what
// kAnalyzePx used to be for A4. A page can be far bigger than A4 (a scan placed
// 1:1 at its pixel size gives a 3700 x 3100 pt canvas): at a fixed 360 px such a
// page would be downsampled until its text vanished and the crop would either do
// nothing or cut away a whole column, so the target grows with the page and is
// capped to keep the per-page cost (and the memory of one render) bounded.
constexpr float kAnalyzePtPerPx = 2.3f;
constexpr int kAnalyzePxMin = 300;
// A 3700 pt canvas is a scan placed 1:1 at its pixel size: at 1000 px one pixel
// is 3.7 pt, still plenty to see lines of text, and MuPDF then decodes the JPEG
// at a reduced scale, which is where most of the per-page time went.
constexpr int kAnalyzePxMax = 1000;
constexpr int kAnalyzeSliceMs = 40;  // work per timer tick, keeps the UI alive
// Briss greys out the part of the page a preview crop would drop but leaves it
// readable, so what is about to be cut can be checked before it goes. This is
// the share of the original that stays under the white wash.
constexpr int kDroppedInkPct = 55;
constexpr int kTimerAnalyze = 1;

enum : int {
    kDragNone = 0,
    kDragLeft = 1,
    kDragRight = 2,
    kDragTop = 4,
    kDragBottom = 8,
    kDragMove = 16,
};

enum : int {
    kBtnAnalyze = 1,
    kBtnDetect,
    kBtnFullPage,
    kBtnPrevPage,
    kBtnNextPage,
    kBtnApply,
    kBtnTarget,
    kBtnClose,
};

struct Button {
    int id;
    Rect rc;
    const WCHAR* label;
};

// One group of same-sized pages: the rectangle that contains the content of
// every page in the group, as fractions of a page.
struct BrissGroup {
    SizeF size;          // page size in points, identifies the group
    RectF frac;          // plain union of the content boxes, for reference
    float frameW = 0;    // frame every page of the group is cropped to, in points
    float frameH = 0;
    int count = 0;       // pages in the group
};

struct DlgState {
    HWND hwnd = nullptr;
    MainWindow* win = nullptr;
    DisplayModel* dm = nullptr;
    int pageNo = 0;
    RectF mediaBox;   // page coordinates of the page being previewed
    RectF crop;       // page coordinates of the crop rectangle
    Rect previewRect; // client coordinates of the preview area
    Pixmap* pix = nullptr;
    Pixmap* pixOwned = nullptr;
    int drag = 0;
    PointF dragOrigin;
    RectF dragStartCrop;
    int hoverBtn = 0;
    BrissGroup* groups = nullptr;  // one per page size found by the analysis
    int groupCount = 0;
    int groupCap = 0;
    int* pageGroup = nullptr;  // pageNo (1-based) -> group index, -1 if none
    int pageGroupCount = 0;
    RectF* pageContent = nullptr;  // pageNo -> content box found, page coordinates
    RectF* pageCrop = nullptr;     // pageNo -> crop this page ends up with
    bool analyzing = false;
    int analyzePage = 1;
    int analyzeTotal = 0;
    int analyzeFailed = 0;
    ULONGLONG analyzeStartedAt = 0;
    HFONT font = nullptr;
    bool fontOwned = false;
    int dpi = 96;
    // What to do with the space the detection finds: the default fits the
    // content, which on a wide page mostly means trimming left and right. On a
    // wide screen the height is what costs scrolling, so this can keep the
    // width (and with it the printed layout) and only take the blank bands off
    // the top and bottom.
    bool trimHeightOnly = false;
};

static DlgState* gDlg = nullptr;

static void FreePagePixmap(DlgState* d) {
    if (d->pixOwned) {
        FreePixmap(d->pixOwned);
        d->pixOwned = nullptr;
    }
    d->pix = nullptr;
}

// Renders pageNo at a resolution good enough for a preview and keeps it.
static void EnsurePagePixmap(DlgState* d) {
    if (d->pix || !d->dm) {
        return;
    }
    EngineBase* engine = d->dm->GetEngine();
    if (!engine) {
        return;
    }
    float boxDx = d->mediaBox.dx > 1 ? d->mediaBox.dx : 1;
    int wantPx = d->previewRect.dx > 0 ? d->previewRect.dx : 600;
    if (wantPx > 1400) {
        wantPx = 1400;
    }
    float zoom = (float)wantPx / boxDx;
    zoom = limitValue(zoom, 0.2f, 4.0f);
    RectF box = d->mediaBox;
    RenderPageArgs args(d->pageNo, zoom, 0, &box);
    Pixmap* pix = engine->RenderPage(args);
    if (!pix) {
        return;
    }
    // takes ownership, returns a blittable BGRA8 pixmap (or null)
    Pixmap* use = PixmapToBgra(pix);
    if (!use || !use->data || use->width < 1 || use->height < 1) {
        FreePixmap(use);
        return;
    }
    d->pix = use;
    d->pixOwned = use;
}

static bool IsUsable(MainWindow* win) {
    if (!win || !win->IsDocLoaded()) {
        return false;
    }
    DisplayModel* dm = win->AsFixed();
    if (!dm || dm->PageCount() < 1) {
        return false;
    }
    return true;
}

// Loads the page being previewed. keepCrop keeps the rectangle being adjusted so
// the user can check it on the next page instead of starting over; otherwise it
// comes from the crop that is already in effect.
static void LoadPage(DlgState* d, int pageNo, bool keepCrop = false) {
    DisplayModel* dm = d->dm;
    int n = dm->PageCount();
    pageNo = limitValue(pageNo, 1, n);
    FreePagePixmap(d);
    d->pageNo = pageNo;
    d->mediaBox = dm->PageMediaBox(pageNo);
    if (keepCrop) {
        RectF c = d->crop;
        float dx = c.dx < d->mediaBox.dx ? c.dx : d->mediaBox.dx;
        float dy = c.dy < d->mediaBox.dy ? c.dy : d->mediaBox.dy;
        float x = limitValue(c.x, d->mediaBox.x, d->mediaBox.Right() - dx);
        float y = limitValue(c.y, d->mediaBox.y, d->mediaBox.Bottom() - dy);
        d->crop = RectF(x, y, dx, dy);
        return;
    }
    RectF frac = dm->GetUserCrop();
    if (frac.IsEmpty()) {
        d->crop = d->mediaBox;
    } else {
        d->crop = RectF(d->mediaBox.x + frac.x * d->mediaBox.dx, d->mediaBox.y + frac.y * d->mediaBox.dy,
                        frac.dx * d->mediaBox.dx, frac.dy * d->mediaBox.dy);
    }
}

static Rect FitPageInPreview(const RectF& box, const Rect& area) {
    if (box.dx <= 0 || box.dy <= 0 || area.dx <= 0 || area.dy <= 0) {
        return area;
    }
    float sx = (float)area.dx / box.dx;
    float sy = (float)area.dy / box.dy;
    float s = sx < sy ? sx : sy;
    int dx = (int)(box.dx * s);
    int dy = (int)(box.dy * s);
    if (dx < 1) {
        dx = 1;
    }
    if (dy < 1) {
        dy = 1;
    }
    int x = area.x + (area.dx - dx) / 2;
    int y = area.y + (area.dy - dy) / 2;
    return Rect(x, y, dx, dy);
}

// Briss-style preview: the page as it is, then the part outside the crop laid
// under a white wash so it is still readable and obviously "about to go", then
// the kept rectangle outlined. The wash is mixed into a copy of the page rather
// than painted over it, because a translucent brush would show the preview
// background through it the way Briss' white does not.
static void PaintBrissPreview(Pixmap* src, HDC hdc, const Rect& dest, const Rect& cropRc) {
    if (!src || !src->data || dest.IsEmpty() || src->width < 1 || src->height < 1) {
        return;
    }
    if (cropRc.x <= dest.x && cropRc.y <= dest.y && cropRc.Right() >= dest.Right() &&
        cropRc.Bottom() >= dest.Bottom()) {
        BlitPixmap(src, hdc, dest);  // nothing is being cut: hand it through as before
        return;
    }
    // the preview pixmap is BGRA8 heap pixels; copy it so the wash can be mixed
    // in without touching the cached render
    int w = src->width;
    int h = src->height;
    Pixmap* copy = AllocPixmap(w, h, PixmapFormat::BGRA8);
    if (!copy || !copy->data) {
        if (copy) {
            FreePixmap(copy);
        }
        BlitPixmap(src, hdc, dest);
        return;
    }
    int srcStride = src->stride;
    int stride = copy->stride;
    for (int y = 0; y < h; y++) {
        memcpy(copy->data + (size_t)y * (size_t)stride, src->data + (size_t)y * (size_t)srcStride,
               (size_t)w * 4);
    }
    for (int y = 0; y < h; y++) {
        // row span of the kept rectangle in this pixmap's own pixels
        int sx0 = (int)((float)(cropRc.x - dest.x) * w / dest.dx);
        int sx1 = (int)((float)(cropRc.Right() - dest.x) * w / dest.dx);
        if (sx0 < 0) sx0 = 0;
        if (sx1 > w) sx1 = w;
        bool rowOutside = y < (int)((float)(cropRc.y - dest.y) * h / dest.dy) ||
                          y >= (int)((float)(cropRc.Bottom() - dest.y) * h / dest.dy);
        u8* row = copy->data + (size_t)y * (size_t)stride;
        for (int x = 0; x < w; x++) {
            if (!rowOutside && x >= sx0 && x < sx1) {
                continue;  // inside the crop: keep it exactly as it is
            }
            u8* px = row + (size_t)x * 4;
            for (int k = 0; k < 3; k++) {
                px[k] = (u8)(px[k] + (255 - px[k]) * (100 - kDroppedInkPct) / 100);
            }
        }
    }
    BlitPixmap(copy, hdc, dest);
    FreePixmap(copy);
}

static Point PageToClient(const DlgState* d, const Rect& dest, float px, float py) {
    float fx = d->mediaBox.dx > 0 ? (px - d->mediaBox.x) / d->mediaBox.dx : 0.f;
    float fy = d->mediaBox.dy > 0 ? (py - d->mediaBox.y) / d->mediaBox.dy : 0.f;
    return Point(dest.x + (int)(fx * dest.dx), dest.y + (int)(fy * dest.dy));
}

static void ClientToPage(const DlgState* d, const Rect& dest, Point pt, float* px, float* py) {
    float fx = dest.dx > 0 ? (float)(pt.x - dest.x) / (float)dest.dx : 0.f;
    float fy = dest.dy > 0 ? (float)(pt.y - dest.y) / (float)dest.dy : 0.f;
    fx = limitValue(fx, 0.f, 1.f);
    fy = limitValue(fy, 0.f, 1.f);
    *px = d->mediaBox.x + fx * d->mediaBox.dx;
    *py = d->mediaBox.y + fy * d->mediaBox.dy;
}

static void Layout(DlgState* d, int clientDx, int clientDy) {
    int rightW = DpiScale(210);
    int margin = DpiScale(14);
    int top = DpiScale(38);
    d->previewRect = Rect(margin, top, clientDx - rightW - 2 * margin, clientDy - top - DpiScale(78));
    if (d->previewRect.dx < 50) {
        d->previewRect.dx = 50;
    }
    if (d->previewRect.dy < 50) {
        d->previewRect.dy = 50;
    }
}

// Positions of the buttons; index 0..kBtnClose-kBtnDetect.
static int ButtonRects(const DlgState* d, int clientDx, int clientDy, Button* out) {
    int w = DpiScale(190);
    int h = DpiScale(28);
    int gap = DpiScale(8);
    int x = clientDx - w - DpiScale(14);
    int y = DpiScale(40);
    int n = 0;
    out[n++] = {kBtnAnalyze, Rect(x, y, w, h), L"分析全篇 (Briss)"};
    y += h + gap;
    out[n++] = {kBtnDetect, Rect(x, y, w, h), L"检测当前页"};
    y += h + gap;
    int half = (w - gap) / 2;
    out[n++] = {kBtnPrevPage, Rect(x, y, half, h), L"< 上一页"};
    out[n++] = {kBtnNextPage, Rect(x + half + gap, y, half, h), L"下一页 >"};
    y += h + gap;
    out[n++] = {kBtnFullPage, Rect(x, y, w, h), L"恢复整页"};
    y += h + gap;
    out[n++] = {kBtnApply, Rect(x, y, w, h), L"当前页框套用到全部"};
    y += h + gap;
    out[n++] = {kBtnTarget, Rect(x, y, w, h), d->trimHeightOnly ? L"只裁上下（宽屏）" : L"裁掉四周空白"};
    y += h + gap + DpiScale(4);
    out[n++] = {kBtnClose, Rect(x, y, w, h), L"关闭"};
    (void)clientDy;
    return n;
}

static void DrawButton(HDC hdc, const Button& b, bool hovered, HFONT font) {
    RECT r = {(LONG)b.rc.x, (LONG)b.rc.y, (LONG)b.rc.Right(), (LONG)b.rc.Bottom()};
    HBRUSH br = CreateSolidBrush(hovered ? GetSysColor(COLOR_BTNFACE) : GetSysColor(COLOR_3DFACE));
    FillRect(hdc, &r, br);
    DeleteObject(br);
    FrameRect(hdc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
    HFONT prev = (HFONT)SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
    DrawTextW(hdc, b.label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, prev);
}

// Defined with the analysis below; Paint draws it under the margins.
static void StatusText(DlgState* d, WCHAR* buf, int n);

static void Paint(DlgState* d, HDC hdcTarget) {
    // first paint of a page: render it now, the preview has nowhere else to come from
    EnsurePagePixmap(d);
    Rect client = HwndClientRect(d->hwnd);
    HDC hdc = CreateCompatibleDC(hdcTarget);
    HBITMAP bmp = CreateCompatibleBitmap(hdcTarget, client.dx > 0 ? client.dx : 1, client.dy > 0 ? client.dy : 1);
    HBITMAP prevBmp = (HBITMAP)SelectObject(hdc, bmp);

    HBRUSH bg = CreateSolidBrush(GetSysColor(COLOR_3DFACE));
    RECT all = {0, 0, client.dx, client.dy};
    FillRect(hdc, &all, bg);
    DeleteObject(bg);

    HFONT prevFont = (HFONT)SelectObject(hdc, d->font);
    SetBkMode(hdc, TRANSPARENT);

    WCHAR title[128] = {};
    if (d->dm) {
        wsprintfW(title, L"调整裁剪范围 — 第 %d / %d 页", d->pageNo, d->dm->PageCount());
    } else {
        wsprintfW(title, L"调整裁剪范围");
    }
    RECT titleRc = {DpiScale(14), DpiScale(10), client.dx - DpiScale(14), DpiScale(32)};
    SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
    DrawTextW(hdc, title, -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // page image, then mask everything outside the crop so it reads as a preview
    Rect dest = FitPageInPreview(d->mediaBox, d->previewRect);
    Point tl = PageToClient(d, dest, d->crop.x, d->crop.y);
    Point br = PageToClient(d, dest, d->crop.Right(), d->crop.Bottom());
    Rect cropRc(tl.x, tl.y, br.x - tl.x, br.y - tl.y);

    // Briss' preview: the whole page stays visible, the part the crop drops is
    // washed out instead of being replaced by the dialog background, so what is
    // about to be cut can be checked before it goes.
    if (d->pix && d->pix->data) {
        SetStretchBltMode(hdc, HALFTONE);
        PaintBrissPreview(d->pix, hdc, dest, cropRc);
    }
    RECT pageRc = {dest.x, dest.y, dest.Right(), dest.Bottom()};
    FrameRect(hdc, &pageRc, (HBRUSH)GetStockObject(GRAY_BRUSH));

    HPEN pen = CreatePen(PS_DASH, 1, RGB(200, 30, 30));
    HPEN prevPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH hollow = (HBRUSH)GetStockObject(HOLLOW_BRUSH);
    HBRUSH prevBrush = (HBRUSH)SelectObject(hdc, hollow);
    Rectangle(hdc, cropRc.x, cropRc.y, cropRc.Right(), cropRc.Bottom());
    SelectObject(hdc, prevBrush);
    SelectObject(hdc, prevPen);
    DeleteObject(pen);

    // corner handles
    int hs = DpiScale(5);
    HBRUSH hb = CreateSolidBrush(RGB(200, 30, 30));
    Point corners[4] = {{cropRc.x, cropRc.y},
                        {cropRc.Right(), cropRc.y},
                        {cropRc.x, cropRc.Bottom()},
                        {cropRc.Right(), cropRc.Bottom()}};
    for (Point c : corners) {
        RECT r = {c.x - hs, c.y - hs, c.x + hs, c.y + hs};
        FillRect(hdc, &r, hb);
    }
    DeleteObject(hb);

    // readouts
    WCHAR info[256] = {};
    if (d->dm) {
        wsprintfW(info, L"边距 (pt)：左 %d  上 %d  右 %d  下 %d", (int)(d->crop.x - d->mediaBox.x),
                  (int)(d->crop.y - d->mediaBox.y), (int)(d->mediaBox.Right() - d->crop.Right()),
                  (int)(d->mediaBox.Bottom() - d->crop.Bottom()));
    }
    RECT infoRc = {DpiScale(14), client.dy - DpiScale(54), client.dx - DpiScale(240), client.dy - DpiScale(36)};
    DrawTextW(hdc, info, -1, &infoRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    WCHAR status[256] = {};
    StatusText(d, status, 256);
    RECT statusRc = {DpiScale(14), client.dy - DpiScale(74), client.dx - DpiScale(240), client.dy - DpiScale(56)};
    DrawTextW(hdc, status, -1, &statusRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    const WCHAR* hint = L"拖动边界调整；方向键微调，Shift 加速，Ctrl 改右/下边";
    RECT hintRc = {DpiScale(14), client.dy - DpiScale(34), client.dx - DpiScale(240), client.dy - DpiScale(12)};
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    DrawTextW(hdc, hint, -1, &hintRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));

    Button buttons[12];
    int nBtn = ButtonRects(d, client.dx, client.dy, buttons);
    for (int i = 0; i < nBtn; i++) {
        DrawButton(hdc, buttons[i], d->hoverBtn == buttons[i].id, d->font);
    }
    // the mode button is a toggle: draw it pressed while it is on
    for (int i = 0; i < nBtn; i++) {
        if (buttons[i].id != kBtnTarget || !d->trimHeightOnly) {
            continue;
        }
        RECT r = {(LONG)buttons[i].rc.x, (LONG)buttons[i].rc.y, (LONG)buttons[i].rc.Right(),
                  (LONG)buttons[i].rc.Bottom()};
        InflateRect(&r, -DpiScale(2), -DpiScale(2));
        DrawEdge(hdc, &r, BDR_SUNKENOUTER, BF_RECT);
    }

    SelectObject(hdc, prevFont);
    BitBlt(hdcTarget, 0, 0, client.dx, client.dy, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, prevBmp);
    DeleteObject(bmp);
    DeleteDC(hdc);
}

static void AutoDetect(DlgState* d) {
    EnsurePagePixmap(d);
    if (!d->pix || !d->pix->data) {
        return;
    }
    int w = d->pix->width;
    int h = d->pix->height;
    int bpp = 4;
    u8* gray = (u8*)malloc((size_t)w * (size_t)h);
    if (!gray) {
        return;
    }
    int stride = d->pix->stride;
    for (int y = 0; y < h; y++) {
        const u8* row = d->pix->data + (size_t)y * (size_t)stride;
        u8* out = gray + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            const u8* px = row + (size_t)x * (size_t)bpp;
            int b = px[0], g = px[1], r = px[2];
            // composite over white when the page has transparency
            int lum = (r * 299 + g * 587 + b * 114) / 1000;
            out[x] = (u8)limitValue(lum, 0, 255);
        }
    }
    float pppX = d->mediaBox.dx > 0 ? (float)w / d->mediaBox.dx : 1.f;
    float pppY = d->mediaBox.dy > 0 ? (float)h / d->mediaBox.dy : 1.f;
    float ppp = pppX < pppY ? pppX : pppY;
    int marginPx = (int)(4 * ppp);
    if (marginPx < 2) {
        marginPx = 2;
    }
    briss_crop::Rect r;
    bool ok = briss_crop::Detect(gray, w, h, w, &r, marginPx);
    free(gray);
    if (!ok) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    float sx = w > 0 ? (float)w / d->mediaBox.dx : 1.f;
    float sy = h > 0 ? (float)h / d->mediaBox.dy : 1.f;
    RectF box(d->mediaBox.x + (float)r.left / sx, d->mediaBox.y + (float)r.top / sy,
              (float)(r.right - r.left) / sx, (float)(r.bottom - r.top) / sy);
    if (box.dx < kMinCropPt || box.dy < kMinCropPt) {
        return;
    }
    d->crop = box;
}

// ---- whole-document analysis, the way Briss does it ------------------------
//
// Briss does not look at one page: it measures every page, groups the pages
// that have the same size and orientation, and crops each group to the box that
// still contains the content of all of its pages. A page whose text sits higher
// than its neighbours therefore keeps its text, while the group as a whole
// still loses the margins that every page has. The result is one rectangle per
// group, applied per page, not one rectangle for the whole document.

static void FreeGroups(DlgState* d) {
    free(d->groups);
    d->groups = nullptr;
    d->groupCount = 0;
    d->groupCap = 0;
    free(d->pageGroup);
    d->pageGroup = nullptr;
    d->pageGroupCount = 0;
    free(d->pageContent);
    d->pageContent = nullptr;
    free(d->pageCrop);
    d->pageCrop = nullptr;
}

// Pages count as the same size when they agree within a point and a half; scans
// of one book are not always pixel-identical.
static int GroupForSize(const DlgState* d, SizeF size) {
    for (int i = 0; i < d->groupCount; i++) {
        float dw = d->groups[i].size.dx - size.dx;
        float dh = d->groups[i].size.dy - size.dy;
        if (dw < 0) dw = -dw;
        if (dh < 0) dh = -dh;
        if (dw <= 1.5f && dh <= 1.5f) {
            return i;
        }
    }
    return -1;
}

// Renders one page at analysis resolution and returns the box around its content, as
// fractions of the page.
static bool MeasurePageContent(DlgState* d, int pageNo, SizeF* size, RectF* frac) {
    EngineBase* engine = d->dm ? d->dm->GetEngine() : nullptr;
    if (!engine) {
        return false;
    }
    RectF media = d->dm->PageMediaBox(pageNo);
    if (media.dx < 5 || media.dy < 5) {
        return false;
    }
    size->dx = media.dx;
    size->dy = media.dy;
    float longest = media.dx > media.dy ? media.dx : media.dy;
    int targetPx = (int)(longest / kAnalyzePtPerPx);
    if (targetPx < kAnalyzePxMin) {
        targetPx = kAnalyzePxMin;
    }
    if (targetPx > kAnalyzePxMax) {
        targetPx = kAnalyzePxMax;
    }
    float scale = (float)targetPx / longest;
    // rotation 0: the pixel box has to line up with the unrotated page so the
    // fractions can be used as a crop on the page as it is laid out
    RenderPageArgs args(pageNo, scale, 0);
    Pixmap* pix = engine->RenderPage(args);
    if (!pix) {
        return false;
    }
    Pixmap* img = PixmapToBgra(pix);  // takes ownership of pix
    if (!img) {
        return false;
    }
    defer { FreePixmap(img); };
    int w = img->width;
    int h = img->height;
    if (!img->data || w < 8 || h < 8) {
        return false;
    }
    u8* gray = (u8*)malloc((size_t)w * (size_t)h);
    if (!gray) {
        return false;
    }
    defer { free(gray); };
    int stride = img->stride;
    for (int y = 0; y < h; y++) {
        const u8* row = img->data + (size_t)y * (size_t)stride;
        u8* out = gray + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            const u8* px = row + (size_t)x * 4;
            int lum = (px[2] * 299 + px[1] * 587 + px[0] * 114) / 1000;
            out[x] = (u8)limitValue(lum, 0, 255);
        }
    }
    briss_crop::Rect r;
    if (!briss_crop::Detect(gray, w, h, w, &r, 0)) {
        return false;
    }
    int bw = r.right - r.left;
    int bh = r.bottom - r.top;
    if (bw < 3 || bh < 3) {
        return false;
    }
    *frac = RectF((float)r.left / (float)w, (float)r.top / (float)h, (float)bw / (float)w, (float)bh / (float)h);
    return true;
}

static void AnalyzeStart(DlgState* d) {
    if (!d->dm || d->analyzing) {
        return;
    }
    FreeGroups(d);
    int n = d->dm->PageCount();
    d->pageGroup = (int*)malloc(sizeof(int) * (size_t)(n + 1));
    if (!d->pageGroup) {
        return;
    }
    d->pageGroupCount = n + 1;
    d->pageContent = (RectF*)calloc((size_t)(n + 1), sizeof(RectF));
    d->pageCrop = (RectF*)calloc((size_t)(n + 1), sizeof(RectF));
    if (!d->pageContent || !d->pageCrop) {
        FreeGroups(d);
        return;
    }
    for (int i = 0; i <= n; i++) {
        d->pageGroup[i] = -1;
    }
    d->analyzing = true;
    d->analyzePage = 1;
    d->analyzeTotal = n;
    d->analyzeFailed = 0;
    d->analyzeStartedAt = GetTickCount64();
    SetTimer(d->hwnd, kTimerAnalyze, 15, nullptr);
    InvalidateRect(d->hwnd, nullptr, FALSE);
}

// Measures pages until the slice is used up; returns true while work remains.
static bool AnalyzeStep(DlgState* d) {
    ULONGLONG startedAt = GetTickCount64();
    while (d->analyzePage <= d->analyzeTotal) {
        int pageNo = d->analyzePage;
        d->analyzePage++;
        SizeF size;
        RectF frac;
        if (!MeasurePageContent(d, pageNo, &size, &frac)) {
            d->analyzeFailed++;
        } else {
            int g = GroupForSize(d, size);
            if (g < 0) {
                if (d->groupCount == d->groupCap) {
                    int newCap = d->groupCap > 0 ? d->groupCap * 2 : 8;
                    BrissGroup* grown = (BrissGroup*)realloc(d->groups, sizeof(BrissGroup) * (size_t)newCap);
                    if (!grown) {
                        return false;
                    }
                    d->groups = grown;
                    d->groupCap = newCap;
                }
                g = d->groupCount;
                d->groupCount++;
                d->groups[g] = BrissGroup();
                d->groups[g].size = size;
                d->groups[g].frac = frac;
                d->groups[g].count = 1;
            } else {
                // union: keeps the content of every page in the group
                d->groups[g].frac = d->groups[g].frac.Union(frac);
                d->groups[g].count++;
            }
            if (pageNo < d->pageGroupCount) {
                d->pageGroup[pageNo] = g;
                RectF media = d->dm->PageMediaBox(pageNo);
                d->pageContent[pageNo] = RectF(media.x + frac.x * media.dx, media.y + frac.y * media.dy,
                                               frac.dx * media.dx, frac.dy * media.dy);
            }
        }
        // Hand the previous pages' decoded images back: a cached display list
        // keeps them alive, which on scanned pages is ~20 MB each.
        EngineMupdfTrimPageCaches(d->dm->GetEngine(), pageNo);
        if (GetTickCount64() - startedAt >= (ULONGLONG)kAnalyzeSliceMs) {
            break;
        }
    }
    return d->analyzePage <= d->analyzeTotal;
}

// Places a frame of w x h points over the content of one page: centred on that
// content and kept inside the page. Centring is what makes the pages line up:
// a page whose text sits 4 pt lower than its neighbour's gets a crop that is
// 4 pt lower as well, so the text lands in the same place in both.
static RectF CenterFrameOnContent(DlgState* d, int pageNo, float w, float h) {
    RectF c = d->pageContent[pageNo];
    RectF media = d->dm->PageMediaBox(pageNo);
    if (w > media.dx) {
        w = media.dx;
    }
    if (h > media.dy) {
        h = media.dy;
    }
    float x = c.x + c.dx / 2 - w / 2;
    float y = c.y + c.dy / 2 - h / 2;
    x = limitValue(x, media.x, media.Right() - w);
    y = limitValue(y, media.y, media.Bottom() - h);
    return RectF(x, y, w, h);
}

// The frame of a group is the size of the largest content in it (so no page is
// ever cut) and every page of the group is cropped to that same frame, centred
// on its own content. A plain union kept each page's own offset instead, which
// is what made one-offset-in-every-direction pages inflate the crop.
static void BuildGroupCrops(DlgState* d, int g) {
    if (!d->pageContent || !d->pageCrop || g < 0 || g >= d->groupCount) {
        return;
    }
    float w = 0;
    float h = 0;
    for (int pageNo = 1; pageNo < d->pageGroupCount; pageNo++) {
        if (d->pageGroup[pageNo] != g) {
            continue;
        }
        RectF c = d->pageContent[pageNo];
        if (c.dx > w) {
            w = c.dx;
        }
        if (c.dy > h) {
            h = c.dy;
        }
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    // Height-only: a wide page on a wide screen costs scrolling, not side
    // space, so keep the full width (the printed line width is then unchanged)
    // and let the analysis take just the blank bands off the top and bottom.
    // Each page keeps its own vertical centre, which is what lines them up.
    if (d->trimHeightOnly) {
        w = d->dm ? d->dm->PageMediaBox(1).dx : w;
        for (int pageNo = 1; pageNo < d->pageGroupCount; pageNo++) {
            if (d->pageGroup[pageNo] != g) {
                continue;
            }
            float pageW = d->dm->PageMediaBox(pageNo).dx;
            if (pageW > w) {
                w = pageW;
            }
        }
    }
    d->groups[g].frameW = w;
    d->groups[g].frameH = h;
    for (int pageNo = 1; pageNo < d->pageGroupCount; pageNo++) {
        if (d->pageGroup[pageNo] != g) {
            continue;
        }
        // with the full page width the horizontal centring is a no-op and only
        // the top/bottom bands get taken off
        float pageW = w;
        if (d->trimHeightOnly) {
            pageW = d->dm->PageMediaBox(pageNo).dx;
        }
        d->pageCrop[pageNo] = CenterFrameOnContent(d, pageNo, pageW, h);
    }
}

static void BuildAllGroupCrops(DlgState* d) {
    for (int g = 0; g < d->groupCount; g++) {
        BuildGroupCrops(d, g);
    }
}

// Hands the per-page rectangles to the display model.
static void AnalyzeApply(DlgState* d) {
    int n = d->dm ? d->dm->PageCount() : 0;
    if (n <= 0 || !d->pageCrop) {
        return;
    }
    RectF* fracs = (RectF*)malloc(sizeof(RectF) * (size_t)n);
    if (!fracs) {
        return;
    }
    for (int i = 0; i < n; i++) {
        int pageNo = i + 1;
        RectF c = pageNo < d->pageGroupCount ? d->pageCrop[pageNo] : RectF();
        RectF media = pageNo < d->pageGroupCount ? d->dm->PageMediaBox(pageNo) : RectF();
        if (c.IsEmpty() || media.dx <= 0 || media.dy <= 0) {
            fracs[i] = RectF();
            continue;
        }
        fracs[i] = RectF((c.x - media.x) / media.dx, (c.y - media.y) / media.dy, c.dx / media.dx, c.dy / media.dy);
    }
    ScrollState st = d->dm->GetScrollState();
    DisplayModelApplyPageCrops(d->dm, fracs, n);
    d->dm->SetScrollState(st);
    free(fracs);
}

static void AnalyzeFinish(DlgState* d) {
    KillTimer(d->hwnd, kTimerAnalyze);
    d->analyzing = false;
    BuildAllGroupCrops(d);
    AnalyzeApply(d);
    int g = d->pageNo < d->pageGroupCount ? d->pageGroup[d->pageNo] : -1;
    RectF c = g >= 0 && d->pageCrop ? d->pageCrop[d->pageNo] : RectF();
    d->crop = c.IsEmpty() ? d->mediaBox : c;
    InvalidateRect(d->hwnd, nullptr, FALSE);
}

// One line under the margins: what the analysis is doing, or what it found.
static void StatusText(DlgState* d, WCHAR* buf, int n) {
    if (d->analyzing) {
        EngineMupdfTrimPageCaches(d->dm->GetEngine(), d->pageNo);
        int done = d->analyzePage - 1;
        int pct = d->analyzeTotal > 0 ? done * 100 / d->analyzeTotal : 0;
        wsprintfW(buf, L"分析全篇：%d / %d 页（%d%%）", done, d->analyzeTotal, pct);
        return;
    }
    if (d->groupCount > 0) {
        int g = d->pageNo < d->pageGroupCount ? d->pageGroup[d->pageNo] : -1;
        if (g >= 0) {
            wsprintfW(buf, L"已分析 %d 页，%d 组；本页第 %d 组，组框 %d×%d pt%s", d->analyzeTotal,
                      d->groupCount, g + 1, (int)d->groups[g].frameW, (int)d->groups[g].frameH,
                      d->trimHeightOnly ? L"（只裁上下）" : L"，已按内容居中");
        } else {
            wsprintfW(buf, L"已分析 %d 页，%d 组；本页没有可用内容，不裁剪", d->analyzeTotal, d->groupCount);
        }
        return;
    }
    wsprintfW(buf, L"尚未分析全篇：现在的框只作用于这一页");
    (void)n;
}

// Switches the previewed rectangle to the one its group was cropped to.
static void ShowGroupCropForPage(DlgState* d) {
    int g = (d->pageGroup && d->pageNo < d->pageGroupCount) ? d->pageGroup[d->pageNo] : -1;
    if (g >= 0 && d->pageCrop) {
        RectF c = d->pageCrop[d->pageNo];
        if (!c.IsEmpty()) {
            d->crop = c;
        }
    }
}

static void ApplyCrop(DlgState* d) {
    if (!d->dm || d->mediaBox.dx <= 0 || d->mediaBox.dy <= 0) {
        return;
    }
    // A full-page rectangle means "no crop", not "crop to the whole page": the
    // latter would keep TrimEmptyMargins on and make the trim path re-measure
    // the page at every zoom, which visibly slows paging after re-opening a
    // cropped view. Clearing the user crop restores the plain page.
    bool full = d->crop.x <= d->mediaBox.x + 0.5f && d->crop.y <= d->mediaBox.y + 0.5f &&
                d->crop.Right() >= d->mediaBox.Right() - 0.5f && d->crop.Bottom() >= d->mediaBox.Bottom() - 0.5f;
    ScrollState st = d->dm->GetScrollState();
    d->dm->SetUserCrop(full ? RectF() : RectF((d->crop.x - d->mediaBox.x) / d->mediaBox.dx,
                                             (d->crop.y - d->mediaBox.y) / d->mediaBox.dy,
                                             d->crop.dx / d->mediaBox.dx, d->crop.dy / d->mediaBox.dy));
    d->dm->SetScrollState(st);
}

static int HitTest(const DlgState* d, Point pt, const Rect& dest) {
    Point tl = PageToClient(d, dest, d->crop.x, d->crop.y);
    Point br = PageToClient(d, dest, d->crop.Right(), d->crop.Bottom());
    int slop = kHitSlopPx;
    bool inX = pt.x >= tl.x - slop && pt.x <= br.x + slop;
    bool inY = pt.y >= tl.y - slop && pt.y <= br.y + slop;
    if (!inX || !inY) {
        return kDragNone;
    }
    int hit = kDragNone;
    if (abs(pt.x - tl.x) <= slop) {
        hit |= kDragLeft;
    }
    if (abs(pt.x - br.x) <= slop) {
        hit |= kDragRight;
    }
    if (abs(pt.y - tl.y) <= slop) {
        hit |= kDragTop;
    }
    if (abs(pt.y - br.y) <= slop) {
        hit |= kDragBottom;
    }
    if (hit) {
        return hit;
    }
    if (pt.x > tl.x && pt.x < br.x && pt.y > tl.y && pt.y < br.y) {
        return kDragMove;
    }
    return kDragNone;
}

static void DragTo(DlgState* d, const Rect& dest, Point pt) {
    float px = 0;
    float py = 0;
    ClientToPage(d, dest, pt, &px, &py);
    RectF c = d->dragStartCrop;
    if (d->drag & kDragMove) {
        float mx = d->dragStartCrop.x + (px - d->dragOrigin.x);
        float my = d->dragStartCrop.y + (py - d->dragOrigin.y);
        mx = limitValue(mx, d->mediaBox.x, d->mediaBox.Right() - c.dx);
        my = limitValue(my, d->mediaBox.y, d->mediaBox.Bottom() - c.dy);
        // undo the clamping done above for the start rect offset
        d->crop = RectF(mx, my, c.dx, c.dy);
        return;
    }
    RectF n = c;
    if (d->drag & kDragLeft) {
        float v = limitValue(px, d->mediaBox.x, c.Right() - kMinCropPt);
        n = RectF(v, n.y, c.Right() - v, n.dy);
    }
    if (d->drag & kDragRight) {
        float v = limitValue(px, c.x + kMinCropPt, d->mediaBox.Right());
        n.dx = v - n.x;
    }
    if (d->drag & kDragTop) {
        float v = limitValue(py, d->mediaBox.y, c.Bottom() - kMinCropPt);
        n = RectF(n.x, v, n.dx, c.Bottom() - v);
    }
    if (d->drag & kDragBottom) {
        float v = limitValue(py, c.y + kMinCropPt, d->mediaBox.Bottom());
        n.dy = v - n.y;
    }
    d->crop = n;
}

static void Nudge(DlgState* d, int dx, int dy, bool resize) {
    float step = (GetKeyState(VK_SHIFT) & 0x8000) ? 10.f : 1.f;
    RectF c = d->crop;
    if (resize) {
        float newDx = limitValue(c.dx + dx * step, (float)kMinCropPt, d->mediaBox.Right() - c.x);
        float newDy = limitValue(c.dy + dy * step, (float)kMinCropPt, d->mediaBox.Bottom() - c.y);
        d->crop = RectF(c.x, c.y, newDx, newDy);
    } else {
        float x = limitValue(c.x + dx * step, d->mediaBox.x, d->mediaBox.Right() - c.dx);
        float y = limitValue(c.y + dy * step, d->mediaBox.y, d->mediaBox.Bottom() - c.dy);
        d->crop = RectF(x, y, c.dx, c.dy);
    }
}

// Re-applies the group rectangles after the user adjusted one of them: the
// edited rectangle becomes its group's rectangle, so the pages that share the
// page size follow along (that is what makes the adjustment worth anything).
static void GroupCropChanged(DlgState* d) {
    if (!d->pageGroup || d->groupCount <= 0) {
        return;
    }
    int g = d->pageNo < d->pageGroupCount ? d->pageGroup[d->pageNo] : -1;
    if (g < 0 || !d->pageCrop) {
        return;
    }
    // the page keeps exactly the rectangle the user dragged; the rest of the
    // group is re-centred on its own content with the new frame size
    d->pageCrop[d->pageNo] = d->crop;
    d->groups[g].frameW = d->crop.dx;
    d->groups[g].frameH = d->crop.dy;
    for (int pageNo = 1; pageNo < d->pageGroupCount; pageNo++) {
        if (pageNo == d->pageNo || d->pageGroup[pageNo] != g) {
            continue;
        }
        d->pageCrop[pageNo] = CenterFrameOnContent(d, pageNo, d->crop.dx, d->crop.dy);
    }
    AnalyzeApply(d);
}

static void OnButton(DlgState* d, int id) {
    switch (id) {
        case kBtnAnalyze:
            AnalyzeStart(d);
            break;
        case kBtnDetect:
            AutoDetect(d);
            break;
        case kBtnFullPage:
            d->crop = d->mediaBox;
            break;
        case kBtnPrevPage:
            if (d->pageNo > 1) {
                LoadPage(d, d->pageNo - 1, true);
                ShowGroupCropForPage(d);
            }
            break;
        case kBtnNextPage:
            if (d->dm && d->pageNo < d->dm->PageCount()) {
                LoadPage(d, d->pageNo + 1, true);
                ShowGroupCropForPage(d);
            }
            break;
        case kBtnApply:
            ApplyCrop(d);
            break;
        case kBtnTarget:
            // Re-crops with the other rule. Rebuilding from the measured
            // content boxes is instant; the manual rectangle is only used when
            // nothing has been analysed yet.
            d->trimHeightOnly = !d->trimHeightOnly;
            if (d->groupCount > 0) {
                BuildAllGroupCrops(d);
                AnalyzeApply(d);
                ShowGroupCropForPage(d);
            }
            break;
        case kBtnClose:
            DestroyWindow(d->hwnd);
            return;
    }
    InvalidateRect(d->hwnd, nullptr, FALSE);
}

static LRESULT CALLBACK CropDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    DlgState* d = gDlg;
    if (!d || d->hwnd != hwnd) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            Paint(d, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_SIZE: {
            int cx = LOWORD(lp);
            int cy = HIWORD(lp);
            Layout(d, cx, cy);
            FreePagePixmap(d);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            Point pt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            Rect client = HwndClientRect(hwnd);
            Button buttons[12];
            int n = ButtonRects(d, client.dx, client.dy, buttons);
            for (int i = 0; i < n; i++) {
                if (buttons[i].rc.Contains(pt)) {
                    SetFocus(hwnd);
                    OnButton(d, buttons[i].id);
                    return 0;
                }
            }
            Rect dest = FitPageInPreview(d->mediaBox, d->previewRect);
            int hit = HitTest(d, pt, dest);
            if (hit) {
                SetFocus(hwnd);
                SetCapture(hwnd);
                float sx = 0;
                float sy = 0;
                ClientToPage(d, dest, pt, &sx, &sy);
                d->drag = hit;
                d->dragStartCrop = d->crop;
                d->dragOrigin = PointF(sx, sy);
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            Point pt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (d->drag) {
                Rect dest = FitPageInPreview(d->mediaBox, d->previewRect);
                DragTo(d, dest, pt);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            Rect client = HwndClientRect(hwnd);
            Button buttons[12];
            int n = ButtonRects(d, client.dx, client.dy, buttons);
            int hover = 0;
            for (int i = 0; i < n; i++) {
                if (buttons[i].rc.Contains(pt)) {
                    hover = buttons[i].id;
                }
            }
            if (hover != d->hoverBtn) {
                d->hoverBtn = hover;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONUP:
            if (d->drag) {
                d->drag = 0;
                ReleaseCapture();
                GroupCropChanged(d);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_TIMER:
            if (wp == kTimerAnalyze && d->analyzing) {
                if (!AnalyzeStep(d)) {
                    AnalyzeFinish(d);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;

        case WM_SETCURSOR: {
            Point pt = HwndGetCursorPos(hwnd);
            Rect dest = FitPageInPreview(d->mediaBox, d->previewRect);
            int hit = HitTest(d, pt, dest);
            const WCHAR* id = IDC_ARROW;
            if (hit == (kDragLeft | kDragTop) || hit == (kDragRight | kDragBottom)) {
                id = IDC_SIZENWSE;
            } else if (hit == (kDragRight | kDragTop) || hit == (kDragLeft | kDragBottom)) {
                id = IDC_SIZENESW;
            } else if (hit & (kDragLeft | kDragRight)) {
                id = IDC_SIZEWE;
            } else if (hit & (kDragTop | kDragBottom)) {
                id = IDC_SIZENS;
            } else if (hit == kDragMove) {
                id = IDC_SIZEALL;
            }
            SetCursor(LoadCursorW(nullptr, id));
            return TRUE;
        }

        case WM_KEYDOWN: {
            int dx = 0;
            int dy = 0;
            if (wp == VK_LEFT) {
                dx = -1;
            } else if (wp == VK_RIGHT) {
                dx = 1;
            } else if (wp == VK_UP) {
                dy = -1;
            } else if (wp == VK_DOWN) {
                dy = 1;
            } else if (wp == VK_ESCAPE) {
                DestroyWindow(hwnd);
                return 0;
            } else {
                break;
            }
            bool resize = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            Nudge(d, dx, dy, resize);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_ACTIVATE: {
            // the window stays open while the user switches tabs: follow the document
            DisplayModel* dm = d->win ? d->win->AsFixed() : nullptr;
            if (dm != d->dm) {
                if (dm) {
                    d->dm = dm;
                    FreeGroups(d);  // groups belong to the document that was analyzed
                    LoadPage(d, dm->CurrentPageNo());
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, kTimerAnalyze);
            FreeGroups(d);
            FreePagePixmap(d);
            if (d->fontOwned && d->font) {
                DeleteObject(d->font);
            }
            gDlg = nullptr;
            delete d;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void EnsureClass() {
    static ATOM atom = 0;
    if (atom) {
        return;
    }
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = CropDlgProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    wc.lpszClassName = L"SumatraCropAdjustWnd";
    atom = RegisterClassExW(&wc);
}

}  // namespace cropdlg

// Open (or focus) the crop adjustment window for the current document.
static void ShowCropAdjustDialog(MainWindow* win) {
    using namespace cropdlg;
    if (!IsUsable(win)) {
        return;
    }
    if (gDlg && IsWindow(gDlg->hwnd)) {
        SetForegroundWindow(gDlg->hwnd);
        InvalidateRect(gDlg->hwnd, nullptr, FALSE);
        return;
    }
    EnsureClass();

    RECT frame = {};
    GetWindowRect(win->hwndFrame, &frame);
    // client size, then the window size that gives it, then a position that
    // stays inside the work area so the whole dialog is reachable
    int clientDx = DpiScale(900);
    int clientDy = DpiScale(620);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT rc = {0, 0, clientDx, clientDy};
    AdjustWindowRect(&rc, style, FALSE);
    int winDx = rc.right - rc.left;
    int winDy = rc.bottom - rc.top;
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int workDx = work.right - work.left;
    int workDy = work.bottom - work.top;
    if (workDx > 0 && winDx > workDx - DpiScale(20)) {
        clientDx -= winDx - (workDx - DpiScale(20));
        rc = {0, 0, clientDx, clientDy};
        AdjustWindowRect(&rc, style, FALSE);
        winDx = rc.right - rc.left;
        winDy = rc.bottom - rc.top;
    }
    if (workDy > 0 && winDy > workDy - DpiScale(20)) {
        clientDy -= winDy - (workDy - DpiScale(20));
        rc = {0, 0, clientDx, clientDy};
        AdjustWindowRect(&rc, style, FALSE);
        winDx = rc.right - rc.left;
        winDy = rc.bottom - rc.top;
    }
    int x = frame.left + ((frame.right - frame.left) - winDx) / 2;
    int y = frame.top + ((frame.bottom - frame.top) - winDy) / 2;
    if (workDx > 0 && workDy > 0) {
        if (x + winDx > work.right) {
            x = work.right - winDx;
        }
        if (y + winDy > work.bottom) {
            y = work.bottom - winDy;
        }
        if (x < work.left) {
            x = work.left;
        }
        if (y < work.top) {
            y = work.top;
        }
    }

    DlgState* d = new DlgState();
    d->win = win;
    d->dm = win->AsFixed();
    d->dpi = DpiScale(96);
    d->font = CreateFontW(-MulDiv(9, d->dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                          L"Microsoft YaHei");
    if (!d->font) {
        d->font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    } else {
        d->fontOwned = true;
    }
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"SumatraCropAdjustWnd", L"调整裁剪范围", style, x, y, winDx,
                                winDy, win->hwndFrame, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        if (d->fontOwned && d->font) {
            DeleteObject(d->font);
        }
        delete d;
        return;
    }
    d->hwnd = hwnd;
    gDlg = d;
    Layout(d, clientDx, clientDy);
    LoadPage(d, d->dm->CurrentPageNo());
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
}

#endif  // LOCAL_CROP_DIALOG_H_
