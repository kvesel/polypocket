# polypocket

Combine PDF, JPEG, ZIP, PNG, EXE and/or ELF files into a single polyglot file that is simultaneously valid in all provided formats. Pass it to a PDF reader, an image viewer, `unzip`, or run it directly — each tool sees its own format.

## Build

```
gcc polypocket.c -o polypocket
```

## Usage

```
./polypocket OUTPUT INPUT [INPUT ...]
```

Provide 2–3 input files. Types are auto-detected from magic bytes (not file extension), so the order and names don't matter.

```bash
./polypocket out.poly test.pdf test.zip
./polypocket out.poly test.exe test.pdf
./polypocket out.poly test.png test.pdf test.zip
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
| PNG + ZIP | `unzip` extracts the archive; image viewers show the photo |
| PNG + PDF | Image viewers show the photo; PDF readers open the document |
| PNG + PDF + ZIP | All three work simultaneously |
| PE + ZIP | `unzip` extracts the archive; the binary executes normally |
| PE + PDF | PDF readers open the document; the binary executes normally |
| PE + PDF + ZIP | All three work simultaneously |

## Limitations

- **PDF cross-reference streams** (PDF 1.5+, linearized PDFs) are not supported. If `polypocket` exits with an error about xref streams, downgrade the PDF first:
  ```
  gs -dNOPAUSE -dBATCH -sDEVICE=pdfwrite -dCompatibilityLevel=1.4 \
     -sOutputFile=compat.pdf in.pdf
  ```
- **Zip64** archives are supported. Standard 32-bit ZIP archives are also supported.
- **ELF + JPEG, ELF + PNG, ELF + PE, JPEG + PNG, JPEG + PE, PNG + PE** are not supported — each of these formats requires specific magic bytes at offset 0.
- Some image viewers (particularly multi-format viewers) may detect the embedded `%PDF` signature in the JPEG comment and attempt to render both formats.
