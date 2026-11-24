"""The game's electronic manual from the user's own NSP or XCI, as <work_dir>/manual/ (the title screen's Manual
button opens its index.html in the PC's browser: runtime/src/web.c).

    python -m tools.yw4.manual

The manual is game data (rule 1): it is never in the repo. On a Switch it is a set of HTML pages in the game's
"HtmlDocument" NCA, shown by the system's offline web applet (nn::web::ShowOfflineHtmlPage with a path such as
"manual.htdocs/index.html", relative to the NCA's html-document/ folder). The NSP holds that NCA next to the program;
extracted ExeFS/RomFS folders don't. tools.yw4.extract makes it too, from the NCAs it already unpacked.

- Only the small NCAs are unpacked (icon.unpack_small: a manual is a few megabytes). hactool calls both the
  HtmlDocument and the LegalInformation NCA "Manual"; the one whose RomFS has html-document/ is the manual.
- The update's is preferred over the base game's, as for the icon: it would carry 4++'s pages if it has its own.
- manual.html: the 17 pages in one page in the site's look (owner, 2026-11-19), our template manual_page.html filled
  with each page's title and content (#inner), its pictures pointed at manual.htdocs/common/img; the Switch-only
  parts (jQuery, the page-top button, next / back links) are left out. One page, so a browser's Translate reads all
  of it at once. `python -m tools.yw4.manual --page-only` remakes it from work/manual.
"""

from __future__ import annotations

import argparse
import base64
import html
import re
import shutil
import sys
import tempfile
from pathlib import Path

from . import config, extract, icon, pfs0, xci


class ManualError(Exception):
    pass


TEMPLATE = Path(__file__).with_name("manual_page.html")


def site_assets() -> dict[str, str]:
    """The site's icon and background patterns (ours, docs/), as data URLs for the template: the page is a local file
    and works offline. Missing ones (a checkout without docs/) are left empty: the page still reads."""
    docs = config.REPO_ROOT / "docs"
    files = {"{{ICON}}": ("img/icon-192.png", "image/png"), "{{PATTERN}}": ("icons.svg", "image/svg+xml"),
             "{{PATTERN_DARK}}": ("icons-dark.svg", "image/svg+xml")}
    out = {}
    for marker, (name, mime) in files.items():
        f = docs / name
        out[marker] = f"data:{mime};base64,{base64.b64encode(f.read_bytes()).decode()}" if f.is_file() else ""
    return out


def balance_divs(body: str) -> str:
    """A page's content with its <div>s balanced: a closing one with nothing open is dropped, missing ones are added.
    The game's page06 has one </div> too many (the Switch's browser let it pass); in one page with the others it
    closed the layout, and everything after it lost the side list's column (owner's screenshot, 2026-11-19)."""
    out, depth, at = [], 0, 0
    for m in re.finditer(r"<div\b[^>]*>|</div\s*>", body):
        out.append(body[at:m.start()])
        at = m.end()
        if m.group().startswith("</"):
            if depth == 0:
                continue  # nothing open: the stray one
            depth -= 1
        else:
            depth += 1
        out.append(m.group())
    out.append(body[at:])
    return "".join(out) + "</div>" * depth


def build_page(out: Path) -> Path:
    """<out>/manual.html from <out>/manual.htdocs (the pages in the order of their own list)."""
    docs = out / "manual.htdocs"
    index = (docs / "index.html").read_text(encoding="utf-8", errors="replace")
    nav_block = index[index.find("<nav"):index.find("</nav>")]  # the page list (the footer's "next" link isn't one)
    entries = re.findall(r'<li class="([^"]+)"><a href="([^"]+\.html)">([^<]+)</a></li>', nav_block)
    if not entries:
        raise ManualError("the manual's index has no page list")
    nav, pages = [], []
    for anchor, href, title in entries:
        src = (docs / href).read_text(encoding="utf-8", errors="replace")
        a, b = src.find('<div id="inner">'), src.find('<footer id="footer">')
        if a < 0 or b < 0:
            continue
        body = src[a + len('<div id="inner">'):b]
        body = balance_divs(re.sub(r"(?:\s*</div>){2}\s*$", "", body))  # #inner's and .wrap's ends
        body = re.sub(r'(src|href)="(?:\./)?common/', r'\1="manual.htdocs/common/', body)
        body = re.sub(r'\s(?:onclick|onload|onmouseover)="[^"]*"', "", body)
        body = re.sub(r'href="javascript:[^"]*"', 'href="#"', body)
        body = re.sub(r"<script\b.*?</script>", "", body, flags=re.S)
        nav.append(f'<li><a href="#{html.escape(anchor)}">{title}</a></li>')
        pages.append(f'<section class="page" id="{html.escape(anchor)}">\n<h1>{title}</h1>\n{body.strip()}\n</section>')
    page = TEMPLATE.read_text(encoding="utf-8")
    for marker, value in site_assets().items():
        page = page.replace(marker, value)
    page = page.replace("{{NAV}}", "\n".join(nav)).replace("{{PAGES}}", "\n".join(pages))
    (out / "manual.html").write_text(page, encoding="utf-8")
    return out / "manual.html"


def make(cfg: config.Config, home: Path, ncas: list[Path]) -> Path:
    """<work_dir>/manual/ (the html-document folder's contents) from the first HtmlDocument NCA in `ncas` (preferred
    first; hactool's keys already in `home`). Returns the folder."""
    out = cfg.work_dir / "manual"
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        for n in ncas:
            if not re.search(r"Content Type:\s+Manual", extract.run_hactool(cfg.hactool_exe, home, ["-i", str(n)])):
                continue
            romfs = tmp / n.stem
            extract.run_hactool(cfg.hactool_exe, home, [f"--romfsdir={romfs}", str(n)])
            doc = romfs / "html-document"
            if not doc.is_dir():
                continue  # the legal information
            if out.exists():
                shutil.rmtree(out)
            shutil.copytree(doc, out)
            build_page(out)
            return out
    raise ManualError("no HtmlDocument NCA (the game's manual) in these files")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--page-only", action="store_true", help="remake manual.html from work/manual")
    args = ap.parse_args(argv)
    try:
        cfg = config.load(require_exefs=False)
        if args.page_only:
            print(f"wrote {build_page(cfg.work_dir / 'manual')}")
            return 0
        files = [p for p in (cfg.update_nsp, cfg.base_nsp) if p is not None]  # the update's first
        if not files or cfg.hactool_exe is None or cfg.keys_dir is None:
            raise config.ConfigError("needs base_nsp (or update_nsp), hactool_exe and keys_dir: the manual is only "
                                     "in an NSP or XCI, not in extracted folders")
        nca_dir = cfg.work_dir / "manual-nca"
        nca_dir.mkdir(parents=True, exist_ok=True)
        try:
            ncas, rids, tkeys = [], set(), {}
            for tag, p in zip(("upd", "base") if cfg.update_nsp else ("base",), files):
                n, r = icon.unpack_small(p, nca_dir, tag)
                ncas, rids = ncas + n, rids | r
                tkeys |= extract.ticket_keys_of(p)
            with tempfile.TemporaryDirectory() as t:
                home = Path(t)
                extract.write_temp_keys(cfg.keys_dir, rids, home, tkeys)
                out = make(cfg, home, ncas)
        finally:
            shutil.rmtree(nca_dir, ignore_errors=True)
    except (config.ConfigError, ManualError, extract.ExtractError, pfs0.Pfs0Error, xci.XciError) as e:
        print(f"manual: {e}", file=sys.stderr)
        return 1
    pages = sum(1 for _ in (out / "manual.htdocs").glob("*.htm*"))
    print(f"wrote {out} ({pages} pages, one page: manual.html)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
