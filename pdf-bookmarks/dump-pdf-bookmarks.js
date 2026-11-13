// Info: List the PDF bookmarks (outline / TOC) of a document.
// Arg: <INPUT> : PDF to inspect
//
// Read-only check. Use after add-pdf-bookmarks.js to confirm the /Outlines
// dictionary was really written into the file.
//
// The bundled copy has no "// Arg:" rows (Wine 6.0.2 crashes when the dialog
// creates one), so edit CFG_INPUT when running from the GUI.

// === CONFIG: edit when running without CLI arguments ===
var CFG_INPUT = "Z:\\path\\to\\book-out.pdf";
// ======================================================

var srcPath = scriptArgs.length > 0 ? scriptArgs[0] : CFG_INPUT;
if (srcPath.indexOf("path\\to\\") >= 0) {
  print("ERROR: edit CFG_INPUT at the top, or run: dump-pdf-bookmarks.js file.pdf");
  quit();
}
var doc = Document.openDocument(srcPath);

function dump(list, depth) {
  if (!list) return 0;
  var n = 0;
  for (var i = 0; i < list.length; i++) {
    var it = list[i];
    var page = -1;
    if (it.uri) page = doc.resolveLink(it.uri) + 1;
    var pad = "";
    for (var d = 0; d < depth; d++) pad += "  ";
    print(pad + "- " + it.title + "  [page " + page + "]  " + (it.uri ? it.uri : ""));
    n++;
    if (it.down) n += dump(it.down, depth + 1);
  }
  return n;
}

var total = dump(doc.loadOutline(), 0);
print("total: " + total + " bookmarks");
