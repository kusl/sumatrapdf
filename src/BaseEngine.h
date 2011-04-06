/* Copyright 2006-2011 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#ifndef BaseEngine_h
#define BaseEngine_h

#include "BaseUtil.h"
#include "GeomUtil.h"

#define DOS_NEWLINE _T("\r\n")

/* certain OCGs will only be rendered for some of these (e.g. watermarks) */
enum RenderTarget { Target_View, Target_Print, Target_Export };

enum PageLayoutType { Layout_Single = 0, Layout_Facing = 1, Layout_Book = 2, Layout_R2L = 16 };

class RenderedBitmap {
public:
    bool outOfDate;

    RenderedBitmap(HBITMAP hbmp, int width, int height) :
        _hbmp(hbmp), _width(width), _height(height), outOfDate(false) { }
    ~RenderedBitmap() { DeleteObject(_hbmp); }

    // callers must not delete this (use CopyImage if you have to modify it)
    HBITMAP getBitmap() const { return _hbmp; }
    SizeI size() const { return SizeI(_width, _height); }

    void stretchDIBits(HDC hdc, int leftMargin, int topMargin, int pageDx, int pageDy) {
        HDC bmpDC = CreateCompatibleDC(hdc);
        HGDIOBJ oldBmp = SelectObject(bmpDC, _hbmp);
        SetStretchBltMode(hdc, HALFTONE);
        StretchBlt(hdc, leftMargin, topMargin, pageDx, pageDy,
            bmpDC, 0, 0, _width, _height, SRCCOPY);
        SelectObject(bmpDC, oldBmp);
        DeleteDC(bmpDC);
    }

    void grayOut(float alpha) {
        HDC hDC = GetDC(NULL);
        BITMAPINFO bmi = { 0 };

        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biHeight = _height;
        bmi.bmiHeader.biWidth = _width;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        unsigned char *bmpData = (unsigned char *)malloc(_width * _height * 4);
        if (GetDIBits(hDC, _hbmp, 0, _height, bmpData, &bmi, DIB_RGB_COLORS)) {
            int dataLen = _width * _height * 4;
            for (int i = 0; i < dataLen; i++)
                if ((i + 1) % 4) // don't affect the alpha channel
                    bmpData[i] = (unsigned char)(bmpData[i] * alpha + (alpha > 0 ? 0 : 255));
            SetDIBits(hDC, _hbmp, 0, _height, bmpData, &bmi, DIB_RGB_COLORS);
        }

        free(bmpData);
        ReleaseDC(NULL, hDC);
    }
    void invertColors() { grayOut(-1); }

protected:
    HBITMAP _hbmp;
    int     _width;
    int     _height;
};

class BaseEngine {
public:
    virtual ~BaseEngine() { }
    // creates a clone of this engine (e.g. for printing on a different thread)
    virtual BaseEngine *Clone() = 0;

    // the name of the file this engine handles
    virtual const TCHAR *FileName() const = 0;
    // number of pages the loaded document contains
    virtual int PageCount() const = 0;

    // the angle in degrees the given page is rotated natively (usually 0 deg)
    virtual int PageRotation(int pageNo) { return 0; }
    // the dimensions of the given page (shortcut for PageMediabox(pageNo).Size())
    SizeD PageSize(int pageNo) {
        assert(1 <= pageNo && pageNo <= PageCount());
        return PageMediabox(pageNo).Size();
    }
    // the box into which a page will be drawn (usually RectD(0, 0, pageWidth, pageHeight))
    virtual RectD PageMediabox(int pageNo) = 0;
    // the box inside PageMediabox that actually contains any relevant content
    // (used for auto-cropping in Fit Content mode, can be PageMediabox)
    virtual RectI PageContentBox(int pageNo, RenderTarget target=Target_View) {
        return PageMediabox(pageNo).Convert<int>();
    }

    // renders a page into a cacheable RenderedBitmap
    virtual RenderedBitmap *RenderBitmap(int pageNo, float zoom, int rotation,
                         RectD *pageRect=NULL, /* if NULL: defaults to the page's mediabox */
                         RenderTarget target=Target_View, bool useGdi=false) = 0;
    // renders a page directly into an hDC (e.g. for printing)
    virtual bool RenderPage(HDC hDC, int pageNo, RectI screenRect,
                         float zoom=0, int rotation=0,
                         RectD *pageRect=NULL, RenderTarget target=Target_View) = 0;

    // applies zoom and rotation to a point in user/page space
    // or in the inverse direction from screen to user/page space
    virtual PointD Transform(PointD pt, int pageNo, float zoom, int rotate, bool inverse=false) = 0;
    virtual RectD Transform(RectD rect, int pageNo, float zoom, int rotate, bool inverse=false) = 0;

    // returns the binary data for the current file
    // (e.g. for saving again when the file has already been deleted)
    // caller needs to free() the result
    virtual unsigned char *GetFileData(size_t *cbCount) { return NULL; }
    // extracts all text found in the given page (and optionally also the
    // coordinates of the individual glyphs)
    // caller needs to free() the result
    virtual TCHAR * ExtractPageText(int pageNo, TCHAR *lineSep=DOS_NEWLINE, RectI **coords_out=NULL, RenderTarget target=Target_View) = 0;
    // certain optimizations can be made for a page consisting of a single large image
    virtual bool IsImagePage(int pageNo) = 0;
    // the layout type this document's author suggests (if the user doesn't care)
    virtual PageLayoutType PreferredLayout() { return Layout_Single; }

    // TODO: needs a more general interface
    // whether it is allowed to print the current document
    virtual bool IsPrintingAllowed() { return true; }
    // whether it is allowed to extract text from the current document
    // (except for searching an accessibility reasons)
    virtual bool IsCopyingTextAllowed() { return true; }

    // the DPI for a file is needed when converting internal measures to physical ones
    virtual float GetFileDPI() const { return 96.0f; }

    // loads the given page so that the time required can be measured
    // without also measuring rendering times
    virtual bool _benchLoadPage(int pageNo) = 0;
};

#endif
