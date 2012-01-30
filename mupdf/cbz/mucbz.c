#include "fitz.h"
#include "mucbz.h"

#include <zlib.h>

#include <ctype.h> /* for tolower */

#define ZIP_LOCAL_FILE_SIG 0x04034b50
#define ZIP_CENTRAL_DIRECTORY_SIG 0x02014b50
#define ZIP_END_OF_CENTRAL_DIRECTORY_SIG 0x06054b50

#define DPI (72.0f / 144.0f)

static const char *cbz_ext_list[] = {
	".jpg", ".jpeg", ".png",
	".JPG", ".JPEG", ".PNG",
	NULL
};

struct cbz_page_s
{
	fz_pixmap *image;
};

typedef struct cbz_entry_s cbz_entry;

struct cbz_entry_s
{
	char *name;
	int offset;
};

struct cbz_document_s
{
	fz_context *ctx;
	fz_stream *file;
	int entry_count;
	cbz_entry *entry;
	int page_count;
	int *page;
};

static inline int getshort(fz_stream *file)
{
	int a = fz_read_byte(file);
	int b = fz_read_byte(file);
	return a | b << 8;
}

static inline int getlong(fz_stream *file)
{
	int a = fz_read_byte(file);
	int b = fz_read_byte(file);
	int c = fz_read_byte(file);
	int d = fz_read_byte(file);
	return a | b << 8 | c << 16 | d << 24;
}

static void *
cbz_zip_alloc_items(void *ctx, unsigned int items, unsigned int size)
{
	return fz_malloc_array(ctx, items, size);
}

static void
cbz_zip_free(void *ctx, void *ptr)
{
	fz_free(ctx, ptr);
}

static unsigned char *
cbz_read_zip_entry(cbz_document *doc, int offset, int *sizep)
{
	fz_context *ctx = doc->ctx;
	fz_stream *file = doc->file;
	int sig, method, namelength, extralength;
	unsigned long csize, usize;
	unsigned char *cdata;
	int code;

	fz_seek(file, offset, 0);

	sig = getlong(doc->file);
	if (sig != ZIP_LOCAL_FILE_SIG)
		fz_throw(ctx, "wrong zip local file signature (0x%x)", sig);

	(void) getshort(doc->file); /* version */
	(void) getshort(doc->file); /* general */
	method = getshort(doc->file);
	(void) getshort(doc->file); /* file time */
	(void) getshort(doc->file); /* file date */
	(void) getlong(doc->file); /* crc-32 */
	csize = getlong(doc->file); /* csize */
	usize = getlong(doc->file); /* usize */
	namelength = getshort(doc->file);
	extralength = getshort(doc->file);

	fz_seek(file, namelength + extralength, 1);

	cdata = fz_malloc(ctx, csize);
	fz_try(ctx)
	{
		fz_read(file, cdata, csize);
	}
	fz_catch(ctx)
	{
		fz_free(ctx, cdata);
		fz_rethrow(ctx);
	}

	if (method == 0)
	{
		*sizep = usize;
		return cdata;
	}

	if (method == 8)
	{
		unsigned char *udata = fz_malloc(ctx, usize);
		z_stream stream;

		memset(&stream, 0, sizeof stream);
		stream.zalloc = cbz_zip_alloc_items;
		stream.zfree = cbz_zip_free;
		stream.opaque = ctx;
		stream.next_in = cdata;
		stream.avail_in = csize;
		stream.next_out = udata;
		stream.avail_out = usize;

		fz_try(ctx)
		{
			code = inflateInit2(&stream, -15);
			if (code != Z_OK)
				fz_throw(ctx, "zlib inflateInit2 error: %s", stream.msg);
			code = inflate(&stream, Z_FINISH);
			if (code != Z_STREAM_END) {
				inflateEnd(&stream);
				fz_throw(ctx, "zlib inflate error: %s", stream.msg);
			}
			code = inflateEnd(&stream);
			if (code != Z_OK)
				fz_throw(ctx, "zlib inflateEnd error: %s", stream.msg);
		}
		fz_always(ctx)
		{
			fz_free(ctx, cdata);
		}
		fz_catch(ctx)
		{
			fz_free(ctx, udata);
			fz_rethrow(ctx);
		}

		*sizep = usize;
		return udata;
	}

	fz_throw(ctx, "unknown zip method: %d", method);
	return NULL; /* not reached */
}

static int
cbz_compare_entries(const void *a_, const void *b_)
{
	const cbz_entry *a = a_;
	const cbz_entry *b = b_;
	return strcmp(a->name, b->name);
}

static void
cbz_read_zip_dir_imp(cbz_document *doc, int startoffset)
{
	fz_context *ctx = doc->ctx;
	fz_stream *file = doc->file;
	int sig, offset, count;
	int namesize, metasize, commentsize;
	int i, k;

	fz_seek(file, startoffset, 0);

	sig = getlong(file);
	if (sig != ZIP_END_OF_CENTRAL_DIRECTORY_SIG)
		fz_throw(ctx, "wrong zip end of central directory signature (0x%x)", sig);

	(void) getshort(file); /* this disk */
	(void) getshort(file); /* start disk */
	(void) getshort(file); /* entries in this disk */
	count = getshort(file); /* entries in central directory disk */
	(void) getlong(file); /* size of central directory */
	offset = getlong(file); /* offset to central directory */

	doc->entry = fz_malloc_array(ctx, count, sizeof(cbz_entry));
	/* SumatraPDF: don't crash in cbz_close_document in case the above
	   malloc fails or an exception is thrown in the loop below */
	memset(doc->entry, 0, count * sizeof(cbz_entry));
	doc->entry_count = count;

	fz_seek(file, offset, 0);

	for (i = 0; i < count; i++)
	{
		cbz_entry *entry = doc->entry + i;

		sig = getlong(doc->file);
		if (sig != ZIP_CENTRAL_DIRECTORY_SIG)
			fz_throw(doc->ctx, "wrong zip central directory signature (0x%x)", sig);

		(void) getshort(file); /* version made by */
		(void) getshort(file); /* version to extract */
		(void) getshort(file); /* general */
		(void) getshort(file); /* method */
		(void) getshort(file); /* last mod file time */
		(void) getshort(file); /* last mod file date */
		(void) getlong(file); /* crc-32 */
		(void) getlong(file); /* csize */
		(void) getlong(file); /* usize */
		namesize = getshort(file);
		metasize = getshort(file);
		commentsize = getshort(file);
		(void) getshort(file); /* disk number start */
		(void) getshort(file); /* int file atts */
		(void) getlong(file); /* ext file atts */
		entry->offset = getlong(file);

		entry->name = malloc(namesize + 1);
		fz_read(file, (unsigned char *)entry->name, namesize);
		entry->name[namesize] = 0;

		fz_seek(file, metasize, 1);
		fz_seek(file, commentsize, 1);
	}

	qsort(doc->entry, count, sizeof(cbz_entry), cbz_compare_entries);

	doc->page_count = 0;
	doc->page = fz_malloc_array(ctx, count, sizeof(int));

	for (i = 0; i < count; i++)
		for (k = 0; cbz_ext_list[k]; k++)
			if (strstr(doc->entry[i].name, cbz_ext_list[k]))
				doc->page[doc->page_count++] = i;
}

static void
cbz_read_zip_dir(cbz_document *doc)
{
	fz_stream *file = doc->file;
	unsigned char buf[512];
	int filesize, back, maxback;
	int i, n;

	fz_seek(file, 0, 2);
	filesize = fz_tell(file);

	maxback = MIN(filesize, 0xFFFF + sizeof buf);
	back = MIN(maxback, sizeof buf);

	while (back < maxback)
	{
		fz_seek(file, filesize - back, 0);
		n = fz_read(file, buf, sizeof buf);
		for (i = n - 4; i > 0; i--)
		{
			if (!memcmp(buf + i, "PK\5\6", 4))
			{
				cbz_read_zip_dir_imp(doc, filesize - back + i);
				return;
			}
		}
		back += sizeof buf - 4;
	}

	fz_throw(doc->ctx, "cannot find end of central directory");
}

cbz_document *
cbz_open_document_with_stream(fz_stream *file)
{
	fz_context *ctx = file->ctx;
	cbz_document *doc;

	doc = fz_malloc_struct(ctx, cbz_document);
	doc->ctx = ctx;
	doc->file = fz_keep_stream(file);
	doc->entry_count = 0;
	doc->entry = NULL;
	doc->page_count = 0;
	doc->page = NULL;

	fz_try(ctx)
	{
		cbz_read_zip_dir(doc);
	}
	fz_catch(ctx)
	{
		cbz_close_document(doc);
		fz_rethrow(ctx);
	}

	return doc;
}

cbz_document *
cbz_open_document(fz_context *ctx, char *filename)
{
	fz_stream *file;
	cbz_document *doc;

	file = fz_open_file(ctx, filename);
	if (!file)
		fz_throw(ctx, "cannot open file '%s': %s", filename, strerror(errno));

	fz_try(ctx) {
		doc = cbz_open_document_with_stream(file);
	} fz_always(ctx) {
		fz_close(file);
	} fz_catch(ctx) {
		fz_rethrow(ctx);
	}

	return doc;
}

void
cbz_close_document(cbz_document *doc)
{
	int i;
	fz_context *ctx = doc->ctx;
	for (i = 0; i < doc->entry_count; i++)
		fz_free(ctx, doc->entry[i].name);
	fz_free(ctx, doc->entry);
	fz_free(ctx, doc->page);
	fz_close(doc->file);
	fz_free(ctx, doc);
}

int
cbz_count_pages(cbz_document *doc)
{
	return doc->page_count;
}

cbz_page *
cbz_load_page(cbz_document *doc, int number)
{
	fz_context *ctx = doc->ctx;
	unsigned char *data = NULL;
	cbz_page *page = NULL;
	int size;

	if (number < 0 || number >= doc->page_count)
		return NULL;

	number = doc->page[number];

	fz_var(data);
	fz_var(page);
	fz_try(ctx)
	{
		page = fz_malloc_struct(doc->ctx, cbz_page);
		page->image = NULL;

		data = cbz_read_zip_entry(doc, doc->entry[number].offset, &size);

		if (data[0] == 0xff && data[1] == 0xd8)
			page->image = fz_load_jpeg(ctx, data, size);
		else if (memcmp(data, "\211PNG\r\n\032\n", 8) == 0)
			page->image = fz_load_png(ctx, data, size);
		else
			fz_throw(ctx, "unknown image format");
	}
	fz_always(ctx)
	{
		fz_free(ctx, data);
	}
	fz_catch(ctx)
	{
		if (page && page->image)
			fz_drop_pixmap(ctx, page->image);
		fz_free(ctx, page);
		fz_rethrow(ctx);
	}

	return page;
}

void
cbz_free_page(cbz_document *doc, cbz_page *page)
{
	fz_drop_pixmap(doc->ctx, page->image);
	fz_free(doc->ctx, page);
}

fz_rect
cbz_bound_page(cbz_document *doc, cbz_page *page)
{
	fz_pixmap *image = page->image;
	fz_rect bbox;
	bbox.x0 = bbox.y0 = 0;
	bbox.x1 = image->w * DPI;
	bbox.y1 = image->h * DPI;
	return bbox;
}

void
cbz_run_page(cbz_document *doc, cbz_page *page, fz_device *dev, fz_matrix ctm, fz_cookie *cookie)
{
	fz_pixmap *image = page->image;
	ctm = fz_concat(fz_scale(image->w * DPI, image->h * DPI), ctm);
	fz_fill_image(dev, image, ctm, 1);
}
