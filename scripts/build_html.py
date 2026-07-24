import os
import re
import gzip

SRC_DIR = "src"


def minify_css(css: str) -> str:
    """Strip /* ... */ comments and collapse insignificant whitespace.

    Preserves spaces inside string literals (double or single quoted).
    No semicolon trimming -- that adds risk for very little payoff vs.
    gzip's redundancy handling.
    """
    out = []
    i = 0
    n = len(css)
    in_str = None  # quote char if inside a string
    while i < n:
        c = css[i]
        nxt = css[i + 1] if i + 1 < n else ''
        if in_str:
            out.append(c)
            if c == '\\' and i + 1 < n:
                out.append(nxt)
                i += 2
                continue
            if c == in_str:
                in_str = None
            i += 1
            continue
        if c == '"' or c == "'":
            in_str = c
            out.append(c)
            i += 1
            continue
        if c == '/' and nxt == '*':
            # Skip until */
            i += 2
            while i + 1 < n and not (css[i] == '*' and css[i + 1] == '/'):
                i += 1
            i += 2
            # Replace comment with a single space to keep tokens separated.
            out.append(' ')
            continue
        if c in ' \t\r\n':
            # Collapse whitespace run to a single space, dropping if the
            # last emitted character was already a separator.
            j = i
            while j < n and css[j] in ' \t\r\n':
                j += 1
            i = j
            if out and out[-1] not in ' \n{};:,>+~()':
                out.append(' ')
            continue
        out.append(c)
        i += 1
    # Tidy: remove space right after / before punctuation that doesn't need it.
    s = ''.join(out)
    s = re.sub(r'\s*([{};:,>+~])\s*', r'\1', s)
    return s.strip()


def minify_js(js: str) -> str:
    """Strip JS comments + collapse excess whitespace.

    Aware of:
      - Single (') and double (") strings + backtick template literals
      - Block comments /* ... */
      - Line comments // ... \\n
      - Regex literals (rough heuristic: '/' is a regex only when the
        previous non-whitespace token can't end an expression)

    Conservative: never strips a space that joins two identifier chars.
    Doesn't rename variables, mangle, or rewrite -- safe to round-trip
    through the existing FilesPage. ~30-50% size reduction on heavily
    commented sources; gzip then drops ~half of that on the wire.
    """
    out = []
    i = 0
    n = len(js)
    in_str = None  # quote char
    last_significant = ''  # last non-whitespace, non-comment char emitted
    # JS tokens that can end an expression (so a following '/' is
    # division). Anything else makes a following '/' a regex literal.
    expr_end = set("])}") | set("0123456789_") | set("abcdefghijklmnopqrstuvwxyz") | set("ABCDEFGHIJKLMNOPQRSTUVWXYZ")
    while i < n:
        c = js[i]
        nxt = js[i + 1] if i + 1 < n else ''
        if in_str:
            out.append(c)
            if c == '\\' and i + 1 < n:
                out.append(nxt)
                i += 2
                continue
            if c == in_str:
                in_str = None
                last_significant = c
            i += 1
            continue
        if c == '"' or c == "'" or c == '`':
            in_str = c
            out.append(c)
            i += 1
            continue
        if c == '/' and nxt == '*':
            # Block comment
            i += 2
            while i + 1 < n and not (js[i] == '*' and js[i + 1] == '/'):
                i += 1
            i += 2
            continue
        if c == '/' and nxt == '/':
            # Line comment
            i += 2
            while i < n and js[i] != '\n':
                i += 1
            continue
        if c == '/' and last_significant not in expr_end:
            # Regex literal. Skip until unescaped /.
            out.append(c)
            i += 1
            while i < n:
                cc = js[i]
                out.append(cc)
                if cc == '\\' and i + 1 < n:
                    out.append(js[i + 1])
                    i += 2
                    continue
                if cc == '[':
                    # In a character class until ]
                    i += 1
                    while i < n:
                        out.append(js[i])
                        if js[i] == '\\' and i + 1 < n:
                            out.append(js[i + 1])
                            i += 2
                            continue
                        if js[i] == ']':
                            i += 1
                            break
                        i += 1
                    continue
                if cc == '/':
                    i += 1
                    break
                i += 1
            last_significant = '/'
            continue
        if c in ' \t\r\n':
            # Collapse: drop the run, optionally keeping a single separator
            # when it joins two identifier-like characters (otherwise the
            # tokens would fuse).
            j = i
            had_newline = False
            while j < n and js[j] in ' \t\r\n':
                if js[j] == '\n':
                    had_newline = True
                j += 1
            i = j
            # Decide whether we need a separator.
            prev = last_significant
            after = js[i] if i < n else ''
            def is_word(ch):
                return ch.isalnum() or ch == '_' or ch == '$'
            need_space = is_word(prev) and is_word(after)
            # Newlines are also significant for ASI (automatic semicolon
            # insertion). Preserve a newline when the previous token could
            # end a statement and the next token could start one.
            if had_newline and prev in ')]}\'\"+-/*%' and after and after not in ')]},;:.+-/*%':
                out.append('\n')
            elif need_space:
                out.append(' ')
            continue
        out.append(c)
        last_significant = c
        i += 1
    return ''.join(out).strip()


def minify_html(html: str) -> str:
    # Tags where whitespace should be preserved
    preserve_tags = ['pre', 'code', 'textarea', 'script', 'style']
    preserve_regex = '|'.join(preserve_tags)

    # Protect preserve blocks with placeholders. CrumBLE: also minify the
    # inner bytes of <script> and <style> blocks so the on-flash size
    # tracks the source comments shrink. <pre>/<code>/<textarea> stay
    # verbatim because they hold author-visible content.
    preserve_blocks = []
    def preserve(match):
        whole = match.group(0)
        tag = match.group(1).lower()
        if tag in ('script', 'style'):
            # Find inner body between the open and close tags.
            open_end = whole.find('>') + 1
            close_start = whole.rfind('<')
            head = whole[:open_end]
            body = whole[open_end:close_start]
            tail = whole[close_start:]
            try:
                body = minify_js(body) if tag == 'script' else minify_css(body)
            except Exception as e:
                # Conservative fallback: leave the block alone if our
                # minifier hits an edge case. Build still completes.
                print(f"  minify {tag} fallback: {e}")
            whole = head + body + tail
        preserve_blocks.append(whole)
        return f"__PRESERVE_BLOCK_{len(preserve_blocks)-1}__"

    html = re.sub(rf'<({preserve_regex})[\s\S]*?</\1>', preserve, html, flags=re.IGNORECASE)

    # Remove HTML comments
    html = re.sub(r'<!--.*?-->', '', html, flags=re.DOTALL)

    # Collapse all whitespace between tags
    html = re.sub(r'>\s+<', '><', html)

    # Collapse multiple spaces inside tags
    html = re.sub(r'\s+', ' ', html)

    # Restore preserved blocks
    for i, block in enumerate(preserve_blocks):
        html = html.replace(f"__PRESERVE_BLOCK_{i}__", block)

    return html.strip()

def sanitize_identifier(name: str) -> str:
    """Sanitize a filename to create a valid C identifier.

    C identifiers must:
    - Start with a letter or underscore
    - Contain only letters, digits, and underscores
    """
    # Replace non-alphanumeric characters (including hyphens) with underscores
    sanitized = re.sub(r'[^a-zA-Z0-9_]', '_', name)
    # Prefix with underscore if starts with a digit
    if sanitized and sanitized[0].isdigit():
        sanitized = f"_{sanitized}"
    return sanitized

for root, _, files in os.walk(SRC_DIR):
    for file in files:
        if file.endswith(".html") or file.endswith(".js"):
            file_path = os.path.join(root, file)
            with open(file_path, "r", encoding="utf-8") as f:
                content = f.read()

            # Only minify HTML files; JS files are typically pre-minified (e.g., jszip.min.js).
            # Exception: files-app.js -- extracted from FilesPage.html in v18.9.9.97 and
            # therefore heavily commented; run it through minify_js so the served payload
            # matches what would have been produced by minify_html's inline-script pass.
            if file.endswith(".html"):
                processed = minify_html(content)
            elif file == "files-app.js":
                processed = minify_js(content)
            else:
                processed = content

            # Compress with gzip (compresslevel 9 is maximum compression)
            # IMPORTANT: we don't use brotli because Firefox doesn't support brotli with insecured context (only supported on HTTPS)
            compressed = gzip.compress(processed.encode('utf-8'), compresslevel=9)

            # Create valid C identifier from filename
            # Use appropriate suffix based on file type
            suffix = "Html" if file.endswith(".html") else "Js"
            base_name = sanitize_identifier(f"{os.path.splitext(file)[0]}{suffix}")
            header_path = os.path.join(root, f"{base_name}.generated.h")

            with open(header_path, "w", encoding="utf-8") as h:
                h.write(f"// THIS FILE IS AUTOGENERATED, DO NOT EDIT MANUALLY\n\n")
                h.write(f"#pragma once\n")
                h.write(f"#include <cstddef>\n\n")

                # Write the compressed data as a byte array
                h.write(f"constexpr char {base_name}[] PROGMEM = {{\n")

                # Write bytes in rows of 16
                for i in range(0, len(compressed), 16):
                    chunk = compressed[i:i+16]
                    hex_values = ', '.join(f'0x{b:02x}' for b in chunk)
                    h.write(f"  {hex_values},\n")

                h.write(f"}};\n\n")
                h.write(f"constexpr size_t {base_name}CompressedSize = {len(compressed)};\n")
                h.write(f"constexpr size_t {base_name}OriginalSize = {len(processed)};\n")

            print(f"Generated: {header_path}")
            print(f"  Original: {len(content)} bytes")
            print(f"  Minified: {len(processed)} bytes ({100*len(processed)/len(content):.1f}%)")
            print(f"  Compressed: {len(compressed)} bytes ({100*len(compressed)/len(content):.1f}%)")
