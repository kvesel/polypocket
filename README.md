# polypocket

Combine PDF, JPEG, ZIP, PNG, GIF, EXE, PostScript, MP3 and/or ELF files into a single polyglot file that is simultaneously valid in all provided formats. Pass it to a PDF reader, an image viewer, `unzip`, or run it directly — each tool sees its own format.

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
| GIF + ZIP | `unzip` extracts the archive; image viewers show the animation |
| GIF + PDF | Image viewers show the animation; PDF readers open the document |
| GIF + PDF + ZIP | All three work simultaneously |
| MP3 + ZIP | `unzip` extracts the archive; media players play the audio |
| MP3 + PDF | Media players play the audio; PDF readers open the document |
| MP3 + PDF + ZIP | All three work simultaneously |
| PS + ZIP | `unzip` extracts the archive; PostScript interpreters render the page |
| PS + PDF | PostScript interpreters render the page; PDF readers open the document |
| PS + PDF + ZIP | All three work simultaneously |

## Exploitability matrix

Known CVEs and in-the-wild campaigns that use the same structural techniques polypocket produces.

| Combo | CVE | In-wild use |
|-------|-----|-------------|
| PE + ZIP | CVE-2020-1464, CVE-2020-1599, CVE-2021-26413 | ZLoader, Batloader |
| GIF + ZIP | GIFAR (JRE 6 CVE, 2008) | SyncCrypt ransomware |
| PDF + ZIP | None; MalDoc-in-PDF pattern | IcedID, Batloader |
| JPEG + ZIP/PDF | None; JPEG+PHP upload bypass pattern | 2023 SaaS RCE |
| PNG + ZIP | Analogous to CVE-2019-11687 | DarkTrack RAT |
| ELF + PDF | Analogous to CVE-2019-11687 | PoC on medical devices |
| MP3 + PDF | None found | None documented |
| PS + PDF | None found | None documented |

**CVE-2020-1464 "GlueBall"** (CVSS 7.8, CISA KEV): `WinVerifyTrust` validates the PE/MSI prefix and ignores the appended ZIP, so a legitimately-signed PE can carry a malicious JAR payload executed by a second runtime. CVE-2020-1599 and CVE-2021-26413 are related Authenticode/MSI signing bypass variants in the same family.

**GIFAR**: GIF header at the front, JAR/ZIP index at the back — used to bypass the Java applet same-origin policy. SyncCrypt ransomware (2017) and DarkTrack RAT applied the same technique with JPEG+ZIP and PNG+ZIP respectively.

**CVE-2019-11687** (DICOM preamble): the DICOM spec reserves a 128-byte preamble for arbitrary content; researchers embedded full PE or ELF headers there (PEDICOM/ELFDICOM), producing a file that displays as a medical image and executes as a binary. Structurally analogous to polypocket's ELF ident padding ([8..15]) and PNG tEXt chunk injection techniques.

## Limitations

- **PDF cross-reference streams** (PDF 1.5+, linearized PDFs) are not supported. If `polypocket` exits with an error about xref streams, downgrade the PDF first:
  ```
  gs -dNOPAUSE -dBATCH -sDEVICE=pdfwrite -dCompatibilityLevel=1.4 \
     -sOutputFile=compat.pdf in.pdf
  ```

- **ELF, JPEG, PNG, PE, GIF, MP3, and PS** cannot be combined with each other — all require specific magic bytes at offset 0. Each may be freely combined with PDF and/or ZIP.
- **MP3** must be an ID3v2-tagged file (i.e. starts with `ID3`). ID3v2 extended headers and ID3v2.4 footers are not supported. Raw MPEG-frame-only files are not supported for the PDF combination (ZIP still works by appending).
- Some image viewers (particularly multi-format viewers) may detect the embedded `%PDF` signature in the JPEG comment and attempt to render both formats.
- **PS + PDF** relies on PostScript interpreters ignoring data after `%%EOF`. Interpreters that scan past `%%EOF` may produce errors from the trailing PDF body.
