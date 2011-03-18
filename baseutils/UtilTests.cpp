/* Copyright 2006-2011 the SumatraPDF project authors (see ../AUTHORS file).
   License: FreeBSD (see ./COPYING) */

#ifdef DEBUG

#include "BaseUtil.h"
#include "BencUtil.h"
#include "FileUtil.h"
#include "GeomUtil.h"
#include "StrUtil.h"
#include "Vec.h"
#include <time.h>

static void GeomTest()
{
    PointD ptD(12.4, -13.6);
    assert(ptD.x == 12.4 && ptD.y == -13.6);
    PointI ptI = ptD.Convert<int>();
    assert(ptI.x == 12 && ptI.y == -14);
    ptD = ptI.Convert<double>();
    assert(ptD.x == 12 && ptD.y == -14);

    SizeD szD(7.7, -3.3);
    assert(szD.dx == 7.7 && szD.dy == -3.3);
    SizeI szI = szD.Convert<int>();
    assert(szI.dx == 8 && szI.dy == -3);
    szD = szI.Convert<double>();
    assert(szD.dx == 8 && szD.dy == -3);

    struct SRIData {
        int     x1s, x1e, y1s, y1e;
        int     x2s, x2e, y2s, y2e;
        bool    intersect;
        int     i_xs, i_xe, i_ys, i_ye;
        int     u_xs, u_xe, u_ys, u_ye;
    } testData[] = {
        { 0,10, 0,10,   0,10, 0,10,  true,  0,10, 0,10,  0,10, 0,10 }, /* complete intersect */
        { 0,10, 0,10,  20,30,20,30,  false, 0, 0, 0, 0,  0,30, 0,30 }, /* no intersect */
        { 0,10, 0,10,   5,15, 0,10,  true,  5,10, 0,10,  0,15, 0,10 }, /* { | } | */
        { 0,10, 0,10,   5, 7, 0,10,  true,  5, 7, 0,10,  0,10, 0,10 }, /* { | | } */
        { 0,10, 0,10,   5, 7, 5, 7,  true,  5, 7, 5, 7,  0,10, 0,10 },
        { 0,10, 0,10,   5, 15,5,15,  true,  5,10, 5,10,  0,15, 0,15 },
    };

    for (size_t i = 0; i < dimof(testData); i++) {
        struct SRIData *curr = &testData[i];

        RectI rx1(curr->x1s, curr->y1s, curr->x1e - curr->x1s, curr->y1e - curr->y1s);
        RectI rx2 = RectI::FromXY(curr->x2s, curr->y2s, curr->x2e, curr->y2e);
        RectI isect = rx1.Intersect(rx2);
        if (curr->intersect) {
            assert(!isect.IsEmpty());
            assert(isect.x == curr->i_xs && isect.y == curr->i_ys);
            assert(isect.x + isect.dx == curr->i_xe && isect.y + isect.dy == curr->i_ye);
        }
        else {
            assert(isect.IsEmpty());
        }
        RectI urect = rx1.Union(rx2);
        assert(urect.x == curr->u_xs && urect.y == curr->u_ys);
        assert(urect.x + urect.dx == curr->u_xe && urect.y + urect.dy == curr->u_ye);

        /* if we swap rectangles, the results should be the same */
        swap(rx1, rx2);
        isect = rx1.Intersect(rx2);
        if (curr->intersect) {
            assert(!isect.IsEmpty());
            assert(isect.x == curr->i_xs && isect.y == curr->i_ys);
            assert(isect.x + isect.dx == curr->i_xe && isect.y + isect.dy == curr->i_ye);
        }
        else {
            assert(isect.IsEmpty());
        }
        urect = rx1.Union(rx2);
        assert(urect.x == curr->u_xs && urect.y == curr->u_ys);
        assert(urect.x + urect.dx == curr->u_xe && urect.y + urect.dy == curr->u_ye);

        assert(!rx1.Inside(PointI(-2, -2)));
        assert(rx1.Inside(rx1.TL()));
        assert(!rx1.Inside(PointI(rx1.x, INT_MAX)));
        assert(!rx1.Inside(PointI(INT_MIN, rx1.y)));
    }
}

static void TStrTest()
{
    TCHAR buf[32];
    TCHAR *str = _T("a string");
    assert(Str::Len(str) == 8);
    assert(Str::Eq(str, _T("a string")) && Str::Eq(str, str));
    assert(!Str::Eq(str, NULL) && !Str::Eq(str, _T("A String")));
    assert(Str::EqI(str, _T("A String")) && Str::EqI(str, str));
    assert(!Str::EqI(str, NULL) && Str::EqI((char*)NULL, (char*)NULL));
    assert(Str::StartsWith(str, _T("a s")) && Str::StartsWithI(str, _T("A Str")));
    assert(!Str::StartsWith(str, _T("Astr")));
    assert(Str::EndsWith(str, _T("ing")) && Str::EndsWithI(str, _T("ING")));
    assert(!Str::EndsWith(str, _T("ung")));
    assert(Str::IsEmpty((char*)NULL) && Str::IsEmpty((WCHAR*)NULL)&& Str::IsEmpty(_T("")) && !Str::IsEmpty(str));
    assert(Str::FindChar(str, _T('s')) && !Str::FindChar(str, _T('S')));
    size_t len = Str::BufSet(buf, dimof(buf), str);
    assert(len == Str::Len(buf) + 1 && Str::Eq(buf, str));
    len = Str::BufSet(buf, 6, str);
    assert(len == 6 && Str::Eq(buf, _T("a str")));

    str = Str::Dup(buf);
    assert(Str::Eq(str, buf));
    free(str);
    str = Str::DupN(buf, 4);
    assert(Str::Eq(str, _T("a st")));
    free(str);
    str = Str::Format(_T("%s"), buf);
    assert(Str::Eq(str, buf));
    free(str);
    str = Str::Join(buf, buf);
    assert(Str::Len(str) == 2 * Str::Len(buf));
    free(str);
    str = Str::Join(NULL, _T("ab"));
    assert(Str::Eq(str, _T("ab")));
    free(str);

    Str::BufSet(buf, dimof(buf), _T("abc\1efg\1"));
    size_t count = Str::TransChars(buf, _T("ace"), _T("ACE"));
    assert(Str::Eq(buf, _T("AbC\1Efg\1")) && count == 3);
    count = Str::TransChars(buf, _T("\1"), _T("\0"));
    assert(Str::Eq(buf, _T("AbC")) && Str::Eq(buf + 4, _T("Efg")) && count == 2);
    count = Str::TransChars(buf, _T(""), _T("X"));
    assert(Str::Eq(buf, _T("AbC")) && count == 0);

    Str::Parser parser;
    const TCHAR *pos = _T("[Open(\"filename.pdf\",0,1,0)]");
    assert(parser.Init(pos));
    assert(parser.Skip(_T("[Open(\"")));
    assert(parser.CopyUntil('"', buf, dimof(buf)));
    assert(Str::Eq(buf, _T("filename.pdf")));
    assert(!parser.Skip(_T("0,1")));
    assert(parser.Skip(_T("0,1"), _T(",0,1")));
    *buf = _T('\0');
    assert(!parser.CopyUntil('"', buf, dimof(buf)));
    assert(!*parser.Peek() && !*buf);

    int i1, i2;
    assert(parser.Init(_T("1,2+3")));
    assert(parser.Scan(_T("%d,%d+"), &i1, &i2) && i1 == 1 && i2 == 2);
    assert(!Str::IsEmpty(parser.Peek()));

    // the test string should only contain ASCII characters,
    // as all others might not be available in all code pages
#define TEST_STRING "aBc"
    char *strA = Str::Conv::ToAnsi(_T(TEST_STRING));
    assert(Str::Eq(strA, TEST_STRING));
    str = Str::Conv::FromAnsi(strA);
    free(strA);
    assert(Str::Eq(str, _T(TEST_STRING)));
    free(str);
#undef TEST_STRING

    assert(ChrIsDigit('0') && ChrIsDigit(_T('5')) && ChrIsDigit(L'9'));
    assert(iswdigit(L'\xB2') && !ChrIsDigit(L'\xB2'));
}

static void FileUtilTest()
{
    TCHAR *path1 = _T("C:\\Program Files\\SumatraPDF\\SumatraPDF.exe");

    const TCHAR *baseName = Path::GetBaseName(path1);
    assert(Str::Eq(baseName, _T("SumatraPDF.exe")));

    TCHAR *dirName = Path::GetDir(path1);
    assert(Str::Eq(dirName, _T("C:\\Program Files\\SumatraPDF")));
    baseName = Path::GetBaseName(dirName);
    assert(Str::Eq(baseName, _T("SumatraPDF")));
    free(dirName);

    path1 = _T("C:\\Program Files");
    dirName = Path::GetDir(path1);
    assert(Str::Eq(dirName, _T("C:\\")));
    free(dirName);

    TCHAR *path2 = Path::Join(_T("C:\\"), _T("Program Files"));
    assert(Str::Eq(path1, path2));
    free(path2);
    path2 = Path::Join(path1, _T("SumatraPDF"));
    assert(Str::Eq(path2, _T("C:\\Program Files\\SumatraPDF")));
    free(path2);
    path2 = Path::Join(_T("C:\\"), _T("\\Windows"));
    assert(Str::Eq(path2, _T("C:\\Windows")));
    free(path2);
}

static void VecStrTest()
{
    VStrList v;
    v.Append(Str::Dup(_T("foo")));
    v.Append(Str::Dup(_T("bar")));
    TCHAR *s = v.Join();
    assert(v.Count() == 2);
    assert(Str::Eq(_T("foobar"), s));
    free(s);

    s = v.Join(_T(";"));
    assert(v.Count() == 2);
    assert(Str::Eq(_T("foo;bar"), s));
    free(s);

    v.Append(Str::Dup(_T("glee")));
    s = v.Join(_T("_ _"));
    assert(v.Count() == 3);
    assert(Str::Eq(_T("foo_ _bar_ _glee"), s));
    free(s);
}

static void VecTest()
{
    Vec<int> ints;
    assert(ints.Count() == 0);
    ints.Append(1);
    ints.Push(2);
    ints.InsertAt(0, -1);
    assert(ints.Count() == 3);
    assert(ints[0] == -1 && ints[1] == 1 && ints[2] == 2);
    assert(ints[0] == ints.At(0) && ints[1] == ints.At(1) && ints[2] == ints.At(2));
    assert(ints.At(0) == -1 && ints.Last() == 2);
    int last = ints.Pop();
    assert(last == 2);
    assert(ints.Count() == 2);
    ints.Push(3);
    ints.RemoveAt(0);
    assert(ints.Count() == 2);
    assert(ints[0] == 1 && ints[1] == 3);
    ints.Reset();
    assert(ints.Count() == 0);

    for (int i = 0; i < 1000; i++)
        ints.Push(i);
    assert(ints.Count() == 1000 && ints[500] == 500);
    ints.Remove(500);
    assert(ints.Count() == 999 && ints[500] == 501);

    {
        char buf[2] = {'a', '\0'};
        Str::Str<char> v(0);
        for (int i=0; i<7; i++) {
            v.Append(buf, 1);
            buf[0] = buf[0] + 1;
        }
        char *s = v.LendData();
        assert(Str::Eq("abcdefg", s));
        assert(7 == v.Count());
        v.Set("helo");
        assert(4 == v.Count());
        assert(Str::Eq("helo", v.LendData()));
    }

    {
        Str::Str<char> v(128);
        v.Append("boo", 3);
        assert(Str::Eq("boo", v.LendData()));
        assert(v.Count() == 3);
        v.Append("fop");
        assert(Str::Eq("boofop", v.LendData()));
        assert(v.Count() == 6);
        v.RemoveAt(2, 3);
        assert(v.Count() == 3);
        assert(Str::Eq("bop", v.LendData()));
        v.Append('a');
        assert(v.Count() == 4);
        assert(Str::Eq("bopa", v.LendData()));
        char *s = v.StealData();
        assert(Str::Eq("bopa", s));
        free(s);
        assert(v.Count() == 0);
    }

    {
        Str::Str<char> v(0);
        for (int i=0; i<32; i++) {
            assert(v.Count() == i * 6);
            v.Append("lambd", 5);
            if (i % 2 == 0)
                v.Append('a');
            else
                v.Push('a');
        }

        for (int i=1; i<=16; i++) {
            v.RemoveAt((16 - i) * 6, 6);
            assert(v.Count() == (32 - i) * 6);
        }

        v.RemoveAt(0, 6 * 15);
        assert(v.Count() == 6);
        char *s = v.LendData();
        assert(Str::Eq(s, "lambda"));
        s = v.StealData();
        assert(Str::Eq(s, "lambda"));
        free(s);
        assert(v.Count() == 0);
    }

    {
        Vec<PointI *> v;
        srand((unsigned int)time(NULL));
        for (int i = 0; i < 128; i++) {
            v.Append(new PointI(i, i));
            size_t pos = rand() % v.Count();
            v.InsertAt(pos, new PointI(i, i));
        }
        while (v.Count() > 64) {
            size_t pos = rand() % v.Count();
            PointI *f = v[pos];
            v.Remove(f);
            delete f;
        }
        DeleteVecMembers(v);
    }
}

static void BencTestSerialization(BencObj *obj, const char *dataOrig)
{
    ScopedMem<char> data(obj->Encode());
    assert(data);
    assert(Str::Eq(data, dataOrig));
}

static void BencTestParseInt()
{
    struct {
        const char *    benc;
        bool            valid;
        int64_t         value;
    } testData[] = {
        { NULL, false },
        { "", false },
        { "a", false },
        { "0", false },
        { "i", false },
        { "ie", false },
        { "i0", false },
        { "i1", false },
        { "i23", false },
        { "i-", false },
        { "i-e", false },
        { "i-0e", false },
        { "i23f", false },
        { "i2-3e", false },
        { "i23-e", false },
        { "i041e", false },
        { "i9223372036854775808e", false },
        { "i-9223372036854775809e", false },

        { "i0e", true, 0 },
        { "i1e", true, 1 },
        { "i9823e", true, 9823 },
        { "i-1e", true, -1 },
        { "i-53e", true, -53 },
        { "i123e", true, 123 },
        { "i2147483647e", true, INT_MAX },
        { "i2147483648e", true, (int64_t)INT_MAX + 1 },
        { "i-2147483648e", true, INT_MIN },
        { "i-2147483649e", true, (int64_t)INT_MIN - 1 },
        { "i9223372036854775807e", true, _I64_MAX },
        { "i-9223372036854775808e", true, _I64_MIN },
    };

    for (int i = 0; i < dimof(testData); i++) {
        BencObj *obj = BencObj::Decode(testData[i].benc);
        if (testData[i].valid) {
            assert(obj);
            assert(obj->Type() == BT_INT);
            assert(static_cast<BencInt *>(obj)->Value() == testData[i].value);
            BencTestSerialization(obj, testData[i].benc);
            delete obj;
        } else {
            assert(!obj);
        }
    }
}

static void BencTestParseString()
{
    struct {
        const char *    benc;
        TCHAR *         value;
    } testData[] = {
        { NULL, NULL },
        { "", NULL },
        { "0", NULL },
        { "1234", NULL },
        { "a", NULL },
        { ":", NULL },
        { ":z", NULL },
        { "1:ab", NULL },
        { "3:ab", NULL },
        { "-2:ab", NULL },
        { "2e:ab", NULL },

        { "0:", _T("") },
        { "1:a", _T("a") },
        { "2::a", _T(":a") },
        { "4:spam", _T("spam") },
        { "4:i23e", _T("i23e") },
#ifdef UNICODE
        { "5:\xC3\xA4\xE2\x82\xAC", L"\u00E4\u20AC" },
#endif
    };

    for (int i = 0; i < dimof(testData); i++) {
        BencObj *obj = BencObj::Decode(testData[i].benc);
        if (testData[i].value) {
            assert(obj);
            assert(obj->Type() == BT_STRING);
            ScopedMem<TCHAR> value(static_cast<BencString *>(obj)->Value());
            assert(Str::Eq(value, testData[i].value));
            BencTestSerialization(obj, testData[i].benc);
            delete obj;
        } else {
            assert(!obj);
        }
    }

    BencRawString raw("a\x82");
    BencTestSerialization(&raw, "2:a\x82");
    assert(Str::Eq(raw.RawValue(), "a\x82"));
}

static void BencTestParseArray(const char *benc, size_t expectedLen)
{
    BencObj *obj = BencObj::Decode(benc);
    assert(obj);
    assert(obj->Type() == BT_ARRAY);
    assert(static_cast<BencArray *>(obj)->Length() == expectedLen);
    BencTestSerialization(obj, benc);
    delete obj;
}

static void BencTestParseArrays()
{   
    BencObj *obj;

    obj = BencObj::Decode("l");
    assert(!obj);
    obj = BencObj::Decode("l123");
    assert(!obj);
    obj = BencObj::Decode("li12e");
    assert(!obj);
    obj = BencObj::Decode("l2:ie");
    assert(!obj);

    BencTestParseArray("le", 0);
    BencTestParseArray("li35ee", 1);
    BencTestParseArray("llleee", 1);
    BencTestParseArray("li35ei-23e2:abe", 3);
    BencTestParseArray("li42e2:teldeedee", 4);
}

static void BencTestParseDict(const char *benc, size_t expectedLen)
{
    BencObj *obj = BencObj::Decode(benc);
    assert(obj);
    assert(obj->Type() == BT_DICT);
    assert(static_cast<BencDict *>(obj)->Length() == expectedLen);
    BencTestSerialization(obj, benc);
    delete obj;
}

static void BencTestParseDicts()
{   
    BencObj *obj;

    obj = BencObj::Decode("d");
    assert(!obj);
    obj = BencObj::Decode("d123");
    assert(!obj);
    obj = BencObj::Decode("di12e");
    assert(!obj);
    obj = BencObj::Decode("di12e2:ale");
    assert(!obj);

    BencTestParseDict("de", 0);
    BencTestParseDict("d2:hai35ee", 1);
    BencTestParseDict("d4:borg1:a3:rum3:leee", 2);
    BencTestParseDict("d1:Zi-23e2:able3:keyi35ee", 3);
}

#define ITERATION_COUNT 128

static void BencTestArrayAppend()
{
    BencArray *array = new BencArray();
    for (size_t i = 1; i <= ITERATION_COUNT; i++) {
        array->Add(i);
        assert(array->Length() == i);
    }
    array->Add(new BencDict());
    for (size_t i = 1; i <= ITERATION_COUNT; i++) {
        BencInt *obj = array->GetInt(i - 1);
        assert(obj && obj->Type() == BT_INT);
        assert(obj->Value() == i);
        assert(!array->GetString(i - 1));
        assert(!array->GetArray(i - 1));
        assert(!array->GetDict(i - 1));
    }
    assert(!array->GetInt(ITERATION_COUNT));
    assert(array->GetDict(ITERATION_COUNT));
    delete array;
}

static void BencTestDictAppend()
{
    /* test insertion in ascending order */
    BencDict *dict = new BencDict();
    for (size_t i = 1; i <= ITERATION_COUNT; i++) {
        ScopedMem<char> key(Str::Format("%04u", i));
        assert(Str::Len(key) == 4);
        dict->Add(key, i);
        assert(dict->Length() == i);
        assert(dict->GetInt(key));
        assert(!dict->GetString(key));
        assert(!dict->GetArray(key));
        assert(!dict->GetDict(key));
    }
    BencInt *intObj = dict->GetInt("0123");
    assert(intObj && intObj->Value() == 123);
    delete dict;

    /* test insertion in descending order */
    dict = new BencDict();
    for (size_t i = ITERATION_COUNT; i > 0; i--) {
        ScopedMem<char> key(Str::Format("%04u", i));
        assert(Str::Len(key) == 4);
        BencObj *obj = new BencInt(i);
        dict->Add(key, obj);
        assert(dict->Length() == ITERATION_COUNT + 1 - i);
        assert(dict->GetInt(key));
    }
    intObj = dict->GetInt("0123");
    assert(intObj && intObj->Value() == 123);
    delete dict;

    dict = new BencDict();
    dict->Add("ab", 1);
    dict->Add("KL", 2);
    dict->Add("gh", 3);
    dict->Add("YZ", 4);
    dict->Add("ab", 5);
    BencTestSerialization(dict, "d2:KLi2e2:YZi4e2:abi5e2:ghi3ee");
    delete dict;
}

void BaseUtils_UnitTests(void)
{
    DBG_OUT("Running BaseUtils unit tests\n");
    GeomTest();
    TStrTest();
    FileUtilTest();
    VecStrTest();
    VecTest();
    BencTestParseInt();
    BencTestParseString();
    BencTestParseArrays();
    BencTestParseDicts();
    BencTestArrayAppend();
    BencTestDictAppend();
}

#endif
