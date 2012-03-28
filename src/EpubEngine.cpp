/* Copyright 2012 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Test engines to see how well the BaseEngine API fits flowed ebook formats
// (pages are layed out the same as for a "B Format" paperback: 5.12" x 7.8")

#include "EpubEngine.h"
#include "Scoped.h"
#include "Allocator.h"
#include "StrUtil.h"
#include "FileUtil.h"
#include "ZipUtil.h"
#include "MiniMui.h"
#include "GdiPlusUtil.h"
#include "TrivialHtmlParser.h"
#include "HtmlPullParser.h"
#include "PageLayout.h"
#include "EpubDoc.h"

// disable warning C4250 which is wrongly issued due to a compiler bug; cf.
// http://connect.microsoft.com/VisualStudio/feedback/details/101259/disable-warning-c4250-class1-inherits-class2-member-via-dominance-when-weak-member-is-a-pure-virtual-function
#pragma warning( disable: 4250 ) /* 'class1' : inherits 'class2::member' via dominance */

/* common classes for EPUB, FictionBook2, Mobi and CHM engines */

namespace str {
    namespace conv {

inline TCHAR *FromUtf8N(const char *s, size_t len)
{
    ScopedMem<char> tmp(str::DupN(s, len));
    return str::conv::FromUtf8(tmp);
}

    }
}

inline bool IsExternalUrl(const TCHAR *url)
{
    return str::FindChar(url, ':') != NULL;
}

struct PageAnchor {
    DrawInstr *instr;
    int pageNo;

    PageAnchor(DrawInstr *instr=NULL, int pageNo=-1) : instr(instr), pageNo(pageNo) { }
};

class EbookEngine : public virtual BaseEngine {
public:
    EbookEngine();
    virtual ~EbookEngine();

    virtual const TCHAR *FileName() const { return fileName; };
    virtual int PageCount() const { return pages ? pages->Count() : 0; }

    virtual RectD PageMediabox(int pageNo) { return pageRect; }
    virtual RectD PageContentBox(int pageNo, RenderTarget target=Target_View) {
        RectD mbox = PageMediabox(pageNo);
        mbox.Inflate(-pageBorder, -pageBorder);
        return mbox;
    }

    virtual RenderedBitmap *RenderBitmap(int pageNo, float zoom, int rotation,
                         RectD *pageRect=NULL, /* if NULL: defaults to the page's mediabox */
                         RenderTarget target=Target_View);
    virtual bool RenderPage(HDC hDC, RectI screenRect, int pageNo, float zoom, int rotation=0,
                         RectD *pageRect=NULL, RenderTarget target=Target_View);

    virtual PointD Transform(PointD pt, int pageNo, float zoom, int rotation, bool inverse=false);
    virtual RectD Transform(RectD rect, int pageNo, float zoom, int rotation, bool inverse=false);

    virtual unsigned char *GetFileData(size_t *cbCount) {
        return fileName ? (unsigned char *)file::ReadAll(fileName, cbCount) : NULL;
    }
    virtual TCHAR * ExtractPageText(int pageNo, TCHAR *lineSep, RectI **coords_out=NULL,
                                    RenderTarget target=Target_View);
    // make RenderCache request larger tiles than per default
    virtual bool HasClipOptimizations(int pageNo) { return false; }
    virtual PageLayoutType PreferredLayout() { return Layout_Book; }

    virtual Vec<PageElement *> *GetElements(int pageNo);
    virtual PageElement *GetElementAtPos(int pageNo, PointD pt);

    virtual PageDestination *GetNamedDest(const TCHAR *name);

    virtual bool BenchLoadPage(int pageNo) { return true; }

protected:
    const TCHAR *fileName;
    Vec<PageData *> *pages;
    Vec<PageAnchor> anchors;
    // contains for each page the last anchor indicating
    // a break between two merged documents
    Vec<DrawInstr *> baseAnchors;
    // needed so that memory allocated by ResolveHtmlEntities isn't leaked
    PoolAllocator allocator;
    // needed since pages::IterStart/IterNext aren't thread-safe
    CRITICAL_SECTION pagesAccess;
    // needed to undo the DPI specific UnitPoint-UnitPixel conversion
    int currFontDpi;

    RectD pageRect;
    float pageBorder;

    void GetTransform(Matrix& m, float zoom, int rotation) {
        GetBaseTransform(m, RectF(0, 0, (REAL)pageRect.dx, (REAL)pageRect.dy),
                         zoom, rotation);
    }
    bool ExtractPageAnchors();
    void FixFontSizeForResolution(HDC hDC);
    PageElement *CreatePageLink(DrawInstr *link, RectI rect, int pageNo);

    Vec<DrawInstr> *GetPageData(int pageNo) {
        CrashIf(pageNo < 1 || PageCount() < pageNo);
        if (pageNo < 1 || PageCount() < pageNo)
            return NULL;
        return &pages->At(pageNo - 1)->instructions;
    }
};

class SimpleDest2 : public PageDestination {
    int pageNo;
    RectD rect;
    ScopedMem<TCHAR> value;

public:
    SimpleDest2(int pageNo, RectD rect, TCHAR *value=NULL) :
        pageNo(pageNo), rect(rect), value(value) { }

    virtual const char *GetDestType() const { return value ? "LaunchURL" : "ScrollTo"; }
    virtual int GetDestPageNo() const { return pageNo; }
    virtual RectD GetDestRect() const { return rect; }
    virtual TCHAR *GetDestValue() const { return value ? str::Dup(value) : NULL; }
};

class EbookLink : public PageElement, public PageDestination {
    PageDestination *dest; // required for internal links, NULL for external ones
    DrawInstr *link; // owned by *EngineImpl::pages
    RectI rect;
    int pageNo;

public:
    EbookLink() : dest(NULL), link(NULL), pageNo(-1) { }
    EbookLink(DrawInstr *link, RectI rect, PageDestination *dest, int pageNo=-1) :
        link(link), rect(rect), dest(dest), pageNo(pageNo) { }
    virtual ~EbookLink() { delete dest; }

    virtual PageElementType GetType() const { return Element_Link; }
    virtual int GetPageNo() const { return pageNo; }
    virtual RectD GetRect() const { return rect.Convert<double>(); }
    virtual TCHAR *GetValue() const {
        if (!dest)
            return str::conv::FromUtf8N(link->str.s, link->str.len);
        return NULL;
    }
    virtual PageDestination *AsLink() { return dest ? dest : this; }

    virtual const char *GetDestType() const { return "LaunchURL"; }
    virtual int GetDestPageNo() const { return 0; }
    virtual RectD GetDestRect() const { return RectD(); }
    virtual TCHAR *GetDestValue() const { return GetValue(); }
};

class ImageDataElement : public PageElement {
    int pageNo;
    ImageData *id; // owned by *EngineImpl::pages
    RectI bbox;

public:
    ImageDataElement(int pageNo, ImageData *id, RectI bbox) :
        pageNo(pageNo), id(id), bbox(bbox) { }

    virtual PageElementType GetType() const { return Element_Image; }
    virtual int GetPageNo() const { return pageNo; }
    virtual RectD GetRect() const { return bbox.Convert<double>(); }
    virtual TCHAR *GetValue() const { return NULL; }

    virtual RenderedBitmap *GetImage() {
        HBITMAP hbmp;
        Bitmap *bmp = BitmapFromData(id->data, id->len);
        if (!bmp || bmp->GetHBITMAP(Color::White, &hbmp) != Ok) {
            delete bmp;
            return NULL;
        }
        SizeI size(bmp->GetWidth(), bmp->GetHeight());
        delete bmp;
        return new RenderedBitmap(hbmp, size);
    }
};

class EbookTocItem : public DocTocItem {
    PageDestination *dest;

public:
    EbookTocItem(TCHAR *title, PageDestination *dest) :
        DocTocItem(title, dest ? dest->GetDestPageNo() : 0), dest(dest) { }
    ~EbookTocItem() { delete dest; }

    virtual PageDestination *GetLink() { return dest; }
};

EbookEngine::EbookEngine() : fileName(NULL), pages(NULL),
    pageRect(0, 0, 5.12 * GetFileDPI(), 7.8 * GetFileDPI()), // "B Format" paperback
    pageBorder(0.4f * GetFileDPI()), currFontDpi(96)
{
    InitializeCriticalSection(&pagesAccess);
}

EbookEngine::~EbookEngine()
{
    EnterCriticalSection(&pagesAccess);

    if (pages)
        DeleteVecMembers(*pages);
    delete pages;
    free((void *)fileName);

    LeaveCriticalSection(&pagesAccess);
    DeleteCriticalSection(&pagesAccess);
}

bool EbookEngine::ExtractPageAnchors()
{
    ScopedCritSec scope(&pagesAccess);

    DrawInstr *baseAnchor = NULL;
    for (int pageNo = 1; pageNo <= PageCount(); pageNo++) {
        Vec<DrawInstr> *pageInstrs = GetPageData(pageNo);
        if (!pageInstrs)
            return false;

        for (size_t k = 0; k < pageInstrs->Count(); k++) {
            DrawInstr *i = &pageInstrs->At(k);
            if (InstrAnchor != i->type)
                continue;
            anchors.Append(PageAnchor(i, pageNo));
            if (k < 2 && str::StartsWith(i->str.s + i->str.len, "\" page_marker />"))
                baseAnchor = i;
        }
        baseAnchors.Append(baseAnchor);
    }

    CrashIf(baseAnchors.Count() != pages->Count());
    return true;
}

PointD EbookEngine::Transform(PointD pt, int pageNo, float zoom, int rotation, bool inverse)
{
    RectD rect = Transform(RectD(pt, SizeD()), pageNo, zoom, rotation, inverse);
    return PointD(rect.x, rect.y);
}

RectD EbookEngine::Transform(RectD rect, int pageNo, float zoom, int rotation, bool inverse)
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

RenderedBitmap *EbookEngine::RenderBitmap(int pageNo, float zoom, int rotation, RectD *pageRect, RenderTarget target)
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

void EbookEngine::FixFontSizeForResolution(HDC hDC)
{
    int dpi = GetDeviceCaps(hDC, LOGPIXELSY);
    if (dpi == currFontDpi)
        return;

    ScopedCritSec scope(&pagesAccess);

    float dpiFactor = 1.0f * currFontDpi / dpi;
    Graphics g(hDC);
    LOGFONTW lfw;

    for (int pageNo = 1; pageNo <= PageCount(); pageNo++) {
        Vec<DrawInstr> *pageInstrs = GetPageData(pageNo);
        for (DrawInstr *i = pageInstrs->IterStart(); i; i = pageInstrs->IterNext()) {
            if (InstrSetFont == i->type) {
                Status ok = i->font->GetLogFontW(&g, &lfw);
                if (Ok == ok) {
                    REAL newSize = i->font->GetSize() * dpiFactor;
                    FontStyle newStyle = (FontStyle)i->font->GetStyle();
                    i->font = mui::GetCachedFont(lfw.lfFaceName, newSize, newStyle);
                }
            }
        }
    }
    currFontDpi = dpi;
}

bool EbookEngine::RenderPage(HDC hDC, RectI screenRect, int pageNo, float zoom, int rotation, RectD *pageRect, RenderTarget target)
{
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
    screenR.Inflate(1, 1);
    g.FillRectangle(&SolidBrush(white), screenR);

    Matrix m;
    GetTransform(m, zoom, rotation);
    m.Translate((REAL)(screenRect.x - screen.x), (REAL)(screenRect.y - screen.y), MatrixOrderAppend);
    g.SetTransform(&m);

    ScopedCritSec scope(&pagesAccess);
    FixFontSizeForResolution(hDC);
    DrawPageLayout(&g, GetPageData(pageNo), pageBorder, pageBorder, false, &Color(Color::Black));
    return true;
}

static RectI GetInstrBbox(DrawInstr *instr, float pageBorder)
{
    RectT<float> bbox(instr->bbox.X, instr->bbox.Y, instr->bbox.Width, instr->bbox.Height);
    bbox.Offset(pageBorder, pageBorder);
    return bbox.Round();
}

TCHAR *EbookEngine::ExtractPageText(int pageNo, TCHAR *lineSep, RectI **coords_out, RenderTarget target)
{
    ScopedCritSec scope(&pagesAccess);

    str::Str<TCHAR> content;
    Vec<RectI> coords;
    bool insertSpace = false;

    Vec<DrawInstr> *pageInstrs = GetPageData(pageNo);
    for (DrawInstr *i = pageInstrs->IterStart(); i; i = pageInstrs->IterNext()) {
        RectI bbox = GetInstrBbox(i, pageBorder);
        switch (i->type) {
        case InstrString:
            if (coords.Count() > 0 && bbox.x <= coords.Last().BR().x) {
                content.Append(lineSep);
                coords.AppendBlanks(str::Len(lineSep));
                CrashIf(*lineSep && !coords.Last().IsEmpty());
            }
            else if (insertSpace && coords.Count() > 0) {
                int swidth = bbox.x - coords.Last().BR().x;
                if (swidth > 0) {
                    content.Append(' ');
                    coords.Append(RectI(bbox.x - swidth, bbox.y, swidth, bbox.dy));
                }
            }
            insertSpace = false;
            {
                ScopedMem<TCHAR> s(str::conv::FromUtf8N(i->str.s, i->str.len));
                content.Append(s);
                size_t len = str::Len(s);
                double cwidth = 1.0 * bbox.dx / len;
                for (size_t k = 0; k < len; k++)
                    coords.Append(RectI((int)(bbox.x + k * cwidth), bbox.y, (int)cwidth, bbox.dy));
            }
            break;
        case InstrElasticSpace:
        case InstrFixedSpace:
            insertSpace = true;
            break;
        }
    }

    if (coords_out) {
        CrashIf(coords.Count() != content.Count());
        *coords_out = new RectI[coords.Count()];
        memcpy(*coords_out, coords.LendData(), coords.Count() * sizeof(RectI));
    }
    return content.StealData();
}

PageElement *EbookEngine::CreatePageLink(DrawInstr *link, RectI rect, int pageNo)
{
    // internal links don't start with a protocol
    bool isInternal = !memchr(link->str.s, ':', link->str.len);
    if (!isInternal)
        return new EbookLink(link, rect, NULL, pageNo);

    ScopedMem<TCHAR> id;
    DrawInstr *baseAnchor = baseAnchors.At(pageNo-1);
    if (baseAnchor) {
        ScopedMem<char> basePath(str::DupN(baseAnchor->str.s, baseAnchor->str.len));
        ScopedMem<char> url(str::DupN(link->str.s, link->str.len));
        url.Set(NormalizeURL(url, basePath));
        id.Set(str::conv::FromUtf8(url));
    }
    else
        id.Set(str::conv::FromUtf8N(link->str.s, link->str.len));

    PageDestination *dest = GetNamedDest(id);
    if (!dest)
        return NULL;
    return new EbookLink(link, rect, dest, pageNo);
}

Vec<PageElement *> *EbookEngine::GetElements(int pageNo)
{
    Vec<PageElement *> *els = new Vec<PageElement *>();

    Vec<DrawInstr> *pageInstrs = GetPageData(pageNo);
    // CreatePageLink -> GetNamedDest might use pageInstrs->IterStart()
    for (size_t k = 0; k < pageInstrs->Count(); k++) {
        DrawInstr *i = &pageInstrs->At(k);
        if (InstrImage == i->type)
            els->Append(new ImageDataElement(pageNo, &i->img, GetInstrBbox(i, pageBorder)));
        else if (InstrLinkStart == i->type && !i->bbox.IsEmptyArea()) {
            PageElement *link = CreatePageLink(i, GetInstrBbox(i, pageBorder), pageNo);
            if (link)
                els->Append(link);
        }
    }

    return els;
}

PageElement *EbookEngine::GetElementAtPos(int pageNo, PointD pt)
{
    Vec<PageElement *> *els = GetElements(pageNo);
    if (!els)
        return NULL;

    PageElement *el = NULL;
    for (size_t i = 0; i < els->Count() && !el; i++)
        if (els->At(i)->GetRect().Contains(pt))
            el = els->At(i);

    if (el)
        els->Remove(el);
    DeleteVecMembers(*els);
    delete els;

    return el;
}

PageDestination *EbookEngine::GetNamedDest(const TCHAR *name)
{
    ScopedMem<char> name_utf8(str::conv::ToUtf8(name));
    const char *id = name_utf8;
    if (str::FindChar(id, '#'))
        id = str::FindChar(id, '#') + 1;

    // if the name consists of both path and ID,
    // try to first skip to the page with the desired
    // path before looking for the ID to allow
    // for the same ID to be reused on different pages
    DrawInstr *baseAnchor = NULL;
    int basePageNo = 0;
    if (id > name_utf8 + 1) {
        size_t base_len = id - name_utf8 - 1;
        for (size_t i = 0; i < baseAnchors.Count(); i++) {
            DrawInstr *anchor = baseAnchors.At(i);
            if (base_len == anchor->str.len &&
                str::EqNI(name_utf8, anchor->str.s, base_len)) {
                baseAnchor = anchor;
                basePageNo = (int)i + 1;
                break;
            }
        }
    }

    size_t id_len = str::Len(id);
    for (size_t i = 0; i < anchors.Count(); i++) {
        PageAnchor *anchor = &anchors.At(i);
        if (baseAnchor) {
            if (anchor->instr == baseAnchor)
                baseAnchor = NULL;
            continue;
        }
        // note: at least CHM treats URLs as case-independent
        if (id_len == anchor->instr->str.len &&
            str::EqNI(id, anchor->instr->str.s, id_len)) {
            RectD rect(0, anchor->instr->bbox.Y + pageBorder, pageRect.dx, 10);
            rect.Inflate(-pageBorder, 0);
            return new SimpleDest2(anchor->pageNo, rect);
        }
    }

    // don't fail if an ID doesn't exist in a merged document
    if (basePageNo != 0) {
        RectD rect(0, pageBorder, pageRect.dx, 10);
        rect.Inflate(-pageBorder, 0);
        return new SimpleDest2(basePageNo, rect);
    }

    return NULL;
}

static void AppendTocItem(EbookTocItem *& root, EbookTocItem *item, int level)
{
    if (!root) {
        root = item;
        return;
    }
    // find the last child at each level, until finding the parent of the new item
    DocTocItem *r2 = root;
    while (level-- > 0) {
        for (; r2->next; r2 = r2->next);
        if (r2->child)
            r2 = r2->child;
        else {
            r2->child = item;
            return;
        }
    }
    r2->AddSibling(item);
}

/* formatting extensions for EPUB */

class EpubFormatter : public HtmlFormatter {
protected:
    void HandleTagImg_Epub(HtmlToken *t);
    void HandleHtmlTag_Epub(HtmlToken *t);

    EpubDoc *epubDoc;
    ScopedMem<char> pagePath;

public:
    EpubFormatter(LayoutInfo *li, EpubDoc *doc) : HtmlFormatter(li), epubDoc(doc) { }

    Vec<PageData*> *FormatAllPages();
};

void EpubFormatter::HandleTagImg_Epub(HtmlToken *t)
{
    CrashIf(!epubDoc);
    if (t->IsEndTag())
        return;
    AttrInfo *attr = t->GetAttrByName("src");
    if (!attr)
        return;
    ScopedMem<char> src(str::DupN(attr->val, attr->valLen));
    ImageData2 *img = epubDoc->GetImageData(src, pagePath);
    if (img)
        EmitImage((ImageData *)img);
}

void EpubFormatter::HandleHtmlTag_Epub(HtmlToken *t)
{
    HtmlTag tag = FindTag(t);
    if (Tag_Img == tag) {
        HandleTagImg_Epub(t);
        HandleAnchorTag(t);
    }
    else if (Tag_Pagebreak == tag) {
        AttrInfo *attr = t->GetAttrByName("page_path");
        if (!attr || pagePath)
            ForceNewPage();
        if (attr) {
            RectF bbox(0, currY, pageDx, 0);
            currPage->instructions.Append(DrawInstr::Anchor(attr->val, attr->valLen, bbox));
            pagePath.Set(str::DupN(attr->val, attr->valLen));
        }
    }
    else
        HandleHtmlTag(t);
}

Vec<PageData*> *EpubFormatter::FormatAllPages()
{
    HtmlToken *t;
    while ((t = htmlParser->Next()) && !t->IsError()) {
        if (t->IsTag())
            HandleHtmlTag_Epub(t);
        else if (!IgnoreText())
            HandleText(t);
    }

    FlushCurrLine(true);
    UpdateLinkBboxes(currPage);
    pagesToSend.Append(currPage);
    currPage = NULL;

    Vec<PageData *> *result = new Vec<PageData *>(pagesToSend);
    pagesToSend.Reset();
    return result;
}

/* BaseEngine for handling EPUB documents */

class EpubEngineImpl : public EbookEngine, public EpubEngine {
    friend EpubEngine;

public:
    EpubEngineImpl() : EbookEngine(), doc(NULL) { }
    virtual ~EpubEngineImpl() { delete doc; }
    virtual EpubEngine *Clone() {
        return fileName ? CreateFromFile(fileName) : NULL;
    }

    virtual TCHAR *GetProperty(char *name) { return doc->GetProperty(name); }
    virtual const TCHAR *GetDefaultFileExt() const { return _T(".epub"); }

    virtual bool HasTocTree() const {
        return ScopedMem<char>(doc->GetTocData()) != NULL;
    }
    virtual DocTocItem *GetTocTree();

protected:
    EpubDoc *doc;

    bool Load(const TCHAR *fileName);
    bool Load(IStream *stream);
    bool FinishLoading();

    DocTocItem *BuildTocTree(HtmlPullParser& parser, int& idCounter);
};

bool EpubEngineImpl::Load(const TCHAR *fileName)
{
    this->fileName = str::Dup(fileName);
    doc = EpubDoc::CreateFromFile(fileName);
    return FinishLoading();
}

bool EpubEngineImpl::Load(IStream *stream)
{
    doc = EpubDoc::CreateFromStream(stream);
    return FinishLoading();
}

bool EpubEngineImpl::FinishLoading()
{
    if (!doc)
        return false;

    LayoutInfo li;
    li.htmlStr = doc->GetTextData(&li.htmlStrLen);
    li.pageDx = (int)(pageRect.dx - 2 * pageBorder);
    li.pageDy = (int)(pageRect.dy - 2 * pageBorder);
    li.fontName = L"Georgia";
    li.fontSize = 11;
    li.textAllocator = &allocator;

    pages = EpubFormatter(&li, doc).FormatAllPages();
    if (!ExtractPageAnchors())
        return false;

    return pages->Count() > 0;
}

DocTocItem *EpubEngineImpl::BuildTocTree(HtmlPullParser& parser, int& idCounter)
{
    ScopedMem<TCHAR> itemText, itemSrc;
    EbookTocItem *root = NULL;
    int level = -1;

    HtmlToken *tok;
    while ((tok = parser.Next()) && !tok->IsError() && (!tok->IsEndTag() || !tok->NameIs("navMap") && !tok->NameIs("ncx:navMap"))) {
        if (tok->IsTag() && (tok->NameIs("navPoint") || tok->NameIs("ncx:navPoint"))) {
            if (itemText) {
                PageDestination *dest;
                if (!itemSrc)
                    dest = NULL;
                else if (IsExternalUrl(itemSrc))
                    dest = new SimpleDest2(0, RectD(), itemSrc.StealData());
                else
                    dest = GetNamedDest(itemSrc);
                itemSrc.Set(NULL);
                EbookTocItem *item = new EbookTocItem(itemText.StealData(), dest);
                item->id = ++idCounter;
                item->open = level <= 1;
                AppendTocItem(root, item, level);
            }
            if (tok->IsStartTag())
                level++;
            else if (tok->IsEndTag())
                level--;
        }
        else if (tok->IsStartTag() && (tok->NameIs("text") || tok->NameIs("ncx:text"))) {
            tok = parser.Next();
            if (tok->IsText())
                itemText.Set(str::conv::FromUtf8N(tok->s, tok->sLen));
            else if (tok->IsError())
                break;
        }
        else if (tok->IsTag() && !tok->IsEndTag() && (tok->NameIs("content") || tok->NameIs("ncx:content"))) {
            AttrInfo *attrInfo = tok->GetAttrByName("src");
            if (attrInfo)
                itemSrc.Set(str::conv::FromUtf8N(attrInfo->val, attrInfo->valLen));
        }
    }

    return root;
}

DocTocItem *EpubEngineImpl::GetTocTree()
{
    ScopedMem<char> tocXml(doc->GetTocData());
    if (!tocXml)
        return NULL;

    HtmlPullParser parser(tocXml, str::Len(tocXml));
    HtmlToken *tok;
    // skip to the start of the navMap
    while ((tok = parser.Next()) && !tok->IsError()) {
        if (tok->IsStartTag() && (tok->NameIs("navMap") || tok->NameIs("ncx:navMap"))) {
            int idCounter = 0;
            return BuildTocTree(parser, idCounter);
        }
    }
    return NULL;
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

EpubEngine *EpubEngine::CreateFromStream(IStream *stream)
{
    EpubEngineImpl *engine = new EpubEngineImpl();
    if (!engine->Load(stream)) {
        delete engine;
        return NULL;
    }
    return engine;
}

/* formatting extensions for FictionBook2 */

class Fb2Formatter : public HtmlFormatter {
    int section;

    void HandleTagImg_Fb2(HtmlToken *t);
    void HandleTagAsHtml(HtmlToken *t, const char *name);
    void HandleFb2Tag(HtmlToken *t);

    Fb2Doc *fb2Doc;

public:
    Fb2Formatter(LayoutInfo *li, Fb2Doc *doc) :
        HtmlFormatter(li), fb2Doc(doc), section(1) { }

    Vec<PageData*> *FormatAllPages();
};

void Fb2Formatter::HandleTagImg_Fb2(HtmlToken *t)
{
    CrashIf(!fb2Doc);
    if (t->IsEndTag())
        return;
    AttrInfo *attr = t->GetAttrByName(fb2Doc->GetHrefName());
    if (!attr)
        return;
    ScopedMem<char> src(str::DupN(attr->val, attr->valLen));
    ImageData2 *img = fb2Doc->GetImageData(src);
    if (img)
        EmitImage((ImageData *)img);
}

void Fb2Formatter::HandleTagAsHtml(HtmlToken *t, const char *name)
{
    HtmlToken tok;
    tok.SetValue(t->type, name, name + str::Len(name));
    HandleHtmlTag(&tok);
}

void Fb2Formatter::HandleFb2Tag(HtmlToken *t)
{
    if (t->NameIs("title") || t->NameIs("subtitle")) {
        bool isSubtitle = t->NameIs("subtitle");
        ScopedMem<char> name(str::Format("h%d", section + (isSubtitle ? 1 : 0)));
        HtmlToken tok;
        tok.SetValue(t->type, name, name + str::Len(name));
        HandleTagHx(&tok);
        HandleAnchorTag(t);
    }
    else if (t->NameIs("section")) {
        if (t->IsStartTag())
            section++;
        else if (t->IsEndTag() && section > 1)
            section--;
        FlushCurrLine(true);
        HandleAnchorTag(t);
    }
    else if (t->NameIs("p")) {
        if (htmlParser->tagNesting.Find(Tag_Title) == -1)
            HandleHtmlTag(t);
    }
    else if (t->NameIs("image")) {
        HandleTagImg_Fb2(t);
        HandleAnchorTag(t);
    }
    else if (t->NameIs("a")) {
        HandleTagA(t, fb2Doc->GetHrefName());
        HandleAnchorTag(t, true);
    }
    else if (t->NameIs("pagebreak"))
        ForceNewPage();
    else if (t->NameIs("strong"))
        HandleTagAsHtml(t, "b");
    else if (t->NameIs("emphasis"))
        HandleTagAsHtml(t, "i");
    else if (t->NameIs("epigraph"))
        HandleTagAsHtml(t, "blockquote");
    else if (t->NameIs("empty-line")) {
        if (!t->IsEndTag())
            EmitParagraph(0);
    }
}

Vec<PageData*> *Fb2Formatter::FormatAllPages()
{
    HtmlToken *t;
    while ((t = htmlParser->Next()) && !t->IsError()) {
        if (t->IsTag())
            HandleFb2Tag(t);
        else
            HandleText(t);
    }

    FlushCurrLine(true);
    UpdateLinkBboxes(currPage);
    pagesToSend.Append(currPage);
    currPage = NULL;

    Vec<PageData *> *result = new Vec<PageData *>(pagesToSend);
    pagesToSend.Reset();
    return result;
}

/* BaseEngine for handling FictionBook2 documents */

class Fb2EngineImpl : public EbookEngine, public Fb2Engine {
    friend Fb2Engine;

public:
    Fb2EngineImpl() : EbookEngine(), doc(NULL) { }
    virtual ~Fb2EngineImpl() { delete doc; }
    virtual Fb2Engine *Clone() {
        return fileName ? CreateFromFile(fileName) : NULL;
    }

    virtual TCHAR *GetProperty(char *name) { return doc->GetProperty(name); }
    virtual const TCHAR *GetDefaultFileExt() const { return _T(".fb2"); }

protected:
    Fb2Doc *doc;

    bool Load(const TCHAR *fileName);
};

bool Fb2EngineImpl::Load(const TCHAR *fileName)
{
    this->fileName = str::Dup(fileName);

    doc = Fb2Doc::CreateFromFile(fileName);
    if (!doc)
        return false;

    LayoutInfo li;
    li.htmlStr = doc->GetTextData(&li.htmlStrLen);
    li.pageDx = (int)(pageRect.dx - 2 * pageBorder);
    li.pageDy = (int)(pageRect.dy - 2 * pageBorder);
    li.fontName = L"Georgia";
    li.fontSize = 11;
    li.textAllocator = &allocator;

    pages = Fb2Formatter(&li, doc).FormatAllPages();
    if (!ExtractPageAnchors())
        return false;

    return pages->Count() > 0;
}

bool Fb2Engine::IsSupportedFile(const TCHAR *fileName, bool sniff)
{
    return Fb2Doc::IsSupportedFile(fileName, sniff);
}

Fb2Engine *Fb2Engine::CreateFromFile(const TCHAR *fileName)
{
    Fb2EngineImpl *engine = new Fb2EngineImpl();
    if (!engine->Load(fileName)) {
        delete engine;
        return NULL;
    }
    return engine;
}

/* BaseEngine for handling Mobi documents (for reference testing) */

#include "MobiDoc.h"

class MobiEngineImpl : public EbookEngine, public MobiEngine {
    friend MobiEngine;

public:
    MobiEngineImpl() : EbookEngine(), doc(NULL), tocReparsePoint(NULL) { }
    virtual ~MobiEngineImpl() { delete doc; }
    virtual MobiEngine *Clone() {
        return fileName ? CreateFromFile(fileName) : NULL;
    }

    virtual PageDestination *GetNamedDest(const TCHAR *name);
    virtual bool HasTocTree() const { return tocReparsePoint != NULL; }
    virtual DocTocItem *GetTocTree();

    virtual const TCHAR *GetDefaultFileExt() const { return _T(".mobi"); }

protected:
    MobiDoc *doc;
    const char *tocReparsePoint;

    bool Load(const TCHAR *fileName);
};

bool MobiEngineImpl::Load(const TCHAR *fileName)
{
    this->fileName = str::Dup(fileName);

    doc = MobiDoc::CreateFromFile(fileName);
    if (!doc)
        return false;

    LayoutInfo li;
    li.htmlStr = doc->GetBookHtmlData(li.htmlStrLen);
    li.pageDx = (int)(pageRect.dx - 2 * pageBorder);
    li.pageDy = (int)(pageRect.dy - 2 * pageBorder);
    li.fontName = L"Georgia";
    li.fontSize = 11;
    li.textAllocator = &allocator;

    pages = MobiFormatter(&li, doc).FormatAllPages();
    if (!ExtractPageAnchors())
        return false;

    HtmlParser parser;
    if (parser.Parse(li.htmlStr)) {
        HtmlElement *ref = NULL;
        while ((ref = parser.FindElementByName("reference", ref))) {
            ScopedMem<TCHAR> type(ref->GetAttribute("type"));
            ScopedMem<TCHAR> filepos(ref->GetAttribute("filepos"));
            if (str::EqI(type, _T("toc")) && filepos) {
                unsigned int pos;
                if (str::Parse(filepos, _T("%u%$"), &pos) && pos < li.htmlStrLen) {
                    tocReparsePoint = li.htmlStr + pos;
                    break;
                }
            }
        }
    }

    return pages->Count() > 0;
}

PageDestination *MobiEngineImpl::GetNamedDest(const TCHAR *name)
{
    int filePos = _ttoi(name);
    if (filePos < 0 || 0 == filePos && *name != '0')
        return NULL;
    int pageNo;
    for (pageNo = 1; pageNo < PageCount(); pageNo++) {
        if (pages->At(pageNo)->reparseIdx > filePos)
            break;
    }
    CrashIf(pageNo < 1 || pageNo > PageCount());

    size_t htmlLen;
    char *start = doc->GetBookHtmlData(htmlLen);
    if ((size_t)filePos > htmlLen)
        return NULL;

    ScopedCritSec scope(&pagesAccess);
    Vec<DrawInstr> *pageInstrs = GetPageData(pageNo);
    // link to the bottom of the page, if filePos points
    // beyond the last visible DrawInstr of a page
    float currY = (float)pageRect.dy;
    for (DrawInstr *i = pageInstrs->IterStart(); i; i = pageInstrs->IterNext()) {
        if (InstrString == i->type && i->str.s >= start &&
            i->str.s <= start + htmlLen && i->str.s - start >= filePos) {
            currY = i->bbox.Y;
            break;
        }
    }
    RectD rect(0, currY + pageBorder, pageRect.dx, 10);
    rect.Inflate(-pageBorder, 0);
    return new SimpleDest2(pageNo, rect);
}

DocTocItem *MobiEngineImpl::GetTocTree()
{
    if (!tocReparsePoint)
        return NULL;

    EbookTocItem *root = NULL;
    ScopedMem<TCHAR> itemText;
    ScopedMem<TCHAR> itemLink;
    int itemLevel = 0;
    int idCounter = 0;

    // there doesn't seem to be a standard for Mobi ToCs, so we try to
    // determine the author's intentions by looking at commonly used tags
    HtmlPullParser parser(tocReparsePoint, str::Len(tocReparsePoint));
    HtmlToken *tok;
    while ((tok = parser.Next()) && !tok->IsError()) {
        if (itemLink && tok->IsText()) {
            ScopedMem<TCHAR> linkText(str::conv::FromUtf8N(tok->s, tok->sLen));
            if (itemText)
                itemText.Set(str::Join(itemText, _T(" "), linkText));
            else
                itemText.Set(linkText.StealData());
        }
        else if (!tok->IsTag())
            continue;
        else if (tok->NameIs("mbp:pagebreak"))
            break;
        else if (!itemLink && tok->IsStartTag() && tok->NameIs("a")) {
            AttrInfo *attr = tok->GetAttrByName("filepos");
            if (!attr)
                attr = tok->GetAttrByName("href");
            if (attr)
                itemLink.Set(str::conv::FromUtf8N(attr->val, attr->valLen));
        }
        else if (itemLink && tok->IsEndTag() && tok->NameIs("a")) {
            PageDestination *dest = NULL;
            if (!itemText) {
                itemLink.Set(NULL);
                continue;
            }
            if (IsExternalUrl(itemLink))
                dest = new SimpleDest2(0, RectD(), itemLink.StealData());
            else
                dest = GetNamedDest(itemLink);
            EbookTocItem *item = new EbookTocItem(itemText.StealData(), dest);
            item->id = ++idCounter;
            AppendTocItem(root, item, itemLevel);
            itemLink.Set(NULL);
        }
        else if (tok->NameIs("blockquote") || tok->NameIs("ul") || tok->NameIs("ol")) {
            if (tok->IsStartTag())
                itemLevel++;
            else if (tok->IsEndTag() && itemLevel > 0)
                itemLevel--;
        }
    }

    return root;
}

bool MobiEngine::IsSupportedFile(const TCHAR *fileName, bool sniff)
{
    return str::EndsWithI(fileName, _T(".mobi")) ||
           str::EndsWithI(fileName, _T(".azw"))  ||
           str::EndsWithI(fileName, _T(".prc"));
}

MobiEngine *MobiEngine::CreateFromFile(const TCHAR *fileName)
{
    MobiEngineImpl *engine = new MobiEngineImpl();
    if (!engine->Load(fileName)) {
        delete engine;
        return NULL;
    }
    return engine;
}

/* formatting extensions for CHM */

#include "ChmDoc.h"

class ChmDataCache {
    ChmDoc *doc; // owned by creator
    ScopedMem<char> html;
    Vec<ImageData2> images;

public:
    ChmDataCache(ChmDoc *doc, char *html) : doc(doc), html(html) { }
    ~ChmDataCache() {
        for (size_t i = 0; i < images.Count(); i++) {
            free(images.At(i).data);
            free(images.At(i).id);
        }
    }

    const char *GetTextData(size_t *lenOut) {
        *lenOut = html ? str::Len(html) : 0;
        return html;
    }

    ImageData2 *GetImageData(const char *id, const char *pagePath) {
        ScopedMem<char> url(NormalizeURL(id, pagePath));
        for (size_t i = 0; i < images.Count(); i++) {
            if (str::Eq(images.At(i).id, url))
                return &images.At(i);
        }

        ImageData2 data = { 0 };
        data.data = (char *)doc->GetData(url, &data.len);
        if (!data.data)
            return NULL;
        data.id = url.StealData();
        images.Append(data);
        return &images.Last();
    }
};

class ChmFormatter : public HtmlFormatter {
protected:
    void HandleTagImg_Chm(HtmlToken *t);
    void HandleHtmlTag_Chm(HtmlToken *t);

    ChmDataCache *chmDoc;
    ScopedMem<char> pagePath;

public:
    ChmFormatter(LayoutInfo *li, ChmDataCache *doc) : HtmlFormatter(li), chmDoc(doc) { }

    Vec<PageData*> *FormatAllPages();
};

void ChmFormatter::HandleTagImg_Chm(HtmlToken *t)
{
    CrashIf(!chmDoc);
    if (t->IsEndTag())
        return;
    AttrInfo *attr = t->GetAttrByName("src");
    if (!attr)
        return;
    ScopedMem<char> src(str::DupN(attr->val, attr->valLen));
    ImageData2 *img = chmDoc->GetImageData(src, pagePath);
    if (img)
        EmitImage((ImageData *)img);
}

void ChmFormatter::HandleHtmlTag_Chm(HtmlToken *t)
{
    HtmlTag tag = FindTag(t);
    if (Tag_Img == tag) {
        HandleTagImg_Chm(t);
        HandleAnchorTag(t);
    }
    else if (Tag_Pagebreak == tag) {
        AttrInfo *attr = t->GetAttrByName("page_path");
        if (!attr || pagePath)
            ForceNewPage();
        if (attr) {
            RectF bbox(0, currY, pageDx, 0);
            currPage->instructions.Append(DrawInstr::Anchor(attr->val, attr->valLen, bbox));
            pagePath.Set(str::DupN(attr->val, attr->valLen));
        }
    }
    else
        HandleHtmlTag(t);
}

Vec<PageData*> *ChmFormatter::FormatAllPages()
{
    HtmlToken *t;
    while ((t = htmlParser->Next()) && !t->IsError()) {
        if (t->IsTag())
            HandleHtmlTag_Chm(t);
        else if (!IgnoreText())
            HandleText(t);
    }

    FlushCurrLine(true);
    UpdateLinkBboxes(currPage);
    pagesToSend.Append(currPage);
    currPage = NULL;

    Vec<PageData *> *result = new Vec<PageData *>(pagesToSend);
    pagesToSend.Reset();
    return result;
}

/* BaseEngine for handling CHM documents */

class Chm2EngineImpl : public EbookEngine, public Chm2Engine {
    friend Chm2Engine;

public:
    Chm2EngineImpl() : EbookEngine(), doc(NULL), dataCache(NULL) {
        // ISO 216 A4 (210mm x 297mm)
        pageRect = RectD(0, 0, 8.27 * GetFileDPI(), 11.693 * GetFileDPI());
    }
    virtual ~Chm2EngineImpl() {
        delete dataCache;
        delete doc;
    }
    virtual Chm2Engine *Clone() {
        return fileName ? CreateFromFile(fileName) : NULL;
    }

    virtual TCHAR *GetProperty(char *name) { return doc->GetProperty(name); }
    virtual const TCHAR *GetDefaultFileExt() const { return _T(".chm"); }

    virtual PageLayoutType PreferredLayout() { return Layout_Single; }

    virtual bool HasTocTree() const { return doc->HasToc(); }
    virtual DocTocItem *GetTocTree();

protected:
    ChmDoc *doc;
    ChmDataCache *dataCache;

    bool Load(const TCHAR *fileName);

    DocTocItem *BuildTocTree(HtmlPullParser& parser, int& idCounter);
};

static TCHAR *ToPlainUrl(const TCHAR *url)
{
    TCHAR *plainUrl = str::Dup(url);
    str::TransChars(plainUrl, _T("#?"), _T("\0\0"));
    return plainUrl;
}

class ChmHtmlCollector : public ChmTocVisitor {
    ChmDoc *doc;
    StrVec added;
    str::Str<char> html;

public:
    ChmHtmlCollector(ChmDoc *doc) : doc(doc) { }

    char *GetHtml() {
        // first add the homepage
        const char *index = doc->GetIndexPath();
        ScopedMem<TCHAR> url(doc->ToStr(index));
        visit(NULL, url, 0);

        // then add all pages linked to from the table of contents
        doc->ParseToc(this);

        // finally add all the remaining HTML files
        Vec<char *> *paths = doc->GetAllPaths();
        for (size_t i = 0; i < paths->Count(); i++) {
            char *path = paths->At(i);
            if (str::EndsWithI(path, ".htm") || str::EndsWithI(path, ".html")) {
                if (*path == '/')
                    path++;
                url.Set(doc->ToStr(path));
                visit(NULL, url, -1);
            }
        }
        FreeVecMembers(*paths);
        delete paths;

        return html.StealData();
    }

    virtual void visit(const TCHAR *name, const TCHAR *url, int level) {
        if (!url || IsExternalUrl(url))
            return;
        ScopedMem<TCHAR> plainUrl(ToPlainUrl(url));
        if (added.FindI(plainUrl) != -1)
            return;
        ScopedMem<char> urlUtf8(str::conv::ToUtf8(plainUrl));
        // TODO: use the native codepage for the path to GetData
        ScopedMem<unsigned char> pageHtml(doc->GetData(urlUtf8, NULL));
        if (!pageHtml)
            return;
        html.AppendFmt("<pagebreak page_path=\"%s\" page_marker />", urlUtf8);
        html.AppendAndFree(doc->ToUtf8(pageHtml));
        added.Append(plainUrl.StealData());
    }
};

bool Chm2EngineImpl::Load(const TCHAR *fileName)
{
    this->fileName = str::Dup(fileName);
    doc = ChmDoc::CreateFromFile(fileName);
    if (!doc)
        return false;

    char *html = ChmHtmlCollector(doc).GetHtml();
    dataCache = new ChmDataCache(doc, html);

    LayoutInfo li;
    li.htmlStr = dataCache->GetTextData(&li.htmlStrLen);
    li.pageDx = (int)(pageRect.dx - 2 * pageBorder);
    li.pageDy = (int)(pageRect.dy - 2 * pageBorder);
    li.fontName = L"Georgia";
    li.fontSize = 11;
    li.textAllocator = &allocator;

    pages = ChmFormatter(&li, dataCache).FormatAllPages();
    if (!ExtractPageAnchors())
        return false;

    return pages->Count() > 0;
}

class Chm2TocBuilder : public ChmTocVisitor {
    ChmDoc *doc;
    Chm2Engine *engine;
    EbookTocItem *root;
    int idCounter;

public:
    Chm2TocBuilder(Chm2Engine *engine, ChmDoc *doc) :
        engine(engine), doc(doc), root(NULL), idCounter(0) { }

    EbookTocItem *GetTocRoot() {
        doc->ParseToc(this);
        return root;
    }

    virtual void visit(const TCHAR *name, const TCHAR *url, int level) {
        PageDestination *dest;
        if (!url)
            dest = NULL;
        else if (IsExternalUrl(url))
            dest = new SimpleDest2(0, RectD(), str::Dup(url));
        else
            dest = engine->GetNamedDest(url);

        EbookTocItem *item = new EbookTocItem(str::Dup(name), dest);
        item->id = ++idCounter;
        item->open = level == 1;
        AppendTocItem(root, item, level);
    }
};

DocTocItem *Chm2EngineImpl::GetTocTree()
{
    return Chm2TocBuilder(this, doc).GetTocRoot();
}

bool Chm2Engine::IsSupportedFile(const TCHAR *fileName, bool sniff)
{
    return ChmDoc::IsSupportedFile(fileName, sniff);
}

Chm2Engine *Chm2Engine::CreateFromFile(const TCHAR *fileName)
{
    Chm2EngineImpl *engine = new Chm2EngineImpl();
    if (!engine->Load(fileName)) {
        delete engine;
        return NULL;
    }
    return engine;
}
