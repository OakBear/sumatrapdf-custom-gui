// Info: Add one PDF bookmark (real outline entry) at a given page.
// Arg: <INPUT> : Input PDF (the current document)
// Arg: <OUTPUT> : Output PDF (a new file)
// Arg: 1 : Page number (1-based)
// Arg: My bookmark : Bookmark title
// Arg: append : "append" (keep existing) or "replace"
//
// Writes the PDF /Outlines dictionary, so the entry shows up in the F12
// "Bookmarks" sidebar of ANY PDF reader. NOT the "Favorites" in settings.
//
// Two ways to run:
//  * CLI: SumatraPDF-static.exe run add-one-bookmark.js in.pdf out.pdf 42 "Title" append
//    (scriptArgs overrides everything below)
//  * GUI: pick this in Example Scripts, edit the CONFIG lines, press Ctrl+Enter
//
// WHY NO ARGUMENT ROWS IN THE BUNDLED COPY: the dialog crashes under Wine
// 6.0.2 as soon as it creates an argument row (reproducible on the stock,
// unmodified EXE). make-scripts.py strips the "// Arg:" lines by default,
// so the bundled version uses this CONFIG block instead. Pass
// --keep-arg-rows to make-scripts.py if you only run on real Windows.

// === CONFIG: edit these when running without CLI arguments ===
var CFG_INPUT = "Z:\\path\\to\\book.pdf";
var CFG_OUTPUT = "Z:\\path\\to\\book-out.pdf";
var CFG_PAGE = 42;
var CFG_TITLE = "Chapter 42";
var CFG_MODE = "append";
// ============================================================

var srcPath = scriptArgs.length > 0 ? scriptArgs[0] : CFG_INPUT;
var dstPath = scriptArgs.length > 1 ? scriptArgs[1] : CFG_OUTPUT;
var page = parseInt(scriptArgs.length > 2 ? scriptArgs[2] : CFG_PAGE, 10);
var title = scriptArgs.length > 3 ? scriptArgs[3] : CFG_TITLE;
var mode = scriptArgs.length > 4 ? scriptArgs[4] : CFG_MODE;

function outlineToNodes(doc, list) {
  var nodes = [];
  if (!list) return nodes;
  for (var i = 0; i < list.length; i++) {
    var it = list[i];
    var p = 1;
    if (it.uri) {
      var r = doc.resolveLink(it.uri);
      if (r >= 0) p = r + 1;
    }
    nodes.push({
      title: it.title ? it.title : "Untitled",
      page: p,
      children: outlineToNodes(doc, it.down)
    });
  }
  return nodes;
}

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
  print("ERROR: edit CFG_INPUT / CFG_OUTPUT at the top of this script first,");
  print("       or run it from the command line with: in.pdf out.pdf page title");
  quit();
}

var doc = Document.openDocument(srcPath);
var pageCount = doc.countPages();
if (isNaN(page) || page < 1 || title.length === 0) {
  print("ERROR: page must be a number >= 1 and title must not be empty");
  quit();
}
if (page > pageCount) {
  print("ERROR: page " + page + " exceeds the " + pageCount + " pages of this PDF");
  quit();
}

var roots = [];
if (mode === "append") {
  try {
    roots = outlineToNodes(doc, doc.loadOutline());
  } catch (e) {
    print("warning: could not read existing outline: " + e);
  }
}
roots.push({ title: title, page: page, children: [] });

var cursor = doc.outlineIterator();
while (cursor.item()) cursor.delete();
var written = insertNodes(cursor, roots);

try {
  doc.save(dstPath, "compress");
  print("save: full rewrite (compress)");
} catch (e) {
  // Wine 6.0.2 CRT rejects fopen("wb+x") used for fresh output.
  doc.save(dstPath, "incremental");
  print("save: incremental");
}
print("OK: '" + title + "' -> page " + page + "; " + written + " bookmarks total in " + dstPath);
