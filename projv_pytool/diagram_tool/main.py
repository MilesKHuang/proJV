"""
diagram_tool -- generate diagrams from Graphviz DOT language code.
Input: JSON via stdin with keys: code, output (opt), format (opt).
Output: rendered diagram file path, or DOT text + error info.
"""
import sys, json, os, tempfile, subprocess


def run(args: dict) -> str:
    code = args.get("code", "")
    output = args.get("output", "diagram")
    fmt = args.get("format", "png")

    if not code or not code.strip():
        return "Error: 'code' parameter is empty. Provide Graphviz DOT language code."

    if fmt not in ("png", "svg"):
        return f"Error: unsupported format '{fmt}'. Use 'png' or 'svg'."

    workspace = os.environ.get("PROJV_WORKSPACE", os.getcwd())
    out_path = os.path.join(workspace, f"{output}.{fmt}")

    # Write DOT to temp file
    try:
        fd, dot_path = tempfile.mkstemp(suffix=".dot", prefix="diagram_")
        with os.fdopen(fd, 'w', encoding='utf-8') as f:
            f.write(code)
    except OSError as e:
        return f"Error: failed to write temp DOT file: {e}"

    # Strategy 1: try system 'dot' command (Graphviz)
    try:
        subprocess.run(
            ["dot", f"-T{fmt}", f"-o{out_path}", dot_path],
            check=True, capture_output=True, text=True, timeout=30
        )
        os.unlink(dot_path)
        return f"Diagram saved to: {out_path}\nFormat: {fmt}\nSize: {os.path.getsize(out_path)} bytes"
    except FileNotFoundError:
        pass  # dot not installed
    except subprocess.CalledProcessError as e:
        # dot is installed but rendering failed -- DOT syntax error likely
        os.unlink(dot_path)
        stderr = e.stderr.strip() if e.stderr else ""
        return (
            f"Graphviz rendering failed (syntax error in DOT code).\n\n"
            f"Error: {stderr}\n\n"
            f"--- DOT code ---\n{code}\n--- end DOT ---"
        )
    except Exception as e:
        pass  # fall through to next strategy

    # Strategy 2: try Python graphviz library
    try:
        import graphviz
    except ImportError:
        os.unlink(dot_path)
        return (
            "Graphviz is not installed.\n"
            "Install system Graphviz: choco install graphviz / scoop install graphviz\n"
            "Or install Python library: pip install graphviz\n\n"
            f"--- DOT code ---\n{code}\n--- end DOT ---"
        )

    try:
        # Use graphviz library to render
        gv = graphviz.Source(code, format=fmt)
        gv.render(filename=output, directory=workspace, cleanup=True)
        os.unlink(dot_path)
        final_path = os.path.join(workspace, f"{output}.{fmt}")
        return f"Diagram saved to: {final_path}\nFormat: {fmt}\nSize: {os.path.getsize(final_path)} bytes"
    except Exception as e:
        os.unlink(dot_path)
        return (
            f"Graphviz Python rendering failed.\nError: {e}\n\n"
            f"--- DOT code ---\n{code}\n--- end DOT ---"
        )


def main():
    try:
        raw = sys.stdin.read()
        args = json.loads(raw)
        result = run(args)
        print(result)
    except json.JSONDecodeError as e:
        print(f"Error: invalid JSON args: {e}", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
