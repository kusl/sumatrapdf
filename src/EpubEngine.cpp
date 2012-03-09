/* Copyright 2006-2012 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Test engine to see how well the BaseEngine API fits a flowed ebook format.
// (pages are layout out the same as for a "B Format" paperback: 5.12" x 7.8")

#include "EpubEngine.h"
#include "StrUtil.h"
#include "FileUtil.h"
#include "Scoped.h"
#include "ZipUtil.h"
#include "TrivialHtmlParser.h"
#include "Allocator.h"

/* epub loading code (cf. ebooktest2/EpubDoc.cpp and ebooktest2/BaseEbookDoc.h) */

// for ImageData
#include "MobiDoc.h"

struct ImageData2 {
    ImageData base;
    char *  id; // path by which content refers to this image
    size_t  idx; // document specific index at which to find this image
};

class EpubEngineImpl;

class EpubDoc {
    friend EpubEngineImpl;

    ZipFile zip;
    str::Str<char> htmlData;
    Vec<ImageData2> images;

    bool Load();

    static bool VerifyEpub(ZipFile& zip) {
        ScopedMem<char> firstFileData(zip.GetFileData(_T("mimetype")));
        // a proper Epub documents has a "mimetype" file with content
        // "application/epub+zip" as the first entry in its ZIP structure
        return str::Eq(zip.GetFileName(0), _T("mimetype")) &&
               str::Eq(firstFileData, "application/epub+zip");
    }

public:
    EpubDoc(const TCHAR *fileName) : zip(fileName) { }
    ~EpubDoc() {
        for (size_t i = 0; i < images.Count(); i++) {
            free(images.At(i).base.data);
            free(images.At(i).id);
        }
    }

    const char *GetBookHtmlData(size_t& lenOut) {
        lenOut = htmlData.Size();
        return htmlData.Get();
    }

    ImageData *GetImageData(const char *id) {
        // TODO: paths are relative from the html document to the image
        for (size_t i = 0; i < images.Count(); i++) {
            if (str::EndsWith(id, images.At(i).id))
                return GetImageData(i);
        }
        return NULL;
    }

    ImageData *GetImageData(size_t index) {
        if (index >= images.Count())
            return NULL;
        if (!images.At(index).base.data) {
            images.At(index).base.data = zip.GetFileData(images.At(index).idx, &images.At(index).base.len);
            if (!images.At(index).base.data)
                return NULL;
        }
        return &images.At(index).base;
    }

    static bool IsSupportedFile(const TCHAR *fileName, bool sniff) {
        if (sniff) {
            return VerifyEpub(ZipFile(fileName));
        }
        return str::EndsWithI(fileName, _T(".epub"));
    }
};

static void UrlDecode(TCHAR *url)
{
    for (TCHAR *src = url; *src; src++, url++) {
        int val;
        if (*src == '%' && str::Parse(src, _T("%%%2x"), &val)) {
            *url = (char)val;
            src += 2;
        } else {
            *url = *src;
        }
    }
    *url = '\0';
}

bool EpubDoc::Load()
{
    if (!VerifyEpub(zip))
        return false;

    ScopedMem<char> container(zip.GetFileData(_T("META-INF/container.xml")));
    HtmlParser parser;
    HtmlElement *node = parser.ParseInPlace(container);
    if (!node)
        return false;
    // only consider the first <rootfile> element (default rendition)
    node = parser.FindElementByName("rootfile");
    if (!node)
        return false;
    ScopedMem<TCHAR> contentPath(node->GetAttribute("full-path"));
    if (!contentPath)
        return false;

    ScopedMem<char> content(zip.GetFileData(contentPath));
    if (!content)
        return false;
    node = parser.ParseInPlace(content);
    if (!node)
        return false;
    node = parser.FindElementByName("manifest");
    if (!node)
        return false;

    if (str::FindChar(contentPath, '/'))
        *(TCHAR *)(str::FindCharLast(contentPath, '/') + 1) = '\0';
    else
        *contentPath = '\0';
    for (node = node->down; node; node = node->next) {
        ScopedMem<TCHAR> mediatype(node->GetAttribute("media-type"));
        if (str::Eq(mediatype, _T("application/xhtml+xml"))) {
            ScopedMem<TCHAR> htmlPath(node->GetAttribute("href"));
            if (!htmlPath)
                continue;
            htmlPath.Set(str::Join(contentPath, htmlPath));

            ScopedMem<char> html(zip.GetFileData(htmlPath));
            if (!html)
                continue;
            if (htmlData.Count() > 0) {
                // insert explicit page-breaks between sections
                htmlData.Append("<pagebreak />");
            }
            // TODO: merge/remove <head>s and drop everything else outside of <body>s(?)
            htmlData.Append(html);
        }
        else if (str::Eq(mediatype, _T("image/png"))  ||
                 str::Eq(mediatype, _T("image/jpeg")) ||
                 str::Eq(mediatype, _T("image/gif"))) {
            ScopedMem<TCHAR> imgPath(node->GetAttribute("href"));
            if (!imgPath)
                continue;
            ScopedMem<TCHAR> zipPath(str::Join(contentPath, imgPath));
            UrlDecode(zipPath);
            // load the image lazily
            ImageData2 data = { 0 };
            data.id = str::conv::ToUtf8(imgPath);
            data.idx = zip.GetFileIndex(zipPath);
            images.Append(data);
        }
    }

    return htmlData.Count() > 0;
}

/* PageLayout extensions for Epub (cf. ebooktest2/PageLayout.cpp) */

#include "PageLayout.h"
#include "ebooktest2/MiniMui.h"
#include "GdiPlusUtil.h"

class PageLayout2 : public PageLayout {
    void HandleTagImg2(HtmlToken *t);
    void HandleTagHeader(HtmlToken *t);
    void HandleHtmlTag2(HtmlToken *t);

    bool IgnoreText();
    bool IsEmptyPage(PageData *d);

public:
    PageLayout2(LayoutInfo *li);

    Vec<PageData*> *Layout();
};

void PageLayout2::HandleTagImg2(HtmlToken *t)
{
    if (!layoutInfo->mobiDoc)
        return;
    EpubDoc *doc = (EpubDoc *)layoutInfo->mobiDoc;

    AttrInfo *attr = t->GetAttrByName("src");
    if (!attr)
        return;
    ScopedMem<char> src(str::DupN(attr->val, attr->valLen));
    ImageData *img = doc->GetImageData(src);
    if (img)
        EmitImage(img);
}

void PageLayout2::HandleTagHeader(HtmlToken *t)
{
    if (t->IsEndTag()) {
        HandleTagP(t);
        currFontSize = defaultFontSize;
        ChangeFontStyle(FontStyleBold, false);
        currY += 10;
    }
    else {
        currJustification = Align_Left;
        HandleTagP(t);
        currFontSize = defaultFontSize * (1 + ('5' - t->s[1]) * 0.2f);
        ChangeFontStyle(FontStyleBold, true);
    }
}

void PageLayout2::HandleHtmlTag2(HtmlToken *t)
{
    HtmlTag tag = FindTag(t);
    switch (tag) {
    case Tag_Img:
        HandleTagImg2(t);
        break;
    case Tag_Pagebreak:
        ForceNewPage();
        break;
    case Tag_Ul: case Tag_Ol: case Tag_Dl:
        currJustification = Align_Left;
        break;
    case Tag_Li: case Tag_Dd: case Tag_Dt:
        FlushCurrLine(false);
        break;
    case Tag_Center:
        currJustification = Align_Center;
        break;
    case Tag_H1: case Tag_H2: case Tag_H3:
    case Tag_H4: case Tag_H5:
        HandleTagHeader(t);
        break;
    case Tag_Abbr: case Tag_Acronym: case Tag_Code:
    case Tag_Lh: case Tag_Link: case Tag_Meta:
    case Tag_Pre: case Tag_Style: case Tag_Title:
        // ignore instead of crashing in HandleHtmlTag
        break;
    default:
        HandleHtmlTag(t);
        break;
    }
}

PageLayout2::PageLayout2(LayoutInfo* li)
{
    CrashIf(currPage);
    finishedParsing = false;
    layoutInfo = li;
    pageDx = (REAL)layoutInfo->pageDx;
    pageDy = (REAL)layoutInfo->pageDy;
    textAllocator = layoutInfo->textAllocator;
    htmlParser = new HtmlPullParser(layoutInfo->htmlStr, layoutInfo->htmlStrLen);

    CrashIf(gfx);
    gfx = mui::AllocGraphicsForMeasureText();
    defaultFontName.Set(str::Dup(layoutInfo->fontName));
    defaultFontSize = layoutInfo->fontSize;
    SetCurrentFont(FontStyleRegular, defaultFontSize);

    coverImage = NULL;
    pageCount = 0;
    inLink = false;

    lineSpacing = currFont->GetHeight(gfx);
    spaceDx = currFontSize / 2.5f; // note: a heuristic
    float spaceDx2 = GetSpaceDx(gfx, currFont);
    if (spaceDx2 < spaceDx)
        spaceDx = spaceDx2;

    currJustification = Align_Justify;
    currX = 0; currY = 0;
    currPage = new PageData;
    currPage->reparsePoint = currReparsePoint;

    currLineTopPadding = 0;
    // Epub documents contain no cover image
}

bool PageLayout2::IgnoreText()
{
    // ignore the content of <head>, <style> and <title> tags
    return htmlParser->tagNesting.Find(Tag_Head) != -1 ||
           htmlParser->tagNesting.Find(Tag_Style) != -1 ||
           htmlParser->tagNesting.Find(Tag_Title) != -1;
}

// empty page is one that consists of only invisible instructions
bool PageLayout2::IsEmptyPage(PageData *p)
{
    if (!p)
        return false;
    DrawInstr *i;
    for (i = p->instructions.IterStart(); i; i = p->instructions.IterNext()) {
        switch (i->type) {
        case InstrString:
        case InstrImage:
            return false;
        case InstrLine:
            // if a page only consits of lines we consider it empty. It's different
            // than what Kindle does but I don't see the purpose of showing such
            // pages to the user
            break;
        }
    }
    // all instructions were invisible
    return true;
}

Vec<PageData*> *PageLayout2::Layout()
{
    HtmlToken *t;
    while ((t = htmlParser->Next()) && !t->IsError()) {
        currReparsePoint = t->GetReparsePoint();
        if (t->IsTag())
            HandleHtmlTag2(t);
        else if (!IgnoreText())
            HandleText(t);
    }

    FlushCurrLine(true);
    pagesToSend.Append(currPage);
    currPage = NULL;

    // remove empty pages (same as PageLayout::IterNext)
    for (size_t i = 0; i < pagesToSend.Count(); i++) {
        if (IsEmptyPage(pagesToSend.At(i))) {
            delete pagesToSend.At(i);
            pagesToSend.RemoveAt(i--);
        }
    }

    Vec<PageData *> *result = new Vec<PageData *>(pagesToSend);
    pagesToSend.Reset();
    return result;
}

/* actual BaseEngine for handling Epub documents */

class EpubEngineImpl : public EpubEngine {
    friend EpubEngine;

public:
    EpubEngineImpl();
    virtual ~EpubEngineImpl();
    virtual EpubEngine *Clone() {
        return CreateFromFile(fileName);
    }

    virtual const TCHAR *FileName() const { return fileName; };
    virtual int PageCount() const { return pages ? pages->Count() : 0; }

    virtual RectD PageMediabox(int pageNo) { return pageRect; }
    virtual RectD PageContentBox(int pageNo, RenderTarget target=Target_View) {
        RectD mbox = PageMediabox(pageNo);
        mbox.Inflate(-PAGE_BORDER, -PAGE_BORDER);
        return mbox;
    }

    virtual RenderedBitmap *RenderBitmap(int pageNo, float zoom, int rotation,
                         RectD *pageRect=NULL, /* if NULL: defaults to the page's mediabox */
                         RenderTarget target=Target_View);
    virtual bool RenderPage(HDC hDC, RectI screenRect, int pageNo, float zoom, int rotation=0,
                         RectD *pageRect=NULL, RenderTarget target=Target_View);

    virtual PointD Transform(PointD pt, int pageNo, float zoom, int rotation, bool inverse=false);
    virtual RectD Transform(RectD rect, int pageNo, float zoom, int rotation, bool inverse=false);

    virtual unsigned char *GetFileData(size_t *cbCount);

    virtual TCHAR * ExtractPageText(int pageNo, TCHAR *lineSep, RectI **coords_out=NULL,
                                    RenderTarget target=Target_View) {
        return NULL;
    }

    virtual bool IsImagePage(int pageNo) { return false; }
    virtual PageLayoutType PreferredLayout() { return Layout_Single; }

    virtual bool HasTocTree() const {
        return false;
    }

    virtual const TCHAR *GetDefaultFileExt() const { return _T(".epub"); }

    virtual bool BenchLoadPage(int pageNo) { return true; }

protected:
    const TCHAR *fileName;
    EpubDoc *doc;
    Vec<PageData *> *pages;
    // needed so that memory allocated by ResolveHtmlEntities isn't leaked
    PoolAllocator allocator;
    RectD pageRect;

    static const int PAGE_BORDER = (int)(0.4 * 96);

    bool Load(const TCHAR *fileName);
    void GetTransform(Matrix& m, float zoom, int rotation);
};

EpubEngineImpl::EpubEngineImpl() : fileName(NULL), doc(NULL), pages(NULL),
    pageRect(0, 0, 5.12 * 96, 7.8 * 96) // "B Format" paperback
{
}

EpubEngineImpl::~EpubEngineImpl()
{
    delete doc;
    if (pages)
        DeleteVecMembers(*pages);
    delete pages;
    free((void *)fileName);
}

bool EpubEngineImpl::Load(const TCHAR *fileName)
{
    this->fileName = str::Dup(fileName);

    doc = new EpubDoc(fileName);
    if (!doc || !doc->Load())
        return false;

    LayoutInfo li;
    li.htmlStr = doc->GetBookHtmlData(li.htmlStrLen);
    li.pageDx = (int)pageRect.dx - 2 * PAGE_BORDER;
    li.pageDy = (int)pageRect.dy - 2 * PAGE_BORDER;
    li.textAllocator = &allocator;

    li.mobiDoc = (MobiDoc *)doc; // hack to allow passing doc through PageLayout
    li.fontName = L"Georgia";
    li.fontSize = 11;

    pages = PageLayout2(&li).Layout();

    return true;
}

void EpubEngineImpl::GetTransform(Matrix& m, float zoom, int rotation)
{
    rotation = rotation % 360;
    if (rotation < 0) rotation = rotation + 360;
    if (90 == rotation)
        m.Translate(0, (REAL)-pageRect.dy, MatrixOrderAppend);
    else if (180 == rotation)
        m.Translate((REAL)-pageRect.dx, (REAL)-pageRect.dy, MatrixOrderAppend);
    else if (270 == rotation)
        m.Translate((REAL)-pageRect.dx, 0, MatrixOrderAppend);
    else // if (0 == rotation)
        m.Translate(0, 0, MatrixOrderAppend);

    m.Scale(zoom, zoom, MatrixOrderAppend);
    m.Rotate((REAL)rotation, MatrixOrderAppend);
}

PointD EpubEngineImpl::Transform(PointD pt, int pageNo, float zoom, int rotation, bool inverse)
{
    RectD rect = Transform(RectD(pt, SizeD()), pageNo, zoom, rotation, inverse);
    return PointD(rect.x, rect.y);
}

RectD EpubEngineImpl::Transform(RectD rect, int pageNo, float zoom, int rotation, bool inverse)
{
    PointF pts[2] = {
        PointF((REAL)rect.x, (REAL)rect.y),
        PointF((REAL)(rect.x + rect.dx), (REAL)(rect.y + rect.dy))
    };
    Matrix m;
    GetTransform(m, zoom, rotation);
    if (inverse)
        m.Invert();
    m.TransformPoints(pts, 2);
    return RectD::FromXY(pts[0].X, pts[0].Y, pts[1].X, pts[1].Y);
}

RenderedBitmap *EpubEngineImpl::RenderBitmap(int pageNo, float zoom, int rotation, RectD *pageRect, RenderTarget target)
{
    RectD pageRc = pageRect ? *pageRect : PageMediabox(pageNo);
    RectI screen = Transform(pageRc, pageNo, zoom, rotation).Round();
    screen.Offset(-screen.x, -screen.y);

    HDC hDC = GetDC(NULL);
    HDC hDCMem = CreateCompatibleDC(hDC);
    HBITMAP hbmp = CreateCompatibleBitmap(hDC, screen.dx, screen.dy);
    DeleteObject(SelectObject(hDCMem, hbmp));

    bool ok = RenderPage(hDCMem, screen, pageNo, zoom, rotation, pageRect, target);
    DeleteDC(hDCMem);
    ReleaseDC(NULL, hDC);
    if (!ok) {
        DeleteObject(hbmp);
        return NULL;
    }

    return new RenderedBitmap(hbmp, screen.Size());
}

bool EpubEngineImpl::RenderPage(HDC hDC, RectI screenRect, int pageNo, float zoom, int rotation, RectD *pageRect, RenderTarget target)
{
    if (pageNo < 1 || PageCount() < pageNo)
        return false;
    PageData *pageData = pages->At(pageNo - 1);

    RectD pageRc = pageRect ? *pageRect : PageMediabox(pageNo);
    RectI screen = Transform(pageRc, pageNo, zoom, rotation).Round();

    Graphics g(hDC);
    g.SetCompositingQuality(CompositingQualityHighQuality);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPageUnit(UnitPixel);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    Color white(0xFF, 0xFF, 0xFF);
    Rect screenR(screenRect.x, screenRect.y, screenRect.dx, screenRect.dy);
    g.SetClip(screenR);
    g.FillRectangle(&SolidBrush(white), screenR);

    Matrix m;
    GetTransform(m, zoom, rotation);
    m.Translate((REAL)(screenRect.x - screen.x), (REAL)(screenRect.y - screen.y), MatrixOrderAppend);
    g.SetTransform(&m);

    DrawPageLayout(&g, &pageData->instructions, (REAL)PAGE_BORDER, (REAL)PAGE_BORDER, false);
    return true;
}

unsigned char *EpubEngineImpl::GetFileData(size_t *cbCount)
{
    return (unsigned char *)file::ReadAll(fileName, cbCount);
}

bool EpubEngine::IsSupportedFile(const TCHAR *fileName, bool sniff)
{
    return EpubDoc::IsSupportedFile(fileName, sniff);
}

EpubEngine *EpubEngine::CreateFromFile(const TCHAR *fileName)
{
    EpubEngineImpl *engine = new EpubEngineImpl();
    if (!engine->Load(fileName)) {
        delete engine;
        return NULL;
    }
    return engine;
}
