// Info: Add real PDF bookmarks (outline / TOC) from a text list.
// Arg: <INPUT> : Input PDF (the current document)
// Arg: <OUTPUT> : Output PDF (a new file, never overwrites the input)
// Arg: <DIR>/bookmarks.txt : Bookmark list: level,page,title
// Arg: replace : "replace" (default) or "append" (keep existing bookmarks)
// Arg: auto : save mode: auto | compress | incremental
//
// Runs INSIDE SumatraPDF-static.exe: either paste it into
//   right-click -> Document -> Run PDF Script...
// or from the command line:
//   SumatraPDF-static.exe run add-pdf-bookmarks.js in.pdf out.pdf bookmarks.txt replace
//
// This writes the PDF /Outlines dictionary (real bookmarks, shown by F12),
// NOT the "Favorites" stored in SumatraPDF-settings.txt.
//
// WHY NO ARGUMENT ROWS IN THE BUNDLED COPY: the dialog crashes under Wine
// 6.0.2 as soon as it creates an argument row (also reproducible on the
// stock, unmodified EXE). make-scripts.py strips "// Arg:" by default, so
// the bundled version uses the CONFIG block. --keep-arg-rows restores them
// for real Windows only.

// === CONFIG: edit these when running without CLI arguments ===
var CFG_INPUT = "Z:\\path\\to\\book.pdf";
var CFG_OUTPUT = "Z:\\path\\to\\book-out.pdf";
var CFG_LIST = "Z:\\path\\to\\bookmarks.txt";
var CFG_MODE = "replace";
var CFG_SAVE = "auto";
// ============================================================

var srcPath = scriptArgs.length > 0 ? scriptArgs[0] : CFG_INPUT;
var dstPath = scriptArgs.length > 1 ? scriptArgs[1] : CFG_OUTPUT;
var specPath = scriptArgs.length > 2 ? scriptArgs[2] : CFG_LIST;
var mode = scriptArgs.length > 3 ? scriptArgs[3] : CFG_MODE;

function isSpace(c) {
  return c === " " || c === "\t" || c === "\r" || c === "\n";
}

function trim(s) {
  if (s === null || s === undefined) return "";
  var a = 0;
  var b = s.length;
  while (a < b && isSpace(s.charAt(a))) a++;
  while (b > a && isSpace(s.charAt(b - 1))) b--;
  return s.slice(a, b);
}

// Spec lines: level,page,title  (page is 1-based, level 1 = top level)
// e.g. 1,1,Chapter 1   /   2,7,Section 1.1
function parseSpec(text) {
  var roots = [];
  var last = [];
  var lines = text.split("\n");
  var count = 0;
  for (var i = 0; i < lines.length; i++) {
    var line = trim(lines[i]);
    if (line.length === 0 || line.charAt(0) === "#") continue;
    var p1 = line.indexOf(",");
    var p2 = p1 < 0 ? -1 : line.indexOf(",", p1 + 1);
    if (p2 < 0) {
      print("skip line " + (i + 1) + " (expected level,page,title): " + line);
      continue;
    }
    var level = parseInt(trim(line.slice(0, p1)), 10);
    var page = parseInt(trim(line.slice(p1 + 1, p2)), 10);
    var title = trim(line.slice(p2 + 1));
    if (isNaN(level) || isNaN(page) || level < 1 || page < 1 || title.length === 0) {
      print("skip line " + (i + 1) + " (bad values): " + line);
      continue;
    }
    var node = { title: title, page: page, children: [] };
    last[level] = node;
    for (var L = level + 1; L < 32; L++) last[L] = null;
    var parent = level > 1 ? last[level - 1] : null;
    if (parent) parent.children.push(node);
    else roots.push(node);
    count++;
  }
  return { roots: roots, count: count };
}

// Convert MuPDF's loadOutline() tree into our node tree (used by "append").
function outlineToNodes(doc, list) {
  var nodes = [];
  if (!list) return nodes;
  for (var i = 0; i < list.length; i++) {
    var it = list[i];
    var page = 1;
    if (it.uri) {
      var p = doc.resolveLink(it.uri);
      if (p >= 0) page = p + 1;
    }
    var node = {
      title: it.title ? it.title : "Untitled",
      page: page,
      children: outlineToNodes(doc, it.down)
    };
    nodes.push(node);
  }
  return nodes;
}

function clampPages(nodes, pageCount) {
  for (var i = 0; i < nodes.length; i++) {
    if (nodes[i].page > pageCount) {
      print("warning: '" + nodes[i].title + "' page " + nodes[i].page +
            " > " + pageCount + ", clamped to last page");
      nodes[i].page = pageCount;
    }
    clampPages(nodes[i].children, pageCount);
  }
}

// Mirrors MuPDF's copy-outline.js: insert, step back, descend for children,
// come back up, then step forward past the parent.
function insertNodes(cursor, nodes) {
  var n = 0;
  for (var i = 0; i < nodes.length; i++) {
    var node = nodes[i];
    cursor.insert({ title: node.title, uri: "#page=" + node.page, open: true });
    n++;
    if (node.children.length > 0) {
      cursor.prev();
      cursor.down();
      n += insertNodes(cursor, node.children);
      cursor.up();
      cursor.next();
    }
  }
  return n;
}

if (srcPath.indexOf("path\\to\\") >= 0) {
  print("ERROR: edit CFG_INPUT / CFG_OUTPUT / CFG_LIST at the top first, or run:");
  print("       SumatraPDF-static.exe run add-pdf-bookmarks.js in.pdf out.pdf list.txt replace auto");
  quit();
}

var specText;
try {
  specText = readFile(specPath).asString();
} catch (e) {
  print("ERROR: cannot read bookmark list '" + specPath + "': " + e);
  print("Put a bookmarks.txt next to the PDF, or point CFG_LIST at your list.");
  quit();
}
var spec = parseSpec(specText);
if (spec.count === 0) {
  print("no bookmarks found in " + specPath);
  quit();
}

var doc = Document.openDocument(srcPath);
var pageCount = doc.countPages();

var roots = [];
if (mode === "append") {
  try {
    roots = outlineToNodes(doc, doc.loadOutline());
    print("keeping " + roots.length + " existing top-level bookmarks");
  } catch (e) {
    print("warning: could not read existing outline: " + e);
  }
} else if (mode !== "replace") {
  print("warning: unknown mode '" + mode + "', using replace");
}
for (var i = 0; i < spec.roots.length; i++) roots.push(spec.roots[i]);

clampPages(roots, pageCount);

var cursor = doc.outlineIterator();
while (cursor.item()) cursor.delete();
var written = insertNodes(cursor, roots);

// "compress" does a clean full rewrite. MuPDF opens a fresh output with
// fopen("wb+x"); Wine 6.0.2's CRT does not support the "x" flag and fails
// with EINVAL, so fall back to "incremental" there (real Windows is fine).
var saveMode = scriptArgs.length > 4 ? scriptArgs[4] : CFG_SAVE;
if (saveMode === "auto") {
  try {
    doc.save(dstPath, "compress");
    print("save: full rewrite (compress)");
  } catch (e) {
    print("save: full rewrite unavailable, using incremental (" + e + ")");
    doc.save(dstPath, "incremental");
    print("save: incremental");
  }
} else {
  doc.save(dstPath, saveMode);
  print("save: " + saveMode);
}
print("OK: wrote " + written + " PDF bookmarks to " + dstPath);
