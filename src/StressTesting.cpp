/* Copyright 2006-2011 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "BaseUtil.h"
#include <time.h>
#include "WinUtil.h"
#include "FileUtil.h"
#include "SimpleLog.h"

#include "StressTesting.h"
#include "PdfEngine.h"
#include "DjVuEngine.h"
#include "WindowInfo.h"
#include "AppTools.h"
#include "RenderCache.h"
#include "SumatraPDF.h"

static Log::Logger *gLog;
#define logbench(msg, ...) gLog->LogFmt(_T(msg), __VA_ARGS__)
// #define logbench(msg, ...) Log::LogFmt(_T(msg), __VA_ARGS__)

static void BenchLoadRender(BaseEngine *engine, int pagenum)
{
    MillisecondTimer t;

    t.Start();
    bool ok = engine->BenchLoadPage(pagenum);
    t.Stop();

    if (!ok) {
        logbench("Error: failed to load page %d", pagenum);
        return;
    }
    double timems = t.GetTimeInMs();
    logbench("pageload   %3d: %.2f ms", pagenum, timems);

    t.Start();
    RenderedBitmap *rendered = engine->RenderBitmap(pagenum, 1.0, 0);
    t.Stop();

    if (!rendered) {
        logbench("Error: failed to render page %d", pagenum);
        return;
    }
    delete rendered;
    timems = t.GetTimeInMs();
    logbench("pagerender %3d: %.2f ms", pagenum, timems);
}

// <s> can be in form "1" or "3-58". If the range is followed
// by a comma, that's skipped. The end of the parsed string
// is returned (or NULL in case of a parsing error).
static const TCHAR *GetRange(const TCHAR *s, int *start, int *end)
{
    const TCHAR *next = str::Parse(s, _T("%d-%d%?,"), start, end);
    if (!next) {
        next = str::Parse(s, _T("%d%?,"), start);
        if (next)
            *end = *start;
    }
    return next;
}

// <s> can be:
// * "loadonly"
// * description of page ranges e.g. "1", "1-5", "2-3,6,8-10"
bool IsBenchPagesInfo(const TCHAR *s)
{
    if (str::IsEmpty(s))
        return false;
    if (str::EqI(s, _T("loadonly")))
        return true;

    while (!str::IsEmpty(s)) {
        int start, end;
        s = GetRange(s, &start, &end);
        if (!s || start < 0 || end < 0 || start > end)
            return false;
    }

    return true;
}

static void BenchFile(TCHAR *filePath, const TCHAR *pagesSpec)
{
    if (!file::Exists(filePath)) {
        logbench("Error: file %s doesn't exist", filePath);
        return;
    }

    MillisecondTimer total;
    total.Start();

    logbench("Starting: %s", filePath);

    MillisecondTimer t;
    BaseEngine *engine;
    t.Start();
    if (XpsEngine::IsSupportedFile(filePath))
        engine = XpsEngine::CreateFromFileName(filePath);
    if (DjVuEngine::IsSupportedFile(filePath))
        engine = DjVuEngine::CreateFromFileName(filePath);
    else
        engine = PdfEngine::CreateFromFileName(filePath);
    t.Stop();

    if (!engine) {
        logbench("Error: failed to load %s", filePath);
        return;
    }

    double timems = t.GetTimeInMs();
    logbench("load: %.2f ms", timems);
    int pages = engine->PageCount();
    logbench("page count: %d", pages);

    if (NULL == pagesSpec) {
        for (int i = 1; i <= pages; i++)
            BenchLoadRender(engine, i);
    }

    assert(!pagesSpec || IsBenchPagesInfo(pagesSpec));
    while (!str::IsEmpty(pagesSpec)) {
        int start, end;
        pagesSpec = GetRange(pagesSpec, &start, &end);
        for (int j = start; j <= end; j++) {
            if (1 <= j && j <= pages)
                BenchLoadRender(engine, j);
        }
    }

    delete engine;
    total.Stop();

    logbench("Finished (in %.2f ms): %s", total.GetTimeInMs(), filePath);
}

void Bench(StrVec& filesToBench)
{
    gLog = new Log::StderrLogger();
    // Log::Initialize();
    // Log::StderrLogger logger;
    // Log::AddLogger(&logger);

    size_t n = filesToBench.Count() / 2;
    for (size_t i = 0; i < n; i++)
        BenchFile(filesToBench[2*i], filesToBench[2*i + 1]);

    delete gLog;
    // Log::RemoveLogger(&logger);
    // Log::Destroy();
}

inline bool IsSpecialDir(const TCHAR *s)
{
    return str::Eq(s, _T(".")) || str::Eq(s, _T(".."));
}

bool CollectPathsFromDirectory(const TCHAR *pattern, StrVec& paths, bool dirsInsteadOfFiles)
{
    ScopedMem<TCHAR> dirPath(path::GetDir(pattern));

    WIN32_FIND_DATA fdata;
    HANDLE hfind = FindFirstFile(pattern, &fdata);
    if (INVALID_HANDLE_VALUE == hfind)
        return false;

    do {
        bool append = !dirsInsteadOfFiles;
        if ((fdata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            append = dirsInsteadOfFiles && !IsSpecialDir(fdata.cFileName);
        if (append)
            paths.Append(path::Join(dirPath, fdata.cFileName));
    } while (FindNextFile(hfind, &fdata));
    FindClose(hfind);

    return paths.Count() > 0;
}

// return t1 - t2 in seconds
static int SystemTimeDiffInSecs(SYSTEMTIME& t1, SYSTEMTIME& t2)
{
    FILETIME ft1, ft2;
    SystemTimeToFileTime(&t1, &ft1);
    SystemTimeToFileTime(&t2, &ft2);
    return FileTimeDiffInSecs(ft1, ft2);
}

static int SecsSinceSystemTime(SYSTEMTIME& time)
{
    SYSTEMTIME currTime;    
    GetSystemTime(&currTime);
    return SystemTimeDiffInSecs(currTime, time);
}

static TCHAR *FormatTime(int totalSecs)
{
    int secs = totalSecs % 60;
    int totalMins = totalSecs / 60;
    int mins = totalMins % 60;
    int hrs = totalMins / 60;
    if (hrs > 0)
        return str::Format(_T("%d hrs %d mins %d secs"), hrs, mins, secs);
    if (mins > 0)
        return str::Format(_T("%d mins %d secs"), mins, secs);
    return str::Format(_T("%d secs"), secs);
}

void RandomIsOverGlyph(DisplayModel *dm, int pageNo)
{
    if (!dm->validPageNo(pageNo))
        pageNo = 1;
    if (!dm->validPageNo(pageNo))
        return;
    // try random position in the page
    int x = rand() % 640;
    int y = rand() % 480;
    dm->textSelection->IsOverGlyph(pageNo, x, y);
}

/* The idea of StressTest is to render a lot of PDFs sequentially, simulating
a human advancing one page at a time. This is mostly to run through a large number
of PDFs before a release to make sure we're crash proof. */

class StressTest : public CallbackFunc {
    WindowInfo *      win;
    RenderCache *     renderCache;
    MillisecondTimer  currPageRenderTime;
    int               currPage;
    int               pageForSearchStart;
    int               filesCount; // number of files processed so far

    SYSTEMTIME        stressStartTime;

    // current state of directory traversal
    StrVec            filesToOpen;
    StrVec            dirsToVisit;

    // for file stress
    int               repCount;
    TCHAR *           fileToTest;

    bool OpenDir(const TCHAR *dirPath);
    bool OpenFile(const TCHAR *fileName);

    bool GoToNextPage();
    bool GoToNextFile();

    void OnTimer();
    void TickTimer();
    void Finished();

public:
    StressTest(WindowInfo *win, RenderCache *renderCache) :
        win(win), renderCache(renderCache), filesCount(0), 
        repCount(0), fileToTest(NULL)
        { }
    virtual ~StressTest() {
        free(fileToTest);
    }

    char *GetLogInfo();
    void StartDirStress(const TCHAR *dirPath);
    void StartFileStress(const TCHAR *filePath, int repCount);

    virtual void Callback() { OnTimer(); }
};

bool StressTest::GoToNextPage()
{
    if (currPage >= win->dm->pageCount()) {
        if (GoToNextFile())
            return true;
        Finished();
        return false;
    }

    ++currPage;
    win->dm->goToPage(currPage, 0);
    double pageRenderTime = currPageRenderTime.GetCurrTimeInMs();
    ScopedMem<TCHAR> s(str::Format(_T("Page %d rendered in %d milliseconds"), currPage-1, (int)pageRenderTime));
    win->ShowNotification(s, true, false, NG_DIR_STRESS_PAGE_TIMING);
    currPageRenderTime.Start();

    // start text search when we're in the middle of the document, so that
    // search thread touches both pages that were already rendered and not yet
    // rendered
    // TODO: it would be nice to also randomize search starting page but the
    // current API doesn't make it easy
    if (currPage == pageForSearchStart) {
        // use text that is unlikely to be found, so that we search all pages
        win::SetText(win->hwndFindBox, _T("!z_yt"));
        FindTextOnThread(win);
    }
    return true;
}

bool StressTest::OpenDir(const TCHAR *dirPath)
{
    assert(filesToOpen.Count() == 0);

    ScopedMem<TCHAR> pattern(str::Format(_T("%s\\*.pdf"), dirPath));
    bool hasFiles = CollectPathsFromDirectory(pattern, filesToOpen);
    pattern.Set(str::Format(_T("%s\\*.xps"), dirPath));
    hasFiles |= CollectPathsFromDirectory(pattern, filesToOpen);
    pattern.Set(str::Format(_T("%s\\*.djvu"), dirPath));
    hasFiles |= CollectPathsFromDirectory(pattern, filesToOpen);
    // NTFS returns files sorted anyway, maybe an explicit randomization would be better, though
    filesToOpen.Sort();

    pattern.Set(str::Format(_T("%s\\*"), dirPath));
    bool hasSubDirs = CollectPathsFromDirectory(pattern, dirsToVisit, true);

    return hasFiles || hasSubDirs;
}

bool StressTest::OpenFile(const TCHAR *fileName)
{
    bool reuse = rand() % 3 != 1;
    WindowInfo *w = LoadDocument(fileName, NULL, true /* show */, reuse, true /* suppressPwdUI */);
    if (!w)
        return false;

    if (w == win) { // WindowInfo reused
        if (!win->dm)
            return false;
    } else if (!w->dm) { // new WindowInfo
        CloseWindow(w, false, true);
        return false;
    }

    // transfer ownership of dirStressTest object to a new window and close the
    // current one
    assert(this == win->dirStressTest);
    if (w != win) {
        WindowInfo *toClose = win;
        w->dirStressTest = win->dirStressTest;
        win->dirStressTest = NULL;
        win = w;
        CloseWindow(toClose, false);
    }

    win->dm->changeDisplayMode(DM_SINGLE_PAGE);
    win->dm->zoomTo(ZOOM_FIT_PAGE);
    win->dm->goToFirstPage();
    if (win->tocShow)
        win->HideTocBox();

    currPage = 1;
    currPageRenderTime.Start();
    ++filesCount;

    pageForSearchStart = (rand() % win->dm->pageCount()) + 1;

    int secs = SecsSinceSystemTime(stressStartTime);
    ScopedMem<TCHAR> tm(FormatTime(secs));
    ScopedMem<TCHAR> s(str::Format(_T("File %d: %s, time: %s"), filesCount, fileName, tm));
    win->ShowNotification(s, false, false, NG_DIR_STRESS_NEW_FILE);

    return true;
}

void StressTest::TickTimer()
{
    SetTimer(win->hwndCanvas, DIR_STRESS_TIMER_ID, USER_TIMER_MINIMUM, NULL);
}

void StressTest::OnTimer()
{
    KillTimer(win->hwndCanvas, DIR_STRESS_TIMER_ID);
    if (!win->dm)
        return;
    BitmapCacheEntry *entry = renderCache->Find(win->dm, currPage, win->dm->rotation());
    if (!entry) {
        // not sure how reliable renderCache.Find() is, don't wait more than
        // 3 seconds for a single page to be rendered
        double timeInMs = currPageRenderTime.GetCurrTimeInMs();
        if (timeInMs > (double)3 * 1000) {
            if (!GoToNextPage())
                return;
        }
    } else {
        if (!GoToNextPage())
            return;
    }
    RandomIsOverGlyph(win->dm, currPage); // as a bonus, try to trigger ...
    TickTimer();
}

bool StressTest::GoToNextFile()
{
    // a case of testing just one file repeateadly
    if (fileToTest) {
        if (--repCount < 0)
            return false;
        return OpenFile(fileToTest);
    }

    // a case of testing all files in a directory
    for (;;) {
        while (filesToOpen.Count() > 0) {
            ScopedMem<TCHAR> path(filesToOpen[0]);
            filesToOpen.RemoveAt(0);
            if (OpenFile(path))
                return true;
        }

        if (dirsToVisit.Count() == 0)
            return false;

        // Go to next directory
        ScopedMem<TCHAR> path(dirsToVisit[0]);
        dirsToVisit.RemoveAt(0);
        OpenDir(path);
    }
}

void StressTest::Finished()
{
    win->dirStressTest = NULL;

    int secs = SecsSinceSystemTime(stressStartTime);
    ScopedMem<TCHAR> tm(FormatTime(secs));
    ScopedMem<TCHAR> s(str::Format(_T("Stress test complete, rendered %d files in %s"), filesCount, tm));
    win->ShowNotification(s, false, false, NG_DIR_STRESS_NEW_FILE);
    CloseWindow(win, false);
    delete this;
}

char *StressTest::GetLogInfo()
{
    int secs = SecsSinceSystemTime(stressStartTime);
    ScopedMem<TCHAR> st(FormatTime(secs));
    ScopedMem<char> su(str::conv::ToUtf8(st));
    return str::Format(", stress test rendered %d files in %s, currPage: %d", filesCount, su, currPage);
}

void StressTest::StartDirStress(const TCHAR *dirPath)
{
    srand((unsigned int)time(NULL));
    GetSystemTime(&stressStartTime);

    if (!dir::Exists(dirPath) || !OpenDir(dirPath)) {
        // Note: dev only, don't translate
        ScopedMem<TCHAR> s(str::Format(_T("Directory '%s' doesn't exist or is empty"), dirPath));
        win->ShowNotification(s, false /* autoDismiss */, false, NG_ERROR);
        Finished();
        return;
    }
    if (GoToNextFile())
        TickTimer();
    else
        Finished();
}

void StressTest::StartFileStress(const TCHAR *filePath, int repCount)
{
    srand((unsigned int)time(NULL));
    GetSystemTime(&stressStartTime);

    if (!file::Exists(filePath)) {
        // Note: dev only, don't translate
        ScopedMem<TCHAR> s(str::Format(_T("File '%s' doesn't exist or is empty"), filePath));
        win->ShowNotification(s, false /* autoDismiss */, false, NG_ERROR);
        Finished();
        return;
    }
    fileToTest = str::Dup(filePath);
    this->repCount = repCount;
    if (GoToNextFile())
        TickTimer();
    else
        Finished();
}

char *GetStressTestInfo(StressTest *dst)
{
    return dst->GetLogInfo();
}

void StartDirStressTest(WindowInfo *win, const TCHAR *dir, RenderCache *renderCache)
{
    // dst will be deleted when the stress ends
    StressTest *dst = new StressTest(win, renderCache);
    win->dirStressTest = dst;
    dst->StartDirStress(dir);
}

void StartFileStressTest(WindowInfo *win, const TCHAR *filePath, RenderCache *renderCache, int repCount)
{
    // dst will be deleted when the stress ends
    StressTest *dst = new StressTest(win, renderCache);
    win->dirStressTest = dst;
    dst->StartFileStress(filePath, repCount);
}

