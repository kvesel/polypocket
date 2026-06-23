/*
 * polypocket.c — polyglot file builder
 *
 * Usage: polypocket output input1 input2 [input3]
 * Builds a single output file that is simultaneously valid in all
 * supplied formats.
 *
 * Compile: gcc polypocket.c -o polypocket
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>

/* ── error / types ──────────────────────────────────────────────── */

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "polypocket: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

typedef enum { FT_PDF, FT_JPEG, FT_ZIP, FT_ELF, FT_PNG, FT_PE, FT_GIF, FT_MP3, FT_PS } FileType;

typedef struct { uint8_t *data; size_t size; } File;

/* ── dynamic byte buffer ────────────────────────────────────────── */

typedef struct { uint8_t *data; size_t len, cap; } Buf;

static void buf_init(Buf *b) { b->data = NULL; b->len = b->cap = 0; }

static void buf_free(Buf *b) { free(b->data); buf_init(b); }

static void buf_grow(Buf *b, size_t extra) {
    size_t need = b->len + extra;
    if (need <= b->cap) return;
    size_t nc = b->cap ? b->cap * 2 : 4096;
    while (nc < need) nc *= 2;
    b->data = realloc(b->data, nc);
    if (!b->data) die("out of memory");
    b->cap = nc;
}

static void buf_append(Buf *b, const void *src, size_t n) {
    if (!n) return;
    buf_grow(b, n);
    memcpy(b->data + b->len, src, n);
    b->len += n;
}


/* ── endian helpers ─────────────────────────────────────────────── */

static uint16_t r16le(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t r32le(const uint8_t *p) {
    return (uint32_t)(p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24));
}
static void w32le(uint8_t *p, uint32_t v) {
    p[0] = v; p[1] = v>>8; p[2] = v>>16; p[3] = v>>24;
}
static uint64_t r64le(const uint8_t *p) {
    return (uint64_t)r32le(p) | ((uint64_t)r32le(p + 4) << 32);
}
static void w64le(uint8_t *p, uint64_t v) {
    w32le(p, (uint32_t)v); w32le(p + 4, (uint32_t)(v >> 32));
}
static uint16_t r16be(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static void w16be(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static void w32be(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
}

/* write exactly 10 ASCII decimal digits into dst (no null terminator) */
static void fmt10(uint8_t *dst, uint64_t v) {
    for (int i = 9; i >= 0; i--) { dst[i] = '0' + (uint8_t)(v % 10); v /= 10; }
}

/* ── file I/O ───────────────────────────────────────────────────── */

static File read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open '%s': %s", path, strerror(errno));
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    File file;
    file.size = (size_t)sz;
    file.data = malloc(file.size ? file.size : 1);
    if (!file.data) die("out of memory");
    if (file.size && fread(file.data, 1, file.size, f) != file.size)
        die("read error: %s", path);
    fclose(f);
    return file;
}

static void write_file(const char *path, const Buf *b) {
    FILE *f = fopen(path, "wb");
    if (!f) die("cannot write '%s': %s", path, strerror(errno));
    if (b->len && fwrite(b->data, 1, b->len, f) != b->len)
        die("write error: %s", path);
    fclose(f);
}

/* ── file type detection ────────────────────────────────────────── */

static FileType detect_type(const File *f, const char *name) {
    if (f->size >= 3 && f->data[0] == 0xff && f->data[1] == 0xd8 && f->data[2] == 0xff)
        return FT_JPEG;
    if (f->size >= 4 && memcmp(f->data, "PK\x03\x04", 4) == 0)
        return FT_ZIP;
    if (f->size >= 4 && memcmp(f->data, "\x7f" "ELF", 4) == 0)
        return FT_ELF;
    if (f->size >= 2 && f->data[0] == 'M' && f->data[1] == 'Z')
        return FT_PE;
    if (f->size >= 8 && memcmp(f->data, "\x89PNG\r\n\x1a\n", 8) == 0)
        return FT_PNG;
    if (f->size >= 6 && memcmp(f->data, "GIF8", 4) == 0 &&
        (f->data[4] == '7' || f->data[4] == '9') && f->data[5] == 'a')
        return FT_GIF;
    if (f->size >= 3 && memcmp(f->data, "ID3", 3) == 0)
        return FT_MP3;
    if (f->size >= 2 && f->data[0] == '%' && f->data[1] == '!')
        return FT_PS;
    size_t scan = f->size < 1024 ? f->size : 1024;
    for (size_t i = 0; i + 4 <= scan; i++)
        if (memcmp(f->data + i, "%PDF", 4) == 0) return FT_PDF;
    die("unrecognised file type: %s", name);
    return FT_PDF; /* unreachable */
}

/* ── ZIP handling ───────────────────────────────────────────────── */

/*
 * Search backward from the end of the file for the End of Central
 * Directory record signature (PK\x05\x06).  We validate the comment
 * length field to avoid false positives inside file data.
 */
static size_t find_eocd(const uint8_t *data, size_t size) {
    if (size < 22) die("ZIP: file too small to contain EOCD");
    size_t limit = size > (22u + 65535u) ? size - 22u - 65535u : 0;
    size_t i = size - 22u;
    for (;;) {
        if (data[i] == 'P' && data[i+1] == 'K' &&
            data[i+2] == 0x05 && data[i+3] == 0x06) {
            uint16_t clen = r16le(data + i + 20);
            if (i + 22u + clen == size) return i;
        }
        if (i == limit) break;
        i--;
    }
    die("ZIP: EOCD record not found");
    return 0;
}

/*
 * Scan the extra field block for the Zip64 extended-info block (ID 0x0001)
 * and add `delta` to its local-header-offset subfield.
 *
 * usz32 / csz32 are the 32-bit sizes from the CD entry: when they equal
 * 0xFFFFFFFF the 64-bit values precede the offset inside the Zip64 block,
 * so we must skip them to find the right position.
 */
static void zip64_patch_lhoff(uint8_t *extra, uint16_t ex_len,
                               uint32_t usz32, uint32_t csz32, uint64_t delta) {
    size_t p = 0;
    while (p + 4 <= (size_t)ex_len) {
        uint16_t id  = r16le(extra + p);
        uint16_t dsz = r16le(extra + p + 2);
        if (id == 0x0001) {
            size_t fp = 4;                          /* offset from block start */
            if (usz32 == 0xFFFFFFFFu) fp += 8;     /* skip original size      */
            if (csz32 == 0xFFFFFFFFu) fp += 8;     /* skip compressed size    */
            if (fp + 8 > (size_t)4 + dsz)
                die("ZIP: Zip64 extra field too small to hold local header offset");
            w64le(extra + p + fp, r64le(extra + p + fp) + delta);
            return;
        }
        p += 4u + dsz;
    }
    die("ZIP: Zip64 extra field (0x0001) not found for entry with sentinel offset");
}

/*
 * Return a new Buf containing the ZIP data with all local-header offsets
 * shifted up by `delta` bytes.  Handles both standard (32-bit) and Zip64
 * archives transparently.
 *
 * Zip64 detection: look for the EOCD64 locator (PK\x06\x07) in the 20
 * bytes immediately before the EOCD, then follow its pointer to the EOCD64
 * record (PK\x06\x06) which carries the true 64-bit CD size and offset.
 */
static Buf zip_with_delta(const File *zip, size_t delta) {
    size_t eocd_off  = find_eocd(zip->data, zip->size);
    uint16_t cmt_len = r16le(zip->data + eocd_off + 20);

    /* ── Zip64 detection ── */
    int    is_zip64     = 0;
    size_t eocd64_off   = 0;
    size_t eocd64_total = 0;
    uint64_t cd_size64  = 0, cd_off64 = 0;

    if (eocd_off >= 20 &&
        memcmp(zip->data + eocd_off - 20, "PK\x06\x07", 4) == 0) {
        const uint8_t *loc = zip->data + eocd_off - 20;
        eocd64_off = (size_t)r64le(loc + 8);
        if (eocd64_off + 56 > zip->size ||
            memcmp(zip->data + eocd64_off, "PK\x06\x06", 4) != 0)
            die("ZIP: Zip64 EOCD record not found at offset from locator");
        is_zip64     = 1;
        eocd64_total = (size_t)(12 + r64le(zip->data + eocd64_off + 4));
        cd_size64    = r64le(zip->data + eocd64_off + 40);
        cd_off64     = r64le(zip->data + eocd64_off + 48);
    }

    size_t actual_cd_off  = is_zip64 ? (size_t)cd_off64
                                     : (size_t)r32le(zip->data + eocd_off + 16);
    size_t actual_cd_size = is_zip64 ? (size_t)cd_size64
                                     : (size_t)r32le(zip->data + eocd_off + 12);

    Buf out; buf_init(&out);

    /* local file entries — copy verbatim */
    buf_append(&out, zip->data, actual_cd_off);

    /* rebuild central directory with patched local-header offsets */
    size_t cd_start = out.len;
    const uint8_t *cd = zip->data + actual_cd_off;
    size_t pos = 0;
    while (pos + 4 <= actual_cd_size &&
           memcmp(cd + pos, "PK\x01\x02", 4) == 0) {
        uint16_t fn = r16le(cd + pos + 28);
        uint16_t ex = r16le(cd + pos + 30);
        uint16_t cm = r16le(cd + pos + 32);
        size_t   es = 46u + fn + ex + cm;
        size_t   wo = out.len;
        buf_append(&out, cd + pos, es);

        uint32_t lhoff32 = r32le(cd + pos + 42);
        if (lhoff32 == 0xFFFFFFFFu) {
            /* Zip64: patch the 64-bit offset inside the Zip64 extra field */
            zip64_patch_lhoff(out.data + wo + 46 + fn, ex,
                              r32le(cd + pos + 24),   /* uncompressed size */
                              r32le(cd + pos + 20),   /* compressed size   */
                              (uint64_t)delta);
        } else {
            w32le(out.data + wo + 42, lhoff32 + (uint32_t)delta);
        }
        pos += es;
    }

    size_t new_cd_size = out.len - cd_start;

    if (is_zip64) {
        /* EOCD64: copy verbatim, then patch cd_size and cd_off */
        size_t eocd64_pos = out.len;
        buf_append(&out, zip->data + eocd64_off, eocd64_total);
        w64le(out.data + eocd64_pos + 40, (uint64_t)new_cd_size);
        w64le(out.data + eocd64_pos + 48, (uint64_t)(cd_start + delta));

        /* EOCD64 locator: copy and patch the EOCD64 file offset */
        uint8_t loc[20];
        memcpy(loc, zip->data + eocd_off - 20, 20);
        w64le(loc + 8, (uint64_t)(eocd64_pos + delta));
        buf_append(&out, loc, 20);

        /* EOCD: keep verbatim — sentinel 0xFFFFFFFF values stay correct */
        buf_append(&out, zip->data + eocd_off, 22);
    } else {
        if ((uint64_t)(cd_start + delta) > 0xFFFFFFFFu)
            die("ZIP: output exceeds 4 GiB; provide a Zip64 archive instead");
        uint8_t eocd[22];
        memcpy(eocd, zip->data + eocd_off, 22);
        w32le(eocd + 12, (uint32_t)new_cd_size);
        w32le(eocd + 16, (uint32_t)(cd_start + delta));
        buf_append(&out, eocd, 22);
    }

    if (cmt_len)
        buf_append(&out, zip->data + eocd_off + 22, cmt_len);

    return out;
}

/* ── PDF handling ───────────────────────────────────────────────── */

/*
 * Search backward from the end of the file for the last "startxref"
 * keyword (within the final 1 KiB, which is always enough).
 */
static size_t find_startxref(const uint8_t *data, size_t size) {
    size_t limit = size > 1024 ? size - 1024 : 0;
    if (size < 9) die("PDF: file too small");
    size_t i = size - 9;
    for (;;) {
        if (memcmp(data + i, "startxref", 9) == 0) return i;
        if (i == limit) break;
        i--;
    }
    die("PDF: 'startxref' keyword not found");
    return 0;
}

/*
 * Return a new Buf containing the PDF with all in-use (type 'n') xref
 * entry byte-offsets shifted up by `delta`, and the `startxref` value
 * updated to match.  The section from `startxref` onward is rewritten
 * so the value length change is handled cleanly.
 *
 * strip_prefix: bytes to omit from the start of pdf->data in the output.
 * The caller ensures delta is already calculated to account for this
 * (i.e. strip_prefix bytes were put elsewhere in the file so they still
 * count toward the offset of the first object).
 *
 * Limitation: PDFs that use cross-reference STREAMS (PDF 1.5+,
 * linearized PDFs) are detected and rejected with a helpful message.
 */
static Buf pdf_with_delta(const File *pdf, size_t delta, size_t strip_prefix) {
    /* find last startxref */
    size_t sx_off = find_startxref(pdf->data, pdf->size);

    /* parse the integer that follows startxref */
    size_t p = sx_off + 9;
    while (p < pdf->size && (pdf->data[p] == '\r' || pdf->data[p] == '\n' ||
                              pdf->data[p] == ' '))
        p++;
    uint64_t old_sx = 0;
    while (p < pdf->size && pdf->data[p] >= '0' && pdf->data[p] <= '9')
        old_sx = old_sx * 10 + (pdf->data[p++] - '0');

    if (old_sx + 4 > pdf->size)
        die("PDF: startxref value %llu is past end of file",
            (unsigned long long)old_sx);

    if (old_sx < strip_prefix)
        die("PDF: xref table sits inside the stripped header — malformed PDF");

    /* check for cross-reference stream (not supported) */
    if (memcmp(pdf->data + old_sx, "xref", 4) != 0)
        die("PDF uses cross-reference streams (PDF 1.5+ / linearized).\n"
            "Convert it first with ghostscript:\n"
            "  gs -dNOPAUSE -dBATCH -sDEVICE=pdfwrite -dCompatibilityLevel=1.4"
            " -sOutputFile=compat.pdf input.pdf");

    /* copy content from strip_prefix up to (not including) "startxref" */
    Buf out;
    buf_init(&out);
    buf_append(&out, pdf->data + strip_prefix, sx_off - strip_prefix);

    /* patch xref entries — the table is at (old_sx - strip_prefix) in out */
    size_t xp = (size_t)(old_sx - strip_prefix) + 4;  /* skip "xref" */
    while (xp < out.len && (out.data[xp] == '\r' || out.data[xp] == '\n'))
        xp++;

    while (xp < out.len && out.data[xp] != 't') { /* 't' starts "trailer" */
        /* subsection header: "first_obj count\n" */
        uint32_t count = 0;
        while (xp < out.len && out.data[xp] >= '0' && out.data[xp] <= '9') xp++;
        while (xp < out.len && out.data[xp] == ' ') xp++;
        while (xp < out.len && out.data[xp] >= '0' && out.data[xp] <= '9')
            count = count * 10 + (out.data[xp++] - '0');
        while (xp < out.len && (out.data[xp] == '\r' || out.data[xp] == '\n'))
            xp++;

        /* each entry is exactly 20 bytes */
        for (uint32_t i = 0; i < count; i++) {
            if (xp + 20 > out.len)
                die("PDF: xref entry %u overruns file", i);
            /* byte 17 is 'f' (free) or 'n' (in-use) */
            if (out.data[xp + 17] == 'n') {
                uint64_t off = 0;
                for (int j = 0; j < 10; j++)
                    off = off * 10 + (out.data[xp + j] - '0');
                fmt10(out.data + xp, off + delta);
            }
            xp += 20;
        }
    }

    /* rewrite startxref + %%EOF tail with updated value */
    uint64_t new_sx = old_sx + (uint64_t)delta;
    char tail[80];
    int tlen = snprintf(tail, sizeof(tail),
                        "startxref\n%llu\n%%%%EOF\n",
                        (unsigned long long)new_sx);
    buf_append(&out, (const uint8_t *)tail, (size_t)tlen);

    return out;
}

/* ── JPEG handling ──────────────────────────────────────────────── */

/*
 * Return a new Buf containing the JPEG with a comment segment
 * (FF FE) injected after the SOI and any immediately-following APP0,
 * carrying `text` (tlen bytes).  This embeds %PDF within the first
 * 1024 bytes so PDF parsers recognise the combined file.
 */
static Buf jpeg_inject_comment(const File *jpeg,
                               const uint8_t *text, size_t tlen) {
    if (jpeg->size < 4 || jpeg->data[0] != 0xff || jpeg->data[1] != 0xd8)
        die("JPEG: missing FF D8 SOI");
    if (tlen > 65533)
        die("JPEG: comment too large (%zu bytes, max 65533)", tlen);

    /* inject after APP0 (FF E0) when present to stay JFIF-compatible */
    size_t inject_at = 2;
    if (jpeg->data[2] == 0xff && jpeg->data[3] == 0xe0 && jpeg->size >= 6) {
        uint16_t seg_len = r16be(jpeg->data + 4);
        size_t seg_end = 2u + 2u + (size_t)seg_len;
        if (seg_end < jpeg->size) inject_at = seg_end;
    }

    uint16_t seg_len = (uint16_t)(tlen + 2); /* +2 for length field itself */

    Buf out;
    buf_init(&out);
    buf_append(&out, jpeg->data, inject_at);
    uint8_t marker[2] = {0xff, 0xfe};
    buf_append(&out, marker, 2);
    uint8_t lb[2]; w16be(lb, seg_len);
    buf_append(&out, lb, 2);
    buf_append(&out, text, tlen);
    buf_append(&out, jpeg->data + inject_at, jpeg->size - inject_at);

    return out;
}

/* ── PNG handling ───────────────────────────────────────────────── */

/*
 * Streaming CRC32 (ISO 3309 / ITU-T V.42, reflected polynomial 0xEDB88320).
 * Used for PNG chunk CRC fields.
 */
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0u);
    }
    return crc;
}

/*
 * Inject a tEXt chunk after IHDR carrying `text` (tlen bytes).  The chunk
 * uses keyword "X" so the PDF first line starting with "%PDF" falls within
 * the first 1024 bytes where PDF parsers scan for the header.
 *
 * Chunk layout: 4-byte length | "tEXt" | "X\0" | text | 4-byte CRC.
 * Fixed overhead = 4+4+2+4 = 14 bytes (the text payload is in addition).
 */
static Buf png_inject_text(const File *png, const uint8_t *text, size_t tlen) {
    if (png->size < 33 || memcmp(png->data, "\x89PNG\r\n\x1a\n", 8) != 0)
        die("PNG: invalid signature or file too small");
    if (memcmp(png->data + 12, "IHDR", 4) != 0)
        die("PNG: IHDR chunk not found at expected position");

    static const uint8_t type[4] = {'t','E','X','t'};
    static const uint8_t kw[2]   = {'X', 0};
    uint32_t data_len = 2u + (uint32_t)tlen;

    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, type, 4);
    crc = crc32_update(crc, kw, 2);
    crc = crc32_update(crc, text, tlen);
    crc ^= 0xFFFFFFFFu;

    Buf out; buf_init(&out);
    uint8_t len_buf[4]; w32be(len_buf, data_len);
    uint8_t crc_buf[4]; w32be(crc_buf, crc);
    buf_append(&out, png->data, 33);   /* sig + IHDR chunk */
    buf_append(&out, len_buf, 4);
    buf_append(&out, type, 4);
    buf_append(&out, kw, 2);
    buf_append(&out, text, tlen);
    buf_append(&out, crc_buf, 4);
    buf_append(&out, png->data + 33, png->size - 33);
    return out;
}

static Buf combine_png_zip(const File *png, const File *zip) {
    Buf out; buf_init(&out);
    buf_append(&out, png->data, png->size);
    Buf zp = zip_with_delta(zip, png->size);
    buf_append(&out, zp.data, zp.len);
    buf_free(&zp);
    return out;
}

static Buf combine_png_pdf(const File *png, const File *pdf) {
    size_t M = 0;
    while (M < pdf->size && pdf->data[M] != '\n') M++;
    if (M < pdf->size) M++;
    if (M < 5 || memcmp(pdf->data, "%PDF-", 5) != 0)
        die("PDF: file does not start with %%PDF-");

    /*
     * tEXt chunk overhead = 14 bytes; M bytes embedded in chunk cancel the
     * M bytes stripped from PDF, so delta = png->size + 14.
     */
    size_t delta = png->size + 14;

    Buf pngmod = png_inject_text(png, pdf->data, M);
    Buf pdfmod = pdf_with_delta(pdf, delta, M);
    Buf out; buf_init(&out);
    buf_append(&out, pngmod.data, pngmod.len);
    buf_append(&out, pdfmod.data, pdfmod.len);
    buf_free(&pngmod);
    buf_free(&pdfmod);
    return out;
}

static Buf combine_png_pdf_zip(const File *png, const File *pdf, const File *zip) {
    Buf pp = combine_png_pdf(png, pdf);
    Buf zp = zip_with_delta(zip, pp.len);
    Buf out; buf_init(&out);
    buf_append(&out, pp.data, pp.len);
    buf_append(&out, zp.data, zp.len);
    buf_free(&pp);
    buf_free(&zp);
    return out;
}

/* ── GIF handling ───────────────────────────────────────────────── */

/*
 * Inject a Comment Extension (0x21 0xFE) into the GIF immediately after
 * the Logical Screen Descriptor and optional Global Color Table, carrying
 * `text` (tlen bytes, max 255).  This places %PDF within the first 1024
 * bytes so PDF parsers recognise the combined file.
 *
 * Comment layout: 0x21 0xFE | 1-byte sub-block length | text | 0x00
 * Fixed overhead = 2+1+1 = 4 bytes (text payload is additional).
 */
static Buf gif_inject_comment(const File *gif, const uint8_t *text, size_t tlen) {
    if (gif->size < 13)
        die("GIF: file too small to contain header and LSD");
    if (memcmp(gif->data, "GIF8", 4) != 0 ||
        (gif->data[4] != '7' && gif->data[4] != '9') || gif->data[5] != 'a')
        die("GIF: invalid header signature");
    if (tlen > 255)
        die("GIF: comment text too large (%zu bytes, max 255)", tlen);

    /* skip Global Color Table if present (bit 7 of packed field at byte 10) */
    size_t gct_size = 0;
    if (gif->data[10] & 0x80)
        gct_size = 3u << ((gif->data[10] & 0x07) + 1);  /* 3 * 2^(n+1) */

    size_t inject_at = 13 + gct_size;
    if (inject_at > gif->size)
        die("GIF: Global Color Table extends beyond end of file");

    Buf out; buf_init(&out);
    buf_append(&out, gif->data, inject_at);
    uint8_t hdr[3] = {0x21, 0xFE, (uint8_t)tlen};
    buf_append(&out, hdr, 3);
    buf_append(&out, text, tlen);
    uint8_t term = 0x00;
    buf_append(&out, &term, 1);
    buf_append(&out, gif->data + inject_at, gif->size - inject_at);
    return out;
}

static Buf combine_gif_zip(const File *gif, const File *zip) {
    Buf out; buf_init(&out);
    buf_append(&out, gif->data, gif->size);
    Buf zp = zip_with_delta(zip, gif->size);
    buf_append(&out, zp.data, zp.len);
    buf_free(&zp);
    return out;
}

static Buf combine_gif_pdf(const File *gif, const File *pdf) {
    size_t M = 0;
    while (M < pdf->size && pdf->data[M] != '\n') M++;
    if (M < pdf->size) M++;
    if (M < 5 || memcmp(pdf->data, "%PDF-", 5) != 0)
        die("PDF: file does not start with %%PDF-");

    /* comment overhead = 4 bytes; M bytes in comment cancel M bytes stripped */
    size_t delta = gif->size + 4;

    Buf gifmod = gif_inject_comment(gif, pdf->data, M);
    Buf pdfmod = pdf_with_delta(pdf, delta, M);
    Buf out; buf_init(&out);
    buf_append(&out, gifmod.data, gifmod.len);
    buf_append(&out, pdfmod.data, pdfmod.len);
    buf_free(&gifmod);
    buf_free(&pdfmod);
    return out;
}

static Buf combine_gif_pdf_zip(const File *gif, const File *pdf, const File *zip) {
    Buf gp = combine_gif_pdf(gif, pdf);
    Buf zp = zip_with_delta(zip, gp.len);
    Buf out; buf_init(&out);
    buf_append(&out, gp.data, gp.len);
    buf_append(&out, zp.data, zp.len);
    buf_free(&gp);
    buf_free(&zp);
    return out;
}

/* ── MP3/ID3v2 handling ─────────────────────────────────────────── */

static uint32_t id3_decode_syncsafe(const uint8_t *p) {
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) <<  7) |  (uint32_t)(p[3] & 0x7F);
}
static void id3_encode_syncsafe(uint8_t *p, uint32_t v) {
    if (v > 0x0FFFFFFFu) die("MP3: ID3v2 tag size exceeds syncsafe limit");
    p[3] = v & 0x7F; v >>= 7; p[2] = v & 0x7F; v >>= 7;
    p[1] = v & 0x7F; v >>= 7; p[0] = v & 0x7F;
}

/*
 * Inject a TXXX frame into the ID3v2 tag carrying `text` (the PDF first
 * line) so that %PDF appears near the start of the file where PDF parsers
 * scan for the header.
 *
 * Frame layout: "TXXX" | size(4) | flags(2) | 0x00 (encoding) | 0x00 (desc) | text
 * Fixed overhead = 10+2 = 12 bytes (text payload is additional).
 *
 * Only ID3v2.3 and ID3v2.4 are supported (the vast majority of MP3 files).
 * Frame size encoding: big-endian for v2.3, syncsafe for v2.4.
 */
static Buf mp3_inject_txxx(const File *mp3, const uint8_t *text, size_t tlen) {
    if (mp3->size < 10 || memcmp(mp3->data, "ID3", 3) != 0)
        die("MP3: ID3v2 tag not found");

    uint8_t major = mp3->data[3];
    uint8_t flags = mp3->data[5];
    if (major < 3 || major > 4)
        die("MP3: unsupported ID3v2 version 2.%u (need 2.3 or 2.4)", (unsigned)major);
    if (flags & 0x40) die("MP3: ID3v2 extended header is not supported");
    if (major == 4 && (flags & 0x10)) die("MP3: ID3v2.4 footer is not supported");

    uint32_t tag_size  = id3_decode_syncsafe(mp3->data + 6);
    if (10u + (size_t)tag_size > mp3->size)
        die("MP3: ID3v2 tag size exceeds file size");

    uint32_t fc = (uint32_t)tlen + 2u;  /* encoding + null desc + text */
    uint32_t ft = 10u + fc;             /* full frame size             */

    Buf out; buf_init(&out);
    buf_append(&out, mp3->data, 10);
    id3_encode_syncsafe(out.data + 6, tag_size + ft);

    uint8_t fhdr[10]; memcpy(fhdr, "TXXX", 4);
    if (major == 4) id3_encode_syncsafe(fhdr + 4, fc);
    else            w32be(fhdr + 4, fc);
    fhdr[8] = fhdr[9] = 0x00;
    buf_append(&out, fhdr, 10);

    uint8_t enc_desc[2] = {0x00, 0x00};
    buf_append(&out, enc_desc, 2);
    buf_append(&out, text, tlen);
    buf_append(&out, mp3->data + 10, tag_size);  /* original frames */
    size_t audio = 10u + (size_t)tag_size;
    if (audio < mp3->size)
        buf_append(&out, mp3->data + audio, mp3->size - audio);
    return out;
}

static Buf combine_mp3_zip(const File *mp3, const File *zip) {
    Buf out; buf_init(&out);
    buf_append(&out, mp3->data, mp3->size);
    Buf zp = zip_with_delta(zip, mp3->size);
    buf_append(&out, zp.data, zp.len);
    buf_free(&zp);
    return out;
}

static Buf combine_mp3_pdf(const File *mp3, const File *pdf) {
    size_t M = 0;
    while (M < pdf->size && pdf->data[M] != '\n') M++;
    if (M < pdf->size) M++;
    if (M < 5 || memcmp(pdf->data, "%PDF-", 5) != 0)
        die("PDF: file does not start with %%PDF-");

    /* TXXX frame overhead = 12 bytes; M bytes in frame cancel M stripped */
    size_t delta = mp3->size + 12;

    Buf mp3mod = mp3_inject_txxx(mp3, pdf->data, M);
    Buf pdfmod = pdf_with_delta(pdf, delta, M);
    Buf out; buf_init(&out);
    buf_append(&out, mp3mod.data, mp3mod.len);
    buf_append(&out, pdfmod.data, pdfmod.len);
    buf_free(&mp3mod);
    buf_free(&pdfmod);
    return out;
}

static Buf combine_mp3_pdf_zip(const File *mp3, const File *pdf, const File *zip) {
    Buf mp = combine_mp3_pdf(mp3, pdf);
    Buf zp = zip_with_delta(zip, mp.len);
    Buf out; buf_init(&out);
    buf_append(&out, mp.data, mp.len);
    buf_append(&out, zp.data, zp.len);
    buf_free(&mp);
    buf_free(&zp);
    return out;
}

/* ── PostScript handling ─────────────────────────────────────────── */

/*
 * Inject `text` (the PDF first line, e.g. "%PDF-1.7\n") as a PostScript
 * comment immediately after the first line of the PS file.  Because text
 * already starts with '%', it is syntactically a valid PS comment with no
 * additional framing — overhead is zero.
 *
 * Modified PS size = ps->size + M; PDF body starts at ps->size + M;
 * object originally at offset O → ps->size + O, so delta = ps->size.
 */
static Buf ps_inject_comment(const File *ps, const uint8_t *text, size_t tlen) {
    if (ps->size < 2 || ps->data[0] != '%' || ps->data[1] != '!')
        die("PS: file does not begin with %%!");

    size_t L = 0;
    while (L < ps->size && ps->data[L] != '\n') L++;
    if (L < ps->size) L++;

    Buf out; buf_init(&out);
    buf_append(&out, ps->data, L);
    buf_append(&out, text, tlen);
    buf_append(&out, ps->data + L, ps->size - L);
    return out;
}

static Buf combine_ps_zip(const File *ps, const File *zip) {
    Buf out; buf_init(&out);
    buf_append(&out, ps->data, ps->size);
    Buf zp = zip_with_delta(zip, ps->size);
    buf_append(&out, zp.data, zp.len);
    buf_free(&zp);
    return out;
}

static Buf combine_ps_pdf(const File *ps, const File *pdf) {
    size_t M = 0;
    while (M < pdf->size && pdf->data[M] != '\n') M++;
    if (M < pdf->size) M++;
    if (M < 5 || memcmp(pdf->data, "%PDF-", 5) != 0)
        die("PDF: file does not start with %%PDF-");

    /* zero extra overhead; delta = ps->size */
    size_t delta = ps->size;

    Buf psmod = ps_inject_comment(ps, pdf->data, M);
    Buf pdfmod = pdf_with_delta(pdf, delta, M);
    Buf out; buf_init(&out);
    buf_append(&out, psmod.data, psmod.len);
    buf_append(&out, pdfmod.data, pdfmod.len);
    buf_free(&psmod);
    buf_free(&pdfmod);
    return out;
}

static Buf combine_ps_pdf_zip(const File *ps, const File *pdf, const File *zip) {
    Buf pp = combine_ps_pdf(ps, pdf);
    Buf zp = zip_with_delta(zip, pp.len);
    Buf out; buf_init(&out);
    buf_append(&out, pp.data, pp.len);
    buf_append(&out, zp.data, zp.len);
    buf_free(&pp);
    buf_free(&zp);
    return out;
}

/* ── combination functions ──────────────────────────────────────── */

static Buf combine_pdf_zip(const File *pdf, const File *zip) {
    Buf out; buf_init(&out);
    buf_append(&out, pdf->data, pdf->size);
    Buf zp = zip_with_delta(zip, pdf->size);
    buf_append(&out, zp.data, zp.len);
    buf_free(&zp);
    return out;
}

static Buf combine_jpeg_zip(const File *jpeg, const File *zip) {
    Buf out; buf_init(&out);
    buf_append(&out, jpeg->data, jpeg->size);
    Buf zp = zip_with_delta(zip, jpeg->size);
    buf_append(&out, zp.data, zp.len);
    buf_free(&zp);
    return out;
}

static Buf combine_jpeg_pdf(const File *jpeg, const File *pdf) {
    /*
     * Use the PDF's own first line ("%PDF-x.y\n") as the JPEG comment
     * text, then strip that line from the appended PDF content.
     *
     * This means the file has exactly ONE %PDF occurrence (in the JPEG
     * comment at ~offset 24), not two — preventing viewers that do
     * multi-format scanning from detecting a second standalone PDF
     * starting at the appended content.
     *
     * The M bytes added by the comment exactly cancel the M bytes
     * stripped from the PDF, so delta collapses to jpeg->size + 4
     * (4 = the FF FE comment marker + 2-byte length field).
     */

    /* measure PDF first line: everything up to and including the first \n */
    size_t M = 0;
    while (M < pdf->size && pdf->data[M] != '\n') M++;
    if (M < pdf->size) M++; /* include the \n */

    if (M < 5 || memcmp(pdf->data, "%PDF-", 5) != 0)
        die("PDF: file does not start with %%PDF-");

    /* delta = jpeg->size + 4 regardless of M (see comment above) */
    size_t delta = jpeg->size + 4;

    Buf jmod = jpeg_inject_comment(jpeg, pdf->data, M);
    Buf pmod = pdf_with_delta(pdf, delta, M);
    Buf out; buf_init(&out);
    buf_append(&out, jmod.data, jmod.len);
    buf_append(&out, pmod.data, pmod.len);
    buf_free(&jmod);
    buf_free(&pmod);
    return out;
}

static Buf combine_jpeg_pdf_zip(const File *jpeg, const File *pdf, const File *zip) {
    Buf jp = combine_jpeg_pdf(jpeg, pdf);
    Buf zp = zip_with_delta(zip, jp.len);
    Buf out; buf_init(&out);
    buf_append(&out, jp.data, jp.len);
    buf_append(&out, zp.data, zp.len);
    buf_free(&jp);
    buf_free(&zp);
    return out;
}

/* ── ELF combinations ───────────────────────────────────────────── */

/*
 * ELF+ZIP: the kernel only loads PT_LOAD segments and ignores trailing
 * bytes, so we can append a ZIP just like we do after a JPEG or PDF.
 */
static Buf combine_elf_zip(const File *elf, const File *zip) {
    Buf out; buf_init(&out);
    buf_append(&out, elf->data, elf->size);
    Buf zp = zip_with_delta(zip, elf->size);
    buf_append(&out, zp.data, zp.len);
    buf_free(&zp);
    return out;
}

/*
 * ELF+PDF: smuggle %PDF into ELF ident bytes [8..15].
 *
 * The ELF ident is 16 bytes: magic[4] class data version osabi
 * abiversion[1] pad[7].  The Linux kernel ignores bytes 8-15
 * (EI_ABIVERSION + EI_PAD) entirely, so we can overwrite them without
 * breaking execution.
 *
 * We place the first 8 bytes of the PDF header ("%PDF-1.7") there,
 * dropping only the trailing newline.  The rest of the PDF (from byte M
 * onward, where M = length of the first line including "\n") is appended
 * after the ELF.
 *
 * Object at original PDF offset O lands at (elf->size + O - M) in the
 * output, so delta = elf->size - M.
 */
static Buf combine_elf_pdf(const File *elf, const File *pdf) {
    if (elf->size < 16)
        die("ELF: file too small to patch ident");
    if (pdf->size < 5 || memcmp(pdf->data, "%PDF-", 5) != 0)
        die("PDF: file does not start with %%PDF-");

    size_t M = 0;
    while (M < pdf->size && pdf->data[M] != '\n') M++;
    if (M < pdf->size) M++;   /* include the \n in the stripped prefix */
    if (M < 5)
        die("PDF: header line too short");

    size_t delta = elf->size - M;

    Buf out; buf_init(&out);
    buf_append(&out, elf->data, elf->size);
    /* patch ident[8..15] with first 8 bytes of PDF header (no \n) */
    size_t patch = (M - 1) < 8 ? (M - 1) : 8;
    memcpy(out.data + 8, pdf->data, patch);
    if (patch < 8) memset(out.data + 8 + patch, 0, 8 - patch);

    Buf pmod = pdf_with_delta(pdf, delta, M);
    buf_append(&out, pmod.data, pmod.len);
    buf_free(&pmod);
    return out;
}

static Buf combine_elf_pdf_zip(const File *elf, const File *pdf, const File *zip) {
    Buf ep = combine_elf_pdf(elf, pdf);
    Buf zp = zip_with_delta(zip, ep.len);
    Buf out; buf_init(&out);
    buf_append(&out, ep.data, ep.len);
    buf_append(&out, zp.data, zp.len);
    buf_free(&ep);
    buf_free(&zp);
    return out;
}

/* ── PE/COFF combinations ───────────────────────────────────────── */

/*
 * PE+ZIP: the Windows loader maps only the sections listed in the section
 * table and ignores any trailing bytes (the "overlay"), so ZIP appends
 * cleanly just like it does after an ELF or JPEG.
 */
static Buf combine_pe_zip(const File *pe, const File *zip) {
    Buf out; buf_init(&out);
    buf_append(&out, pe->data, pe->size);
    Buf zp = zip_with_delta(zip, pe->size);
    buf_append(&out, zp.data, zp.len);
    buf_free(&zp);
    return out;
}

/*
 * PE+PDF: smuggle %PDF into the MZ reserved field e_res2 at offset 0x28.
 *
 * The DOS/MZ header has 20 bytes of reserved zeros at e_res2 (0x28–0x3B).
 * The Windows PE loader only uses the MZ magic (0x00) and e_lfanew (0x3C),
 * so bytes 0x28–0x2F are safe to overwrite with the first 8 bytes of the
 * PDF header line (e.g. "%PDF-1.7"), placing %PDF within the first 64 bytes
 * where all PDF parsers will find it.
 *
 * delta = pe->size - M  (same arithmetic as ELF+PDF).
 */
static Buf combine_pe_pdf(const File *pe, const File *pdf) {
    if (pe->size < 0x40)
        die("PE: file too small to contain MZ header reserved fields");
    if (pdf->size < 5 || memcmp(pdf->data, "%PDF-", 5) != 0)
        die("PDF: file does not start with %%PDF-");

    size_t M = 0;
    while (M < pdf->size && pdf->data[M] != '\n') M++;
    if (M < pdf->size) M++;
    if (M < 5)
        die("PDF: header line too short");

    size_t delta = pe->size - M;

    Buf out; buf_init(&out);
    buf_append(&out, pe->data, pe->size);
    size_t patch = (M - 1) < 8 ? (M - 1) : 8;
    memcpy(out.data + 0x28, pdf->data, patch);
    if (patch < 8) memset(out.data + 0x28 + patch, 0, 8 - patch);

    Buf pmod = pdf_with_delta(pdf, delta, M);
    buf_append(&out, pmod.data, pmod.len);
    buf_free(&pmod);
    return out;
}

static Buf combine_pe_pdf_zip(const File *pe, const File *pdf, const File *zip) {
    Buf ep = combine_pe_pdf(pe, pdf);
    Buf zp = zip_with_delta(zip, ep.len);
    Buf out; buf_init(&out);
    buf_append(&out, ep.data, ep.len);
    buf_append(&out, zp.data, zp.len);
    buf_free(&ep);
    buf_free(&zp);
    return out;
}

/* ── main ───────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    if (argc < 4 || argc > 5) {
        fprintf(stderr,
            "usage: polypocket <output> <file1> <file2> [file3]\n"
            "  builds a polyglot file valid in all supplied formats\n"
            "  supported types: pdf, jpeg/jpg, zip, elf, png, pe/exe, gif, mp3, ps\n"
            "  note: jpeg/elf/png/pe/gif/mp3/ps cannot be combined with each other\n"
            "        (all require specific magic bytes at offset 0)\n");
        return 1;
    }

    const char *outpath = argv[1];
    int ninputs = argc - 2;

    File   inputs[3];
    FileType types[3];
    for (int i = 0; i < ninputs; i++) {
        inputs[i] = read_file(argv[2 + i]);
        types[i]  = detect_type(&inputs[i], argv[2 + i]);
    }

    for (int i = 0; i < ninputs; i++)
        for (int j = i + 1; j < ninputs; j++)
            if (types[i] == types[j])
                die("duplicate input type (%s and %s are both %s)",
                    argv[2+i], argv[2+j],
                    types[i] == FT_PDF  ? "PDF"  :
                    types[i] == FT_JPEG ? "JPEG" :
                    types[i] == FT_ZIP  ? "ZIP"  :
                    types[i] == FT_ELF  ? "ELF"  :
                    types[i] == FT_PNG  ? "PNG"  :
                    types[i] == FT_PE   ? "PE"   :
                    types[i] == FT_GIF  ? "GIF"  :
                    types[i] == FT_MP3  ? "MP3"  : "PS");

    File *pdf = NULL, *jpeg = NULL, *zip = NULL, *elf = NULL;
    File *png = NULL, *pe = NULL, *gif = NULL, *mp3 = NULL, *ps = NULL;
    for (int i = 0; i < ninputs; i++) {
        if (types[i] == FT_PDF)  pdf  = &inputs[i];
        if (types[i] == FT_JPEG) jpeg = &inputs[i];
        if (types[i] == FT_ZIP)  zip  = &inputs[i];
        if (types[i] == FT_ELF)  elf  = &inputs[i];
        if (types[i] == FT_PNG)  png  = &inputs[i];
        if (types[i] == FT_PE)   pe   = &inputs[i];
        if (types[i] == FT_GIF)  gif  = &inputs[i];
        if (types[i] == FT_MP3)  mp3  = &inputs[i];
        if (types[i] == FT_PS)   ps   = &inputs[i];
    }

    /* jpeg/elf/png/pe/gif/mp3/ps all need specific magic at byte 0 — only one allowed */
    if ((!!jpeg + !!elf + !!png + !!pe + !!gif + !!mp3 + !!ps) > 1)
        die("cannot combine jpeg/elf/png/pe/gif/mp3/ps with each other "
            "(all require specific magic bytes at offset 0)");

    Buf out; buf_init(&out);

    if      (jpeg && pdf && zip) out = combine_jpeg_pdf_zip(jpeg, pdf, zip);
    else if (elf  && pdf && zip) out = combine_elf_pdf_zip(elf, pdf, zip);
    else if (png  && pdf && zip) out = combine_png_pdf_zip(png, pdf, zip);
    else if (pe   && pdf && zip) out = combine_pe_pdf_zip(pe, pdf, zip);
    else if (gif  && pdf && zip) out = combine_gif_pdf_zip(gif, pdf, zip);
    else if (mp3  && pdf && zip) out = combine_mp3_pdf_zip(mp3, pdf, zip);
    else if (ps   && pdf && zip) out = combine_ps_pdf_zip(ps, pdf, zip);
    else if (jpeg && pdf)        out = combine_jpeg_pdf(jpeg, pdf);
    else if (jpeg && zip)        out = combine_jpeg_zip(jpeg, zip);
    else if (pdf  && zip)        out = combine_pdf_zip(pdf, zip);
    else if (elf  && pdf)        out = combine_elf_pdf(elf, pdf);
    else if (elf  && zip)        out = combine_elf_zip(elf, zip);
    else if (png  && pdf)        out = combine_png_pdf(png, pdf);
    else if (png  && zip)        out = combine_png_zip(png, zip);
    else if (pe   && pdf)        out = combine_pe_pdf(pe, pdf);
    else if (pe   && zip)        out = combine_pe_zip(pe, zip);
    else if (gif  && pdf)        out = combine_gif_pdf(gif, pdf);
    else if (gif  && zip)        out = combine_gif_zip(gif, zip);
    else if (mp3  && pdf)        out = combine_mp3_pdf(mp3, pdf);
    else if (mp3  && zip)        out = combine_mp3_zip(mp3, zip);
    else if (ps   && pdf)        out = combine_ps_pdf(ps, pdf);
    else if (ps   && zip)        out = combine_ps_zip(ps, zip);
    else die("no supported combination (need pdf/jpeg/zip/elf/png/pe/gif/mp3/ps)");

    write_file(outpath, &out);
    printf("wrote %s (%zu bytes)\n", outpath, out.len);

    buf_free(&out);
    for (int i = 0; i < ninputs; i++) free(inputs[i].data);
    return 0;
}
