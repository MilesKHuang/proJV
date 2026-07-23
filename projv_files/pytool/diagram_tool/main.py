"""
diagram_tool -- generate diagrams from Graphviz DOT language.
Auto-downloads dot.exe if not found. Outputs PNG/SVG to doc/ by default.
Input: JSON via stdin: code, output (opt), format (opt), path (opt).
"""
import sys, json, os, tempfile, subprocess, shutil


def find_dot() -> str:
    """Find dot: PATH -> tool_dir/bin/ -> download"""
    dot = shutil.which("dot")
    if dot:
        return dot
    tool_dir = os.environ.get("PROJV_TOOL_DIR", "")
    if tool_dir:
        local = os.path.join(tool_dir, "bin", "dot.exe")
        if os.path.isfile(local):
            return local
    return ""


def download_dot() -> str:
    """Download portable Graphviz, extract dot.exe + DLLs to tool_dir/bin/. Returns dot path or empty."""
    tool_dir = os.environ.get("PROJV_TOOL_DIR", "")
    if not tool_dir:
        return ""
    bin_dir = os.path.join(tool_dir, "bin")
    os.makedirs(bin_dir, exist_ok=True)
    dot_exe = os.path.join(bin_dir, "dot.exe")
    if os.path.isfile(dot_exe):
        return dot_exe

    # Download portable Graphviz from GitLab releases
    url = ("https://gitlab.com/api/v4/projects/4207231/packages/generic/"
           "graphviz-releases/12.2.1/windows_10_cmake_Release_Graphviz-12.2.1-win64.zip")
    import urllib.request
    try:
        print("[diagram_tool] Downloading Graphviz (portable, ~30MB)...", file=sys.stderr)
        tmp_zip = os.path.join(tempfile.gettempdir(), "graphviz_portable.zip")
        urllib.request.urlretrieve(url, tmp_zip)
    except Exception as e:
        print("[diagram_tool] Download failed: %s" % e, file=sys.stderr)
        return ""

    # Extract dot.exe and all DLLs from bin/
    import zipfile
    tmp_extract = os.path.join(tempfile.gettempdir(), "graphviz_extract")
    try:
        with zipfile.ZipFile(tmp_zip, 'r') as zf:
            for name in zf.namelist():
                parts = name.replace("\\", "/").split("/")
                if len(parts) >= 2 and parts[-2] == "bin":
                    fname = parts[-1]
                    if fname.endswith((".exe", ".dll")) or fname == "config6":
                        zf.extract(name, tmp_extract)
                        src = os.path.join(tmp_extract, name)
                        dst = os.path.join(bin_dir, fname)
                        if not os.path.isfile(dst):
                            shutil.copy2(src, dst)
        os.unlink(tmp_zip)
        shutil.rmtree(tmp_extract, ignore_errors=True)
    except Exception as e:
        print("[diagram_tool] Extraction failed: %s" % e, file=sys.stderr)
        return ""

    if os.path.isfile(dot_exe):
        return dot_exe
    return ""


def run(args: dict) -> str:
    code = args.get("code", "")
    output = args.get("output", "diagram")
    fmt = args.get("format", "png")

    if not code or not code.strip():
        return "Error: 'code' parameter is empty."
    if fmt not in ("png", "svg"):
        return "Error: unsupported format '%s'. Use 'png' or 'svg'." % fmt

    # output can include path components (e.g. "proJV/doc/diagram")
    workspace = os.environ.get("PROJV_WORKSPACE", os.getcwd())
    out_path = os.path.join(workspace, "%s.%s" % (output, fmt))
    out_dir = os.path.dirname(out_path)
    os.makedirs(out_dir, exist_ok=True)

    # Write DOT to temp file
    try:
        fd, dot_path = tempfile.mkstemp(suffix=".dot", prefix="diagram_")
        with os.fdopen(fd, 'w', encoding='utf-8') as f:
            f.write(code)
    except OSError as e:
        return "Error: failed to write temp DOT file: %s" % e

    # Find or download dot
    dot = find_dot()
    if not dot:
        dot = download_dot()
    if not dot:
        os.unlink(dot_path)
        return (
            "Graphviz 'dot' not found and could not be downloaded.\n"
            "Install manually: https://graphviz.org/download/\n"
            "Or: winget install graphviz\n\n"
            "--- DOT code ---\n%s\n--- end DOT ---" % code
        )

    # Render with dot
    try:
        subprocess.run(
            [dot, "-T%s" % fmt, "-o%s" % out_path, dot_path],
            check=True, capture_output=True, text=True, timeout=30
        )
        os.unlink(dot_path)
        return "Diagram saved to: %s\nFormat: %s\nSize: %d bytes" % (
            out_path, fmt, os.path.getsize(out_path))
    except FileNotFoundError:
        os.unlink(dot_path)
        return "Error: dot executable not found at: %s" % dot
    except subprocess.CalledProcessError as e:
        os.unlink(dot_path)
        stderr = e.stderr.strip() if e.stderr else ""
        return (
            "Graphviz rendering failed (DOT syntax error).\n"
            "Error: %s\n\n"
            "--- DOT code ---\n%s\n--- end DOT ---" % (stderr, code)
        )
    except Exception as e:
        os.unlink(dot_path)
        return "Error: %s\n\n--- DOT code ---\n%s\n--- end DOT ---" % (e, code)


def main():
    try:
        raw = sys.stdin.read()
        args = json.loads(raw)
        result = run(args)
        print(result)
    except json.JSONDecodeError as e:
        print("Error: invalid JSON args: %s" % e, file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print("Error: %s" % e, file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
