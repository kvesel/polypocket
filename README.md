# polypocket

Combine PDF, JPEG, and/or ZIP files into a single polyglot file that is simultaneously valid in all provided formats. Pass it to a PDF reader, an image viewer, or `unzip` — each tool sees its own format.

## Build

```
cc -O2 -o polypocket polypocket.c
```

## Usage

```
./polypocket OUTPUT INPUT [INPUT ...]
```

Provide 2–3 input files. Types are auto-detected from magic bytes (not file extension), so the order and names don't matter. Every supported combination of PDF, JPEG, and ZIP is handled.

```bash
./polypocket out.poly test.pdf test.zip
./polypocket out.poly test.jpg test.zip
./polypocket out.poly test.jpg test.pdf
./polypocket out.poly test.jpg test.pdf test.zip
```

## Supported combinations

| Inputs | Output behaviour |
|--------|-----------------|
| PDF + ZIP | `unzip` extracts the archive; PDF readers open the document |
| JPEG + ZIP | `unzip` extracts the archive; image viewers show the photo |
| JPEG + PDF | Image viewers show the photo; PDF readers open the document |
| JPEG + PDF + ZIP | All three work simultaneously |

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
```

## Limitations

- **PDF cross-reference streams** (PDF 1.5+, linearized PDFs) are not supported. If `polypocket` exits with an error about xref streams, downgrade the PDF first:
  ```
  gs -dNOPAUSE -dBATCH -sDEVICE=pdfwrite -dCompatibilityLevel=1.4 \
     -sOutputFile=compat.pdf in.pdf
  ```
- **Zip64** extended format is not supported. ZIP archives must use standard 32-bit offsets.
- Some image viewers (particularly multi-format viewers) may detect the embedded `%PDF` signature in the JPEG comment and attempt to render both formats.
