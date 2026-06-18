# polypocket

Combine PDF, JPEG, ZIP, and/or ELF files into a single polyglot file that is simultaneously valid in all provided formats. Pass it to a PDF reader, an image viewer, `unzip`, or run it directly — each tool sees its own format.

## Build

```
cc -O2 -o polypocket polypocket.c
```

## Usage

```
./polypocket OUTPUT INPUT [INPUT ...]
```

Provide 2–3 input files. Types are auto-detected from magic bytes (not file extension), so the order and names don't matter.

```bash
./polypocket out.poly test.pdf test.zip
./polypocket out.poly test.jpg test.zip
./polypocket out.poly test.jpg test.pdf
./polypocket out.poly test.jpg test.pdf test.zip
./polypocket out.poly hello test.zip
./polypocket out.poly hello test.pdf
./polypocket out.poly hello test.pdf test.zip
```

## Supported combinations

| Inputs | Output behaviour |
|--------|-----------------|
| PDF + ZIP | `unzip` extracts the archive; PDF readers open the document |
| JPEG + ZIP | `unzip` extracts the archive; image viewers show the photo |
| JPEG + PDF | Image viewers show the photo; PDF readers open the document |
| JPEG + PDF + ZIP | All three work simultaneously |
| ELF + ZIP | `unzip` extracts the archive; the binary executes normally |
| ELF + PDF | PDF readers open the document; the binary executes normally |
| ELF + PDF + ZIP | All three work simultaneously |

ELF cannot be combined with JPEG — both formats require specific magic bytes at offset 0.

## Verification

```bash
# Detect all embedded formats
file -k out.poly

# Open as ZIP
unzip -l out.poly

# Open as PDF (requires poppler-utils)
pdfinfo out.poly

# Open as JPEG
identify out.poly          # ImageMagick
exiftool out.poly

# Open as ELF
readelf -h out.poly
chmod +x out.poly && ./out.poly
```

## How ELF embedding works

The ELF ident header has 8 bytes of padding at offsets 8–15 (`EI_ABIVERSION` + `EI_PAD`) that the Linux kernel loader ignores entirely. `polypocket` overwrites those 8 bytes with the first 8 bytes of the PDF header (`%PDF-1.7`), placing the PDF magic within the file's first 1024 bytes where PDF parsers expect it. The remaining PDF content is appended after the ELF with xref offsets adjusted accordingly. ZIP data is appended after that.

## Limitations

- **PDF cross-reference streams** (PDF 1.5+, linearized PDFs) are not supported. If `polypocket` exits with an error about xref streams, downgrade the PDF first:
  ```
  gs -dNOPAUSE -dBATCH -sDEVICE=pdfwrite -dCompatibilityLevel=1.4 \
     -sOutputFile=compat.pdf in.pdf
  ```
- **Zip64** extended format is not supported. ZIP archives must use standard 32-bit offsets.
- **ELF + JPEG** is not supported — both formats require specific magic bytes at offset 0.
- Some image viewers (particularly multi-format viewers) may detect the embedded `%PDF` signature in the JPEG comment and attempt to render both formats.
