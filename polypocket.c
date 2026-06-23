/*
 * polypocket.c — polyglot file builder for PDF, JPEG, ZIP, and ELF
 *
 * Usage: polypocket output input1 input2 [input3]
 * Builds a single output file that is simultaneously valid in all
 * supplied formats (any combination of pdf/jpeg/zip/elf).
 *
 * Compile: cc -O2 -o polypocket polypocket.c
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

typedef enum { FT_PDF, FT_JPEG, FT_ZIP, FT_ELF } FileType;

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
static uint16_t r16be(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static void w16be(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
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
 * Return a new Buf containing the ZIP data with all local-header
 * offsets shifted up by `delta` bytes (to account for data prepended
 * in the output file before the ZIP block).
 */
static Buf zip_with_delta(const File *zip, size_t delta) {
    size_t eocd_off   = find_eocd(zip->data, zip->size);
    uint32_t cd_size  = r32le(zip->data + eocd_off + 12);
    uint32_t cd_off   = r32le(zip->data + eocd_off + 16);
    uint16_t cmt_len  = r16le(zip->data + eocd_off + 20);

    Buf out;
    buf_init(&out);

    /* local file entries — copy verbatim */
    buf_append(&out, zip->data, (size_t)cd_off);

    /* rebuild central directory entries with patched local-header offsets */
    size_t cd_start = out.len;
    const uint8_t *cd = zip->data + cd_off;
    size_t pos = 0;
    while (pos + 4 <= (size_t)cd_size &&
           memcmp(cd + pos, "PK\x01\x02", 4) == 0) {
        uint16_t fn = r16le(cd + pos + 28);
        uint16_t ex = r16le(cd + pos + 30);
        uint16_t cm = r16le(cd + pos + 32);
        size_t   es = 46u + fn + ex + cm;
        size_t   wo = out.len;        /* write offset of this entry */
        buf_append(&out, cd + pos, es);
        /* patch local-header offset field at +42 within the entry */
        uint32_t new_off = r32le(cd + pos + 42) + (uint32_t)delta;
        w32le(out.data + wo + 42, new_off);
        pos += es;
    }

    /* rebuild EOCD */
    uint8_t eocd[22];
    memcpy(eocd, zip->data + eocd_off, 22);
    w32le(eocd + 12, (uint32_t)(out.len - cd_start)); /* new cd size  */
    w32le(eocd + 16, (uint32_t)(cd_start + delta));   /* new cd offset in output file */
    buf_append(&out, eocd, 22);
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

/* ── main ───────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    if (argc < 4 || argc > 5) {
        fprintf(stderr,
            "usage: polypocket <output> <file1> <file2> [file3]\n"
            "  builds a polyglot file valid in all supplied formats\n"
            "  supported types: pdf, jpeg/jpg, zip, elf\n"
            "  note: elf cannot be combined with jpeg (both require magic at byte 0)\n");
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
                    types[i] == FT_ZIP  ? "ZIP"  : "ELF");

    File *pdf = NULL, *jpeg = NULL, *zip = NULL, *elf = NULL;
    for (int i = 0; i < ninputs; i++) {
        if (types[i] == FT_PDF)  pdf  = &inputs[i];
        if (types[i] == FT_JPEG) jpeg = &inputs[i];
        if (types[i] == FT_ZIP)  zip  = &inputs[i];
        if (types[i] == FT_ELF)  elf  = &inputs[i];
    }

    if (elf && jpeg)
        die("ELF and JPEG cannot be combined (both require specific magic at byte 0)");

    Buf out; buf_init(&out);

    if      (jpeg && pdf && zip) out = combine_jpeg_pdf_zip(jpeg, pdf, zip);
    else if (elf  && pdf && zip) out = combine_elf_pdf_zip(elf, pdf, zip);
    else if (jpeg && pdf)        out = combine_jpeg_pdf(jpeg, pdf);
    else if (jpeg && zip)        out = combine_jpeg_zip(jpeg, zip);
    else if (pdf  && zip)        out = combine_pdf_zip(pdf, zip);
    else if (elf  && pdf)        out = combine_elf_pdf(elf, pdf);
    else if (elf  && zip)        out = combine_elf_zip(elf, zip);
    else die("no supported combination (need pdf/jpeg/zip/elf)");

    write_file(outpath, &out);
    printf("wrote %s (%zu bytes)\n", outpath, out.len);

    buf_free(&out);
    for (int i = 0; i < ninputs; i++) free(inputs[i].data);
    return 0;
}
